#include "Model.h"
#include "ui/Theme.h"

namespace sx
{
using namespace juce;


//==============================================================================
String controllerName (int cc)
{
    if (cc == kCCPitchBend) return TRANS ("Pitch Bend");
    if (cc == kCCAftertouch) return TRANS ("Channel Pressure");
    switch (cc)
    {
        case 0:   return TRANS ("Bank Select MSB");
        case 1:   return TRANS ("Modulation");
        case 2:   return TRANS ("Breath");
        case 4:   return TRANS ("Foot Controller");
        case 5:   return TRANS ("Portamento Time");
        case 7:   return TRANS ("Volume");
        case 8:   return TRANS ("Balance");
        case 10:  return TRANS ("Pan");
        case 11:  return TRANS ("Expression");
        case 32:  return TRANS ("Bank Select LSB");
        case 64:  return TRANS ("Sustain");
        case 65:  return TRANS ("Portamento");
        case 66:  return TRANS ("Sostenuto");
        case 67:  return TRANS ("Soft Pedal");
        case 71:  return TRANS ("Resonance");
        case 72:  return TRANS ("Release");
        case 73:  return TRANS ("Attack");
        case 74:  return TRANS ("Cutoff");
        case 91:  return TRANS ("Reverb Send");
        case 93:  return TRANS ("Chorus Send");
        case 120: return TRANS ("All Sound Off");
        case 121: return TRANS ("Reset Controllers");
        case 123: return TRANS ("All Notes Off");
        default:  return TRANS ("Controller");
    }
}

int controllerMax (int cc) { return cc == kCCPitchBend ? 16383 : 127; }
int controllerDefault (int cc)
{
    if (cc == kCCPitchBend) return 8192;
    if (cc == 7) return 100;
    if (cc == 10) return 64;
    if (cc == 11) return 127;
    return 0;
}

void quantizeRecordedNotes (std::vector<Note>& notes, double grid)
{
    if (grid <= 0.0) return;
    for (auto& n : notes)
    {
        n.start = jmax (0.0, std::round (n.start / grid) * grid);
        n.length = jmax (n.length, 1.0 / 128.0);   // keep the phrasing, just never collapse a note
    }
    std::stable_sort (notes.begin(), notes.end(),
                      [] (const Note& a, const Note& b) { return a.start < b.start; });
}

std::vector<LoopTakeSegment> splitLoopRecording (juce::int64 writtenSamples, juce::int64 samplesTillLoopEnd,
                                                 juce::int64 passSamples, double recordStartBeat,
                                                 double loopStartBeat)
{
    std::vector<LoopTakeSegment> out;
    if (writtenSamples <= 0 || passSamples <= 0) return out;

    juce::int64 pos = 0;
    int pass = 0;
    // the first pass is cut short at the loop end (unless the punch-in was exactly on the loop start)
    if (samplesTillLoopEnd > 0 && samplesTillLoopEnd < passSamples && writtenSamples > samplesTillLoopEnd)
    {
        out.push_back ({ 0, samplesTillLoopEnd, recordStartBeat });
        pos = samplesTillLoopEnd;
        ++pass;
    }
    while (pos + passSamples / 2 <= writtenSamples)
    {
        const juce::int64 len = jmin (passSamples, writtenSamples - pos);
        out.push_back ({ pos, len, loopStartBeat });
        pos += len;
        ++pass;
        if (pass > 512) break;                     // a runaway cycle setting cannot flood the track
    }
    return out;
}

bool Project::groupTakes (Track& t, bool midi)
{
    if (! takeRecording) return false;

    // ends of the clips, in timeline beats, for both kinds
    auto endOf = [this] (const AudioClip& c) { return c.start + clipBeats (c.start, c.lengthSec); };

    std::vector<std::pair<int, int>> members;   // takeIndex, clip id
    double freshStart = 0.0, freshEnd = 0.0;
    juce::int64 group = 0;
    int maxIndex = -1;
    int activeId = 0;

    auto consider = [&] (auto& clips, auto endBeatOf)
    {
        if (clips.empty()) return;
        auto& fresh = clips.back();
        freshStart = fresh.start;
        freshEnd = endBeatOf (fresh);
        const double freshLen = jmax (1.0e-9, freshEnd - freshStart);

        for (size_t i = 0; i + 1 < clips.size(); ++i)
        {
            auto& c = clips[i];
            const double cEnd = endBeatOf (c);
            const double ov = jmin (freshEnd, cEnd) - jmax (freshStart, c.start);
            const double shortest = jmin (freshLen, jmax (1.0e-9, cEnd - c.start));
            if (ov <= 0.4 * shortest) continue;      // not recorded over

            if (c.takeGroup == 0) c.takeGroup = (juce::int64) newId();
            if (group == 0) group = c.takeGroup;
            c.takeGroup = group;                     // merge stacks that both overlap this pass
            maxIndex = jmax (maxIndex, c.takeIndex);
            c.takeActive = false;                    // only the newest pass plays
            members.push_back ({ c.takeIndex, c.id });
        }
        if (group == 0) return;
        fresh.takeGroup = group;
        fresh.takeIndex = maxIndex + 1;
        fresh.takeActive = true;
        activeId = fresh.id;
    };

    if (midi) consider (t.midiClips, [] (const MidiClip& c) { return c.start + c.length; });
    else      consider (t.audioClips, endOf);

    juce::ignoreUnused (members, activeId, freshStart, freshEnd);
    return group != 0;
}

void appendControllerMessages (const MidiClip& clip, int channel, std::vector<std::pair<double, MidiMessage>>& out)
{
    const int ch = jlimit (1, 16, channel);

    // where a bend starts and ends inside the clip: the parts of the clip outside that range have
    // to sit in the middle, so the events there are written explicitly
    const CCPoint* firstBend = nullptr;
    const CCPoint* lastBend = nullptr;
    for (auto& p : clip.ccs)
    {
        if (p.cc != kCCPitchBend || p.beat < 0.0 || p.beat > clip.length) continue;
        if (firstBend == nullptr || p.beat < firstBend->beat) firstBend = &p;
        if (lastBend == nullptr || p.beat > lastBend->beat) lastBend = &p;
    }

    const int restValue = controllerDefault (kCCPitchBend);
    if (firstBend != nullptr)
    {
        if (firstBend->beat > 0.0)
            out.push_back ({ 0.0, MidiMessage::pitchWheel (ch, restValue) });
        if (lastBend->value != restValue && lastBend->beat + kControllerRestGap < clip.length)
            out.push_back ({ lastBend->beat + kControllerRestGap, MidiMessage::pitchWheel (ch, restValue) });
    }

    for (auto& p : clip.ccs)
    {
        if (p.beat < 0.0 || p.beat > clip.length) continue;
        MidiMessage m;
        if (p.cc == kCCPitchBend)       m = MidiMessage::pitchWheel (ch, jlimit (0, 16383, p.value));
        else if (p.cc == kCCAftertouch) m = MidiMessage::channelPressureChange (ch, jlimit (0, 127, p.value));
        else                            m = MidiMessage::controllerEvent (ch, p.cc & 127, jlimit (0, 127, p.value));
        out.push_back ({ p.beat, m });
    }
}

int64 makeUid()
{
    int64 v = Random::getSystemRandom().nextInt64() & 0x7fffffffffffffffLL;
    return v == 0 ? 1 : v;
}

PluginSlot makeBuiltInSynthSlot()
{
    PluginSlot s;
    s.uid = makeUid();
    s.builtIn = true;
    s.desc.name = "SX Synth";
    s.desc.descriptiveName = "SoundX built-in synthesizer";
    s.desc.pluginFormatName = "Built-in";
    s.desc.fileOrIdentifier = "sx.builtin.synth";
    s.desc.manufacturerName = "SoundX";
    s.desc.isInstrument = true;
    s.desc.numInputChannels = 0;
    s.desc.numOutputChannels = 2;
    s.desc.uniqueId = 0x53585331;
    return s;
}

bool isBuiltInSynth (const PluginDescription& d) { return d.fileOrIdentifier == "sx.builtin.synth"; }

PluginSlot makeBuiltInSamplerSlot()
{
    PluginSlot s;
    s.uid = makeUid();
    s.builtIn = true;
    s.desc.name = "SX Sampler";
    s.desc.descriptiveName = "SoundX built-in sampler";
    s.desc.pluginFormatName = "Built-in";
    s.desc.fileOrIdentifier = "sx.builtin.sampler";
    s.desc.manufacturerName = "SoundX";
    s.desc.isInstrument = true;
    s.desc.numInputChannels = 0;
    s.desc.numOutputChannels = 2;
    s.desc.uniqueId = 0x53585332;
    return s;
}

bool isBuiltInSampler (const PluginDescription& d) { return d.fileOrIdentifier == "sx.builtin.sampler"; }

PluginSlot makeBuiltInDelaySlot()
{
    PluginSlot s;
    s.uid = makeUid();
    s.builtIn = true;
    s.desc.name = "SX Test Delay";
    s.desc.descriptiveName = "SoundX built-in test delay (960 samples)";
    s.desc.pluginFormatName = "Built-in";
    s.desc.fileOrIdentifier = "sx.builtin.delay";
    s.desc.manufacturerName = "SoundX";
    s.desc.isInstrument = false;
    s.desc.numInputChannels = 2;
    s.desc.numOutputChannels = 2;
    s.desc.uniqueId = 0x53584431;
    return s;
}

bool isBuiltInDelay (const PluginDescription& d) { return d.fileOrIdentifier == "sx.builtin.delay"; }

//==============================================================================
void AudioData::buildPeaks()
{
    const int n = buffer.getNumSamples();
    const int blocks = (n + kPeakBlock - 1) / kPeakBlock;
    peakMin.assign ((size_t) blocks, 0.0f);
    peakMax.assign ((size_t) blocks, 0.0f);
    const int nch = buffer.getNumChannels();
    // stereo files also get a per-channel envelope: the arrangement draws L and R as two lanes
    // instead of one mixed waveform
    channelPeakMin.clear();
    channelPeakMax.clear();
    if (nch > 1)
    {
        channelPeakMin.resize ((size_t) nch);
        channelPeakMax.resize ((size_t) nch);
        for (int ch = 0; ch < nch; ++ch)
        {
            channelPeakMin[(size_t) ch].assign ((size_t) blocks, 0.0f);
            channelPeakMax[(size_t) ch].assign ((size_t) blocks, 0.0f);
        }
    }
    for (int b = 0; b < blocks; ++b)
    {
        const int s0 = b * kPeakBlock, s1 = jmin (n, s0 + kPeakBlock);
        float lo = 0.0f, hi = 0.0f;
        for (int ch = 0; ch < nch; ++ch)
        {
            // the waveform always grows from the zero line, and the SIMD min/max is several
            // times faster than the old per-sample loop (this runs on the import thread)
            const auto rng = FloatVectorOperations::findMinAndMax (buffer.getReadPointer (ch, s0), s1 - s0);
            lo = jmin (lo, rng.getStart());
            hi = jmax (hi, rng.getEnd());
            if (nch > 1)
            {
                channelPeakMin[(size_t) ch][(size_t) b] = jmin (0.0f, rng.getStart());
                channelPeakMax[(size_t) ch][(size_t) b] = jmax (0.0f, rng.getEnd());
            }
        }
        peakMin[(size_t) b] = lo;
        peakMax[(size_t) b] = hi;
    }
}

std::shared_ptr<AudioData> AudioPool::load (const File& f, AudioFormatManager& fm)
{
    auto key = f.getFullPathName();
    {
        const ScopedLock sl (cacheLock);
        if (auto it = cache.find (key); it != cache.end())
            if (auto sp = it->second.lock())
                return sp;
    }

    std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0)
        return nullptr;

    auto d = std::make_shared<AudioData>();
    const int nch = (int) jmin<unsigned int> (2u, r->numChannels);
    d->buffer.setSize (nch, (int) r->lengthInSamples);
    r->read (&d->buffer, 0, (int) r->lengthInSamples, 0, true, nch > 1);
    d->sampleRate = r->sampleRate;
    d->file = f;
    d->buildPeaks();
    {
        const ScopedLock sl (cacheLock);
        cache[key] = d;
    }
    return d;
}

//==============================================================================
MidiClip* Track::findMidi (int clipId)
{
    for (auto& c : midiClips) if (c.id == clipId) return &c;
    return nullptr;
}

AudioClip* Track::findAudio (int clipId)
{
    for (auto& c : audioClips) if (c.id == clipId) return &c;
    return nullptr;
}

AutoLane* Track::findLane (const String& target)
{
    for (auto& l : lanes) if (l.target == target) return &l;
    return nullptr;
}

const AutoLane* Track::findLane (const String& target) const
{
    for (auto& l : lanes) if (l.target == target) return &l;
    return nullptr;
}

AutoLane& Track::ensureLane (const String& target, const String& label)
{
    if (auto* l = findLane (target)) { if (label.isNotEmpty()) l->label = label; return *l; }
    AutoLane l;
    l.target = target;
    l.label = label;
    lanes.push_back (std::move (l));
    return lanes.back();
}

int Track::visibleLaneCount() const
{
    // a stand-alone automation track draws its one curve in the track body itself, not as a
    // sub-row below it
    if (isAutomation()) return 0;
    int n = 0;
    for (auto& l : lanes) if (l.visible) ++n;
    return n;
}

AutoLane* Track::visibleLaneAt (int index)
{
    if (isAutomation()) return nullptr;   // its curve is the track body (see automationLane())
    int n = 0;
    for (auto& l : lanes) if (l.visible && n++ == index) return &l;
    return nullptr;
}

const AutoLane* Track::visibleLaneAt (int index) const
{
    if (isAutomation()) return nullptr;
    int n = 0;
    for (auto& l : lanes) if (l.visible && n++ == index) return &l;
    return nullptr;
}

int Track::visibleLaneIndex (const String& target) const
{
    if (isAutomation()) return -1;
    int n = 0;
    for (auto& l : lanes) if (l.visible) { if (l.target == target) return n; ++n; }
    return -1;
}

PluginSlot* Track::findSlot (int64 uid)
{
    if (hasInstrument && instrument.uid == uid) return &instrument;
    for (auto& s : extraInstruments) if (s.uid == uid) return &s;
    for (auto& s : inserts) if (s.uid == uid) return &s;
    return nullptr;
}

//==============================================================================
std::vector<PluginSlot*> Track::instruments()
{
    std::vector<PluginSlot*> v;
    if (! hasInstrument) return v;
    v.push_back (&instrument);
    for (auto& s : extraInstruments) v.push_back (&s);
    return v;
}

std::vector<const PluginSlot*> Track::instruments() const
{
    std::vector<const PluginSlot*> v;
    if (! hasInstrument) return v;
    v.push_back (&instrument);
    for (auto& s : extraInstruments) v.push_back (&s);
    return v;
}

PluginSlot* Track::findInstrument (int64 slotUid)
{
    if (! hasInstrument) return nullptr;
    if (instrument.uid == slotUid) return &instrument;
    for (auto& s : extraInstruments) if (s.uid == slotUid) return &s;
    return nullptr;
}

bool Track::removeInstrument (int64 slotUid)
{
    if (! hasInstrument) return false;

    if (instrument.uid == slotUid)
    {
        // promote the first stacked instrument: the track keeps sounding, and the "primary" slot
        // (the one the channel strip shows) never dangles
        if (extraInstruments.empty())
        {
            hasInstrument = false;
            instrument = PluginSlot();
        }
        else
        {
            instrument = extraInstruments.front();
            extraInstruments.erase (extraInstruments.begin());
        }
        return true;
    }

    for (size_t i = 0; i < extraInstruments.size(); ++i)
        if (extraInstruments[i].uid == slotUid)
        {
            extraInstruments.erase (extraInstruments.begin() + (long long) i);
            return true;
        }
    return false;
}

double Track::endBeat (const std::vector<TempoSegment>& tempo) const
{
    double e = 0.0;
    for (auto& c : midiClips) e = jmax (e, c.start + c.length);
    for (auto& c : audioClips)   // audio plays in seconds: exact end through the tempo map (ramps too)
        e = jmax (e, tempomap::secondsToBeats (tempo, tempomap::beatsToSeconds (tempo, c.start) + c.lengthSec));
    // a positive track delay pushes the content past the last clip end
    if (delayMs > 0.0 && e > 0.0)
    {
        const double spb = jmax (1.0e-4, tempomap::beatsToSeconds (tempo, 1.0) - tempomap::beatsToSeconds (tempo, 0.0));
        e += delayMs / 1000.0 / spb;
    }
    return e;
}

float faderToGain (float n)
{
    if (n <= 0.0001f) return 0.0f;
    if (n <= 0.75f) { float x = n / 0.75f; return x * x * x; }
    return Decibels::decibelsToGain ((n - 0.75f) / 0.25f * 6.0f);
}

float gainToFader (float g)
{
    if (g <= 0.0f) return 0.0f;
    if (g <= 1.0f) return 0.75f * std::cbrt (g);
    return jmin (1.0f, 0.75f + Decibels::gainToDecibels (g) / 6.0f * 0.25f);
}

float panFromNorm (float n) { return jlimit (-1.0f, 1.0f, n * 2.0f - 1.0f); }

//==============================================================================
namespace tempomap
{
    void normalize (std::vector<TempoSegment>& list)
    {
        std::stable_sort (list.begin(), list.end(), [] (const TempoSegment& a, const TempoSegment& b) { return a.beat < b.beat; });
        // collapse duplicates at the same beat: the last one wins
        std::vector<TempoSegment> out;
        for (auto& s : list)
            if (out.empty() || std::abs (out.back().beat - s.beat) > 1.0e-9)
                out.push_back (s);
            else
                out.back() = s;
        if (out.empty() || out.front().beat > 1.0e-9)
        {
            TempoSegment first;
            if (! out.empty()) { first.bpm = out.front().bpm; first.tsNum = out.front().tsNum; first.tsDen = out.front().tsDen; }
            out.insert (out.begin(), first);
        }
        out.front().beat = 0.0;
        for (auto& s : out)
        {
            s.beat = jmax (0.0, s.beat);
            s.bpm = jlimit (20.0, 400.0, s.bpm);
            s.tsNum = jlimit (1, 32, s.tsNum);
            s.tsDen = jlimit (1, 32, s.tsDen);
        }
        list = std::move (out);
    }

    static const TempoSegment* segmentAt (const std::vector<TempoSegment>& list, double beat)
    {
        if (list.empty()) return nullptr;
        auto it = std::upper_bound (list.begin(), list.end(), beat, [] (double b, const TempoSegment& s) { return b < s.beat; });
        if (it == list.begin()) return &list.front();
        return &*(it - 1);
    }

    double bpmAt (const std::vector<TempoSegment>& list, double beat)
    {
        if (list.empty()) return 120.0;
        const auto* s = segmentAt (list, beat);
        if (s == nullptr) return 120.0;
        // linear ramp to the next segment
        if (s->ramp)
        {
            const size_t i = (size_t) (s - list.data());
            if (i + 1 < list.size())
            {
                const double span = list[i + 1].beat - s->beat;
                if (span > 1.0e-9)
                {
                    const double f = jlimit (0.0, 1.0, (beat - s->beat) / span);
                    return jlimit (20.0, 400.0, s->bpm + (list[i + 1].bpm - s->bpm) * f);
                }
            }
        }
        return s->bpm;
    }

    // seconds covered by a linear bpm ramp from v0 to v1 across 'beats' beats:
    // integral of 60 / (v0 + k x) dx  =  (60 / k) ln (v1 / v0), with the k -> 0 limit
    static double rampSeconds (double v0, double v1, double beats)
    {
        v0 = jlimit (1.0, 1000.0, v0);
        v1 = jlimit (1.0, 1000.0, v1);
        const double k = beats > 1.0e-12 ? (v1 - v0) / beats : 0.0;
        if (std::abs (k) < 1.0e-9 || beats <= 1.0e-12) return beats * 60.0 / v0;
        return (60.0 / k) * std::log (v1 / v0);
    }

    void meterAt (const std::vector<TempoSegment>& list, double beat, int& num, int& den)
    {
        num = 4; den = 4;
        if (auto* s = segmentAt (list, beat)) { num = s->tsNum; den = s->tsDen; }
    }

    static double barWidth (const TempoSegment& s) { return s.tsNum > 0 ? s.tsNum * 4.0 / s.tsDen : 4.0; }

    double beatsToSeconds (const std::vector<TempoSegment>& list, double beat)
    {
        if (beat < 0.0)
            return beat * 60.0 / jlimit (1.0, 1000.0, list.empty() ? 120.0 : list.front().bpm);
        double sec = 0.0, beatPos = 0.0;
        for (size_t i = 0; i < list.size() && beat > beatPos + 1.0e-12; ++i)
        {
            const double end = i + 1 < list.size() ? jmin (beat, list[i + 1].beat) : beat;
            if (end <= beatPos) break;
            const bool ramping = list[i].ramp && i + 1 < list.size() && list[i + 1].beat > beatPos + 1.0e-9;
            if (ramping)
            {
                const double v0 = jlimit (1.0, 1000.0, list[i].bpm);
                const double v1 = jlimit (1.0, 1000.0, list[i + 1].bpm);
                const double span = list[i + 1].beat - beatPos;
                const double k = (v1 - v0) / span;
                const double frac = jlimit (0.0, 1.0, (end - beatPos) / span);
                if (std::abs (k) > 1.0e-9)
                    sec += (60.0 / k) * std::log ((v0 + k * span * frac) / v0);
                else
                    sec += (end - beatPos) * 60.0 / v0;
            }
            else
                sec += (end - beatPos) * 60.0 / jlimit (1.0, 1000.0, list[i].bpm);
            beatPos = end;
        }
        return sec;
    }

    double secondsToBeats (const std::vector<TempoSegment>& list, double seconds)
    {
        if (seconds < 0.0)
            return seconds * jlimit (1.0, 1000.0, list.empty() ? 120.0 : list.front().bpm) / 60.0;
        if (seconds <= 1.0e-12) return 0.0;
        double sec = 0.0, beatPos = 0.0;
        for (size_t i = 0; i < list.size(); ++i)
        {
            const double nextBeat = i + 1 < list.size() ? list[i + 1].beat : 1.0e18;
            const bool ramping = list[i].ramp && i + 1 < list.size() && nextBeat > beatPos + 1.0e-9;
            double segSec;
            if (ramping)
                segSec = rampSeconds (list[i].bpm, list[i + 1].bpm, nextBeat - beatPos);
            else
                segSec = (nextBeat - beatPos) * 60.0 / jlimit (1.0, 1000.0, list[i].bpm);
            if (seconds <= sec + segSec + 1.0e-12)
            {
                if (! ramping)
                    return beatPos + (seconds - sec) * jlimit (1.0, 1000.0, list[i].bpm) / 60.0;
                // invert the log integral: bpm grows exponentially with time
                const double v0 = jlimit (1.0, 1000.0, list[i].bpm);
                const double v1 = jlimit (1.0, 1000.0, list[i + 1].bpm);
                const double span = nextBeat - beatPos;
                const double k = (v1 - v0) / span;
                if (std::abs (k) < 1.0e-9)
                    return beatPos + (seconds - sec) * v0 / 60.0;
                const double into = jlimit (0.0, segSec, seconds - sec);
                const double v = v0 * std::exp (k * into / 60.0);
                return beatPos + jlimit (0.0, span, (v - v0) / k);
            }
            sec += segSec;
            beatPos = nextBeat;
        }
        return seconds * 2.0; // unreachable in practice
    }

    // bar-grid helper: a new bar "era" opens only where the METER changes; tempo-only
    // segments keep the running grid. Bars are anchored at the era's first segment.
    static const TempoSegment* eraOf (const std::vector<TempoSegment>& list, double beat)
    {
        const TempoSegment* era = &list.front();
        for (size_t i = 1; i < list.size() && list[i].beat <= beat + 1.0e-9; ++i)
            if (list[i].tsNum != era->tsNum || list[i].tsDen != era->tsDen) era = &list[i];
        return era;
    }

    static double eraEnd (const std::vector<TempoSegment>& list, size_t index)
    {
        for (size_t j = index + 1; j < list.size(); ++j)
            if (list[j].tsNum != list[index].tsNum || list[j].tsDen != list[index].tsDen) return list[j].beat;
        return 1.0e18;
    }

    double barStartAtOrBefore (const std::vector<TempoSegment>& list, double beat)
    {
        if (beat <= 0.0 || list.empty()) return 0.0;
        const auto* s = eraOf (list, beat);
        const double w = barWidth (*s);
        // +1.0e-9 keeps exact bar lines landing on the bar they start
        return s->beat + std::floor ((beat - s->beat) / w + 1.0e-9) * w;
    }

    void barStartsBetween (const std::vector<TempoSegment>& list, double from, double to, std::vector<BarInfo>& out)
    {
        out.clear();
        if (list.empty() || to <= jmax (0.0, from)) return;
        const double f = jmax (0.0, from);
        int guard = 0;
        for (size_t i = 0; i < list.size(); ++i)
        {
            // only segments that change the meter open a new era
            if (i > 0 && list[i].tsNum == list[i - 1].tsNum && list[i].tsDen == list[i - 1].tsDen) continue;
            const auto& s = list[i];
            const double w = barWidth (s);
            const double end = eraEnd (list, i);
            for (int k = 0; s.beat + k * w < end - 1.0e-9 && guard++ < 400000; ++k)
            {
                const double b = s.beat + k * w;
                if (b >= to - 1.0e-9) return;
                if (b >= f - 1.0e-9) out.push_back ({ b, s.tsNum, s.tsDen });
            }
        }
    }

    BarPos barPosition (const std::vector<TempoSegment>& list, double beat)
    {
        BarPos r;
        if (list.empty()) return r;
        if (beat <= 0.0)
        {
            if (beat < 0.0)
            {
                // before the start: extrapolate backwards with the first segment
                const double w = barWidth (list.front());
                const double neg = -beat;
                r.bar = -1 - (int) std::ceil (neg / w - 1.0e-9);
                double b = list.front().beat - neg;
                b -= std::floor (b / w + 1.0e-9) * w;
                r.beatInBar = b;
                r.num = list.front().tsNum; r.den = list.front().tsDen;
            }
            else { r.num = list.front().tsNum; r.den = list.front().tsDen; }
            return r;
        }

        // count bars era by era, then settle inside the era that contains the beat
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (i > 0 && list[i].tsNum == list[i - 1].tsNum && list[i].tsDen == list[i - 1].tsDen) continue;
            const auto& s = list[i];
            if (beat < s.beat - 1.0e-9) break;
            const double w = barWidth (s);
            const double end = eraEnd (list, i);
            const int k = (int) std::floor ((beat - s.beat) / w + 1.0e-9);
            if (s.beat + k * w < end - 1.0e-9)
            {
                r.bar += k;
                r.beatInBar = beat - (s.beat + k * w);
                r.num = s.tsNum; r.den = s.tsDen;
                return r;
            }
            r.bar += jmax (1, (int) std::ceil ((end - s.beat) / w - 1.0e-9));
        }
        // beyond the map: continue with the last meter era
        const auto* s = eraOf (list, 1.0e18);
        const double w = barWidth (*s);
        const int k = (int) std::floor ((beat - s->beat) / w + 1.0e-9);
        r.bar += k;
        r.beatInBar = beat - (s->beat + k * w);
        r.num = s->tsNum; r.den = s->tsDen;
        return r;
    }

    double beatOfBar (const std::vector<TempoSegment>& list, int barIndex)
    {
        if (barIndex <= 0 || list.empty()) return 0.0;
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (i > 0 && list[i].tsNum == list[i - 1].tsNum && list[i].tsDen == list[i - 1].tsDen) continue;
            const auto& s = list[i];
            const double w = barWidth (s);
            const double end = eraEnd (list, i);
            if (end > 9.0e17) return s.beat + barIndex * w;    // last era: all remaining bars
            const int bars = jmax (1, (int) std::ceil ((end - s.beat) / w - 1.0e-9));
            if (barIndex < bars) return s.beat + barIndex * w;
            barIndex -= bars;
        }
        const auto& s = list.back();
        return s.beat + (double) barIndex * barWidth (s);
    }
}

//==============================================================================
Project::Project() { reset(); }

TempoSegment* Project::findTempoSegment (double beat)
{
    for (auto& s : tempo)
        if (std::abs (s.beat - beat) < 1.0e-6)
            return &s;
    return nullptr;
}

TempoSegment* Project::findTempoSegmentAt (double beat)
{
    TempoSegment* best = nullptr;
    for (auto& s : tempo)
        if (s.beat <= beat + 1.0e-9 && (best == nullptr || s.beat >= best->beat))
            best = &s;
    return best;
}

void Project::setTempo (double beat, double bpm, int num, int den)
{
    // a seconds-timebase clip stays at its absolute position while the map changes
    const TempoEdit keepSeconds (*this);

    beat = jmax (0.0, beat);
    if (auto* s = findTempoSegment (beat))
    {
        if (bpm >= 0.0) s->bpm = jlimit (20.0, 400.0, bpm);
        if (num > 0) s->tsNum = num;
        if (den > 0) s->tsDen = den;
    }
    else
    {
        TempoSegment seg;
        seg.beat = beat;
        seg.bpm = bpm > 0.0 ? jlimit (20.0, 400.0, bpm) : bpmAt (beat);
        seg.tsNum = num > 0 ? num : 4;
        seg.tsDen = den > 0 ? den : 4;
        if (num <= 0 || den <= 0)
        {
            int n, d;
            meterAt (beat, n, d);
            if (num <= 0) seg.tsNum = n;
            if (den <= 0) seg.tsDen = d;
        }
        tempo.push_back (seg);
    }
    tempomap::normalize (tempo);
    syncLegacyTempo();
}

void Project::setTempoRamp (double beat, bool ramp)
{
    if (auto* s = findTempoSegment (beat))
    {
        if (s->ramp == ramp) return;
        const TempoEdit keepSeconds (*this);
        s->ramp = ramp;
    }
    else
        return;
    tempomap::normalize (tempo);
    syncLegacyTempo();
}

void Project::removeTempo (double beat)
{
    const TempoEdit keepSeconds (*this);
    tempo.erase (std::remove_if (tempo.begin(), tempo.end(), [beat] (const TempoSegment& s)
                                 { return s.beat > 0.0 && std::abs (s.beat - beat) < 1.0e-6; }),
                 tempo.end());
    tempomap::normalize (tempo);
    syncLegacyTempo();
}

//==============================================================================
void Project::captureSecondsClips()
{
    secondsClips.clear();

    for (auto& t : tracks)
    {
        if (t == nullptr) continue;
        for (auto& c : t->audioClips)
            if (c.timebase == Timebase::Seconds)
                secondsClips.push_back ({ t->id, c.id, beatsToSeconds (c.start) });
    }
}

void Project::restoreSecondsClips()
{
    for (auto& s : secondsClips)
        if (auto* t = findTrack (s.trackId))
            for (auto& c : t->audioClips)
                if (c.id == s.clipId)
                    c.start = jmax (0.0, secondsToBeats (s.seconds));

    secondsClips.clear();
}

void Project::syncLegacyTempo()
{
    if (tempo.empty()) return;
    bpm = tempo.front().bpm;
    tsNum = tempo.front().tsNum;
    tsDen = tempo.front().tsDen;
}

//==============================================================================

void Project::sendContentChanged()   { dirty = true; listeners.call ([] (Listener& l) { l.projectContentChanged(); }); }
void Project::sendMixChanged()       { dirty = true; listeners.call ([] (Listener& l) { l.projectMixChanged(); }); }
void Project::sendStructureChanged() { refreshFolders(); dirty = true; listeners.call ([] (Listener& l) { l.projectStructureChanged(); }); }

void Project::refreshFolders()
{
    for (auto& t : tracks)
        if (t != nullptr) { t->folderDepth = 0; t->folderHidden = false; }

    for (auto& t : tracks)
    {
        if (t == nullptr) continue;
        const int parent = t->folderParent;
        if (parent != 0 && findTrack (parent) == nullptr)
            t->folderParent = 0;          // a dangling parent (deleted folder) becomes top level

        int p = t->folderParent, depth = 0;
        bool hiddenByFolder = false;
        // the depth is what indents the row; a closed ancestor hides the whole subtree. Guard
        // against a hand-edited / corrupted project that points a folder at itself.
        while (p != 0 && depth < 64)
        {
            auto* folder = findTrack (p);
            if (folder == nullptr) break;
            ++depth;
            if (! folder->folderOpen) hiddenByFolder = true;
            p = folder->folderParent;
        }
        t->folderDepth = depth;
        t->folderHidden = hiddenByFolder;
    }
}

int Project::folderChildCount (int folderId) const
{
    int n = 0;
    for (auto& t : tracks) if (t != nullptr && t->folderParent == folderId) ++n;
    return n;
}

std::vector<int> Project::folderChildren (int folderId) const
{
    std::vector<int> ids;
    for (auto& t : tracks) if (t != nullptr && t->folderParent == folderId) ids.push_back (t->id);
    return ids;
}

Track* Project::findTrack (int id)
{
    for (auto& t : tracks) if (t->id == id) return t.get();
    return nullptr;
}

const Track* Project::findTrack (int id) const
{
    for (auto& t : tracks) if (t->id == id) return t.get();
    return nullptr;
}

int Project::indexOfTrack (int id) const
{
    for (size_t i = 0; i < tracks.size(); ++i) if (tracks[i]->id == id) return (int) i;
    return -1;
}

Track& Project::addTrack (TrackType type, const String& name)
{
    auto t = std::make_unique<Track>();
    t->id = newId();
    t->type = type;
    int count = 0;
    for (auto& o : tracks) if (o->type == type) ++count;
    const String base = type == TrackType::Bus ? TRANS ("Bus ") : type == TrackType::Send ? TRANS ("FX Send ")
                     : type == TrackType::Automation ? TRANS ("Automation ")
                     : type == TrackType::Audio ? TRANS ("Audio ") : TRANS ("Inst ");
    t->name = name.isNotEmpty() ? name : base + String (count + 1);
    t->colour = type == TrackType::Bus ? theme::busTrackColour
              : type == TrackType::Send ? theme::sendTrackColour
              : theme::trackPalette()[(int) tracks.size() % theme::trackPalette().size()];
    // An instrument track starts without an instrument: the user picks the plug-in (the built-in
    // synth is one of the choices, but loading it unasked would put a sound on every empty track).
    tracks.push_back (std::move (t));
    return *tracks.back();
}

//==============================================================================
Track& Project::addOutputChannel (int instrumentTrackId, int sourceChannel, const String& name)
{
    auto& t = addTrack (TrackType::Instrument, name);
    t.hasInstrument = false;          // it plays the parent's instrument, not its own
    t.sourceTrack = instrumentTrackId;
    t.sourceChannel = jmax (0, sourceChannel);
    t.hidden = true;                  // a mixer channel: the arranger stays about the sources
    const int newTrackId = t.id;

    if (auto* parent = findTrack (instrumentTrackId))
    {
        const auto colour = parent->colour;
        const int parentIdx = indexOfTrack (instrumentTrackId);

        // right after the parent and its existing output channels, so the mixer reads left to
        // right like the plug-in's outputs
        int at = parentIdx + 1;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i]->id != newTrackId && tracks[i]->sourceTrack == instrumentTrackId)
                at = jmax (at, (int) i + 1);

        auto child = std::move (tracks.back());
        child->colour = colour;
        tracks.pop_back();
        at = jlimit (0, (int) tracks.size(), at);
        tracks.insert (tracks.begin() + at, std::move (child));
    }

    auto* created = findTrack (newTrackId);
    jassert (created != nullptr);
    return created != nullptr ? *created : *tracks.back();
}

int Project::numOutputChannels (int instrumentTrackId) const
{
    int n = 0;
    for (auto& t : tracks) if (t != nullptr && t->sourceTrack == instrumentTrackId) ++n;
    return n;
}

int Project::removeOutputChannels (int instrumentTrackId)
{
    int removed = 0;
    for (int i = (int) tracks.size() - 1; i >= 0; --i)
        if (tracks[(size_t) i] != nullptr && tracks[(size_t) i]->sourceTrack == instrumentTrackId)
        {
            const int id = tracks[(size_t) i]->id;
            tracks.erase (tracks.begin() + i);
            // drops that pointed at it (the same cleanup removeTrack() does)
            for (auto& t : tracks)
            {
                if (t->outputBusId == id) t->outputBusId = 0;
                auto& s = t->sends;
                s.erase (std::remove_if (s.begin(), s.end(), [id] (const SendSlot& x) { return x.target == id; }), s.end());
            }
            ++removed;
        }
    return removed;
}

void Project::removeTrack (int id)
{
    // a multi-output instrument takes its channel tracks with it: they would play silence
    for (int i = (int) tracks.size() - 1; i >= 0; --i)
        if (tracks[(size_t) i] != nullptr && tracks[(size_t) i]->sourceTrack == id)
            tracks.erase (tracks.begin() + i);

    // stand-alone automation tracks belong to their channel: they go with it
    for (int i = (int) tracks.size() - 1; i >= 0; --i)
        if (tracks[(size_t) i] != nullptr && tracks[(size_t) i]->isAutomation()
            && tracks[(size_t) i]->autoTargetTrack == id)
            tracks.erase (tracks.begin() + i);

    int removedOutput = 0;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i]->id == id)
        {
            // packed tracks survive their folder: they move one level up, and keep feeding what
            // the folder was feeding (so removing a folder does not mute the mix)
            removedOutput = tracks[i]->outputBusId;
            for (auto& t : tracks)
                if (t->folderParent == id) t->folderParent = tracks[i]->folderParent;
            tracks.erase (tracks.begin() + (long long) i);
            // drop any routing that pointed at the removed track
            for (auto& t : tracks)
            {
                if (t->outputBusId == id) t->outputBusId = removedOutput;
                auto& s = t->sends;
                s.erase (std::remove_if (s.begin(), s.end(), [id] (const SendSlot& x) { return x.target == id; }), s.end());
            }
            refreshFolders();
            return;
        }
    refreshFolders();
}

double Project::endBeat() const
{
    double e = 0.0;
    for (auto& t : tracks) e = jmax (e, t->endBeat (tempo));
    return e;
}

bool Project::anySolo() const
{
    for (auto& t : tracks) if (t->solo) return true;
    return false;
}

//==============================================================================
// chord track
namespace
{
    bool sameBeat (double a, double b) { return std::abs (a - b) < 1.0e-4; }

    void sortChords (std::vector<ChordEvent>& list)
    {
        std::stable_sort (list.begin(), list.end(),
                          [] (const ChordEvent& a, const ChordEvent& b) { return a.beat < b.beat; });
    }
}

void Project::normalizeChords()
{
    sortChords (chords);
    std::vector<ChordEvent> out;
    out.reserve (chords.size());
    for (auto& c : chords)
    {
        c.beat = jmax (0.0, c.beat);
        if (c.symbol.trim().isEmpty()) continue;                  // an empty symbol is not a chord
        c.length = jmax (kMinChordBeats, c.length);
        if (! out.empty())
        {
            auto& prev = out.back();
            const double prevEnd = prev.beat + prev.length;
            // blocks never overlap: a chord that starts inside the previous one splits it
            if (c.beat < prevEnd - 1.0e-9)
            {
                const double room = c.beat - prev.beat;
                if (room >= kMinChordBeats) prev.length = room;
                else c.beat = prevEnd;
            }
        }
        out.push_back (c);
    }
    chords = std::move (out);
}

ChordEvent& Project::addChord (double beat, const String& symbol, double length)
{
    // one chord per beat: a new chord replaces whatever sat exactly there
    chords.erase (std::remove_if (chords.begin(), chords.end(),
                                  [&] (const ChordEvent& e) { return sameBeat (e.beat, beat); }),
                  chords.end());
    ChordEvent c;
    c.id = newId();
    c.beat = jmax (0.0, beat);
    c.length = jmax (kMinChordBeats, length);
    c.symbol = symbol;
    chords.push_back (c);
    normalizeChords();
    if (auto* added = findChord (c.id)) return *added;
    return chords.back();
}

ChordEvent* Project::findChord (int id)
{
    for (auto& c : chords) if (c.id == id) return &c;
    return nullptr;
}

ChordEvent* Project::chordAt (double beat)
{
    for (auto& c : chords)
    {
        if (c.beat > beat + 1.0e-9) break;
        if (beat < c.beat + c.length - 1.0e-9) return &c;
    }
    return nullptr;
}

const ChordEvent* Project::chordAt (double beat) const
{
    return const_cast<Project*> (this)->chordAt (beat);
}

void Project::moveChord (int id, double beat, bool removeCollisions)
{
    if (findChord (id) == nullptr) return;
    beat = jmax (0.0, beat);
    if (removeCollisions)
        chords.erase (std::remove_if (chords.begin(), chords.end(),
                                      [&] (const ChordEvent& e) { return e.id != id && sameBeat (e.beat, beat); }),
                      chords.end());
    if (auto* moved = findChord (id)) moved->beat = beat;
    if (removeCollisions) normalizeChords();          // the drop may split the chord it lands in
    else sortChords (chords);                        // while dragging it may pass over others
}

void Project::resizeChord (int id, double length)
{
    auto* c = findChord (id);
    if (c == nullptr) return;
    double maxLen = 1.0e9;
    for (auto& o : chords)
        if (o.id != id && o.beat > c->beat + 1.0e-9) maxLen = jmin (maxLen, o.beat - c->beat);
    c->length = jlimit (kMinChordBeats, jmax (kMinChordBeats, maxLen), jmax (0.0, length));
}


void Project::setChordSymbol (int id, const String& symbol)
{
    auto* c = findChord (id);
    if (c == nullptr) return;
    c->symbol = symbol;
    if (symbol.trim().isEmpty()) removeChord (id);
}

void Project::removeChord (int id)
{
    chords.erase (std::remove_if (chords.begin(), chords.end(),
                                  [id] (const ChordEvent& e) { return e.id == id; }),
                  chords.end());
}

void Project::clearChords() { chords.clear(); }

//==============================================================================
// marker track (arrangement sections)
juce::Colour defaultSectionColour (int index)
{
    static const juce::Colour palette[] {
        juce::Colour (0xff4c8bf5), juce::Colour (0xffe0873a), juce::Colour (0xff46a97a),
        juce::Colour (0xffa05fc4), juce::Colour (0xffd15a7c), juce::Colour (0xff3aa8b8),
        juce::Colour (0xffc9a227), juce::Colour (0xff7a8794)
    };
    constexpr int n = (int) (sizeof (palette) / sizeof (palette[0]));
    return palette[((index % n) + n) % n];
}

String suggestSectionName (const std::vector<SectionMarker>& existing)
{
    static const char* const base[] { "Intro", "Verse", "Pre-Chorus", "Chorus", "Bridge", "Solo", "Break", "Outro" };
    constexpr int n = (int) (sizeof (base) / sizeof (base[0]));
    const auto taken = [&existing] (const String& name)
    {
        for (auto& s : existing) if (s.name.equalsIgnoreCase (name)) return true;
        return false;
    };

    for (int i = 0; i < n; ++i)
    {
        const String name = TRANS (base[i]);
        if (! taken (name)) return name;
    }

    // every basic name is in use: number them ("Verse 2", "Verse 3", ...)
    for (int i = 0; i < n; ++i)
    {
        const String root = TRANS (base[i]);
        int count = 0;
        for (auto& s : existing)
            if (s.name.startsWithIgnoreCase (root)) ++count;
        for (int k = count + 1; k < count + 32; ++k)
        {
            const String candidate = root + " " + String (k);
            if (! taken (candidate)) return candidate;
        }
    }
    return TRANS ("Section") + " " + String ((int) existing.size() + 1);
}

void Project::normalizeSections()
{
    std::stable_sort (sections.begin(), sections.end(),
                      [] (const SectionMarker& a, const SectionMarker& b) { return a.beat < b.beat; });
    for (size_t i = 0; i < sections.size(); ++i)
    {
        auto& s = sections[i];
        s.beat = jmax (0.0, s.beat);
        if (i > 0)
        {
            const auto& prev = sections[i - 1];
            s.beat = jmax (s.beat, prev.beat + prev.length);      // sections never overlap
        }
        s.length = jmax (kMinSectionBeats, s.length);
        if (i + 1 < sections.size())
            s.length = jmin (s.length, jmax (kMinSectionBeats, sections[i + 1].beat - s.beat));
        if (s.name.trim().isEmpty()) s.name = TRANS ("Section");
        if (! s.colour.isOpaque()) s.colour = defaultSectionColour ((int) i);
    }
}

SectionMarker& Project::addSection (double beat, double length, const String& name)
{
    SectionMarker s;
    s.id = newId();
    s.beat = jmax (0.0, beat);
    s.length = jmax (kMinSectionBeats, length);
    s.colour = defaultSectionColour ((int) sections.size());
    s.name = name.trim().isNotEmpty() ? name.trim() : suggestSectionName (sections);
    sections.push_back (s);
    normalizeSections();
    if (auto* added = findSection (s.id)) return *added;
    return sections.back();
}

SectionMarker* Project::findSection (int id)
{
    for (auto& s : sections) if (s.id == id) return &s;
    return nullptr;
}

SectionMarker* Project::sectionAt (double beat)
{
    for (auto& s : sections)
        if (beat >= s.beat - 1.0e-9 && beat < s.beat + s.length - 1.0e-9) return &s;
    return nullptr;
}

const SectionMarker* Project::sectionAt (double beat) const
{
    return const_cast<Project*> (this)->sectionAt (beat);
}

void Project::moveSection (int id, double beat)
{
    auto* s = findSection (id);
    if (s == nullptr) return;
    int index = -1;
    for (int i = 0; i < (int) sections.size(); ++i)
        if (sections[(size_t) i].id == id) { index = i; break; }
    if (index < 0) return;

    // the section may slide inside the gap between its neighbours, but never over them
    const double minBeat = index > 0 ? sections[(size_t) index - 1].beat + sections[(size_t) index - 1].length : 0.0;
    const double maxBeat = index + 1 < (int) sections.size()
                               ? jmax (minBeat, sections[(size_t) index + 1].beat - kMinSectionBeats)
                               : 1.0e9;
    s->beat = jlimit (minBeat, maxBeat, jmax (0.0, beat));
    if (index + 1 < (int) sections.size())
        s->length = jmin (s->length, jmax (kMinSectionBeats, sections[(size_t) index + 1].beat - s->beat));
    s->length = jmax (kMinSectionBeats, s->length);
}

void Project::resizeSection (int id, double length)
{
    auto* s = findSection (id);
    if (s == nullptr) return;
    double maxLen = 1.0e9;
    for (auto& o : sections)
        if (o.id != id && o.beat > s->beat + 1.0e-9) maxLen = jmin (maxLen, o.beat - s->beat);
    s->length = jlimit (kMinSectionBeats, jmax (kMinSectionBeats, maxLen), jmax (0.0, length));
}

void Project::setSectionName (int id, const String& name)
{
    if (auto* s = findSection (id))
        s->name = name.trim().isNotEmpty() ? name.trim() : s->name;
}

void Project::removeSection (int id)
{
    sections.erase (std::remove_if (sections.begin(), sections.end(),
                                    [id] (const SectionMarker& s) { return s.id == id; }),
                    sections.end());
}

void Project::clearSections() { sections.clear(); }

int Project::addSectionAfter (int id)
{
    auto* s = findSection (id);
    if (s == nullptr) return 0;
    const double start = s->beat + s->length;
    const double len = s->length;
    auto& added = addSection (start, len, {});
    return added.id;
}

File Project::audioFilesFolderFor (const File& projectFile)
{
    return projectFile.getParentDirectory().getChildFile ("Audio Files");
}

static String projectRelativePath (const File& projectFile, const File& f)
{
    if (f.getFullPathName().isEmpty()) return {};
    if (projectFile.existsAsFile())
    {
        const auto rel = f.getRelativePathFrom (projectFile.getParentDirectory());
        if (rel.isNotEmpty() && ! rel.startsWith ("..")) return rel;   // portable path
    }
    return f.getFullPathName();
}

int Project::collectAudioFiles (const File& projectFile)
{
    if (projectFile.getFullPathName().isEmpty()) return 0;
    const File folder = audioFilesFolderFor (projectFile);
    const File tempRoot = File::getSpecialLocation (File::tempDirectory).getChildFile ("SoundX");

    // relocate each unique source file once, then rewrite every clip that used it
    std::map<String, File> remap;
    int relocated = 0;
    auto relocate = [&] (const File& src) -> File
    {
        const String key = src.getFullPathName();
        if (key.isEmpty()) return src;
        if (auto it = remap.find (key); it != remap.end()) return it->second;
        if (! src.existsAsFile() || src.isAChildOf (folder)) { remap[key] = src; return src; }

        folder.createDirectory();
        File dest = folder.getChildFile (src.getFileName());
        if (dest.existsAsFile())
            dest = folder.getNonexistentChildFile (src.getFileNameWithoutExtension(), src.getFileExtension());

        // session recordings are moved out of the temp folder, imported files are copied
        const bool fromSessionTemp = src.isAChildOf (tempRoot);
        bool ok = fromSessionTemp ? src.moveFileTo (dest) : src.copyFileTo (dest);
        if (! ok) ok = src.copyFileTo (dest);
        if (! ok) { remap[key] = src; return src; }

        remap[key] = dest;
        ++relocated;
        return dest;
    };

    for (auto& t : tracks)
    {
        for (auto& c : t->audioClips)
            c.file = relocate (c.file);
        if (t->frozen && ! t->frozenFile.getFullPathName().isEmpty())
            t->frozenFile = relocate (t->frozenFile);
    }
    return relocated;
}

bool Project::writeSnapshotTo (const File& target)
{
    if (target.getFullPathName().isEmpty()) return false;
    target.getParentDirectory().createDirectory();

    // audio paths inside the snapshot are written relative to the snapshot's own folder, but the
    // project keeps its real file path and dirty state
    const File previous = file;
    const bool wasDirty = dirty;
    file = target;
    auto xml = toXml (true);
    // remember which project the snapshot came from, so recovery can tell if it is newer
    if (xml != nullptr) xml->setAttribute ("autosaveOf", previous.getFullPathName());
    file = previous;
    dirty = wasDirty;

    TemporaryFile tmp (target);
    return xml->writeTo (tmp.getFile()) && tmp.overwriteTargetFileWithTemporary();
}

//==============================================================================
void ProjectMeta::setCover (const Image& img)
{
    cover = img;
    coverData.reset();
    if (! img.isValid()) return;

    PNGImageFormat png;
    MemoryOutputStream out (coverData, false);
    if (png.writeImageToStream (img, out)) out.flush();
}

void ProjectMeta::clearCover()
{
    cover = {};
    coverData.reset();
}

bool ProjectMeta::isEmpty() const
{
    return title.isEmpty() && author.isEmpty() && genre.isEmpty() && album.isEmpty()
        && year.isEmpty() && comment.isEmpty() && coverData.getSize() == 0;
}

//==============================================================================
void Project::clearAll()
{
    tracks.clear();
    chords.clear();
    sections.clear();
    // the master keeps its identity (id 0, name, colour); only its content is cleared
    masterTrack = Track {};
    masterTrack.id = 0;
    masterTrack.name = "Master";
    masterTrack.type = TrackType::Bus;
    masterTrack.colour = theme::masterTrackColour;
    masterTrack.autoMode = AutoMode::Read;
    masterTrack.automationInTrack = false;   // the master keeps its automation sub-rows
}

void Project::reset()
{
    clearAll();
    meta = {};
    midiMappings.clear();
    bpm = 120.0; tsNum = 4; tsDen = 4;
    tempo = { TempoSegment{} };
    loopOn = false; loopStart = 0.0; loopEnd = 8.0;
    metronome = false;
    countInBars = 0;
    recordQuantize = 0.0;
    takeRecording = true;
    file = File();
    dirty = false;
    undoStack.clear();
    redoStack.clear();
    undoBatchDepth = 0;
    undoBatchCheckpointed = false;
}

AutoLane& Project::masterFaderLane()
{
    if (auto* l = masterTrack.findLane ("vol")) return *l;
    AutoLane lane;
    lane.target = "vol";
    lane.label = "Master Volume";
    lane.visible = false;
    masterTrack.lanes.push_back (std::move (lane));
    return masterTrack.lanes.back();
}

//==============================================================================
PluginSlot* Project::findSlotByUid (int64 uid)
{
    for (auto& t : tracks)
    {
        if (t == nullptr) continue;
        if (t->hasInstrument && t->instrument.uid == uid) return &t->instrument;
        for (auto& s : t->inserts) if (s.uid == uid) return &s;
    }
    for (auto& s : masterTrack.inserts) if (s.uid == uid) return &s;
    return nullptr;
}

std::set<int64> Project::collectSlotUids() const
{
    std::set<int64> uids;
    for (auto& t : tracks)
    {
        if (t == nullptr) continue;
        if (t->hasInstrument) uids.insert (t->instrument.uid);
        for (auto& s : t->inserts) uids.insert (s.uid);
    }
    for (auto& s : masterTrack.inserts) uids.insert (s.uid);
    return uids;
}

void Project::pushUndo (EditSnapshot&& snapshot)
{
    undoStack.push_back (std::move (snapshot));
    if (undoStack.size() > 200) undoStack.erase (undoStack.begin());
    redoStack.clear();
    dirty = true;
}

void Project::checkpoint()
{
    // Inside a batch only the state from before its first change is worth keeping: the rest of the
    // batch is one and the same undo step (that is what makes an import one Ctrl+Z).
    if (undoBatchDepth > 0 && undoBatchCheckpointed)
        return;

    EditSnapshot s;
    s.project = toXml (false);
    s.slotUids = collectSlotUids();
    if (capturePluginParams != nullptr) capturePluginParams (s.pluginParams);
    pushUndo (std::move (s));

    if (undoBatchDepth > 0)
        undoBatchCheckpointed = true;
}

void Project::beginUndoBatch()
{
    ++undoBatchDepth;
}

void Project::endUndoBatch()
{
    if (--undoBatchDepth > 0)
        return;                        // a nested batch: the outer one still owns the undo step

    undoBatchDepth = 0;
    undoBatchCheckpointed = false;
}

void Project::checkpointRemoving (const std::set<int64>& slotUidsToSave)
{
    EditSnapshot s;
    s.project = toXml (false);
    s.slotUids = collectSlotUids();
    if (capturePluginParams != nullptr) capturePluginParams (s.pluginParams);
    if (capturePluginStatesFor != nullptr)
    {
        std::set<int64> wanted;
        for (auto uid : slotUidsToSave) if (s.slotUids.count (uid) > 0) wanted.insert (uid);
        if (! wanted.empty()) capturePluginStatesFor (wanted, s.pluginStates);
    }
    pushUndo (std::move (s));
}

// The counter entry of an undo/redo has to hold the state the project is in right now, including
// the plug-ins that this step is about to delete (their only other copy would be gone).
Project::EditSnapshot Project::counterSnapshotFor (const EditSnapshot& step)
{
    EditSnapshot s;
    s.project = toXml (false);
    s.slotUids = collectSlotUids();
    if (capturePluginParams != nullptr) capturePluginParams (s.pluginParams);

    if (capturePluginStatesFor != nullptr)
    {
        std::set<int64> lost;
        for (auto uid : s.slotUids) if (step.slotUids.count (uid) == 0) lost.insert (uid);
        if (! lost.empty()) capturePluginStatesFor (lost, s.pluginStates);
    }
    return s;
}

void Project::undo()
{
    if (undoStack.empty()) return;
    auto step = std::move (undoStack.back());
    undoStack.pop_back();
    redoStack.push_back (counterSnapshotFor (step));
    restoreFrom (step);
}

void Project::redo()
{
    if (redoStack.empty()) return;
    auto step = std::move (redoStack.back());
    redoStack.pop_back();
    undoStack.push_back (counterSnapshotFor (step));
    restoreFrom (step);
}

void Project::restoreFrom (const EditSnapshot& step)
{
    if (step.project != nullptr) fromXml (*step.project);

    // the model carries only the plug-in descriptions: the opaque state of the slots this step
    // brings back is written here, before the listeners rebuild the engine's instances from it
    for (auto& [uid, blob] : step.pluginStates)
        if (auto* slot = findSlotByUid (uid); slot != nullptr && blob.getSize() > 0)
            slot->state = blob;

    dirty = true;
    sendStructureChanged();
    sendContentChanged();
    sendMixChanged();

    // parameter values live in the plug-in instances, so they are restored after the tree (and the
    // instances that were missing from it) are back
    if (applyPluginParams != nullptr && ! step.pluginParams.empty())
        applyPluginParams (step.pluginParams);
}

//==============================================================================
static std::unique_ptr<XmlElement> slotToXml (const PluginSlot& s, const char* tag, bool withState)
{
    auto e = std::make_unique<XmlElement> (tag);
    e->setAttribute ("uid", String (s.uid));
    e->setAttribute ("bypass", s.bypass);
    e->setAttribute ("builtIn", s.builtIn);
    if (s.postFader) e->setAttribute ("post", true);
    if (std::abs (s.gainDb) > 1.0e-4) e->setAttribute ("gainDb", (double) s.gainDb);
    if (std::abs (s.pan) > 1.0e-4) e->setAttribute ("pan", (double) s.pan);
    if (s.sidechainTrack != 0) e->setAttribute ("sidechain", s.sidechainTrack);
    if (auto d = s.desc.createXml()) e->addChildElement (d.release());
    if (withState && s.state.getSize() > 0)
        e->setAttribute ("state", s.state.toBase64Encoding());
    if (withState && s.araState.getSize() > 0)
        e->setAttribute ("araState", s.araState.toBase64Encoding());
    return e;
}

static PluginSlot slotFromXml (const XmlElement& e)
{
    PluginSlot s;
    s.uid = e.getStringAttribute ("uid").getLargeIntValue();
    if (s.uid == 0) s.uid = makeUid();
    s.bypass = e.getBoolAttribute ("bypass");
    s.builtIn = e.getBoolAttribute ("builtIn");
    s.postFader = e.getBoolAttribute ("post");
    s.gainDb = (float) e.getDoubleAttribute ("gainDb", 0.0);
    s.pan = (float) jlimit (-1.0, 1.0, e.getDoubleAttribute ("pan", 0.0));
    s.sidechainTrack = e.getIntAttribute ("sidechain", 0);
    if (auto* d = e.getChildByName ("PLUGIN")) s.desc.loadFromXml (*d);
    if (e.hasAttribute ("state")) s.state.fromBase64Encoding (e.getStringAttribute ("state"));
    if (e.hasAttribute ("araState")) s.araState.fromBase64Encoding (e.getStringAttribute ("araState"));
    return s;
}

// automation lanes: one "Lane" element per lane, one "P" per node. Tracks and the master bus
// use exactly the same shape, so a lane moves between them without any conversion.
static void lanesToXml (XmlElement& parent, const std::vector<AutoLane>& lanes)
{
    for (auto& l : lanes)
    {
        auto* le = parent.createNewChildElement ("Lane");
        le->setAttribute ("target", l.target);
        le->setAttribute ("label", l.label);
        le->setAttribute ("visible", l.visible);
        for (auto& p : l.points)
        {
            auto* pe = le->createNewChildElement ("P");
            pe->setAttribute ("b", p.beat);
            pe->setAttribute ("v", (double) p.value);
        }
    }
}

static void lanesFromXml (const XmlElement& parent, std::vector<AutoLane>& lanes)
{
    for (auto* le : parent.getChildWithTagNameIterator ("Lane"))
    {
        AutoLane l;
        l.target = le->getStringAttribute ("target");
        l.label = le->getStringAttribute ("label");
        l.visible = le->getBoolAttribute ("visible");
        for (auto* pe : le->getChildWithTagNameIterator ("P"))
            l.points.push_back ({ pe->getDoubleAttribute ("b"), (float) pe->getDoubleAttribute ("v") });
        normalizeAutoPoints (l.points);   // older files may hold unordered or duplicated beats
        if (l.target.isNotEmpty()) lanes.push_back (std::move (l));
    }
}

double Project::crossfade (AudioClip& a, AudioClip& b, double fadeSec)
{
    if (a.id == b.id || a.data == nullptr || b.data == nullptr) return 0.0;
    if (! (b.start > a.start)) return 0.0;                    // the caller orders the pair
    const double aStartSec = beatsToSeconds (a.start);
    const double aEndSec   = aStartSec + a.lengthSec;
    const double bStartSec = beatsToSeconds (b.start);
    const double bEndSec   = bStartSec + b.lengthSec;
    const double existing  = aEndSec - bStartSec;
    if (existing > 0.005) return existing;                    // already overlapping: the engine crossfades

    const double ov = jlimit (0.01, fadeSec, jmin (fadeSec, 0.45 * a.lengthSec, 0.45 * b.lengthSec));
    if (bStartSec - aEndSec > ov) return 0.0;                 // too far apart: nothing to bridge

    // the new overlap is centred on the junction: A reaches right, B reaches left
    const double roomA = (double) a.data->lengthSeconds() - (a.offsetSec + a.lengthSec);
    const double extA  = jmin (ov * 0.5, jmax (0.0, roomA));
    const double newAEndSec = aEndSec + extA;
    const double newBStartSec = newAEndSec - ov;
    const double shiftB = jmax (0.0, bStartSec - newBStartSec);
    // B's audio stays where it is when the source has head material; otherwise it slides
    const double newBOffset = jmax (0.0, b.offsetSec - shiftB);
    const double newBLen = jmax (0.02, bEndSec - newBStartSec);
    const double maxB = (double) b.data->lengthSeconds() - newBOffset;

    if (newAEndSec - newBStartSec <= 0.005) return 0.0;

    a.lengthSec = jmax (0.02, newAEndSec - aStartSec);
    b.start     = secondsToBeats (newBStartSec);
    b.offsetSec = newBOffset;
    b.lengthSec = jmin (newBLen, jmax (0.02, maxB));
    return aEndSec + extA - newBStartSec;
}

double Project::resizeCrossfade (AudioClip& a, AudioClip& b, double sec)
{
    if (a.id == b.id || a.data == nullptr || b.data == nullptr) return 0.0;
    if (! (b.start > a.start)) return 0.0;
    const double aEnd = beatsToSeconds (a.start) + a.lengthSec;
    const double bStart = beatsToSeconds (b.start);
    const double ov = aEnd - bStart;
    if (ov <= 1.0e-3) return 0.0;                       // no overlap to size

    // +d = longer overlap: B starts earlier, so it needs head material in the file
    const double maxD = jmin (b.offsetSec, jmax (0.0, 0.9 * a.lengthSec - ov));
    const double minD = -(ov - 0.01);
    const double d = jlimit (minD, maxD, sec - ov);
    if (std::abs (d) < 1.0e-6) return ov;

    const double bEnd = bStart + b.lengthSec;
    const double newStart = bStart - d;
    b.start = secondsToBeats (newStart);
    b.offsetSec = jmax (0.0, b.offsetSec - d);
    b.lengthSec = jmax (0.02, bEnd - newStart);
    return aEnd - newStart;
}

std::unique_ptr<XmlElement> Project::toXml (bool withState) const
{
    if (withState && capturePluginStates) capturePluginStates();

    auto root = std::make_unique<XmlElement> ("SXProject");
    root->setAttribute ("version", 1);
    root->setAttribute ("bpm", bpm);
    root->setAttribute ("tsNum", tsNum);
    root->setAttribute ("tsDen", tsDen);

    // the master bus: one element with the same channel data a track carries
    {
        auto* mb = root->createNewChildElement ("MasterBus");
        mb->setAttribute ("vol", (double) masterTrack.volumeDb);
        if (masterTrack.mute) mb->setAttribute ("mute", true);
        if (masterTrack.mono) mb->setAttribute ("mono", true);
        if (masterTrack.phaseInvert) mb->setAttribute ("phase", true);
        mb->setAttribute ("autoMode", (int) masterTrack.autoMode);
        if (masterTrack.height > 0) mb->setAttribute ("height", masterTrack.height);
        lanesToXml (*mb, masterTrack.lanes);
        for (auto& s : masterTrack.inserts) mb->addChildElement (slotToXml (s, "Insert", withState).release());
    }
    // kept for older builds: the master fader value at the root
    root->setAttribute ("masterDb", (double) masterTrack.volumeDb);

    root->setAttribute ("loopOn", loopOn);
    root->setAttribute ("loopStart", loopStart);
    root->setAttribute ("loopEnd", loopEnd);
    root->setAttribute ("metronome", metronome);
    root->setAttribute ("countIn", countInBars);
    if (recordQuantize > 0.0) root->setAttribute ("recQuantize", recordQuantize);
    if (! takeRecording) root->setAttribute ("takeRecording", false);
    root->setAttribute ("idCounter", idCounter);

    //== MIDI learn mappings
    if (! midiMappings.empty())
    {
        auto* learn = root->createNewChildElement ("MidiLearn");
        for (auto& m : midiMappings)
        {
            auto* e = learn->createNewChildElement ("Map");
            e->setAttribute ("cc", m.cc);
            e->setAttribute ("ch", m.channel);
            e->setAttribute ("kind", (int) m.target.kind);
            if (m.target.trackId != 0) e->setAttribute ("track", m.target.trackId);
            if (m.target.uid != 0) e->setAttribute ("uid", String (m.target.uid));
            if (m.target.paramIndex >= 0) e->setAttribute ("param", m.target.paramIndex);
            if (m.isNote) e->setAttribute ("note", true);
            if (m.label.isNotEmpty()) e->setAttribute ("label", m.label);
        }
    }

    if (! meta.isEmpty())
    {
        auto* info = root->createNewChildElement ("Info");
        info->setAttribute ("title", meta.title);
        info->setAttribute ("author", meta.author);
        info->setAttribute ("genre", meta.genre);
        info->setAttribute ("album", meta.album);
        info->setAttribute ("year", meta.year);
        info->setAttribute ("comment", meta.comment);
        if (meta.coverData.getSize() > 0)
            info->createNewChildElement ("Cover")
                ->addTextElement (meta.coverData.toBase64Encoding());
    }

    {
        auto* tm = root->createNewChildElement ("Tempo");
        for (auto& s : tempo)
        {
            auto* se = tm->createNewChildElement ("Seg");
            se->setAttribute ("beat", s.beat);
            se->setAttribute ("bpm", s.bpm);
            se->setAttribute ("num", s.tsNum);
            se->setAttribute ("den", s.tsDen);
            if (s.ramp) se->setAttribute ("ramp", true);
        }
    }

    // chord track: the symbols exactly as they were typed, each one its own block
    {
        auto* ct = root->createNewChildElement ("Chords");
        for (auto& c : chords)
        {
            auto* ce = ct->createNewChildElement ("Chord");
            ce->setAttribute ("beat", c.beat);
            ce->setAttribute ("len", c.length);
            ce->setAttribute ("sym", c.symbol);
        }
    }

    // marker track: named arrangement sections
    {
        auto* mt = root->createNewChildElement ("Sections");
        for (auto& s : sections)
        {
            auto* se = mt->createNewChildElement ("Marker");
            se->setAttribute ("beat", s.beat);
            se->setAttribute ("len", s.length);
            se->setAttribute ("name", s.name);
            se->setAttribute ("colour", s.colour.toString());
        }
    }

    for (auto& tp : tracks)
    {
        auto& t = *tp;
        auto* e = root->createNewChildElement ("Track");
        e->setAttribute ("id", t.id);
        e->setAttribute ("name", t.name);
        e->setAttribute ("colour", t.colour.toString());
        e->setAttribute ("type", (int) t.type);
        if (t.isListenBus)
        {
            e->setAttribute ("listen", true);
            e->setAttribute ("listenOn", t.listenEnabled);
        }
        e->setAttribute ("vol", (double) t.volumeDb);
        e->setAttribute ("pan", (double) t.pan);
        e->setAttribute ("mute", t.mute);
        if (t.phaseInvert) e->setAttribute ("phase", true);
        e->setAttribute ("solo", t.solo);
        if (t.listenSolo) e->setAttribute ("listenSolo", true);
        e->setAttribute ("arm", t.arm);
        e->setAttribute ("monitor", t.monitor);
        e->setAttribute ("hidden", t.hidden);
        e->setAttribute ("delayMs", t.delayMs);
        e->setAttribute ("frozen", t.frozen);
        if (t.frozen)
        {
            e->setAttribute ("frozenNoFx", t.frozenNoFx);
            e->setAttribute ("frozenStart", t.frozenStartBeat);
            e->setAttribute ("frozenLatency", t.frozenBakedLatency);
            e->setAttribute ("frozenFile", projectRelativePath (file, t.frozenFile));
        }
        e->setAttribute ("ch", t.midiChannel);
        e->setAttribute ("in", t.inputChannel);
        e->setAttribute ("inMono", t.inputMono);
        e->setAttribute ("autoMode", (int) t.autoMode);
        e->setAttribute ("showAuto", t.hasVisibleLanes());
        e->setAttribute ("activeLane", t.activeLane);
        e->setAttribute ("height", t.height);
        e->setAttribute ("outBus", t.outputBusId);
        // folder hierarchy (the derived depth / visibility is rebuilt on load)
        if (t.isFolder) e->setAttribute ("folder", true);
        if (! t.folderOpen) e->setAttribute ("folderOpen", false);
        if (t.folderParent != 0) e->setAttribute ("folderParent", t.folderParent);
        // stand-alone automation track: which channel / parameter it drives
        if (t.isAutomation())
        {
            e->setAttribute ("autoTrack", t.autoTargetTrack);
            e->setAttribute ("autoTarget", t.autoTarget);
            e->setAttribute ("autoLabel", t.autoLabel);
        }
        if (t.sourceTrack != 0)
        {
            e->setAttribute ("srcTrack", t.sourceTrack);
            e->setAttribute ("srcCh", t.sourceChannel);
        }
        if (! t.showOutputChannels) e->setAttribute ("hideOutputs", true);
        for (auto& s : t.sends)
        {
            auto* se = e->createNewChildElement ("Send");
            se->setAttribute ("target", s.target);
            se->setAttribute ("level", (double) s.levelDb);
            se->setAttribute ("pre", s.preFader);
        }
        if (t.hasInstrument) e->addChildElement (slotToXml (t.instrument, "Instrument", withState).release());
        // stacked instruments on the same track
        for (auto& s : t.extraInstruments) e->addChildElement (slotToXml (s, "Instrument", withState).release());
        for (auto& s : t.inserts) e->addChildElement (slotToXml (s, "Insert", withState).release());
        // inserts that were baked into a previous freeze (they come back on unfreeze). Their
        // plug-ins are not instantiated while the track is frozen, so the stored state is the
        // only copy: it is written even when the project is saved without plug-in states.
        for (auto& s : t.frozenChain) e->addChildElement (slotToXml (s, "FrozenInsert", true).release());

        for (auto& c : t.midiClips)
        {
            auto* ce = e->createNewChildElement ("MidiClip");
            ce->setAttribute ("id", c.id);
            ce->setAttribute ("name", c.name);
            ce->setAttribute ("start", c.start);
            ce->setAttribute ("length", c.length);
            ce->setAttribute ("muted", c.muted);
            if (c.takeGroup != 0) { ce->setAttribute ("takeGroup", juce::String (c.takeGroup)); ce->setAttribute ("takeIndex", c.takeIndex); ce->setAttribute ("takeActive", c.takeActive); }
            for (auto& n : c.notes)
            {
                auto* ne = ce->createNewChildElement ("N");
                ne->setAttribute ("i", n.id);
                ne->setAttribute ("p", n.pitch);
                ne->setAttribute ("s", n.start);
                ne->setAttribute ("l", n.length);
                ne->setAttribute ("v", n.velocity);
                if (n.slide) ne->setAttribute ("slide", true);
            }
            for (auto& cc : c.ccs)
            {
                auto* ke = ce->createNewChildElement ("C");
                ke->setAttribute ("i", cc.id);
                ke->setAttribute ("c", cc.cc);
                ke->setAttribute ("b", cc.beat);
                ke->setAttribute ("v", cc.value);
            }
        }
        for (auto& c : t.audioClips)
        {
            auto* ce = e->createNewChildElement ("AudioClip");
            ce->setAttribute ("id", c.id);
            ce->setAttribute ("name", c.name);
            ce->setAttribute ("file", projectRelativePath (file, c.file));
            ce->setAttribute ("start", c.start);
            ce->setAttribute ("len", c.lengthSec);
            ce->setAttribute ("off", c.offsetSec);
            ce->setAttribute ("gain", (double) c.gainDb);
            ce->setAttribute ("muted", c.muted);
            ce->setAttribute ("fadeIn", c.fadeIn);
            ce->setAttribute ("fadeOut", c.fadeOut);
            if (c.fadeInCurve != 1) ce->setAttribute ("fadeInCurve", c.fadeInCurve);
            if (c.fadeOutCurve != 1) ce->setAttribute ("fadeOutCurve", c.fadeOutCurve);
            if (c.takeGroup != 0) { ce->setAttribute ("takeGroup", juce::String (c.takeGroup)); ce->setAttribute ("takeIndex", c.takeIndex); ce->setAttribute ("takeActive", c.takeActive); }
            if (c.gainLine) ce->setAttribute ("gainLine", true);
            if (c.xfadeCurve != 0) ce->setAttribute ("xfadeCurve", c.xfadeCurve);
            if (c.reversed) ce->setAttribute ("reverse", true);
            if (c.timebase == Timebase::Seconds) ce->setAttribute ("timebase", 1);
            if (std::abs (c.stretch - 1.0) > 1.0e-9) ce->setAttribute ("stretch", c.stretch);
            if (std::abs (c.pitchSemitones) > 1.0e-6) ce->setAttribute ("pitch", (double) c.pitchSemitones);
            for (auto& p : c.gainEnv)
            {
                auto* ge = ce->createNewChildElement ("G");
                ge->setAttribute ("b", p.beat);
                ge->setAttribute ("db", (double) p.db);
            }
        }
        lanesToXml (*e, t.lanes);
    }
    return root;
}

void Project::fromXml (const XmlElement& x)
{
    clearAll();
    bpm = x.getDoubleAttribute ("bpm", 120.0);
    tsNum = x.getIntAttribute ("tsNum", 4);
    tsDen = x.getIntAttribute ("tsDen", 4);
    tempo.clear();
    if (auto* tm = x.getChildByName ("Tempo"))
        for (auto* se : tm->getChildWithTagNameIterator ("Seg"))
        {
            TempoSegment s;
            s.beat = se->getDoubleAttribute ("beat");
            s.bpm = se->getDoubleAttribute ("bpm", (double) bpm);
            s.tsNum = se->getIntAttribute ("num", tsNum);
            s.tsDen = se->getIntAttribute ("den", tsDen);
            s.ramp = se->getBoolAttribute ("ramp");
            tempo.push_back (s);
        }
    if (tempo.empty())
        tempo.push_back (TempoSegment { 0.0, bpm, tsNum, tsDen });
    tempomap::normalize (tempo);
    syncLegacyTempo();

    chords.clear();
    if (auto* ct = x.getChildByName ("Chords"))
        for (auto* ce : ct->getChildWithTagNameIterator ("Chord"))
        {
            ChordEvent c;
            c.id = newId();
            c.beat = ce->getDoubleAttribute ("beat");
            c.length = ce->getDoubleAttribute ("len", 4.0);   // projects before blocks: one bar
            c.symbol = ce->getStringAttribute ("sym");
            chords.push_back (c);
        }
    normalizeChords();

    sections.clear();
    if (auto* st = x.getChildByName ("Sections"))
    {
        int index = 0;
        for (auto* se : st->getChildWithTagNameIterator ("Marker"))
        {
            SectionMarker s;
            s.id = newId();
            s.beat = se->getDoubleAttribute ("beat");
            s.length = se->getDoubleAttribute ("len", 16.0);
            s.name = se->getStringAttribute ("name", "Section");
            s.colour = Colour::fromString (se->getStringAttribute ("colour",
                                                                   defaultSectionColour (index).toString()));
            sections.push_back (s);
            ++index;
        }
    }
    normalizeSections();

    // master bus: the MasterBus element carries the whole channel; projects written before it
    // existed are migrated from MasterLanes / MasterVolumeAuto / masterDb / <Master> inserts
    masterTrack.volumeDb = (float) x.getDoubleAttribute ("masterDb", 0.0);
    masterTrack.autoMode = AutoMode::Read;
    masterTrack.height = 0;
    masterTrack.lanes.clear();
    masterTrack.inserts.clear();
    if (auto* mb = x.getChildByName ("MasterBus"))
    {
        masterTrack.volumeDb = (float) mb->getDoubleAttribute ("vol", (double) masterTrack.volumeDb);
        masterTrack.mute = mb->getBoolAttribute ("mute");
        masterTrack.mono = mb->getBoolAttribute ("mono");
        masterTrack.phaseInvert = mb->getBoolAttribute ("phase");
        masterTrack.autoMode = (AutoMode) jlimit (0, 2, mb->getIntAttribute ("autoMode", 1));
        masterTrack.height = mb->getIntAttribute ("height", 0);
        lanesFromXml (*mb, masterTrack.lanes);
        for (auto* e : mb->getChildWithTagNameIterator ("Insert"))
            masterTrack.inserts.push_back (slotFromXml (*e));
    }
    else
    {
        masterTrack.mute = false;
        // before the master had an automation mode its curves always played: Read keeps that
        masterTrack.autoMode = AutoMode::Read;
        if (auto* ml = x.getChildByName ("MasterLanes"))
            lanesFromXml (*ml, masterTrack.lanes);
        for (auto* e : x.getChildWithTagNameIterator ("Master"))
            if (e->getTagName() == "Master")
                for (auto* ie : e->getChildWithTagNameIterator ("Insert"))
                    masterTrack.inserts.push_back (slotFromXml (*ie));
        // the old projects kept the fader curve in its own list: turn it into the "vol" lane
        if (auto* ma = x.getChildByName ("MasterVolumeAuto"))
        {
            auto& lane = masterFaderLane();
            lane.points.clear();
            for (auto* pe : ma->getChildWithTagNameIterator ("Point"))
                lane.points.push_back ({ pe->getDoubleAttribute ("beat"), (float) pe->getDoubleAttribute ("v") });
            normalizeAutoPoints (lane.points);
        }
    }

    loopOn = x.getBoolAttribute ("loopOn");
    loopStart = x.getDoubleAttribute ("loopStart", 0.0);
    loopEnd = x.getDoubleAttribute ("loopEnd", 8.0);
    metronome = x.getBoolAttribute ("metronome");
    countInBars = x.getIntAttribute ("countIn");
    recordQuantize = jmax (0.0, x.getDoubleAttribute ("recQuantize", 0.0));
    takeRecording = x.getBoolAttribute ("takeRecording", true);
    idCounter = jmax (idCounter, x.getIntAttribute ("idCounter"));

    //== MIDI learn mappings
    midiMappings.clear();
    if (auto* learn = x.getChildByName ("MidiLearn"))
        for (auto* e : learn->getChildWithTagNameIterator ("Map"))
        {
            MidiMapping m;
            m.cc = e->getIntAttribute ("cc", -1);
            m.channel = jlimit (0, 16, e->getIntAttribute ("ch", 0));
            m.target.kind = (MidiTargetKind) e->getIntAttribute ("kind", 0);
            m.target.trackId = e->getIntAttribute ("track", 0);
            m.target.uid = e->getStringAttribute ("uid", "0").getLargeIntValue();
            m.target.paramIndex = e->getIntAttribute ("param", -1);
            m.isNote = e->getBoolAttribute ("note", false);
            m.label = e->getStringAttribute ("label");
            if (m.cc >= 0 && m.target.kind != MidiTargetKind::None) midiMappings.push_back (m);
        }

    //== project info
    meta = {};
    if (auto* info = x.getChildByName ("Info"))
    {
        meta.title = info->getStringAttribute ("title");
        meta.author = info->getStringAttribute ("author");
        meta.genre = info->getStringAttribute ("genre");
        meta.album = info->getStringAttribute ("album");
        meta.year = info->getStringAttribute ("year");
        meta.comment = info->getStringAttribute ("comment");
        if (auto* cover = info->getChildByName ("Cover"))
        {
            MemoryBlock decoded;
            if (decoded.fromBase64Encoding (cover->getAllSubText().trim()))
            {
                meta.coverData = decoded;
                meta.cover = ImageFileFormat::loadFrom (meta.coverData.getData(), meta.coverData.getSize());
            }
        }
    }

    for (auto* e : x.getChildWithTagNameIterator ("Track"))
    {
        auto t = std::make_unique<Track>();
        t->id = e->getIntAttribute ("id");
        idCounter = jmax (idCounter, t->id);
        t->name = e->getStringAttribute ("name");
        t->colour = Colour::fromString (e->getStringAttribute ("colour", "ff2f7cf6"));
        t->type = (TrackType) e->getIntAttribute ("type");
        t->sourceTrack = e->getIntAttribute ("srcTrack", 0);
        t->sourceChannel = jmax (0, e->getIntAttribute ("srcCh", 0));
        t->showOutputChannels = ! e->getBoolAttribute ("hideOutputs", false);
        t->volumeDb = (float) e->getDoubleAttribute ("vol");
        t->pan = (float) e->getDoubleAttribute ("pan");
        t->mute = e->getBoolAttribute ("mute");
        t->phaseInvert = e->getBoolAttribute ("phase");
        t->isListenBus = e->getBoolAttribute ("listen");
        t->listenEnabled = e->getBoolAttribute ("listenOn", true);
        t->solo = e->getBoolAttribute ("solo");
        t->listenSolo = e->getBoolAttribute ("listenSolo");
        t->arm = e->getBoolAttribute ("arm");
        t->monitor = e->getBoolAttribute ("monitor");
        t->hidden = e->getBoolAttribute ("hidden");
        t->delayMs = e->getDoubleAttribute ("delayMs");
        t->frozen = e->getBoolAttribute ("frozen");
        t->frozenNoFx = e->getBoolAttribute ("frozenNoFx");
        t->frozenStartBeat = e->getDoubleAttribute ("frozenStart");
        t->frozenBakedLatency = jmax (0, e->getIntAttribute ("frozenLatency", 0));
        if (t->frozen)
        {
            const String stored = e->getStringAttribute ("frozenFile");
            File af;
            if (stored.isNotEmpty())
            {
                if (File::isAbsolutePath (stored)) af = File (stored);
                else if (file.getFullPathName().isNotEmpty()) af = file.getParentDirectory().getChildFile (stored);
                else af = File (stored);
            }
            t->frozenFile = af;
            if (formats != nullptr && af.existsAsFile()) t->frozenData = pool.load (af, *formats);
        }
        t->midiChannel = e->getIntAttribute ("ch", 1);
        t->inputChannel = e->getIntAttribute ("in", 0);
        t->inputMono = e->getBoolAttribute ("inMono", true);
        t->autoMode = (AutoMode) e->getIntAttribute ("autoMode", 1);
        t->showAutomation = e->getBoolAttribute ("showAuto");
        t->activeLane = e->getStringAttribute ("activeLane", "vol");
        t->height = e->getIntAttribute ("height", 0);
        t->outputBusId = e->getIntAttribute ("outBus", 0);
        t->isFolder = e->getBoolAttribute ("folder");
        t->folderOpen = e->getBoolAttribute ("folderOpen", true);
        t->folderParent = e->getIntAttribute ("folderParent", 0);
        if (t->isAutomation())
        {
            t->autoTargetTrack = e->getIntAttribute ("autoTrack", 0);
            t->autoTarget = e->getStringAttribute ("autoTarget", "vol");
            t->autoLabel = e->getStringAttribute ("autoLabel");
        }
        for (auto* se : e->getChildWithTagNameIterator ("Send"))
        {
            SendSlot s;
            s.target = se->getIntAttribute ("target");
            s.levelDb = (float) se->getDoubleAttribute ("level", -6.0);
            s.preFader = se->getBoolAttribute ("pre");
            t->sends.push_back (s);
        }

        // every "Instrument" child is one layer; the first one is the track's primary instrument
        for (auto* i : e->getChildWithTagNameIterator ("Instrument"))
        {
            auto slot = slotFromXml (*i);
            if (! t->hasInstrument) { t->hasInstrument = true; t->instrument = slot; }
            else t->extraInstruments.push_back (slot);
        }
        for (auto* i : e->getChildWithTagNameIterator ("Insert")) t->inserts.push_back (slotFromXml (*i));
        for (auto* i : e->getChildWithTagNameIterator ("FrozenInsert")) t->frozenChain.push_back (slotFromXml (*i));

        for (auto* ce : e->getChildWithTagNameIterator ("MidiClip"))
        {
            MidiClip c;
            c.id = ce->getIntAttribute ("id");
            idCounter = jmax (idCounter, c.id);
            c.name = ce->getStringAttribute ("name");
            c.start = ce->getDoubleAttribute ("start");
            c.length = ce->getDoubleAttribute ("length", 4.0);
            c.muted = ce->getBoolAttribute ("muted");
            c.takeGroup = ce->getStringAttribute ("takeGroup").getLargeIntValue();
            c.takeIndex = ce->getIntAttribute ("takeIndex", 0);
            c.takeActive = ce->getBoolAttribute ("takeActive", true);
            for (auto* ne : ce->getChildWithTagNameIterator ("N"))
            {
                Note n;
                n.id = ne->getIntAttribute ("i");
                idCounter = jmax (idCounter, n.id);
                n.pitch = ne->getIntAttribute ("p");
                n.start = ne->getDoubleAttribute ("s");
                n.length = ne->getDoubleAttribute ("l");
                n.velocity = ne->getIntAttribute ("v", 100);
                n.slide = ne->getBoolAttribute ("slide", false);
                c.notes.push_back (n);
            }
            for (auto* ke : ce->getChildWithTagNameIterator ("C"))
            {
                CCPoint p;
                p.id = ke->getIntAttribute ("i");
                idCounter = jmax (idCounter, p.id);
                p.cc = ke->getIntAttribute ("c");
                p.beat = ke->getDoubleAttribute ("b");
                p.value = ke->getIntAttribute ("v");
                c.ccs.push_back (p);
            }
            t->midiClips.push_back (std::move (c));
        }
        for (auto* ce : e->getChildWithTagNameIterator ("AudioClip"))
        {
            AudioClip c;
            c.id = ce->getIntAttribute ("id");
            idCounter = jmax (idCounter, c.id);
            c.name = ce->getStringAttribute ("name");
            {
                // audio paths are stored relative to the project folder when possible
                const String stored = ce->getStringAttribute ("file");
                File af;
                if (stored.isNotEmpty())
                {
                    if (File::isAbsolutePath (stored)) af = File (stored);
                    else if (file.getFullPathName().isNotEmpty()) af = file.getParentDirectory().getChildFile (stored);
                    else af = File (stored);
                }
                c.file = af;
            }
            c.start = ce->getDoubleAttribute ("start");
            c.lengthSec = ce->getDoubleAttribute ("len", 1.0);
            c.offsetSec = ce->getDoubleAttribute ("off");
            c.gainDb = (float) ce->getDoubleAttribute ("gain");
            c.muted = ce->getBoolAttribute ("muted");
            c.fadeIn = ce->getDoubleAttribute ("fadeIn");
            c.fadeOut = ce->getDoubleAttribute ("fadeOut");
            c.fadeInCurve = jlimit (0, 4, ce->getIntAttribute ("fadeInCurve", 1));
            c.fadeOutCurve = jlimit (0, 4, ce->getIntAttribute ("fadeOutCurve", 1));
            c.gainLine = ce->getBoolAttribute ("gainLine");
            c.xfadeCurve = jlimit (0, 2, ce->getIntAttribute ("xfadeCurve", 0));
            c.reversed = ce->getBoolAttribute ("reverse");
            c.timebase = ce->getIntAttribute ("timebase", 0) != 0 ? Timebase::Seconds : Timebase::Bars;
            c.stretch = jlimit (0.1, 10.0, ce->getDoubleAttribute ("stretch", 1.0));
            c.pitchSemitones = (float) jlimit (-24.0, 24.0, ce->getDoubleAttribute ("pitch", 0.0));
            c.takeGroup = ce->getStringAttribute ("takeGroup").getLargeIntValue();
            c.takeIndex = ce->getIntAttribute ("takeIndex", 0);
            c.takeActive = ce->getBoolAttribute ("takeActive", true);
            for (auto* ge : ce->getChildWithTagNameIterator ("G"))
                c.gainEnv.push_back ({ ge->getDoubleAttribute ("b"), (float) ge->getDoubleAttribute ("db") });
            std::sort (c.gainEnv.begin(), c.gainEnv.end(),
                       [] (const ClipGainPoint& a, const ClipGainPoint& b) { return a.beat < b.beat; });
            if (formats != nullptr && c.file.existsAsFile())
                c.data = pool.load (c.file, *formats);
            t->audioClips.push_back (std::move (c));
        }
        lanesFromXml (*e, t->lanes);
        // migrate projects written before lanes had their own visibility flag
        if (t->visibleLaneCount() == 0 && t->showAutomation)
            if (auto* l = t->findLane (t->activeLane)) l->visible = true;
        t->showAutomation = t->hasVisibleLanes();
        if (t->isAutomation())
        {
            // a hand-edited project may carry no lane: create the one the row edits
            if (t->lanes.empty())
                t->ensureLane (t->autoTarget.isNotEmpty() ? t->autoTarget : String ("vol"), t->autoLabel);
            t->lanes.front().visible = true;
            if (t->autoTarget.isEmpty()) t->autoTarget = t->lanes.front().target;
            if (t->autoTargetTrack != 0 && findTrack (t->autoTargetTrack) == nullptr)
                t->autoTargetTrack = 0;
        }
        tracks.push_back (std::move (t));
    }
    refreshFolders();
}

bool Project::saveTo (const File& f)
{
    const File previous = file;
    file = f;               // set first: audio paths are written relative to the target folder

    // a freeze render of a track that is no longer frozen is only kept so that undo can still
    // play it. Saving commits the project, so those caches can go now (a frozen track keeps its)
    for (auto& t : tracks)
        if (t != nullptr && ! t->frozen && t->frozenFile != File())
        {
            if (t->frozenFile.existsAsFile()) t->frozenFile.deleteFile();
            t->frozenFile = File();
            t->frozenData.reset();
        }

    auto xml = toXml (true);
    TemporaryFile tmp (f);
    if (! xml->writeTo (tmp.getFile()) || ! tmp.overwriteTargetFileWithTemporary())
    {
        file = previous;
        return false;
    }
    dirty = false;
    return true;
}

bool Project::saveAsTemplate (const File& templateFile)
{
    const File previous = file;
    templateFile.getParentDirectory().createDirectory();

    // a template is a project: same format, with the plug-in states so the instruments come back
    // with their sound. The current project keeps its own path and title.
    auto xml = toXml (true);
    TemporaryFile tmp (templateFile);
    const bool ok = xml->writeTo (tmp.getFile()) && tmp.overwriteTargetFileWithTemporary();
    file = previous;
    return ok;
}

bool Project::loadFromTemplate (const File& templateFile)
{
    const bool dirtyBefore = dirty;

    if (! loadFrom (templateFile))
        return false;

    // from here on this is a new, unsaved project - not the template itself
    file = File();
    clearUndo();
    dirty = false;
    juce::ignoreUnused (dirtyBefore);
    return true;
}

bool Project::loadFrom (const File& f)
{    auto xml = parseXML (f);
    if (xml == nullptr || xml->getTagName() != "SXProject")
        return false;
    undoStack.clear();
    redoStack.clear();
    undoBatchDepth = 0;
    undoBatchCheckpointed = false;
    file = f;                 // set first: relative audio paths are resolved against this folder
    fromXml (*xml);
    dirty = false;
    sendStructureChanged();
    sendContentChanged();
    sendMixChanged();
    dirty = false;
    return true;
}

//==============================================================================
//==============================================================================
std::vector<TempoSegment> readMidiTempoMap (const File& f, double fallbackBpm, int fallbackNum, int fallbackDen)
{
    std::vector<TempoSegment> map;
    FileInputStream in (f);
    MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in)) return map;

    const int tf = mf.getTimeFormat();
    if (tf <= 0) return map;                       // SMPTE time code: no musical tempo events
    const double toBeats = 1.0 / (double) tf;

    TempoSegment running { 0.0, fallbackBpm, fallbackNum, fallbackDen, false };
    for (int i = 0; i < mf.getNumTracks(); ++i)
    {
        if (mf.getTrack (i) == nullptr) continue;
        for (auto* ev : *mf.getTrack (i))
        {
            auto& m = ev->message;
            const bool isTempo = m.isTempoMetaEvent() && m.getTempoSecondsPerQuarterNote() > 0.0;
            if (! isTempo && ! m.isTimeSignatureMetaEvent()) continue;

            TempoSegment s = running;              // a meter change keeps the tempo, and vice versa
            s.beat = jmax (0.0, m.getTimeStamp() * toBeats);
            s.ramp = false;
            if (isTempo)
                s.bpm = jlimit (20.0, 400.0, 60.0 / m.getTempoSecondsPerQuarterNote());
            if (m.isTimeSignatureMetaEvent())
            {
                int n = 4, d = 4;
                m.getTimeSignatureInfo (n, d);
                if (n > 0 && d > 0) { s.tsNum = n; s.tsDen = d; }
            }
            if (! map.empty() && std::abs (map.back().beat - s.beat) < 1.0e-9) map.back() = s;
            else map.push_back (s);
            running = s;
        }
    }
    if (map.size() > 1) tempomap::normalize (map);
    return map;
}

bool Project::importMidi (const File& f, double atBeat, int& tracksAdded, bool syncTempo)
{
    tracksAdded = 0;
    FileInputStream in (f);
    MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in))
        return false;

    const int tf = mf.getTimeFormat();
    double toBeats;
    if (tf > 0)
        toBeats = 1.0 / (double) tf;
    else
    {
        mf.convertTimestampTicksToSeconds();
        toBeats = bpm / 60.0;
    }

    // the file's conductor track becomes the project's tempo / meter map when asked to
    if (syncTempo)
    {
        auto map = readMidiTempoMap (f, bpm, tsNum, tsDen);
        if (! map.empty())
        {
            if (map.front().beat > 1.0e-9)
            {
                // the file's first change is not at the very start: the map still has to cover bar 1
                TempoSegment head = map.front();
                head.beat = 0.0;
                map.insert (map.begin(), head);
            }
            const TempoEdit keepSeconds (*this);   // seconds-timebase clips stay at their time
            tempo = std::move (map);
            tempomap::normalize (tempo);
            syncLegacyTempo();
        }
    }

    const double bpb = beatsPerBar();

    for (int ti = 0; ti < mf.getNumTracks(); ++ti)
    {
        MidiMessageSequence seq (*mf.getTrack (ti));
        seq.updateMatchedPairs();

        String trackName;
        struct Chan { std::vector<Note> notes; std::vector<CCPoint> ccs; };
        std::map<int, Chan> chans;

        for (auto* ev : seq)
        {
            auto& m = ev->message;
            const double beat = m.getTimeStamp() * toBeats;
            if (m.isTrackNameEvent() && trackName.isEmpty()) { trackName = m.getTextFromTextMetaEvent(); continue; }

            if (m.isNoteOn())
            {
                Note n;
                n.id = newId();
                n.pitch = m.getNoteNumber();
                n.start = beat;
                n.velocity = m.getVelocity();
                double end = ev->noteOffObject != nullptr ? ev->noteOffObject->message.getTimeStamp() * toBeats : beat + 0.25;
                n.length = jmax (1.0 / 128.0, end - beat);
                chans[m.getChannel()].notes.push_back (n);
            }
            else if (m.isController())
            {
                CCPoint p;
                p.id = newId(); p.cc = m.getControllerNumber(); p.beat = beat; p.value = m.getControllerValue();
                chans[m.getChannel()].ccs.push_back (p);
            }
            else if (m.isPitchWheel())
            {
                CCPoint p;
                p.id = newId(); p.cc = kCCPitchBend; p.beat = beat; p.value = m.getPitchWheelValue();
                chans[m.getChannel()].ccs.push_back (p);
            }
            else if (m.isChannelPressure())
            {
                CCPoint p;
                p.id = newId(); p.cc = kCCAftertouch; p.beat = beat; p.value = m.getChannelPressureValue();
                chans[m.getChannel()].ccs.push_back (p);
            }
        }

        for (auto& [ch, data] : chans)
        {
            if (data.notes.empty() && data.ccs.empty()) continue;

            double first = 1.0e12, last = 0.0;
            for (auto& n : data.notes) { first = jmin (first, n.start); last = jmax (last, n.start + n.length); }
            for (auto& c : data.ccs) { first = jmin (first, c.beat); last = jmax (last, c.beat); }

            const double clipStart = std::floor (first / bpb) * bpb;
            const double clipEnd = jmax (clipStart + bpb, std::ceil (last / bpb - 1.0e-6) * bpb);

            Track& t = addTrack (TrackType::Instrument, trackName.isNotEmpty() ? trackName : "Ch " + String (ch));
            if (chans.size() > 1 && trackName.isNotEmpty()) t.name = trackName + " Ch" + String (ch);
            t.midiChannel = ch;

            MidiClip c;
            c.id = newId();
            c.name = t.name;
            c.start = atBeat + clipStart;
            c.length = clipEnd - clipStart;
            for (auto n : data.notes) { n.start -= clipStart; c.notes.push_back (n); }
            for (auto p : data.ccs) { p.beat -= clipStart; c.ccs.push_back (p); }
            t.midiClips.push_back (std::move (c));
            ++tracksAdded;
        }
    }

    return tracksAdded > 0;
}

bool Project::exportMidi (const File& f, const std::vector<int>* only) const
{
    constexpr int ppq = 960;
    MidiFile mf;
    mf.setTicksPerQuarterNote (ppq);

    MidiMessageSequence conductor;
    conductor.addEvent (MidiMessage::textMetaEvent (3, "Tempo"), 0.0);
    auto tmap = tempo;
    if (tmap.empty()) tmap = { TempoSegment { 0.0, bpm, tsNum, tsDen } };
    int wroteNum = 0, wroteDen = 0;
    for (size_t i = 0; i < tmap.size(); ++i)
    {
        const auto& s = tmap[i];
        const auto emit = [&] (double atBeat, double bpmv)
        {
            conductor.addEvent (MidiMessage::tempoMetaEvent ((int) std::round (60000000.0 / bpmv)), std::round (atBeat * ppq));
        };
        // the meter is written where it changes (the first one always)
        if (s.tsNum != wroteNum || s.tsDen != wroteDen)
        {
            conductor.addEvent (MidiMessage::timeSignatureMetaEvent (s.tsNum, s.tsDen), std::round (s.beat * ppq));
            wroteNum = s.tsNum;
            wroteDen = s.tsDen;
        }
        if (! s.ramp || i + 1 >= tmap.size())
        {
            emit (s.beat, s.bpm);
            continue;
        }
        // MIDI has no ramps: write one tempo per beat whose duration equals the true
        // duration of that beat under the curve, so every beat line stays exactly in time
        const double next = tmap[i + 1].beat;
        for (double b = s.beat; b < next - 1.0e-9;)
        {
            const double e = jmin (std::floor (b + 1.0e-9) + 1.0, next);
            const double secs = tempomap::beatsToSeconds (tmap, e) - tempomap::beatsToSeconds (tmap, b);
            emit (b, secs > 1.0e-12 ? 60.0 * (e - b) / secs : tempomap::bpmAt (tmap, b));
            b = e;
        }
    }
    // the file has to be long enough to hold every tempo change, even one placed after the last
    // note (the end-of-track event would otherwise cut it off)
    double endBeatAll = jmax (endBeat(), 4.0);
    for (auto& s : tmap) endBeatAll = jmax (endBeatAll, s.beat + 1.0);
    double endTicks = endBeatAll * ppq;
    conductor.addEvent (MidiMessage::endOfTrack().withTimeStamp (endTicks), endTicks);
    mf.addTrack (conductor);

    for (auto& tp : tracks)
    {
        if (tp->type != TrackType::Instrument || tp->midiClips.empty()) continue;
        if (only != nullptr && std::find (only->begin(), only->end(), tp->id) == only->end()) continue;

        MidiMessageSequence seq;
        seq.addEvent (MidiMessage::textMetaEvent (3, tp->name), 0.0);
        const int ch = jlimit (1, 16, tp->midiChannel);
        double lastTick = 0.0;

        for (auto& c : tp->midiClips)
        {
            for (auto& n : c.notes)
            {
                if (n.start >= c.length) continue;
                const double on = std::round ((c.start + n.start) * ppq);
                const double off = std::round ((c.start + jmin (n.start + n.length, c.length)) * ppq);
                seq.addEvent (MidiMessage::noteOn (ch, n.pitch, (uint8) jlimit (1, 127, n.velocity)), on);
                seq.addEvent (MidiMessage::noteOff (ch, n.pitch, (uint8) 0), jmax (on + 1.0, off));
                lastTick = jmax (lastTick, off);
            }
            // controllers go through the same helper as the engine: pitch bend is written with its
            // centre value wherever the clip has no anchors, so the file sounds like the editor
            std::vector<std::pair<double, MidiMessage>> ccEvents;
            appendControllerMessages (c, ch, ccEvents);
            for (auto& e : ccEvents)
            {
                const double t = std::round ((c.start + e.first) * ppq);
                seq.addEvent (e.second, t);
                lastTick = jmax (lastTick, t);
            }
        }
        seq.sort();
        seq.updateMatchedPairs();
        seq.addEvent (MidiMessage::endOfTrack(), lastTick + 1.0);
        mf.addTrack (seq);
    }

    FileOutputStream out (f);
    if (! out.openedOk()) return false;
    out.setPosition (0);
    out.truncate();
    return mf.writeTo (out, 1);
}

//==============================================================================
// chord detection helpers
void appendClipNotes (const MidiClip& clip, std::vector<chord::TimedNote>& out)
{
    out.reserve (out.size() + clip.notes.size());
    for (auto& n : clip.notes)
        if (n.length > 1.0e-9)
            out.push_back ({ n.pitch, clip.start + n.start, n.length });
}

std::vector<chord::TimedNote> collectTimedNotes (const Project& p, int onlyTrackId)
{
    std::vector<chord::TimedNote> out;
    for (auto& t : p.tracks)
    {
        if (t == nullptr || (onlyTrackId != 0 && t->id != onlyTrackId)) continue;
        for (auto& c : t->midiClips) appendClipNotes (c, out);
    }
    return out;
}

chord::Detection detectChordAt (const Project& p, double beat, double windowBeats, int onlyTrackId)
{
    const auto notes = collectTimedNotes (p, onlyTrackId);
    return chord::detectInWindow (notes, beat, juce::jmax (1.0e-3, windowBeats));
}

//==============================================================================
String barsBeatsString (double beats, const Project& p)
{    const auto pos = tempomap::barPosition (p.tempo, beats);
    const double beatLen = 4.0 / (double) jmax (1, pos.den);   // one beat = one denominator note
    double rem = pos.beatInBar;
    const bool neg = pos.bar < 0;
    const int bar = std::abs (pos.bar);
    const int beat = (int) std::floor (rem / beatLen);
    rem -= beat * beatLen;
    const int div = (int) std::floor (rem / 0.25);
    rem -= div * 0.25;
    const int ticks = (int) std::floor (rem * 960.0);
    return String (neg ? "-" : "") + String (bar + (neg ? 0 : 1)).paddedLeft ('0', 3) + " " + String (beat + 1)
           + " " + String (div + 1) + " " + String (ticks).paddedLeft ('0', 3);
}
}