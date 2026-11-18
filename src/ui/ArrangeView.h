#pragma once

#include "AppState.h"
#include "Widgets.h"

namespace sx
{
class ArrangeView : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    private Project::Listener,
                    private AppState::Listener,
                    private juce::Timer
{
public:
    explicit ArrangeView (AppState& a);
    ~ArrangeView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // editing commands
    void splitAtPlayhead();
    void splitAtCursor();                     // S: cut the selected clips where the mouse last was
    /** Cuts every selected clip at this beat (shared by the commands and the scissors tool). */
    void splitAt (double cut);
    void addCrossfadeSelected();              // X: Studio One style crossfade between adjacent audio clips
    void fadesToPlayhead (bool fadeIn);       // fade the selected clips up to / away from the playhead
    void clearFadesSelected();
    void setFadesSelected (bool fadeIn, double sec);   // preset fade length for every selected clip
    void setCrossfadeSelected (double sec);            // resize the overlap of each selected pair
    void setCrossfadeCurveSelected (int curve);        // 0 equal power, 1 linear, 2 S-curve
    void nudgeClipGainSelected (float db);             // +/-1 dB on every selected audio clip
    void resetClipGainSelected();
    /** Event timebase of the clicked audio clip and of every selected one: Bars keeps the clip on
        its musical position, Seconds glues it to absolute time (one undo step). */
    void setClipTimebaseSelected (int clickedClipId, Timebase tb);

    // non-destructive audio editing on the selected clips (reverse / speed / transpose)
    void reverseSelectedAudio();
    void setSelectedAudioStretch (double factor);      // timeline length multiplier, 1 = original
    void setSelectedAudioPitch (int semitones);        // absolute transpose, -24..+24
    void nudgeSelectedAudioPitch (int delta);
    void resetSelectedAudioEdits();
    void deleteSelected();
    void duplicateSelected();
    void copySelected();
    void pasteAtPlayhead();
    void cutSelected() { copySelected(); deleteSelected(); }
    void selectAll();
    void setLoopToSelection();
    void mergeSelectedClips();
    void zoomToFit();

    // chord track / marker track entry from the menus: they open the rows when they are hidden
    void enterChordAtPlayhead();
    void addSectionAtPlayhead();

    void zoomBy (double factor);
    void zoomTrackHeights (float notches, int anchorY = -1);      // one step per notch
    static float notchesOf (const juce::MouseWheelDetails& w);    // wheel travel in notches (1.0 = one click)
    void setTrackHeight (int newH, int anchorY = -1);
    int maxTrackHeight() const;                                   // half the screen height

    void zoomPxPerBeat (float notches, int anchorX = -1);         // horizontal zoom, one step per notch
    void setPxPerBeat (double newPpb, int anchorX = -1);          // anchorX is in lane content pixels

    // per-track heights: each track may have its own row height (dragged border);
    // 0 means "follow the session default" (AppState::trackHeight)
    void setTrackRowHeight (int trackId, int newH);   // one track only
    void setMasterRowHeight (int newH);               // the pinned master bus row
    void setAllTrackHeights (int newH);               // presets: every track, clears per-track overrides
    void setDefaultTrackHeight (int newH);            // global zoom: scales the per-track overrides too
    void relayoutTrackHeights (int anchorY = -1);

    static constexpr int kRulerH = 38;
    static constexpr int kHeaderW = 212;
    static constexpr int kAutoH = 70;
    static constexpr int kCompactTrackH = 54;   // below this the name and toggles share one row
    static constexpr int kTempoH = 72;
    static constexpr int kChordH = 44;          // the chord track row (Studio One style)
    static constexpr int kMarkerH = 30;         // the marker / arrangement section row
    int tempoLaneH() const { return app.showTempoLane ? kTempoH : 0; }
    int chordLaneH() const { return app.showChordTrack ? kChordH : 0; }
    int markerLaneH() const { return app.showMarkerTrack ? kMarkerH : 0; }
    // the pinned rows above the scrolling tracks, top to bottom: tempo, chords, markers
    int chordLaneY() const { return kRulerH + tempoLaneH(); }
    int markerLaneY() const { return chordLaneY() + chordLaneH(); }
    int masterRowY() const { return markerLaneY() + markerLaneH(); }
    int pinnedRowsH() const { return tempoLaneH() + chordLaneH() + markerLaneH(); }
    // the pinned master bus row above the tracks (0 when the row is hidden with View > Master Bus Track)
    int masterRowH() const;
    juce::Rectangle<int> masterRowBounds() const;   // the area the row occupies (empty when hidden)
    juce::Component* masterLaneComponent() const;   // the lane sub-rows of the master row (self test)
    juce::Component* masterHeaderComponent() const; // the master row's header (self test)

    // scroll introspection (self test): the header column and the lanes have to stay in step, and
    // they must move in the same call - a timer-only sync leaves them apart for a few frames
    int laneScrollY() const { return laneViewport.getViewPositionY(); }
    int headerScrollY() const { return headerViewport.getViewPositionY(); }
    void scrollLanesTo (int y) { laneViewport.setViewPosition (laneViewport.getViewPositionX(), juce::jmax (0, y)); }
    void scrollLanesTo (int x, int y) { laneViewport.setViewPosition (juce::jmax (0, x), juce::jmax (0, y)); }
    /** The bar ruler (self test: a drag across it has to move the transport). */
    juce::Component* rulerComponent() const;
    /** The header component of a track (nullptr when there is none). */
    juce::Component* trackHeaderComponent (int trackId) const;
    /** The row height the arrangement currently gives a track (0 = hidden / folded away). */
    int trackRowHeight (int trackId) const;
    /** The scrolling lane component and one track row in its content coordinates (self test). */
    juce::Component* lanesComponent() const;
    juce::Rectangle<int> trackRowBounds (int trackId) const;
    /** Where the fader's level meter sits in the header (self test). */
    juce::Rectangle<int> trackHeaderMeterBounds (int trackId) const;
    bool trackHeaderMeterVisible (int trackId) const;
    juce::Component* chordLaneComponent() const;    // the chord track lane (self test)
    juce::Component* markerLaneComponent() const;   // the marker track lane (self test)
    juce::Rectangle<int> chordLaneBounds() const;   // empty when the row is hidden
    juce::Rectangle<int> markerLaneBounds() const;
    juce::Rectangle<int> trackAreaBounds() const;   // where the scrolling track rows start (self test)

    // layout helpers shared by inner classes
    int rowTop (int index) const { return index < (int) rowY.size() ? rowY[(size_t) index] : (rowY.empty() ? 0 : rowY.back()); }
    // automation rows scale with the track, so zooming keeps the ratios intact
    int trackH (const Track& t) const { return t.effectiveHeight (app.trackHeight); }
    int autoH (const Track& t) const { return automationRowHeight (trackH (t)); }
    int rowHeight (const Track& t) const { return t.shownInArranger() ? trackH (t) + t.laneRowCount() * autoH (t) : 0; }
    int laneRowTop (int trackRow, int laneIdx) const
    {
        if (trackRow < 0 || trackRow >= (int) app.project.tracks.size()) return 0;
        auto& t = *app.project.tracks[(size_t) trackRow];
        return rowTop (trackRow) + trackH (t) + laneIdx * autoH (t);
    }
    int totalRowsHeight() const { return rowY.empty() ? 0 : rowY.back(); }
    int trackIndexAtY (int y) const;
    double xToBeat (double x) const { return x / app.pxPerBeat; }
    double beatToX (double b) const { return b * app.pxPerBeat; }
    int scrollX() const;

    // drag-to-reorder state shared between the track headers and the lane/header painters
    int reorderTrackId = 0;      // track being dragged (0 = not dragging)
    int reorderDropIndex = -1;   // insertion index while dragging (-1 = no indicator)
    int dropIndexForScreenY (int screenY) const;
    void showReorderDrop (int index);
    void endTrackReorder (bool commit);
    void repaintClipArea();     // repaint the lanes + track headers (clip visuals)
    std::function<void (int trackId, int clipId)> onEditClipEnvelope;   // open the fullscreen gain editor

    AppState& app;

    class Ruler;
    class TempoLane;
    class ChordLane;
    class MarkerLane;
    class Lanes;
    class Headers;
    class TrackHeader;
    class MasterAutoLane;

private:
    void projectContentChanged() override;    void projectMixChanged() override;
    void projectStructureChanged() override;
    void selectionChanged() override;
    void viewChanged() override;
    void timerCallback() override;
    void layoutRows();
    void layoutMasterRow();
    void updateContentSize();
    void rebuildHeaders();
    /** Moves the header column, ruler, tempo / chord / marker rows and the pinned master row to
        the lane viewport's scroll position right away (called on every scroll, not on a timer). */
    void syncScrollNow();
    void applyToSelectedAudio (const std::function<void (AudioClip&)>& fn);
    void mergeMidiClips (int trackId, const std::vector<MidiClip*>& clips);
    void mergeAudioClips (int trackId, const std::vector<AudioClip*>& clips);


    std::vector<int> rowY;
    int laidOutMasterRowH = 0;   // the pinned master row height the current layout reserves
    double cursorBeat = -1.0;                 // last mouse position in the lanes (beats)
    std::unique_ptr<Ruler> ruler;
    std::unique_ptr<TempoLane> tempoLane;
    std::unique_ptr<ChordLane> chordLane;           // the chord track row above the arrangement
    std::unique_ptr<MarkerLane> markerLane;         // the marker (arrangement section) row
    std::unique_ptr<MasterAutoLane> masterLanes;   // lane rows of the pinned master bus row
    std::unique_ptr<TrackHeader> masterHeader;     // the master bus row header (same style as tracks)
    std::unique_ptr<Lanes> lanes;
    std::unique_ptr<Headers> headers;
    /** A viewport that tells the arrange view the moment it scrolls, so the track headers, the
        ruler and the pinned rows move in the same frame as the lanes (a 30 Hz timer alone makes
        them trail behind, which looks like the clips are out of line with the headers). */
    class ScrollViewport : public juce::Viewport
    {
    public:
        std::function<void()> onScroll;
        void visibleAreaChanged (const juce::Rectangle<int>&) override { if (onScroll) onScroll(); }
    };
    ScrollViewport laneViewport, headerViewport;
    IconButton addTrackBtn { Icon::Plus, TRANS ("Add Track"), theme::accent };
    IconButton chordBtn { Icon::Chord, TRANS ("Chord Track - show / hide the chord track"), theme::accent };
    IconButton markerBtn { Icon::Marker, TRANS ("Marker Track - show / hide the arrangement sections"), theme::accent };
    juce::int64 lastPlayX = -1;
    // the x the playhead is DRAWN at: the timer invalidates a small column for this exact
    // position, so the paint must use the same value - reading the live position inside the
    // paint would draw outside the repainted column (a playhead that flickers / leaves trails
    // whenever the message thread paints late)
    juce::int64 playXForPaint = -1;
    juce::Point<int> lastSyncedScroll { -1, -1 };
    bool syncingScroll = false;
    float trackZoomAccum = 0.0f;   // fractional wheel travel waiting to become a step
    float zoomAccum = 0.0f;        // same, for the horizontal (timeline) zoom
    // the chord the notes play, probed for the chord track header (cached per 16th note)
    double probedChordBeat = -1.0;
    juce::String probedChordName;

    struct ClipCopy { bool audio = false; int trackId = 0; MidiClip midi; AudioClip audioClip; };
    std::vector<ClipCopy> clipboard;
};
}
