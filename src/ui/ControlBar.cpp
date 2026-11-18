#include "ControlBar.h"

namespace sx
{
using namespace juce;

ControlBar::ControlBar (AppState& a) : app (a)
{
    for (auto* b : { (Button*) &rewindBtn, (Button*) &stopBtn, (Button*) &playBtn, (Button*) &recBtn, (Button*) &loopBtn,
                     (Button*) &metroBtn, (Button*) &countBtn, (Button*) &undoBtn, (Button*) &redoBtn, (Button*) &mergeBtn,
                     (Button*) &kbdBtn, (Button*) &pluginBtn, (Button*) &settingsBtn, (Button*) &exportBtn, (Button*) &inspectorBtn,
                     (Button*) &mixerBtn, (Button*) &editorBtn })
        addAndMakeVisible (b);

    for (auto* b : { &loopBtn, &metroBtn, &countBtn, &kbdBtn, &inspectorBtn, &mixerBtn, &editorBtn })
        b->setClickingTogglesState (false);
    for (auto* b : { &rewindBtn, &stopBtn, &playBtn, &recBtn, &loopBtn, &metroBtn, &countBtn })
        b->setBackgroundVisible (true);

    rewindBtn.onClick = [this] { cmd (CmdRewind); };
    stopBtn.onClick = [this] { cmd (CmdStop); };
    playBtn.onClick = [this] { cmd (CmdPlayStop); };
    recBtn.onClick = [this] { cmd (CmdRecord); };
    loopBtn.onClick = [this] { cmd (CmdToggleLoop); };
    metroBtn.onClick = [this] { cmd (CmdToggleMetronome); };
    countBtn.onClick = [this] { showCountInMenu(); };
    undoBtn.onClick = [this] { cmd (CmdUndo); };
    redoBtn.onClick = [this] { cmd (CmdRedo); };
    mergeBtn.onClick = [this] { cmd (CmdMergeClips); };

    for (auto* b : { (Button*) &selectBtn, (Button*) &pencilToolBtn, (Button*) &eraserToolBtn, (Button*) &muteToolBtn, (Button*) &scissorsToolBtn })
    {
        addAndMakeVisible (b);
        b->setClickingTogglesState (false);
    }
    selectBtn.onClick = [this] { app.arrangeTool = ArrangeTool::Select; syncToolButtons(); };
    pencilToolBtn.onClick = [this] { app.arrangeTool = ArrangeTool::Pencil; syncToolButtons(); };
    eraserToolBtn.onClick = [this] { app.arrangeTool = ArrangeTool::Eraser; syncToolButtons(); };
    muteToolBtn.onClick = [this] { app.arrangeTool = ArrangeTool::Mute; syncToolButtons(); };
    scissorsToolBtn.onClick = [this] { app.arrangeTool = ArrangeTool::Scissors; syncToolButtons(); };
    kbdBtn.onClick = [this] { cmd (CmdToggleKeyboardTyping); };
    pluginBtn.onClick = [this] { cmd (CmdPluginManager); };
    settingsBtn.onClick = [this] { cmd (CmdAudioSettings); };
    exportBtn.onClick = [this] { cmd (CmdExportMix); };
    inspectorBtn.onClick = [this] { cmd (CmdToggleInspector); };
    mixerBtn.onClick = [this] { cmd (CmdToggleMixer); };
    editorBtn.onClick = [this] { cmd (CmdTogglePianoRoll); };

    tempoLabel.setFont (theme::mono (20.0f, true));
    tempoLabel.setJustificationType (Justification::centred);
    tempoLabel.setEditable (false, true, false);
    tempoLabel.setColour (Label::textColourId, theme::text);
    tempoLabel.setColour (Label::backgroundWhenEditingColourId, theme::white);
    tempoLabel.setColour (Label::textWhenEditingColourId, theme::text);
    tempoLabel.setTooltip (TRANS ("Tempo - drag up/down or double-click to type"));
    tempoLabel.setMouseCursor (MouseCursor::UpDownResizeCursor);
    tempoLabel.onDragStart = [this] { app.project.checkpoint(); };
    tempoLabel.onDrag = [this] (double d)
    {
        // edits apply to the tempo segment the playhead is in
        if (auto* seg = app.project.findTempoSegmentAt (app.engine.getPositionBeats()))
        {
            const Project::TempoEdit keepSeconds (app.project);   // seconds-timebase clips stay put
            seg->bpm = jlimit (20.0, 400.0, std::round ((seg->bpm + d) * 100.0) / 100.0);
            app.project.syncLegacyTempo();
            app.project.sendContentChanged();
        }
    };
    tempoLabel.onTextChange = [this]
    {
        const double v = tempoLabel.getText().getDoubleValue();
        if (v >= 20.0 && v <= 400.0)
        {
            app.project.checkpoint();
            if (auto* seg = app.project.findTempoSegmentAt (app.engine.getPositionBeats()))
            {
                const Project::TempoEdit keepSeconds (app.project);
                seg->bpm = v;
                app.project.syncLegacyTempo();
                app.project.sendContentChanged();
            }
            else
            {
                app.project.setTempo (app.engine.getPositionBeats(), v, 0, 0);
                app.project.sendContentChanged();
            }
        }
        syncToggles();
    };
    addAndMakeVisible (tempoLabel);

    sigBtn.setTooltip (TRANS ("Time Signature"));
    sigBtn.onClick = [this] { showSignatureMenu(); };
    sigBtn.setColour (TextButton::buttonColourId, Colours::transparentBlack);
    sigBtn.setColour (TextButton::textColourOffId, theme::text);
    addAndMakeVisible (sigBtn);

    // timebase of the rulers / readout, next to the snap grid like Studio One's toolbar
    timebaseBtn.setTooltip (TRANS ("Timebase of the ruler: bars or seconds"));
    timebaseBtn.onClick = [this] { showTimebaseMenu(); };
    timebaseBtn.setColour (TextButton::buttonColourId, Colours::transparentBlack);
    timebaseBtn.setColour (TextButton::textColourOffId, theme::textDim);
    syncTimebaseButton();
    addAndMakeVisible (timebaseBtn);

    cpuLabel.setFont (theme::font (11.5f));
    cpuLabel.setColour (Label::textColourId, theme::textDim);
    cpuLabel.setJustificationType (Justification::centredRight);
    addAndMakeVisible (cpuLabel);

    // snap / grid selector for clip editing (pencil, move, resize)
    int sid = 1;
    for (auto& g : gridOptions()) snapBox.addItem (TRANS (g.name), sid++);
    int snapSel = 5;
    for (int i = 0; i < (int) gridOptions().size(); ++i)
        if (std::abs (gridOptions()[(size_t) i].beats - app.arrangeGrid) < 1.0e-9) snapSel = i + 1;
    snapBox.setSelectedId (snapSel, dontSendNotification);
    snapBox.setTooltip (TRANS ("Snap / grid resolution"));
    snapBox.onChange = [this]
    {
        const int i = snapBox.getSelectedId() - 1;
        if (i >= 0 && i < (int) gridOptions().size()) app.arrangeGrid = gridOptions()[(size_t) i].beats;
    };
    addAndMakeVisible (snapBox);

    followBtn.setClickingTogglesState (false);
    followBtn.setToggleState (app.followPlayhead, dontSendNotification);
    followBtn.onClick = [this]
    {
        app.followPlayhead = ! app.followPlayhead;
        followBtn.setToggleState (app.followPlayhead, dontSendNotification);
    };
    addAndMakeVisible (followBtn);

    mediaPoolBtn.setClickingTogglesState (false);
    mediaPoolBtn.onClick = [this] { cmd (CmdToggleMediaPool); };
    addAndMakeVisible (mediaPoolBtn);

    app.project.addListener (this);
    syncToggles();
    syncToolButtons();
    startTimerHz (30);
}

ControlBar::~ControlBar() { app.project.removeListener (this); }

void ControlBar::syncToolButtons()
{
    selectBtn.setToggleState (app.arrangeTool == ArrangeTool::Select, dontSendNotification);
    pencilToolBtn.setToggleState (app.arrangeTool == ArrangeTool::Pencil, dontSendNotification);
    eraserToolBtn.setToggleState (app.arrangeTool == ArrangeTool::Eraser, dontSendNotification);
    muteToolBtn.setToggleState (app.arrangeTool == ArrangeTool::Mute, dontSendNotification);
    scissorsToolBtn.setToggleState (app.arrangeTool == ArrangeTool::Scissors, dontSendNotification);
}

void ControlBar::syncToggles()
{
    auto& p = app.project;
    loopBtn.setToggleState (p.loopOn, dontSendNotification);
    metroBtn.setToggleState (p.metronome, dontSendNotification);
    countBtn.setToggleState (p.countInBars > 0, dontSendNotification);
    kbdBtn.setToggleState (app.musicalTyping, dontSendNotification);
    undoBtn.setEnabled (p.canUndo());
    redoBtn.setEnabled (p.canRedo());
    mergeBtn.setEnabled (app.selectedClips.size() >= 2);
    if (! tempoLabel.isBeingEdited())
    {
        // shows the interpolated bpm during linear ramps too
        const double v = p.bpmAt (app.engine.getPositionBeats());
        tempoLabel.setText (String (v, v == std::floor (v) ? 0 : 2), dontSendNotification);
        int sn, sd;
        p.meterAt (app.engine.getPositionBeats(), sn, sd);
        sigBtn.setButtonText (String (sn) + "/" + String (sd));
    }
}

void ControlBar::syncTimebaseButton()
{
    const String want = app.timeInSeconds ? TRANS ("Seconds") : TRANS ("Bars");
    if (timebaseBtn.getButtonText() != want) timebaseBtn.setButtonText (want);
}

void ControlBar::showTimebaseMenu()
{
    PopupMenu m;
    m.addItem (CmdTimebaseBars, TRANS ("Bars"), true, ! app.timeInSeconds);
    m.addItem (CmdTimebaseSeconds, TRANS ("Seconds"), true, app.timeInSeconds);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&timebaseBtn), [this] (int r)
    {
        if (r > 0) cmd (r);
    });
}

void ControlBar::showCountInMenu()
{
    auto& p = app.project;
    PopupMenu m;
    m.addItem (CmdCountInOff, TRANS ("Off"), true, p.countInBars <= 0);
    m.addItem (CmdCountIn1Bar, TRANS ("1 bar"), true, p.countInBars == 1);
    m.addItem (CmdCountIn2Bars, TRANS ("2 bars"), true, p.countInBars == 2);
    m.addItem (CmdCountIn4Bars, TRANS ("4 bars"), true, p.countInBars == 4);
    m.addItem (CmdCountIn8Bars, TRANS ("8 bars"), true, p.countInBars == 8);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&countBtn), [this] (int r)
    {
        if (r > 0) cmd (r);
    });
}

void ControlBar::showSignatureMenu()
{
    struct S { int n, d; };
    const S sigs[] = { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 }, { 3, 8 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 }, { 2, 2 } };
    PopupMenu m;
    int id = 1;
    const double nowBeat = app.engine.getPositionBeats();
    int curN = app.project.tsNum, curD = app.project.tsDen;
    if (auto* seg = app.project.findTempoSegmentAt (nowBeat)) { curN = seg->tsNum; curD = seg->tsDen; }
    for (auto& s : sigs)
        m.addItem (id++, String (s.n) + "/" + String (s.d), true, curN == s.n && curD == s.d);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&sigBtn), [this, sigs] (int r)
    {
        if (r <= 0) return;
        app.project.checkpoint();
        const double beat = app.engine.getPositionBeats();
        if (auto* seg = app.project.findTempoSegmentAt (beat))
        {
            seg->tsNum = sigs[r - 1].n;
            seg->tsDen = sigs[r - 1].d;
            app.project.syncLegacyTempo();
        }
        else
            app.project.setTempo (beat, 0, sigs[r - 1].n, sigs[r - 1].d);
        app.project.sendContentChanged();
    });
}

void ControlBar::refreshTheme()
{
    tempoLabel.setColour (Label::textColourId, theme::text);
    tempoLabel.setColour (Label::backgroundWhenEditingColourId, theme::white);
    tempoLabel.setColour (Label::textWhenEditingColourId, theme::text);
    sigBtn.setColour (TextButton::buttonColourId, Colours::transparentBlack);
    sigBtn.setColour (TextButton::textColourOffId, theme::text);
    timebaseBtn.setColour (TextButton::buttonColourId, Colours::transparentBlack);
    timebaseBtn.setColour (TextButton::textColourOffId, theme::textDim);
    cpuLabel.setColour (Label::textColourId, theme::textDim);
    repaint();
}

void ControlBar::timerCallback()
{
    auto& e = app.engine;
    auto& p = app.project;
    playBtn.setToggleState (e.isPlaying() && ! e.isRecording(), dontSendNotification);
    recBtn.setToggleState (e.isRecording(), dontSendNotification);
    syncToolButtons();   // the tool can also change from the arrange view (e.g. double-click a clip)
    syncTimebaseButton();
    followBtn.setToggleState (app.followPlayhead, dontSendNotification);

    const double beats = e.getPositionBeats();
    auto pos = barsBeatsString (beats, p);
    const double sec = p.beatsToSeconds (beats);
    const int m = (int) (std::abs (sec) / 60.0);
    const double s = std::abs (sec) - m * 60.0;
    auto time = String (sec < 0 ? "-" : "") + String (m).paddedLeft ('0', 2) + ":" + (s < 10 ? "0" : "") + String (s, 3);
    auto cpu = "CPU " + String ((int) std::round (e.getCpuLoad() * 100.0)) + "%";
    if (app.engine.isMultiCoreEnabled() && app.engine.multiCoreThreads() > 0)
        cpu += " / " + String (app.engine.multiCoreCores()) + "C";
    if (pos != posText || time != timeText)
    {
        posText = pos; timeText = time;
        repaint (lcdArea);
    }
    // the big readout follows the timebase, which can change without the position moving
    if (lcdShowsSeconds != app.timeInSeconds)
    {
        lcdShowsSeconds = app.timeInSeconds;
        repaint (lcdArea);
    }
    if (cpu != cpuText) { cpuText = cpu; cpuLabel.setText (cpu, dontSendNotification); }
    cpuLabel.setTooltip (app.engine.isMultiCoreEnabled()
                         ? TRANS ("Multi-core Rendering") + ": " + String (app.engine.multiCoreCores()) + " " + TRANS ("cores")
                         : TRANS ("Multi-core Rendering") + ": " + TRANS ("Off"));

    // count-in: the button lights up while it is on and says how many bars
    const auto countTip = p.countInBars > 0
                            ? TRANS ("Count-in") + ": " + String (p.countInBars) + " " + (p.countInBars == 1 ? TRANS ("bar") : TRANS ("bars"))
                            : TRANS ("Count-in: off");
    if (countBtn.getTooltip() != countTip) countBtn.setTooltip (countTip);

    // follow the playhead across tempo / meter changes (live value inside linear ramps)
    if (! tempoLabel.isBeingEdited())
    {
        const double v = std::round (p.bpmAt (beats) * 100.0) / 100.0;
        if (tempoLabel.getText().getDoubleValue() != v)
            tempoLabel.setText (String (v, v == std::floor (v) ? 0 : 2), dontSendNotification);
        int sn, sd;
        p.meterAt (beats, sn, sd);
        const auto sig = String (sn) + "/" + String (sd);
        if (sigBtn.getButtonText() != sig) sigBtn.setButtonText (sig);
    }
}

void ControlBar::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::lineSoft);
    g.fillRect (0, getHeight() - 34, getWidth(), 1);   // line between the transport and tools rows
    g.setColour (theme::line);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    // LCD
    auto r = lcdArea.toFloat();
    g.setColour (theme::white);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (theme::line);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

    auto posArea = lcdArea.withWidth (230).reduced (10, 4);
    // the big readout is the chosen timebase, the small line shows the other one
    const String big = app.timeInSeconds ? timeText : posText;
    const String small = app.timeInSeconds ? posText : timeText;
    g.setColour (theme::text);
    g.setFont (theme::mono (22.0f, true));
    g.drawText (big, posArea.removeFromTop (26), Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::mono (11.5f));
    g.drawText (TRANS ("BAR  BEAT DIV  TICK") + "     " + small, posArea, Justification::centredLeft);

    g.setColour (theme::lineSoft);
    g.fillRect (lcdArea.getX() + 232, lcdArea.getY() + 8, 1, lcdArea.getHeight() - 16);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.5f, true));
    g.drawText (TRANS ("TEMPO"), lcdArea.getX() + 238, lcdArea.getY() + 3, 80, 12, Justification::centredLeft);
    g.drawText (TRANS ("SIGNATURE"), lcdArea.getX() + 326, lcdArea.getY() + 3, 80, 12, Justification::centredLeft);
    g.setColour (theme::lineSoft);
    g.fillRect (lcdArea.getX() + 320, lcdArea.getY() + 8, 1, lcdArea.getHeight() - 16);

    g.setColour (theme::text);
    g.setFont (theme::font (15.0f, true));
    g.drawText ("SoundX", 14, 0, 80, getHeight(), Justification::centredLeft);
}

void ControlBar::resized()
{
    auto r = getLocalBounds().reduced (8, 7);
    auto tools = r.removeFromBottom (26);   // second row: arrange tools + snap
    r.removeFromBottom (2);
    tools.removeFromTop (2);                // nudge the tool row down a touch
    r.removeFromLeft (84);
    const int bh = r.getHeight();

    auto place = [&] (Component& c, int w = 0)
    {
        c.setBounds (r.removeFromLeft (w > 0 ? w : bh));
        r.removeFromLeft (4);
    };
    place (inspectorBtn);
    r.removeFromLeft (6);
    place (undoBtn); place (redoBtn); place (mergeBtn);
    r.removeFromLeft (8);
    place (rewindBtn); place (stopBtn); place (playBtn); place (recBtn);
    r.removeFromLeft (6);
    place (loopBtn); place (metroBtn); place (countBtn);
    r.removeFromLeft (8);

    lcdArea = r.removeFromLeft (440);
    tempoLabel.setBounds (lcdArea.getX() + 236, lcdArea.getY() + 14, 84, lcdArea.getHeight() - 16);
    sigBtn.setBounds (lcdArea.getX() + 328, lcdArea.getY() + 16, 100, lcdArea.getHeight() - 20);

    // right-aligned group shares the transport row only (above the tools row)
    auto grp = r;
    cpuLabel.setBounds (grp.removeFromRight (78));
    exportBtn.setBounds (grp.removeFromRight (bh)); grp.removeFromRight (4);
    settingsBtn.setBounds (grp.removeFromRight (bh)); grp.removeFromRight (4);
    pluginBtn.setBounds (grp.removeFromRight (bh)); grp.removeFromRight (4);
    kbdBtn.setBounds (grp.removeFromRight (bh)); grp.removeFromRight (6);
    editorBtn.setBounds (grp.removeFromRight (bh)); grp.removeFromRight (4);
    mixerBtn.setBounds (grp.removeFromRight (bh));

    // arrange tools row: line it up with the transport buttons above
    tools.removeFromLeft (84);
    selectBtn.setBounds (tools.removeFromLeft (26)); tools.removeFromLeft (2);
    pencilToolBtn.setBounds (tools.removeFromLeft (26)); tools.removeFromLeft (2);
    eraserToolBtn.setBounds (tools.removeFromLeft (26)); tools.removeFromLeft (2);
    muteToolBtn.setBounds (tools.removeFromLeft (26)); tools.removeFromLeft (2);
    scissorsToolBtn.setBounds (tools.removeFromLeft (26)); tools.removeFromLeft (10);
    snapBox.setBounds (tools.removeFromLeft (80)); tools.removeFromLeft (6);
    timebaseBtn.setBounds (tools.removeFromLeft (68)); tools.removeFromLeft (6);
    followBtn.setBounds (tools.removeFromLeft (26)); tools.removeFromLeft (4);
    mediaPoolBtn.setBounds (tools.removeFromLeft (26));
}
}
