#pragma once

#include "engine/Engine.h"
#include "Theme.h"
#include <set>
#include <map>

namespace sx
{
class PluginWindows;

struct GridOption { const char* name; double beats; };
inline const std::vector<GridOption>& gridOptions()
{
    static const std::vector<GridOption> g {
        { "Bar", 4.0 }, { "1/2", 2.0 }, { "1/4", 1.0 }, { "1/8", 0.5 }, { "1/16", 0.25 }, { "1/32", 0.125 },
        { "1/8T", 1.0 / 3.0 }, { "1/16T", 1.0 / 6.0 }, { "Off", 0.0 } };
    return g;
}

inline double snapTo (double beat, double grid)
{
    if (grid <= 0.0) return beat;
    return std::round (beat / grid) * grid;
}

// tools for editing clips in the arrange view (toolbar on the transport bar)
enum class ArrangeTool { Select, Pencil, Eraser, Mute, Scissors };

// track row heights (px) the Ctrl+wheel and Taller / Shorter commands step through
inline const std::vector<int>& trackHeightSteps()
{
    static const std::vector<int> s { 23, 31, 46, 62, 78, 104, 150, 200, 260, 340 };
    return s;
}

// the next step above (dir > 0) or below (dir < 0) 'from'. The top step is 'maxH'
// (half the screen), so the ladder always ends exactly there.
inline int stepTrackHeight (int from, int dir, int maxH)
{
    std::vector<int> steps;
    for (int s : trackHeightSteps()) if (s < maxH) steps.push_back (s);
    steps.push_back (maxH);
    if (dir > 0)
    {
        for (int s : steps) if (s > from) return s;
        return steps.back();
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) if (*it < from) return *it;
    return steps.front();
}

// horizontal zoom levels (px per beat) the ruler wheel steps through
inline const std::vector<double>& pxPerBeatSteps()
{
    static const std::vector<double> s { 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0, 16.0, 20.0, 24.0, 32.0, 40.0,
                                         48.0, 64.0, 80.0, 96.0, 128.0, 160.0, 192.0, 256.0, 320.0, 384.0, 480.0 };
    return s;
}

// the next px-per-beat step above (dir > 0) or below (dir < 0) 'from'
inline double stepPxPerBeat (double from, int dir)
{
    const auto& steps = pxPerBeatSteps();
    if (dir > 0)
    {
        for (double s : steps) if (s > from + 1.0e-6) return s;
        return steps.back();
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) if (*it < from - 1.0e-6) return *it;
    return steps.front();
}

// mixer channel strip widths (px) the Ctrl+wheel steps through
inline const std::vector<int>& stripWidthSteps()
{
    static const std::vector<int> s { 64, 80, 96, 104, 120, 144, 176, 216, 260 };
    return s;
}

inline int stepStripWidth (int from, int dir)
{
    const auto& steps = stripWidthSteps();
    if (dir > 0)
    {
        for (int s : steps) if (s > from) return s;
        return steps.back();
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) if (*it < from) return *it;
    return steps.front();
}

//==============================================================================
// One display snapshot per channel. The audio engine's peak atomics are consumed once per tick
// by MeterCache, so every fader (mixer channel, arrange header, master) can show the same level
// and the same persistent maximum peak without stealing samples from one another.
struct MeterDisplay
{
    float levelL = 0.0f, levelR = 0.0f;   // decayed live level (0..1+)
    float rmsL = 0.0f, rmsR = 0.0f;
    float holdL = 0.0f, holdR = 0.0f;     // slow-decay peak hold
    float maxL = 0.0f, maxR = 0.0f;       // highest peak since the app started
    bool clipped = false;
};

class MeterCache : private juce::Timer
{
public:
    MeterCache (Project& p, Engine& e) : project (p), engine (e) {}
    ~MeterCache() override { clients = 0; stopTimer(); }

    void start();
    void stop();
    const MeterDisplay& get (int trackId) const;
    void resetMaximum (int trackId);
    /** Test hook: consume one meter window without waiting for the message-thread timer. */
    void pumpForTest() { timerCallback(); }

private:
    void timerCallback() override;
    void updateChannel (int trackId);

    Project& project;
    Engine& engine;
    std::map<int, MeterDisplay> values;
    bool running = false;
    bool flushed = false;
    int clients = 0;
};

class AppState
{
public:
    AppState (Project& p, Engine& e, PluginManager& pm);
    ~AppState();

    Project& project;
    Engine& engine;
    PluginManager& plugins;
    MeterCache meters;

    int selectedTrack = -1;           // most recently selected track (also the inspector's target)
    std::set<int> selectedTracks;     // multi-track selection (includes selectedTrack)
    std::set<int> selectedClips;
    int editTrack = -1, editClip = -1;

    double pxPerBeat = 48.0;
    int trackHeight = 78;
    int stripWidth = 104;   // mixer channel strip width (Ctrl+wheel over the mixer zooms it)
    double arrangeGrid = 0.25;
    ArrangeTool arrangeTool = ArrangeTool::Select;
    bool clipGainFreehand = false;   // gain envelope drawing: straight lines vs freehand
    bool clipGainSnap = false;       // snap gain envelope anchors to the arrange grid (off by default: the envelope is a continuous curve and snapping made the anchors land far from the mouse)
    bool freezeBusy = false;         // a freeze render is running
    bool followPlayhead = true;
    bool musicalTyping = false;
    // timebase of the timeline display: the rulers and the transport readout follow the tempo map
    // in bars (false) or in absolute seconds (true). Remembered in the settings file.
    bool timeInSeconds = false;
    void setTimeInSeconds (bool on);
    bool showTempoLane = false;
    // the pinned master bus row in the arrangement (Studio One style, hidden by default).
    // It is a row in the track-row system: same height ladder, same header, automation sub-rows.
    bool showMasterTrack = false;
    // chord track: one chord symbol per position, project wide (off by default, View > Chord Track)
    bool showChordTrack = false;
    // marker track: the named arrangement sections (off by default, View > Marker Track)
    bool showMarkerTrack = false;
    // piano roll: show the chord row (authored chords and auto-detected ones, like Studio One)
    bool pianoRollChords = true;
    // piano roll: the amount the Humanize command applies. The dialog writes it, so the H shortcut
    // and the next time the dialog opens start from what was used last.
    HumanizeSettings humanize;
    // last chord symbol the user typed on the chord track: new chords and chord input start here
    juce::String lastChordSymbol { "C" };
    // length of a new chord block / marker section, in bars (drag the right edge to change a block)
    int chordBars = 1;
    int sectionBars = 4;
    int keyboardOctave = 4;
    int keyboardVelocity = 96;
    // where the floating on-screen keyboard sits inside the main window (-1 = not moved yet, use
    // the default bottom-centre position). Kept across theme switches and window resizes.
    int keyboardX = -1, keyboardY = -1;

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void selectionChanged() {}
        virtual void viewChanged() {}
        virtual void editorClipChanged() {}
        virtual void panelRequested (const juce::String&) {}
    };
    void addListener (Listener* l) { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    void selectTrack (int id, bool additive = false, bool range = false);
    void selectClip (int trackId, int clipId, bool additive);
    void clearClipSelection();
    void openClipInEditor (int trackId, int clipId);
    void setZoom (double ppb);
    void notifyView() { listeners.call ([] (Listener& l) { l.viewChanged(); }); }
    void requestPanel (const juce::String& name) { listeners.call ([&] (Listener& l) { l.panelRequested (name); }); }
    void notifySelection() { listeners.call ([] (Listener& l) { l.selectionChanged(); }); }

    void showPluginEditor (juce::int64 uid);
    /** Opens the editor of a plug-in that was just inserted, when the user wants that
        (View > Plug-in Panels > Open Panel on Insert; on by default). */
    void showInsertedPluginEditor (juce::int64 uid);
    /** The uid of the last insert added to a track (or the master bus, trackId -1). */
    juce::int64 lastInsertUid (int trackId) const;

    // whether inserting a plug-in opens its panel, and whether several panels may stay open
    bool openPluginEditorOnInsert = true;
    bool multiPluginWindows = false;
    void setOpenPluginEditorOnInsert (bool on);
    void setMultiPluginWindows (bool on);

    Track& addTrack (TrackType type);
    void deleteTrack (int id);
    void deleteTracks (const std::vector<int>& ids);
    void duplicateTrack (int id);
    void duplicateTracks (const std::vector<int>& ids);
    void moveTrack (int id, int delta);
    void moveTrackTo (int id, int insertIndex);   // drag & drop reorder (drop-before slot)
    void setTracksHidden (const std::vector<int>& ids, bool hidden);
    /** A track the arranger draws. Multi-output mixer channels (sourceTrack != 0) are
        arranger-hidden by design: they only ever live in the mixer, their 'hidden' flag is not a
        user state, and their mixer visibility is a separate per-instrument switch. */
    static bool isArrangerTrack (const Track& t) { return t.sourceTrack == 0; }
    /** The tracks that own the clips in the current clip selection (track order, unique;
        stale clip ids are skipped). The hide command falls back to this when no track row is
        selected - Ctrl+A and rubber-band selections only fill selectedClips. */
    std::vector<int> selectedClipTrackIds() const;
    /** The hidden tracks the arranger can show again: what "Show All Hidden Tracks" restores. */
    std::vector<int> hiddenArrangerTrackIds() const;

    //== plug-in drag & drop ====================================================================
    /** Dragging a plug-in slot: onto the same track it is reordered, onto another track it is
        copied with its current settings (one undo step). Returns true when something changed. */
    bool moveOrCopySlot (int fromTrackId, juce::int64 uid, bool instrument,
                         int toTrackId, int toIndex);

    //== multi-output instruments ===============================================================
    // A plug-in instrument with several outputs can spread them over mixer channels: every extra
    // stereo pair becomes a channel track with its own fader, inserts, sends and routing (the
    // Studio One model). Expanding is one undo step, collapsing removes them again.
    int instrumentOutputPairs (int trackId);      // extra stereo pairs the plug-in publishes
    int expandOutputChannels (int trackId);       // creates the missing channel tracks, returns how many
    int collapseOutputChannels (int trackId);     // removes them, returns how many
    bool hasOutputChannels (int trackId) const { return project.numOutputChannels (trackId) > 0; }
    void toggleOutputChannelsInMixer (int trackId);   // the show / hide switch of the mixer
    // manual latency compensation for one track, in milliseconds (positive = plays later)
    void setTrackDelay (int trackId, double ms);


    // freeze / unfreeze: a frozen track is pre-rendered to audio and its plugins stay idle
    void freezeTrack (int trackId, bool withFx = true);
    void unfreezeTrack (int trackId);
    void freezeToggle (int trackId);
    // the freeze render runs on a worker thread behind a progress window (some instruments only
    // render happily off the message thread); the self test switches this off
    bool showFreezeProgress = true;
    std::function<void (const juce::String&)> onError;

    //== audio import ==========================================================================
    /** Adds an audio clip at once and decodes the file on a background thread, so dropping a
        (large) file never blocks the UI; several files decode in parallel. 'targetTrackId' -1
        creates a new audio track. Returns false when the file cannot be read at all. */
    bool importAudio (const juce::File& f, double atBeat, int targetTrackId);
    /** Completion of a background decode (message thread): fills every clip that still waits for
        this file and drops the placeholder on failure. */
    void finishAudioImport (const juce::File& f, const std::shared_ptr<AudioData>& data);

    /** How many rows the arranger currently hides (multi-output mixer channels excluded: they are
        arranger-hidden by design, not a user hide state). */
    int hiddenTrackCount() const;
    Track* track() { return project.findTrack (selectedTrack); }

    // grouping / routing: a Bus collects the selected tracks, an FX Send track is a return
    int groupIntoBus (const std::vector<int>& ids);   // returns the new bus id (0 = nothing grouped)
    /** Packs the selected tracks into a folder track: the tracks are moved together below the
        folder, indented, routed through it, and can be collapsed. Returns the folder id. */
    int groupIntoFolder (const std::vector<int>& ids);
    /** Removes a folder but keeps its tracks: they move one level up and keep playing. */
    bool unpackFolder (int folderId);
    void toggleFolderOpen (int folderId);
    /** Adds a stand-alone automation track driving one parameter of 'targetTrackId' (0 = master).
        An existing inline lane for that parameter moves into the new track. Returns its id. */
    int addAutomationTrack (int targetTrackId, const juce::String& target, const juce::String& label);
    /** Moves the inline automation lane of 'trackId' out into its own track (0 on failure). */
    int moveAutomationToTrack (int trackId, const juce::String& target);
    /** Folds a stand-alone automation track back into its channel as an inline lane. */
    bool inlineAutomationTrack (int autoTrackId);
    /** Re-routes an automation track to another channel (0 = the master bus). */
    bool setAutomationTargetTrack (int autoTrackId, int targetTrackId);
    int addSendTrack();                               // returns the new send-track id
    void addSend (int fromTrackId, int targetId);
    void removeSend (int fromTrackId, int targetId);
    void setSendLevel (int fromTrackId, int targetId, float levelDb);
    void setSendPre (int fromTrackId, int targetId, bool pre);

    std::unique_ptr<PluginWindows> windows;

private:
    juce::ListenerList<Listener> listeners;

    // background audio decode: the pool is created on the first import, and the context keeps a
    // safe pointer to this object so a completion that arrives during shutdown does nothing
    struct ImportContext;
    std::shared_ptr<ImportContext> importContext;
    std::unique_ptr<juce::ThreadPool> importPool;
};
}
