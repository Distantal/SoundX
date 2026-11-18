#pragma once

#include "AppState.h"
#include "Widgets.h"

namespace sx
{
//-- curve shapes -------------------------------------------------------------------------------
// What the pencil draws into a controller lane. Free = draw by hand (the old behaviour); the
// other entries fill the dragged range with one shape between the drag's two values, so a bend
// or a vibrato can be drawn in one gesture instead of by hand.
enum class CurveShape { Free = 0, Line, Sine, Triangle, Saw };

/** Value of a shape at t (0 = the drag's start, 1 = its end) between the values 'from' and 'to'.

    Line is a plain ramp. Sine and triangle run one full shape across the drag: they start and end
    on 'from' and reach 'to' in the middle (a sine glides, a triangle has straight sides). The
    sawtooth repeats once per beat (at least once) and jumps back to 'from' after every tooth.
*/
double curveShapeValue (CurveShape shape, double t, double from, double to, double beats);

class PianoRollView : public juce::Component,
                      private Project::Listener,
                      private AppState::Listener,
                      private juce::Timer
{
public:
    explicit PianoRollView (AppState& a);
    ~PianoRollView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    enum class Tool { Pointer, Pencil, Eraser, Scissors };

    // the shape the pencil draws in a controller lane (Free = hand-drawn)
    CurveShape curveShape = CurveShape::Free;
    void setCurveShape (CurveShape s);
    int trackListWidth() const;   // 0 when the sidebar is hidden

    // editing commands (called from the main window shortcuts)
    void deleteSelection();
    void selectAll();
    void quantizeSelection();
    void copySelection();
    void cutSelection() { copySelection(); deleteSelection(); }
    void pasteAtPlayhead();
    void duplicateSelection();
    void transposeSelection (int semitones);
    void nudgeSelection (double beats);
    void reverseSelection();                 // reverse the selected phrase in time
    void setSlideOnSelection (bool slide);   // FL-style slide notes
    bool slideNoteMode() const { return slideMode; }
    void setTool (Tool t);
    MidiClip* clip() const;

    //-- humanize ------------------------------------------------------------------------------
    // Every value a humanise pass starts from. A dialog keeps one while it is open, so clicking
    // Randomize a few times re-draws the same notes instead of drifting further each time, and the
    // whole session is one undo step.
    struct HumanizeSession
    {
        humanize::Seed seed;
        bool checkpointed = false;
    };
    HumanizeSession captureHumanizeSession() const;
    // Applies the settings to the selected notes (every note of the clip when nothing is selected,
    // the same rule Quantize uses) and returns how many notes changed. The first apply of a session
    // pushes the undo step.
    int applyHumanize (const HumanizeSettings& s, HumanizeSession& session);
    // the one-shot version (menu / H shortcut): one apply in a fresh session
    void humanizeSelection (const HumanizeSettings& s);
    void showHumanizeDialog();

    // fullscreen toggle: fired when the toolbar button is clicked (MainComponent owns the layout)
    std::function<void()> onFullscreenToggle;
    void setFullscreen (bool f);

    // anchored zoom: keeps the beat/pitch under the anchor fixed on screen.
    // anchorX/anchorY are content coordinates; -1 picks the anchor automatically
    // (selected notes, then the visible playhead, then the view centre).
    void zoomHorizontal (double factor, int anchorX = -1);
    void zoomVertical (int delta, int anchorY = -1);

    static constexpr int kKeysW = 62;
    static constexpr int kRulerH = 22;
    static constexpr int kChordStripH = 20;   // the chord row above the time ruler
    static constexpr int kLaneH = 118;
    static constexpr int kToolbarH = 32;

    //-- chord row ------------------------------------------------------------------------------
    // One entry of the chord row, worked out from the notes of the project only: it runs from the
    // moment its chord starts sounding up to the moment another chord takes over, so the blocks
    // follow the notes instead of a grid. The arrangement's chord track plays no part in it.
    struct ChordSegment
    {
        double beat = 0.0;
        double length = 0.0;
        juce::String name;       // empty when the notes there are no chord, or on a rest
        bool sounding = false;   // false = nothing sounds in this range
    };
    // the chord row between two absolute beats
    std::vector<ChordSegment> buildChordSegments (double fromAbs, double toAbs) const;
    // the chord that sounds at an absolute timeline position (empty when nothing sounds there)
    juce::String chordNameAt (double absBeat) const;

    class Keys;
    class Grid;
    class Lane;
    class TimeRuler;
    class TrackList;
    class ChordStrip;

    // every selected MIDI clip: shown in the piano roll, but only the editable one can be changed
    struct ClipRef { int trackId = 0; int clipId = 0; };
    std::vector<ClipRef> ghosts;
    std::unique_ptr<TrackList> trackList;
    bool trackListVisible = true;
    void rebuildTrackList();
    void setEditableClip (int trackId, int clipId);

    AppState& app;
    juce::Viewport gridViewport;
    double pxPerBeat = 90.0;
    int keyH = 14;
    double grid = 0.25;
    Tool tool = Tool::Pointer;
    std::set<int> selNotes, selCC;
    bool slideMode = false;
    // visible lower lanes, stacked top to bottom: -1 = velocity, otherwise a controller number
    std::vector<int> laneControllers { -1 };
    // requested pixel height of each stacked lane (one entry per laneControllers element)
    std::vector<int> laneHeights { kLaneH };
    int laneHeightAt (int index) const { return index >= 0 && index < (int) laneHeights.size() ? laneHeights[(size_t) index] : kLaneH; }
    int lanePanelTotal() const;                 // sum of the requested lane heights
    int lanePanelCap() const;                   // most of the lower panel the CC area may take
    void setLaneHeight (int index, int h);
    void resizeLanePanel (int newTotal);        // drag the top border of the CC panel
    bool hasLane (int controller) const;
    void toggleLane (int controller);
    int lastVelocity = 100;
    double lastLength = 0.25;

    juce::Component* gridComp() { return (juce::Component*) gridPtr.get(); }
    juce::Component* laneComp() { return (juce::Component*) lane.get(); }
    juce::Component* rulerComp() { return (juce::Component*) timeRuler.get(); }   // the bar ruler (self test)
    int scrollX() const { return gridViewport.getViewPositionX(); }
    int scrollY() const { return gridViewport.getViewPositionY(); }
    void ensureClipCovers (MidiClip& c, double endBeat);
    /** Cuts a note in two at a clip-relative beat (one undo step). */
    void splitNoteAt (int noteIndex, double cutBeat);
    void changed();
    void laneChanged();

private:
    void projectContentChanged() override;
    void editorClipChanged() override;
    void selectionChanged() override;
    void viewChanged() override;
    void timerCallback() override;
    void updateLaneButton();
    void showLaneMenu();
    void showCurveShapeMenu();
    void fitToNotes();
    void updateContentSize();

    std::unique_ptr<Keys> keys;
    std::unique_ptr<Grid> gridPtr;
    std::unique_ptr<Lane> lane;
    std::unique_ptr<TimeRuler> timeRuler;
    std::unique_ptr<ChordStrip> chordStrip;
    int rulerTop() const { return kToolbarH + (chordStripShown() ? kChordStripH : 0); }
    bool chordStripShown() const { return app.pianoRollChords; }

    IconButton pointerBtn { Icon::Pointer, TRANS ("Pointer Tool") };
    IconButton pencilBtn { Icon::Pencil, TRANS ("Pencil Tool") };
    IconButton eraserBtn { Icon::Eraser, TRANS ("Eraser Tool") };
    IconButton scissorsBtn { Icon::Scissors, TRANS ("Scissors (5): click a note to cut it in two at that point") };
    IconButton trackListBtn { Icon::Menu, TRANS ("Track list - choose the editable track") };
    juce::ComboBox gridBox;
    juce::TextButton quantizeBtn { TRANS ("Quantize") };
    IconButton humanizeBtn { Icon::Dice, TRANS ("Humanize: randomise the velocity of the selected notes (H, Shift+H sets the amount)") };
    juce::TextButton slideBtn { "SLIDE" };
    juce::TextButton laneBtn;
    juce::TextButton shapeBtn;   // the curve the pencil draws into the lane (Free / Line / Sine ...)
    IconButton zoomInBtn { Icon::Plus, TRANS ("Zoom In (horizontal)") };
    juce::TextButton zoomOutBtn { "-" };
    IconButton vZoomInBtn { Icon::Plus, TRANS ("Zoom In (vertical)") };
    IconButton vZoomOutBtn { Icon::Minus, TRANS ("Zoom Out (vertical)") };
    IconButton chordBtn { Icon::Chord, TRANS ("Chord row: the chord the notes play, read from the notes of the arrangement") };
    IconButton fullscreenBtn { Icon::Expand, TRANS ("Fullscreen (F11)") };
    juce::String title;
    juce::int64 lastPlayX = -1;
    // the x the playhead is DRAWN at (kept in step with the invalidated columns - see
    // ArrangeView); reading the live position inside the paint made it flicker / ghost
    juce::int64 playXForPaint = -1;
    int lastScrollX = -1, lastScrollY = -1;
    juce::Random humanizeRng;

    struct NoteCopy { Note n; };
    std::vector<Note> noteClipboard;
    std::vector<CCPoint> ccClipboard;
};

// the humanize dialog's panel without opening it (self test / visual check)
juce::Image renderHumanizePanel (const HumanizeSettings& s, int selectedNotes, int clipNotes);
}
