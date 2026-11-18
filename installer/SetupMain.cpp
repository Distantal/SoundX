// Standalone installer for SoundX (bilingual EN / Chinese).
// Chinese strings are stored as UTF-8 byte escapes so the source file stays pure ASCII and the
// text is correct no matter which code page the compiler assumes.
#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>

namespace
{
struct InstallOptions
{
    juce::File dir;
    bool desktopShortcut = true;
    bool startMenuShortcut = true;
    bool launchAfter = true;
    bool chinese = false;
};

//== bilingual text table =====================================================================
namespace Str
{
    enum Id
    {
        Title, Subtitle, InstallFolder, Browse, Language, LanguageName,
        Desktop, StartMenu, Launch, Install, Exit,
        Preparing, Writing, WritingRuntime, UninstallHelper, CreatingDesktop, CreatingStart, Done, InstalledTo,
        ErrCreateDir, ErrWrite, ErrWriteFail, ErrTitle, OkTitle, OkBody, OK, OpenFolder,
        ChooseFolder, WinTitle, UninstallCmd, Count
    };

    static const char* en[Id::Count] =
    {
        "SoundX Setup",
        "Choose the install folder, then click Install.",
        "Install folder:",
        "Browse...",
        "Language",
        "English",
        "Create desktop shortcut",
        "Create Start Menu shortcut",
        "Launch SoundX when finished",
        "Install",
        "Exit",
        "Preparing...",
        "Writing the runtime files...",
        "Writing program files...",
        "Writing the uninstall helper...",
        "Creating the desktop shortcut...",
        "Creating the Start Menu shortcut...",
        "Done",
        "Installed to ",
        "Cannot create the install folder: ",
        "Cannot write (permission denied?): ",
        "Write failed (no disk space or permission denied)",
        "Install failed",
        "Installation complete",
        "SoundX was installed to:\n@DIR\n\nTo uninstall, run \"Uninstall SoundX.cmd\" in that folder.",
        "OK",
        "Open folder",
        "Choose install folder",
        "SoundX Setup",
        "@echo off\r\necho Removing SoundX...\r\ntimeout /t 1 >nul\r\nrmdir /s /q \"@DIR\"\r\n"
    };

    static const char* zh[Id::Count] =
    {
        /*ZH:Title*/ "SoundX \xE5\xAE\x89\xE8\xA3\x85\xE7\xA8\x8B\xE5\xBA\x8F",
        /*ZH:Subtitle*/ "\xE9\x80\x89\xE6\x8B\xA9\xE5\xAE\x89\xE8\xA3\x85\xE4\xBD\x8D\xE7\xBD\xAE\xE5\x90\x8E\xE7\x82\xB9\xE5\x87\xBB\xE3\x80\x8C\xE5\xAE\x89\xE8\xA3\x85\xE3\x80\x8D\xE3\x80\x82",
        /*ZH:InstallFolder*/ "\xE5\xAE\x89\xE8\xA3\x85\xE4\xBD\x8D\xE7\xBD\xAE\xEF\xBC\x9A",
        /*ZH:Browse*/ "\xE6\xB5\x8F\xE8\xA7\x88...",
        /*ZH:Language*/ "\xE8\xAF\xAD\xE8\xA8\x80",
        /*ZH:LanguageName*/ "\xE4\xB8\xAD\xE6\x96\x87",
        /*ZH:Desktop*/ "\xE5\x88\x9B\xE5\xBB\xBA\xE6\xA1\x8C\xE9\x9D\xA2\xE5\xBF\xAB\xE6\x8D\xB7\xE6\x96\xB9\xE5\xBC\x8F",
        /*ZH:StartMenu*/ "\xE5\x88\x9B\xE5\xBB\xBA\xE5\xBC\x80\xE5\xA7\x8B\xE8\x8F\x9C\xE5\x8D\x95\xE5\xBF\xAB\xE6\x8D\xB7\xE6\x96\xB9\xE5\xBC\x8F",
        /*ZH:Launch*/ "\xE5\xAE\x89\xE8\xA3\x85\xE5\xAE\x8C\xE6\x88\x90\xE5\x90\x8E\xE5\x90\xAF\xE5\x8A\xA8 SoundX",
        /*ZH:Install*/ "\xE5\xAE\x89\xE8\xA3\x85",
        /*ZH:Exit*/ "\xE9\x80\x80\xE5\x87\xBA",
        /*ZH:Preparing*/ "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x87\x86\xE5\xA4\x87...",
        /*ZH:WritingRuntime*/ "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x86\x99\xE5\x85\xA5\xE8\xBF\x90\xE8\xA1\x8C\xE5\xBA\x93\xE6\x96\x87\xE4\xBB\xB6...",
        /*ZH:Writing*/ "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x86\x99\xE5\x85\xA5\xE7\xA8\x8B\xE5\xBA\x8F\xE6\x96\x87\xE4\xBB\xB6...",
        /*ZH:UninstallHelper*/ "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x9B\xE5\xBB\xBA\xE5\x8D\xB8\xE8\xBD\xBD\xE7\xA8\x8B\xE5\xBA\x8F...",
        /*ZH:CreatingDesktop*/ "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x9B\xE5\xBB\xBA\xE6\xA1\x8C\xE9\x9D\xA2\xE5\xBF\xAB\xE6\x8D\xB7\xE6\x96\xB9\xE5\xBC\x8F...",
        /*ZH:CreatingStart*/ "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x9B\xE5\xBB\xBA\xE5\xBC\x80\xE5\xA7\x8B\xE8\x8F\x9C\xE5\x8D\x95\xE5\xBF\xAB\xE6\x8D\xB7\xE6\x96\xB9\xE5\xBC\x8F...",
        /*ZH:Done*/ "\xE5\xAE\x8C\xE6\x88\x90",
        /*ZH:InstalledTo*/ "\xE5\xB7\xB2\xE5\xAE\x89\xE8\xA3\x85\xE5\x88\xB0 ",
        /*ZH:ErrCreateDir*/ "\xE6\x97\xA0\xE6\xB3\x95\xE5\x88\x9B\xE5\xBB\xBA\xE5\xAE\x89\xE8\xA3\x85\xE7\x9B\xAE\xE5\xBD\x95\xEF\xBC\x9A",
        /*ZH:ErrWrite*/ "\xE6\x97\xA0\xE6\xB3\x95\xE5\x86\x99\xE5\x85\xA5\xEF\xBC\x88\xE6\x9D\x83\xE9\x99\x90\xE4\xB8\x8D\xE8\xB6\xB3\xEF\xBC\x9F\xEF\xBC\x89\xEF\xBC\x9A",
        /*ZH:ErrWriteFail*/ "\xE5\x86\x99\xE5\x85\xA5\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x88\xE7\xA3\x81\xE7\x9B\x98\xE7\xA9\xBA\xE9\x97\xB4\xE4\xB8\x8D\xE8\xB6\xB3\xE6\x88\x96\xE6\x9D\x83\xE9\x99\x90\xE4\xB8\x8D\xE8\xB6\xB3\xEF\xBC\x89",
        /*ZH:ErrTitle*/ "\xE5\xAE\x89\xE8\xA3\x85\xE5\xA4\xB1\xE8\xB4\xA5",
        /*ZH:OkTitle*/ "\xE5\xAE\x89\xE8\xA3\x85\xE5\xAE\x8C\xE6\x88\x90",
        /*ZH:OkBody*/ "SoundX \xE5\xB7\xB2\xE5\xAE\x89\xE8\xA3\x85\xE5\x88\xB0\xEF\xBC\x9A\n@DIR\n\n\xE5\x8D\xB8\xE8\xBD\xBD\xEF\xBC\x9A\xE8\xBF\x90\xE8\xA1\x8C\xE8\xAF\xA5\xE7\x9B\xAE\xE5\xBD\x95\xE4\xB8\xAD\xE7\x9A\x84\xE3\x80\x8CUninstall SoundX.cmd\xE3\x80\x8D\xE3\x80\x82",
        /*ZH:OK*/ "\xE7\xA1\xAE\xE5\xAE\x9A",
        /*ZH:OpenFolder*/ "\xE6\x89\x93\xE5\xBC\x80\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9",
        /*ZH:ChooseFolder*/ "\xE9\x80\x89\xE6\x8B\xA9\xE5\xAE\x89\xE8\xA3\x85\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9",
        /*ZH:WinTitle*/ "SoundX \xE5\xAE\x89\xE8\xA3\x85\xE7\xA8\x8B\xE5\xBA\x8F",
        /*ZH:UninstallCmd*/ "echo \xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\xA0\xE9\x99\xA4 SoundX..."
    };

    inline juce::String text (int id, bool chinese, const juce::String& dir = {})
    {
        juce::String s = juce::String::fromUTF8 (chinese ? zh[id] : en[id]);
        if (dir.isNotEmpty()) s = s.replace ("@DIR", dir);
        return s;
    }
}

juce::File defaultInstallDir()
{
    const auto localAppData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                  .getParentDirectory().getChildFile ("Local");
    return localAppData.getChildFile ("Programs").getChildFile ("SoundX");
}

bool writeTextFile (const juce::File& f, const juce::String& text)
{
    f.deleteFile();
    juce::FileOutputStream out (f);
    if (! out.openedOk()) return false;
    out.writeText (text, false, false, "\r\n");
    out.flush();
    return true;
}

// Writes every embedded data file (.dll = the app-local C++ runtime, .pdf = the user manual) next
// to the executable. A file that is currently in use (SoundX is running) is left alone instead of
// failing the whole installation - the running copy already has a matching runtime loaded.
void writeEmbeddedRuntimeFiles (const juce::File& dir)
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const juce::String resourceName (BinaryData::namedResourceList[i]);
        const juce::String fileName (BinaryData::originalFilenames[i]);
        const bool isDll = fileName.endsWithIgnoreCase (".dll");
        const bool isManual = fileName.endsWithIgnoreCase (".pdf");
        if (! isDll && ! isManual) continue;

        int size = 0;
        const char* data = BinaryData::getNamedResource (resourceName.toRawUTF8(), size);
        if (data == nullptr || size <= 0) continue;

        // the manual gets a stable name in the install folder (the resource name carries the hash)
        const auto target = dir.getChildFile (isManual ? juce::String ("SoundX-Manual.pdf") : fileName);
        if (target.existsAsFile() && target.getSize() == (juce::int64) size)
            continue;                                     // already up to date

        if (isDll) target.deleteFile();                   // the runtime must be replaced as a whole
        juce::FileOutputStream out (target);
        if (! out.openedOk())
        {
            juce::Logger::writeToLog ("file is in use, kept as it is: " + target.getFullPathName());
            continue;
        }
        out.write (data, (size_t) size);
        out.flush();
    }
}

bool installTo (const InstallOptions& o, juce::String& error, std::function<void (float, const juce::String&)> report = {})
{
    const bool cn = o.chinese;
    auto reportAt = [&report] (float p, const juce::String& s) { if (report) report (p, s); };

    o.dir.createDirectory();
    if (! o.dir.isDirectory())
    {
        error = Str::text (Str::ErrCreateDir, cn) + o.dir.getFullPathName();
        return false;
    }

    reportAt (0.15f, Str::text (Str::Writing, cn));
    const auto exe = o.dir.getChildFile ("SoundX.exe");
    exe.deleteFile();
    {
        juce::FileOutputStream out (exe);
        if (! out.openedOk())
        {
            error = Str::text (Str::ErrWrite, cn) + exe.getFullPathName();
            return false;
        }
        out.write (BinaryData::SoundX_exe, (size_t) BinaryData::SoundX_exeSize);
        out.flush();
    }
    if (! exe.existsAsFile() || exe.getSize() != (juce::int64) BinaryData::SoundX_exeSize)
    {
        error = Str::text (Str::ErrWriteFail, cn);
        return false;
    }

    {   // a bad payload would only surface later as "not a valid Win32 application"
        juce::FileInputStream check (exe);
        if (check.readByte() != 'M' || check.readByte() != 'Z')
        {
            error = Str::text (Str::ErrWriteFail, cn) + exe.getFullPathName();
            exe.deleteFile();
            return false;
        }
    }

    // The program links the Microsoft C++ runtime dynamically, so the redistributable DLLs are
    // written next to SoundX.exe (app-local deployment). Without them a clean Windows install
    // fails to start SoundX with "MSVCP140.dll was not found".
    reportAt (0.35f, Str::text (Str::WritingRuntime, cn));
    writeEmbeddedRuntimeFiles (o.dir);

    reportAt (0.55f, Str::text (Str::UninstallHelper, cn));
    writeTextFile (o.dir.getChildFile ("Uninstall SoundX.cmd"),
                   Str::text (Str::UninstallCmd, cn, o.dir.getFullPathName()));

    if (o.desktopShortcut)
    {
        reportAt (0.7f, Str::text (Str::CreatingDesktop, cn));
        exe.createShortcut ("SoundX", juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                                           .getChildFile ("SoundX.lnk"));
    }
    if (o.startMenuShortcut)
    {
        reportAt (0.8f, Str::text (Str::CreatingStart, cn));
        auto programs = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("Microsoft").getChildFile ("Windows")
                            .getChildFile ("Start Menu").getChildFile ("Programs")
                            .getChildFile ("SoundX");
        programs.createDirectory();
        exe.createShortcut ("SoundX", programs.getChildFile ("SoundX.lnk"));
    }

    reportAt (1.0f, Str::text (Str::Done, cn));
    return true;
}

//==============================================================================
class SetupComponent : public juce::Component
{
public:
    SetupComponent()
    {
        chinese = juce::SystemStats::getUserLanguage().startsWithIgnoreCase ("zh");

        title.setFont (juce::FontOptions (22.0f).withStyle ("Bold"));
        addAndMakeVisible (title);

        subtitle.setColour (juce::Label::textColourId, juce::Colour (0xff5a6270));
        addAndMakeVisible (subtitle);

        pathLabel.setColour (juce::Label::textColourId, juce::Colour (0xff2a2f37));
        addAndMakeVisible (pathLabel);

        pathEditor.setMultiLine (false);
        pathEditor.setText (defaultInstallDir().getFullPathName(), false);
        addAndMakeVisible (pathEditor);

        browse.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> (Str::text (Str::ChooseFolder, chinese),
                                                           juce::File (pathEditor.getText()), "*");
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f != juce::File()) pathEditor.setText (f.getFullPathName(), false);
            });
        };
        addAndMakeVisible (browse);

        desktopBtn.setToggleState (true, juce::dontSendNotification);
        startMenuBtn.setToggleState (true, juce::dontSendNotification);
        launchBtn.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (desktopBtn);
        addAndMakeVisible (startMenuBtn);
        addAndMakeVisible (launchBtn);

        installBtn.onClick = [this] { startInstall(); };
        addAndMakeVisible (installBtn);

        closeBtn.onClick = [this] { if (auto* app = juce::JUCEApplication::getInstance()) app->systemRequestedQuit(); };
        addAndMakeVisible (closeBtn);

        status.setColour (juce::Label::textColourId, juce::Colour (0xff5a6270));
        addAndMakeVisible (status);

        langBox.addItem (Str::text (Str::LanguageName, false), 1);
        langBox.addItem (Str::text (Str::LanguageName, true), 2);
        langBox.setSelectedId (chinese ? 2 : 1, juce::dontSendNotification);
        langBox.onChange = [this]
        {
            chinese = langBox.getSelectedId() == 2;
            applyLanguage();
        };
        addAndMakeVisible (langBox);

        applyLanguage();
        setSize (660, 350);
    }

    void applyLanguage()
    {
        const juce::String versioned = Str::text (Str::Title, chinese) + " " + juce::String (JUCE_APPLICATION_VERSION_STRING);
        title.setText (versioned, juce::dontSendNotification);
        subtitle.setText (Str::text (Str::Subtitle, chinese), juce::dontSendNotification);
        pathLabel.setText (Str::text (Str::InstallFolder, chinese), juce::dontSendNotification);
        browse.setButtonText (Str::text (Str::Browse, chinese));
        desktopBtn.setButtonText (Str::text (Str::Desktop, chinese));
        startMenuBtn.setButtonText (Str::text (Str::StartMenu, chinese));
        launchBtn.setButtonText (Str::text (Str::Launch, chinese));
        installBtn.setButtonText (Str::text (Str::Install, chinese));
        closeBtn.setButtonText (Str::text (Str::Exit, chinese));
        langLabel.setText (Str::text (Str::Language, chinese), juce::dontSendNotification);
        status.setText (installed ? Str::text (Str::InstalledTo, chinese) + pathEditor.getText() : juce::String(),
                        juce::dontSendNotification);
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->setName (versioned);
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xfff7f8fa));
        if (progress > 0.0f && progress < 1.0f && ! progressRect.isEmpty())
        {
            auto r = progressRect.toFloat();
            g.setColour (juce::Colour (0xffdfe3e8));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colour (0xff2f6fd0));
            g.fillRoundedRectangle (r.withWidth (r.getWidth() * progress), 3.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18, 14);

        auto header = r.removeFromTop (30);
        auto langArea = header.removeFromRight (330);
        langLabel.setBounds (langArea.removeFromLeft (86));
        langBox.setBounds (langArea.removeFromLeft (120));
        title.setBounds (header);

        subtitle.setBounds (r.removeFromTop (20));
        r.removeFromTop (14);

        auto row = r.removeFromTop (30);
        pathLabel.setBounds (row.removeFromLeft (92));
        browse.setBounds (row.removeFromRight (92));
        row.removeFromRight (6);
        pathEditor.setBounds (row);

        r.removeFromTop (14);
        desktopBtn.setBounds (r.removeFromTop (24));
        startMenuBtn.setBounds (r.removeFromTop (24));
        launchBtn.setBounds (r.removeFromTop (24));

        progressRect = r.removeFromTop (8);
        r.removeFromTop (10);
        status.setBounds (r.removeFromTop (20));

        auto buttons = getLocalBounds().reduced (18, 14).removeFromBottom (34);
        installBtn.setBounds (buttons.removeFromRight (130));
        buttons.removeFromRight (8);
        closeBtn.setBounds (buttons.removeFromRight (100));
    }

    void startInstall()
    {
        InstallOptions o;
        o.dir = juce::File (pathEditor.getText().trim());
        if (o.dir.getFullPathName().isEmpty()) o.dir = defaultInstallDir();
        o.desktopShortcut = desktopBtn.getToggleState();
        o.startMenuShortcut = startMenuBtn.getToggleState();
        o.launchAfter = launchBtn.getToggleState();
        o.chinese = chinese;

        installBtn.setEnabled (false);
        browse.setEnabled (false);
        progress = 0.05f;
        status.setText (Str::text (Str::Preparing, chinese), juce::dontSendNotification);

        juce::String error;
        const bool ok = installTo (o, error, [this] (float p, const juce::String& s)
        {
            progress = p;
            status.setText (s, juce::dontSendNotification);
            repaint();
        });

        installBtn.setEnabled (true);
        browse.setEnabled (true);
        repaint();

        if (! ok)
        {
            progress = 0.0f;
            repaint();
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    Str::text (Str::ErrTitle, chinese), error);
            return;
        }

        progress = 1.0f;
        installed = true;
        repaint();
        status.setText (Str::text (Str::InstalledTo, chinese) + o.dir.getFullPathName(), juce::dontSendNotification);

        const auto exe = o.dir.getChildFile ("SoundX.exe");
        const auto dir = o.dir;
        const bool launch = o.launchAfter;
        const bool cn = chinese;

        auto* box = new juce::AlertWindow (Str::text (Str::OkTitle, cn),
                                           Str::text (Str::OkBody, cn, dir.getFullPathName()),
                                           juce::MessageBoxIconType::InfoIcon);
        box->addButton (Str::text (Str::OK, cn), 1);
        box->addButton (Str::text (Str::OpenFolder, cn), 2);
        box->enterModalState (true, juce::ModalCallbackFunction::create ([exe, dir, launch] (int res)
        {
            if (res == 2) dir.revealToUser();
            else if (launch) exe.startAsProcess();
        }), true);
    }

    std::unique_ptr<juce::FileChooser> chooser;

private:
    bool chinese = false;
    bool installed = false;
    float progress = 0.0f;
    juce::Rectangle<int> progressRect;

    juce::Label title, subtitle, pathLabel, status, langLabel;
    juce::TextEditor pathEditor;
    juce::TextButton browse, installBtn, closeBtn;
    juce::ToggleButton desktopBtn, startMenuBtn, launchBtn;
    juce::ComboBox langBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SetupComponent)
};

//==============================================================================
class SetupWindow : public juce::DocumentWindow
{
public:
    SetupWindow()
        : juce::DocumentWindow ("SoundX Setup", juce::Colour (0xfff7f8fa), juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new SetupComponent(), true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (auto* app = juce::JUCEApplication::getInstance()) app->systemRequestedQuit();
    }
};

class SetupApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "SoundX Setup"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

        const auto args = getCommandLineParameterArray();
        const int silentIdx = args.indexOf ("--silent");
        if (silentIdx >= 0)
        {
            // unattended install: --silent <folder> [--zh]
            InstallOptions o;
            o.dir = (silentIdx + 1 < args.size()) ? juce::File (args[silentIdx + 1]) : defaultInstallDir();
            o.launchAfter = false;
            o.chinese = args.contains ("--zh");
            juce::String error;
            const bool ok = installTo (o, error);
            if (! ok) juce::Logger::writeToLog ("install failed: " + error);
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        window = std::make_unique<SetupWindow>();
    }

    void shutdown() override
    {
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<SetupWindow> window;
    juce::LookAndFeel_V4 lnf { juce::LookAndFeel_V4::getLightColourScheme() };
};
}

START_JUCE_APPLICATION (SetupApplication)
