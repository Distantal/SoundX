#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include "model/Model.h"
#include "engine/Engine.h"
#include "engine/PluginManager.h"
#include "engine/PluginScanProcess.h"
#include "ui/PluginManagerWindow.h"
#include "ui/AppState.h"
#include "ui/MainComponent.h"
#include "ui/SlotMenus.h"
#include "ui/Importer.h"
#include "ui/Localisation.h"
#include "ui/Theme.h"
#include "ui/TitleBar.h"
#include "ui/WindowChrome.h"
#include "ui/Dialogs.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <dbghelp.h>
#endif

namespace sx
{
int runSelfTest (Project&, Engine&, PluginManager&, const juce::StringArray&);
void buildDemoProject (Project&, Engine&, int& leadTrack, int& leadClip);

//==============================================================================
// Crash log: an unhandled exception used to leave nothing behind but a Windows "not responding"
// dialog, which made every crash report a guessing game. This writes the exception, the faulting
// module / offset and a symbolised stack (function + source line when the PDB is next to the build
// or found through _NT_SYMBOL_PATH / SOUNDX_SYMBOL_PATH) into %APPDATA%\SoundX\crash-<time>.log.
// It is deliberately allocation-light and runs from the faulting thread itself.
#if JUCE_WINDOWS
namespace
{
    const char* crashExceptionName (DWORD code)
    {
        switch (code)
        {
            case EXCEPTION_ACCESS_VIOLATION:  return "ACCESS_VIOLATION";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
            case EXCEPTION_BREAKPOINT:        return "BREAKPOINT";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLOAT_DIVIDE_BY_ZERO";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
            case EXCEPTION_IN_PAGE_ERROR:     return "IN_PAGE_ERROR";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
            case EXCEPTION_PRIV_INSTRUCTION:  return "PRIV_INSTRUCTION";
            case EXCEPTION_STACK_OVERFLOW:    return "STACK_OVERFLOW";
            case 0xC0000374:                  return "HEAP_CORRUPTION";
            case 0xC0000409:                  return "STACK_BUFFER_OVERRUN";
            case 0xE06D7363:                  return "CPP_EXCEPTION";
            default:                          return "UNKNOWN";
        }
    }

    juce::String crashAddress (DWORD64 address)
    {
        auto s = juce::String ("0x") + juce::String::toHexString ((juce::int64) address);
        HMODULE mod = nullptr;
        if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR> (address), &mod)
            && mod != nullptr)
        {
            wchar_t path[1024] {};
            if (GetModuleFileNameW (mod, path, (DWORD) juce::numElementsInArray (path)) > 0)
                s << " (" << juce::File (juce::String (path)).getFileName() << "+0x"
                  << juce::String::toHexString ((juce::int64) (address - (DWORD64) mod)) << ")";
        }
        return s;
    }

    juce::String crashStack (CONTEXT context)
    {
        juce::String out;
        const auto process = GetCurrentProcess();
        const auto thread  = GetCurrentThread();

        juce::String searchPath = juce::SystemStats::getEnvironmentVariable ("SOUNDX_SYMBOL_PATH", {});
        if (searchPath.isEmpty()) searchPath = juce::SystemStats::getEnvironmentVariable ("_NT_SYMBOL_PATH", {});
        if (searchPath.isEmpty())
            searchPath = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                             .getParentDirectory().getParentDirectory().getParentDirectory()
                             .getFullPathName();

        SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES
                        | SYMOPT_FAIL_CRITICAL_ERRORS);
        const bool syms = SymInitialize (process, searchPath.toRawUTF8(), TRUE) != FALSE;

        STACKFRAME64 frame {};
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrStack.Mode   = AddrModeFlat;
        frame.AddrFrame.Mode   = AddrModeFlat;
       #if defined(_M_X64)
        frame.AddrPC.Offset    = context.Rip;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrFrame.Offset = context.Rbp;
        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
       #else
        frame.AddrPC.Offset    = context.Eip;
        frame.AddrStack.Offset = context.Esp;
        frame.AddrFrame.Offset = context.Ebp;
        const DWORD machine = IMAGE_FILE_MACHINE_I386;
       #endif

        for (int i = 0; i < 96; ++i)
        {
            if (! StackWalk64 (machine, process, thread, &frame, &context, nullptr,
                               SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (frame.AddrPC.Offset == 0)
                break;

            out << juce::String (i).paddedLeft (' ', 2) << ": " << crashAddress (frame.AddrPC.Offset);

            if (syms)
            {
                union
                {
                    SYMBOL_INFO info;
                    char storage[sizeof (SYMBOL_INFO) + 1024] {};
                } sym {};
                sym.info.SizeOfStruct = sizeof (SYMBOL_INFO);
                sym.info.MaxNameLen = 1000;
                DWORD64 displacement = 0;
                if (SymFromAddr (process, frame.AddrPC.Offset, &displacement, &sym.info))
                {
                    out << "  " << sym.info.Name;
                    if (displacement != 0)
                        out << "+0x" << juce::String::toHexString ((juce::int64) displacement);
                }

                IMAGEHLP_LINE64 line {};
                line.SizeOfStruct = sizeof (line);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64 (process, frame.AddrPC.Offset, &lineDisplacement, &line))
                    if (line.FileName != nullptr)
                        out << "  [" << juce::File (juce::String (line.FileName)).getFileName()
                            << ":" << (int) line.LineNumber << "]";
            }
            out << "\n";
        }

        if (syms) SymCleanup (process);
        return out;
    }

    void soundXCrashHandler (void* exceptionInfo) noexcept
    {
        // first-chance fatal exceptions (heap corruption is raised before the unhandled filter
        // sees them) must not write the file twice
        static std::atomic<bool> wrote { false };
        if (wrote.exchange (true, std::memory_order_acq_rel))
            return;
        try
        {
            auto* ep = static_cast<EXCEPTION_POINTERS*> (exceptionInfo);
            juce::String out;
            out << "SoundX crash report\n";
            out << "time     : " << juce::Time::getCurrentTime().toString (true, true) << "\n";
            out << "version  : " << JUCE_APPLICATION_VERSION_STRING << "\n";
            out << "os       : " << juce::SystemStats::getOperatingSystemName()
                << " / " << juce::SystemStats::getCpuModel() << "\n";

            CONTEXT context {};
            if (ep != nullptr && ep->ExceptionRecord != nullptr)
            {
                auto* rec = ep->ExceptionRecord;
                out << "exception: " << crashExceptionName (rec->ExceptionCode)
                    << " (0x" << juce::String::toHexString ((juce::int64) rec->ExceptionCode) << ")"
                    << " at " << crashAddress ((DWORD64) rec->ExceptionAddress) << "\n";
                if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
                {
                    const auto kind = rec->ExceptionInformation[0];
                    out << "access   : " << (kind == 0 ? "read" : kind == 1 ? "write" : "execute")
                        << " " << crashAddress ((DWORD64) rec->ExceptionInformation[1]) << "\n";
                }
                if (rec->ExceptionCode == 0xE06D7363 && rec->NumberParameters >= 1)
                {
                    // an uncaught C++ exception: the thrown object's type is the first parameter
                    out << "cpp type : " << crashAddress ((DWORD64) rec->ExceptionInformation[0]) << "\n";
                }
                if (ep->ContextRecord != nullptr)
                    context = *ep->ContextRecord;
            }
            if (ep == nullptr || ep->ContextRecord == nullptr)
                RtlCaptureContext (&context);

            out << "stack    :\n" << crashStack (context);

            // the self test (and whoever reproduces) can point the report at a known folder
            auto override = juce::SystemStats::getEnvironmentVariable ("SOUNDX_CRASH_DIR", {});
            auto dir = override.isNotEmpty() ? juce::File (override) : PluginManager::dataDirectory();
            dir.createDirectory();
            const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
            auto file = dir.getChildFile ("crash-" + stamp + ".log");
            file.replaceWithText (out);
           #if JUCE_WINDOWS
            ::OutputDebugStringA (("SoundX crash: " + file.getFullPathName() + "\n").toRawUTF8());
           #endif
        }
        catch (...) {}
    }

    /** Heap corruption / fatal exceptions are sent to the process before the unhandled-exception
        filter: log them first-chance so a crash still names the operation that noticed it. */
    LONG CALLBACK soundXVectoredHandler (PEXCEPTION_POINTERS ep) noexcept
    {
        if (ep != nullptr && ep->ExceptionRecord != nullptr)
        {
            const auto code = ep->ExceptionRecord->ExceptionCode;
            if (code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */
                || code == 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */)
                soundXCrashHandler (ep);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

static void installCrashHandler()
{
   #if JUCE_WINDOWS
    juce::SystemStats::setApplicationCrashHandler (soundXCrashHandler);
    AddVectoredExceptionHandler (1, soundXVectoredHandler);
   #endif
}
#else
static void installCrashHandler() {}
#endif

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (AppState& a, std::function<void()> onClose)
        : juce::DocumentWindow ("SoundX", theme::window, juce::DocumentWindow::allButtons),
          titleBar (*this), app (a), closeCallback (std::move (onClose))
    {
        // The whole frame is ours: the OS title bar kept its light Windows colours in Dark Mode,
        // and it could not carry the SoundX look. SXTitleBar draws the bar from the theme palette.
        setUsingNativeTitleBar (false);
        setTitleBarButtonsRequired (0, false);      // the bar has its own buttons
        setTitleBarHeight (kTitleBarHeight);
        setResizable (true, false);                 // edge resize, no extra corner grip over the UI
        setResizeLimits (1000, 620, 8000, 4000);
        // JUCE's "native shadow" mode extends the DWM frame into a borderless window. That frame
        // reserves the system caption-button area at the top right (covering the custom
        // minimise / maximise / close buttons) and draws the accent border. We draw the whole
        // title bar ourselves, so turn it off.
        setDropShadowEnabled (false);
        addAndMakeVisible (titleBar);
        removePlatformWindowBorder (*this);

        titleBar.onClose    = [this] { closeButtonPressed(); };
        titleBar.onMinimise = [this] { setMinimised (true); };
        titleBar.onMaximise = [this] { toggleMaximised(); };

        {
            auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
            const int w = juce::jmin (1440, (int) (area.getWidth() * 0.94)), h = juce::jmin (880, (int) (area.getHeight() * 0.92));
            centreWithSize (w, h);
        }
        installContent();
    }

    // clear the callbacks before the owned content is deleted: they point back into this window
    ~MainWindow() override
    {
        if (auto* c = getContentComponent())
        {
            if (auto* mc = dynamic_cast<MainComponent*> (c))
            {
                mc->onTitleChanged = nullptr;
                mc->onThemeChange = nullptr;
                mc->onToggleAppFullscreen = nullptr;
                mc->onExitAppFullscreen = nullptr;
            }
            setContentOwned (nullptr, false);   // delete content before titleBar / the window goes away
        }
    }

    // Builds the content component once, when the window comes up. A theme switch does NOT go
    // through here any more: MainComponent::refreshTheme() repaints the live tree in place.
    void installContent()
    {
        auto* main = new MainComponent (app);
        main->onTitleChanged = [this] (const juce::String& t) { setName (t); titleBar.setTitle (t); };
        // A theme switch repaints the window in place: the current layout, the open panels, the
        // scroll positions and any plug-in panel stay exactly where they are. Rebuilding the
        // content (the old behaviour) reset the mixer / piano-roll panel and every scroll offset.
        main->onThemeChange = [this] { applyTheme(); };
        main->onToggleAppFullscreen = [this] { toggleMaximised(); };
        main->onExitAppFullscreen = [this]
        {
            if (! fullscreen) return false;
            toggleMaximised();
            return true;
        };
        setContentOwned (main, false);
        main->refreshHostLayer();      // now that the content has a peer, native plug-in panels attach as child HWNDs
        setName (main->windowTitle());
        titleBar.setTitle (main->windowTitle());
        applyTheme();
        setVisible (true);
        titleBar.toFront (false);
        titleBar.repaint();
        removePlatformWindowBorder (*this);
        main->grabKeyboardFocus();
    }

    void paint (juce::Graphics& g) override
    {
        // The title bar is a child component; only the frame around the content is painted here.
        juce::ResizableWindow::paint (g);
    }

    juce::BorderSize<int> getContentComponentBorder() const override
    {
        // In app-fullscreen mode the title bar is the only piece above the content; the OS frame
        // (and its 1 px inset) must not take any more room.
        if (fullscreen) return { kTitleBarHeight, 0, 0, 0 };
        return juce::DocumentWindow::getContentComponentBorder();
    }

    void resized() override
    {
        DocumentWindow::resized();

        if (fullscreen)
            titleBar.setBounds (0, 0, getWidth(), kTitleBarHeight);
        else
            titleBar.setBounds (getTitleBarArea());

        titleBar.toFront (false);
        titleBar.repaint();
    }

    void activeWindowStatusChanged() override
    {
        DocumentWindow::activeWindowStatusChanged();
        removePlatformWindowBorder (*this);
        titleBar.repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // When the title bar itself (rather than the content) has the focus after a click, the
        // content's shortcut handler does not see the key. The two fullscreen exits are also
        // useful when the custom title bar's buttons are hidden behind a fullscreen surface.
        if (fullscreen
            && (key.getKeyCode() == juce::KeyPress::escapeKey
                || (key.getKeyCode() == juce::KeyPress::returnKey && key.getModifiers().isAltDown())))
        {
            toggleMaximised();
            return true;
        }

        return juce::DocumentWindow::keyPressed (key);
    }

    MainComponent* content() { return dynamic_cast<MainComponent*> (getContentComponent()); }
    void closeButtonPressed() override { closeCallback(); }

private:
    static constexpr int kTitleBarHeight = 34;

    void applyTheme()
    {
        setColour (juce::ResizableWindow::backgroundColourId, theme::window);
        setColour (juce::DocumentWindow::textColourId, theme::text);
        titleBar.refreshTheme();
        repaint();
    }

    void toggleMaximised()
    {
        if (fullscreen) exitFullscreen();
        else enterFullscreen();
    }

    void enterFullscreen()
    {
        if (fullscreen) return;

        auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getBounds());
        if (display == nullptr) display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        if (display == nullptr) return;

        windowedBounds = getBounds();

       #if JUCE_WINDOWS
        // JUCE's own setFullScreen() is just a normal maximise on Windows. We use a borderless
        // popup so the window has no system frame, but keep it on the display's work area so the
        // taskbar stays visible.
        if (auto* handle = getWindowHandle())
        {
            auto* hwnd = reinterpret_cast<HWND> (handle);
            windowedStyle = GetWindowLongPtrW (hwnd, GWL_STYLE);
            const auto popupStyle = (windowedStyle
                                     & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU))
                                    | WS_POPUP | WS_VISIBLE;
            SetWindowLongPtrW (hwnd, GWL_STYLE, popupStyle);
        }
       #endif

        fullscreen = true;
        setBounds (display->userArea);

       #if JUCE_WINDOWS
        if (auto* handle = getWindowHandle())
            SetWindowPos (reinterpret_cast<HWND> (handle), HWND_TOP, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
       #endif

        removePlatformWindowBorder (*this);
        titleBar.setMaximised (true);
        resized();
        repaint();
    }

    void exitFullscreen()
    {
        if (! fullscreen) return;

        fullscreen = false;

       #if JUCE_WINDOWS
        if (auto* handle = getWindowHandle())
        {
            auto* hwnd = reinterpret_cast<HWND> (handle);
            if (windowedStyle != 0)
                SetWindowLongPtrW (hwnd, GWL_STYLE, windowedStyle);
            SetWindowPos (hwnd, HWND_TOP, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOZORDER);
        }
       #endif

        setBounds (windowedBounds);
        removePlatformWindowBorder (*this);
        titleBar.setMaximised (false);
        resized();
        repaint();
    }

    SXTitleBar titleBar;
    AppState& app;
    std::function<void()> closeCallback;
    bool fullscreen = false;
    juce::Rectangle<int> windowedBounds;
   #if JUCE_WINDOWS
    LONG_PTR windowedStyle = 0;
   #endif
};
}

class SoundXApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "SoundX"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    // marker that only survives when the application is killed instead of closed
    static juce::File sessionMarkerFile()
    {
        // same folder as the settings and the plug-in list (%APPDATA% on Windows,
        // ~/Library/Application Support on macOS)
        return sx::PluginManager::dataDirectory().getChildFile ("session.lock");
    }

    void initialise (const juce::String&) override
    {
        // crash reporting first: a crash inside plug-in loading or the audio device should leave a
        // readable log behind instead of just the Windows "not responding" dialog
        sx::installCrashHandler();

       #if JUCE_WINDOWS
        // A CPU-heavy plug-in (Ozone and similar) runs its DSP on the audio thread at real-time
        // priority (and often on its own worker threads). That can starve the normal-priority
        // message thread, which is what made the menus and buttons lag while audio stayed fine.
        // Give the UI thread a modest boost — still below the audio thread — so it stays
        // responsive under load. SOUNDX_UI_PRIORITY=highest|above|normal lets a user A/B the
        // trade-off without a rebuild (highest can cost audio headroom on a loaded machine).
        int uiPriority = THREAD_PRIORITY_ABOVE_NORMAL;
        if (auto mode = juce::SystemStats::getEnvironmentVariable ("SOUNDX_UI_PRIORITY", {});
            mode.equalsIgnoreCase ("highest"))
            uiPriority = THREAD_PRIORITY_HIGHEST;
        else if (mode.equalsIgnoreCase ("normal"))
            uiPriority = THREAD_PRIORITY_NORMAL;
        SetThreadPriority (GetCurrentThread(), uiPriority);
       #endif

        // plug-in scanning helper: load exactly one plug-in, report back through a file and
        // exit. It must never build any UI, so this is checked before anything else.
        if (sx::isPluginScanWorkerCommandLine (getCommandLineParameterArray()))
        {
            setApplicationReturnValue (sx::runPluginScanWorker (getCommandLineParameterArray()));
            quit();
            return;
        }

        plugins = std::make_unique<sx::PluginManager>();
        sx::theme::setDark (plugins->properties()->getBoolValue ("darkMode", false));
        lnf.applyColours();
        juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

        sx::applyLanguage (sx::currentLanguageSetting (plugins->properties()));
        project = std::make_unique<sx::Project>();
        engine = std::make_unique<sx::Engine> (*project, *plugins);
        state = std::make_unique<sx::AppState> (*project, *engine, *plugins);
        engine->onError = [] (const juce::String& m) { sx::showError (m); };
        state->onError = engine->onError;

        // a recording folder picked by the user is remembered between sessions
        {
            const juce::File recDir (plugins->properties()->getValue ("recordingFolder"));
            if (recDir.getFullPathName().isNotEmpty()) engine->setRecordingBaseFolder (recDir);
        }

        if (getCommandLineParameterArray().contains ("--selftest"))
        {
            const int code = sx::runSelfTest (*project, *engine, *plugins, getCommandLineParameterArray());
            setApplicationReturnValue (code);
            quit();
            return;
        }

        // A session starts empty: no tracks at all. Unless the user saved a project template, in
        // which case that is what "opening the program" means (File > Save as Template).
        {
            const auto tpl = sx::PluginManager::templateFile();
            if (tpl.existsAsFile()) project->loadFromTemplate (tpl);

            project->dirty = false;
            state->selectedTrack = project->tracks.empty() ? -1 : project->tracks.front()->id;
            state->selectedTracks.clear();
            if (state->selectedTrack >= 0) state->selectedTracks.insert (state->selectedTrack);
            engine->setSelectedTrack (state->selectedTrack);
        }

        // abnormal-exit marker: its presence at startup means the previous run did not shut down
        // cleanly, so autosave recovery should be offered even if the project looks up to date
        {
            const auto marker = sessionMarkerFile();
            previousRunCrashed = marker.existsAsFile();
            marker.getParentDirectory().createDirectory();
            marker.replaceWithText (juce::Time::getCurrentTime().toString (true, true));
        }

        // bring the window up first, then open the audio device in the background so
        // startup stays instant even when the device takes a while to initialise
        window = std::make_unique<sx::MainWindow> (*state, [this] { systemRequestedQuit(); });

        // Some third-party plug-ins create their own popup windows (preset / mode menus) as child
        // windows of their editor. Direct2D presents the host window through a swap chain, and on
        // some drivers that makes those native popups flicker or get dismissed before a value can
        // be picked. --software-render (or SOUNDX_SOFTWARE_RENDER=1) switches the main window to
        // JUCE's software renderer: slower repaints, but every plug-in popup behaves like in a
        // classic GDI host. It is also the switch used to A/B this on a user's machine.
        if (getCommandLineParameterArray().contains ("--software-render")
            || juce::SystemStats::getEnvironmentVariable ("SOUNDX_SOFTWARE_RENDER", {}) == "1")
        {
            juce::MessageManager::callAsync ([this]
            {
                if (window != nullptr)
                    if (auto* peer = window->getPeer())
                        peer->setCurrentRenderingEngine (0);
            });
        }

        // --manual opens the user manual (same path the Help menu uses), handy for shortcuts
        if (getCommandLineParameterArray().contains ("--manual"))
        {
            const auto f = sx::PluginManager::dataDirectory().getChildFile ("SoundX-Manual.pdf");
            if (sx::writeUserManual (f) && ! f.startAsProcess())
                juce::URL (f).launchInDefaultBrowser();
        }

        {
            auto params = getCommandLineParameterArray();
            const int oe = params.indexOf ("--open-editor");
            if (oe >= 0 && oe + 1 < params.size())
            {
                juce::OwnedArray<juce::PluginDescription> found;
                for (int i = 0; i < plugins->formats.getNumFormats(); ++i)
                    if (plugins->formats.getFormat (i)->fileMightContainThisPluginType (params[oe + 1]))
                        plugins->formats.getFormat (i)->findAllTypesForFile (found, params[oe + 1]);
                if (found.size() > 0)
                {
                    juce::String perr;
                    const auto& d = *found[0];
                    // an empty session has no track for an effect: that case is skipped
                    if (! d.isInstrument && project->tracks.empty())
                        sx::showError (TRANS ("Add a track first: a new session starts empty."));
                    else if (d.isInstrument && state->selectedTrack < 0)
                        sx::showError (TRANS ("Add an instrument track first: a new session starts empty."));
                    else
                    {
                        const int tid = d.isInstrument ? state->selectedTrack : project->tracks.back()->id;
                        const bool ok = d.isInstrument ? engine->setInstrument (tid, d, perr) : engine->addInsert (tid, d, perr);
                        if (ok)
                        {
                            auto* t = project->findTrack (tid);
                            state->showPluginEditor (d.isInstrument ? t->instrument.uid : t->inserts.back().uid);
                        }
                        else sx::showError (perr);
                    }
                }
            }
        }

        if (getCommandLineParameterArray().contains ("--demo"))
        {
            int lt = 0, lc = 0;
            sx::buildDemoProject (*project, *engine, lt, lc);
            state->selectedTrack = -1;
            state->selectedTracks.clear();
            state->openClipInEditor (lt, lc);
        }

        // preview / screenshot helper: start with the master bus row and two of its automation
        // sub-rows expanded (the row itself lives in Track menu > Master Bus Track)
        if (getCommandLineParameterArray().contains ("--master-row"))
        {
            state->showMasterTrack = true;
            auto& vol = project->masterFaderLane();
            if (vol.points.empty()) vol.points = { { 0.0, 0.72f }, { 4.0, 0.62f }, { 8.0, 0.7f }, { 12.0, 0.55f }, { 16.0, 0.72f } };
            vol.visible = true;
            vol.label = "Master Volume";
            auto& other = project->masterTrack.ensureLane ("pan", "Master Pan");
            if (other.points.empty()) other.points = { { 0.0, 0.5f }, { 6.0, 0.62f }, { 12.0, 0.38f }, { 16.0, 0.5f } };
            other.visible = true;
            project->sendContentChanged();
            state->notifyView();
        }

        juce::MessageManager::callAsync ([this]
        {
            if (window != nullptr && window->content() != nullptr)
            {
                window->content()->setPreviousRunCrashed (previousRunCrashed);
                window->content()->offerAutosaveRecovery();
            }
        });

        // preview / QA helper: open the plug-in manager without clicking through the menus, and
        // optionally start a scan of one folder right away
        if (getCommandLineParameterArray().contains ("--plugin-manager"))
        {
            const auto params = getCommandLineParameterArray();
            const int sf = params.indexOf ("--scan-folder");
            juce::MessageManager::callAsync ([this, params, sf]
            {
                if (auto* w = plugins->showScannerWindow())
                    if (sf >= 0 && sf + 1 < params.size())
                        w->scanFolder (juce::File (params[sf + 1]), false);
            });
        }

        for (auto& a : getCommandLineParameterArray())
        {
            juce::File f (a);
            if (f.existsAsFile() && f.hasFileExtension ("sxp"))
            {
                if (project->loadFrom (f)) { engine->syncAll(); state->selectedTrack = project->tracks.empty() ? -1 : project->tracks.front()->id; state->selectedTracks = state->selectedTrack >= 0 ? std::set<int> { state->selectedTrack } : std::set<int>(); }
            }
            else if (f.existsAsFile() && sx::isMidiFile (f))
            {
                int n = 0;
                project->reset();
                project->importMidi (f, 0.0, n);
                project->sendStructureChanged();
                project->sendContentChanged();
            }
        }

        juce::MessageManager::callAsync ([this]
        {
            if (engine == nullptr) return;
            const auto err = engine->initialiseAudio();
            if (err.isNotEmpty())
                sx::showError (TRANS ("Audio device problem:") + "\n" + err);
        });

        if (plugins->known.getNumTypes() == 0 && ! plugins->properties()->getBoolValue ("firstScanShown", false))
        {
            plugins->properties()->setValue ("firstScanShown", true);
            plugins->properties()->saveIfNeeded();
            juce::MessageManager::callAsync ([this] { plugins->showScannerWindow(); });
        }
    }

    void systemRequestedQuit() override
    {
        if (window != nullptr && window->content() != nullptr) window->content()->requestQuit();
        else quit();
    }

    void shutdown() override
    {
        window.reset();
        state.reset();
        engine.reset();
        project.reset();
        plugins.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        sessionMarkerFile().deleteFile();   // clean exit: no recovery prompt next time
    }

private:
    sx::SXLookAndFeel lnf;
    std::unique_ptr<sx::PluginManager> plugins;
    std::unique_ptr<sx::Project> project;
    std::unique_ptr<sx::Engine> engine;
    std::unique_ptr<sx::AppState> state;
    std::unique_ptr<sx::MainWindow> window;
    bool previousRunCrashed = false;
};

START_JUCE_APPLICATION (SoundXApp)
