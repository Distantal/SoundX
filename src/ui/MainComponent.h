#pragma once

#include "AppState.h"
#include "ControlBar.h"
#include "ArrangeView.h"
#include "PianoRoll.h"
#include "Panels.h"
#include "Commands.h"
#include "ClipGainEditor.h"
#include "MediaBrowser.h"
#include "AutosaveDialog.h"

namespace sx
{
class VirtualKeyboard;

class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      public juce::DragAndDropContainer,   // plug-in slots can be dragged between strips
                      private AppState::Listener,
                      private Project::Listener,
                      private juce::Timer
{
public:
    explicit MainComponent (AppState& a);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    /** Re-attach the plug-in panels after this component is on the desktop. */
    void refreshHostLayer();
    /** Apply a theme change to the live UI: the window, its layout and every open editor stay
        exactly where they are, only the colours are swapped. */
    void refreshTheme();
    /** Shows / hides the floating keyboard after app.musicalTyping changed. */
    void syncVirtualKeyboard();
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isDown) override;
    void mouseDown (const juce::MouseEvent&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int index, const juce::String& name) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    void perform (int cmd);
    /** Host-wide shortcuts (space, Ctrl+..., F-keys) typed while a plug-in panel has the focus. */
    bool handlePluginShortcut (const juce::KeyPress&);
    void openClipGainEditor (int trackId, int clipId);   // fullscreen clip gain envelope editor
    void offerAutosaveRecovery();                        // called once after startup
    void deleteAutosaveSets();
    void setPreviousRunCrashed (bool c) { previousRunCrashed = c; }
    void doAutosave();
    bool confirmDiscard (std::function<void()> proceed);
    void requestQuit();
    std::function<void (const juce::String&)> onTitleChanged;
    std::function<void()> onThemeChange;   // fired after toggling dark mode (the window repaints in place)
    std::function<void()> onToggleAppFullscreen;      // Maximise / Alt+Enter / View > Fullscreen
    std::function<bool()> onExitAppFullscreen;        // Esc while in fullscreen (true = consumed)
    juce::String windowTitle() const;
    juce::MenuBarComponent& menuBarComponent() { return menuBar; }

private:
    class Resizer;
    enum class Bottom { None, Mixer, PianoRoll };

    static constexpr int kTabStripH = 28;   // always-visible dock strip holding the panel tabs

    void selectionChanged() override {}
    void panelRequested (const juce::String& name) override;
    void projectStructureChanged() override { updateTitle(); syncGainEditor(); }
    void projectContentChanged() override { updateTitle(); syncGainEditor(); }
    void timerCallback() override;
    void setBottom (Bottom b);
    void refreshBottomTabColours();
    void setMusicalTyping (bool on);
    void placeKeyboardWindow (bool resetToDefault);
    void toggleFullscreen();
    void updateTitle();
    void syncGainEditor();   // closes the fullscreen editor when its clip is gone
    void newProject();
    /** Writes the manual (embedded in the exe) next to the settings and opens it. */
    void showUserManual();
    /** New project from the saved template (File > New from Template). */
    void newProjectFromTemplate();
    /** Stores the current project as the template a new session starts from. */
    void saveAsTemplate();
    void deleteTemplate();
    void openProject();
    void saveProject (bool saveAs, std::function<void()> then = {});
    void chooseRecordingFolder();
    void saveRecordingFolderSetting();
    void importMidi();
    void importAudio();
    void exportMidi();
    void musicalTypingUpdate();
    void releaseAllTypingNotes();
    bool pianoRollActive() const { return bottom == Bottom::PianoRoll && lastClickInPiano; }

    AppState& app;
    juce::MenuBarComponent menuBar;
    ControlBar controlBar;
    InspectorView inspector;
    ArrangeView arrange;
    MediaBrowser mediaPool;
    MixerView mixer;
    PianoRollView pianoRoll;
    std::unique_ptr<Resizer> resizer;
    std::unique_ptr<ClipGainEditor> gainEditor;
    std::unique_ptr<AutosaveDialog> autosaveDialog;
    std::unique_ptr<VirtualKeyboard> virtualKeyboard;
    // transparent layer above the whole UI: plug-in panels are children of it, empty areas pass
    // clicks through to the arrangement (one native window, no taskbar entries for plug-ins)
    juce::Component pluginHost;
    bool previousRunCrashed = false;
    juce::TextButton mixerTab { TRANS ("Mixer") }, pianoTab { TRANS ("Piano Roll") };
    IconButton closeBottom { Icon::Close, TRANS ("Close panel") };
    juce::TooltipWindow tooltip { this, 500 };

    Bottom bottom = Bottom::Mixer;
    bool inspectorVisible = true;
    bool mediaPoolVisible = false;
    int bottomHeight = 300;
    bool bottomFull = false;
    bool lastClickInPiano = false;
    juce::String lastTitle;
    std::set<int> typingKeysDown;
    std::map<int, int> typingNotes; // key code -> midi note
    std::shared_ptr<juce::FileChooser> chooser;
    std::shared_ptr<juce::FileChooser> recFolderChooser;
    juce::uint32 lastAutosaveMs = 0;
    juce::File autosaveFile;
    juce::Array<juce::File> autosaveFilesToDiscard;

    // SOUNDX_UI_PROFILE=<file> diagnostic: measures how late the 10 Hz UI timer runs. A late timer
    // means the message thread is being starved (CPU contention) or overloaded (a heavy plug-in
    // editor pumping its UI on the same thread); the summary is appended every 5 seconds.
    double lastUiTickMs = 0.0;
    int uiProfileCount = 0;
    double uiProfileSum = 0.0, uiProfileMax = 0.0;
    int uiStalls = 0;
};
}
