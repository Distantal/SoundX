/*
  ==============================================================================

   Background plug-in scanning.

   See PluginScanner.h for why this is not done on the message thread. The thread fills the
   KnownPluginList through the out-of-process scanner, which is also why a plug-in that hangs or
   crashes never takes this process down with it.

  ==============================================================================
*/

#include "PluginScanner.h"
#include "PluginScanProcess.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace sx
{
using namespace juce;

namespace
{
    // the property key JUCE's own plug-in list uses for its remembered scan folders, so the
    // paths a user picked with an older version keep working
    String searchPathKey (const AudioPluginFormat& format) { return "lastPluginScanPath_" + format.getName(); }

    bool canBeScanned (const AudioPluginFormat* format)
    {
        return format != nullptr && format->canScanForPlugins();
    }

    // What this build can load. On macOS a 32/64-bit or Intel/Apple-silicon mismatch is the most
    // common reason for "the plug-in does not load", so the log says what we are.
    String buildDescription()
    {
       #if JUCE_ARM
        const String arch = "arm64";
       #elif JUCE_INTEL && JUCE_64BIT
        const String arch = "x86_64";
       #else
        const String arch = "x86";
       #endif
        return SystemStats::getOperatingSystemName() + " / " + arch;
    }

    // How many helper processes a folder scan may run at once. Loading a plug-in is dominated by
    // the plug-in's own start-up work (and by the helper process launch), so several of them in
    // parallel cut the wall-clock time of a fresh scan substantially. Four is the sweet spot on a
    // normal machine: more also fight over the same disk and the audio thread.
    int parallelWorkerCount()
    {
        if (const auto env = SystemStats::getEnvironmentVariable ("SOUNDX_SCAN_WORKERS", {});
            env.isNotEmpty())
            return jlimit (1, 8, env.getIntValue());

        const auto hardware = (int) std::thread::hardware_concurrency();
        return jlimit (1, 4, hardware > 1 ? hardware : 1);
    }

    // What one helper process found for one file, handed from a worker thread back to the scan
    // thread (which is the only thread that may touch KnownPluginList's plug-in array).
    struct ScanOutcome
    {
        int index = 0;
        bool ok = false;
        bool skipped = false;
        int skippedCount = 0;
        OwnedArray<PluginDescription> types;
    };
}

PluginScanner::PluginScanner (AudioPluginFormatManager& formatsToUse,
                              KnownPluginList& listToFill,
                              OutOfProcessScanner& outOfProcessToUse,
                              PropertiesFile* propertiesToUse,
                              File deadMansPedalFileToUse)
    : Thread ("SoundX plug-in scanner"),
      formats (formatsToUse),
      known (listToFill),
      outOfProcess (outOfProcessToUse),
      props (propertiesToUse),
      deadMansPedalFile (deadMansPedalFileToUse)
{
    // The helpers are created once and never destroyed while a scan is running: Skip is called
    // from the message thread while the workers are inside them, and it must never look at an
    // object that a worker has already deleted.
    for (int i = 0; i < 4; ++i)
    {
        scanHelpers.push_back (std::make_unique<OutOfProcessScanner>());
        scanHelpers.back()->setCancellationFlag (&parallelCancel);
    }
}

PluginScanner::~PluginScanner()
{
    // the out-of-process scanner kills its helper process as soon as it sees this flag, so the
    // wait here is short even when a plug-in is stuck
    parallelCancel = true;
    signalThreadShouldExit();
    stopThread (10000);
}

//==============================================================================
FileSearchPath PluginScanner::rememberedPathsFor (AudioPluginFormat& format) const
{
    const auto key = searchPathKey (format);

    if (props != nullptr && props->containsKey (key))
    {
        const auto stored = props->getValue (key, {}).trim();
        if (stored.isNotEmpty())
            return FileSearchPath (stored);
    }

    return format.getDefaultLocationsToSearch();
}

void PluginScanner::addFolderToRememberedPaths (PropertiesFile* props, AudioPluginFormat& format, const File& folder)
{
    if (props == nullptr) return;

    auto path = props->containsKey (searchPathKey (format))
                    ? FileSearchPath (props->getValue (searchPathKey (format), {}))
                    : format.getDefaultLocationsToSearch();

    if (path.addIfNotAlreadyThere (folder))
    {
        props->setValue (searchPathKey (format), path.toString());
        props->saveIfNeeded();
    }
}

//==============================================================================
bool PluginScanner::scanEverything()
{
    std::vector<Target> newTargets;

    // resolving the paths touches the properties file, which belongs to the message thread:
    // do it here rather than in the scan thread
    for (int i = 0; i < formats.getNumFormats(); ++i)
        if (auto* format = formats.getFormat (i); canBeScanned (format))
            newTargets.push_back ({ format, rememberedPathsFor (*format) });

    return beginScan (std::move (newTargets), {});
}

bool PluginScanner::scan (const FileSearchPath& folders, const StringArray& files, bool rememberFolders)
{
    std::vector<Target> newTargets;

    if (folders.getNumPaths() > 0)
        for (int i = 0; i < formats.getNumFormats(); ++i)
            if (auto* format = formats.getFormat (i); canBeScanned (format))
            {
                newTargets.push_back ({ format, folders });

                if (rememberFolders)
                    for (int j = 0; j < folders.getNumPaths(); ++j)
                        addFolderToRememberedPaths (props, *format, folders[j]);
            }

    return beginScan (std::move (newTargets), files);
}

bool PluginScanner::scanFolder (const File& folder, bool rememberForNextSession)
{
    if (! folder.isDirectory()) return false;
    return scan (FileSearchPath (folder.getFullPathName()), {}, rememberForNextSession);
}

bool PluginScanner::scanFiles (const StringArray& files)
{
    return scan ({}, files, false);
}

bool PluginScanner::beginScan (std::vector<Target> newTargets, const StringArray& newFiles)
{
    // one scan at a time: the previous one has to be cancelled (and finished) first, otherwise
    // two threads would fill the same KnownPluginList
    if (isThreadRunning()) return false;

    targets = std::move (newTargets);
    filesToScan = newFiles;

    lookingForFiles = false;
    cancelled = false;
    parallelCancel = false;
    numTotal = 0;
    numScanned = 0;
    numSkipped = 0;
    {
        const ScopedLock sl (stateLock);
        currentFile.clear();
        activeFiles.clear();
        skippedFiles.clear();
        failures.clear();
    }

    return startThread();
}

void PluginScanner::cancel()
{
    parallelCancel = true;
    signalThreadShouldExit();
}

String PluginScanner::skipCurrent()
{
    // A parallel scan can have several helpers loading a file at once. Ask them in order; the
    // first one that is actually inside a load has the file the user wants to drop. If none is
    // (or the scan is running through the single serial helper), fall back to that scanner.
    for (auto& helper : scanHelpers)
        if (helper != nullptr)
            if (const auto dropped = helper->skipCurrentFile(); dropped.isNotEmpty())
                return dropped;

    return outOfProcess.skipCurrentFile();
}

//==============================================================================
String PluginScanner::getCurrentFile() const
{
    const ScopedLock sl (stateLock);
    return currentFile;
}

Array<PluginScanner::Failure> PluginScanner::getFailures() const
{
    const ScopedLock sl (stateLock);
    return failures;
}

StringArray PluginScanner::getSkippedFiles() const
{
    const ScopedLock sl (stateLock);
    return skippedFiles;
}

void PluginScanner::beginActiveFile (const String& name)
{
    const ScopedLock sl (stateLock);
    if (! activeFiles.contains (name)) activeFiles.add (name);
    currentFile = name;
}

void PluginScanner::endActiveFile (const String& name)
{
    const ScopedLock sl (stateLock);
    activeFiles.removeString (name);
    currentFile = activeFiles.isEmpty() ? String() : activeFiles[0];
}

int PluginScanner::scanWorkerCount() { return parallelWorkerCount(); }

//==============================================================================
void PluginScanner::scanSerial (const std::vector<WorkItem>& work, StringArray& failedFiles)
{
    for (auto& item : work)
    {
        if (threadShouldExit()) break;

        const auto name = File (item.file).getFileName();
        beginActiveFile (name);

        // Skip (the plug-in manager button) happens inside the helper-process scanner, so the only
        // way to tell a skipped file from a scanned one is to ask how many it has dropped
        const int skippedBefore = outOfProcess.getNumSkipped();

        OwnedArray<PluginDescription> descriptions;
        known.scanAndAddFile (item.file, true, descriptions, *item.format);

        endActiveFile (name);
        ++numScanned;

        if (outOfProcess.getNumSkipped() > skippedBefore)
        {
            ++numSkipped;

            const ScopedLock sl (stateLock);
            skippedFiles.add (item.file);
        }
        else if (known.getBlacklistedFiles().contains (item.file))
            failedFiles.addIfNotAlreadyThere (item.file);
    }
}

void PluginScanner::scanParallel (const std::vector<WorkItem>& work, StringArray& failedFiles)
{
    const int workers = jlimit (1, (int) scanHelpers.size(), scanWorkerCount());
    if (workers <= 1 || work.size() < 2) { scanSerial (work, failedFiles); return; }

    std::atomic<int> nextIndex { 0 }, finishedWorkers { 0 };
    std::mutex resultLock;
    std::condition_variable resultCv;
    std::vector<std::unique_ptr<ScanOutcome>> results;

    auto worker = [this, &work, &nextIndex, &finishedWorkers, &resultLock, &resultCv, &results] (int helperIndex)
    {
        auto& scanner = *scanHelpers[(size_t) helperIndex];

        for (;;)
        {
            if (parallelCancel.load() || threadShouldExit()) break;

            const int index = nextIndex.fetch_add (1);
            if (index >= (int) work.size()) break;

            const auto& item = work[(size_t) index];
            const auto name = File (item.file).getFileName();
            beginActiveFile (name);

            auto outcome = std::make_unique<ScanOutcome>();
            outcome->index = index;
            const int skippedBefore = scanner.getNumSkipped();
            try
            {
                outcome->ok = scanner.findPluginTypesFor (*item.format, outcome->types, item.file);
            }
            catch (...)
            {
                outcome->ok = false;
            }
            outcome->skippedCount = scanner.getNumSkipped() - skippedBefore;
            outcome->skipped = outcome->skippedCount > 0;

            endActiveFile (name);

            {
                const std::lock_guard<std::mutex> lock (resultLock);
                results.push_back (std::move (outcome));
            }
            resultCv.notify_one();
        }

        ++finishedWorkers;
        resultCv.notify_one();
    };

    auto merge = [this, &work, &failedFiles] (const std::unique_ptr<ScanOutcome>& outcome)
    {
        if (outcome == nullptr) return;
        const auto& item = work[(size_t) outcome->index];
        ++numScanned;

        if (outcome->skipped)
        {
            numSkipped += outcome->skippedCount;
            const ScopedLock sl (stateLock);
            if (! skippedFiles.contains (item.file)) skippedFiles.add (item.file);
        }
        else if (! outcome->ok)
        {
            // The helper crashed, hung or could not be started: deactivate the file, exactly like
            // KnownPluginList does around findPluginTypesFor() in the serial path.
            known.addToBlacklist (item.file);
            failedFiles.addIfNotAlreadyThere (item.file);
        }
        else
        {
            // Only this scan thread touches KnownPluginList's array; workers only hand descriptions
            // back through the queue.
            for (auto* d : outcome->types)
                if (d != nullptr)
                    known.addType (*d);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve ((size_t) workers);
    for (int i = 0; i < workers; ++i)
        threads.emplace_back (worker, i);

    for (;;)
    {
        std::vector<std::unique_ptr<ScanOutcome>> batch;
        {
            std::unique_lock<std::mutex> lock (resultLock);
            resultCv.wait_for (lock, std::chrono::milliseconds (50), [&]
            {
                return ! results.empty() || finishedWorkers.load() >= workers;
            });
            batch.swap (results);
        }

        for (auto& outcome : batch)
            merge (outcome);

        if (finishedWorkers.load() >= workers)
        {
            // every worker has pushed its last result before it increments the counter, so an
            // empty queue now means there is nothing left to merge
            bool empty = false;
            batch.clear();
            {
                const std::lock_guard<std::mutex> lock (resultLock);
                empty = results.empty();
                batch.swap (results);
            }
            for (auto& outcome : batch)
                merge (outcome);

            if (empty) break;
        }
    }

    for (auto& t : threads)
        if (t.joinable())
            t.join();
}

//==============================================================================
void PluginScanner::run()
{
    const auto scanStarted = Time::getMillisecondCounter();

    // what this scan is running on: the first thing to look at when a plug-in "does not load" on
    // another platform (architecture mismatch, macOS code signing, ...)
    OutOfProcessScanner::appendLogLine ("---- scan started: " + buildDescription() + " ----");

    // (parallelCancel is cleared by beginScan before the thread starts; resetting it here would
    // lose a Cancel that arrived in the short window between startThread() and this point)

    // plug-ins that took the process down in an older session stay deactivated until the user
    // re-enables them in the list: apply the dead man's pedal file once, like JUCE's plug-in
    // list does, and then get rid of it
    if (deadMansPedalFile != File())
    {
        PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal (known, deadMansPedalFile);
        deadMansPedalFile.deleteFile();
    }

    std::vector<WorkItem> work;

    // walking the plug-in folders is the other half of the freeze this thread exists for: on a
    // machine with a big collection (or a folder on a slow drive) it takes a long time
    lookingForFiles = targets.size() > 0;

    for (auto& target : targets)
    {
        if (threadShouldExit()) break;
        if (target.format == nullptr || target.folders.getNumPaths() == 0) continue;

        for (auto& file : target.format->searchPathsForPlugins (target.folders, true, false))
            work.push_back ({ target.format, file });
    }

    lookingForFiles = false;

    // files the user dropped on the window: scan exactly those
    for (auto& file : filesToScan)
    {
        if (threadShouldExit()) break;

        for (int i = 0; i < formats.getNumFormats(); ++i)
            if (auto* format = formats.getFormat (i);
                format != nullptr && format->fileMightContainThisPluginType (file))
            {
                work.push_back ({ format, file });
                break;
            }
    }

    numTotal = (int) work.size();

    // Unchanged files do not need a helper process at all. KnownPluginList can tell that by the
    // file's modification time; asking it once per file here keeps a rescan from launching a
    // plug-in process for every already-known plug-in (that was the slowest part of "Scan all").
    StringArray failedFiles;
    StringArray blacklisted = known.getBlacklistedFiles();
    std::vector<WorkItem> pending;
    pending.reserve (work.size());

    for (auto& item : work)
    {
        if (threadShouldExit()) break;

        if (blacklisted.contains (item.file))
        {
            ++numScanned;
            failedFiles.addIfNotAlreadyThere (item.file);
            continue;
        }

        if (known.isListingUpToDate (item.file, *item.format))
        {
            ++numScanned;
            continue;
        }

        pending.push_back (item);
    }

    // Only folder scans get the parallel treatment. A scan of explicitly dropped / re-enabled
    // files keeps the serial order its caller and the Skip button expect.
    const int workers = scanWorkerCount();
    if (workers > 1 && pending.size() > 1 && filesToScan.isEmpty())
        scanParallel (pending, failedFiles);
    else
        scanSerial (pending, failedFiles);

    {
        const ScopedLock sl (stateLock);
        activeFiles.clear();
        currentFile.clear();
    }
    cancelled = threadShouldExit();

    // ---- explain what could not be scanned --------------------------------------------------
    // the scanner logged a reason for every failed file (hang, crash, window...); the log also
    // covers earlier sessions, so an entry that is still deactivated gets its old reason back
    Array<Failure> failed;

    if (! failedFiles.isEmpty())
    {
        const auto reasons = OutOfProcessScanner::getFailureReasons();

        for (auto& file : failedFiles)
            failed.add ({ file, reasons.getValue (file, TRANS ("No reason recorded, see the scan log.")) });
    }

    {
        const ScopedLock sl (stateLock);
        failures = failed;
    }

    const double seconds = (Time::getMillisecondCounter() - scanStarted) / 1000.0;
    OutOfProcessScanner::appendLogLine ("---- scan finished in " + String (seconds, 2) + " s: "
                                        + String ((int) work.size()) + " files, "
                                        + String ((int) numScanned) + " checked, "
                                        + String ((int) numSkipped) + " skipped, "
                                        + String (failedFiles.size()) + " failed ----");
}
}
