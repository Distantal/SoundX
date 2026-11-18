#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>

namespace sx
{
/** Command line switch that puts SoundX into plug-in scanning worker mode.

    Loading a third-party plug-in is the one thing a DAW cannot do safely in its own process:
    a broken or copy-protected plug-in can crash, hang forever, or pop up a modal window in the
    middle of a scan. Every scan is therefore handed to a throw-away child process, and only the
    resulting plug-in descriptions come back to the DAW.
*/
juce::String pluginScanWorkerFlag();

/** True when this process was started as a scan worker (it must not build any UI). */
bool isPluginScanWorkerCommandLine (const juce::StringArray& args);

/** Why macOS would not load a plug-in bundle: empty when the loader is happy, or on other
    platforms.

    On macOS a plug-in that fails to load usually fails at the loader (a binary for the other
    architecture, a broken or missing code signature, or a hardened host without the
    "disable library validation" entitlement - see installer/SoundX.entitlements). JUCE only
    reports "Unable to load VST-3 plug-in file", so this asks dyld for its own message, which is
    what makes a bug report actionable.
*/
juce::String describeBundleLoadProblem (const juce::String& fileOrIdentifier);

/** Worker side: scans one plug-in file and writes the result as XML, then returns the process
    exit code (0 = the scan ran to completion, anything else = the plug-in killed the process).
*/
int runPluginScanWorker (const juce::StringArray& args);

//==============================================================================
/** Parent side plug-in scanner: a KnownPluginList::CustomScanner that never loads a plug-in
    binary inside the DAW process.

    A scanning plug-in that crashes, hangs or opens a window only ever affects the helper
    process: its window is hidden and closed, and a process that does not answer in time is
    killed and reported, so the scan always keeps going and the DAW never freezes.
*/
class OutOfProcessScanner : public juce::KnownPluginList::CustomScanner
{
public:
    OutOfProcessScanner();
    ~OutOfProcessScanner() override;

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override;

    void scanFinished() override;

    //== dropping the file that is being looked at ========================================
    /** Asks the scan to drop the plug-in file it is looking at right now and carry on with the
        next one: the helper process of that file is killed and nothing about it is recorded -
        no plug-in entry, no deactivation. Only a line in the scan log says it happened.

        Returns the file that was asked to go (the full path it was scanned under), or an empty
        string when no file is being looked at at this very moment: there is nothing to skip then,
        and a click that lands between two files is ignored instead of dropping the file that
        comes next.

        Safe to call from the message thread while the scan thread is inside
        findPluginTypesFor().
    */
    juce::String skipCurrentFile();

    /** How many files this scanner has dropped at the user's request (the scan thread compares
        this before and after each file to know whether it was skipped). */
    int getNumSkipped() const noexcept        { return numSkipped.load(); }

    /** A parallel scan runs several instances of this scanner at once, each on its own thread.
        The scan thread hands each of them a flag it sets when the whole scan is cancelled, so a
        helper process is killed even though no JUCE ThreadPoolJob is attached to the thread. */
    void setCancellationFlag (const std::atomic<bool>* flag) { cancelFlag = flag; }

    //== settings / report
    /** Seconds a single plug-in may take before the helper process is killed (default 40). */
    static int getTimeoutSeconds();
    static void setTimeoutSeconds (int seconds);

    /** Human readable description of the most recent scan failures (newest last). */
    static juce::StringArray getFailureLog();
    static juce::File getLogFile();
    static void clearFailureLog();

    /** Appends one line to the scan log (also kept in memory for the report). */
    static void appendLogLine (const juce::String& line);

    /** Why each plug-in file failed to scan, keyed by file path (newest reason wins).

        Merges this session's in-memory log with the log file, so the plug-in manager can explain
        a deactivated entry from a previous session as well.
    */
    static juce::StringPairArray getFailureReasons();

private:
    void addFailure (const juce::String& file, const juce::String& reason);

    /** Marks one file as "being looked at" (and forgets a skip request left over from an earlier
        one), and undoes that again. Both are called by findPluginTypesFor() only. */
    void beginLookingAt (const juce::String& fileOrIdentifier);
    void endLookingAt();
    /** True when the file being looked at has a skip request waiting for it. */
    bool skipRequestedForCurrentFile() const;
    /** Records one dropped file: counts it and says so in the log. */
    void noteSkipped (const juce::String& fileOrIdentifier);

    juce::File scratchFolder;

    // which file the scan thread has a helper process for, and which file the user asked to drop.
    // Both are written by the message thread (skipCurrentFile) and the scan thread, hence the lock.
    mutable juce::CriticalSection fileLock;
    juce::String currentFile, skipTarget;
    std::atomic<int> numSkipped { 0 };
    const std::atomic<bool>* cancelFlag = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutOfProcessScanner)
};
}
