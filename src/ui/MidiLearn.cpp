#include "MidiLearn.h"
#include "Theme.h"
#include "Widgets.h"

namespace sx
{
using namespace juce;

namespace
{
String targetKindName (MidiTargetKind k)
{
    switch (k)
    {
        case MidiTargetKind::MasterVolume:      return TRANS ("Master volume");
        case MidiTargetKind::TrackVolume:       return TRANS ("Track volume");
        case MidiTargetKind::TrackPan:          return TRANS ("Track pan");
        case MidiTargetKind::PluginParam:       return TRANS ("Plug-in parameter");
        case MidiTargetKind::Metronome:         return TRANS ("Metronome on / off");
        case MidiTargetKind::TransportPlayStop: return TRANS ("Play / stop");
        default: break;
    }
    return {};
}

/** One row of the mapping list. */
struct Row
{
    MidiMapping map;
    String source;
};

class MidiLearnPanel : public Component,
                       private ListBoxModel,
                       private Timer
{
public:
    explicit MidiLearnPanel (AppState& a) : app (a)
    {
        addAndMakeVisible (title);
        title.setFont (theme::font (13.0f, true));
        title.setText (TRANS ("MIDI LEARN"), dontSendNotification);
        title.setColour (Label::textColourId, theme::text);

        addAndMakeVisible (hint);
        hint.setFont (theme::font (11.0f));
        hint.setColour (Label::textColourId, theme::textDim);
        hint.setText (TRANS ("Pick a target with Add Target, then move the knob / fader / button you "
                             "want to use. Mappings are saved with the project."), dontSendNotification);

        addAndMakeVisible (addBtn);
        addBtn.setButtonText (TRANS ("Add Target..."));
        addBtn.onClick = [this] { chooseTarget(); };

        addAndMakeVisible (cancelBtn);
        cancelBtn.setButtonText (TRANS ("Cancel Learn"));
        cancelBtn.onClick = [this] { app.engine.cancelLearn(); refresh(); };

        addAndMakeVisible (removeBtn);
        removeBtn.setButtonText (TRANS ("Remove"));
        removeBtn.onClick = [this] { removeSelected(); };

        addAndMakeVisible (status);
        status.setFont (theme::font (11.5f, true));
        status.setColour (Label::textColourId, theme::accent);

        list.setRowHeight (24);
        list.setColour (ListBox::backgroundColourId, theme::laneB);
        list.setColour (ListBox::outlineColourId, theme::lineSoft);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        app.engine.onMidiMappingChanged = [this] { refresh(); };
        refresh();
        setSize (470, 460);        // the dialog window takes its size from the content
        startTimerHz (10);
    }

    ~MidiLearnPanel() override
    {
        stopTimer();
        app.engine.onMidiMappingChanged = nullptr;
        app.engine.cancelLearn();
    }

    std::function<void()> onClose;

    void paint (Graphics& g) override { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (22));
        // the hint wraps to three lines in Chinese, so it gets the room for all of them
        hint.setBounds (r.removeFromTop (48));
        r.removeFromTop (4);

        auto tools = r.removeFromTop (26);
        addBtn.setBounds (tools.removeFromLeft (130));
        tools.removeFromLeft (8);
        cancelBtn.setBounds (tools.removeFromLeft (120));
        tools.removeFromLeft (8);
        removeBtn.setBounds (tools.removeFromLeft (90));

        r.removeFromTop (6);
        status.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        list.setBounds (r);
    }

    void refresh()
    {
        rows.clear();
        for (auto& m : app.project.midiMappings)
        {
            Row row;
            row.map = m;
            // "CH 3 · CC 74" or "CH 1 · Note 36"
            row.source = (m.channel == 0 ? String (TRANS ("any channel")) : "CH " + String (m.channel))
                       + " · " + (m.isNote ? "Note " : "CC ") + String (m.cc);
            rows.push_back (row);
        }
        list.updateContent();
        list.repaint();

        const bool learning = app.engine.isLearning();
        cancelBtn.setEnabled (learning);
        if (learning)
        {
            status.setText (TRANS ("Waiting for MIDI: move the controller for") + " "
                            + app.engine.learnLabel() + " ...", dontSendNotification);
            status.setColour (Label::textColourId, theme::accent);
        }
        else
        {
            status.setText (app.project.midiMappings.empty()
                                ? TRANS ("Nothing mapped yet.")
                                : String ((int) app.project.midiMappings.size()) + " "
                                      + TRANS ("mappings"), dontSendNotification);
            status.setColour (Label::textColourId, theme::textDim);
        }
        repaint();
    }

    //== ListBoxModel
    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem (int row, Graphics& g, int width, int height, bool rowIsSelected) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        const auto& r = rows[(size_t) row];

        if (rowIsSelected)
        {
            g.setColour (theme::accentSoft);
            g.fillRect (0, 0, width, height);
        }
        g.setColour (theme::text);
        g.setFont (theme::font (11.5f));
        g.drawFittedText (r.map.label.isNotEmpty() ? r.map.label : targetKindName (r.map.target.kind),
                          Rectangle<int> (6, 0, width - 150, height), Justification::centredLeft, 1);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        g.drawFittedText (r.source, width - 150, 0, 144, height, Justification::centredRight, 1);
    }

    void deleteKeyPressed (int) override { removeSelected(); }

private:
    void timerCallback() override
    {
        // the engine applies controller moves on its own timer; this keeps the learn banner honest
        const bool learning = app.engine.isLearning();
        if (learning != lastLearning)
        {
            lastLearning = learning;
            refresh();
        }
    }

    void removeSelected()
    {
        const int row = list.getSelectedRow();
        if (row < 0 || row >= (int) app.project.midiMappings.size()) return;
        app.project.checkpoint();
        app.project.midiMappings.erase (app.project.midiMappings.begin() + row);
        app.project.sendContentChanged();
        refresh();
    }

    /** The list of things a controller can be mapped to: master, every track, the plug-ins of the
        selected track, the transport and the metronome. */
    void chooseTarget()
    {
        PopupMenu m;

        m.addItem (1, TRANS ("Master volume"));

        PopupMenu tracks;
        int id = 100;
        for (auto& t : app.project.tracks)
        {
            if (t == nullptr || t->isListenBus || t->isOutputChannel()) continue;
            PopupMenu one;
            one.addItem (id * 10 + 1, TRANS ("Volume"));
            one.addItem (id * 10 + 2, TRANS ("Pan"));
            tracks.addSubMenu (t->name, one);
            ++id;
        }
        m.addSubMenu (TRANS ("Track"), tracks, ! tracks.containsAnyActiveItems() ? false : true);

        // plug-in parameters: the selected track's instrument and inserts (a synth can have hundreds
        // of parameters, so this is scoped to one track)
        if (auto* sel = app.project.findTrack (app.selectedTrack))
        {
            PopupMenu plugins;
            auto addSlot = [&] (const PluginSlot& slot)
            {
                auto inst = app.engine.getInstance (slot.uid);
                if (inst == nullptr || inst->proc == nullptr) return;
                auto& params = inst->proc->getParameters();
                if (params.isEmpty()) return;

                PopupMenu ps;
                const int count = jmin (128, params.size());
                for (int i = 0; i < count; ++i)
                    if (auto* p = params[i])
                        ps.addItem (300000 + (int) (slot.uid % 100000) * 1000 + i,
                                    String (i + 1) + ". " + p->getName (48));
                plugins.addSubMenu (slot.desc.name, ps);
            };
            if (sel->hasInstrument) addSlot (sel->instrument);
            for (auto& ins : sel->inserts) addSlot (ins);
            if (plugins.containsAnyActiveItems())
                m.addSubMenu (TRANS ("Plug-in parameter") + " (" + sel->name + ")", plugins);
        }

        m.addSeparator();
        m.addItem (2, TRANS ("Metronome on / off"));
        m.addItem (3, TRANS ("Play / stop"));

        auto* appPtr = &app;
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (&addBtn).withMinimumWidth (200),
                         [this, appPtr] (int r)
        {
            if (r == 0) return;

            MidiTarget target;
            String label;
            if (r == 1) { target.kind = MidiTargetKind::MasterVolume; label = TRANS ("Master volume"); }
            else if (r == 2) { target.kind = MidiTargetKind::Metronome; label = TRANS ("Metronome on / off"); }
            else if (r == 3) { target.kind = MidiTargetKind::TransportPlayStop; label = TRANS ("Play / stop"); }
            else if (r >= 300000)
            {
                // plug-in parameter: decode uid + index
                const int rest = r - 300000;
                const int64 uidLow = rest / 1000;
                const int paramIdx = rest % 1000;
                auto* sel = appPtr->project.findTrack (appPtr->selectedTrack);
                if (sel == nullptr) return;
                auto matchSlot = [&] (const PluginSlot& slot) -> bool
                {
                    return (int64) (slot.uid % 100000) == uidLow;
                };
                int64 uid = 0;
                String pluginName;
                if (sel->hasInstrument && matchSlot (sel->instrument)) { uid = sel->instrument.uid; pluginName = sel->instrument.desc.name; }
                for (auto& ins : sel->inserts)
                    if (matchSlot (ins)) { uid = ins.uid; pluginName = ins.desc.name; }

                String paramName;
                if (auto inst = appPtr->engine.getInstance (uid))
                    if (inst->proc != nullptr && paramIdx < inst->proc->getParameters().size())
                        if (auto* p = inst->proc->getParameters()[paramIdx])
                            paramName = p->getName (48);

                target.kind = MidiTargetKind::PluginParam;
                target.uid = uid;
                target.paramIndex = paramIdx;
                label = (sel->name + " · " + pluginName + " · " + paramName).trimCharactersAtEnd (" ·");
            }
            else if (r >= 1000)
            {
                const int trackIdx = r / 10 - 100;
                const int what = r % 10;
                if (trackIdx < 0 || trackIdx >= (int) appPtr->project.tracks.size()) return;
                auto* t = appPtr->project.tracks[(size_t) trackIdx].get();
                if (t == nullptr) return;
                target.kind = what == 1 ? MidiTargetKind::TrackVolume : MidiTargetKind::TrackPan;
                target.trackId = t->id;
                label = t->name + " - " + (what == 1 ? TRANS ("Volume") : TRANS ("Pan"));
            }
            else return;

            appPtr->engine.setLearnTarget (target, label);
            refresh();
        });
    }

    AppState& app;
    Label title, hint, status;
    TextButton addBtn, cancelBtn, removeBtn;
    ListBox list { "midilearn", this };
    std::vector<Row> rows;
    bool lastLearning = false;
};
}

void showMidiLearnWindow (AppState& app)
{
    auto* panel = new MidiLearnPanel (app);
    DialogWindow::LaunchOptions lo;
    lo.dialogTitle = TRANS ("MIDI Learn");
    lo.content.setOwned (panel);
    lo.dialogBackgroundColour = theme::panel;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = true;
    lo.launchAsync();
}

Image renderMidiLearnPanel (AppState& app)
{
    MidiLearnPanel panel (app);
    panel.setSize (470, 460);
    panel.resized();
    return panel.createComponentSnapshot (panel.getLocalBounds(), true);
}
}
