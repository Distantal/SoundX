#pragma once

#include "AppState.h"
#include "Widgets.h"
#include "Commands.h"

namespace sx
{
class NumberLabel : public juce::Label
{
public:
    std::function<void (double)> onDrag;      // delta in steps
    std::function<void()> onDragStart;
    void mouseDown (const juce::MouseEvent& e) override
    {
        juce::Label::mouseDown (e);
        lastY = e.y;
        acc = 0.0;
        if (onDragStart) onDragStart();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (isBeingEdited()) return;
        acc += (double) (lastY - e.y);
        lastY = e.y;
        const int steps = (int) acc;
        if (steps != 0 && onDrag) { onDrag ((double) steps * (e.mods.isShiftDown() ? 0.1 : 1.0)); acc -= steps; }
    }
    void mouseUp (const juce::MouseEvent& e) override { juce::Label::mouseUp (e); }

private:
    int lastY = 0;
    double acc = 0.0;
};

class ControlBar : public juce::Component, private juce::Timer, private Project::Listener
{
public:
    explicit ControlBar (AppState& a);
    ~ControlBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    /** Re-apply the theme colours that live on individual controls (not on the LookAndFeel). */
    void refreshTheme();

    std::function<void (int)> onCommand;

    IconButton inspectorBtn { Icon::Inspector, TRANS ("Inspector (I)") };
    IconButton mixerBtn { Icon::Mixer, TRANS ("Mixer (X)") };
    IconButton editorBtn { Icon::Piano, TRANS ("Piano Roll (P)") };
    IconButton mediaPoolBtn { Icon::Folder, TRANS ("Media Pool") };

private:
    void timerCallback() override;
    void projectContentChanged() override { syncToggles(); }
    void projectMixChanged() override { syncToggles(); }
    void syncToggles();
    void syncToolButtons();
    void syncTimebaseButton();
    void showCountInMenu();
    void showTimebaseMenu();
    void cmd (int c) { if (onCommand) onCommand (c); }
    void showSignatureMenu();

    AppState& app;
    IconButton rewindBtn { Icon::Rewind, TRANS ("Return to Start (Enter)") };
    IconButton stopBtn { Icon::Stop, TRANS ("Stop") };
    IconButton playBtn { Icon::Play, TRANS ("Play / Pause (Space)"), theme::accent };
    IconButton recBtn { Icon::Record, TRANS ("Record (R)"), theme::record };
    IconButton loopBtn { Icon::Loop, TRANS ("Cycle (C)") };
    IconButton metroBtn { Icon::Metronome, TRANS ("Metronome (K)") };
    IconButton countBtn { Icon::CountIn, TRANS ("Count-in") };
    IconButton undoBtn { Icon::Undo, TRANS ("Undo (Ctrl+Z)") };
    IconButton redoBtn { Icon::Redo, TRANS ("Redo (Ctrl+Shift+Z)") };
    IconButton mergeBtn { Icon::Merge, TRANS ("Merge Selected Clips (Ctrl+J)") };
    // arrange tools (second row)
    IconButton selectBtn { Icon::Pointer, TRANS ("Select Tool (1)") };
    IconButton pencilToolBtn { Icon::Pencil, TRANS ("Pencil Tool (2)") };
    IconButton eraserToolBtn { Icon::Eraser, TRANS ("Eraser Tool (3)") };
    IconButton muteToolBtn { Icon::MuteTool, TRANS ("Mute Region Tool (4)") };
    IconButton scissorsToolBtn { Icon::Scissors, TRANS ("Scissors Tool (5) - click a clip to split it") };
    juce::ComboBox snapBox;
    // what the rulers and the transport readout measure time in: "Bars" or "Seconds"
    juce::TextButton timebaseBtn;
    IconButton followBtn { Icon::Follow, TRANS ("Follow Playhead") };
    IconButton kbdBtn { Icon::Keyboard, TRANS ("Musical Typing (Ctrl+K)") };
    IconButton pluginBtn { Icon::Plugin, TRANS ("Plug-in Manager") };
    IconButton settingsBtn { Icon::Settings, TRANS ("Audio / MIDI Settings") };
    IconButton exportBtn { Icon::Export, TRANS ("Export Mixdown...") };

    NumberLabel tempoLabel;
    juce::TextButton sigBtn;
    juce::Rectangle<int> lcdArea;
    juce::String posText, timeText, cpuText;
    bool lcdShowsSeconds = false;      // the big readout follows the timebase of the ruler
    juce::Label cpuLabel;
    double tempoAtDragStart = 120.0;
};
}
