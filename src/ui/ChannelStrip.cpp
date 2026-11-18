#include "ChannelStrip.h"
#include "SlotMenus.h"
#include "Importer.h"       // promptText: the exact send level can be typed in

namespace sx
{
using namespace juce;

// "SX Synth · 延迟 960 采样 (21.8 ms) · 已自动补偿": the plug-in's reported latency, so the user
// can see what the mixer is compensating for (a linear-phase EQ, a lookahead limiter ...)
String slotTooltip (AppState& app, const PluginSlot& s)
{
    auto inst = app.engine.getInstance (s.uid);
    const int lat = inst != nullptr ? inst->latencySamples.load() : 0;
    String tip = s.desc.name;
    if (lat > 0)
    {
        const double ms = 1000.0 * (double) lat / jmax (1.0, app.engine.getSampleRate());
        tip << "  ·  " << TRANS ("latency") << " " << lat << " " << TRANS ("samples")
            << " (" << String (ms, 1) << " ms)  ·  " << TRANS ("compensated automatically");
    }
    return tip;
}

// pan is shown (and typed) as a number: "L 32" / "C" / "R 20" on a -100..+100 scale
String panText (double normalised)
{
    const int p = (int) std::round (normalised * 100.0);
    if (p == 0) return String ("C");
    return (p < 0 ? String ("L ") : String ("R ")) + String (std::abs (p));
}

bool parsePanText (String s, double& out)
{
    s = s.trim().toUpperCase();
    if (s.isEmpty()) return false;
    if (s == "C" || s == "CENTER" || s == "CENTRE") { out = 0.0; return true; }

    // "L30" / "R30" give the side explicitly; a bare number keeps its own sign (-30 = left)
    double dir = 1.0;
    bool hasSide = true;
    if (s.startsWithChar ('L'))      { dir = -1.0; s = s.substring (1).trim(); }
    else if (s.startsWithChar ('R')) { dir =  1.0; s = s.substring (1).trim(); }
    else                             { hasSide = false; }

    s = s.replaceCharacter (',', '.');
    if (! s.containsAnyOf ("0123456789")) return false;

    const double v = hasSide ? std::abs (s.getDoubleValue()) : s.getDoubleValue();
    out = jlimit (-1.0, 1.0, dir * v / 100.0);
    return true;
}

static String dbText (float norm)
{
    const float g = faderToGain (norm);
    if (g <= 0.0005f) return "-inf";
    return String (Decibels::gainToDecibels (g), 1) + " dB";
}

static String shortDb (float gain)
{
    if (gain <= 0.0005f) return "-inf";
    return String (Decibels::gainToDecibels (gain), 1);
}

ChannelStrip::ChannelStrip (AppState& a, int id) : app (a), trackId (id)
{
    nameLabel.setFont (theme::font (13.0f, true));
    nameLabel.setJustificationType (Justification::centred);
    nameLabel.setEditable (false, id >= 0, false);
    nameLabel.onTextChange = [this]
    {
        if (auto* t = track())
        {
            app.project.checkpoint();
            t->name = nameLabel.getText().isEmpty() ? t->name : nameLabel.getText();
            app.project.sendMixChanged();
        }
    };
    nameLabel.addMouseListener (this, false);
    addAndMakeVisible (nameLabel);

    outputRow.setContent ("OUT", TRANS ("Main"), theme::textDim);
    outputRow.onMenu = [this] { showOutputMenu(); };
    addAndMakeVisible (outputRow);

    addSendRow.setContent ("SND", "+ " + TRANS ("Send"), theme::sendTrackColour);
    addSendRow.onMenu = [this] { showAddSendMenu(); };
    addAndMakeVisible (addSendRow);

    addAndMakeVisible (slotButton);
    phaseBtn.onClick = [this]
    {
        app.project.checkpoint();
        if (trackId == -1) app.project.masterTrack.phaseInvert = phaseBtn.getToggleState();
        else if (auto* t = track()) t->phaseInvert = phaseBtn.getToggleState();
        app.project.sendMixChanged();
        app.project.sendContentChanged();
    };
    addAndMakeVisible (phaseBtn);
    for (auto& b : inserts) addAndMakeVisible (b);
    for (auto& b : sendRows) { addAndMakeVisible (b); b.setVisible (false); }

    pan.setRange (-1.0, 1.0, 0.0);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setTooltip (TRANS ("Pan"));
    pan.setPopupDisplayEnabled (true, false, nullptr);
    pan.setScrollWheelEnabled (true);     // wheel changes pan and writes the automation lane
    pan.textFromValueFunction = [] (double v) { return panText (v); };
    pan.onDragStart = [this]
    {
        app.project.checkpoint();
        app.engine.touchBegin (trackId, "pan");
    };
    pan.onValueChange = [this] { panChanged(); setPanText (pan.getValue()); };
    pan.onDragEnd = [this] { app.engine.touchEnd (trackId, "pan"); };
    addAndMakeVisible (pan);

    // the pan value as a number: readable at a glance and typable ("L30", "R30", "0")
    panLabel.setFont (theme::font (12.0f));
    panLabel.setJustificationType (Justification::centred);
    panLabel.setColour (Label::textColourId, theme::text);
    panLabel.setEditable (true, true, false);
    panLabel.setTooltip (TRANS ("Click to type a pan value (L30 / R30 / 0)"));
    panLabel.onTextChange = [this] { commitPanText(); };
    panLabel.addMouseListener (this, false);      // a plain click opens the editor
    addAndMakeVisible (panLabel);

    fader.setRange (0.0, 1.0, 0.0);
    fader.setDoubleClickReturnValue (true, 0.75);
    fader.setTooltip (TRANS ("Volume"));
    fader.setScrollWheelEnabled (true);    // the wheel adjusts the fader; dragging is left-button only
    fader.onDragStart = [this]
    {
        app.project.checkpoint();
        if (trackId >= 0) app.engine.touchBegin (trackId, "vol");
    };
    fader.onValueChange = [this] { faderChanged(); };
    fader.onDragEnd = [this] { if (trackId >= 0) app.engine.touchEnd (trackId, "vol"); };
    addAndMakeVisible (fader);

    dbLabel.setFont (theme::font (11.5f));
    dbLabel.setJustificationType (Justification::centred);
    dbLabel.setColour (Label::textColourId, theme::textDim);
    dbLabel.setEditable (false, true, false);   // double-click to type a dB value
    dbLabel.onTextChange = [this] { commitVolumeText(); };
    dbLabel.setTooltip (TRANS ("Volume (dB) - double-click to type"));
    addAndMakeVisible (dbLabel);

    addAndMakeVisible (meter);
    meter.onReset = [this] { app.meters.resetMaximum (trackId); };
    addAndMakeVisible (panMeter);
    addChildComponent (scope);        // only the master strip shows it

    monoBtn.onClick = [this]
    {
        if (auto* t = channel())
        {
            app.project.checkpoint();
            t->mono = monoBtn.getToggleState();
            app.project.sendMixChanged();
            app.project.sendContentChanged();
        }
    };
    addChildComponent (monoBtn);
    muteBtn.onClick = [this]
    {
        if (auto* t = channel())
        {
            app.project.checkpoint();
            t->mute = muteBtn.getToggleState();
            if (t->mute) t->solo = false;          // M and S are mutually exclusive
            app.project.sendMixChanged();
            app.project.sendContentChanged();
        }
    };
    soloBtn.onClick = [this]
    {
        if (auto* t = channel())
        {
            app.project.checkpoint();
            t->solo = soloBtn.getToggleState();
            if (t->solo) t->mute = false;          // S wins over M on the same channel
            app.project.sendMixChanged();
            app.project.sendContentChanged();
        }
    };
    armBtn.onClick = [this]
    {
        if (auto* t = track()) { t->arm = armBtn.getToggleState(); app.project.sendMixChanged(); }
    };
    monBtn.onClick = [this]
    {
        if (auto* t = track()) { t->monitor = monBtn.getToggleState(); app.project.sendMixChanged(); }
    };
    addAndMakeVisible (muteBtn);
    addAndMakeVisible (soloBtn);
    // the listen bus gets its own on / off switch in the button row
    listenBtn.onClick = [this]
    {
        if (auto* t = track())
        {
            app.project.checkpoint();
            t->listenEnabled = listenBtn.getToggleState();
            app.project.sendMixChanged();
            app.engine.syncAll();
            repaint();
        }
    };
    addAndMakeVisible (listenBtn);
    listenBtn.setVisible (false);

    // per-track listen: monitor this channel through the listen bus (Studio One style)
    listenSoloBtn.onClick = [this]
    {
        if (auto* t = track())
        {
            app.project.checkpoint();
            t->listenSolo = listenSoloBtn.getToggleState();
            app.project.sendMixChanged();
            repaint();
        }
    };
    addAndMakeVisible (listenSoloBtn);
    listenSoloBtn.setVisible (false);
    addAndMakeVisible (armBtn);
    addAndMakeVisible (monBtn);

    autoBtn.setClickingTogglesState (false);
    autoBtn.setTooltip (TRANS ("Automation mode: Off / Read / Touch (record while moving controls)"));
    autoBtn.onClick = [this]
    {
        if (auto* t = channel())
        {
            app.project.checkpoint();
            t->autoMode = t->autoMode == AutoMode::Off ? AutoMode::Read : t->autoMode == AutoMode::Read ? AutoMode::Touch : AutoMode::Off;
            app.project.sendContentChanged();
        }
    };
    autoBtn.addMouseListener (this, false);
    addAndMakeVisible (autoBtn);

    app.project.addListener (this);
    refresh();
    startTimerHz (30);
}

ChannelStrip::~ChannelStrip() { app.project.removeListener (this); }

void ChannelStrip::setTrackId (int id)
{
    if (trackId == id) return;
    trackId = id;
    nameLabel.setEditable (false, id >= 0, false);
    refresh();
}

void ChannelStrip::refreshTheme()
{
    panLabel.setColour (Label::textColourId, theme::text);
    dbLabel.setColour (Label::textColourId, theme::textDim);
    updateAutoButton();
    repaint();
}

void ChannelStrip::updateAutoButton()
{
    auto* t = channel();
    autoBtn.setVisible (t != nullptr && autoButtonAllowed);
    if (t == nullptr) return;
    const char* names[] = { "Off", "Read", "Touch" };
    autoBtn.setButtonText (TRANS ("Auto") + ": " + TRANS (names[(int) t->autoMode]));
    autoBtn.setColour (TextButton::buttonColourId, t->autoMode == AutoMode::Touch ? theme::record.withAlpha (0.15f)
                                                    : t->autoMode == AutoMode::Read ? theme::accentSoft : theme::white);
    autoBtn.setColour (TextButton::textColourOffId, t->autoMode == AutoMode::Touch ? theme::record
                                                     : t->autoMode == AutoMode::Read ? theme::accentDark : theme::textDim);
}

void ChannelStrip::refresh()
{
    updating = true;
    auto* t = track();
    const bool master = trackId == -1;

    if (master)
    {
        nameLabel.setText (TRANS ("Master"), dontSendNotification);
        outputRow.setVisible (false);
        nameLabel.setTooltip ({ });
        slotButton.setVisible (false);
        pan.setVisible (false);
    panLabel.setVisible (false);
        // the master is a channel like any other: mute switch + automation mode, same as a track
        fader.setEnabled (true);
        muteBtn.setVisible (true); soloBtn.setVisible (false); armBtn.setVisible (false); monBtn.setVisible (false);
        phaseBtn.setVisible (false);   // the master bus has no polarity switch
        phaseBtn.setToggleState (app.project.masterTrack.phaseInvert, dontSendNotification);
        phaseBtn.setTooltip (TRANS ("Invert Phase") + " (Ø)");
        muteBtn.setToggleState (app.project.masterTrack.mute, dontSendNotification);
        muteBtn.setTooltip (TRANS ("Mute the master bus (the whole mix)"));
        monoBtn.setVisible (true);
        monoBtn.setToggleState (app.project.masterTrack.mono, dontSendNotification);
        monoBtn.setTooltip (TRANS ("Mono: fold the main output to mono (phase check)"));
        autoBtn.setVisible (autoButtonAllowed);
        updateAutoButton();
        if (! fader.isMouseButtonDown())
            fader.setValue (app.project.masterTrack.volumeDb <= -99.0f ? 0.0 : gainToFader (Decibels::decibelsToGain (app.project.masterTrack.volumeDb)), dontSendNotification);
        setDbText (dbText ((float) fader.getValue()));
    }
    else if (t == nullptr)
    {
        nameLabel.setText ("", dontSendNotification);
        outputRow.setVisible (false);
        nameLabel.setTooltip ({ });
        slotButton.setVisible (false);
        for (auto& b : inserts) b.setSlot (nullptr);
        pan.setVisible (false);
    panLabel.setVisible (false);
        muteBtn.setVisible (false); soloBtn.setVisible (false); armBtn.setVisible (false); monBtn.setVisible (false);
        phaseBtn.setVisible (false);
        monoBtn.setVisible (false);
        autoBtn.setVisible (false);
        fader.setEnabled (false);
        setDbText ("");
        updating = false;
        applyTierVisibility();
        repaint();
        return;
    }
    else
    {
        fader.setEnabled (true);
        nameLabel.setText (t->name, dontSendNotification);

        // Studio One style: an OUT row showing the bus (or Main) plus one SND row per send
        String outName = TRANS ("Main");
        Colour outTint = theme::textDim;
        if (t->outputBusId != 0)
        {
            if (auto* out = app.project.findTrack (t->outputBusId)) outName = out->name;
            else outName = "#" + String (t->outputBusId);
            outTint = theme::busTrackColour;
        }

        StringArray sendNames;
        for (auto& s : t->sends)
            if (auto* st = app.project.findTrack (s.target)) sendNames.add (st->name);
            else sendNames.add ("#" + String (s.target));

        outputRow.setContent ("OUT", outName, outTint);
        // an effect return (send track) is parallel and always feeds the main output: an OUT
        // routing row would have nothing to choose. It used to be shown here by refresh() and
        // hidden again by resized(), so a mix change could leave a stale row floating over the
        // pan / fader of the strip. Keep it hidden for sends (and for automation tracks, which
        // are not audio channels at all).
        outputRow.setVisible (! t->isSend() && ! t->isAutomation());

        String routeText = TRANS ("Output") + ": " + outName;
        if (! sendNames.isEmpty()) routeText << "   " << TRANS ("Sends") << ": " << sendNames.joinIntoString (", ");
        nameLabel.setTooltip (routeText);

        slotButton.setVisible (! t->isGroupTrack());
        pan.setVisible (true);
        panLabel.setVisible (true);
        const bool monitorBus = t->isListenBus;   // the listen bus is the last stage: no M/S/R
        monitorBusStrip = monitorBus;
        if (monitorBus)
        {
            // the monitor bus has no pan, no automation and no output routing at all
            pan.setVisible (false); panLabel.setVisible (false);
            autoBtn.setVisible (false); outputRow.setVisible (false);
        }
        else if (! autoButtonAllowed)
        {
            autoBtn.setVisible (false);      // the inspector column: automation lives in the mixer
        }
        muteBtn.setVisible (! monitorBus); soloBtn.setVisible (! monitorBus); armBtn.setVisible (! monitorBus);
        phaseBtn.setVisible (! monitorBus && ! t->isGroupTrack());   // buses / returns: no Ø
        phaseBtn.setToggleState (t->phaseInvert, dontSendNotification);
        phaseBtn.setTooltip (TRANS ("Invert Phase") + " (Ø)");
        monoBtn.setVisible (false);                 // the mono fold belongs to the main output
        const bool wasMon = monBtn.isVisible();
        monBtn.setVisible (! monitorBus && t->type == TrackType::Audio);
        if (wasMon != monBtn.isVisible()) resized();
        muteBtn.setToggleState (t->mute, dontSendNotification);
        soloBtn.setToggleState (t->solo, dontSendNotification);
        armBtn.setToggleState (t->arm, dontSendNotification);
        monBtn.setToggleState (t->monitor, dontSendNotification);
        const bool wasListen = listenBtn.isVisible();
        listenBtn.setVisible (t->isListenBus);
        const bool wasListenSolo = listenSoloBtn.isVisible();
        listenSoloBtn.setVisible (! t->isListenBus && ! t->isGroupTrack());
        if (wasListenSolo != listenSoloBtn.isVisible()) resized();
        listenSoloBtn.setToggleState (t->listenSolo, dontSendNotification);
        listenBtn.setToggleState (t->listenEnabled, dontSendNotification);
        if (wasListen != listenBtn.isVisible()) resized();
        if (! pan.isMouseButtonDown()) pan.setValue (t->pan, dontSendNotification);
        if (! panLabel.isBeingEdited()) setPanText (t->pan);
        if (! fader.isMouseButtonDown())
            fader.setValue (t->volumeDb <= -99.0f ? 0.0 : gainToFader (Decibels::decibelsToGain (t->volumeDb)), dontSendNotification);
        setDbText (dbText ((float) fader.getValue()));

        const int tid = trackId;
        if (t->isGroupTrack())
        {
            slotButton.setSlot (nullptr);
        }
        else if (t->type == TrackType::Instrument)
        {
            if (t->hasInstrument)
            {
                const auto uid = t->instrument.uid;
                slotButton.setSlot (&t->instrument, (int) t->extraInstruments.size());
                slotButton.setTooltip (slotTooltip (app, t->instrument));
                slotButton.dragSource = [this, tid] () -> var
                {
                    auto* tr = app.project.findTrack (tid);
                    var d (new DynamicObject());
                    d.getDynamicObject()->setProperty ("sxSlot", true);
                    d.getDynamicObject()->setProperty ("track", tid);
                    d.getDynamicObject()->setProperty ("uid", tr != nullptr ? tr->instrument.uid : (juce::int64) 0);
                    d.getDynamicObject()->setProperty ("instrument", true);
                    return d;
                };
                slotButton.onOpen = [this, uid] { app.showPluginEditor (uid); };
                slotButton.onBypass = [this, tid, uid]
                {
                    if (auto* tr = app.project.findTrack (tid))
                        app.engine.setSlotBypass (tid, uid, ! tr->instrument.bypass);
                };
                slotButton.onMenu = [this, tid, uid] { showSlotMenu (app, tid, uid, true, &slotButton); };
            }
            else
            {
                slotButton.setSlot (nullptr);
                slotButton.setPlaceholder (TRANS ("Instrument"));
                slotButton.onMenu = [this, tid] { showSlotMenu (app, tid, 0, true, &slotButton); };
            }
        }
        else
        {
            slotButton.setSlot (nullptr);
            String in = t->inputMono ? TRANS ("Input") + " " + String (t->inputChannel + 1)
                                     : TRANS ("Input") + " " + String (t->inputChannel + 1) + "+" + String (t->inputChannel + 2);
            slotButton.setPlaceholder (in);
            slotButton.onMenu = [this] { showInputMenu(); };
        }
        updateAutoButton();
    }

    const auto& vec = master ? app.project.masterTrack.inserts : (t != nullptr ? t->inserts : app.project.masterTrack.inserts);
    for (int i = 0; i < kInserts; ++i)
    {
        auto& b = inserts[i];
        if (i < (int) vec.size() && (master || t != nullptr))
        {
            const auto uid = vec[(size_t) i].uid;
            const int tid = trackId;
            b.setSlot (&vec[(size_t) i]);
            b.setTooltip (slotTooltip (app, vec[(size_t) i]));
            {
                const auto suid = vec[(size_t) i].uid;
                const int stid = trackId;
                b.dragSource = [this, stid, suid] () -> var
                {
                    var d (new DynamicObject());
                    d.getDynamicObject()->setProperty ("sxSlot", true);
                    d.getDynamicObject()->setProperty ("track", stid);
                    d.getDynamicObject()->setProperty ("uid", suid);
                    d.getDynamicObject()->setProperty ("instrument", false);
                    return d;
                };
            }
            b.onOpen = [this, uid] { app.showPluginEditor (uid); };
            b.onBypass = [this, tid, uid]
            {
                PluginSlot* s = nullptr;
                if (auto* tr = app.project.findTrack (tid)) s = tr->findSlot (uid);
                else for (auto& x : app.project.masterTrack.inserts) if (x.uid == uid) s = &x;
                if (s != nullptr) app.engine.setSlotBypass (tid, uid, ! s->bypass);
            };
            b.onMenu = [this, tid, uid, &b] { showSlotMenu (app, tid, uid, false, &b); };
        }
        else
        {
            b.setSlot (nullptr);
            b.dragSource = nullptr;
            b.setPlaceholder (i == (int) vec.size() ? "+" : "");
            const int tid = trackId;
            b.onMenu = [this, tid, &b] { showSlotMenu (app, tid, 0, false, &b); };
        }
    }

    // sends into buses / effect returns: pre-fader sends are marked "PRE" in the tooltip
    for (int i = 0; i < kSends; ++i)
    {
        auto& b = sendRows[i];
        b.onMenu = {};
        if (t != nullptr && i < (int) t->sends.size())
        {
            const auto& sn = t->sends[(size_t) i];
            auto* tgt = app.project.findTrack (sn.target);
            const String name = tgt != nullptr ? tgt->name : ("#" + String (sn.target));
            const float norm = sn.levelDb <= -99.0f ? 0.0f : jlimit (0.0f, 1.0f, (sn.levelDb + 60.0f) / 66.0f);
            b.setContent (sn.preFader ? TRANS ("PRE") : TRANS ("SND"), name, theme::sendTrackColour, norm);
            b.setLevel (norm);
            const int to = sn.target;
            b.onMenu = [this, to] { showSendMenu (trackId, to); };
            // drag the row up / down to set any send level (the menu still offers presets and
            // an exact dB entry); a drag is one undo step
            b.onLevelDragStart = [this] { app.project.checkpoint(); };
            b.onLevelDrag = [this, to] (float level01)
            {
                if (auto* tr = app.project.findTrack (trackId))
                    for (auto& s : tr->sends)
                        if (s.target == to)
                        {
                            s.levelDb = level01 <= 0.0f ? -99.0f : level01 * 66.0f - 60.0f;
                            app.project.sendContentChanged();
                            return;
                        }
            };
        }
        else
        {
            b.onLevelDrag = nullptr;
            b.onLevelDragStart = nullptr;
            b.setContent ("SND", {}, theme::textDim, -1.0f);
        }
    }

    updating = false;
    applyTierVisibility();      // a mix notification must not re-show a dropped tier control
    repaint();
}

void ChannelStrip::showSendMenu (int fromTrackId, int targetId)
{
    auto* t = app.project.findTrack (fromTrackId);
    if (t == nullptr) return;
    const SendSlot* sn = nullptr;
    for (auto& s : t->sends) if (s.target == targetId) sn = &s;
    if (sn == nullptr) return;
    auto* target = app.project.findTrack (targetId);

    PopupMenu m;
    const String levelText = sn->levelDb <= -99.0f ? TRANS ("Off") : String (sn->levelDb, 1) + " dB";
    m.addSectionHeader ((sn->preFader ? TRANS ("Pre-fader send") : TRANS ("Post-fader send"))
                        + String (" -> ") + (target != nullptr ? target->name : String (targetId))
                        + "  (" + levelText + ")");
    static const float presets[] = { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -99.0f };
    for (int i = 0; i < 6; ++i)
    {
        const float db = presets[i];
        m.addItem (200 + i, db <= -99.0f ? TRANS ("Off") : String (db, 1) + " dB", true, std::abs (sn->levelDb - db) < 0.05f);
    }
    m.addItem (20, TRANS ("Set Level..."));
    m.addSeparator();
    m.addItem (10, TRANS ("Pre-fader"), true, sn->preFader);
    m.addItem (11, TRANS ("Remove Send"));

    auto* a = &app;
    const String name = t->name;
    juce::ignoreUnused (name);
    const float currentDb = sn->levelDb <= -99.0f ? -60.0f : jlimit (-60.0f, 6.0f, sn->levelDb);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (this), [a, fromTrackId, targetId, currentDb] (int r)
    {
        if (r >= 200 && r < 206)
        {
            static const float p[] = { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -99.0f };
            a->project.checkpoint();
            a->setSendLevel (fromTrackId, targetId, p[r - 200]);
        }
        else if (r == 20)
        {
            promptText (TRANS ("Send Level"), TRANS ("Level in dB (-60 ... +6; -inf = off)"),
                        String (currentDb, 1), [a, fromTrackId, targetId] (String s)
            {
                s = s.trim().toLowerCase();
                const bool off = s == "off" || s == "-inf" || s == "inf" || s == "-infinity";
                const double db = off ? -99.0 : jlimit (-60.0, 6.0, s.getDoubleValue());
                a->project.checkpoint();
                a->setSendLevel (fromTrackId, targetId, (float) db);
                a->project.sendContentChanged();
                a->project.sendMixChanged();
            });
            return;
        }
        else if (r == 10)
        {
            bool pre = false;
            if (auto* tr = a->project.findTrack (fromTrackId))
                for (auto& s : tr->sends) if (s.target == targetId) pre = s.preFader;
            a->setSendPre (fromTrackId, targetId, ! pre);
        }
        else if (r == 11)
            a->removeSend (fromTrackId, targetId);
        a->project.sendContentChanged();
        a->project.sendMixChanged();
    });
}

void ChannelStrip::showOutputMenu()
{
    auto* t = track();
    if (t == nullptr) return;

    PopupMenu m;
    m.addSectionHeader (TRANS ("Output"));
    m.addItem (1, TRANS ("Main"), true, t->outputBusId == 0);

    std::vector<std::pair<int, String>> buses;
    for (auto& o : app.project.tracks)
        if (o->isBus() && o->id != trackId)
            buses.push_back ({ o->id, o->name });
    for (size_t i = 0; i < buses.size(); ++i)
        m.addItem (100 + (int) i, buses[i].second, true, t->outputBusId == buses[i].first);
    if (buses.empty()) m.addItem (2, TRANS ("No bus or effect send track yet"), false);
    m.addSeparator();
    m.addItem (3, TRANS ("New Bus Track"));

    auto* a = &app;
    const int tid = trackId;
    const auto list = buses;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&outputRow), [a, tid, list] (int r)
    {
        if (r == 3) { a->addTrack (TrackType::Bus); return; }
        auto* tr = a->project.findTrack (tid);
        if (tr == nullptr) return;
        if (r == 1)
        {
            a->project.checkpoint();
            tr->outputBusId = 0;
            a->project.sendContentChanged();
        }
        else if (r >= 100 && r - 100 < (int) list.size())
        {
            a->project.checkpoint();
            tr->outputBusId = list[(size_t) (r - 100)].first;
            a->project.sendContentChanged();
        }
    });
}

void ChannelStrip::showAddSendMenu()
{
    auto* t = track();
    if (t == nullptr) return;

    PopupMenu m;
    m.addSectionHeader (TRANS ("Sends"));
    std::vector<std::pair<int, String>> targets;
    for (auto& o : app.project.tracks)
        if (o->isGroupTrack() && o->id != trackId)
        {
            bool has = false;
            for (auto& s : t->sends) has = has || s.target == o->id;
            if (! has) targets.push_back ({ o->id, o->name });
        }
    for (size_t i = 0; i < targets.size(); ++i)
        m.addItem (100 + (int) i, targets[i].second);
    if (targets.empty())
    {
        m.addItem (-1, TRANS ("No bus or effect send track yet"), false);
        m.addItem (2, TRANS ("New Effect Send Track"));
    }

    auto* a = &app;
    const int tid = trackId;
    const auto list = targets;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&addSendRow), [a, tid, list] (int r)
    {
        if (r == 2) { a->addSendTrack(); return; }
        if (r >= 100 && r - 100 < (int) list.size())
            a->addSend (tid, list[(size_t) (r - 100)].first);
    });
}

void ChannelStrip::setDbText (const juce::String& s)
{
    if (! dbLabel.isBeingEdited()) dbLabel.setText (s, dontSendNotification);
}

void ChannelStrip::commitVolumeText()
{
    if (updating) return;
    const String s = dbLabel.getText().trim();
    if (s.isEmpty()) { refresh(); return; }
    float db = -100.0f;
    if (s.equalsIgnoreCase ("-inf") || s.equalsIgnoreCase ("-infdb") || s.equalsIgnoreCase ("off"))
        db = -100.0f;
    else
    {
        if (! s.containsOnly ("-+0123456789.")) { refresh(); return; }
        db = jlimit (-100.0f, 12.0f, (float) s.getDoubleValue());
    }
    app.project.checkpoint();
    if (trackId == -1) app.project.masterTrack.volumeDb = db;
    else if (auto* t = track()) t->volumeDb = db;
    app.project.sendMixChanged();
    refresh();
}

void ChannelStrip::faderChanged()
{
    if (updating) return;
    const float v = (float) fader.getValue();
    const float g = faderToGain (v);
    const float db = g <= 0.0005f ? -100.0f : Decibels::gainToDecibels (g);
    if (trackId == -1)
    {
        app.project.masterTrack.volumeDb = db;
    }
    else if (auto* t = track())
    {
        t->volumeDb = db;
        app.engine.touchValueAuto (trackId, "vol", v);
    }
    app.project.sendMixChanged();
    setDbText (dbText (v));
}

void ChannelStrip::setPanText (double normalised)
{
    panLabel.setText (panText (normalised), dontSendNotification);
}

void ChannelStrip::commitPanText()
{
    double v = 0.0;
    if (! parsePanText (panLabel.getText(), v))
    {
        setPanText (pan.getValue());     // unreadable input: snap back to the real value
        return;
    }

    app.project.checkpoint();
    pan.setValue (v, sendNotificationSync);   // panChanged() applies it to the track
    setPanText (pan.getValue());
}

void ChannelStrip::panChanged()
{
    if (updating) return;
    if (auto* t = track())
    {
        t->pan = (float) pan.getValue();
        app.engine.touchValueAuto (trackId, "pan", (t->pan + 1.0f) * 0.5f);
        app.project.sendMixChanged();
    }
}

void ChannelStrip::timerCallback()
{
    if (! isShowing()) return;
    const auto& m = app.meters.get (trackId);
    meter.setState (m.levelL, m.levelR, m.rmsL, m.rmsR, m.holdL, m.holdR, m.maxL, m.maxR, m.clipped);
    meter.setTooltip (TRANS ("Level") + " " + shortDb (jmax (m.levelL, m.levelR)) + " dB   "
                      + TRANS ("Peak") + " " + shortDb (jmax (m.holdL, m.holdR)) + " dB   "
                      + TRANS ("Max") + " " + shortDb (jmax (m.maxL, m.maxR)) + " dB");
    if (auto* t2 = track())
        panMeter.setLevels (m.levelL, m.levelR, t2->pan);
    if (trackId < 0 && scope.isVisible())
    {
        float scopeData[2 * 256] {};
        app.engine.readScope (scopeData, 256);
        scope.setSamples (scopeData, 256);
    }

    if (auto* t = track())
    {
        if (t->autoMode != AutoMode::Off)
        {
            const float av = app.engine.getAutoVolNorm (trackId);
            if (av >= 0.0f && ! fader.isMouseButtonDown() && std::abs ((float) fader.getValue() - av) > 0.002f)
            {
                updating = true;
                fader.setValue (av, dontSendNotification);
                setDbText (dbText (av));
                updating = false;
            }
            const float ap = app.engine.getAutoPanNorm (trackId);
            if (ap >= 0.0f && ! pan.isMouseButtonDown() && std::abs ((float) pan.getValue() - ap) > 0.002f)
            {
                updating = true;
                pan.setValue (ap, dontSendNotification);
                updating = false;
            }
        }
    }
}

void ChannelStrip::showInputMenu()
{
    auto* t = track();
    auto* dev = app.engine.deviceManager.getCurrentAudioDevice();
    if (t == nullptr || dev == nullptr) return;

    auto names = dev->getInputChannelNames();
    auto active = dev->getActiveInputChannels();
    StringArray act;
    for (int i = 0; i < names.size(); ++i) if (active[i]) act.add (names[i]);

    PopupMenu m;
    if (act.isEmpty()) m.addItem (-1, TRANS ("No input channels active"), false);
    for (int i = 0; i < act.size(); ++i)
        m.addItem (100 + i, act[i] + " (" + TRANS ("mono") + ")", true, t->inputMono && t->inputChannel == i);
    m.addSeparator();
    for (int i = 0; i + 1 < act.size(); i += 2)
        m.addItem (200 + i, act[i] + " + " + act[i + 1] + " (" + TRANS ("stereo") + ")", true, ! t->inputMono && t->inputChannel == i);

    const int tid = trackId;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&slotButton), [this, tid] (int r)
    {
        auto* tr = app.project.findTrack (tid);
        if (tr == nullptr || r < 100) return;
        app.project.checkpoint();
        tr->inputMono = r < 200;
        tr->inputChannel = r < 200 ? r - 100 : r - 200;
        app.project.sendMixChanged();
    });
}

void ChannelStrip::showNameMenu()
{
    if (trackId < 0) return;
    if (trackId < 0) { showMasterMenu(); return; }
    showTrackMenu (app, trackId, &nameLabel, [this] { nameLabel.showEditor(); });
}

void ChannelStrip::showMasterMenu()
{
    PopupMenu m;
    auto* lb = app.project.listenBus();
    m.addItem (1, TRANS ("Add Listen Bus"), lb == nullptr);
    m.addItem (3, TRANS ("Listen Bus On"), lb != nullptr && lb->listenEnabled);
    m.addItem (4, TRANS ("Listen Bus Off"), lb != nullptr && ! lb->listenEnabled);
    m.addItem (2, TRANS ("Remove Listen Bus"), lb != nullptr);
    {
        PopupMenu am;
        const auto mode = app.project.masterTrack.autoMode;
        am.addItem (10, TRANS ("Off"), true, mode == AutoMode::Off);
        am.addItem (11, TRANS ("Read"), true, mode == AutoMode::Read);
        am.addItem (12, TRANS ("Touch"), true, mode == AutoMode::Touch);
        m.addSubMenu (TRANS ("Automation"), am);
    }
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&nameLabel), [this] (int r)
    {
        if (r == 1)
        {
            app.project.checkpoint();
            for (auto& t : app.project.tracks) t->isListenBus = false;
            auto& nt = app.project.addTrack (TrackType::Bus, TRANS ("Listen"));
            nt.isListenBus = true;
            nt.volumeDb = 0.0f;
            app.project.sendStructureChanged();
            app.project.sendContentChanged();
            app.engine.syncAll();
        }
        else if (r == 3 || r == 4)
        {
            if (auto* b = app.project.listenBus())
            {
                app.project.checkpoint();
                b->listenEnabled = (r == 3);
                app.project.sendMixChanged();
                app.engine.syncAll();
            }
        }
        else if (r == 2)
        {
            if (auto* b = app.project.listenBus())
            {
                app.project.checkpoint();
                app.project.removeTrack (b->id);
                app.project.sendStructureChanged();
                app.project.sendContentChanged();
                app.engine.syncAll();
            }
        }
    });
}

//==============================================================================
//== drag & drop of plug-in slots
namespace
{
    /** Reads a slot drag description; returns false when this is not one of ours. */
    bool decodeSlotDrag (const var& v, int& track, int64& uid, bool& instrument)
    {
        auto* o = v.getDynamicObject();
        if (o == nullptr || ! (bool) o->getProperty ("sxSlot")) return false;
        track = (int) o->getProperty ("track");
        uid = (int64) o->getProperty ("uid");
        instrument = (bool) o->getProperty ("instrument");
        return uid != 0;
    }
}

bool ChannelStrip::isInterestedInDragSource (const SourceDetails& d)
{
    int t = 0; int64 uid = 0; bool inst = false;
    if (! decodeSlotDrag (d.description, t, uid, inst)) return false;
    if (trackId == t && ! inst) return true;              // reordering inside this strip
    if (trackId == t && inst) return false;               // the instrument slot stays where it is

    // cross-strip: an instrument only fits an instrument track, an effect any channel strip
    if (inst)
    {
        auto* target = trackId < 0 ? nullptr : app.project.findTrack (trackId);
        return target != nullptr && target->type == TrackType::Instrument;
    }
    return true;
}

int ChannelStrip::dropIndexAt (const Point<int>& pos) const
{
    // which insert slot is under the pointer (below the last one = append)
    for (int i = 0; i < kInserts; ++i)
    {
        auto r = inserts[i].getBounds();
        if (r.getHeight() <= 0) continue;
        if (pos.y < r.getBottom()) return i;
    }
    return -1;
}

Rectangle<int> ChannelStrip::dropIndicatorBounds() const
{
    if (dropIndex < 0) return {};
    auto r = inserts[jlimit (0, kInserts - 1, dropIndex)].getBounds();
    return r.withHeight (2).translated (0, -1);
}

void ChannelStrip::itemDragEnter (const SourceDetails& d)
{
    dropIndex = dropIndexAt (d.localPosition);
    repaint();
}

void ChannelStrip::itemDragMove (const SourceDetails& d)
{
    const int idx = dropIndexAt (d.localPosition);
    if (idx != dropIndex) { dropIndex = idx; repaint(); }
}

void ChannelStrip::itemDragExit (const SourceDetails&)
{
    if (dropIndex >= 0) { dropIndex = -1; repaint(); }
}

void ChannelStrip::itemDropped (const SourceDetails& d)
{
    dropIndex = -1;
    repaint();

    int fromTrack = 0; int64 uid = 0; bool instrument = false;
    if (! decodeSlotDrag (d.description, fromTrack, uid, instrument)) return;

    int toIndex = dropIndexAt (d.localPosition);
    if (toIndex < 0)
    {
        const int n = trackId < 0 ? (int) app.project.masterTrack.inserts.size()
                                  : (app.project.findTrack (trackId) != nullptr ? (int) app.project.findTrack (trackId)->inserts.size() : 0);
        toIndex = n;                                       // dropped below the chain: append
    }
    app.moveOrCopySlot (fromTrack, uid, instrument, trackId, toIndex);
    refresh();
}

void ChannelStrip::mouseDown (const MouseEvent& e)
{
    if (e.eventComponent == &panLabel) { panLabel.showEditor(); return; }
    if (e.mods.isPopupMenu()) { showNameMenu(); return; }   // the master strip menu lives in there too
    if (trackId < 0) return;
    // plain click selects alone; Ctrl toggles and Shift extends, so tracks can be batched here too
    app.selectTrack (trackId, e.mods.isCtrlDown() || e.mods.isCommandDown(), e.mods.isShiftDown());
}

void ChannelStrip::paint (Graphics& g)
{
    g.fillAll (theme::white);
    g.setColour (theme::line);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());

    // a plug-in slot is being dragged over this strip: show where it would land
    if (dropIndex >= 0)
    {
        auto indicator = dropIndicatorBounds();
        if (! indicator.isEmpty())
        {
            g.setColour (theme::accent);
            g.fillRect (indicator);
        }
    }
    auto* t = track();
    if (t != nullptr)
    {
        g.setColour (t->colour);
        g.fillRect (0, 0, getWidth() - 1, 4);
        if (app.selectedTracks.count (trackId) > 0)
        {
            g.setColour (theme::accent.withAlpha (0.10f));
            g.fillRect (0, 4, getWidth() - 1, 30);
        }
    }
    else if (trackId == -1)
    {
        // the master strip carries the master bus colour, like every other channel strip
        g.setColour (app.project.masterTrack.colour);
        g.fillRect (0, 0, getWidth() - 1, 4);
    }
}

//==============================================================================
// The single source of truth for the height-tier visibility. resized() owns the bounds and
// calls this first, refresh() calls it after a state change: without that, a mix notification
// re-showed the slot / pan / OUT / automation controls at the bounds they had in an earlier,
// taller layout, and they were painted on top of the fader and routing rows.
void ChannelStrip::applyTierVisibility()
{
    const int H = getHeight();
    auto* trk = track();
    const bool monitorBus  = monitorBusStrip;      // the listen bus is not part of the arrangement
    const bool realTrack   = trk != nullptr;
    const bool masterStrip = trackId == -1;
    const bool compact     = H < 240;
    const bool showAuto    = H >= 210 && ! monitorBus && (masterStrip || realTrack);
    const bool showSlot    = H >= 250 && trk != nullptr && ! trk->isGroupTrack() && trk->type != TrackType::Bus;
    // the listen bus is a monitor path: no left/right positioning, only a level
    const bool showPan     = H >= 300 && realTrack && ! trk->isListenBus;
    // the routing block (SND / OUT rows) only exists from 280 px up; below that the layout pass
    // dropped it and refresh() used to leave the rows visible at stale bounds
    const bool sendSection = trk != nullptr && ! trk->isSend() && ! trk->isAutomation()
                             && ! monitorBus && H >= 280;

    slotButton.setVisible (! compact && showSlot);
    pan.setVisible (! compact && showPan);
    panLabel.setVisible (! compact && showPan);
    autoBtn.setVisible (! compact && showAuto && autoButtonAllowed);

    if (compact)
    {
        // one clean compact layout (see resized()): the routing block is not shown at all
        for (auto& b : inserts) b.setVisible (false);
        for (auto& b : sendRows) b.setVisible (false);
        addSendRow.setVisible (false);
        outputRow.setVisible (false);
        panMeter.setBounds ({});
        scope.setVisible (false);
        return;
    }

    if (! sendSection)
    {
        for (auto& b : sendRows) b.setVisible (false);
        addSendRow.setVisible (false);
        outputRow.setVisible (false);
    }
}

void ChannelStrip::resized()
{
    const int H = getHeight();
    auto r = getLocalBounds().reduced (5, 0);

    // the strip compresses in tiers instead of clipping: inserts -> pan -> slot ->
    // auto button drop out first; name, fader, meter and M / S / R always stay.
    // applyTierVisibility() is the same decision refresh() uses, so both paths agree.
    auto* trk = track();
    // use the flag set by refresh(): sizing may run when track() is momentarily unavailable,
    // which used to re-show the automation button on top of ON
    const bool monitorBus = monitorBusStrip;
    // the monitor bus carries no automation: it is not part of the arrangement
    const bool realTrack   = trk != nullptr;
    const bool showAuto    = H >= 210 && ! monitorBus && (trackId == -1 || realTrack);   // master + real tracks only
    const bool masterStrip = trackId == -1;
    const bool showSlot    = H >= 250 && trk != nullptr && ! trk->isGroupTrack() && trk->type != TrackType::Bus;
    // the listen bus is a monitor path: no left/right positioning, only a level
    const bool showPan     = H >= 300 && realTrack && ! trk->isListenBus;
    applyTierVisibility();

    // The mixer can be dragged down to a very short strip. At that size there is no honest way
    // to show the inserts / slot / routing block *and* the fader: the old tiering still placed
    // them and they stacked on top of each other. Use one compact layout instead: name, fader +
    // meter, dB and the M / S / R row.
    if (H < 240)
    {
        for (auto& b : inserts) b.setVisible (false);
        for (auto& b : sendRows) b.setVisible (false);
        addSendRow.setVisible (false);
        outputRow.setVisible (false);
        slotButton.setVisible (false);
        pan.setVisible (false);
        panLabel.setVisible (false);
        panMeter.setBounds ({});
        autoBtn.setVisible (false);
        scope.setVisible (false);

        auto bottom = r.removeFromBottom (30);
        auto row = bottom.removeFromTop (22).withTrimmedTop (2);
        if (masterStrip)
        {
            const int bw = jmax (22, (row.getWidth() - 4) / 2);
            muteBtn.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (4);
            monoBtn.setBounds (row.removeFromLeft (jmax (28, row.getWidth())));
        }
        else if (monitorBus)
        {
            if (listenBtn.isVisible()) listenBtn.setBounds (row.reduced (2, 1));
        }
        else
        {
            const int count = 3 + (phaseBtn.isVisible() ? 1 : 0)
                              + (monBtn.isVisible() ? 1 : 0) + (listenSoloBtn.isVisible() ? 1 : 0);
            const int bw = jmax (14, (row.getWidth() - 3 * (count - 1)) / count);
            muteBtn.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (3);
            if (phaseBtn.isVisible())
            {
                phaseBtn.setBounds (row.removeFromLeft (bw));
                row.removeFromLeft (3);
            }
            soloBtn.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (3);
            armBtn.setBounds (row.removeFromLeft (bw));
            if (monBtn.isVisible()) { row.removeFromLeft (3); monBtn.setBounds (row.removeFromLeft (bw)); }
            if (listenSoloBtn.isVisible()) { row.removeFromLeft (3); listenSoloBtn.setBounds (row.removeFromLeft (bw)); }
        }

        dbLabel.setBounds (r.removeFromBottom (16));
        r.removeFromTop (4);
        auto nameRow = r.removeFromTop (jlimit (12, 24, r.getHeight() - 34));
        nameLabel.setBounds (nameRow);

        auto faderArea = r.reduced (0, 2);
        meter.setBounds (faderArea.removeFromRight (14).reduced (0, 3));
        faderArea.removeFromRight (4);
        fader.setBounds (faderArea);
        const bool showFader = faderArea.getHeight() >= 18;
        fader.setVisible (showFader);
        if (! showFader) meter.setBounds ({});
        return;
    }

    // the master keeps its automation switch, but as a compact square next to M / MONO instead of
    // the grey block on a row of its own
    autoBtn.setVisible (autoBtn.isVisible() && (trackId < 0 || showAuto));
    auto bottom = r.removeFromBottom (trackId == -1 ? 46 : (showAuto ? 52 : 30));
    if (trackId == -1)
    {
        // the master strip: M | MONO on the first line, the automation switch as a slim labelled
        // row underneath (it is a status line, not the grey block it used to be)
        auto row = bottom.removeFromTop (26).withTrimmedTop (2);
        if (phaseBtn.isVisible())
        {
            const int bw = jmax (24, (row.getWidth() - 8) / 3);
            muteBtn.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (4);
            phaseBtn.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (4);
            monoBtn.setBounds (row.removeFromLeft (jmax (28, row.getWidth())));
        }
        else
        {
            const int bw = jmax (24, (row.getWidth() - 4) / 2);
            muteBtn.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (4);
            monoBtn.setBounds (row.removeFromLeft (jmax (28, row.getWidth())));
        }
        bottom.removeFromTop (2);
        autoBtn.setBounds ((showAuto && autoButtonAllowed) ? bottom.removeFromTop (18) : Rectangle<int>());
    }
    else if (trackId != -1)
    {
        auto row = showAuto ? bottom.removeFromTop (22) : bottom.withTrimmedTop (4);
        if (monitorBus)
        {
            // the monitor bus has no mute / solo / arm: ON is the only button down here
            if (listenBtn.isVisible()) listenBtn.setBounds (row.reduced (2, 1));
        }
        else
        {
            const int count = 3 + (phaseBtn.isVisible() ? 1 : 0)
                              + (monBtn.isVisible() ? 1 : 0) + (listenSoloBtn.isVisible() ? 1 : 0);
            const int bw = jmax (14, (row.getWidth() - 3 * (count - 1)) / count);
            muteBtn.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (3);
            if (phaseBtn.isVisible())
            {
                phaseBtn.setBounds (row.removeFromLeft (bw));
                row.removeFromLeft (3);
            }
            soloBtn.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (3);
            armBtn.setBounds (row.removeFromLeft (bw));
            if (monBtn.isVisible())
            {
                row.removeFromLeft (3);
                monBtn.setBounds (row.removeFromLeft (bw));
            }
            if (listenSoloBtn.isVisible())
            {
                row.removeFromLeft (3);
                listenSoloBtn.setBounds (row);
            }
        }
        if (showAuto)
        {
            bottom.removeFromTop (4);
            autoBtn.setBounds (bottom.removeFromTop (20));
        }
    }

    dbLabel.setBounds (r.removeFromBottom (18));

    r.removeFromTop (6);
    auto nameRow = r.removeFromTop (jlimit (14, 26, r.getHeight() - 40));   // at least 40 for the fader
    nameLabel.setBounds (nameRow);
    if (showSlot) { r.removeFromTop (3); slotButton.setBounds (r.removeFromTop (22)); r.removeFromTop (4); }

    const int reserve = (showPan ? 42 : 0) + 46;
    const int rowH = 18, rowGap = 2, rowStep = rowH + rowGap;

    // Studio One style routing block: inserts, then the SND rows plus an explicit "+ Send" row,
    // then the OUT row. The master strip only owns inserts, which must stay visible at any
    // usable strip height (they used to vanish when the strip was shorter than 310 px).
    int avail = r.getHeight() - reserve;
    int visibleInserts = 0, visibleSends = 0;
    bool showOut = false, showAddSend = false;

    // (sendSection uses monitorBus, declared at the top of this function)
    const bool sendSection = trk != nullptr && ! trk->isSend() && ! trk->isAutomation() && ! monitorBus && H >= 280;
    if (sendSection)
    {
        showOut = avail >= rowStep;
        if (showOut) avail -= rowStep;
        visibleSends = jlimit (0, kSends, jmin ((int) trk->sends.size(), avail / rowStep));
        avail -= visibleSends * rowStep;
        showAddSend = visibleSends < kSends && avail >= rowStep;
        if (showAddSend) avail -= rowStep;
        // never let the insert chain disappear entirely because of sends
        if (visibleSends > 0 && avail < rowStep)
        {
            --visibleSends;
            avail += rowStep;
            showAddSend = false;
        }
    }
    visibleInserts = jlimit (0, kInserts, jmax (0, avail) / rowStep);

    for (int i = 0; i < kInserts; ++i)
    {
        inserts[i].setVisible (i < visibleInserts);
        if (i < visibleInserts) { inserts[i].setBounds (r.removeFromTop (rowH)); r.removeFromTop (rowGap); }
    }
    for (int i = 0; i < kSends; ++i)
    {
        sendRows[i].setVisible (i < visibleSends);
        if (i < visibleSends) { sendRows[i].setBounds (r.removeFromTop (rowH)); r.removeFromTop (rowGap); }
    }
    addSendRow.setVisible (showAddSend);
    if (showAddSend) { addSendRow.setBounds (r.removeFromTop (rowH)); r.removeFromTop (rowGap); }
    outputRow.setVisible (showOut);
    if (showOut) outputRow.setBounds (r.removeFromTop (rowH));

    if (showPan)
    {
        auto panArea = r.removeFromTop (52);
        pan.setBounds (panArea.removeFromTop (38).withSizeKeepingCentre (38, 38));
        panLabel.setBounds (panArea.reduced (2, 0));
    }

    auto faderArea = r.reduced (0, 2);

    // The master's goniometer and the channel's pan meter sit between the pan section and the
    // fader. Their strip is carved out of the fader area *before* the fader is placed, so they
    // can never be positioned over the routing block or the fader again; when the strip is too
    // short for the extra row they stay empty.
    if (masterStrip)
    {
        scope.setVisible (true);
        if (faderArea.getHeight() >= 64 + 4 + 24)
            scope.setBounds (faderArea.removeFromTop (64).reduced (2, 1));
        else
            scope.setBounds ({});
    }
    else
    {
        scope.setVisible (false);
        scope.setBounds ({});
    }

    if (showPan && faderArea.getHeight() >= 12 + 4 + 24)
        panMeter.setBounds (faderArea.removeFromTop (12).reduced (2, 0));
    else
        panMeter.setBounds ({});

    meter.setBounds (faderArea.removeFromRight (14).reduced (0, 4));
    faderArea.removeFromRight (4);
    fader.setBounds (faderArea);
    const bool showFader = faderArea.getHeight() >= 24;
    fader.setVisible (showFader);
    if (! showFader) meter.setBounds ({});
}
}