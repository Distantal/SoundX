#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>
#include <cmath>
#include <memory>
#include <set>
#include <vector>
#include <map>

#include "Chord.h"

namespace sx
{
constexpr int kCCPitchBend = 128;
constexpr int kCCAftertouch = 129;

juce::String controllerName (int cc);
int controllerMax (int cc);
int controllerDefault (int cc);

/** Controllers that physically spring back to the middle - pitch bend is the one that matters.

    Wherever a clip holds no anchor for such a controller, its value is the default (centre): the
    lane draws it that way and the engine / MIDI export send it that way, so editing one part of a
    clip never bends the untouched parts of it. Ordinary controllers keep their last value instead,
    which is how MIDI plays them.
*/
inline bool controllerRestsAtDefault (int cc) { return cc == kCCPitchBend; }

// gap between the last pitch-bend anchor and the centre event that ends it (beats, ~0.5 ms)
inline constexpr double kControllerRestGap = 1.0e-3;

struct Note
{
    int id = 0;
    int pitch = 60;
    double start = 0.0;   // beats, relative to the clip
    double length = 1.0;  // beats
    int velocity = 100;
    bool slide = false;   // FL-style slide / portamento note: start at the previous note's pitch and glide here
};

struct CCPoint
{
    int id = 0;
    int cc = 1;           // 0-127 controller, 128 pitch bend, 129 channel pressure
    double beat = 0.0;    // relative to the clip
    int value = 0;
};

//== humanize -----------------------------------------------------------------------------------
// Randomising the values of notes ("humanising") is what makes a programmed pattern sound played:
// each note gets its own small deviation instead of sitting exactly where the pencil put it.
// Velocity is the one that matters most - a hi-hat or a piano chord at one fixed velocity sounds
// like a machine. The timing offset moves a note off the grid by a random number of MIDI ticks,
// which is what turns a block chord into a strum.
struct HumanizeSettings
{
    int velocityRange = 20;   // ± velocity steps (0 = leave the velocity alone)
    int timingTicks = 0;      // ± MIDI ticks at 960 per beat (0 = leave the position alone)
    // true:  most notes stay close to where they were and only a few move far (played feel)
    // false: every deviation in the range is equally likely (Cubase's randomise)
    bool bell = true;

    bool empty() const { return velocityRange <= 0 && timingTicks <= 0; }
    bool operator== (const HumanizeSettings& o) const
    {
        return velocityRange == o.velocityRange && timingTicks == o.timingTicks && bell == o.bell;
    }
    bool operator!= (const HumanizeSettings& o) const { return ! operator== (o); }
};

inline constexpr double kTicksPerBeat = 960.0;   // tick resolution of the MIDI files this app writes

namespace humanize
{
    // One random deviation in [-range, +range].
    inline int offset (juce::Random& rng, int range, bool bell)
    {
        if (range <= 0) return 0;
        if (! bell) return rng.nextInt (2 * range + 1) - range;
        // three uniform draws averaged: a bell around zero whose tails still reach ±range
        const double u = (rng.nextDouble() + rng.nextDouble() + rng.nextDouble()) / 3.0;
        const double v = juce::jlimit (-1.0, 1.0, (u - 0.5) * 3.0);
        return (int) std::lround (v * (double) range);
    }

    // The values a note had before it was humanised. Re-drawing from these (instead of from the
    // note's current values) is what keeps the deviations from piling up when the editor
    // randomises the same notes a few times in a row.
    struct NoteSeed
    {
        int velocity = 100;
        double start = 0.0;
    };
    using Seed = std::map<int, NoteSeed>;

    inline void capture (const std::vector<Note>& notes, Seed& seed)
    {
        seed.clear();
        for (auto& n : notes) seed[n.id] = NoteSeed { n.velocity, n.start };
    }

    inline NoteSeed seedFor (const Note& n, const Seed* seed)
    {
        if (seed != nullptr)
        {
            const auto it = seed->find (n.id);
            if (it != seed->end()) return it->second;
        }
        return NoteSeed { n.velocity, n.start };
    }

    // Applies the settings to every note whose id is in 'ids' (an empty 'ids' = every note) and
    // returns how many notes were changed. 'seed' (optional) makes the pass relative to values
    // captured earlier instead of the note's own.
    inline int apply (std::vector<Note>& notes, const std::set<int>& ids,
                      const HumanizeSettings& s, juce::Random& rng, const Seed* seed = nullptr)
    {
        if (s.empty()) return 0;
        const double beatsPerTick = 1.0 / kTicksPerBeat;
        int changed = 0;
        for (auto& n : notes)
        {
            if (! ids.empty() && ids.count (n.id) == 0) continue;
            const auto from = seedFor (n, seed);
            if (s.velocityRange > 0)
                n.velocity = juce::jlimit (1, 127, from.velocity + offset (rng, s.velocityRange, s.bell));
            if (s.timingTicks > 0)
                n.start = juce::jmax (0.0, from.start + (double) offset (rng, s.timingTicks, s.bell) * beatsPerTick);
            ++changed;
        }
        return changed;
    }
}

struct MidiClip
{
    int id = 0;
    juce::String name;
    double start = 0.0;   // beats on the timeline
    double length = 4.0;  // beats
    bool muted = false;
    std::vector<Note> notes;
    std::vector<CCPoint> ccs;

    //-- takes ----------------------------------------------------------------------------------
    // Recording over the same range again keeps the earlier passes as takes: they stay in the
    // project (and in the undo history) but only the active one is played and drawn.
    juce::int64 takeGroup = 0;      // 0 = not part of a take stack
    int takeIndex = 0;              // 0-based; shown as "Take 1 / 3"
    bool takeActive = true;
};

/** The controller / pitch-bend messages a clip's data stands for, at clip-relative beats.

    Every anchor becomes one message, and a controller that rests at its default (pitch bend) also
    gets that default before its first anchor - when that anchor starts later than the clip - and
    right after its last anchor, so the bend of one gesture ends where the gesture ends instead of
    leaning on the rest of the clip. Notes are not included. Both the engine and the MIDI export
    go through this, so what the lane draws is what is played and written.
*/
/** Record quantize: snaps the starts of freshly recorded notes onto the grid (0 = no-op). Note
    lengths are left alone so the take keeps its phrasing; nothing shrinks below a 128th note. */
void quantizeRecordedNotes (std::vector<Note>& notes, double grid);

/** One pass of a loop recording: where it starts in the recorded file and where it belongs on the
    timeline. Used to turn a cycle recording into one take per pass. */
struct LoopTakeSegment
{
    juce::int64 startSample = 0;    // into the recorded file
    juce::int64 lengthSamples = 0;
    double startBeat = 0.0;         // timeline position of this pass
};

/** Splits a loop recording into passes: the first one may be short (recording started mid-cycle),
    every later pass is exactly one cycle. Trailing fragments below half a pass are dropped. */
std::vector<LoopTakeSegment> splitLoopRecording (juce::int64 writtenSamples, juce::int64 samplesTillLoopEnd,
                                                 juce::int64 passSamples, double recordStartBeat,
                                                 double loopStartBeat);

void appendControllerMessages (const MidiClip& clip, int channel,
                               std::vector<std::pair<double, juce::MidiMessage>>& out);

struct AudioData
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    juce::File file;
    // one entry per kPeakBlock samples; peakMin/peakMax is the envelope of all channels mixed
    // (what a mono-looking waveform needs), the per-channel vectors are filled for stereo files so
    // the arrangement can draw the left and right channel as two separate lanes
    std::vector<float> peakMin, peakMax;
    std::vector<std::vector<float>> channelPeakMin, channelPeakMax;
    static constexpr int kPeakBlock = 128;

    double lengthSeconds() const { return sampleRate > 0 ? buffer.getNumSamples() / sampleRate : 0.0; }
    bool hasChannelPeaks() const { return ! channelPeakMin.empty() && channelPeakMin.size() == channelPeakMax.size(); }
    void buildPeaks();
};

// one point of an audio clip's gain envelope (beat is relative to the clip start)
struct ClipGainPoint
{
    double beat = 0.0;
    float db = 0.0f;
};

// value of a clip gain envelope at a clip-relative beat (flat 'fallbackDb' when empty)
inline float evalClipGainDb (const std::vector<ClipGainPoint>& pts, double beat, float fallbackDb)
{
    if (pts.empty()) return fallbackDb;
    if (beat <= pts.front().beat) return pts.front().db;
    if (beat >= pts.back().beat) return pts.back().db;
    for (size_t i = 1; i < pts.size(); ++i)
        if (beat <= pts[i].beat)
        {
            const auto& a = pts[i - 1];
            const auto& b = pts[i];
            const double d = b.beat - a.beat;
            const double t = d > 1.0e-9 ? (beat - a.beat) / d : 0.0;
            return (float) (a.db + (b.db - a.db) * t);
        }
    return pts.back().db;
}

// splits a clip gain envelope at a clip-relative beat (used when an audio clip is cut):
// 'left' keeps everything before the cut plus a boundary point at it, 'right' is
// re-anchored to its new start and starts with the same value, so the curve joins up
inline void splitClipGainEnv (const std::vector<ClipGainPoint>& src, double cutBeat, float fallbackDb,
                              std::vector<ClipGainPoint>& left, std::vector<ClipGainPoint>& right)
{
    left.clear();
    right.clear();
    if (src.empty()) return;
    const float seamDb = evalClipGainDb (src, cutBeat, fallbackDb);
    for (auto& p : src)
    {
        if (p.beat < cutBeat - 1.0e-9) left.push_back (p);
        else if (p.beat > cutBeat + 1.0e-9) right.push_back ({ juce::jmax (0.0, p.beat - cutBeat), p.db });
    }
    left.push_back ({ cutBeat, seamDb });
    right.insert (right.begin(), { 0.0, seamDb });
}

// Timebase of an audio clip (Studio One calls it the event timebase):
//   Bars    - the clip sits on a musical position; a tempo change moves it in seconds
//   Seconds - the clip sits on an absolute time; a tempo change moves it in bars
enum class Timebase { Bars = 0, Seconds = 1 };

/** The gain a fade applies at position t (0 at the silent end, 1 at the full end) for one of the
    curve shapes. Shared by the render and by the curve drawn on the clip, so what you see is what
    you hear. */
inline double fadeShape (int curve, double t)
{
    t = juce::jlimit (0.0, 1.0, t);
    switch (curve)
    {
        case 0:  return t;                                          // linear
        case 2:  return std::sin (1.5707963267948966 * t);          // equal power
        case 3:  return t * t;                                      // exponential (slow start)
        case 4:  return 1.0 - (1.0 - t) * (1.0 - t);                // logarithmic (fast start)
        default: return t * t * (3.0 - 2.0 * t);                    // smooth S-curve
    }
}

struct AudioClip
{
    int id = 0;
    juce::String name;
    juce::File file;
    double start = 0.0;       // beats
    double lengthSec = 1.0;   // timeline duration (already includes 'stretch')
    double offsetSec = 0.0;   // where the used source material begins
    float gainDb = 0.0f;
    double fadeIn = 0.0;      // seconds
    double fadeOut = 0.0;     // seconds
    // shape of the two ramps: 0 linear, 1 smooth S-curve (default), 2 equal power,
    // 3 exponential, 4 logarithmic
    int fadeInCurve = 1, fadeOutCurve = 1;
    bool muted = false;
    // Studio One style event timebase: Bars keeps the clip on its musical position (a tempo change
    // moves it in seconds); Seconds glues it to absolute time (a tempo change moves it in bars).
    Timebase timebase = Timebase::Bars;
    // curve used when this clip is the earlier half of a crossfade:
    // 0 = equal power (cos / sin), 1 = linear, 2 = symmetric S-curve
    int xfadeCurve = 0;
    // per-clip gain envelope: the line is off by default, points are clip-relative beats
    bool gainLine = false;
    std::vector<ClipGainPoint> gainEnv;
    std::shared_ptr<AudioData> data;
    // imported from a file and still being decoded in the background: the clip is already on the
    // timeline (so the drop feels instant), playback skips it until the samples arrive
    bool loading = false;

    //-- audio editing (non-destructive, saved with the project) --------------------------------
    // plays the used source segment backwards (fades / gain envelope stay where they are)
    bool reversed = false;
    // timeline duration / source duration: 2 = half speed (twice as long), 0.5 = double speed.
    // lengthSec is kept in sync, so the rest of the code keeps working in timeline seconds.
    double stretch = 1.0;
    // transpose in semitones, independent of the speed (time-stretch + resample, duration kept)
    float pitchSemitones = 0.0f;

    //-- takes ----------------------------------------------------------------------------------
    // Recording over the same range again keeps the earlier passes as takes (see MidiClip).
    juce::int64 takeGroup = 0;
    int takeIndex = 0;
    bool takeActive = true;

    // off-line render of the stretched / transposed segment (derived, never saved)
    std::shared_ptr<AudioData> rendered;
    // the parameters the render was built from, so it is rebuilt exactly when they change
    struct RenderKey
    {
        const AudioData* source = nullptr;
        double offsetSec = -1.0, sourceLenSec = -1.0, stretch = 1.0, pitch = 0.0;
        bool matches (const AudioClip& c) const;
    };
    RenderKey renderKey;

    double sourceLengthSec() const { return stretch > 1.0e-9 ? lengthSec / stretch : lengthSec; }
    // source seconds consumed by one second on the timeline
    double sourceRate() const { return 1.0 / (stretch > 1.0e-9 ? stretch : 1.0); }
    bool isStretched() const { return std::abs (stretch - 1.0) > 1.0e-9 || std::abs (pitchSemitones) > 1.0e-5; }
    // the buffer that actually plays: the rendered one when stretch / pitch are in use
    const AudioData* playbackData() const
    {
        return (isStretched() && rendered != nullptr && renderKey.matches (*this)) ? rendered.get() : data.get();
    }
    bool hasEdit() const { return reversed || isStretched(); }
};

// clip-relative seconds -> seconds into its playback buffer (handles stretch and reverse)
inline double clipBufferSeconds (const AudioClip& c, double regionSec)
{
    const auto* d = c.playbackData();
    const double dataLen = d != nullptr ? d->lengthSeconds() : juce::jmax (0.0, c.lengthSec);
    if (c.lengthSec <= 1.0e-9) return 0.0;
    const bool rendered = c.isStretched() && c.playbackData() == c.rendered.get();
    if (rendered)
    {
        const double t = juce::jlimit (0.0, 1.0, regionSec / c.lengthSec) * dataLen;
        return c.reversed ? dataLen - t : t;
    }
    // source data: the used segment is sourceLengthSec long and starts at offsetSec
    const double srcLen = juce::jmin (c.sourceLengthSec(), juce::jmax (0.0, dataLen - c.offsetSec));
    const double rate = srcLen / juce::jmax (1.0e-9, c.lengthSec);
    if (c.reversed) return c.offsetSec + srcLen - rate * regionSec;
    return c.offsetSec + rate * regionSec;
}

// builds AudioClip::rendered for the current stretch / pitch (returns true when a render exists)
bool ensureClipPlayback (AudioClip& clip);
void invalidateClipRender (AudioClip& clip);

struct PluginSlot
{
    juce::int64 uid = 0;
    juce::PluginDescription desc;
    juce::MemoryBlock state;
    juce::MemoryBlock araState;   // ARA document archive (pitch / audio edits of Melodyne & co)
    bool bypass = false;
    bool builtIn = false;
    bool postFader = false;   // master only: run after the master fader instead of before it
    // instrument slots can be stacked on one track: these balance the layers against each other
    // (they are applied to that layer's output only, before the track's inserts and fader)
    float gainDb = 0.0f;
    float pan = 0.0f;
    // side-chain source: the id of the track whose audio feeds this plug-in's detector (0 = none)
    int sidechainTrack = 0;
    juce::String displayName() const { return desc.name; }
};

struct AutoPoint
{
    double beat = 0.0;
    float value = 0.0f; // normalised 0..1
};

struct AutoLane
{
    // "vol", "pan", or "p:<slotUid>:<paramIndex>"
    juce::String target;
    juce::String label;
    std::vector<AutoPoint> points;   // always sorted, strictly increasing in beat (normalizeAutoPoints)
    bool visible = false;   // shown as its own row under the track in the arrange view
};

// -- automation curve -------------------------------------------------------------------------
// An automation curve is a function of time: nodes are kept sorted by beat with strictly
// increasing beats, and the value between two nodes is a straight segment. The curve therefore
// only ever rises or falls monotonically between its nodes, never doubles back in time and
// never overshoots the two node values (no spline ringing). The editor and the audio engine both
// go through these helpers, so what is drawn is exactly what is heard.
// The 1e-4 beat gap is the smallest distance two nodes may have: it keeps every segment
// non-degenerate (a vertical jump would be two values at one instant).
inline void normalizeAutoPoints (std::vector<AutoPoint>& pts)
{
    std::stable_sort (pts.begin(), pts.end(), [] (const AutoPoint& a, const AutoPoint& b) { return a.beat < b.beat; });
    std::vector<AutoPoint> out;
    out.reserve (pts.size());
    for (auto& p : pts)
    {
        p.beat = juce::jmax (0.0, p.beat);
        if (! out.empty() && p.beat - out.back().beat < 1.0e-4)
        {
            out.back() = p;          // two nodes on one beat: the latest value wins
            continue;
        }
        out.push_back (p);
    }
    pts = std::move (out);
}

// adds a node and returns its index; clicking exactly on an existing beat edits that node
// instead of stacking a second one there (which is what kept the old curve from being monotone)
inline int insertAutoPoint (std::vector<AutoPoint>& pts, double beat, float value)
{
    normalizeAutoPoints (pts);
    beat = juce::jmax (0.0, beat);
    value = juce::jlimit (0.0f, 1.0f, value);
    for (int i = 0; i < (int) pts.size(); ++i)
        if (std::abs (pts[(size_t) i].beat - beat) < 1.0e-3)
        {
            pts[(size_t) i].value = value;
            return i;
        }
    auto it = std::upper_bound (pts.begin(), pts.end(), beat, [] (double b, const AutoPoint& p) { return b < p.beat; });
    const int index = (int) (it - pts.begin());
    pts.insert (it, AutoPoint { beat, value });
    return index;
}

// moves a node, clamped between its neighbours so the curve stays monotone in time
inline void moveAutoPoint (std::vector<AutoPoint>& pts, int index, double beat, float value)
{
    if (index < 0 || index >= (int) pts.size()) return;
    constexpr double eps = 1.0e-4;
    const double lo = index > 0 ? pts[(size_t) index - 1].beat + eps : 0.0;
    const double hi = index + 1 < (int) pts.size() ? pts[(size_t) index + 1].beat - eps : 1.0e9;
    pts[(size_t) index].beat = juce::jlimit (lo, juce::jmax (lo, hi), juce::jmax (0.0, beat));
    pts[(size_t) index].value = juce::jlimit (0.0f, 1.0f, value);
}

// value of the curve at 'beat' (-1 when the lane is empty, i.e. "no automation")
inline float evalAutoLane (const std::vector<AutoPoint>& pts, double beat)
{
    if (pts.empty()) return -1.0f;
    if (beat <= pts.front().beat) return pts.front().value;
    if (beat >= pts.back().beat) return pts.back().value;
    auto it = std::upper_bound (pts.begin(), pts.end(), beat, [] (double b, const AutoPoint& p) { return b < p.beat; });
    const auto& p1 = *it;
    const auto& p0 = *(it - 1);
    const double span = p1.beat - p0.beat;
    if (span <= 1.0e-9) return p1.value;
    const double t = juce::jlimit (0.0, 1.0, (beat - p0.beat) / span);
    return p0.value + (float) (t * (double) (p1.value - p0.value));
}

// height of one automation row under a track (the master bus keeps these sub-rows): it scales
// with the track so every zoom step keeps the lanes in proportion (a 23 px track gets a 21 px
// lane, not a fixed 36 px row that would be taller than the track itself)
inline int automationRowHeight (int trackHeight)
{
    return juce::jmax (20, (int) std::round ((double) trackHeight * 70.0 / 78.0));
}

enum class AutoMode { Off = 0, Read = 1, Touch = 2 };
// Bus = a group channel that sums the tracks routed into it; Send = an effect return that
// receives sends. Both have inserts and a fader and feed the master bus.
// Automation = a stand-alone automation track: it is not an audio channel but a parameter curve
// of another track (or of the master) shown as a row of its own in the arranger.
enum class TrackType { Instrument = 0, Audio = 1, Bus = 2, Send = 3, Automation = 4 };

// one send from a track into a bus / effect-return track
struct SendSlot
{
    int target = 0;          // track id of a Bus or Send track
    float levelDb = -6.0f;
    bool preFader = false;
};

// -- chord track -------------------------------------------------------------------------------
// One chord symbol on the timeline, drawn as its own small block (like a note in the piano roll):
// it covers [beat, beat + length) and blocks never overlap. 'symbol' is kept exactly as it was
// typed ("Cmaj7", "Am7/G", even "fill"): the chord track shows that text, and chord::parse()
// decides what harmony it implies. Chords are project-wide (like the tempo map), not owned by a
// track.
struct ChordEvent
{
    int id = 0;
    double beat = 0.0;       // timeline position
    double length = 4.0;     // beats; the block never runs into the chord after it
    juce::String symbol;
};

inline constexpr double kMinChordBeats = 0.25;   // shortest block a chord can be shrunk to

// -- marker track (arrangement sections) -------------------------------------------------------
// A named section of the song (Intro / Verse / Chorus ...) drawn as a range on the marker track.
// Sections never overlap: a section ends where the next one begins at the latest.
struct SectionMarker
{
    int id = 0;
    juce::String name;
    double beat = 0.0;        // start of the section
    double length = 16.0;     // beats
    juce::Colour colour;
};

inline constexpr double kMinSectionBeats = 0.5;

// colours a new section cycles through (stored per section, so a saved one keeps its colour)
juce::Colour defaultSectionColour (int index);
// default name for a new section: "Intro", "Verse 2", "Chorus" ... (never repeats an existing name)
juce::String suggestSectionName (const std::vector<SectionMarker>& existing);

struct TempoSegment
{
    double beat = 0.0;        // timeline position in beats
    double bpm = 120.0;
    int tsNum = 4, tsDen = 4;
    bool ramp = false;        // when true, bpm changes linearly to the next segment's bpm
    bool operator== (const TempoSegment& o) const { return beat == o.beat && bpm == o.bpm && tsNum == o.tsNum && tsDen == o.tsDen && ramp == o.ramp; }
    bool operator!= (const TempoSegment& o) const { return ! operator== (o); }
};

namespace tempomap
{
    struct BarInfo { double beat = 0.0; int num = 4, den = 4; };

    void normalize (std::vector<TempoSegment>& list);
    double bpmAt (const std::vector<TempoSegment>& list, double beat);
    void meterAt (const std::vector<TempoSegment>& list, double beat, int& num, int& den);
    double beatsToSeconds (const std::vector<TempoSegment>& list, double beat);
    double secondsToBeats (const std::vector<TempoSegment>& list, double seconds);
    double barStartAtOrBefore (const std::vector<TempoSegment>& list, double beat);
    void barStartsBetween (const std::vector<TempoSegment>& list, double from, double to, std::vector<BarInfo>& out);
    struct BarPos { int bar = 0; double beatInBar = 0.0; int num = 4, den = 4; };
    BarPos barPosition (const std::vector<TempoSegment>& list, double beat);
    double beatOfBar (const std::vector<TempoSegment>& list, int barIndex);

    /** "m:ss" label for a position in seconds: what the ruler shows when the timebase of the
        timeline is seconds instead of bars. */
    inline juce::String timeLabel (double seconds)
    {
        const bool neg = seconds < 0.0;
        const double s = std::abs (seconds);
        const int m = (int) (s / 60.0);
        const int rest = (int) std::floor (s - m * 60.0);
        return (neg ? "-" : "") + juce::String (m) + ":" + juce::String (rest).paddedLeft ('0', 2);
    }
}

struct Track
{
    int id = 0;
    juce::String name;
    juce::Colour colour { 0xff2f7cf6 };
    TrackType type = TrackType::Instrument;
    float volumeDb = 0.0f;
    float pan = 0.0f;
    bool mute = false, solo = false, arm = false, monitor = false;
    // invert the channel's polarity (Ø): both channels of the signal are multiplied by -1
    bool phaseInvert = false;
    // master bus only: fold the main output to mono (Studio One style mono / phase check).
    // The fold is an average of L and R so the level does not jump when it is switched in.
    bool mono = false;
    bool hidden = false;    // hidden from the arranger (still plays, still shows in the mixer)
    double delayMs = 0.0;   // manual latency offset: positive = the track plays later

    //-- folder hierarchy -----------------------------------------------------------------------
    // A folder is a bus track flagged with isFolder: the tracks packed into it route through it
    // and are drawn indented below it. folderParent is the id of the folder a track belongs to
    // (0 = top level). folderDepth / folderHidden are derived by Project::refreshFolders() and
    // are never written to the project file.
    bool isFolder = false;
    bool folderOpen = true;
    int folderParent = 0;      // id of the folder track this track sits in (0 = none)
    int folderDepth = 0;       // 0 = top level, 1 = inside one folder, ...
    bool folderHidden = false; // an ancestor folder is closed: the row is not drawn in the arranger

    //-- stand-alone automation track -----------------------------------------------------------
    // A TrackType::Automation track drives one parameter of another channel. Its single AutoLane
    // lives in 'lanes' like any other lane, so the engine, the editor and the undo stack keep one
    // code path; autoTargetTrack is the track it belongs to (0 = the master bus), autoTarget the
    // lane target ("vol", "pan", "p:<uid>:<param>").
    int autoTargetTrack = 0;
    juce::String autoTarget, autoLabel;
    bool isAutomation() const { return type == TrackType::Automation; }
    /** The one lane an automation track carries (nullptr for every other track type). */
    AutoLane* automationLane() { return isAutomation() && ! lanes.empty() ? &lanes.front() : nullptr; }
    const AutoLane* automationLane() const { return isAutomation() && ! lanes.empty() ? &lanes.front() : nullptr; }
    /** The name shown on the row of a stand-alone automation track ("Volume", "Sylenth: Cutoff"). */
    juce::String automationTitle() const
    {
        if (autoLabel.isNotEmpty()) return autoLabel;
        if (autoTarget == "vol") return "Volume";
        if (autoTarget == "pan") return "Pan";
        return autoTarget;
    }
    bool isChildOfFolder() const { return folderParent != 0; }
    bool shownInArranger() const { return ! hidden && ! folderHidden; }

    // freeze: the instrument and its insert chain are pre-rendered to audio; while frozen the
    // track plays this file and its plugins stay idle. The insert slots that went into the render
    // are kept in frozenChain and moved back by unfreeze, so effects can still be inserted *after*
    // the freeze: they process the frozen audio live, and the automation lanes stay editable.
    bool frozen = false;
    bool frozenNoFx = false;   // frozen without the insert effects (baked raw)
    double frozenStartBeat = 0.0;
    juce::File frozenFile;
    std::shared_ptr<AudioData> frozenData;
    // plug-in latency that is already inside the render (instrument + baked inserts): the engine
    // adds it to its delay compensation, otherwise a frozen track with a latency plug-in would
    // play late against the rest of the project
    int frozenBakedLatency = 0;
    // inserts that were baked into the render (restored, in front of the chain, by unfreeze)
    std::vector<PluginSlot> frozenChain;

    int midiChannel = 1;
    int inputChannel = 0;   // first physical input channel
    bool inputMono = true;

    bool hasInstrument = false;
    PluginSlot instrument;                    // the first instrument of the track
    // A track can stack several instruments: every one of them receives the track's MIDI and their
    // outputs are summed (each with its own gain / pan / bypass) before the track's inserts.
    std::vector<PluginSlot> extraInstruments;

    int numInstruments() const { return hasInstrument ? 1 + (int) extraInstruments.size() : 0; }
    /** Every instrument of this track, the first one first (order = mixer / menu order). */
    std::vector<PluginSlot*> instruments();
    std::vector<const PluginSlot*> instruments() const;
    PluginSlot* findInstrument (juce::int64 slotUid);
    /** Removes the instrument with this uid; promotes a stacked one when the first is removed. */
    bool removeInstrument (juce::int64 slotUid);
    std::vector<PluginSlot> inserts;

    //-- multi-output instruments -----------------------------------------------------------------
    // A track with sourceTrack != 0 is an *output channel* of another track's instrument: it plays
    // a channel pair of that instrument (sourceChannel = 2, 4, 6 ...) through its own fader,
    // inserts, sends and routing, so a multi-out sampler can be mixed channel by channel.
    int sourceTrack = 0;                    // 0 = a normal track, else the instrument track's id
    int sourceChannel = 0;                  // first channel in that instrument's output
    bool isOutputChannel() const { return sourceTrack != 0; }
    // on the instrument track: do its output channels show up in the mixer? (they always play)
    bool showOutputChannels = true;

    // routing: one output bus plus any number of sends (both feed Bus / Send tracks or master)
    int outputBusId = 0;             // 0 = master, else the id of the Bus track this feeds
    std::vector<SendSlot> sends;
    bool isBus() const { return type == TrackType::Bus; }
    bool isListenBus = false;      // monitors the master mix: its own level + post-fader inserts
    bool listenEnabled = true;     // user switch: when off the master goes straight to the device
    bool listenSolo = false;       // track only: monitor this channel through the listen bus
    bool isSend() const { return type == TrackType::Send; }
    bool isGroupTrack() const { return type == TrackType::Bus || type == TrackType::Send; }

    //-- inline automation ----------------------------------------------------------------------
    // Studio One style: automation of a channel is drawn *in the track row* over its events, not
    // as a row of its own. The events are dimmed while the curve is shown. 'automationEdit' is a
    // transient editing mode (not saved): clicks inside the row edit the active lane instead of
    // selecting clips. The master bus keeps separate lane rows (automationInTrack = false).
    bool automationInTrack = true;
    bool automationEdit = false;
    /** Visible sub-rows below the track: 0 while the lane is drawn in the track body. */
    int laneRowCount() const { return automationInTrack || isAutomation() ? 0 : visibleLaneCount(); }
    /** The lane shown in the track body: the active visible lane (or the first visible one). */
    AutoLane* inlineAutomationLane()
    {
        if (! automationInTrack || isAutomation()) return nullptr;
        if (auto* l = findLane (activeLane); l != nullptr && l->visible) return l;
        for (auto& l : lanes) if (l.visible) return &l;
        return nullptr;
    }
    const AutoLane* inlineAutomationLane() const
    {
        if (! automationInTrack || isAutomation()) return nullptr;
        if (auto* l = findLane (activeLane); l != nullptr && l->visible) return l;
        for (auto& l : lanes) if (l.visible) return &l;
        return nullptr;
    }

    std::vector<MidiClip> midiClips;
    std::vector<AudioClip> audioClips;

    std::vector<AutoLane> lanes;
    AutoMode autoMode = AutoMode::Read;
    bool showAutomation = false;   // legacy flag: kept in sync with "any lane visible"
    juce::String activeLane = "vol";
    int height = 0;                // row height in px, 0 = follow the session default (AppState::trackHeight)
    int effectiveHeight (int fallback) const { return height > 0 ? height : fallback; }

    // multiple automation lanes can be visible at the same time, one row each
    int visibleLaneCount() const;
    AutoLane* visibleLaneAt (int index);
    const AutoLane* visibleLaneAt (int index) const;
    int visibleLaneIndex (const juce::String& target) const;   // -1 when not visible
    bool hasVisibleLanes() const { return visibleLaneCount() > 0; }

    MidiClip* findMidi (int clipId);
    AudioClip* findAudio (int clipId);
    AutoLane* findLane (const juce::String& target);
    const AutoLane* findLane (const juce::String& target) const;
    AutoLane& ensureLane (const juce::String& target, const juce::String& label);
    PluginSlot* findSlot (juce::int64 uid);
    double endBeat (const std::vector<TempoSegment>& tempo) const;
};

float faderToGain (float norm);      // 0..1 -> linear gain
float gainToFader (float gain);
float panFromNorm (float n);

class AudioPool
{
public:
    std::shared_ptr<AudioData> load (const juce::File& f, juce::AudioFormatManager& fm);
private:
    // imports decode on background threads now, so the cache is shared between them
    juce::CriticalSection cacheLock;
    std::map<juce::String, std::weak_ptr<AudioData>> cache;
};

// Project metadata the user fills in (File > Project Info...). It travels inside the .sxp: the
// fields as attributes, the cover picture as base64 PNG, so a project file stays self-contained.
struct ProjectMeta
{
    juce::String title, author, genre, album, year, comment;
    juce::MemoryBlock coverData;      // PNG bytes of the cover (empty = no picture)
    juce::Image cover;                // the decoded picture, kept in sync with coverData

    void setCover (const juce::Image& img);        // re-encodes the picture as PNG
    void clearCover();
    bool isEmpty() const;
};

// MIDI learn: a hardware controller (a CC, or a note-on button, on a MIDI channel) drives a
// parameter of the project. The mappings live in the .sxp, so a controller keeps working when the
// project is opened again on the same machine (or on one with the same controller plugged in).
enum class MidiTargetKind
{
    None = 0,
    MasterVolume,       // the master fader
    TrackVolume,        // trackId
    TrackPan,           // trackId
    PluginParam,        // uid + paramIndex (an instrument or an insert slot)
    Metronome,          // a switch: value / velocity above 63 toggles it
    TransportPlayStop   // a switch: above 63 toggles play / stop
};

struct MidiTarget
{
    MidiTargetKind kind = MidiTargetKind::None;
    int trackId = 0;
    juce::int64 uid = 0;      // PluginParam: the plug-in (instrument or insert) instance
    int paramIndex = -1;      // PluginParam: index in that plug-in's parameter list
};

struct MidiMapping
{
    MidiTarget target;
    int cc = -1;              // controller number, or the note number for a button (0..127)
    int channel = 0;          // 0 = any channel, otherwise 1..16
    bool isNote = false;      // learned from a note-on (controller buttons)
    juce::String label;       // what the UI shows ("Kick - Volume", "SX Synth · Cutoff")

    bool sameSource (int otherCc, int otherChannel) const { return cc == otherCc && isNote == false && channel == otherChannel; }
};

class Project
{
public:
    Project();

    double bpm = 120.0;
    int tsNum = 4, tsDen = 4;
    std::vector<TempoSegment> tempo;    // full tempo / meter map, segment 0 starts at beat 0

    double bpmAt (double beat) const { return tempomap::bpmAt (tempo, beat); }
    void meterAt (double beat, int& n, int& d) const { tempomap::meterAt (tempo, beat, n, d); }
    double beatsToSeconds (double beat) const { return tempomap::beatsToSeconds (tempo, beat); }
    double secondsToBeats (double sec) const { return tempomap::secondsToBeats (tempo, sec); }
    int64_t beatToSamples (double beat, double sr) const { return (int64_t) std::llround (beatsToSeconds (beat) * sr); }
    double samplesToBeats (int64_t samples, double sr) const { return secondsToBeats ((double) samples / sr); }
    // beats spanned by 'lengthSec' seconds of audio starting at 'startBeat' (exact through ramps)
    double clipBeats (double startBeat, double lengthSec) const { return secondsToBeats (beatsToSeconds (startBeat) + lengthSec) - startBeat; }
    void setTempo (double beat, double bpm, int num, int den);   // insert or modify a segment
    void setTempoRamp (double beat, bool ramp);                  // linear ramp from this segment to the next
    void removeTempo (double beat);
    TempoSegment* findTempoSegment (double beat);
    TempoSegment* findTempoSegmentAt (double beat);              // the segment active at 'beat'
    void syncLegacyTempo();                                     // keep bpm/tsNum/tsDen mirrors in sync

    //-- event timebase (seconds clips) ---------------------------------------------------------
    /** Keeps the clips whose timebase is Seconds at their absolute position across a tempo edit:
        bracket the edit with this. The three setTempo* functions do it themselves; the tempo lane
        and the tempo field of the transport bar mutate a segment directly, so they use it too. */
    struct TempoEdit
    {
        explicit TempoEdit (Project& p) : project (p) { project.captureSecondsClips(); }
        ~TempoEdit() { project.restoreSecondsClips(); }
        Project& project;
        JUCE_DECLARE_NON_COPYABLE (TempoEdit)
    };
    void captureSecondsClips();
    void restoreSecondsClips();
    bool loopOn = false;
    double loopStart = 0.0, loopEnd = 8.0;
    bool metronome = false;
    int countInBars = 0;
    // grid the notes of a MIDI take are quantized to while it is recorded (0 = keep them as played)
    double recordQuantize = 0.0;
    // recording over the same range keeps the earlier pass as a take instead of replacing it
    bool takeRecording = true;

    /** Groups the clip that was just added to a track with the takes it was recorded over: the
        earlier passes join a take stack (and go quiet) and the new one becomes the active take.
        Called by the engine when a recording stops, and directly by the tests. */
    bool groupTakes (Track& t, bool midi);
    std::vector<std::unique_ptr<Track>> tracks;

    //-- chord track ----------------------------------------------------------------------------
    // Sorted by beat and never overlapping: a chord that starts inside another one splits it.
    std::vector<ChordEvent> chords;
    ChordEvent& addChord (double beat, const juce::String& symbol, double length = 4.0);
    ChordEvent* findChord (int id);
    ChordEvent* chordAt (double beat);                  // the chord block covering 'beat' (0 = none)
    const ChordEvent* chordAt (double beat) const;
    void moveChord (int id, double beat, bool removeCollisions = true);   // clamped to >= 0
    void resizeChord (int id, double length);           // right edge, clamped by the next chord
    void removeChord (int id);
    void setChordSymbol (int id, const juce::String& symbol);
    void clearChords();
    void normalizeChords();                             // sorts, drops empty ones, splits overlaps

    //-- marker track (arrangement sections) ----------------------------------------------------
    std::vector<SectionMarker> sections;
    SectionMarker& addSection (double beat, double length, const juce::String& name);
    SectionMarker* findSection (int id);
    SectionMarker* sectionAt (double beat);             // the section containing 'beat'
    const SectionMarker* sectionAt (double beat) const;
    void moveSection (int id, double beat);             // keeps the length, clamps into the gap
    void resizeSection (int id, double length);
    void setSectionName (int id, const juce::String& name);
    void removeSection (int id);
    void clearSections();
    void normalizeSections();                           // sorts and stops sections overlapping
    int addSectionAfter (int id);                       // creates the next section right after it

    // The master bus is a first-class channel: it has the same row / header / automation model
    // as a track (fader, mute, inserts, automation lanes as sub-rows) but lives outside 'tracks'
    // with the reserved id 0, so selecting, reordering and deleting tracks never touch it.
    Track masterTrack;
    Track& master() { return masterTrack; }
    const Track& master() const { return masterTrack; }
    // the master fader curve is the "vol" lane of the master; created on demand
    AutoLane& masterFaderLane();
    const AutoLane* masterFaderLaneIfAny() const
    {
        for (auto& l : masterTrack.lanes) if (l.target == "vol") return &l;
        return nullptr;
    }

    Track* listenBus()                                // the bus flagged as the monitor path (nullptr = none)
    {
        for (auto& t : tracks) if (t != nullptr && t->isListenBus) return t.get();
        return nullptr;
    }
    const Track* listenBus() const
    {
        for (auto& t : tracks) if (t != nullptr && t->isListenBus) return t.get();
        return nullptr;
    }
    juce::File file;
    bool dirty = false;

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void projectContentChanged() {}   // clips / notes / automation / structure
        virtual void projectMixChanged() {}       // volume pan mute solo arm
        virtual void projectStructureChanged() {} // tracks / plugins added or removed
    };
    void addListener (Listener* l) { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    void sendContentChanged();
    void sendMixChanged();
    void sendStructureChanged();

    int newId() { return ++idCounter; }
    Track* findTrack (int id);
    const Track* findTrack (int id) const;
    int indexOfTrack (int id) const;
    Track& addTrack (TrackType t, const juce::String& name = {});
    void removeTrack (int id);

    //== folders ===============================================================================
    /** Recomputes the derived hierarchy fields (folderDepth / folderHidden) of every track.
        Cheap; called on every structural notification and after loading a project. */
    void refreshFolders();
    /** How many tracks are packed into this folder (direct children). */
    int folderChildCount (int folderId) const;
    /** Every direct child of a folder, in project order. */
    std::vector<int> folderChildren (int folderId) const;

    //== multi-output instruments ===============================================================
    /** Adds a mixer channel playing one channel pair of this instrument track's plug-in output
        (sourceChannel 2, 4, 6 ...). It is hidden from the arranger and mixed like any track. */
    Track& addOutputChannel (int instrumentTrackId, int sourceChannel, const juce::String& name);
    /** How many output channels this instrument track has. */
    int numOutputChannels (int instrumentTrackId) const;
    /** Removes all output channels of this instrument track. Returns how many were removed. */
    int removeOutputChannels (int instrumentTrackId);

    // gathers every audio clip file into <projectDir>/Audio Files and rewrites the clip paths:
    // session recordings (temp folder) are moved, imported files are copied.
    // returns the number of files relocated
    static juce::File audioFilesFolderFor (const juce::File& projectFile);
    int collectAudioFiles (const juce::File& projectFile);

    // writes an autosave copy without touching the project file path or the dirty flag
    bool writeSnapshotTo (const juce::File& target);
    double beatsPerBar() const { return tsNum * 4.0 / tsDen; }
    double endBeat() const;
    bool anySolo() const;

    // undo
    /** One undo step: the project tree, plus the plug-in data the tree does not carry.

        Plug-in parameters and opaque plug-in state live in the engine's instances, not in the
        model, so an edit that touches a plug-in (a knob move, a bypass switch, inserting or
        removing an effect) is only undoable if those values are saved next to the XML.
    */
    struct EditSnapshot
    {
        std::unique_ptr<juce::XmlElement> project;
        std::map<juce::int64, std::vector<float>> pluginParams;   // normalised parameter values per slot uid
        std::map<juce::int64, juce::MemoryBlock> pluginStates;    // opaque state of the slots this step deletes
        std::set<juce::int64> slotUids;                           // the plug-in slots that existed at capture time
    };

    void checkpoint();
    /** Checkpoint for an edit that removes these plug-in slots: their state is saved as well, so
        undoing brings the plug-in back with its sound instead of its defaults. */
    void checkpointRemoving (const std::set<juce::int64>& slotUidsToSave);

    /** Groups everything checkpointed() until endUndoBatch() into a single undo step, so an
        operation that changes many things at once (dropping a folder of stems on the arrange
        view adds a track per file) is taken back by one Ctrl+Z instead of one per item - the way
        Studio One groups an import.

        Batches nest; only the outermost endUndoBatch() closes the step. The first checkpoint()
        inside the batch keeps the state from before the first change: later ones are dropped.

        A batch belongs around one synchronous operation: anything that runs a message loop while
        it is open (a modal dialog, an async file chooser) would end up in the same undo step too.
        checkpointRemoving() is not grouped (the removed plug-in's state would be lost otherwise).
    */
    void beginUndoBatch();
    void endUndoBatch();

    bool canUndo() const { return ! undoStack.empty(); }
    /** Number of undo steps currently held (self-test / diagnostics). */
    int undoDepth() const { return (int) undoStack.size(); }
    bool canRedo() const { return ! redoStack.empty(); }
    void undo();
    void redo();
    void clearUndo() { undoStack.clear(); redoStack.clear(); undoBatchDepth = 0; undoBatchCheckpointed = false; }

    /** The plug-in slot with this uid, anywhere in the project (tracks, master). */
    PluginSlot* findSlotByUid (juce::int64 uid);
    std::set<juce::int64> collectSlotUids() const;

    // persistence
    std::unique_ptr<juce::XmlElement> toXml (bool withPluginState) const;
    void fromXml (const juce::XmlElement& xml);
    bool saveTo (const juce::File& f);
    bool loadFrom (const juce::File& f);
    void reset();

    /** Author / genre / album / cover and the rest of what the user types into File > Project Info */
    ProjectMeta meta;

    /** MIDI learn mappings (View > MIDI Learn...): controller moves the faders and knobs */
    std::vector<MidiMapping> midiMappings;

    //== project templates ======================================================================
    /** Saves this project as a template: everything including the plug-in states, so an instrument
        loaded in the template comes back with its sound. The project keeps its own file / title. */
    bool saveAsTemplate (const juce::File& templateFile);
    /** Loads a project template as an *untitled* project: the undo history is dropped and nothing
        points at the template file, so "Save" asks for a name like a fresh project does. */
    bool loadFromTemplate (const juce::File& templateFile);

    // MIDI
    // 'syncTempo' writes the file's own tempo / time signature map into the project before the
    // notes are placed (the import dialog asks, drag & drop uses the remembered setting).
    bool importMidi (const juce::File& f, double atBeat, int& tracksAdded, bool syncTempo = true);
    bool exportMidi (const juce::File& f, const std::vector<int>* onlyTracks = nullptr) const;

    // Studio One style crossfade: make 'a' (the earlier clip) and 'b' overlap by up to
    // fadeSec seconds, extending each into unused source material where available.
    // Overlapping clips crossfade automatically in the engine (equal power).
    // Returns the overlap now in place (seconds); 0 when nothing was done
    // (too far apart, no source data, or 'b' does not start after 'a').
    double crossfade (AudioClip& a, AudioClip& b, double fadeSec);

    // Sizes an existing overlap between two clips: 'b' (the later clip) moves so that
    // its audio keeps its place in the timeline and its end stays put. The overlap can
    // only grow as far as B's headroom in the source file allows. Returns the overlap
    // now in place in seconds (0 when the clips do not overlap).
    double resizeCrossfade (AudioClip& a, AudioClip& b, double sec);

    AudioPool pool;
    juce::AudioFormatManager* formats = nullptr;
    std::function<void()> capturePluginStates; // set by engine before saving

    //== plug-in values for the undo stack (installed by the engine, which owns the instances) ===
    /** Fills 'out' with the normalised parameter values of every loaded plug-in. */
    std::function<void (std::map<juce::int64, std::vector<float>>& out)> capturePluginParams;
    /** Fills 'out' with the opaque state of the given slots' plug-ins. */
    std::function<void (const std::set<juce::int64>& uids, std::map<juce::int64, juce::MemoryBlock>& out)> capturePluginStatesFor;
    /** Applies normalised parameter values to the live plug-ins (after the tree was restored). */
    std::function<void (const std::map<juce::int64, std::vector<float>>& values)> applyPluginParams;

private:
    int idCounter = 0;
    juce::ListenerList<Listener> listeners;
    std::vector<EditSnapshot> undoStack, redoStack;
    struct SecondsClipPos { int trackId = 0, clipId = 0; double seconds = 0.0; };
    std::vector<SecondsClipPos> secondsClips;   // captured by TempoEdit, restored by restoreSecondsClips()
    int undoBatchDepth = 0;            // > 0 while an import (or another batch) is being recorded
    bool undoBatchCheckpointed = false; // the batch already pushed its one snapshot
    void pushUndo (EditSnapshot&& snapshot);
    EditSnapshot counterSnapshotFor (const EditSnapshot& step);
    void restoreFrom (const EditSnapshot& step);
    void clearAll();
};

// WAV tempo metadata ("acid" chunk): loop tempo + length in beats, written on export when asked
bool writeWavTempo (const juce::File& file, double bpm, double beats);
bool readWavTempo (const juce::File& file, double& bpm, double& beats);

//-- MIDI tempo map ----------------------------------------------------------------------------
// The tempo / time-signature map a MIDI file describes (every conductor event, merged per beat).
// Empty when the file carries neither, or when it uses SMPTE time code. 'fallback*' is the running
// state before the file's first event, so a file that only changes the meter keeps the tempo it is
// imported into.
std::vector<TempoSegment> readMidiTempoMap (const juce::File& f, double fallbackBpm = 120.0,
                                            int fallbackNum = 4, int fallbackDen = 4);

//-- chord detection helpers -------------------------------------------------------------------
// The notes of a MIDI clip placed on the timeline (clip-relative beats -> absolute beats)
void appendClipNotes (const MidiClip& clip, std::vector<chord::TimedNote>& out);
// every MIDI note of the project, optionally limited to one track's id (0 = all tracks)
std::vector<chord::TimedNote> collectTimedNotes (const Project& p, int onlyTrackId = 0);
// the chord that sounds at 'beat': the notes inside [beat, beat + window] decide it
chord::Detection detectChordAt (const Project& p, double beat, double windowBeats = 1.0, int onlyTrackId = 0);

juce::String barsBeatsString (double beats, const Project& p);
juce::int64 makeUid();
PluginSlot makeBuiltInSynthSlot();
bool isBuiltInSynth (const juce::PluginDescription& d);
PluginSlot makeBuiltInSamplerSlot();
bool isBuiltInSampler (const juce::PluginDescription& d);
PluginSlot makeBuiltInDelaySlot();
bool isBuiltInDelay (const juce::PluginDescription& d);
}
