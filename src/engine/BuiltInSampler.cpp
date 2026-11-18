#include "BuiltInSampler.h"
#include "../ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace sx
{
using namespace juce;

namespace
{
    /** The longest file the sampler will put in memory: a sampler plays a sound, and a cap keeps a
        wrongly dropped two-hour file from eating all the RAM (10 minutes, or 32M frames at very
        high sample rates). */
    constexpr double kMaxSampleSeconds = 600.0;
    constexpr int64 kMaxSampleFrames = 32 * 1000 * 1000;

    String noteName (int note)
    {
        return MidiMessage::getMidiNoteName (jlimit (0, 127, note), true, true, 3);
    }
}

//==============================================================================
BuiltInSampler::BuiltInSampler()
    : AudioProcessor (BusesProperties().withOutput ("Output", AudioChannelSet::stereo(), true))
{
    formatManager.registerFormat (new WavAudioFormat(), false);
    formatManager.registerFormat (new AiffAudioFormat(), false);
    formatManager.registerFormat (new FlacAudioFormat(), false);
   #if JUCE_USE_MP3AUDIOFORMAT
    formatManager.registerFormat (new MP3AudioFormat(), false);
   #endif

    // the parameters are real host parameters: automation, MIDI learn and the undo capture all
    // read getParameters(), and the state below stores them by id
    addParameter (rootNoteParam = new AudioParameterInt (
        ParameterID { "root", 1 }, TRANS ("Root Note"), 0, 127, 60,
        AudioParameterIntAttributes()
            .withStringFromValueFunction ([] (int v, int)
            {
                return String (v) + " (" + noteName (v) + ")";
            })
            .withLabel ("MIDI")));

    addParameter (gainParam = new AudioParameterFloat (
        ParameterID { "gain", 1 }, TRANS ("Gain"),
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })
            .withLabel ("dB")));

    addParameter (attackParam = new AudioParameterFloat (
        ParameterID { "attack", 1 }, TRANS ("Attack"),
        NormalisableRange<float> (0.5f, 2000.0f, 0.0f, 0.3f), 2.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 2 : 0) + " ms"; })
            .withLabel ("ms")));

    addParameter (releaseParam = new AudioParameterFloat (
        ParameterID { "release", 1 }, TRANS ("Release"),
        NormalisableRange<float> (0.5f, 8000.0f, 0.0f, 0.3f), 150.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 2 : 0) + " ms"; })
            .withLabel ("ms")));

    addParameter (velocityParam = new AudioParameterFloat (
        ParameterID { "velocity", 1 }, TRANS ("Velocity"),
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; })
            .withLabel ("%")));

    addParameter (loopParam = new AudioParameterBool (
        ParameterID { "loop", 1 }, TRANS ("Loop"), false,
        AudioParameterBoolAttributes()
            .withStringFromValueFunction ([] (bool on, int) { return on ? TRANS ("Forward") : TRANS ("Off"); })));

    addParameter (loopStartParam = new AudioParameterFloat (
        ParameterID { "loopStart", 1 }, TRANS ("Loop Start"),
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (v * 100.0f, 1) + " %"; })
            .withLabel ("%")));

    addParameter (loopEndParam = new AudioParameterFloat (
        ParameterID { "loopEnd", 1 }, TRANS ("Loop End"),
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) { return String (v * 100.0f, 1) + " %"; })
            .withLabel ("%")));
}

BuiltInSampler::~BuiltInSampler() = default;

void BuiltInSampler::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    setLatencySamples (0);
}

//==============================================================================
bool BuiltInSampler::loadSample (const File& file, String& error)
{
    error.clear();
    const ScopedLock sl (loadLock);

    if (! file.existsAsFile())
    {
        error = TRANS ("File not found") + ": " + file.getFullPathName();
        return false;
    }

    std::unique_ptr<AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        error = TRANS ("Unsupported audio file") + ": " + file.getFileName();
        return false;
    }

    const auto length = reader->lengthInSamples;
    const int fileChannels = (int) reader->numChannels;
    if (length <= 0 || fileChannels <= 0)
    {
        error = TRANS ("The file contains no audio") + ": " + file.getFileName();
        return false;
    }

    const double fileRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    if ((double) length > fileRate * kMaxSampleSeconds || length > kMaxSampleFrames)
    {
        error = TRANS ("The sample is too long") + ": " + file.getFileName();
        return false;
    }

    auto next = std::make_unique<Sample>();

    // the player is stereo: mono files are duplicated into both rows, files with more than two
    // channels keep their first two
    try
    {
        next->buffer.setSize (2, (int) length, false, true, false);
        next->buffer.clear();
        if (! reader->read (&next->buffer, 0, (int) length, 0, true, true))
        {
            error = TRANS ("Could not read the audio data") + ": " + file.getFileName();
            return false;
        }
    }
    catch (...)
    {
        // a huge file can run the machine out of memory: report it instead of letting the
        // allocation failure escape into the caller
        error = TRANS ("Could not read the audio data") + ": " + file.getFileName();
        return false;
    }

    next->path = file.getFullPathName();
    next->name = file.getFileName();
    next->sourceSampleRate = fileRate;
    next->sourceChannels = fileChannels;
    next->lengthSamples = (int) length;
    next->lengthSeconds = (double) length / fileRate;
    buildPeaks (*next);

    {
        // the audio thread only ever holds this pointer under a try-lock, so swapping (and freeing
        // the old sample) here can never pull the buffer out from under a running block
        const SpinLock::ScopedLockType lock (sampleLock);
        live = std::move (next);
    }
    sampleGeneration.fetch_add (1, std::memory_order_release);
    return true;
}

void BuiltInSampler::clearSample()
{
    {
        const SpinLock::ScopedLockType lock (sampleLock);
        live.reset();
    }
    sampleGeneration.fetch_add (1, std::memory_order_release);
}

void BuiltInSampler::buildPeaks (Sample& s) const
{
    s.peaks.assign ((size_t) kPeakBuckets * 2, 0.0f);
    const int len = s.lengthSamples;
    if (len <= 0) return;

    const int chans = jmax (1, s.buffer.getNumChannels());
    for (int b = 0; b < kPeakBuckets; ++b)
    {
        const int from = (int) ((int64) b * len / kPeakBuckets);
        const int to = (int) ((int64) (b + 1) * len / kPeakBuckets);
        float mn = 0.0f, mx = 0.0f;
        for (int c = 0; c < chans; ++c)
        {
            const auto* d = s.buffer.getReadPointer (c);
            for (int i = from; i < to; ++i)
            {
                mn = jmin (mn, d[i]);
                mx = jmax (mx, d[i]);
            }
        }
        s.peaks[(size_t) b * 2] = mn;
        s.peaks[(size_t) b * 2 + 1] = mx;
    }
}

//==============================================================================
String BuiltInSampler::getSamplePath() const
{
    const SpinLock::ScopedLockType lock (sampleLock);
    return live != nullptr ? live->path : String();
}

String BuiltInSampler::getSampleName() const
{
    const SpinLock::ScopedLockType lock (sampleLock);
    return live != nullptr ? live->name : String();
}

double BuiltInSampler::getSampleLengthSeconds() const
{
    const SpinLock::ScopedLockType lock (sampleLock);
    return live != nullptr ? live->lengthSeconds : 0.0;
}

double BuiltInSampler::getSourceSampleRate() const
{
    const SpinLock::ScopedLockType lock (sampleLock);
    return live != nullptr ? live->sourceSampleRate : 0.0;
}

int BuiltInSampler::getLoadedChannels() const
{
    const SpinLock::ScopedLockType lock (sampleLock);
    return live != nullptr ? live->sourceChannels : 0;
}

int BuiltInSampler::getSampleLengthSamples() const
{
    const SpinLock::ScopedLockType lock (sampleLock);
    return live != nullptr ? live->lengthSamples : 0;
}

double BuiltInSampler::getRootNote() const
{
    return rootNoteParam != nullptr ? (double) rootNoteParam->get() : 60.0;
}

std::vector<float> BuiltInSampler::copyWaveformPeaks() const
{
    const SpinLock::ScopedLockType lock (sampleLock);
    return live != nullptr ? live->peaks : std::vector<float>();
}

//==============================================================================
void BuiltInSampler::handleMidi (const MidiMessage& m, double sr)
{
    if (m.isNoteOn())
        startVoice (m.getNoteNumber(), m.getChannel(), m.getFloatVelocity(), sr);
    else if (m.isNoteOff (true))
        releaseVoice (m.getNoteNumber(), m.getChannel(), sr);
    else if (m.isAllSoundOff())
    {
        stopAllVoices();
        sustainPedal = false;
    }
    else if (m.isAllNotesOff())
    {
        releaseAllVoices (sr);
        sustainPedal = false;
    }
    else if (m.isSustainPedalOn())  sustainPedal = true;
    else if (m.isSustainPedalOff())
    {
        sustainPedal = false;
        for (auto& v : voices)
            if (v.active && v.sustained) beginRelease (v, sr);
    }
    else if (m.isPitchWheel())
        pitchBendSemitones = ((float) m.getPitchWheelValue() - 8192.0f) / 8192.0f * 2.0f;   // +- 2 semitones
}

void BuiltInSampler::startVoice (int note, int channel, float velocity, double sr)
{
    Voice* target = nullptr;

    // retrigger a voice that is already playing this note instead of stacking a second one
    for (auto& v : voices)
        if (v.active && v.note == note && v.channel == channel && ! v.sustained)
        {
            target = &v;
            break;
        }

    if (target == nullptr)
        for (auto& v : voices)
            if (! v.active)
            {
                target = &v;
                break;
            }

    if (target == nullptr)   // all 16 busy: the oldest voice goes
    {
        juce::uint64 oldest = std::numeric_limits<juce::uint64>::max();
        target = &voices[0];
        for (auto& v : voices)
            if (v.age < oldest)
            {
                oldest = v.age;
                target = &v;
            }
    }

    const float attackMs = attackParam != nullptr ? jmax (0.5f, attackParam->get()) : 2.0f;
    const float attackSamples = jmax (1.0f, attackMs * 0.001f * (float) sr);

    target->active = true;
    target->sustained = false;
    target->note = jlimit (0, 127, note);
    target->channel = channel;
    target->velocity = jlimit (0.0f, 1.0f, velocity);
    target->pos = 0.0;
    target->env = 0.0f;
    target->stage = Voice::Stage::attack;
    target->attackInc = 1.0f / attackSamples;
    target->releaseDec = target->attackInc;
    target->age = ++voiceAge;
}

void BuiltInSampler::beginRelease (Voice& v, double sr)
{
    const float releaseMs = releaseParam != nullptr ? jmax (0.5f, releaseParam->get()) : 150.0f;
    const float releaseSamples = jmax (1.0f, releaseMs * 0.001f * (float) sr);

    v.releaseDec = v.env > 0.0f ? v.env / releaseSamples : 1.0f;
    v.stage = Voice::Stage::release;
    v.sustained = false;
}

void BuiltInSampler::releaseVoice (int note, int channel, double sr)
{
    for (auto& v : voices)
        if (v.active && v.note == note && v.channel == channel
            && ! v.sustained && v.stage != Voice::Stage::release)
        {
            if (sustainPedal) v.sustained = true;
            else beginRelease (v, sr);
        }
}

void BuiltInSampler::releaseAllVoices (double sr)
{
    for (auto& v : voices)
        if (v.active) beginRelease (v, sr);
}

void BuiltInSampler::stopAllVoices()
{
    for (auto& v : voices)
    {
        v.active = false;
        v.env = 0.0f;
        v.sustained = false;
    }
}

//==============================================================================
void BuiltInSampler::renderVoices (AudioBuffer<float>& out, int startSample, int numSamples,
                                   const Sample* s, const BlockParams& p)
{
    if (numSamples <= 0 || out.getNumChannels() <= 0) return;

    auto* outL = out.getWritePointer (0, startSample);
    auto* outR = out.getNumChannels() > 1 ? out.getWritePointer (1, startSample) : nullptr;

    const bool hasSample = s != nullptr && s->lengthSamples > 0 && s->buffer.getNumSamples() > 0;
    const int len = hasSample ? s->buffer.getNumSamples() : 0;
    const float* d0 = hasSample ? s->buffer.getReadPointer (0) : nullptr;
    const float* d1 = (hasSample && s->buffer.getNumChannels() > 1) ? s->buffer.getReadPointer (1) : d0;

    const bool looping = p.loop && hasSample && (p.loopEnd - p.loopStart) >= 1.0;

    for (auto& v : voices)
    {
        if (! v.active) continue;

        const float velGain = 1.0f - p.velSens + p.velSens * v.velocity;
        const double semitones = (double) v.note - p.rootNote + (double) pitchBendSemitones;
        const double inc = std::pow (2.0, semitones / 12.0) * p.fileRatio;
        const float amp = p.gain * velGain;

        for (int i = 0; i < numSamples; ++i)
        {
            if (v.stage == Voice::Stage::attack)
            {
                v.env += v.attackInc;
                if (v.env >= 1.0f)
                {
                    v.env = 1.0f;
                    v.stage = Voice::Stage::sustain;
                }
            }
            else if (v.stage == Voice::Stage::release)
            {
                v.env -= v.releaseDec;
                if (v.env <= 0.0f)
                {
                    v.env = 0.0f;
                    v.active = false;
                    break;
                }
            }

            if (hasSample)
            {
                const int idx = (int) v.pos;
                if (idx >= 0 && idx < len)
                {
                    // linear interpolation between the two neighbouring frames
                    const float frac = (float) (v.pos - (double) idx);
                    const int idx2 = idx + 1 < len ? idx + 1 : idx;
                    const float l0 = d0[idx], l1 = d0[idx2];
                    const float left = l0 + (l1 - l0) * frac;
                    const float g = v.env * amp;
                    outL[i] += left * g;
                    if (outR != nullptr)
                    {
                        const float r0 = d1[idx], r1 = d1[idx2];
                        outR[i] += (r0 + (r1 - r0) * frac) * g;
                    }
                }

                v.pos += inc;
                if (looping)
                {
                    if (v.pos >= p.loopEnd)
                    {
                        const double span = p.loopEnd - p.loopStart;
                        if (span >= 1.0) v.pos = p.loopStart + std::fmod (v.pos - p.loopStart, span);
                        else             v.pos = p.loopStart;
                    }
                }
                else if (v.pos >= (double) len)
                {
                    v.active = false;      // one-shot: the sample ran out
                    break;
                }
            }
        }
    }
}

void BuiltInSampler::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    buffer.clear();
    const int n = buffer.getNumSamples();
    if (n <= 0) return;

    // the host normally sets the rate before prepareToPlay; fall back to what prepareToPlay got
    const double hostRate = getSampleRate();
    const double sr = hostRate > 0.0 ? hostRate : (currentSampleRate > 0.0 ? currentSampleRate : 44100.0);

    BlockParams p;
    p.sr = sr;
    p.rootNote = rootNoteParam != nullptr ? (double) rootNoteParam->get() : 60.0;
    p.gain = Decibels::decibelsToGain (gainParam != nullptr ? gainParam->get() : 0.0f, -60.0f);
    p.velSens = jlimit (0.0f, 1.0f, (velocityParam != nullptr ? velocityParam->get() : 60.0f) * 0.01f);

    // the audio thread never blocks on the sample: if a load is swapping the buffer right now
    // this block is simply silent (the voices still age, so note-offs stay sample accurate)
    const SpinLock::ScopedTryLockType lock (sampleLock);
    const Sample* s = lock.isLocked() ? live.get() : nullptr;

    if (s != nullptr)
    {
        p.fileRatio = s->sourceSampleRate > 0.0 ? s->sourceSampleRate / sr : 1.0;

        if (loopParam != nullptr && loopParam->get() && s->lengthSamples > 1)
        {
            double start = jlimit (0.0, 1.0, (double) (loopStartParam != nullptr ? loopStartParam->get() : 0.0f));
            double end = jlimit (0.0, 1.0, (double) (loopEndParam != nullptr ? loopEndParam->get() : 1.0f));
            if (end < start) std::swap (start, end);

            p.loopStart = std::floor (start * (double) s->lengthSamples);
            p.loopEnd = std::ceil (end * (double) s->lengthSamples);
            p.loopStart = jlimit (0.0, (double) jmax (0, s->lengthSamples - 1), p.loopStart);
            p.loopEnd = jlimit (0.0, (double) s->lengthSamples, p.loopEnd);
            if (p.loopEnd <= p.loopStart) p.loopEnd = (double) s->lengthSamples;
            p.loop = (p.loopEnd - p.loopStart) >= 1.0;
        }
    }

    // render in segments so every note starts on the sample position it was played at
    int rendered = 0;
    for (const auto meta : midi)
    {
        const int offset = jlimit (0, n - 1, meta.samplePosition);
        if (offset > rendered)
        {
            renderVoices (buffer, rendered, offset - rendered, s, p);
            rendered = offset;
        }
        handleMidi (meta.getMessage(), sr);
    }
    if (rendered < n) renderVoices (buffer, rendered, n - rendered, s, p);

    int active = 0;
    for (const auto& v : voices)
        if (v.active) ++active;
    activeVoices.store (active, std::memory_order_relaxed);
}

//==============================================================================
void BuiltInSampler::getStateInformation (MemoryBlock& dest)
{
    XmlElement e ("SXSampler");
    e.setAttribute ("version", 1);
    e.setAttribute ("samplePath", getSamplePath());

    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            e.setAttribute (rp->getParameterID(), (double) rp->getValue());

    copyXmlToBinary (e, dest);
}

void BuiltInSampler::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName ("SXSampler")) return;

    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            if (xml->hasAttribute (rp->getParameterID()))
                rp->setValueNotifyingHost ((float) xml->getDoubleAttribute (rp->getParameterID()));

    // the sample itself is not inside the state: the file is reloaded from the path (a project
    // that moved without its sample stays silent, and nothing crashes)
    const auto path = xml->getStringAttribute ("samplePath");
    if (path.isEmpty())
    {
        clearSample();
        return;
    }

    String error;
    if (! loadSample (File (path), error))
        clearSample();
}

//==============================================================================
namespace
{
    /** The sample display: a min / max envelope of the loaded file, the loop region on top of it,
        and an invitation to drop a file while nothing is loaded. Drawn here rather than through
        ui/Waveform.h, so the instrument is independent of the arrangement's clip drawing. */
    class SamplerWave : public Component
    {
    public:
        explicit SamplerWave (BuiltInSampler& p) : proc (p)
        {
            setInterceptsMouseClicks (false, false);
        }

        void setPeaks (std::vector<float> p)
        {
            peaks = std::move (p);
            repaint();
        }

        void setDragOver (bool on)
        {
            if (dragOver != on)
            {
                dragOver = on;
                repaint();
            }
        }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (peaks.empty() ? theme::panelAlt : theme::white);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (dragOver ? theme::accent : theme::line);
            g.drawRoundedRectangle (r, 5.0f, dragOver ? 2.0f : 1.0f);

            auto inner = r.reduced (7.0f, 6.0f).toNearestInt();
            if (inner.getWidth() < 4 || inner.getHeight() < 4) return;

            if (peaks.empty())
            {
                auto top = inner.removeFromTop (inner.getHeight() / 2 + 6);
                g.setColour (theme::textFaint);
                g.setFont (theme::font (13.0f));
                g.drawText (TRANS ("No sample loaded"), top, Justification::centredBottom);
                g.setFont (theme::font (11.0f));
                g.drawText (TRANS ("Drag an audio file here"), inner, Justification::centredTop);
                return;
            }

            // loop region behind the envelope so the waveform stays readable on top of it
            const bool loopOn = proc.loopParam != nullptr && proc.loopParam->get();
            float ls = proc.loopStartParam != nullptr ? proc.loopStartParam->get() : 0.0f;
            float le = proc.loopEndParam != nullptr ? proc.loopEndParam->get() : 1.0f;
            ls = jlimit (0.0f, 1.0f, ls);
            le = jlimit (0.0f, 1.0f, le);
            if (le < ls) std::swap (ls, le);

            const int lx0 = inner.getX() + roundToInt (ls * (float) inner.getWidth());
            const int lx1 = inner.getX() + roundToInt (le * (float) inner.getWidth());
            if (loopOn && lx1 > lx0)
            {
                g.setColour (theme::accentSoft);
                g.fillRect (lx0, inner.getY(), lx1 - lx0, inner.getHeight());
                g.setColour (theme::accent);
                g.fillRect (lx0, inner.getY(), 1, inner.getHeight());
                g.fillRect (jmax (lx0, lx1 - 1), inner.getY(), 1, inner.getHeight());
            }

            const float mid = (float) inner.getCentreY();
            const float halfH = (float) inner.getHeight() * 0.5f - 2.0f;
            g.setColour (theme::lineSoft);
            g.fillRect (inner.getX(), roundToInt (mid), inner.getWidth(), 1);

            const int buckets = (int) peaks.size() / 2;
            const int w = inner.getWidth();
            g.setColour (theme::accentDark);
            for (int x = 0; x < w; ++x)
            {
                const int b0 = (int) ((int64) x * buckets / w);
                const int b1 = jmax (b0 + 1, (int) ((int64) (x + 1) * buckets / w));
                float mn = 0.0f, mx = 0.0f;
                for (int b = b0; b < b1 && b < buckets; ++b)
                {
                    mn = jmin (mn, peaks[(size_t) b * 2]);
                    mx = jmax (mx, peaks[(size_t) b * 2 + 1]);
                }
                mn = jlimit (-1.0f, 1.0f, mn);
                mx = jlimit (-1.0f, 1.0f, mx);

                const int y0 = roundToInt (mid - mx * halfH);
                const int y1 = roundToInt (mid - mn * halfH);
                g.fillRect (inner.getX() + x, y0, 1, jmax (1, y1 - y0));
            }
        }

    private:
        BuiltInSampler& proc;
        std::vector<float> peaks;
        bool dragOver = false;
    };

    //==========================================================================
    class SamplerEditor : public AudioProcessorEditor,
                          private Timer,
                          private FileDragAndDropTarget
    {
    public:
        explicit SamplerEditor (BuiltInSampler& p)
            : AudioProcessorEditor (&p), proc (p), wave (p)
        {
            addAndMakeVisible (wave);

            auto addKnob = [this] (Slider& s, Label& cap, const String& text)
            {
                s.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
                s.setTextBoxStyle (Slider::TextBoxBelow, false, 66, 15);
                s.setMouseDragSensitivity (140);
                s.setWantsKeyboardFocus (false);
                s.setColour (Slider::textBoxTextColourId, theme::textDim);
                s.setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
                s.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
                addAndMakeVisible (s);

                cap.setText (text, dontSendNotification);
                cap.setJustificationType (Justification::centred);
                cap.setFont (theme::font (10.5f));
                cap.setColour (Label::textColourId, theme::textDim);
                cap.setInterceptsMouseClicks (false, false);
                addAndMakeVisible (cap);
            };

            addKnob (rootKnob,      rootCap,      TRANS ("Root Note"));
            addKnob (gainKnob,      gainCap,      TRANS ("Gain"));
            addKnob (attackKnob,    attackCap,    TRANS ("Attack"));
            addKnob (releaseKnob,   releaseCap,   TRANS ("Release"));
            addKnob (velKnob,       velCap,       TRANS ("Velocity"));
            addKnob (loopStartKnob, loopStartCap, TRANS ("Loop Start"));
            addKnob (loopEndKnob,   loopEndCap,   TRANS ("Loop End"));

            loopBtn.setButtonText (TRANS ("Loop"));
            loopBtn.setColour (ToggleButton::textColourId, theme::text);
            addAndMakeVisible (loopBtn);

            loadBtn.setButtonText (TRANS ("Load Sample..."));
            loadBtn.setTooltip (TRANS ("Load an audio file to play from the keyboard"));
            loadBtn.setColour (TextButton::buttonColourId, theme::white);
            loadBtn.onClick = [this] { chooseSample(); };
            addAndMakeVisible (loadBtn);

            clearBtn.setButtonText (TRANS ("Clear"));
            clearBtn.setTooltip (TRANS ("Remove the sample"));
            clearBtn.setColour (TextButton::buttonColourId, theme::white);
            clearBtn.onClick = [this] { proc.clearSample(); refresh (true); };
            addAndMakeVisible (clearBtn);

            info.setFont (theme::font (11.5f));
            info.setColour (Label::textColourId, theme::textDim);
            info.setJustificationType (Justification::centredLeft);
            info.setMinimumHorizontalScale (0.65f);
            info.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (info);

            // real parameter attachments: the editor follows the host (undo, automation, state
            // restore) without a single manual sync, and gestures reach the undo capture
            if (proc.rootNoteParam != nullptr)
                rootAtt = std::make_unique<SliderParameterAttachment> (*proc.rootNoteParam, rootKnob);
            if (proc.gainParam != nullptr)
                gainAtt = std::make_unique<SliderParameterAttachment> (*proc.gainParam, gainKnob);
            if (proc.attackParam != nullptr)
                attackAtt = std::make_unique<SliderParameterAttachment> (*proc.attackParam, attackKnob);
            if (proc.releaseParam != nullptr)
                releaseAtt = std::make_unique<SliderParameterAttachment> (*proc.releaseParam, releaseKnob);
            if (proc.velocityParam != nullptr)
                velAtt = std::make_unique<SliderParameterAttachment> (*proc.velocityParam, velKnob);
            if (proc.loopStartParam != nullptr)
                loopStartAtt = std::make_unique<SliderParameterAttachment> (*proc.loopStartParam, loopStartKnob);
            if (proc.loopEndParam != nullptr)
                loopEndAtt = std::make_unique<SliderParameterAttachment> (*proc.loopEndParam, loopEndKnob);
            if (proc.loopParam != nullptr)
                loopAtt = std::make_unique<ButtonParameterAttachment> (*proc.loopParam, loopBtn);

            setSize (520, 360);
            startTimerHz (15);
            refresh (true);
        }

        void paint (Graphics& g) override
        {
            g.fillAll (theme::panel);

            auto head = getLocalBounds().removeFromTop (44);
            g.setColour (theme::white);
            g.fillRect (head);
            g.setColour (theme::accent);
            g.fillRect (head.getX(), head.getY(), 4, head.getHeight());

            auto text = head.withTrimmedLeft (16).withTrimmedRight (200);
            g.setColour (theme::text);
            g.setFont (theme::font (16.0f, true));
            g.drawText (proc.getName(), text.removeFromTop (28).withTrimmedTop (6), Justification::centredLeft);
            g.setColour (theme::textFaint);
            g.setFont (theme::font (10.0f));
            g.drawText (TRANS ("SoundX built-in sampler"), text, Justification::centredLeft);

            g.setColour (theme::line);
            g.fillRect (0, head.getBottom(), getWidth(), 1);
        }

        void resized() override
        {
            auto head = getLocalBounds().removeFromTop (44).reduced (12, 10);
            loadBtn.setBounds (head.removeFromRight (120));
            head.removeFromRight (8);
            clearBtn.setBounds (head.removeFromRight (64));

            auto body = getLocalBounds().withTrimmedTop (44).reduced (12, 8);
            wave.setBounds (body.removeFromTop (150));
            body.removeFromTop (6);

            auto infoRow = body.removeFromTop (22);
            loopBtn.setBounds (infoRow.removeFromRight (84).withSizeKeepingCentre (84, 20));
            info.setBounds (infoRow.withTrimmedRight (8));
            body.removeFromTop (6);

            Slider* knobs[] { &rootKnob, &gainKnob, &attackKnob, &releaseKnob, &velKnob, &loopStartKnob, &loopEndKnob };
            Label* caps[] { &rootCap, &gainCap, &attackCap, &releaseCap, &velCap, &loopStartCap, &loopEndCap };
            const int count = (int) std::size (knobs);
            const int colW = body.getWidth() / count;
            for (int i = 0; i < count; ++i)
            {
                auto col = body.removeFromLeft (i == count - 1 ? body.getWidth() : colW);
                caps[i]->setBounds (col.removeFromTop (14));
                knobs[i]->setBounds (col.reduced (2, 0));
            }
        }

        //== FileDragAndDropTarget =============================================
        bool isInterestedInFileDrag (const StringArray& files) override
        {
            for (const auto& name : files)
            {
                const auto ext = File (name).getFileExtension().toLowerCase();
                if (ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".mp3")
                    return true;
            }
            return false;
        }

        void fileDragEnter (const StringArray&, int, int) override { wave.setDragOver (true); }
        void fileDragExit (const StringArray&) override             { wave.setDragOver (false); }

        void filesDropped (const StringArray& files, int, int) override
        {
            wave.setDragOver (false);
            if (! files.isEmpty()) loadFile (File (files[0]));
        }

    private:
        void timerCallback() override { refresh (false); }

        void chooseSample()
        {
            auto start = File::getSpecialLocation (File::userDocumentsDirectory);
            const auto current = proc.getSamplePath();
            if (current.isNotEmpty())
            {
                const auto parent = File (current).getParentDirectory();
                if (parent.isDirectory()) start = parent;
            }

            chooser = std::make_unique<FileChooser> (TRANS ("Load Sample..."), start,
                                                     "*.wav;*.aif;*.aiff;*.flac;*.mp3");
            SafePointer<SamplerEditor> safe (this);
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [safe] (const FileChooser& fc)
            {
                if (auto* self = safe.getComponent())
                {
                    const auto file = fc.getResult();
                    if (file.existsAsFile()) self->loadFile (file);
                }
            });
        }

        void loadFile (const File& file)
        {
            String error;
            if (! proc.loadSample (file, error))
            {
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "SX Sampler", error);
                return;
            }
            refresh (true);
        }

        void updateInfo()
        {
            String text;
            if (proc.getSampleLengthSamples() <= 0)
            {
                text = TRANS ("No sample loaded");
            }
            else
            {
                text << proc.getSampleName()
                     << "   ·   " << String (proc.getSourceSampleRate() / 1000.0, 1) << " kHz"
                     << "   ·   " << String (proc.getLoadedChannels()) << " " << TRANS ("channels")
                     << "   ·   " << String (proc.getSampleLengthSeconds(), 2) << " s";
            }
            text << "   ·   " << String (lastVoices) << " / " << String (BuiltInSampler::kNumVoices)
                 << " " << TRANS ("voices");
            info.setText (text, dontSendNotification);
        }

        void refresh (bool force)
        {
            const int generation = proc.getSampleGeneration();
            const int voices = proc.getActiveVoiceCount();
            bool repaintInfo = false;

            if (force || generation != lastGeneration)
            {
                lastGeneration = generation;
                wave.setPeaks (proc.copyWaveformPeaks());
                clearBtn.setEnabled (proc.getSampleLengthSamples() > 0);
                repaintInfo = true;
            }
            if (force || voices != lastVoices)
            {
                lastVoices = voices;
                repaintInfo = true;
            }
            if (repaintInfo) updateInfo();

            const bool loopOn = proc.loopParam != nullptr && proc.loopParam->get();
            const float ls = proc.loopStartParam != nullptr ? proc.loopStartParam->get() : 0.0f;
            const float le = proc.loopEndParam != nullptr ? proc.loopEndParam->get() : 1.0f;
            if (force || loopOn != lastLoop || ls != lastLoopStart || le != lastLoopEnd)
            {
                lastLoop = loopOn;
                lastLoopStart = ls;
                lastLoopEnd = le;
                wave.repaint();
            }
        }

        BuiltInSampler& proc;
        SamplerWave wave;
        Label info, rootCap, gainCap, attackCap, releaseCap, velCap, loopStartCap, loopEndCap;
        Slider rootKnob, gainKnob, attackKnob, releaseKnob, velKnob, loopStartKnob, loopEndKnob;
        ToggleButton loopBtn;
        TextButton loadBtn, clearBtn;
        std::unique_ptr<SliderParameterAttachment> rootAtt, gainAtt, attackAtt, releaseAtt,
                                                   velAtt, loopStartAtt, loopEndAtt;
        std::unique_ptr<ButtonParameterAttachment> loopAtt;
        std::unique_ptr<FileChooser> chooser;

        int lastGeneration = -1, lastVoices = -1;
        bool lastLoop = false;
        float lastLoopStart = -1.0f, lastLoopEnd = -1.0f;
    };
}

//==============================================================================
AudioProcessorEditor* BuiltInSampler::createEditor()
{
    return new SamplerEditor (*this);
}
}
