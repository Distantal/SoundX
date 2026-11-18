#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <vector>

namespace sx
{
class OutOfProcessScanner;

/** Scans plug-in files for the plug-in manager, on a thread of its own.

    Two things make scanning slow: looking for the files (a recursive walk over the plug-in
    folders, which can be thousands of entries on a big installation) and loading each one (every
    file goes through a throw-away helper process, see PluginScanProcess.h: between a tenth of a
    second and, for a big shell plug-in, a minute). Doing either on the message thread is exactly
    what made the plug-in manager freeze and get the Windows "not responding" ghost window while
    scanning, so all of it runs here. The window only reads the counters below and repaints.
*/
class PluginScanner : private juce::Thread
{
public:
    struct Failure
    {
        juce::String file, reason;
    };

    PluginScanner (juce::AudioPluginFormatManager& formatsToUse,
                   juce::KnownPluginList& listToFill,
                   OutOfProcessScanner& outOfProcessToUse,
                   juce::PropertiesFile* propertiesToUse,
                   juce::File deadMansPedalFileToUse);
    ~PluginScanner() override;

    //== starting a scan (message thread) ==================================================
    /** Scans the remembered (or default) folders of every plug-in format. */
    bool scanEverything();

    /** Scans these folders recursively and, if asked, also these single files. */
    bool scan (const juce::FileSearchPath& folders, const juce::StringArray& files, bool rememberFolders);

    /** Scans one folder recursively. rememberForNextSession adds it to the folders that
        scanEverything() walks from now on. */
    bool scanFolder (const juce::File& folder, bool rememberForNextSession);

    /** Scans exactly these files (drag and drop of plug-in files). */
    bool scanFiles (const juce::StringArray& files);

    /** Asks the scan to stop. The helper process of the plug-in being looked at is killed, so
        the thread is back within a couple of hundred milliseconds at the most. */
    void cancel();

    /** Drops the plug-in file that is being scanned right now and carries on with the next one:
        its helper process is killed and nothing about it is recorded (neither a plug-in entry nor
        a deactivation). Returns the file that was dropped (the full path it was scanned under), or
        an empty string when no file is being loaded at this moment - there is nothing to skip
        then.

        This is the difference between "this plug-in is taking forever / I do not want it" and
        cancelling the whole scan. */
    juce::String skipCurrent();

    bool isScanning() const noexcept          { return isThreadRunning(); }

    //== progress / result (readable from any thread) ======================================
    /** True while the plug-in folders are being walked (no total count yet). */
    bool isLookingForFiles() const noexcept   { return lookingForFiles.load(); }
    int getNumTotal() const noexcept          { return numTotal.load(); }
    int getNumScanned() const noexcept        { return numScanned.load(); }
    /** Name of the plug-in file being scanned right now (empty when idle). */
    juce::String getCurrentFile() const;
    /** Whether the most recent scan was stopped with cancel(). */
    bool wasCancelled() const noexcept        { return cancelled.load(); }
    /** How many files the most recent scan dropped with skipCurrent() at the user's request. */
    int getNumSkipped() const noexcept        { return numSkipped.load(); }
    /** The files the most recent scan dropped that way, in scan order. */
    juce::StringArray getSkippedFiles() const;
    /** Plug-ins that failed during the most recent scan, with the reason from the scan log. */
    juce::Array<Failure> getFailures() const;

private:
    struct Target
    {
        juce::AudioPluginFormat* format = nullptr;
        juce::FileSearchPath folders;
    };

    struct WorkItem
    {
        juce::AudioPluginFormat* format = nullptr;
        juce::String file;
    };

    void run() override;
    bool beginScan (std::vector<Target> newTargets, const juce::StringArray& newFiles);
    void setCurrentFile (const juce::String& name);
    void beginActiveFile (const juce::String& name);
    void endActiveFile (const juce::String& name);
    void scanSerial (const std::vector<WorkItem>& work, juce::StringArray& failedFiles);
    void scanParallel (const std::vector<WorkItem>& work, juce::StringArray& failedFiles);
    static int scanWorkerCount();
    juce::FileSearchPath rememberedPathsFor (juce::AudioPluginFormat& format) const;
    static void addFolderToRememberedPaths (juce::PropertiesFile* props, juce::AudioPluginFormat& format,
                                            const juce::File& folder);

    juce::AudioPluginFormatManager& formats;
    juce::KnownPluginList& known;
    OutOfProcessScanner& outOfProcess;           // the one known's scan actually runs through
    juce::PropertiesFile* props;                 // only touched on the message thread
    juce::File deadMansPedalFile;

    std::vector<Target> targets;
    juce::StringArray filesToScan;

    std::atomic<bool> lookingForFiles { false }, cancelled { false };
    std::atomic<int> numTotal { 0 }, numScanned { 0 }, numSkipped { 0 };

    // The helper-process scanners used by a parallel folder scan. They are created once and kept
    // for the lifetime of this object, so Skip (on the message thread) can always ask them what
    // they are loading without racing their destruction.
    std::vector<std::unique_ptr<OutOfProcessScanner>> scanHelpers;
    std::atomic<bool> parallelCancel { false };

    mutable juce::CriticalSection stateLock;
    juce::String currentFile;
    juce::StringArray activeFiles;
    juce::StringArray skippedFiles;
    juce::Array<Failure> failures;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScanner)
};
}
