#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "../model/Model.h"

#include <array>

namespace sx
{
//==============================================================================
/** The built-in effect family. Every one of them is a normal juce::AudioProcessor, so it goes
    through the same insert / automation / state code as a hosted VST - only the creation and the
    editor differ. */
enum class BuiltInEffect
{
    Eq, Compressor, Convolver, AnalogReverb, DigitalReverb, Limiter,
    Stereo, Chorus, Delay, Distortion, Tremolo, Flanger, Phaser,
    OptoCompressor, FetCompressor,   // valve opto ("2A") and FET ("76") style compressors
    Meter,                           // legacy combined meter (kept so old projects still load)
    LoudnessMeter,                   // level / RMS / LUFS meter (no sound change)
    StereoField,                     // goniometer / vectorscope / width (no sound change)
    Doubler,                         // four-voice micro-delay doubler
    GuitarRig,                       // pedalboard, amp heads, cabinets and time block (GuitarRig.h)
    Tape,                            // tape machine: saturation, wow and flutter, head bump, hiss
    Exciter,                         // harmonic exciter (high band harmonics)
    Saturator,                       // saturation / distortion with four curves
    MultibandComp,                   // OTT style up + down multiband compression
    ChorusEnsemble,                  // three voice chorus
    Maximizer,                       // look-ahead maximiser / limiter
    Tuner,                           // chromatic tuner (passes audio through)
    Clipper,                         // oversampled clipper with three shapes
    SslChannel,                      // SSL-style console channel: HPF, shelves, compressor
    DeEsser,                         // sibilance compressor
    Gate,                            // gate / downward expander
    Denoiser,                        // NS1-style adaptive noise suppressor
    Reverse,                         // reverse-grain audio effect
    PitchCorrector,                  // monophonic auto-tune / pitch correction
};

struct BuiltInEffectInfo
{
    BuiltInEffect id;
    const char* name;
    const char* description;
    const char* identifier;   // PluginDescription::fileOrIdentifier
    int uniqueId;
};

const std::vector<BuiltInEffectInfo>& builtInEffects();
bool isBuiltInEffect (const juce::PluginDescription& d);
BuiltInEffect builtInEffectKind (const juce::PluginDescription& d);
std::unique_ptr<juce::AudioProcessor> createBuiltInEffect (BuiltInEffect id);
PluginSlot makeBuiltInEffectSlot (BuiltInEffect id);

//==============================================================================
/** Common base: stereo in / out, a plain parameter list, XML state, and a generic editor. */
/** What the host offers the editor: enable / bypass toggles and the side-chain source. Empty
    std::functions mean the control is not available (e.g. inside a hosted VST build). */
struct SxEffectActions
{
    std::function<bool()> getEnabled;
    std::function<void (bool)> setEnabled;
    std::function<bool()> getBypass;
    std::function<void (bool)> setBypass;
    std::function<bool()> hasSidechainSource;
    std::function<juce::String()> sidechainName;
    std::function<void (juce::Component&)> showSidechainMenu;
};
class SxEffectBase : public juce::AudioProcessor
{
public:
    struct ParamSpec
    {
        const char* id;
        const char* name;
        float min, max, def;
        const char* suffix = "";
        float skew = 1.0f;                 // 1 = linear, < 1 = more resolution at the low end
    };

    SxEffectBase (const juce::String& name, std::vector<ParamSpec> specs, double tailSeconds = 1.0);

    /** Side-chain input: the host copies the source track's block in here before processBlock. */
    struct SidechainFeed
    {
        static constexpr int kMax = 4096;
        float l[kMax] {}, r[kMax] {};
        int n = 0;
        std::atomic<bool> valid { false };
        std::atomic<int> sourceTrack { 0 };
    };
    std::shared_ptr<SidechainFeed> sidechainFeed = std::make_shared<SidechainFeed>();
    std::shared_ptr<SxEffectActions> actions;   // set by the host that owns the editor
    bool hasSidechain() const { return sidechainFeed != nullptr && sidechainFeed->valid.load() && sidechainFeed->n > 0; }

    /** Presets: a whole state (every parameter) as one small file, shared by all built-ins. */
    void setParamPublic (int i, float v) { setParam (i, v); }
    bool savePresetFile (const juce::File& f);
    bool loadPresetFile (const juce::File& f);

    const juce::String getName() const override { return effectName; }
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
            && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return tail; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int size) override;

    //-- what the editor needs ---------------------------------------------------------------
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    int numParams() const { return (int) specs.size(); }
    const ParamSpec& spec (int i) const { return specs[(size_t) i]; }
    float param (int i) const { return params[(size_t) i]->get(); }
    void setParam (int i, float v)
    {
        if (i >= 0 && i < (int) params.size())
            params[(size_t) i]->setValueNotifyingHost (params[(size_t) i]->convertTo0to1 (v));
    }
    /** Changes whenever a parameter moves or the audio thread pushed new display data, so an editor
        repaints only when something happened (an idle plug-in window costs nothing). */
    std::uint64_t displayStamp() const
    {
        std::uint64_t s = (std::uint64_t) histPos.load() * 0x9E3779B97F4A7C15ull + (std::uint64_t) scopePos.load();
        for (auto* p : params)
        {
            const float v = p->get();
            std::uint32_t bits = 0;
            std::memcpy (&bits, &v, sizeof bits);
            s = (s ^ bits) * 0x100000001B3ull;
        }
        return s;
    }
    /** The name of the track this effect sits on (set by the host; empty when unknown). */
    std::function<juce::String()> trackNameSource;
    juce::String trackName() const { return trackNameSource ? trackNameSource() : juce::String(); }
    /** Gain reduction in dB (compressors / limiters) for the editor meter. */
    virtual float gainReductionDb() const { return 0.0f; }
    /** The response curve of an EQ in dB at a frequency (0 = "not an EQ"). */
    virtual float curveDb (float /*hz*/) const { return 0.0f; }
    virtual bool hasCurve() const { return false; }
    double sampleRate = 44100.0;

    //-- what the big display shows ------------------------------------------------------------
    BuiltInEffect kind = BuiltInEffect::Eq;
    // a rolling history the DSP writes (gain reduction, wet level, LFO value ...) so the editor can
    // draw "what happened in the last second" without touching the audio thread
    static constexpr int kHistSize = 180;
    void pushHistory (float v) { hist[(size_t) (histPos % kHistSize)].store (v); ++histPos; }
    float historyAt (int i) const { return hist[(size_t) i].load(); }
    // logarithmic spectrum of the input (EQ analyser): 96 bins from 20 Hz to 20 kHz
    static constexpr int kSpectrumBins = 512;
    void pushSpectrum (const float* bins, int n);
    float spectrumAt (int i) const { return spectrum[(size_t) i].load(); }
    float lfoPhase() const { return lfoPhaseValue.load(); }
    void setLfoPhase (float p) { lfoPhaseValue.store (p); }
    // stereo pair pushed by the effects that care about width (stereo / reverbs)
    void pushStereo (float l, float r) { scopePairs[(size_t) (scopePos % kScopeSize)] = { l, r }; ++scopePos; }
    std::pair<float, float> stereoAt (int i) const { return scopePairs[(size_t) i]; }
    static constexpr int kScopeSize = 128;

protected:
    juce::String effectName;
    std::array<std::atomic<float>, kHistSize> hist {};
    std::atomic<int> histPos { 0 };
    std::array<std::atomic<float>, kSpectrumBins> spectrum {};
    std::atomic<float> lfoPhaseValue { 0.0f };
    std::array<std::pair<float, float>, kScopeSize> scopePairs {};
    std::atomic<int> scopePos { 0 };
    std::vector<ParamSpec> specs;
    std::vector<juce::AudioParameterFloat*> params;
    double tail = 1.0;
    std::vector<std::unique_ptr<juce::AudioParameterFloat>> owned;
};

//==============================================================================
// One class per effect. They are tiny on purpose: the DSP is the interesting part and the base
// class carries everything the host needs.
/** Eight freely shaped bands. A band is a type (bell / low shelf / high shelf / low cut / high
    cut) switched on or off, so nodes can be added and removed in the editor without changing the
    parameter set of the processor. */
class SxEq : public SxEffectBase
{
public:
    static constexpr int kNumBands = 8;
    // one band: on, type, freq, gain, q, dynamic on, threshold, range, attack, release
    static constexpr int kParamsPerBand = 10;
    enum BandType { Bell = 0, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPass };
    static constexpr int kMaxSections = 8;          // 96 dB/oct is eight biquads
    static constexpr int kAnalyserFft = 4096;       // analyser window in samples
    static constexpr int kAnalyserHop = 1024;       // the analyser refreshes every this many samples

    SxEq();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool hasCurve() const override { return true; }
    float curveDb (float hz) const override;
    /** The curve with the current dynamic reductions applied (what you actually hear now). */
    float curveDbEffective (float hz) const;
    /** The response in dB at n frequencies, of one band (band >= 0) or of the whole EQ (band < 0). */
    void responseDb (int band, const float* hz, int n, float* outDb, bool effective) const;
    /** Presets: all band settings in one XML file, so a preset can be shared like a project. */
    bool savePreset (const juce::File& f) const;
    bool loadPreset (const juce::File& f);
    float bandDynDb (int band) const { return dynReductionDb[band].load(); }

    // parameter index helpers, shared by the DSP and the editor
    static int pOn (int band) { return band * kParamsPerBand; }
    static int pType (int band) { return band * kParamsPerBand + 1; }
    static int pFreq (int band) { return band * kParamsPerBand + 2; }
    static int pGain (int band) { return band * kParamsPerBand + 3; }
    static int pQ (int band) { return band * kParamsPerBand + 4; }
    static int pDynOn (int band) { return band * kParamsPerBand + 5; }
    static int pThr (int band) { return band * kParamsPerBand + 6; }
    static int pRange (int band) { return band * kParamsPerBand + 7; }
    static int pAtk (int band) { return band * kParamsPerBand + 8; }
    static int pRel (int band) { return band * kParamsPerBand + 9; }
    static int pSolo() { return kNumBands * kParamsPerBand; }                    // 0 = off, 1..8 = solo that band
    static int pMode() { return kNumBands * kParamsPerBand + 1; }                // 0 = minimum phase, 1 = linear
    static int pSlope (int band) { return kNumBands * kParamsPerBand + 2 + band; }   // cuts: 0 = 12 dB/oct (Q), 1..5 = 24..96
    static int pAnalyser() { return kNumBands * kParamsPerBand + 10; }           // 0 = pre, 1 = post, 2 = off
    static const char* typeName (int t)
    {
        static const char* n[] { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" };
        return n[juce::jlimit (0, 6, t)];
    }
    static const char* slopeName (int s)
    {
        static const char* n[] { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" };
        return n[juce::jlimit (0, 5, s)];
    }

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float x1[2] {}, x2[2] {}, y1[2] {}, y2[2] {};
        void reset() { x1[0] = x1[1] = x2[0] = x2[1] = y1[0] = y1[1] = y2[0] = y2[1] = 0.0f; }
        inline float process (int ch, float in)
        {
            const float out = b0 * in + b1 * x1[ch] + b2 * x2[ch] - a1 * y1[ch] - a2 * y2[ch];
            x2[ch] = x1[ch]; x1[ch] = in; y2[ch] = y1[ch]; y1[ch] = out;
            return out;
        }
        static Biquad lowShelf (double sr, float hz, float gainDb, float q);
        static Biquad highShelf (double sr, float hz, float gainDb, float q);
        static Biquad peak (double sr, float hz, float gainDb, float q);
        static Biquad highPass (double sr, float hz, float q);
        static Biquad lowPass (double sr, float hz, float q);
        static Biquad notch (double sr, float hz, float q);
        static Biquad bandPass (double sr, float hz, float q);
        static Biquad forBand (double sr, int type, float hz, float gainDb, float q);
    };

    /** A cascade of biquads: one band, or a cut with its slope. */
    struct Chain { Biquad s[kMaxSections]; int n = 0; };
    /** on, type, freq, gain, q, slope: what a band is built from. */
    using BandSettings = std::array<float, 6>;
    using SettingsKey = std::array<BandSettings, kNumBands>;

    static float runChain (Chain& c, int ch, float x)
    {
        for (int s = 0; s < c.n; ++s) x = c.s[s].process (ch, x);
        return x;
    }
    BandSettings readBand (int band) const;
    SettingsKey settingsKey() const;
    static Chain chainFor (double sr, int type, float hz, float gainDb, float q, int slope);
    static Chain chainFromSettings (double sr, const BandSettings& s);
    static double chainDb (const Chain& c, double hz, double sr);
    static void copyCoefs (Biquad& dst, const Biquad& src);
    void rebuild (bool force);
    void refreshCurves (bool effective) const;
    void feedAnalyser (const juce::AudioBuffer<float>& in, int n);
    void analyse();
    void buildLinearIr();

    Chain bands[kNumBands];                  // the filters that process the audio
    Biquad sideChain[kNumBands];             // the same shape, used to follow the band level
    float bandEnv[kNumBands] {};             // dynamic envelope per band (linear)
    bool bandDyn[kNumBands] {}, bandOn[kNumBands] {};
    float dynThr[kNumBands] {}, dynRange[kNumBands] {}, dynAtk[kNumBands] {}, dynRel[kNumBands] {};
    std::atomic<float> dynReductionDb[kNumBands];
    SettingsKey lastKey {};                  // the settings the filters were built from

    // the response caches are only rebuilt when a setting actually moved
    mutable SettingsKey curveKey {}, dynCurveKey {};
    mutable bool curveValid = false, dynCurveValid = false;
    mutable Chain curveChains[kNumBands], dynCurveChains[kNumBands];

    // analyser state (audio thread)
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, fftBuffer, ring;
    int ringPos = 0, hopCount = 0;
    float smoothDb[SxEffectBase::kSpectrumBins] {};

    juce::dsp::Convolution linearConv;
    bool linearPrepared = false, linearReady = false;
    SettingsKey linearKey {};
    int linearWait = 0;
};


class SxCompressor : public SxEffectBase
{
public:
    SxCompressor();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    float envDb = 0.0f, atk = 0.0f, rel = 0.0f;
    std::atomic<float> grDb { 0.0f };
};

class SxConvolver : public SxEffectBase,
                    private juce::Timer
{
public:
    SxConvolver();
    ~SxConvolver() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void timerCallback() override;

private:
    void rebuildIr();
    juce::dsp::Convolution conv { juce::dsp::Convolution::NonUniform { 2048 } };
    juce::AudioBuffer<float> predelay;
    int preWrite = 0, preSize = 0;
    float lastSize = -1.0f, lastDamp = -1.0f;
    int lastPre = -1;
    juce::AudioBuffer<float> dryCopy;
};

class SxAnalogReverb : public SxEffectBase
{
public:
    SxAnalogReverb();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    juce::Reverb reverb;
    juce::AudioBuffer<float> predelay, work;
    int preWrite = 0, preSize = 0;
};

class SxDigitalReverb : public SxEffectBase
{
public:
    SxDigitalReverb();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    static constexpr int kLines = 4;
    std::vector<float> lines[kLines];
    int sizes[kLines] {}, write[kLines] {};
    float damp[2][kLines] {};
    juce::AudioBuffer<float> predelay, work;
    int preWrite = 0, preSize = 0;
};

class SxLimiter : public SxEffectBase
{
public:
    SxLimiter();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    juce::AudioBuffer<float> look;
    int lookSize = 0, write = 0;
    float envDb = 0.0f;
    std::atomic<float> grDb { 0.0f };
    int appliedLookahead = -1;
};

/** SX Stereo: places the track in a virtual room. Azimuth and distance put the source where it is dragged
    (level, air and the room's reverb follow the distance); width and bass mono work as before. A link group
    ties tracks together: a linked drag moves the whole group, and loud tracks that sit on the same spot push
    each other apart (auto-spacing). At its default settings it passes the signal through unchanged. */
class SxStereo : public SxEffectBase
{
public:
    enum { pWidth = 0, pBass, pOutput, pAzimuth, pDistance, pRoom, pSpacing, pGroup };

    SxStereo();
    ~SxStereo() override;
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    /** The link group, 0 for none. */
    int group() const { return (int) std::round (param (pGroup)); }
    /** Where the track is placed after auto-spacing, in degrees (0 = front, positive = right). */
    float placedAzimuth() const { return placedAz.load(); }
    /** The input level of the track in dBFS, as the room sees it (for the auto-spacing and the display). */
    float levelDb() const { return level.load(); }
    /** The other tracks of this track's link group. */
    std::vector<SxStereo*> groupPeers() const;
    /** Every live SX Stereo, this one included: the tracks of the room. */
    std::vector<SxStereo*> sceneMembers() const;
    /** Moves every other track of the group by the same amount: degrees of azimuth and percent of distance. */
    void shiftGroup (float dAzimuth, float dDistance);

private:
    struct OnePole
    {
        float z[2] {};
        float a = 0.0f;
        float low (int ch, float x) { z[ch] += a * (x - z[ch]); return z[ch]; }
    };
    // a fractional delay line, 256 samples: read(0) is the newest sample
    struct Delay
    {
        std::array<float, 256> buf {};
        int w = 0;
        void write (float x) { buf[(size_t) w] = x; w = (w + 1) & 255; }
        float read (float d) const
        {
            const float p = (float) (w - 1) - d;
            const int i0 = (int) std::floor (p);
            const float f = p - (float) i0;
            return buf[(size_t) (i0 & 255)] * (1.0f - f) + buf[(size_t) ((i0 + 1) & 255)] * f;
        }
    };

    void setParamGesture (int index, float value);

    OnePole bassL, bassR, air;
    Delay delayL, delayR;
    juce::Reverb reverb;
    juce::AudioBuffer<float> wet;
    bool reverbUsed = false;
    double sampleRate = 44100.0;
    float spaceOffset = 0.0f;                              // auto-spacing, smoothed, in degrees
    std::atomic<float> placedAz { 0.0f }, level { -100.0f };
};

class SxChorus : public SxEffectBase
{
public:
    SxChorus();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    std::vector<float> ring[2];
    int size = 0, write = 0;
    float phase = 0.0f;
};

class SxDelay : public SxEffectBase
{
public:
    SxDelay();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    std::vector<float> ring[2];
    int size = 0, write = 0;
    float lp[2] {};
};

class SxDistortion : public SxEffectBase
{
public:
    SxDistortion();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    float toneZ[2] {};
};

class SxTremolo : public SxEffectBase
{
public:
    SxTremolo();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    float phase = 0.0f;
};

class SxFlanger : public SxEffectBase
{
public:
    SxFlanger();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    std::vector<float> ring[2];
    int size = 0, write = 0;
    float phase = 0.0f;
};

class SxOptoCompressor : public SxEffectBase
{
public:
    SxOptoCompressor();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    float envDb = 0.0f, releaseSlow = 0.0f;
    float emphasisZ[2] {};
    std::atomic<float> grDb { 0.0f };
};

class SxFetCompressor : public SxEffectBase
{
public:
    SxFetCompressor();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

    /** The output level in dBFS since the last call; the editor's meter reads it. */
    float takeOutputLevelDb() noexcept { return outDb.exchange (-100.0f); }

private:
    float envDb = 0.0f;
    std::atomic<float> grDb { 0.0f };
    std::atomic<float> outDb { -100.0f };
};

class SxMeterEffect : public SxEffectBase
{
public:
    SxMeterEffect();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return 0.0f; }
    // what the display reads: peak / rms / correlation of the last block
    float peak (int ch) const { return peakVals[(size_t) ch].load(); }
    float rms (int ch) const { return rmsVals[(size_t) ch].load(); }
    float correlation() const { return corr.load(); }
    float balance() const { return bal.load(); }

private:
    std::array<std::atomic<float>, 2> peakVals {}, rmsVals {};
    std::atomic<float> corr { 0.0f }, bal { 0.0f };
};

/** Loudness meter: peak, true peak, RMS and BS.1770 loudness (momentary, short-term and gated
    integrated). The histories hold one value per 100 ms for the editor's graph. No audio change. */
class SxLoudnessMeter : public SxEffectBase
{
public:
    static constexpr int kHistory = 120;          // one value every 100 ms: twelve seconds

    SxLoudnessMeter();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float peak (int ch) const { return peakVals[(size_t) ch].load(); }
    float rms (int ch) const { return rmsVals[(size_t) ch].load(); }
    float momentaryLufs() const { return lufsMomentary.load(); }
    float shortTermLufs() const { return lufsShort.load(); }
    float integratedLufs() const { return lufsIntegrated.load(); }
    float truePeakHoldDb() const { return truePeakHold.load(); }
    /** The histories, oldest first (i = 0 is twelve seconds ago, kHistory - 1 is the newest). */
    float historyLufs (int i) const { return histLufs[(size_t) slot (i)].load(); }
    float historyPeak (int i) const { return histPeak[(size_t) slot (i)].load(); }
    float historyRms (int i) const { return histRms[(size_t) slot (i)].load(); }

private:
    int slot (int i) const { return (loudPos.load() + i) % kHistory; }

    struct Section { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    static constexpr int kHistBins = 800;         // 0.1 LU bins from -70 LUFS up to +10 LUFS
    static constexpr int kChunks = 30;            // 3 s of 100 ms mean squares

    double sampleRate = 44100.0;
    Section shelf[2], highPass[2];                // K-weighting, one pair per channel
    double tp[2][4] {};                           // the last four samples, for the 4x true peak
    double peakCur[2] {}, peakHoldLeft[2] {}, rmsSq[2] {};
    std::array<std::atomic<float>, 2> peakVals {}, rmsVals {};
    std::atomic<float> lufsMomentary { -100.0f }, lufsShort { -100.0f }, lufsIntegrated { -100.0f };
    std::atomic<float> truePeakHold { -100.0f };

    double chunkEnergy = 0.0, chunkPeak = 0.0;
    int chunkSamples = 0, chunkLength = 4410, chunkCount = 0, chunkWrite = 0;
    std::array<double, kChunks> chunkMs {};
    std::array<double, kHistBins> histCount {}, histEnergy {};

    std::array<std::atomic<float>, kHistory> histLufs {}, histPeak {}, histRms {};
    std::atomic<int> loudPos { 0 };
};

/** Stereo field meter: goniometer (with a short persistence), vectorscope, correlation and width
    histories. The scope keeps real L/R sample pairs. No audio change. */
class SxStereoFieldMeter : public SxEffectBase
{
public:
    static constexpr int kScope = 2048;          // sample pairs kept for the goniometer
    static constexpr int kHistory = 120;         // one value every 100 ms: twelve seconds

    SxStereoFieldMeter();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float correlation() const { return corr.load(); }
    float balance() const { return bal.load(); }
    float widthPercent() const { return width.load(); }
    float sideLevel() const { return sideLvl.load(); }
    float midLevel() const { return midLvl.load(); }
    /** Copies the newest kScope pairs into out, oldest first. */
    void copyScope (std::array<std::pair<float, float>, kScope>& out) const;
    float correlationHistory (int i) const { return corrHist[(size_t) slot (i)].load(); }
    float widthHistory (int i) const { return widthHist[(size_t) slot (i)].load(); }

private:
    int slot (int i) const { return (fieldPos.load() + i) % kHistory; }

    double sampleRate = 44100.0;
    double sllS = 0.0, srrS = 0.0, slrS = 0.0, smmS = 0.0, sssS = 0.0;   // smoothed over about 300 ms
    int histSamples = 0;
    std::atomic<float> corr { 0.0f }, bal { 0.0f }, width { 0.0f }, sideLvl { 0.0f }, midLvl { 0.0f };

    std::array<std::pair<float, float>, kScope> scope {};
    int scopeWrite = 0;
    mutable juce::SpinLock scopeLock;

    std::array<std::atomic<float>, kHistory> corrHist {}, widthHist {};
    std::atomic<int> fieldPos { 0 };
};

/** Four-voice doubler: short differing delays and a slow drift, like four takes of the same part. */
class SxDoubler : public SxEffectBase
{
public:
    SxDoubler();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    struct Voice { float delayL = 0.0f, delayR = 0.0f, baseL = 0.0f, baseR = 0.0f, phase = 0.0f, rate = 0.0f; };
    std::array<Voice, 4> voices {};
    std::vector<float> bufL, bufR;
    int writePos = 0;
    float feedbackZ[2] {};
};

/** Tape machine: tape compression, head bump, wow / flutter and hiss. */
class SxTape : public SxEffectBase
{
public:
    SxTape();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    std::vector<float> line[2];
    int writePos = 0;
    float wowPhase = 0.0f, flutterPhase = 0.0f;
    float lpZ[2] {}, bumpZ[2] {}, hissZ[2] {};
    juce::Random rng;
};

/** Harmonic exciter: splits the top band and adds generated harmonics. */
class SxExciter : public SxEffectBase
{
public:
    SxExciter();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    float lowZ[2] {}, midZ[2] {}, toneZ[2] {};
};

/** Saturation / distortion with tape, tube, diode and fold curves. */
class SxSaturator : public SxEffectBase
{
public:
    SxSaturator();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    float lpZ[2] {}, dcZ[2] {};
};

/** OTT style multiband compression: downward above and upward below the threshold, three bands. */
class SxMultibandComp : public SxEffectBase
{
public:
    SxMultibandComp();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    struct BandState { float env = 0.0f; float gr = 1.0f; };
    BandState bands[3][2];
    float lp1[2] {}, lp2[2] {}, lp1b[2] {}, lp2b[2] {};
    std::atomic<float> grDb { 0.0f };
};

/** Three voice chorus with stereo spread and tone control. */
class SxChorusEnsemble : public SxEffectBase
{
public:
    SxChorusEnsemble();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    std::vector<float> line[2];
    int writePos = 0;
    float phase[3] {};
    float toneZ[2] {};
};

/** Look-ahead maximiser: transparent gain reduction up to a ceiling, with character modes. */
class SxMaximizer : public SxEffectBase
{
public:
    SxMaximizer();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }
    float outputLufs() const { return lufs.load(); }

private:
    std::vector<float> look[2];
    int lookPos = 0;
    float releaseCoef = 0.0f, gain = 1.0f;
    std::atomic<float> grDb { 0.0f }, lufs { -100.0f };
};

/** Chromatic tuner: passes audio through and reports the detected pitch. */
class SxTuner : public SxEffectBase
{
public:
    SxTuner();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float detectedHz() const { return hz.load(); }
    int noteIndex() const { return note.load(); }
    float cents() const { return centsOff.load(); }
    bool signalPresent() const { return hasSignal.load(); }

private:
    std::vector<float> history;
    int writePos = 0;
    std::atomic<float> hz { 0.0f }, centsOff { 0.0f };
    std::atomic<int> note { -1 };
    std::atomic<bool> hasSignal { false };
};
/** Oversampled clipper: hard, soft and fold shapes with drive, tone and output. */
class SxClipper : public SxEffectBase
{
public:
    SxClipper();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    int osFactor = 0;
    float lpZ[2] {};
    std::atomic<float> grDb { 0.0f };
};

class SxPhaser : public SxEffectBase
{
public:
    SxPhaser();
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    float phase = 0.0f;
    float apX1[2][8] {}, apY1[2][8] {};
    float fb[2] {};
};

//== console channel / dynamics ----------------------------------------------------------------
/** SSL-style channel strip: HPF, low and high shelves, a soft-knee compressor, makeup and output. */
class SxSslChannel : public SxEffectBase
{
public:
    SxSslChannel();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    float hpZ[2] {}, lowZ[2] {};
    float envDb = 0.0f;
    std::atomic<float> grDb { 0.0f };
};

/** Sibilance compressor: the high band is detected and attenuated, the low band passes. */
class SxDeEsser : public SxEffectBase
{
public:
    SxDeEsser();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    float lowZ[2] {};
    float envDb = 0.0f;
    std::atomic<float> grDb { 0.0f };
};

/** Gate / downward expander with hold and a separate floor. */
class SxGate : public SxEffectBase
{
public:
    SxGate();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    float env = 0.0f, gain = 1.0f;
    int holdSamples = 0;
    bool open = false;
    std::atomic<float> grDb { 0.0f };
};

/** NS1-style adaptive noise suppressor: tracks the noise floor and pulls low-level material down. */
class SxDenoiser : public SxEffectBase
{
public:
    SxDenoiser();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return grDb.load(); }

private:
    float env = 0.0f, floor = 0.0f, gain = 1.0f;
    std::atomic<float> grDb { 0.0f };
};

/** Reverse-grain effect: records the input and plays short grains backwards, with feedback. */
class SxReverse : public SxEffectBase
{
public:
    SxReverse();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

private:
    std::vector<float> ring[2];
    int ringSize = 0, writePos = 0;
    float phase = 0.0f, lp[2] {};
    float grainSamples = 1.0f;
    float lastWet[2] {};
};

/** Monophonic pitch correction: detects the sung / played pitch, snaps it to a scale and shifts
    the signal with a two-grain time-domain shifter. */
class SxPitchCorrector : public SxEffectBase
{
public:
    SxPitchCorrector();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float detectedHz() const { return detected.load(); }
    float correctionCents() const { return cents.load(); }

private:
    void analyse();
    std::vector<float> ring[2];
    std::vector<float> analysis;
    int ringSize = 0, writePos = 0, analysisWrite = 0, analysisCount = 0;
    float phase = 0.0f;
    double currentRatio = 1.0, targetRatio = 1.0;
    std::atomic<float> detected { 0.0f }, cents { 0.0f };
};
}