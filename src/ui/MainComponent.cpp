#include "MainComponent.h"
#include "Importer.h"
#include "Dialogs.h"
#include "MidiLearn.h"
#include "SlotMenus.h"
#include "PluginWindows.h"
#include "Localisation.h"
#include "VirtualKeyboard.h"
#if SOUNDX_HAS_MANUAL
 #include "BinaryData.h"
#endif

#if JUCE_WINDOWS
 // imm32 is linked by juce_gui_basics; declaring it here avoids pulling in <windows.h>
 extern "C" __declspec (dllimport) int __stdcall ImmAssociateContextEx (void* hWnd, void* hIMC, unsigned long dwFlags);
#endif

namespace sx
{
using namespace juce;

class MainComponent::Resizer : public Component
{
public:
    explicit Resizer (MainComponent& o) : owner (o) { setMouseCursor (MouseCursor::UpDownResizeCursor); }
    void paint (Graphics& g) override
    {
        g.fillAll (theme::line);
        g.setColour (theme::textFaint);
        g.fillRoundedRectangle ((float) getWidth() * 0.5f - 18.0f, (float) getHeight() * 0.5f - 1.0f, 36.0f, 2.0f, 1.0f);
    }
    void mouseDown (const MouseEvent&) override { startH = owner.bottomHeight; }
    void mouseDrag (const MouseEvent& e) override
    {
        owner.bottomHeight = jlimit (140, jmax (160, owner.getHeight() - 260), startH - e.getDistanceFromDragStartY());
        owner.resized();
    }
private:
    MainComponent& owner;
    int startH = 300;
};

namespace
{
    const int kTrackHeights[] = { 46, 62, 78, 104, 150 };                     // XS, S, M, L, XL
    const char* kTrackHeightNames[] = { "Extra Small", "Small", "Medium", "Large", "Extra Large" };
}

MainComponent::MainComponent (AppState& a)
    : app (a), controlBar (a), inspector (a), arrange (a), mediaPool (a), mixer (a), pianoRoll (a)
{
    setWantsKeyboardFocus (true);
    menuBar.setModel (this);
    addAndMakeVisible (menuBar);
    addAndMakeVisible (controlBar);
    addAndMakeVisible (inspector);
    addAndMakeVisible (arrange);
    addChildComponent (mediaPool);
    mediaPool.onClose = [this]
    {
        mediaPoolVisible = false;
        resized();
        repaint();
    };
    addAndMakeVisible (mixer);
    addChildComponent (pianoRoll);
    resizer = std::make_unique<Resizer> (*this);
    addAndMakeVisible (*resizer);
    addAndMakeVisible (mixerTab);
    addAndMakeVisible (pianoTab);
    addAndMakeVisible (closeBottom);

    for (auto* b : { &mixerTab, &pianoTab })
    {
        b->setClickingTogglesState (false);
        b->setColour (TextButton::buttonColourId, Colours::transparentBlack);
    }
    mixerTab.setTooltip (TRANS ("Open the mixer panel"));
    pianoTab.setTooltip (TRANS ("Open the piano roll panel"));
    mixerTab.onClick = [this] { setBottom (Bottom::Mixer); };
    pianoTab.onClick = [this] { setBottom (Bottom::PianoRoll); };
    closeBottom.onClick = [this] { setBottom (Bottom::None); };
    pianoRoll.onFullscreenToggle = [this] { toggleFullscreen(); };

    controlBar.onCommand = [this] (int c) { perform (c); };

    // fullscreen clip gain envelope editor (opened by double-clicking an audio clip)
    gainEditor = std::make_unique<ClipGainEditor> (app);
    addChildComponent (*gainEditor);
    gainEditor->onClose = [this] { resized(); };
    arrange.onEditClipEnvelope = [this] (int trackId, int clipId) { openClipGainEditor (trackId, clipId); };

    // autosave history dialog
    autosaveDialog = std::make_unique<AutosaveDialog> (app);
    addChildComponent (*autosaveDialog);
    autosaveDialog->onClose = [this] { resized(); repaint(); };
    autosaveDialog->onDiscard = [this]
    {
        // drop the whole snapshot set
        for (auto& f : autosaveFilesToDiscard) f.deleteFile();
        autosaveFilesToDiscard.clear();
        resized();
        repaint();
    };
    autosaveDialog->onRecover = [this] (const juce::File& snapshot)
    {        if (! app.project.loadFrom (snapshot))
        {
            showError (TRANS ("Could not load the autosave."));
            return;
        }
        // a recovered project has no home yet: never overwrite the original by accident
        app.project.file = juce::File();
        app.project.dirty = true;
        app.engine.syncAll();
        app.editTrack = -1;
        app.editClip = -1;
        app.selectedTrack = app.project.tracks.empty() ? -1 : app.project.tracks.front()->id;
        app.selectedTracks.clear();
        if (app.selectedTrack >= 0) app.selectedTracks.insert (app.selectedTrack);
        app.notifySelection();
        app.notifyView();
        updateTitle();
        resized();
        repaint();

        const auto options = MessageBoxOptions::makeOptionsOkCancel (MessageBoxIconType::QuestionIcon,
                                                                     TRANS ("Recovered autosave"),
                                                                     TRANS ("The recovered project has not been saved yet. Save it now?"),
                                                                     TRANS ("Save As..."), TRANS ("Later"), this);
        AlertWindow::showAsync (options, [this] (int r) { if (r == 1) saveProject (true); });
    };

    // The plug-in panels live inside the main window: a transparent layer that only lets its
    // child panels take clicks (everything else falls through to the arrangement). This is what
    // keeps a single taskbar entry and lets Space reach the transport while a plug-in has focus.
    pluginHost.setInterceptsMouseClicks (false, true);
    pluginHost.setOpaque (false);
    addAndMakeVisible (pluginHost);
    if (app.windows != nullptr)
    {
        app.windows->setHost (&pluginHost);
        app.windows->onGlobalKey = [this] (const juce::KeyPress& k) { return handlePluginShortcut (k); };
    }
    app.plugins.setPanelHost (&pluginHost);

    // The on-screen piano of the computer-keyboard performance mode is its own floating panel on
    // top of the whole UI. It takes no keyboard focus (typing must keep reaching this component)
    // and can be dragged anywhere by its title bar.
    virtualKeyboard = std::make_unique<VirtualKeyboard> (app);
    virtualKeyboard->isNoteActive = [this] (int note)
    {
        for (auto& [key, n] : typingNotes) { juce::ignoreUnused (key); if (n == note) return true; }
        return false;
    };
    virtualKeyboard->onClose = [this] { setMusicalTyping (false); };
    virtualKeyboard->onResetPosition = [this] { placeKeyboardWindow (true); };
    virtualKeyboard->onMoved = [this] (Point<int> p) { app.keyboardX = p.x; app.keyboardY = p.y; };
    addChildComponent (*virtualKeyboard);

    app.addListener (this);
    app.project.addListener (this);
    addMouseListener (this, true);
    setBottom (Bottom::Mixer);
    app.meters.start();
    startTimerHz (10);
    setSize (1440, 880);
}

MainComponent::~MainComponent()
{
    // When the theme rebuilds the UI, the new MainComponent is constructed before the old one is
    // deleted. Only the component that still owns the shared host may clear it; otherwise the
    // just-installed new host would be replaced by the fallback and every plug-in panel would
    // silently disappear (the "plug-in window does not open" bug).
    if (app.windows != nullptr && app.windows->hostComponent() == &pluginHost)
        app.windows->setHost (nullptr);
    if (app.plugins.panelHostComponent() == &pluginHost)
        app.plugins.setPanelHost (nullptr);
    app.meters.stop();
    menuBar.setModel (nullptr);
    releaseAllTypingNotes();
    app.removeListener (this);
    app.project.removeListener (this);
}

String MainComponent::windowTitle() const
{
    auto name = app.project.file.existsAsFile() ? app.project.file.getFileNameWithoutExtension() : TRANS ("Untitled");
    // the title typed into Project Info is worth showing next to the file name
    const auto title = app.project.meta.title;
    if (title.isNotEmpty() && title != name) name = name + " - " + title;
    return "SoundX - " + name + (app.project.dirty ? " *" : "");
}

void MainComponent::updateTitle()
{
    auto t = windowTitle();
    if (t != lastTitle)
    {
        lastTitle = t;
        if (onTitleChanged) onTitleChanged (t);
    }
}

void MainComponent::timerCallback()
{
    updateTitle();
    syncVirtualKeyboard();

    // message-thread responsiveness diagnostic (off unless SOUNDX_UI_PROFILE points at a file)
    if (auto path = juce::SystemStats::getEnvironmentVariable ("SOUNDX_UI_PROFILE", {}); path.isNotEmpty())
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (lastUiTickMs > 0.0)
        {
            const double d = now - lastUiTickMs;
            uiProfileSum += d;
            uiProfileMax = jmax (uiProfileMax, d);
            if (d > 150.0) ++uiStalls;       // the 10 Hz timer should tick every ~100 ms
        }
        lastUiTickMs = now;
        if (++uiProfileCount >= 50)
        {
            juce::File (path).appendText ("ui profile: avg " + String (uiProfileSum / uiProfileCount, 1)
                                          + " ms, max " + String (uiProfileMax, 1)
                                          + " ms, stalls(>150ms) " + String (uiStalls) + "\n");
            uiProfileCount = 0;
            uiProfileSum = uiProfileMax = 0.0;
            uiStalls = 0;
        }
    }

    controlBar.inspectorBtn.setToggleState (inspectorVisible, dontSendNotification);
    controlBar.mixerBtn.setToggleState (bottom == Bottom::Mixer, dontSendNotification);
    controlBar.editorBtn.setToggleState (bottom == Bottom::PianoRoll, dontSendNotification);
    controlBar.mediaPoolBtn.setToggleState (mediaPoolVisible, dontSendNotification);

    // autosave: every 10 minutes, into the cache folder (same place as the temporary takes)
    if (lastAutosaveMs == 0) lastAutosaveMs = juce::Time::getMillisecondCounter();
    else if (juce::Time::getMillisecondCounter() - lastAutosaveMs >= 10 * 60 * 1000)
    {
        lastAutosaveMs = juce::Time::getMillisecondCounter();
        doAutosave();
    }

    // keyboard self-heal: if this window is the active one but no component inside it holds
    // the keyboard focus (e.g. after a theme rebuild or when a menu has just closed), take it
    // back so the shortcuts always work.
    //
    // Important: never take the focus back while a plug-in editor panel is open. Plug-ins create
    // their popup menus / combo dropdowns as native windows; JUCE then reports "no focused
    // component" and grabbing the keyboard focus here made the host steal it back on the next
    // 30 Hz tick, which dismissed the popup again - the plug-in menus "flashed but could not be
    // selected". While a panel is open its own key listener already forwards the host shortcuts
    // (see PluginWindows::keyPressed), so nothing is lost by skipping the heal.
    const bool pluginPanelOpen = app.windows != nullptr && app.windows->hasVisibleWindow();
    if (! pluginPanelOpen)
        if (auto* top = dynamic_cast<juce::TopLevelWindow*> (getTopLevelComponent()))
            if (top->isActiveWindow() && juce::Component::getCurrentlyFocusedComponent() == nullptr)
                grabKeyboardFocus();

   #if JUCE_WINDOWS
    // A Chinese IME swallows plain letter keys (and Ctrl combinations) while no text field is
    // being edited, which silently kills every keyboard shortcut. Detach the IME from our window
    // in that case - JUCE re-associates it automatically when a text editor gains focus.
    if (auto* peer = getPeer())
    {
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        if (focused == nullptr || dynamic_cast<juce::TextEditor*> (focused) == nullptr)
            ImmAssociateContextEx (peer->getNativeHandle(), nullptr, 0);
    }
   #endif
}

void MainComponent::refreshHostLayer()
{
    if (app.windows != nullptr) app.windows->setHost (&pluginHost);
    app.plugins.setPanelHost (&pluginHost);
}

void MainComponent::paint (Graphics& g)
{
    syncVirtualKeyboard();
    g.fillAll (theme::window);

    // always-visible dock strip at the bottom edge
    auto strip = getLocalBounds().removeFromBottom (kTabStripH);
    g.setColour (theme::panel);
    g.fillRect (strip);
    g.setColour (theme::line);
    g.fillRect (strip.getX(), strip.getY(), strip.getWidth(), 1);

    if (bottom == Bottom::None)
    {
        // hint so a collapsed editor stays easy to find again
        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        g.drawText (TRANS ("Click a tab to open the editor"), strip.reduced (196, 0), Justification::centredLeft);
    }
}

void MainComponent::openClipGainEditor (int trackId, int clipId)
{
    if (gainEditor == nullptr) return;
    // size the editor before opening it, so its very first paint already uses the
    // final bounds (a paint at a stale size would leave leftovers in the corner)
    gainEditor->setBounds (getLocalBounds());
    gainEditor->open (trackId, clipId);
    resized();
    repaint();
}

void MainComponent::syncGainEditor()
{
    // undo / delete can remove the clip the fullscreen editor is pointing at: close
    // the editor instead of leaving it behind on a dead clip (it would draw "no audio
    // clip" over a stale view)
    if (gainEditor == nullptr || ! gainEditor->isOpen() || gainEditor->clipExists()) return;
    gainEditor->close();
    resized();
    repaint();
}

void MainComponent::syncVirtualKeyboard()
{
    if (virtualKeyboard == nullptr) return;

    if (app.musicalTyping)
    {
        if (! virtualKeyboard->isVisible())
        {
            placeKeyboardWindow (false);
            virtualKeyboard->setVisible (true);
            virtualKeyboard->toFront (false);
        }
    }
    else if (virtualKeyboard->isVisible())
    {
        virtualKeyboard->setVisible (false);
    }
}

void MainComponent::setMusicalTyping (bool on)
{
    const bool wasOn = app.musicalTyping;
    if (wasOn != on)
    {
        app.musicalTyping = on;
        if (! on) releaseAllTypingNotes();
    }

    if (on) placeKeyboardWindow (false);
    syncVirtualKeyboard();

    if (wasOn != on)
    {
        app.project.sendMixChanged();   // the toolbar button follows app.musicalTyping
        repaint();
    }
}

void MainComponent::placeKeyboardWindow (bool resetToDefault)
{
    if (virtualKeyboard == nullptr) return;
    // leave the dock strip free: the tabs at the bottom are always reachable
    auto area = getLocalBounds().withTrimmedBottom (kTabStripH).reduced (10);
    if (area.getWidth() < 200 || area.getHeight() < 100) return;
    virtualKeyboard->layoutIn (area, resetToDefault);
}

void MainComponent::refreshTheme()
{
    // Only the palette changed: the layout, the open panels and every scroll position stay as
    // they are. sendLookAndFeelChange() re-applies the JUCE-drawn controls, the explicit tree
    // repaint covers everything this app paints from the theme:: values at paint time.
    if (auto* lf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel()))
        lf->applyColours();

    sendLookAndFeelChange();

    std::function<void (Component&)> repaintTree = [&] (Component& c)
    {
        c.repaint();
        for (auto* child : c.getChildren()) repaintTree (*child);
    };
    repaintTree (*this);

    controlBar.refreshTheme();
    inspector.refreshTheme();
    mediaPool.refreshTheme();
    mixer.refreshTheme();
    if (gainEditor != nullptr) gainEditor->refreshTheme();
    if (virtualKeyboard != nullptr) virtualKeyboard->refreshTheme();
    app.plugins.refreshTheme();
    app.windows->refreshAll();

    // the dock tabs carry an explicit colour that depends on which panel is active
    refreshBottomTabColours();
    repaint();
}

void MainComponent::resized()
{
    // the clip gain envelope editor covers the whole window while it is open
    if (autosaveDialog != nullptr)
    {
        if (autosaveDialog->isOpen())
        {
            autosaveDialog->setBounds (getLocalBounds());
            autosaveDialog->toFront (true);
        }
        else
            autosaveDialog->setVisible (false);
    }

    if (gainEditor != nullptr)
    {
        if (gainEditor->isOpen())
        {
            gainEditor->setBounds (getLocalBounds());
            gainEditor->toFront (true);
        }
        else
            gainEditor->setVisible (false);
    }

    auto r = getLocalBounds();
    menuBar.setBounds (r.removeFromTop (26));
    controlBar.setBounds (r.removeFromTop (86));   // transport row + the arrange tools / snap row

    // the dock strip keeps the Mixer / Piano Roll tabs on screen even while the panel is closed,
    // so both editors can always be pulled back up from here
    auto strip = r.removeFromBottom (kTabStripH);
    auto stripInner = strip.reduced (6, 0);
    mixerTab.setBounds (stripInner.removeFromLeft (84).reduced (4, 3));
    pianoTab.setBounds (stripInner.removeFromLeft (96).reduced (4, 3));
    closeBottom.setBounds (stripInner.removeFromRight (28).reduced (3));
    mixerTab.setVisible (true);
    pianoTab.setVisible (true);
    closeBottom.setVisible (bottom != Bottom::None);

    const bool full = bottom == Bottom::PianoRoll && bottomFull;

    if (full)
    {
        // true fullscreen: the piano roll covers the whole window (menu bar, transport bar,
        // dock strip and every other panel step aside); exit with its fullscreen button or F11
        menuBar.setVisible (false);
        controlBar.setVisible (false);
        mixerTab.setVisible (false);
        pianoTab.setVisible (false);
        closeBottom.setVisible (false);
        inspector.setVisible (false);
        resizer->setVisible (false);
        mixer.setVisible (false);
        pianoRoll.setVisible (true);
        pianoRoll.setBounds (getLocalBounds());
    }
    else
    {
        menuBar.setVisible (true);
        controlBar.setVisible (true);
        if (inspectorVisible)
        {
            inspector.setVisible (true);
            inspector.setBounds (r.removeFromLeft (232));
        }
        else
            inspector.setVisible (false);

        if (bottom != Bottom::None)
        {
            auto bottomArea = r.removeFromBottom (jmin (bottomHeight, jmax (140, r.getHeight() - 220)));
            resizer->setVisible (true);
            resizer->setBounds (bottomArea.getX(), bottomArea.getY() - 3, bottomArea.getWidth(), 5);
            mixer.setBounds (bottomArea);
            pianoRoll.setBounds (bottomArea);
            mixer.setVisible (bottom == Bottom::Mixer);
            pianoRoll.setVisible (bottom == Bottom::PianoRoll);
        }
        else
        {
            resizer->setVisible (false);
            mixer.setVisible (false);
            pianoRoll.setVisible (false);
        }
    }
    // media pool on the right of the arrangement area
    if (mediaPoolVisible)
    {
        const int poolW = jlimit (190, 320, r.getWidth() / 5);
        mediaPool.setVisible (true);
        mediaPool.setBounds (r.removeFromRight (poolW));
    }
    else
        mediaPool.setVisible (false);

    arrange.setBounds (r);
    pluginHost.setBounds (getLocalBounds());
    if (app.windows != nullptr) app.windows->hostResized();

    syncVirtualKeyboard();
    if (virtualKeyboard != nullptr && virtualKeyboard->isVisible())
    {
        placeKeyboardWindow (false);
        virtualKeyboard->toFront (false);   // stay above the fullscreen gain editor / autosave dialog
    }

    // layout diagnostic (SOUNDX_TRACE=<file>): helps debugging window-size dependent clipping
    if (auto path = juce::SystemStats::getEnvironmentVariable ("SOUNDX_TRACE", {}); path.isNotEmpty())
        juce::File (path).appendText ("layout: main " + getLocalBounds().toString()
                                      + " topLevel " + getTopLevelComponent()->getBounds().toString()
                                      + " screenPos " + juce::String (getScreenPosition().x) + ","
                                      + juce::String (getScreenPosition().y)
                                      + " blend " + juce::String (getTopLevelComponent()->getDesktopScaleFactor())
                                      + " backend " + juce::String (getTopLevelComponent()->getPeer() != nullptr
                                            ? getTopLevelComponent()->getPeer()->getCurrentRenderingEngine() : -1)
                                      + " bottomHeight " + juce::String (bottomHeight)
                                      + " arrange " + arrange.getBounds().toString()
                                      + " mixer " + mixer.getBounds().toString()
                                      + " mixerScreen " + mixer.getScreenBounds().toString()
                                      + " mixerVisible " + juce::String ((int) mixer.isVisible())
                                      + " tabs " + mixerTab.getBounds().toString() + "\n");
    repaint();
}

void MainComponent::refreshBottomTabColours()
{
    mixerTab.setColour (TextButton::textColourOffId,
                        bottom == Bottom::Mixer ? theme::accentDark : theme::textDim);
    pianoTab.setColour (TextButton::textColourOffId,
                        bottom == Bottom::PianoRoll ? theme::accentDark : theme::textDim);
}

void MainComponent::setBottom (Bottom b)
{
    bottom = b;
    app.engine.setPianoRollAuditioning (b == Bottom::PianoRoll);
    if (b != Bottom::PianoRoll) bottomFull = false;
    pianoRoll.setFullscreen (bottomFull);
    if (b == Bottom::PianoRoll) bottomHeight = jmax (bottomHeight, jmin (440, getHeight() / 2 + 40));
    refreshBottomTabColours();
    resized();
}

void MainComponent::toggleFullscreen()
{
    if (bottom != Bottom::PianoRoll)
    {
        bottomFull = true;
        setBottom (Bottom::PianoRoll);
    }
    else
    {
        bottomFull = ! bottomFull;
        pianoRoll.setFullscreen (bottomFull);
        resized();
    }
}

void MainComponent::panelRequested (const String& name)
{
    if (name == "pianoroll") setBottom (Bottom::PianoRoll);
}

void MainComponent::mouseDown (const MouseEvent& e)
{
    if (dynamic_cast<TextEditor*> (e.originalComponent) == nullptr)
        grabKeyboardFocus();

    // a click on the floating keyboard is not a click into the piano roll below it
    const bool onKeyboard = virtualKeyboard != nullptr
                            && (e.originalComponent == virtualKeyboard.get()
                                || (e.originalComponent != nullptr && virtualKeyboard->isParentOf (e.originalComponent)));
    lastClickInPiano = ! onKeyboard && pianoRoll.isVisible()
                       && pianoRoll.getScreenBounds().contains (e.getScreenPosition());
}

//==============================================================================
StringArray MainComponent::getMenuBarNames()
{
    return { TRANS ("File"), TRANS ("Edit"), TRANS ("Track"), TRANS ("Transport"), TRANS ("View"), TRANS ("Help") };
}

PopupMenu MainComponent::getMenuForIndex (int index, const String&)
{
    PopupMenu m;
    auto& p = app.project;
    switch (index)
    {
        case 0:
            m.addItem (CmdNewProject, TRANS ("New Project"), true, false);
            {
                const bool haveTemplate = PluginManager::templateFile().existsAsFile();
                m.addItem (CmdNewFromTemplate, TRANS ("New Project from Template"), haveTemplate);
                m.addItem (CmdSaveAsTemplate, TRANS ("Save as Project Template..."));
                m.addItem (CmdDeleteTemplate, TRANS ("Delete Project Template"), haveTemplate);
            }
            m.addItem (CmdOpenProject, TRANS ("Open..."));
            m.addItem (CmdSaveProject, TRANS ("Save"));
            m.addItem (CmdSaveProjectAs, TRANS ("Save As..."));
            m.addItem (CmdProjectInfo, TRANS ("Project Info..."));
            m.addSeparator();
            m.addItem (CmdImportMidi, TRANS ("Import MIDI File..."));
            m.addItem (CmdImportAudio, TRANS ("Import Audio File..."));
            m.addSeparator();
            m.addItem (CmdExportMidi, TRANS ("Export MIDI File..."));
            m.addItem (CmdExportMix, TRANS ("Export Mixdown (WAV)..."));
            m.addItem (CmdExportStems, TRANS ("Export Stems (WAV per track)..."));
            m.addSeparator();
            m.addItem (CmdSetRecordingFolder, TRANS ("Cache Folder..."));
            m.addSeparator();
            m.addItem (CmdQuit, TRANS ("Quit"));
            break;
        case 1:
        {
            PopupMenu::Item undo (TRANS ("Undo")); undo.itemID = CmdUndo; undo.isEnabled = p.canUndo(); undo.shortcutKeyDescription = "Ctrl+Z";
            PopupMenu::Item redo (TRANS ("Redo")); redo.itemID = CmdRedo; redo.isEnabled = p.canRedo(); redo.shortcutKeyDescription = "Ctrl+Y";
            m.addItem (undo); m.addItem (redo);
            m.addSeparator();
            auto add = [&m] (int id, const String& t, const String& sc, bool enabled = true)
            {
                PopupMenu::Item it (t); it.itemID = id; it.shortcutKeyDescription = sc; it.isEnabled = enabled; m.addItem (it);
            };
            add (CmdCut, TRANS ("Cut"), "Ctrl+X");
            add (CmdCopy, TRANS ("Copy"), "Ctrl+C");
            add (CmdPaste, TRANS ("Paste"), "Ctrl+V");
            add (CmdDelete, TRANS ("Delete"), "Del");
            add (CmdDuplicate, TRANS ("Duplicate"), "Ctrl+D");
            add (CmdSelectAll, TRANS ("Select All"), "Ctrl+A");
            m.addSeparator();
            add (CmdSplit, TRANS ("Split at Playhead"), "Alt+X");
            add (CmdSplitAtCursor, TRANS ("Split at Cursor"), "S", ! app.selectedClips.empty());
            add (CmdAddCrossfade, TRANS ("Add Crossfade"), "X", ! app.selectedClips.empty());
            add (CmdFadeInToPlayhead, TRANS ("Fade In to Playhead"), "", ! app.selectedClips.empty());
            add (CmdFadeOutFromPlayhead, TRANS ("Fade Out from Playhead"), "", ! app.selectedClips.empty());
            add (CmdClearFades, TRANS ("Clear Fades"), "", ! app.selectedClips.empty());
            add (CmdQuantize, TRANS ("Quantize Notes"), "Q");
            add (CmdReverseNotes, TRANS ("Reverse Selected Notes"), "");
            {
                // Humanize works on the notes of the editor: it is offered when the piano roll has
                // an editable clip, right next to Quantize in the same menu
                const bool notes = pianoRoll.isVisible() && pianoRoll.clip() != nullptr;
                add (CmdHumanize, TRANS ("Humanize Velocity"), "H", notes);
                add (CmdHumanizeDialog, TRANS ("Humanize Velocity..."), "Shift+H", notes);
            }
            {
                const bool anyAudio = [this]
                {
                    for (auto& t : app.project.tracks)
                        for (auto& c : t->audioClips)
                            if (app.selectedClips.count (c.id) > 0) return true;
                    return false;
                }();
                PopupMenu audio;
                audio.addItem (CmdReverseAudio, TRANS ("Reverse Audio"), anyAudio);
                audio.addItem (CmdAudioResetEdits, TRANS ("Reset Reverse / Speed / Transpose"), anyAudio);
                audio.addSeparator();
                PopupMenu stretch;
                for (size_t i = 0; i < audioStretchPresets().size(); ++i)
                    stretch.addItem (CmdAudioStretchPreset + (int) i, audioStretchPresetNames()[i]);
                stretch.addSeparator();
                stretch.addItem (CmdAudioStretchPrompt, TRANS ("Enter Speed Factor..."));
                audio.addSubMenu (TRANS ("Time Stretch (Speed)"), stretch, anyAudio);
                PopupMenu pitch;
                for (size_t i = 0; i < audioPitchPresets().size(); ++i)
                    pitch.addItem (CmdAudioPitchPreset + (int) i, TRANS ("Semitones") + ": " + audioPitchPresetNames()[i]);
                pitch.addSeparator();
                pitch.addItem (CmdAudioPitchPrompt, TRANS ("Enter Semitones..."));
                audio.addSubMenu (TRANS ("Transpose"), pitch, anyAudio);
                m.addSubMenu (TRANS ("Audio"), audio, anyAudio);
            }
            add (CmdSetLoopToSelection, TRANS ("Set Cycle to Selection"), "");
            add (CmdMergeClips, TRANS ("Merge Selected Clips"), "Ctrl+J", app.selectedClips.size() >= 2);
            m.addSeparator();
            add (CmdChordEnter, TRANS ("Enter Chord at Playhead..."), "");
            add (CmdMarkerEnter, TRANS ("Add Section Marker at Playhead..."), "");
            break;
        }
        case 2:
            m.addItem (CmdNewInstrumentTrack, TRANS ("New Software Instrument Track"));
            m.addItem (CmdNewAudioTrack, TRANS ("New Audio Track"));
            m.addItem (CmdNewBusTrack, TRANS ("New Bus Track"));
            m.addItem (CmdNewSendTrack, TRANS ("New Effect Send Track"));
            m.addItem (CmdGroupIntoBus, TRANS ("Group Selected Tracks into Bus"), ! app.selectedTracks.empty());
            m.addItem (CmdGroupIntoFolder, TRANS ("Create Folder from Selected Tracks"), ! app.selectedTracks.empty());
            {
                bool anyFolder = false;
                for (int id : app.selectedTracks)
                    if (auto* t = app.project.findTrack (id); t != nullptr && t->isFolder) anyFolder = true;
                m.addItem (CmdUnpackFolder, TRANS ("Unpack Folder"), anyFolder);
            }
            m.addItem (CmdAddAutomationTrack, TRANS ("Add Automation Track..."), app.selectedTrack >= 0);
            {
                // the Track menu displays its shortcuts like Edit / Transport: else Ctrl+H is
                // undiscoverable (a plain H is Humanize while the piano roll has a clip). A hide
                // command also works on a clip selection, which fills only selectedClips.
                PopupMenu::Item hide (TRANS ("Hide Selected Tracks"));
                hide.itemID = CmdHideTracks;
                hide.shortcutKeyDescription = "Ctrl+H";
                hide.isEnabled = ! app.selectedTracks.empty() || app.selectedTrack >= 0
                                 || ! app.selectedClipTrackIds().empty();
                PopupMenu::Item show (TRANS ("Show All Hidden Tracks") + " (" + String (app.hiddenTrackCount()) + ")");
                show.itemID = CmdShowAllTracks;
                show.shortcutKeyDescription = "Ctrl+Shift+H";
                show.isEnabled = app.hiddenTrackCount() > 0;
                m.addItem (hide);
                m.addItem (show);
            }
            m.addItem (CmdAddTracks, TRANS ("Add Multiple Tracks..."));
            m.addItem (CmdToggleMasterTrack, TRANS ("Master Bus Track"), true, app.showMasterTrack);
            m.addItem (CmdToggleChordTrack, TRANS ("Chord Track"), true, app.showChordTrack);
            m.addItem (CmdToggleMarkerTrack, TRANS ("Marker Track (Sections)"), true, app.showMarkerTrack);
            m.addItem (CmdAddListenBus, TRANS ("Add Listen Bus"), app.project.listenBus() == nullptr);
            m.addItem (CmdRemoveListenBus, TRANS ("Remove Listen Bus"), app.project.listenBus() != nullptr);
            m.addSeparator();
            m.addItem (CmdDuplicateTrack, TRANS ("Duplicate Track"), ! app.selectedTracks.empty());
            m.addItem (CmdDeleteTrack, TRANS ("Delete Track"), ! app.selectedTracks.empty());
            break;
        case 3:
        {
            auto add = [&m] (int id, const String& t, const String& sc, bool tick = false)
            {
                PopupMenu::Item it (t); it.itemID = id; it.shortcutKeyDescription = sc; it.isTicked = tick; m.addItem (it);
            };
            add (CmdPlayStop, TRANS ("Play / Pause"), "Space");
            add (CmdStop, TRANS ("Stop"), "0");
            add (CmdRecord, TRANS ("Record"), "R");
            add (CmdRewind, TRANS ("Return to Start"), "Enter");
            m.addSeparator();
            add (CmdToggleLoop, TRANS ("Cycle"), "C", p.loopOn);
            add (CmdToggleMetronome, TRANS ("Metronome"), "K", p.metronome);
            {
                // count-in: how many bars the transport counts before recording starts
                PopupMenu ci;
                ci.addItem (CmdCountInOff, TRANS ("Off"), true, p.countInBars <= 0);
                ci.addItem (CmdCountIn1Bar, TRANS ("1 bar"), true, p.countInBars == 1);
                ci.addItem (CmdCountIn2Bars, TRANS ("2 bars"), true, p.countInBars == 2);
                ci.addItem (CmdCountIn4Bars, TRANS ("4 bars"), true, p.countInBars == 4);
                ci.addItem (CmdCountIn8Bars, TRANS ("8 bars"), true, p.countInBars == 8);
                m.addSubMenu (TRANS ("Count-in"), ci);
            }
            {
                // what the notes of a MIDI take are snapped to as they are recorded
                PopupMenu rq;
                auto tick = [&] (double v) { return std::abs (app.project.recordQuantize - v) < 1.0e-9; };
                rq.addItem (CmdRecQuantOff, TRANS ("Off (keep as played)"), true, tick (0.0));
                rq.addItem (CmdRecQuant4, "1/4", true, tick (1.0));
                rq.addItem (CmdRecQuant8, "1/8", true, tick (0.5));
                rq.addItem (CmdRecQuant8T, TRANS ("1/8 Triplet"), true, tick (1.0 / 3.0));
                rq.addItem (CmdRecQuant16, "1/16", true, tick (0.25));
                rq.addItem (CmdRecQuant16T, TRANS ("1/16 Triplet"), true, tick (1.0 / 6.0));
                rq.addItem (CmdRecQuant32, "1/32", true, tick (0.125));
                m.addSubMenu (TRANS ("Record Quantize"), rq);
            }
            add (CmdToggleTakeRecording, TRANS ("Keep Re-recorded Passes as Takes"), "", app.project.takeRecording);
            break;
        }
        case 3 + 1:
        {
            auto add = [&m] (int id, const String& t, const String& sc, bool tick = false)
            {
                PopupMenu::Item it (t); it.itemID = id; it.shortcutKeyDescription = sc; it.isTicked = tick; m.addItem (it);
            };
            add (CmdToggleInspector, TRANS ("Inspector"), "I", inspectorVisible);
            add (CmdToggleMediaPool, TRANS ("Media Pool"), "", mediaPoolVisible);
            add (CmdToggleMixer, TRANS ("Mixer"), "X", bottom == Bottom::Mixer);
            add (CmdTogglePianoRoll, TRANS ("Piano Roll"), "P", bottom == Bottom::PianoRoll);
            add (CmdPianoRollFull, TRANS ("Fullscreen Piano Roll"), "F11", bottom == Bottom::PianoRoll && bottomFull);
            add (CmdToggleTempoTrack, TRANS ("Tempo Track"), "T", app.showTempoLane);
            add (CmdToggleMasterTrack, TRANS ("Master Bus Track"), "", app.showMasterTrack);
            add (CmdToggleChordTrack, TRANS ("Chord Track"), "", app.showChordTrack);
            add (CmdToggleMarkerTrack, TRANS ("Marker Track (Sections)"), "", app.showMarkerTrack);
            add (CmdTogglePianoRollChords, TRANS ("Chords in Piano Roll"), "", app.pianoRollChords);
            {
                // what the rulers and the transport readout are measured in
                PopupMenu tb;
                tb.addItem (CmdTimebaseBars, TRANS ("Bars"), true, ! app.timeInSeconds);
                tb.addItem (CmdTimebaseSeconds, TRANS ("Seconds"), true, app.timeInSeconds);
                m.addSubMenu (TRANS ("Timebase"), tb);
            }
            m.addSeparator();
            add (CmdToggleAppFullscreen, TRANS ("Fullscreen"), "Alt+Enter");
            add (CmdToggleDarkMode, TRANS ("Dark Mode"), "", theme::isDark());
            {
                // opening a plug-in panel right after inserting it, and whether several panels
                // may stay on screen at once (the picker offers the second switch as well)
                PopupMenu panels;
                panels.addItem (CmdTogglePluginEditorOnInsert, TRANS ("Open Panel on Insert"), true,
                                app.openPluginEditorOnInsert);
                panels.addItem (CmdToggleMultiPluginWindows, TRANS ("Keep Multiple Panels Open"), true,
                                app.multiPluginWindows);
                m.addSubMenu (TRANS ("Plug-in Panels"), panels);
            }
            m.addSeparator();
            m.addItem (CmdMidiLearn, TRANS ("MIDI Learn..."));
            m.addSeparator();
            add (CmdToggleKeyboardTyping, TRANS ("Musical Typing"), "Ctrl+K", app.musicalTyping);
            {
                // how many cores render the graph: automatic (one worker per physical core), off,
                // or a fixed count the user picks
                PopupMenu mc;
                const bool on = app.engine.isMultiCoreEnabled();
                mc.addItem (CmdRenderCoresAuto,
                            TRANS ("Automatic") + " (" + String (Engine::autoRenderCores()) + " " + TRANS ("cores") + ")",
                            true, on && app.engine.getRenderCoresSetting() == 0);
                mc.addItem (CmdRenderCoresOff, TRANS ("Off"), true, ! on);
                mc.addSeparator();
                const int maxCores = jlimit (2, 16, (int) SystemStats::getNumCpus());
                for (int c = 2; c <= maxCores; ++c)
                    mc.addItem (CmdRenderCores2 + c - 2, String (c) + " " + TRANS ("cores"), true,
                                on && app.engine.getRenderCoresSetting() == c);
                m.addSubMenu (TRANS ("Multi-core Rendering"), mc);
            }
            add (CmdToggleIdleDeviceRelease, TRANS ("Release Audio Device When Idle"), "",
                 app.engine.getReleaseIdleDevice());
            add (CmdToggleFollow, TRANS ("Follow Playhead"), "", app.followPlayhead);
            m.addSeparator();
            add (CmdZoomIn, TRANS ("Zoom In"), "+");
            add (CmdZoomOut, TRANS ("Zoom Out"), "-");
            add (CmdZoomFit, TRANS ("Zoom to Fit Project"), "Z");
            add (CmdTrackTaller, TRANS ("Taller Tracks"), "");
            add (CmdTrackShorter, TRANS ("Shorter Tracks"), "");
            {
                PopupMenu th;
                for (int i = 0; i < 5; ++i)
                {
                    const int id = CmdTrackHeightXS + i;
                    const int h = kTrackHeights[i];
                    th.addItem (id, TRANS (kTrackHeightNames[i]), true, app.trackHeight == h);
                }
                m.addSubMenu (TRANS ("Track Height"), th);
            }
            m.addSeparator();
            {
                PopupMenu lang;
                const auto cur = currentLanguageSetting (app.plugins.properties());
                lang.addItem (CmdLanguageAuto, TRANS ("Automatic"), true, cur == "auto");
                lang.addItem (CmdLanguageEnglish, "English", true, cur == "en");
                lang.addItem (CmdLanguageChinese, juce::String::fromUTF8 ("\xe4\xb8\xad\xe6\x96\x87"), true, cur == "zh");
                m.addSubMenu (TRANS ("Language"), lang);
            }
            add (CmdAudioSettings, TRANS ("Audio / MIDI Settings..."), "");
            add (CmdPluginManager, TRANS ("Plug-in Manager..."), "");
            break;
        }
        default:
            m.addItem (CmdUserManual, TRANS ("User Manual (PDF)"));
            m.addSeparator();
            // the version is spelled out here so it is visible without opening the About box
            m.addItem (CmdAbout, TRANS ("About SoundX") + "  " + String (JUCE_APPLICATION_VERSION_STRING));
            break;
    }
    return m;
}

void MainComponent::menuItemSelected (int id, int) { perform (id); }

//==============================================================================
bool MainComponent::confirmDiscard (std::function<void()> proceed)
{
    if (! app.project.dirty) { proceed(); return true; }
    AlertWindow::showAsync (MessageBoxOptions().withIconType (MessageBoxIconType::QuestionIcon)
                                .withTitle ("SoundX")
                                .withMessage (TRANS ("The project has unsaved changes. Save them first?"))
                                .withButton (TRANS ("Save"))
                                .withButton (TRANS ("Don't Save"))
                                .withButton (TRANS ("Cancel")),
                            [this, proceed] (int r)
    {
        if (r == 1) saveProject (false, proceed);
        else if (r == 2)
        {
            // an explicit discard also throws away the autosave history of this session
            deleteAutosaveSets();
            proceed();
        }
    });
    return false;
}

void MainComponent::deleteAutosaveSets()
{
    const auto dir = app.engine.getCacheFolder();
    if (! dir.isDirectory()) return;
    for (int i = 0; i <= 3; ++i)
        dir.getChildFile (i == 0 ? String ("autosave.sxp") : "autosave-" + String (i) + ".sxp").deleteFile();
    autosaveFile = File();
}

void MainComponent::requestQuit()
{
    confirmDiscard ([] { JUCEApplication::getInstance()->quit(); });
}

void MainComponent::newProject()
{
    confirmDiscard ([this]
    {
        app.engine.stop();
        app.windows->closeAll();
        app.selectedClips.clear();
        app.selectedTrack = -1;
        app.selectedTracks.clear();
        app.editClip = app.editTrack = -1;
        app.project.reset();                  // a new project is empty: no tracks at all
        app.project.dirty = false;
        app.project.sendStructureChanged();
        app.project.sendContentChanged();
        app.project.sendMixChanged();
        app.engine.syncAll();
        app.engine.setPositionBeats (0.0);
        app.project.dirty = false;
        app.notifySelection();
    });
}

void MainComponent::showUserManual()
{
    // the manual is compiled into the exe, so a single SoundX.exe is enough to hand out: it is
    // written next to the settings and then opened with the system viewer
    const File f = PluginManager::dataDirectory().getChildFile ("SoundX-Manual.pdf");
    if (writeUserManual (f))
    {
        if (! f.startAsProcess())
            URL (f).launchInDefaultBrowser();
        return;
    }
    showError (TRANS ("This build ships without the manual (docs/SoundXManual.pdf was not found "
                      "when it was compiled)."));
}

void MainComponent::newProjectFromTemplate()
{
    confirmDiscard ([this]
    {
        const auto tpl = PluginManager::templateFile();
        if (! tpl.existsAsFile()) { showError (TRANS ("There is no project template yet.")); return; }

        app.engine.stop();
        app.windows->closeAll();
        app.selectedClips.clear();
        app.selectedTrack = -1;
        app.selectedTracks.clear();
        app.editClip = app.editTrack = -1;
        if (! app.project.loadFromTemplate (tpl))
        {
            showError (TRANS ("Could not read the project template."));
            return;
        }

        app.selectedTrack = app.project.tracks.empty() ? -1 : app.project.tracks.front()->id;
        if (app.selectedTrack >= 0) app.selectedTracks.insert (app.selectedTrack);
        app.engine.syncAll();
        app.engine.setPositionBeats (0.0);
        app.project.dirty = false;
        app.project.sendStructureChanged();
        app.project.sendContentChanged();
        app.project.sendMixChanged();
        app.notifySelection();
    });
}

void MainComponent::saveAsTemplate()
{
    const auto tpl = PluginManager::templateFile();
    auto doSave = [this, tpl]
    {
        if (! app.project.saveAsTemplate (tpl))
            showError (TRANS ("Could not write the project template."));
    };

    if (! tpl.existsAsFile()) { doSave(); return; }

    AlertWindow::showAsync (MessageBoxOptions()
                                .withIconType (MessageBoxIconType::QuestionIcon)
                                .withTitle (TRANS ("Project Template"))
                                .withMessage (TRANS ("Replace the project template with the current project?"))
                                .withButton (TRANS ("Replace"))
                                .withButton (TRANS ("Cancel")),
                            [doSave] (int r) { if (r == 1) doSave(); });
}

void MainComponent::deleteTemplate()
{
    const auto tpl = PluginManager::templateFile();
    if (! tpl.existsAsFile()) return;

    AlertWindow::showAsync (MessageBoxOptions()
                                .withIconType (MessageBoxIconType::QuestionIcon)
                                .withTitle (TRANS ("Project Template"))
                                .withMessage (TRANS ("Delete the project template? A new session then starts empty again."))
                                .withButton (TRANS ("Delete"))
                                .withButton (TRANS ("Cancel")),
                            [tpl] (int r) { if (r == 1) tpl.deleteFile(); });
}

void MainComponent::openProject()
{
    confirmDiscard ([this]
    {
        chooser = std::make_shared<FileChooser> (TRANS ("Open project"), File::getSpecialLocation (File::userDocumentsDirectory), "*.sxp");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            app.engine.stop();
            app.windows->closeAll();
            app.selectedClips.clear();
            app.editClip = app.editTrack = -1;
            if (! app.project.loadFrom (f))
            {
                showError (TRANS ("Could not open this project file."));
                return;
            }
            app.engine.syncAll();
            app.engine.setPositionBeats (0.0);
            app.selectedTrack = app.project.tracks.empty() ? -1 : app.project.tracks.front()->id;
            app.selectedTracks = app.selectedTrack >= 0 ? std::set<int> { app.selectedTrack } : std::set<int>();
            app.engine.setSelectedTrack (app.selectedTrack);
            app.notifySelection();
            app.project.dirty = false;
        });
    });
}

void MainComponent::doAutosave()
{
    if (! app.project.dirty || app.project.tracks.empty()) return;
    const auto dir = app.engine.getCacheFolder();
    if (dir.getFullPathName().isEmpty()) return;

    // keep a short history: autosave -> autosave-1 -> autosave-2 -> autosave-3 (oldest dropped)
    for (int i = 3; i >= 1; --i)
    {
        const auto older = dir.getChildFile ("autosave-" + String (i) + ".sxp");
        if (i == 3) older.deleteFile();
        const auto prev = dir.getChildFile (i > 1 ? "autosave-" + String (i - 1) + ".sxp" : String ("autosave.sxp"));
        if (prev.existsAsFile()) prev.moveFileTo (older);
    }

    const auto newest = dir.getChildFile ("autosave.sxp");
    if (app.project.writeSnapshotTo (newest)) autosaveFile = newest;
}

void MainComponent::offerAutosaveRecovery()
{
    // look in the user cache folder and in every session folder of the temp directory
    std::vector<File> candidates;
    auto collect = [&candidates] (const File& dir)
    {
        if (! dir.isDirectory()) return;
        for (auto& f : dir.findChildFiles (File::findFiles, false, "autosave*.sxp"))
            candidates.push_back (f);
    };
    collect (app.engine.getRecordingBaseFolder());
    const auto tempRoot = File::getSpecialLocation (File::tempDirectory).getChildFile ("SoundX");
    if (tempRoot.isDirectory())
        for (auto& d : tempRoot.findChildFiles (File::findDirectories, false))
            collect (d);

    File newest;
    for (auto& f : candidates)
        if (newest == File() || f.getLastModificationTime() > newest.getLastModificationTime())
            newest = f;
    if (newest == File()) return;

    // which project the snapshot came from, and whether it is actually newer than that project
    String sourcePath;
    if (auto xml = XmlDocument::parse (newest)) sourcePath = xml->getStringAttribute ("autosaveOf");
    const File source (sourcePath);
    const bool newerThanProject = source.existsAsFile()
                               && newest.getLastModificationTime() > source.getLastModificationTime();
    if (! previousRunCrashed)
    {
        // a clean exit only prompts when a saved project is older than its snapshot; a snapshot
        // of a project that was never saved is discarded work the user chose to leave behind
        if (! source.existsAsFile() || ! newerThanProject) return;
    }

    // the history set around the newest snapshot, newest first
    Array<File> snapshots;
    autosaveFilesToDiscard.clearQuick();
    const auto dir = newest.getParentDirectory();
    for (int i = 0; i <= 3; ++i)
    {
        auto f = dir.getChildFile (i == 0 ? String ("autosave.sxp") : "autosave-" + String (i) + ".sxp");
        if (f.existsAsFile()) snapshots.add (f);
    }
    std::sort (snapshots.begin(), snapshots.end(), [] (const File& a, const File& b)
    {
        return a.getLastModificationTime() > b.getLastModificationTime();
    });
    autosaveFilesToDiscard = snapshots;
    if (snapshots.isEmpty()) return;

    autosaveDialog->open (snapshots, source, previousRunCrashed);
    resized();
}

void MainComponent::chooseRecordingFolder()
{
    const File current = app.engine.getRecordingBaseFolder();
    PopupMenu m;
    m.addSectionHeader (TRANS ("Cache folder") + ": "
                        + (current != File() ? current.getFullPathName() : TRANS ("Default (session cache folder)")));
    m.addItem (1, TRANS ("Choose Folder..."));
    m.addItem (2, TRANS ("Use Default Folder"), current != File());
    m.addItem (3, TRANS ("Open Cache Folder..."));
    m.addItem (4, TRANS ("Autosave Now"));

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&menuBar), [this] (int r)
    {
        if (r == 2)
        {
            app.engine.setRecordingBaseFolder ({});
            saveRecordingFolderSetting();
            return;
        }
        if (r == 3)
        {
            app.engine.getCacheFolder().revealToUser();
            return;
        }
        if (r == 4)
        {
            doAutosave();
            if (autosaveFile.existsAsFile()) showError (TRANS ("Autosave written to:") + "\n" + autosaveFile.getFullPathName());
            return;
        }
        if (r != 1) return;

        recFolderChooser = std::make_shared<FileChooser> (TRANS ("Choose the cache folder"),
                                                        File::getSpecialLocation (File::userMusicDirectory));
        recFolderChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                                       [this] (const FileChooser& fc)
        {
            const auto dir = fc.getResult();
            if (dir == File()) return;
            app.engine.setRecordingBaseFolder (dir);
            saveRecordingFolderSetting();
        });
    });
}

void MainComponent::saveRecordingFolderSetting()
{
    auto* props = app.plugins.properties();
    if (props == nullptr) return;
    props->setValue ("recordingFolder", app.engine.getRecordingBaseFolder().getFullPathName());
    props->saveIfNeeded();
}

void MainComponent::saveProject (bool saveAs, std::function<void()> then)
{
    if (! saveAs && app.project.file.getFullPathName().isNotEmpty())
    {
        app.project.collectAudioFiles (app.project.file);   // gather takes / imports next to the project
        if (! app.project.saveTo (app.project.file)) showError (TRANS ("Could not save the project."));
        else if (then) then();
        return;
    }
    chooser = std::make_shared<FileChooser> (TRANS ("Save project"), File::getSpecialLocation (File::userDocumentsDirectory), "*.sxp");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                          [this, then] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        if (! f.hasFileExtension ("sxp")) f = f.withFileExtension ("sxp");
        app.project.collectAudioFiles (f);   // gather takes / imports next to the project
        if (! app.project.saveTo (f)) showError (TRANS ("Could not save the project."));
        else if (then) then();
        updateTitle();
    });
}

void MainComponent::importMidi()
{
    chooser = std::make_shared<FileChooser> (TRANS ("Import MIDI file"), File::getSpecialLocation (File::userMusicDirectory), "*.mid;*.midi");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectMultipleItems | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc)
    {
        const auto files = fc.getResults();
        if (files.isEmpty()) return;

        // an empty project takes the file at bar 1, otherwise it lands on the bar under the playhead
        const bool empty = app.project.tracks.empty();
        const double atBeat = empty ? 0.0
                                    : jmax (0.0, tempomap::barStartAtOrBefore (app.project.tempo, app.engine.getPositionBeats()));
        // ... and the dialog asks whether the file's tempo / meter map should come along
        showMidiImportDialog (app, files, [this, files, atBeat] (bool syncTempo)
        {
            // all files of one import are one undo step, so a single Ctrl+Z takes the whole
            // selection back (Studio One does the same with a dropped folder)
            importFiles (app, files, atBeat, -1, syncTempo);
        });
    });
}

void MainComponent::importAudio()
{
    chooser = std::make_shared<FileChooser> (TRANS ("Import audio file"), File::getSpecialLocation (File::userMusicDirectory),
                                             app.engine.formats.getWildcardForAllFormats());
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectMultipleItems | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc)
    {
        const auto files = fc.getResults();
        if (files.isEmpty()) return;

        // the whole selection is one undo step (see importFiles)
        importFiles (app, files, app.engine.getPositionBeats(), app.selectedTrack);
    });
}

void MainComponent::exportMidi()
{
    chooser = std::make_shared<FileChooser> (TRANS ("Export MIDI file"), File::getSpecialLocation (File::userMusicDirectory), "*.mid");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                          [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        if (! f.hasFileExtension ("mid;midi")) f = f.withFileExtension ("mid");
        if (! app.project.exportMidi (f)) showError (TRANS ("Could not write the MIDI file."));
    });
}

//==============================================================================
void MainComponent::perform (int cmd)
{
    auto& p = app.project;
    auto& e = app.engine;
    const bool pr = pianoRollActive();

    switch (cmd)
    {
        case CmdNewProject: newProject(); break;
        case CmdNewFromTemplate: newProjectFromTemplate(); break;
        case CmdSaveAsTemplate: saveAsTemplate(); break;
        case CmdDeleteTemplate: deleteTemplate(); break;
        case CmdOpenProject: openProject(); break;
        case CmdSaveProject: saveProject (false); break;
        case CmdSaveProjectAs: saveProject (true); break;
        case CmdProjectInfo: showProjectInfoDialog (app); break;
        case CmdMidiLearn: showMidiLearnWindow (app); break;
        case CmdImportMidi: importMidi(); break;
        case CmdImportAudio: importAudio(); break;
        case CmdExportMidi: exportMidi(); break;
        case CmdExportMix:
        case CmdExportStems:
        {
            const auto mode = cmd == CmdExportMix ? ExportOptions::Mix : ExportOptions::Stems;
            // Open the export dialog on the next message-loop turn: launching a modal dialog from
            // inside a menu popup's callback can leave it hidden/behind the main window, which is
            // exactly the intermittent "clicking Export does nothing" (single and repeated) bug.
            Component::SafePointer<MainComponent> safe (this);
            auto* appPtr = &app;
            MessageManager::callAsync ([safe, appPtr, mode]
            {
                if (safe.getComponent() != nullptr) showExportDialog (*appPtr, mode);
            });
            break;
        }
        case CmdSetRecordingFolder: chooseRecordingFolder(); break;
        case CmdQuit: requestQuit(); break;

        case CmdUndo: p.undo(); break;
        case CmdRedo: p.redo(); break;
        case CmdCut: if (pr) pianoRoll.cutSelection(); else arrange.cutSelected(); break;
        case CmdCopy: if (pr) pianoRoll.copySelection(); else arrange.copySelected(); break;
        case CmdPaste: if (pr) pianoRoll.pasteAtPlayhead(); else arrange.pasteAtPlayhead(); break;
        case CmdDelete: if (pr) pianoRoll.deleteSelection(); else arrange.deleteSelected(); break;
        case CmdSelectAll: if (pr) pianoRoll.selectAll(); else arrange.selectAll(); break;
        case CmdDuplicate: if (pr) pianoRoll.duplicateSelection(); else arrange.duplicateSelected(); break;
        case CmdSplit: arrange.splitAtPlayhead(); break;
        case CmdSplitAtCursor: arrange.splitAtCursor(); break;
        case CmdAddCrossfade: arrange.addCrossfadeSelected(); break;
        case CmdFadeInToPlayhead: arrange.fadesToPlayhead (true); break;
        case CmdFadeOutFromPlayhead: arrange.fadesToPlayhead (false); break;
        case CmdClearFades: arrange.clearFadesSelected(); break;
        case CmdQuantize: pianoRoll.quantizeSelection(); break;
        case CmdReverseNotes: pianoRoll.reverseSelection(); break;
        case CmdHumanize: pianoRoll.humanizeSelection (app.humanize); break;
        case CmdHumanizeDialog: pianoRoll.showHumanizeDialog(); break;
        case CmdSetLoopToSelection: arrange.setLoopToSelection(); break;
        case CmdMergeClips: arrange.mergeSelectedClips(); break;
        case CmdReverseAudio: arrange.reverseSelectedAudio(); break;
        case CmdAudioResetEdits: arrange.resetSelectedAudioEdits(); break;
        case CmdAudioStretchPrompt:
            promptText (TRANS ("Time Stretch"), TRANS ("Speed factor: 2 = twice as long, 0.5 = twice as fast"),
                        String ((double) 1.0, 2), [this] (String s)
            {
                const double v = s.trim().getDoubleValue();
                if (v > 0.0) arrange.setSelectedAudioStretch (v);
            });
            break;
        case CmdAudioPitchPrompt:
            promptText (TRANS ("Transpose"), TRANS ("Semitones (-24 .. +24)"), "0", [this] (String s)
            {
                arrange.setSelectedAudioPitch ((int) std::lround (s.trim().getDoubleValue()));
            });
            break;

        case CmdNewInstrumentTrack: app.addTrack (TrackType::Instrument); break;
        case CmdNewAudioTrack: app.addTrack (TrackType::Audio); break;
        case CmdNewBusTrack: app.addTrack (TrackType::Bus); break;
        case CmdNewSendTrack: app.addSendTrack(); break;
        case CmdHideTracks:
        {
            // the arrangement can be in a clip selection with no track row selected at all: then
            // hide the channels that own the selected clips instead of doing nothing
            std::vector<int> ids (app.selectedTracks.begin(), app.selectedTracks.end());
            if (ids.empty() && app.selectedTrack >= 0) ids.push_back (app.selectedTrack);
            if (ids.empty()) ids = app.selectedClipTrackIds();
            app.setTracksHidden (ids, true);
            break;
        }
        case CmdShowAllTracks:
            // only the rows the arranger can show: multi-output mixer channels stay arranger-hidden
            app.setTracksHidden (app.hiddenArrangerTrackIds(), false);
            break;
        case CmdGroupIntoBus:
        {
            std::vector<int> ids (app.selectedTracks.begin(), app.selectedTracks.end());
            if (ids.empty() && app.selectedTrack >= 0) ids.push_back (app.selectedTrack);
            app.groupIntoBus (ids);
            break;
        }
        case CmdGroupIntoFolder:
        {
            std::vector<int> ids (app.selectedTracks.begin(), app.selectedTracks.end());
            if (ids.empty() && app.selectedTrack >= 0) ids.push_back (app.selectedTrack);
            app.groupIntoFolder (ids);
            break;
        }
        case CmdUnpackFolder:
        {
            for (int id : app.selectedTracks)
                if (auto* t = app.project.findTrack (id); t != nullptr && t->isFolder)
                {
                    app.unpackFolder (id);
                    break;
                }
            break;
        }
        case CmdAddAutomationTrack:
        {
            int id = app.selectedTrack;
            if (auto* t = app.project.findTrack (id); t != nullptr && t->isAutomation())
                id = t->autoTargetTrack;
            if (id < 0) break;
            showAutomationTargetMenu (app, id, nullptr, true);
            break;
        }
        case CmdAddTracks: showAddTracksDialog (app); break;
        case CmdToggleMasterTrack:
            app.showMasterTrack = ! app.showMasterTrack;
            app.notifyView();          // the arrangement re-lays out the pinned master bus row
            arrange.repaint();
            break;
        case CmdAddListenBus:
        {
            app.project.checkpoint();
            for (auto& t : app.project.tracks) t->isListenBus = false;
            auto& lb = app.project.addTrack (TrackType::Bus, TRANS ("Listen"));
            lb.isListenBus = true;
            lb.hidden = true;          // monitoring stage: mixer only
            lb.listenEnabled = true;
            lb.volumeDb = 0.0f;
            app.project.sendStructureChanged();
            app.project.sendContentChanged();
            app.engine.syncAll();
            break;
        }
        case CmdRemoveListenBus:
        {
            if (auto* lb = app.project.listenBus())
            {
                app.project.checkpoint();
                app.project.removeTrack (lb->id);
                app.project.sendStructureChanged();
                app.project.sendContentChanged();
                app.engine.syncAll();
            }
            break;
        }
        case CmdDeleteTrack:
        {
            std::vector<int> ids (app.selectedTracks.begin(), app.selectedTracks.end());
            if (ids.empty() && app.selectedTrack >= 0) ids.push_back (app.selectedTrack);
            app.deleteTracks (ids);
            break;
        }
        case CmdDuplicateTrack:
        {
            std::vector<int> ids (app.selectedTracks.begin(), app.selectedTracks.end());
            if (ids.empty() && app.selectedTrack >= 0) ids.push_back (app.selectedTrack);
            app.duplicateTracks (ids);
            break;
        }

        case CmdPlayStop: e.togglePlay(); break;
        case CmdStop:
            if (e.isPlaying()) e.stop();
            else e.seekToBeats (p.loopOn ? p.loopStart : 0.0);
            break;
        case CmdRecord: e.record(); break;
        case CmdRewind: e.seekToBeats (p.loopOn ? p.loopStart : 0.0); break;
        case CmdToggleLoop:
            p.loopOn = ! p.loopOn;
            if (p.loopOn && p.loopEnd - p.loopStart < 0.5) { p.loopStart = 0.0; p.loopEnd = tempomap::beatOfBar (p.tempo, 4); }
            p.sendContentChanged();
            break;
        case CmdToggleMetronome: p.metronome = ! p.metronome; p.sendMixChanged(); p.sendContentChanged(); break;
        case CmdToggleCountIn: p.countInBars = p.countInBars > 0 ? 0 : 1; p.sendContentChanged(); break;
        case CmdCountInOff:    p.countInBars = 0; p.sendContentChanged(); break;
        case CmdCountIn1Bar:   p.countInBars = 1; p.sendContentChanged(); break;
        case CmdCountIn2Bars:  p.countInBars = 2; p.sendContentChanged(); break;
        case CmdCountIn4Bars:  p.countInBars = 4; p.sendContentChanged(); break;
        case CmdCountIn8Bars:  p.countInBars = 8; p.sendContentChanged(); break;
        case CmdRecQuantOff:   p.recordQuantize = 0.0; p.sendContentChanged(); break;
        case CmdRecQuant4:     p.recordQuantize = 1.0; p.sendContentChanged(); break;
        case CmdRecQuant8:     p.recordQuantize = 0.5; p.sendContentChanged(); break;
        case CmdRecQuant8T:    p.recordQuantize = 1.0 / 3.0; p.sendContentChanged(); break;
        case CmdRecQuant16:    p.recordQuantize = 0.25; p.sendContentChanged(); break;
        case CmdRecQuant16T:   p.recordQuantize = 1.0 / 6.0; p.sendContentChanged(); break;
        case CmdRecQuant32:    p.recordQuantize = 0.125; p.sendContentChanged(); break;
        case CmdToggleTakeRecording: p.takeRecording = ! p.takeRecording; p.sendContentChanged(); break;
        case CmdTimebaseBars:    app.setTimeInSeconds (false); break;
        case CmdTimebaseSeconds: app.setTimeInSeconds (true); break;

        case CmdToggleInspector: inspectorVisible = ! inspectorVisible; resized(); break;
        case CmdToggleMixer: setBottom (bottom == Bottom::Mixer ? Bottom::None : Bottom::Mixer); break;
        case CmdTogglePianoRoll: setBottom (bottom == Bottom::PianoRoll ? Bottom::None : Bottom::PianoRoll); break;
        case CmdPianoRollFull: toggleFullscreen(); break;
        case CmdToggleAppFullscreen:
            if (onToggleAppFullscreen != nullptr) onToggleAppFullscreen();
            break;
        case CmdToggleDarkMode:
        {
            theme::setDark (! theme::isDark());
            if (auto* lf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel()))
                lf->applyColours();
            app.plugins.properties()->setValue ("darkMode", theme::isDark());
            app.plugins.properties()->saveIfNeeded();
            if (onThemeChange) onThemeChange();   // window frame / title bar
            refreshTheme();                       // the live UI, in place
            break;
        }
        case CmdTogglePluginEditorOnInsert:
            app.setOpenPluginEditorOnInsert (! app.openPluginEditorOnInsert);
            break;
        case CmdToggleMultiPluginWindows:
            app.setMultiPluginWindows (! app.multiPluginWindows);
            break;
        case CmdToggleMediaPool:
            mediaPoolVisible = ! mediaPoolVisible;
            resized();
            repaint();
            break;
        case CmdToggleKeyboardTyping:
            setMusicalTyping (! app.musicalTyping);
            break;
        case CmdToggleMultiCore:
            app.engine.setRenderCores (app.engine.isMultiCoreEnabled() ? 1 : 0);
            break;
        case CmdToggleIdleDeviceRelease:
            app.engine.setReleaseIdleDevice (! app.engine.getReleaseIdleDevice());
            break;
        case CmdRenderCoresOff: app.engine.setRenderCores (1); break;
        case CmdRenderCoresAuto: app.engine.setRenderCores (0); break;
        case CmdToggleFollow: app.followPlayhead = ! app.followPlayhead; break;
        case CmdToggleTempoTrack: app.showTempoLane = ! app.showTempoLane; app.notifyView(); break;
        case CmdToggleChordTrack:
            app.showChordTrack = ! app.showChordTrack;
            app.notifyView();          // the arrangement re-lays out the pinned rows
            arrange.repaint();
            break;
        case CmdToggleMarkerTrack:
            app.showMarkerTrack = ! app.showMarkerTrack;
            app.notifyView();
            arrange.repaint();
            break;
        case CmdTogglePianoRollChords:
            app.pianoRollChords = ! app.pianoRollChords;
            pianoRoll.resized();
            pianoRoll.repaint();
            break;
        case CmdChordEnter: arrange.enterChordAtPlayhead(); break;
        case CmdMarkerEnter: arrange.addSectionAtPlayhead(); break;
        case CmdZoomIn: if (pr) pianoRoll.zoomHorizontal (1.3); else arrange.zoomBy (1.3); break;
        case CmdZoomOut: if (pr) pianoRoll.zoomHorizontal (1.0 / 1.3); else arrange.zoomBy (1.0 / 1.3); break;
        case CmdZoomFit: arrange.zoomToFit(); break;
        case CmdTrackTaller: arrange.zoomTrackHeights (1.0f); break;
        case CmdTrackShorter: arrange.zoomTrackHeights (-1.0f); break;
        case CmdTrackHeightXS:
        case CmdTrackHeightS:
        case CmdTrackHeightM:
        case CmdTrackHeightL:
        case CmdTrackHeightXL:
            arrange.setAllTrackHeights (kTrackHeights[cmd - CmdTrackHeightXS]);
            break;

        case CmdAudioSettings: showAudioSettings (app); break;
        case CmdPluginManager: app.plugins.showScannerWindow(); break;
        case CmdAbout: showAboutDialog(); break;
        case CmdUserManual: showUserManual(); break;
        case CmdLanguageAuto:
        case CmdLanguageEnglish:
        case CmdLanguageChinese:
            saveLanguageSetting (app.plugins.properties(), cmd == CmdLanguageAuto ? "auto" : cmd == CmdLanguageEnglish ? "en" : "zh");
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "SoundX", TRANS ("Restart SoundX to apply the language change."));
            break;
        default:
            if (cmd >= CmdRenderCores2 && cmd < CmdRenderCores2 + 15)
                app.engine.setRenderCores (cmd - CmdRenderCores2 + 2);
            else if (cmd >= CmdAudioStretchPreset && cmd < CmdAudioStretchPreset + (int) audioStretchPresets().size())
                arrange.setSelectedAudioStretch (audioStretchPresets()[(size_t) (cmd - CmdAudioStretchPreset)]);
            else if (cmd >= CmdAudioPitchPreset && cmd < CmdAudioPitchPreset + (int) audioPitchPresets().size())
                arrange.setSelectedAudioPitch (audioPitchPresets()[(size_t) (cmd - CmdAudioPitchPreset)]);
            break;
    }
    controlBar.repaint();
}

//==============================================================================
bool MainComponent::handlePluginShortcut (const KeyPress& k)
{
    // while a plug-in's text field has the keyboard, its typing (including Space and Ctrl+Z)
    // belongs to the plug-in; every other control lets the host shortcuts through
    if (dynamic_cast<TextEditor*> (Component::getCurrentlyFocusedComponent()) != nullptr) return false;
    if (app.musicalTyping) return false;

    const auto m = k.getModifiers();
    const int code = k.getKeyCode();
    if (code == KeyPress::spaceKey) { perform (CmdPlayStop); return true; }
    if (m.isCtrlDown() || m.isCommandDown()) return keyPressed (k);
    if (code == KeyPress::F2Key || code == KeyPress::F4Key || code == KeyPress::F11Key)
        return keyPressed (k);
    if (code == KeyPress::returnKey && m.isAltDown()) return keyPressed (k);
    return false;
}

bool MainComponent::keyPressed (const KeyPress& k)
{
    // Computer-keyboard performance owns every key while it is on: no transport / edit / view
    // shortcut may fire. Text fields still edit normally; click the toolbar keyboard button (or
    // use the menu) to leave this mode.
    if (dynamic_cast<TextEditor*> (Component::getCurrentlyFocusedComponent()) != nullptr) return false;
    if (app.musicalTyping) return true;

    const auto m = k.getModifiers();
    const int code = k.getKeyCode();
    juce_wchar ch = (juce_wchar) CharacterFunctions::toLowerCase (k.getTextCharacter());
    // A Chinese IME (or any layout that swallows WM_CHAR) delivers no text character for letter
    // keys, which used to kill every letter shortcut. Fall back to the key code. Windows also
    // maps Ctrl+Z to the control character 0x1a (Ctrl+Y to 0x19, ...) and JUCE hands that over
    // as the key code with no text character at all, so a *shortcut* has to be turned back into
    // its letter here as well - otherwise Ctrl+Z / Ctrl+Y (undo / redo) and every other
    // Ctrl+letter command did nothing at all.
    if (ch == 0)
    {
        if (code >= 'A' && code <= 'Z') ch = (juce_wchar) (code - 'A' + 'a');
        else if (code >= 'a' && code <= 'z') ch = (juce_wchar) code;
        else if (code >= 1 && code <= 26) ch = (juce_wchar) ('a' + code - 1);
        else if (code >= 32 && code < 127) ch = (juce_wchar) code;
    }
    const bool ctrl = m.isCtrlDown() || m.isCommandDown();
    const bool pr = pianoRollActive();

    if (code == KeyPress::spaceKey) { perform (CmdPlayStop); return true; }
    if (code == KeyPress::returnKey && m.isAltDown()) { perform (CmdToggleAppFullscreen); return true; }
    if (code == KeyPress::returnKey && ! ctrl) { perform (CmdRewind); return true; }

    if (ctrl)
    {
        if (ch == 'z') { perform (m.isShiftDown() ? CmdRedo : CmdUndo); return true; }
        if (ch == 'y') { perform (CmdRedo); return true; }
        if (ch == 's') { perform (m.isShiftDown() ? CmdSaveProjectAs : CmdSaveProject); return true; }
        if (ch == 'o') { perform (CmdOpenProject); return true; }
        if (ch == 'n') { perform (m.isShiftDown() ? CmdNewInstrumentTrack : CmdNewProject); return true; }
        if (ch == 'i') { perform (CmdImportMidi); return true; }
        if (ch == 'e') { perform (CmdExportMix); return true; }
        if (ch == 'a') { perform (CmdSelectAll); return true; }
        if (ch == 'c') { perform (CmdCopy); return true; }
        if (ch == 'x') { perform (CmdCut); return true; }
        if (ch == 'v') { perform (CmdPaste); return true; }
        if (ch == 'd') { perform (CmdDuplicate); return true; }
        if (ch == 't') { perform (CmdSplit); return true; }
        if (ch == 'x' && m.isAltDown()) { perform (CmdSplit); return true; }   // Studio One style split
        if (ch == 'j') { perform (CmdMergeClips); return true; }
        if (ch == 'k') { perform (CmdToggleKeyboardTyping); return true; }
        if (ch == 'h') { perform (m.isShiftDown() ? CmdShowAllTracks : CmdHideTracks); return true; }
        return false;
    }

    if (code == KeyPress::F2Key) { perform (CmdToggleMixer); return true; }
    if (code == KeyPress::F4Key) { perform (CmdTogglePianoRoll); return true; }
    if (code == KeyPress::F11Key) { perform (CmdPianoRollFull); return true; }
    if (code == KeyPress::deleteKey || code == KeyPress::backspaceKey) { perform (CmdDelete); return true; }

    if (pr)
    {
        const double step = pianoRoll.grid > 0 ? pianoRoll.grid : 0.25;
        if (code == KeyPress::upKey) { pianoRoll.transposeSelection (m.isShiftDown() ? 12 : 1); return true; }
        if (code == KeyPress::downKey) { pianoRoll.transposeSelection (m.isShiftDown() ? -12 : -1); return true; }
        if (code == KeyPress::leftKey) { pianoRoll.nudgeSelection (-step); return true; }
        if (code == KeyPress::rightKey) { pianoRoll.nudgeSelection (step); return true; }
    }

    // Tool shortcuts: 1 / 2 / 3 pick pointer, pencil and eraser. The numbers follow the panel that
    // is in front: while the piano roll is the active context they drive its own three tools, in
    // the arrangement they drive the arrange tools - which have a fourth (4 = mute regions).
    if (ch >= '1' && ch <= '5' && ! m.isAltDown())
    {
        if (pr)
        {
            if (ch == '1') { pianoRoll.setTool (PianoRollView::Tool::Pointer); return true; }
            if (ch == '2') { pianoRoll.setTool (PianoRollView::Tool::Pencil); return true; }
            if (ch == '3') { pianoRoll.setTool (PianoRollView::Tool::Eraser); return true; }
            // 5 = scissors in both editors: click a clip / a note to cut it
            if (ch == '5') { pianoRoll.setTool (PianoRollView::Tool::Scissors); app.arrangeTool = ArrangeTool::Scissors; return true; }
            return false;   // the piano roll has no mute tool, so 4 keeps its meaning elsewhere
        }

        app.arrangeTool = ch == '1' ? ArrangeTool::Select
                        : ch == '2' ? ArrangeTool::Pencil
                        : ch == '3' ? ArrangeTool::Eraser
                        : ch == '4' ? ArrangeTool::Mute
                                    : ArrangeTool::Scissors;
        return true;
    }

    if (code == KeyPress::leftKey || code == KeyPress::rightKey)
    {
        auto& tempo = app.project.tempo;
        const double cur = app.engine.getPositionBeats();
        double target;
        if (code == KeyPress::leftKey)
        {
            const double barStart = tempomap::barStartAtOrBefore (tempo, cur);
            target = (cur - barStart > 1.0e-4) ? barStart : tempomap::barStartAtOrBefore (tempo, barStart - 1.0e-3);
        }
        else
        {
            target = tempomap::beatOfBar (tempo, tempomap::barPosition (tempo, cur).bar + 1);
        }
        app.engine.seekToBeats (jmax (0.0, target));
        return true;
    }

    if (app.musicalTyping)
    {
        // letter keys belong to the on-screen keyboard
        if (String (kTypingKeys).containsChar (ch) || ch == 'z' || ch == 'x' || ch == 'c' || ch == 'v') return true;
        return false;
    }

    if (ch == 'r') { perform (CmdRecord); return true; }
    if (ch == 'c') { perform (CmdToggleLoop); return true; }
    if (ch == 'k') { perform (CmdToggleMetronome); return true; }
    if (ch == 's') { perform (CmdSplitAtCursor); return true; }
    if (ch == 'x')
    {
        // Studio One: X crossfades the selected audio clips; with no audio selection it toggles the mixer
        bool audioSel = false;
        for (auto& t : app.project.tracks)
            for (auto& c : t->audioClips)
                if (app.selectedClips.count (c.id)) { audioSel = true; break; }
        perform (audioSel ? CmdAddCrossfade : CmdToggleMixer);
        return true;
    }
    if (ch == 'p') { perform (CmdTogglePianoRoll); return true; }
    if (ch == 'i') { perform (CmdToggleInspector); return true; }
    if (ch == 't') { perform (CmdToggleTempoTrack); return true; }
    if (ch == 'l') { perform (CmdToggleLoop); return true; }
    if (ch == 'q') { perform (CmdQuantize); return true; }
    // H humanises the notes of the piano roll while its panel is open, Shift+H sets the amount
    if (ch == 'h' && pianoRoll.isVisible() && pianoRoll.clip() != nullptr)
    {
        perform (m.isShiftDown() ? CmdHumanizeDialog : CmdHumanize);
        return true;
    }
    if (ch == 'z') { perform (CmdZoomFit); return true; }
    if (ch == '0') { perform (CmdStop); return true; }
    if (ch == '+' || ch == '=') { perform (CmdZoomIn); return true; }
    if (ch == '-') { perform (CmdZoomOut); return true; }
    if (code == KeyPress::escapeKey)
    {
        if (onExitAppFullscreen != nullptr && onExitAppFullscreen()) return true;
        app.clearClipSelection();
        return true;
    }
    return false;
}

bool MainComponent::keyStateChanged (bool)
{
    if (dynamic_cast<TextEditor*> (Component::getCurrentlyFocusedComponent()) != nullptr)
    {
        if (! typingNotes.empty()) releaseAllTypingNotes();
        return false;
    }
    if (! app.musicalTyping)
    {
        if (! typingNotes.empty()) releaseAllTypingNotes();
        return false;
    }
    musicalTypingUpdate();
    return false;
}

void MainComponent::musicalTypingUpdate()
{
    const String keys (kTypingKeys);
    const int base = (app.keyboardOctave + 2) * 12;
    bool consumed = false;
    for (int i = 0; i < keys.length(); ++i)
    {
        const int kc = (int) keys[i];
        const bool down = KeyPress::isKeyCurrentlyDown (kc == ';' ? (int) ';' : (int) CharacterFunctions::toUpperCase (keys[i]));
        auto it = typingNotes.find (kc);
        if (down && it == typingNotes.end())
        {
            const int note = jlimit (0, 127, base + i);
            typingNotes[kc] = note;
            app.engine.noteOn (note, app.keyboardVelocity);
            consumed = true;
        }
        else if (! down && it != typingNotes.end())
        {
            app.engine.noteOff (it->second);
            typingNotes.erase (it);
        }
    }
    static bool zDown = false, xDown = false, cDown = false, vDown = false;
    auto edge = [] (int key, bool& state) { const bool d = KeyPress::isKeyCurrentlyDown (key); const bool r = d && ! state; state = d; return r; };
    if (edge ('Z', zDown)) app.keyboardOctave = jmax (-2, app.keyboardOctave - 1);
    if (edge ('X', xDown)) app.keyboardOctave = jmin (8, app.keyboardOctave + 1);
    if (edge ('C', cDown)) app.keyboardVelocity = jmax (8, app.keyboardVelocity - 16);
    if (edge ('V', vDown)) app.keyboardVelocity = jmin (127, app.keyboardVelocity + 16);
    if (virtualKeyboard != nullptr && virtualKeyboard->isVisible()) virtualKeyboard->notesChanged();
    juce::ignoreUnused (consumed);
}

void MainComponent::releaseAllTypingNotes()
{
    for (auto& [k, n] : typingNotes) app.engine.noteOff (n);
    typingNotes.clear();
    if (virtualKeyboard != nullptr) virtualKeyboard->notesChanged();
}
}
