/*
  ==============================================================================

   Out-of-process plug-in scanning.

   Scanning a plug-in means loading a third-party binary into the process: a broken, badly
   behaved or copy-protected plug-in can crash, hang forever, or open a modal window right in
   the middle of a scan. In process that means the whole DAW disappears or freezes.

   So every scan runs in a throw-away child process (SoundX.exe --plugin-scan-worker ...):

     - the child loads the plug-in and writes the descriptions it finds to a small XML file,
     - a watchdog thread inside the child hides and closes any window the plug-in opens, so
       nothing ever pops up on the user's desktop,
     - the parent kills the child when it does not answer within the timeout,
     - the parent only ever receives data, never plug-in code.

  A plug-in that kills its worker is reported and blacklisted by KnownPluginList (the plug-in
  manager shows the entry in red and it can be re-enabled from there).

  ==============================================================================
*/

#include "PluginScanProcess.h"
#include "PluginManager.h"     // the scan log lives next to the settings (dataDirectory())

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_MAC || JUCE_IOS
 #include <dlfcn.h>
 #include <libproc.h>
 #include <sys/resource.h>
 #include <unistd.h>
#elif JUCE_LINUX || JUCE_ANDROID
 #include <unistd.h>
#endif

namespace sx
{
using namespace juce;

namespace
{
    const char* workerFlag = "--plugin-scan-worker";

    // Unique scratch-file names even when several helper processes are started at the same
    // millisecond (parallel scanning runs up to four at once).
    std::atomic<int>& scanTagCounter()
    {
        static std::atomic<int> counter { 0 };
        return counter;
    }

    //== diagnostics ======================================================================
    CriticalSection& logLock()
    {
        static CriticalSection lock;
        return lock;
    }

    StringArray& logEntries()
    {
        static StringArray entries;
        return entries;
    }

    void appendToLog (const String& line)
    {
        const ScopedLock sl (logLock());
        auto& entries = logEntries();
        entries.add (line);
        while (entries.size() > 400) entries.remove (0);

        File log = OutOfProcessScanner::getLogFile();
        log.getParentDirectory().createDirectory();
        // the log only ever grows otherwise: a user who rescans a big folder of broken plug-ins
        // would end up with an ever bigger file that nobody reads
        if (log.existsAsFile() && log.getSize() > 256 * 1024)
            log.deleteFile();
        log.appendText (line + "\n");
    }

    // lines are written as "<file>  --  <reason>"
    StringPairArray parseFailureLines (const StringArray& lines)
    {
        StringPairArray result;
        for (auto& line : lines)
        {
            const int sep = line.indexOf ("  --  ");
            if (sep <= 0) continue;
            result.set (line.substring (0, sep), line.substring (sep + 6));
        }
        return result;
    }

    //== helpers shared by the worker and the parent =======================================

    // The parent watches how much work this process does: a plug-in that is really scanning keeps
    // using processor time or reading its files, one that is stuck in a modal window (that we just
    // closed) or in a deadlock does neither. That lets slow shell plug-ins (Waves, Kontakt...) scan
    // for as long as they need while a stuck one is still caught within the idle timeout.
   #if JUCE_WINDOWS || JUCE_MAC
    struct WorkerLoad
    {
        uint64 cpuMs = 0;
        uint64 ioBytes = 0;
        bool valid = false;
    };

   #if JUCE_WINDOWS
    uint64 fileTimeToMs (const FILETIME& t)
    {
        return ((uint64) t.dwHighDateTime << 32 | t.dwLowDateTime) / 10000ull;
    }
   #endif

    WorkerLoad processLoad (uint32 pid)
    {
        WorkerLoad load;
        if (pid == 0) return load;

       #if JUCE_WINDOWS
        if (HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid))
        {
            FILETIME created {}, exited {}, kernel {}, user {};
            if (GetProcessTimes (h, &created, &exited, &kernel, &user))
            {
                load.cpuMs = fileTimeToMs (kernel) + fileTimeToMs (user);
                load.valid = true;
            }

            // a plug-in that is streaming samples off the disk burns little CPU while it works, so
            // file traffic counts as progress too
            IO_COUNTERS io {};
            if (GetProcessIoCounters (h, &io))
                load.ioBytes = (uint64) io.ReadTransferCount + (uint64) io.WriteTransferCount;

            CloseHandle (h);
        }
       #else
        // macOS: the same two numbers for our own child process (libproc is part of libSystem)
        rusage_info_v2 info {};
        if (proc_pid_rusage ((int) pid, RUSAGE_INFO_V2, (rusage_info_t*) &info) == 0)
        {
            load.cpuMs = (uint64) ((info.ri_user_time + info.ri_system_time) / 1000000ull);
            load.ioBytes = (uint64) info.ri_diskio_bytesread + (uint64) info.ri_diskio_byteswritten;
            load.valid = true;
        }
       #endif

        return load;
    }

    // "still working" = the helper used at least 2% of one core, or moved 256 kB, since the last
    // time we saw progress. The window watchdog thread alone needs far less than that (it walks
    // the window list a few times a second), so it cannot keep a stuck plug-in alive forever.
    bool madeProgress (const WorkerLoad& now, const WorkerLoad& reference, uint32 elapsedMs)
    {
        if (! now.valid || elapsedMs == 0) return false;
        if (now.ioBytes >= reference.ioBytes + 256ull * 1024ull) return true;
        if (now.cpuMs <= reference.cpuMs) return false;
        const double cpuFraction = (double) (now.cpuMs - reference.cpuMs) / (double) elapsedMs;
        return cpuFraction >= 0.02;
    }
   #endif

    uint32 readWorkerPid (const File& pidFile, int waitMs)
    {
        const auto started = Time::getMillisecondCounter();
        for (;;)
        {
            if (pidFile.existsAsFile())
            {
                const auto value = (uint32) pidFile.loadFileAsString().trim().getLargeIntValue();
                if (value != 0) return value;
            }
            if (Time::getMillisecondCounter() - started > (uint32) waitMs) return 0;
            Thread::sleep (20);
        }
    }

    // Hides every window this process opens. A plug-in scan must never show a dialog: the
    // user would get a "Fatal Error" box in the middle of a scan (and the scan would block
    // until it was answered). The child closes the window so the plug-in can carry on.
    //
    // Windows only: there the window list can be walked and the windows of another thread can be
    // hidden without asking that thread. On macOS a window may only be touched from the main
    // thread - which is the very thread that is blocked inside the plug-in - so a dialog a plug-in
    // opens there is caught by the parent's idle timeout instead (and the plug-in is reported).
    class WindowWatchdog : public Thread
    {
    public:
        WindowWatchdog() : Thread ("SoundX plug-in scan watchdog") {}

        void run() override
        {
           #if JUCE_WINDOWS
            for (;;)
            {
                EnumWindows (&WindowWatchdog::visitWindow, reinterpret_cast<LPARAM> (this));
                if (threadShouldExit()) break;
                wait (120);
            }
           #endif
        }

        StringArray takeNotes()
        {
           #if JUCE_WINDOWS
            const ScopedLock sl (lock);
            auto copy = notes;
            notes.clear();
            return copy;
           #else
            return {};
           #endif
        }

       #if JUCE_WINDOWS
        static BOOL CALLBACK collectChildText (HWND hwnd, LPARAM lp)
        {
            wchar_t cls[64] = {};
            GetClassNameW (hwnd, cls, (int) numElementsInArray (cls));
            const String className (cls);
            if (className.equalsIgnoreCase ("Static") || className.equalsIgnoreCase ("Button"))
            {
                wchar_t buf[1024] = {};
                GetWindowTextW (hwnd, buf, (int) numElementsInArray (buf));
                const String text (buf);
                if (text.isNotEmpty())
                {
                    auto& out = *reinterpret_cast<String*> (lp);
                    if (! out.contains (text)) out = out.isEmpty() ? text : out + " " + text;
                }
            }
            return TRUE;
        }

        static BOOL CALLBACK visitWindow (HWND hwnd, LPARAM lp)
        {
            auto& self = *reinterpret_cast<WindowWatchdog*> (lp);

            DWORD pid = 0;
            GetWindowThreadProcessId (hwnd, &pid);
            if (pid != GetCurrentProcessId() || ! IsWindowVisible (hwnd))
                return TRUE;

            wchar_t cls[64] = {}, title[1024] = {};
            GetClassNameW (hwnd, cls, (int) numElementsInArray (cls));
            GetWindowTextW (hwnd, title, (int) numElementsInArray (title));

            String text;
            EnumChildWindows (hwnd, &WindowWatchdog::collectChildText, reinterpret_cast<LPARAM> (&text));

            const String note = String (cls) + " | " + String (title) + " | " + text;
            {
                const ScopedLock sl (self.lock);
                if (! self.notes.contains (note)) self.notes.add (note);
            }

            // hide it before the user can see it, then ask it to go away so the plug-in does
            // not stay blocked inside a modal loop
            ShowWindow (hwnd, SW_HIDE);
            PostMessageW (hwnd, WM_CLOSE, 0, 0);
            return TRUE;
        }

        CriticalSection lock;
        StringArray notes;
       #endif
    };

    void writeScanResult (const File& out, const String& file, const StringArray& dialogs,
                          const OwnedArray<PluginDescription>& found, const String& error)
    {
        XmlElement root ("SOUNDXPLUGINSCAN");
        root.setAttribute ("file", file);
        if (error.isNotEmpty()) root.setAttribute ("error", error);
        if (! dialogs.isEmpty()) root.setAttribute ("dialogs", dialogs.joinIntoString (" ;; "));

        for (auto* d : found)
            if (d != nullptr)
                if (auto xml = d->createXml())
                    root.addChildElement (xml.release());

        out.getParentDirectory().createDirectory();
        root.writeTo (out);
    }
}

//==============================================================================
String pluginScanWorkerFlag() { return workerFlag; }

bool isPluginScanWorkerCommandLine (const StringArray& args) { return args.contains (workerFlag); }

String describeBundleLoadProblem (const String& fileOrIdentifier)
{
   #if JUCE_MAC
    const File path (fileOrIdentifier);
    if (path == File()) return {};

    // a .vst3 is a bundle: dyld needs the Mach-O binary inside it
    File binary = path;
    if (path.isDirectory())
    {
        binary = path.getChildFile ("Contents").getChildFile ("MacOS")
                     .getChildFile (path.getFileNameWithoutExtension());
        if (! binary.existsAsFile())
        {
            const auto candidates = path.getChildFile ("Contents").getChildFile ("MacOS")
                                        .findChildFiles (File::findFiles, false);
            if (candidates.isEmpty()) return TRANS ("The plug-in bundle has no executable inside it.");
            binary = candidates.getFirst();
        }
    }

    if (! binary.existsAsFile()) return TRANS ("The plug-in file does not exist.");

    // Ask the loader directly. If dyld is happy the failure is above it (no VST3 factory in the
    // bundle), which is a different problem and not ours to explain.
    if (void* handle = dlopen (binary.getFullPathName().toRawUTF8(), RTLD_LAZY | RTLD_LOCAL))
    {
        dlclose (handle);
        return {};
    }

    const auto* why = dlerror();
    return TRANS ("macOS refused to load the plug-in binary: ") + String (why != nullptr ? why : "unknown error");
   #else
    juce::ignoreUnused (fileOrIdentifier);
    return {};
   #endif
}

int runPluginScanWorker (const StringArray& args)
{
    const int i = args.indexOf (workerFlag);
    if (i < 0 || i + 3 >= args.size())
        return 1;

    const String formatName = args[i + 1];
    const String file = args[i + 2];
    const File out (args[i + 3]);
    const File note = out.withFileExtension ("note");
    const File pidFile = out.withFileExtension ("pid");
    out.deleteFile();
    note.deleteFile();
    // the parent uses this to tell a slow scan from a stuck one (see processLoad). Written on
    // every platform: without it the parent can only apply a flat timeout, which would kill a
    // plug-in that legitimately needs a minute (Kontakt-sized sample libraries).
   #if JUCE_WINDOWS
    pidFile.replaceWithText (String ((int) GetCurrentProcessId()));
   #else
    pidFile.replaceWithText (String ((int) getpid()));
   #endif

   #if JUCE_MAC
    // a scan must not bounce a Dock icon (or steal the focus) once per plug-in: this helper is a
    // throw-away process, and the user never interacts with it
    Process::setDockIconVisible (false);
   #endif

   #if JUCE_WINDOWS
    // a plug-in crash must end this process silently: no "application error" box, no debugger
    // prompt, no Windows error report window for the parent to wait on
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
   #endif

    auto* watchdog = new WindowWatchdog();
    watchdog->startThread();

    // test hooks used by --selftest: the real failure modes (a plug-in that hangs, and one that
    // takes the process down) can be checked without a broken plug-in on the machine
    {
        const auto hook = SystemStats::getEnvironmentVariable ("SOUNDX_SCAN_WORKER_TEST", {});
        if (hook.equalsIgnoreCase ("hang"))
        {
            Thread::sleep (600000);
            return 1;
        }
        if (hook.equalsIgnoreCase ("crash"))
        {
            std::abort();
        }
    }

    // plug-ins frequently resolve their own resources relative to the working directory
    const File pluginPath (file);
    if (pluginPath.exists())
        pluginPath.getParentDirectory().setAsCurrentWorkingDirectory();

    std::unique_ptr<AudioPluginFormat> format;
    if (formatName.equalsIgnoreCase ("VST3"))
        format = std::make_unique<VST3PluginFormat>();
   #if JUCE_PLUGINHOST_VST
    else if (formatName.equalsIgnoreCase ("VST"))
        format = std::make_unique<VSTPluginFormat>();
   #endif

    String error;
    OwnedArray<PluginDescription> found;

    if (format == nullptr)
        error = "unknown plug-in format: " + formatName;
    else
    {
        try
        {
            format->findAllTypesForFile (found, file);
        }
        catch (const std::exception& e) { error = String ("exception: ") + e.what(); }
        catch (...)                     { error = "unknown exception"; }
    }

    // A file that loads but reports no plug-in type at all used to be recorded as "scanned fine,
    // no types", which left the user (and a bug report) without any clue. Say what happened, and
    // on macOS ask dyld why it would not open the bundle - wrong architecture, broken signature
    // and missing nested code are the three ways a VST3 fails there, and the message names the one.
    if (found.isEmpty() && error.isEmpty())
    {
        error = TRANS ("This file did not report any plug-in types.");
       #if JUCE_MAC
        if (const auto why = describeBundleLoadProblem (file); why.isNotEmpty())
            error << "  " << why;
       #endif
    }

    const auto dialogs = watchdog->takeNotes();
    writeScanResult (out, file, dialogs, found, error);

    if (! dialogs.isEmpty())
    {
        StringArray lines;
        lines.add ("dialog shown while scanning: " + file);
        for (auto& d : dialogs) lines.add ("    " + d);
        note.replaceWithText (lines.joinIntoString ("\r\n"));
    }

    // A plug-in window belongs to the thread that is loading the plug-in. If that thread never
    // comes back, the watchdog can be stuck inside ShowWindow()/GetWindowText() on it: deleting a
    // thread whose code is still running would pull the memory under it (and take this process
    // down in the middle of writing the result). This process exits right after this line, so
    // simply leaving such a thread behind is the safe choice.
    if (watchdog->stopThread (500))
        delete watchdog;

    return error.isEmpty() ? 0 : 3;
}

//==============================================================================
OutOfProcessScanner::OutOfProcessScanner()
{
    scratchFolder = File::getSpecialLocation (File::tempDirectory).getChildFile ("SoundXScanner");

    // remove leftovers from a session that was killed while a scan was in flight
    if (scratchFolder.isDirectory())
    {
        const auto cutoff = Time::getCurrentTime() - RelativeTime::hours (12);
        for (auto& f : scratchFolder.findChildFiles (File::findFiles, false))
            if (f.getLastModificationTime() < cutoff)
                f.deleteFile();
    }
}

OutOfProcessScanner::~OutOfProcessScanner() {}

static std::atomic<int>& timeoutStorage()
{
    // idle time (seconds) after which a helper that stopped using the CPU is considered stuck
    static std::atomic<int> seconds { 30 };
    return seconds;
}

int OutOfProcessScanner::getTimeoutSeconds()
{
    return jmax (5, timeoutStorage().load());
}

void OutOfProcessScanner::setTimeoutSeconds (int seconds)
{
    timeoutStorage() = jlimit (5, 600, seconds);
}

File OutOfProcessScanner::getLogFile()
{
    // next to the settings and the plug-in list (see PluginManager::dataDirectory)
    return PluginManager::dataDirectory().getChildFile ("plugin-scan.log");
}

StringArray OutOfProcessScanner::getFailureLog()
{
    const ScopedLock sl (logLock());
    return logEntries();
}

void OutOfProcessScanner::clearFailureLog()
{
    {
        const ScopedLock sl (logLock());
        logEntries().clear();
    }
    getLogFile().deleteFile();
}

void OutOfProcessScanner::appendLogLine (const String& line)
{
    appendToLog (line);
}

StringPairArray OutOfProcessScanner::getFailureReasons()
{
    // the file holds the previous sessions, the memory log the current one (and wins)
    StringArray lines;
    if (getLogFile().existsAsFile())
        getLogFile().readLines (lines);

    auto result = parseFailureLines (lines);
    const auto memory = getFailureLog();
    result.addArray (parseFailureLines (memory));
    return result;
}

void OutOfProcessScanner::addFailure (const String& file, const String& reason)
{
    appendToLog (file + "  --  " + reason);
}

//==============================================================================
void OutOfProcessScanner::beginLookingAt (const String& fileOrIdentifier)
{
    const ScopedLock sl (fileLock);
    currentFile = fileOrIdentifier;

    // A request for another file must never be applied to this one: a click that arrived while
    // the previous file was finishing belongs to that file, which is already gone.
    if (skipTarget != fileOrIdentifier)
        skipTarget.clear();
}

void OutOfProcessScanner::endLookingAt()
{
    const ScopedLock sl (fileLock);
    currentFile.clear();
    skipTarget.clear();
}

bool OutOfProcessScanner::skipRequestedForCurrentFile() const
{
    const ScopedLock sl (fileLock);
    return skipTarget.isNotEmpty() && skipTarget == currentFile;
}

String OutOfProcessScanner::skipCurrentFile()
{
    const ScopedLock sl (fileLock);
    if (currentFile.isEmpty())
        return {};                        // nothing is loading right now, so there is nothing to drop

    skipTarget = currentFile;
    return currentFile;
}

void OutOfProcessScanner::noteSkipped (const String& fileOrIdentifier)
{
    ++numSkipped;

    // Deliberately not the "<file>  --  <reason>" shape of a failure: a file the user dropped is
    // not a broken plug-in, and it must not show up as a deactivation reason later.
    appendToLog ("skipped by the user: " + fileOrIdentifier);
}

void OutOfProcessScanner::scanFinished()
{
    // nothing persistent to release: every plug-in ran in its own process
}

bool OutOfProcessScanner::findPluginTypesFor (AudioPluginFormat& format,
                                             OwnedArray<PluginDescription>& result,
                                             const String& fileOrIdentifier)
{
    // from here until this function returns, the plug-in manager's Skip button has a file it can
    // drop; a request made after this point belongs to this file and to no other
    beginLookingAt (fileOrIdentifier);
    const ScopeGuard forgetFile ([this] { endLookingAt(); });

    if (cancelFlag != nullptr && cancelFlag->load())
        return true;                 // the scan was stopped before this file was even started

    const auto exe = File::getSpecialLocation (File::currentExecutableFile);
    if (! exe.existsAsFile())
    {
        addFailure (fileOrIdentifier, TRANS ("The SoundX executable could not be found."));
        return false;
    }

    scratchFolder.createDirectory();
    // A tag unique across all helpers of this process *and* of every other running SoundX
    // instance (they share %TEMP%\SoundXScanner): a collision would make two scans overwrite each
    // other's result file.
   #if JUCE_WINDOWS
    const int processId = (int) GetCurrentProcessId();
   #else
    const int processId = (int) getpid();
   #endif
    const String tag = String (processId) + "-" + String (Time::currentTimeMillis()) + "-"
                       + String (++scanTagCounter());
    const File outFile = scratchFolder.getChildFile (tag + ".xml");
    const File noteFile = scratchFolder.getChildFile (tag + ".note");
    const File pidFile = scratchFolder.getChildFile (tag + ".pid");
    outFile.deleteFile();
    noteFile.deleteFile();
    pidFile.deleteFile();

    StringArray command;
    command.add (exe.getFullPathName());
    command.add (workerFlag);
    command.add (format.getName());
    command.add (fileOrIdentifier);
    command.add (outFile.getFullPathName());

    ChildProcess child;

    // no stdio pipes: the child reports through the result file only
    if (! child.start (command, 0))
    {
        addFailure (fileOrIdentifier, TRANS ("The scanner helper could not be started."));
        return false;
    }

    const uint32 pid = readWorkerPid (pidFile, 3000);
    const auto idleMs = (uint32) (getTimeoutSeconds() * 1000);
    const auto hardCapMs = (uint32) 5u * 60u * 1000u;       // absolute limit per plug-in
    // the helper writes its pid before it loads anything, so a scan that never reported one is
    // stuck in the worker's own startup rather than in a slow plug-in
    const auto noPidMs = jmax (20000u, idleMs);
    const auto started = Time::getMillisecondCounter();
    auto lastProgress = started;
   #if JUCE_WINDOWS || JUCE_MAC
    WorkerLoad loadAtLastProgress;
   #endif

    bool timedOut = false, cancelled = false, hardCapped = false, skipped = false;

    for (;;)
    {
        if (child.waitForProcessToFinish (60)) break;

        // KnownPluginList::CustomScanner::shouldExit() only knows about ThreadPool jobs, so a
        // scan running on a plain Thread (the plug-in manager's scan thread) has to ask its own
        // thread object as well - otherwise Cancel would not interrupt a slow plug-in at all.
        // Parallel helpers run on their own std::thread, where the scanner owner hands over a
        // cancellation flag instead.
        if ((cancelFlag != nullptr && cancelFlag->load())
            || shouldExit() || Thread::currentThreadShouldExit())
        {
            cancelled = true;
            break;
        }

        // Skip: this one file is dropped and the scan carries on with the next one. The helper is
        // killed below, but - unlike a cancellation - nothing else about the scan changes.
        if (skipRequestedForCurrentFile()) { skipped = true; break; }

        const auto now = Time::getMillisecondCounter();

       #if JUCE_WINDOWS || JUCE_MAC
        if (pid != 0)
        {
            const auto load = processLoad (pid);
            if (madeProgress (load, loadAtLastProgress, now - lastProgress))
            {
                loadAtLastProgress = load;
                lastProgress = now;
            }

            if (now - lastProgress >= idleMs) { timedOut = true; break; }
        }
        else
       #endif
        {
            if (now - started >= noPidMs) { timedOut = true; break; }
        }

        if (now - started >= hardCapMs) { hardCapped = true; break; }
    }

    if (cancelled || skipped || timedOut || hardCapped)
        child.kill();

    if (cancelled)
    {
        outFile.deleteFile();
        noteFile.deleteFile();
        pidFile.deleteFile();
        // the user cancelled the scan - that is not a broken plug-in
        return true;
    }

    if (skipped)
    {
        outFile.deleteFile();
        noteFile.deleteFile();
        pidFile.deleteFile();
        // the user asked for this file to go: not a failure, so KnownPluginList must not blacklist
        // it (returning false there would deactivate it for good)
        noteSkipped (fileOrIdentifier);
        return true;
    }

    const auto exitCode = (timedOut || hardCapped) ? 0u : child.getExitCode();

    std::unique_ptr<XmlElement> xml (outFile.existsAsFile() ? XmlDocument::parse (outFile) : nullptr);

    StringArray notes;
    if (noteFile.existsAsFile())
        notes.addLines (noteFile.loadFileAsString());

    if (xml != nullptr)
    {
        for (auto* e : xml->getChildIterator())
        {
            if (! e->hasTagName ("PLUGIN")) continue;
            PluginDescription d;
            if (d.loadFromXml (*e))
                result.add (new PluginDescription (d));
        }
    }

    outFile.deleteFile();
    noteFile.deleteFile();
    pidFile.deleteFile();

    if (! notes.isEmpty())
        addFailure (fileOrIdentifier, notes.joinIntoString (" "));

    if (timedOut || hardCapped)
    {
        addFailure (fileOrIdentifier, timedOut
                        ? TRANS ("Stopped answering while scanning (hang or a plug-in window).")
                        : TRANS ("The scan took far too long and was stopped."));
        return false;
    }

    if (xml == nullptr && exitCode != 0)
    {
        addFailure (fileOrIdentifier, TRANS ("Crashed while scanning."));
        return false;
    }

    if (xml != nullptr)
    {
        const auto error = xml->getStringAttribute ("error");
        if (error.isNotEmpty())
            addFailure (fileOrIdentifier, error);
    }

    return true;
}
}
