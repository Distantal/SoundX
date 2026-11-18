#include "GuitarRig.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace sx
{
using namespace juce;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    inline void setCoefficients (RigBiquad& f, double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        f.b0 = (float) (b0 / a0);
        f.b1 = (float) (b1 / a0);
        f.b2 = (float) (b2 / a0);
        f.a1 = (float) (a1 / a0);
        f.a2 = (float) (a2 / a0);
    }

    inline double safeHz (double sr, double hz) { return jlimit (10.0, sr * 0.45, hz); }

    void highPass (RigBiquad& f, double sr, double hz, double q)
    {
        const double w = 2.0 * kPi * safeHz (sr, hz) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        setCoefficients (f, (1.0 + c) * 0.5, -(1.0 + c), (1.0 + c) * 0.5, 1.0 + al, -2.0 * c, 1.0 - al);
    }

    void lowPass (RigBiquad& f, double sr, double hz, double q)
    {
        const double w = 2.0 * kPi * safeHz (sr, hz) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        setCoefficients (f, (1.0 - c) * 0.5, 1.0 - c, (1.0 - c) * 0.5, 1.0 + al, -2.0 * c, 1.0 - al);
    }

    void peaking (RigBiquad& f, double sr, double hz, double gainDb, double q)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * safeHz (sr, hz) / sr, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        setCoefficients (f, 1.0 + al * A, -2.0 * c, 1.0 - al * A, 1.0 + al / A, -2.0 * c, 1.0 - al / A);
    }

    void lowShelf (RigBiquad& f, double sr, double hz, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * safeHz (sr, hz) / sr, c = std::cos (w);
        const double beta = std::sqrt (A) * std::sin (w) * std::sqrt (2.0);
        setCoefficients (f,
                         A * ((A + 1.0) - (A - 1.0) * c + beta),
                         2.0 * A * ((A - 1.0) - (A + 1.0) * c),
                         A * ((A + 1.0) - (A - 1.0) * c - beta),
                         (A + 1.0) + (A - 1.0) * c + beta,
                         -2.0 * ((A - 1.0) + (A + 1.0) * c),
                         (A + 1.0) + (A - 1.0) * c - beta);
    }

    void highShelf (RigBiquad& f, double sr, double hz, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * safeHz (sr, hz) / sr, c = std::cos (w);
        const double beta = std::sqrt (A) * std::sin (w) * std::sqrt (2.0);
        setCoefficients (f,
                         A * ((A + 1.0) + (A - 1.0) * c + beta),
                         -2.0 * A * ((A - 1.0) + (A + 1.0) * c),
                         A * ((A + 1.0) + (A - 1.0) * c - beta),
                         (A + 1.0) - (A - 1.0) * c + beta,
                         2.0 * ((A - 1.0) - (A + 1.0) * c),
                         (A + 1.0) - (A - 1.0) * c - beta);
    }

    /** Soft clip with unity slope through zero and a ceiling of +-1: quiet material passes unchanged. */
    inline float cubicClip (float x) noexcept
    {
        const float y = jlimit (-1.5f, 1.5f, x);
        return y - y * y * y * (1.0f / 6.75f);
    }

    inline float dB (float db) noexcept { return Decibels::decibelsToGain (db); }
    inline int rounded (float v) noexcept { return (int) std::round (v); }

    std::vector<SxEffectBase::ParamSpec> rigSpecs()
    {
        return {
            // pedalboard, in signal order
            { "compOn",    "Comp On",            0.0f,    1.0f,   0.0f,  "" },
            { "compSus",   "Comp Sustain",       0.0f,    100.0f, 50.0f, " %" },
            { "compAtk",   "Comp Attack",        1.0f,    50.0f,  10.0f, " ms", 0.5f },
            { "compLvl",   "Comp Level",        -12.0f,   12.0f,  0.0f,  " dB" },
            { "boostOn",   "Boost On",           0.0f,    1.0f,   0.0f,  "" },
            { "boostGain", "Boost Gain",         0.0f,    100.0f, 40.0f, " %" },
            { "boostTre",  "Boost Treble",      -6.0f,    6.0f,   0.0f,  " dB" },
            { "boostLvl",  "Boost Level",      -12.0f,   12.0f,  0.0f,  " dB" },
            { "tsOn",      "TS808 On",           0.0f,    1.0f,   0.0f,  "" },
            { "tsDrive",   "TS808 Drive",        0.0f,    100.0f, 50.0f, " %" },
            { "tsTone",    "TS808 Tone",         0.0f,    100.0f, 50.0f, " %" },
            { "tsLvl",     "TS808 Level",      -12.0f,   12.0f,  0.0f,  " dB" },
            { "odOn",      "Yellow OD On",       0.0f,    1.0f,   0.0f,  "" },
            { "odDrive",   "Yellow OD Drive",    0.0f,    100.0f, 50.0f, " %" },
            { "odTone",    "Yellow OD Tone",     0.0f,    100.0f, 50.0f, " %" },
            { "odLvl",     "Yellow OD Level",  -12.0f,   12.0f,  0.0f,  " dB" },
            { "fuzzOn",    "Fuzz On",            0.0f,    1.0f,   0.0f,  "" },
            { "fuzzDrive", "Fuzz Drive",         0.0f,    100.0f, 60.0f, " %" },
            { "fuzzTone",  "Fuzz Tone",          0.0f,    100.0f, 50.0f, " %" },
            { "fuzzLvl",   "Fuzz Level",       -12.0f,   12.0f,  0.0f,  " dB" },
            // time block
            { "revOn",     "Reverb On",          0.0f,    1.0f,   0.0f,  "" },
            { "revMix",    "Reverb Mix",         0.0f,    100.0f, 25.0f, " %" },
            { "revDecay",  "Reverb Decay",       0.0f,    100.0f, 40.0f, " %" },
            { "revTone",   "Reverb Tone",        0.0f,    100.0f, 50.0f, " %" },
            { "dlyOn",     "Delay On",           0.0f,    1.0f,   0.0f,  "" },
            { "dlyTime",   "Delay Time",        20.0f,    1000.0f, 350.0f, " ms", 0.5f },
            { "dlyFb",     "Delay Repeats",      0.0f,    90.0f,  30.0f, " %" },
            { "dlyMix",    "Delay Mix",          0.0f,    100.0f, 25.0f, " %" },
            { "choOn",     "Chorus On",          0.0f,    1.0f,   0.0f,  "" },
            { "choRate",   "Chorus Rate",        0.05f,   5.0f,   0.8f,  " Hz", 0.5f },
            { "choDepth",  "Chorus Depth",       0.0f,    100.0f, 40.0f, " %" },
            { "choMix",    "Chorus Mix",         0.0f,    100.0f, 40.0f, " %" },
            { "tremOn",    "Tremolo On",         0.0f,    1.0f,   0.0f,  "" },
            { "tremRate",  "Tremolo Rate",       0.5f,    12.0f,  4.5f,  " Hz", 0.5f },
            { "tremDepth", "Tremolo Depth",      0.0f,    100.0f, 50.0f, " %" },
            // amp heads
            { "amp",       "Amp Model",          0.0f,    3.0f,   0.0f,  "" },
            { "fCh",       "Blackface Channel",  0.0f,    1.0f,   0.0f,  "" },
            { "fNVol",     "Normal Volume",      0.0f,    10.0f,  5.0f,  "" },
            { "fNTre",     "Normal Treble",      0.0f,    10.0f,  6.0f,  "" },
            { "fNMid",     "Normal Middle",      0.0f,    10.0f,  5.0f,  "" },
            { "fNBas",     "Normal Bass",        0.0f,    10.0f,  5.0f,  "" },
            { "fNBri",     "Normal Bright",      0.0f,    1.0f,   0.0f,  "" },
            { "fVVol",     "Vibrato Volume",     0.0f,    10.0f,  5.0f,  "" },
            { "fVTre",     "Vibrato Treble",     0.0f,    10.0f,  6.0f,  "" },
            { "fVMid",     "Vibrato Middle",     0.0f,    10.0f,  5.0f,  "" },
            { "fVBas",     "Vibrato Bass",       0.0f,    10.0f,  5.0f,  "" },
            { "fVRev",     "Vibrato Reverb",     0.0f,    10.0f,  3.0f,  "" },
            { "fVSpd",     "Vibrato Speed",      0.0f,    10.0f,  3.0f,  "" },
            { "fVInt",     "Vibrato Intensity",  0.0f,    10.0f,  3.0f,  "" },
            { "fVBri",     "Vibrato Bright",     0.0f,    1.0f,   0.0f,  "" },
            { "jPre",      "JCM Pre-amp",        0.0f,    10.0f,  6.0f,  "" },
            { "jPres",     "JCM Presence",       0.0f,    10.0f,  5.0f,  "" },
            { "jBas",      "JCM Bass",           0.0f,    10.0f,  5.0f,  "" },
            { "jMid",      "JCM Middle",         0.0f,    10.0f,  6.0f,  "" },
            { "jTre",      "JCM Treble",         0.0f,    10.0f,  7.0f,  "" },
            { "jMas",      "JCM Master",         0.0f,    10.0f,  6.0f,  "" },
            { "jSens",     "JCM High Input",     0.0f,    1.0f,   1.0f,  "" },
            { "pVol1",     "Plexi Volume I",     0.0f,    10.0f,  6.0f,  "" },
            { "pVol2",     "Plexi Volume II",    0.0f,    10.0f,  5.0f,  "" },
            { "pJump",     "Plexi Jump",         0.0f,    1.0f,   0.0f,  "" },
            { "pPres",     "Plexi Presence",     0.0f,    10.0f,  5.0f,  "" },
            { "pBas",      "Plexi Bass",         0.0f,    10.0f,  5.0f,  "" },
            { "pMid",      "Plexi Middle",       0.0f,    10.0f,  5.0f,  "" },
            { "pTre",      "Plexi Treble",       0.0f,    10.0f,  6.0f,  "" },
            { "mCh",       "Rectifier Channel",  0.0f,    1.0f,   0.0f,  "" },
            { "m1Gain",    "CH1 Gain",           0.0f,    10.0f,  5.0f,  "" },
            { "m1Mas",     "CH1 Master",         0.0f,    10.0f,  6.0f,  "" },
            { "m1Pres",    "CH1 Presence",       0.0f,    10.0f,  5.0f,  "" },
            { "m1Bas",     "CH1 Bass",           0.0f,    10.0f,  5.0f,  "" },
            { "m1Mid",     "CH1 Middle",         0.0f,    10.0f,  5.0f,  "" },
            { "m1Tre",     "CH1 Treble",         0.0f,    10.0f,  6.0f,  "" },
            { "m1Crush",   "CH1 Crush",          0.0f,    1.0f,   0.0f,  "" },
            { "m2Gain",    "CH2 Gain",           0.0f,    10.0f,  7.0f,  "" },
            { "m2Mas",     "CH2 Master",         0.0f,    10.0f,  6.0f,  "" },
            { "m2Pres",    "CH2 Presence",       0.0f,    10.0f,  5.0f,  "" },
            { "m2Bas",     "CH2 Bass",           0.0f,    10.0f,  5.0f,  "" },
            { "m2Mid",     "CH2 Middle",         0.0f,    10.0f,  5.0f,  "" },
            { "m2Tre",     "CH2 Treble",         0.0f,    10.0f,  6.0f,  "" },
            { "m2Crush",   "CH2 Crush",          0.0f,    1.0f,   1.0f,  "" },
            { "mWatt",     "Rectifier 100 W",    0.0f,    1.0f,   0.0f,  "" },
            // cabinet and output
            { "cab",       "Cabinet",            0.0f,    3.0f,   0.0f,  "" },
            { "micOff",    "Mic Off-Axis",       0.0f,    1.0f,   0.0f,  "" },
            { "out",       "Output",           -24.0f,   12.0f,  0.0f,  " dB" },
            // pedal order: slot i holds the pedal id that runs i-th (default: comp, boost, TS808, OD, fuzz)
            { "slot0",     "Pedal Slot 1",       0.0f,    4.0f,   0.0f,  "" },
            { "slot1",     "Pedal Slot 2",       0.0f,    4.0f,   1.0f,  "" },
            { "slot2",     "Pedal Slot 3",       0.0f,    4.0f,   2.0f,  "" },
            { "slot3",     "Pedal Slot 4",       0.0f,    4.0f,   3.0f,  "" },
            { "slot4",     "Pedal Slot 5",       0.0f,    4.0f,   4.0f,  "" },
            // module bypass: pedals / time effects keep their own On switches
            { "ampOn",     "Amp On",             0.0f,    1.0f,   1.0f,  "" },
            { "cabOn",     "Cabinet On",         0.0f,    1.0f,   1.0f,  "" },
            { "irOn",      "IR Cab On",          0.0f,    1.0f,   0.0f,  "" },
            { "irMix",     "IR Mix",             0.0f,  100.0f, 100.0f, " %" },
        };
    }
}

//==============================================================================
SxGuitarRig::SxGuitarRig() : SxEffectBase ("SX Guitar Rig", rigSpecs(), 2.5)
{
    jassert ((int) params.size() == (int) kNumParams);
}

void SxGuitarRig::prepareToPlay (double sr, int)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;
    sampleRateInner = 2.0 * sampleRate;

    oversampling.initProcessing ((size_t) kMaxChunk);
    oversampling.reset();
    setLatencySamples ((int) std::round (oversampling.getLatencyInSamples()));
    irConvolution.prepare (dsp::ProcessSpec { sampleRate, (juce::uint32) kMaxChunk, 2 });
    irDry.setSize (2, kMaxChunk);
    if (irPath.isNotEmpty())
    {
        juce::String ignored;
        loadIRPath (irPath, ignored);
    }

    for (int c = 0; c < 2; ++c)
    {
        boostTone[c].reset();
        tsHp[c].reset(); tsMid[c].reset(); tsLp[c].reset();
        odHp[c].reset(); odMid[c].reset(); odLp[c].reset();
        fzHp[c].reset(); fzLp[c].reset(); fzOut[c].reset();
        ampIn[c].reset(); ampBass[c].reset(); ampMid[c].reset(); ampTreble[c].reset(); ampPresence[c].reset();
        cabHp[c].reset(); cabRes[c].reset(); cabHonk[c].reset(); cabMic[c].reset(); cabLp[c].reset();
        for (auto& z : ampDc[c]) z = 0.0f;
        ampSag[c] = 0.0f;
        ampLfo[c] = 0.25f * (float) c;
        delayLp[c] = 0.0f;
    }
    compEnv = 0.0f;
    compReductionDb.store (0.0f);
    peakOut.store (0.0f);
    tremPhase = 0.0f;
    chorusPhase = 0.0f;

    chorusSize = jmax (16, (int) (sampleRateInner * 0.05));
    delaySize = jmax (16, (int) (sampleRateInner * 2.1));
    for (int c = 0; c < 2; ++c)
    {
        chorusRing[c].assign ((size_t) chorusSize, 0.0f);
        delayRing[c].assign ((size_t) delaySize, 0.0f);
    }
    chorusWrite = 0;
    delayWrite = 0;

    roomReverb.setSampleRate (sampleRateInner);
    roomReverb.reset();
    ampSpring.setSampleRate (sampleRateInner);
    ampSpring.reset();
}

void SxGuitarRig::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    if (buffer.getNumChannels() < 2) return;

    setupAmp();
    setupCab();

    dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), 2, (size_t) buffer.getNumSamples());
    for (size_t start = 0; start < block.getNumSamples(); start += (size_t) kMaxChunk)
    {
        const size_t len = jmin ((size_t) kMaxChunk, block.getNumSamples() - start);
        auto sub = block.getSubBlock (start, len);
        auto up = oversampling.processSamplesUp (sub);
        renderChunk (up.getChannelPointer (0), up.getChannelPointer (1), (int) up.getNumSamples());
        oversampling.processSamplesDown (sub);

        // IR cabinet at the project rate: the modelled cab is skipped in renderChunk when this
        // path is active. Mix keeps the dry amp/cab signal available for parallel IR blending.
        if (param (pCabOn) > 0.5f && param (pIrOn) > 0.5f && irReady.load())
        {
            const int m = (int) sub.getNumSamples();
            const float mix = jlimit (0.0f, 1.0f, (float) param (pIrMix) / 100.0f);
            if (mix < 1.0f)
                for (int c = 0; c < 2; ++c)
                    irDry.copyFrom (c, 0, sub.getChannelPointer (c), m);
            dsp::ProcessContextReplacing<float> irContext (sub);
            irConvolution.process (irContext);
            if (mix < 1.0f)
                for (int c = 0; c < 2; ++c)
                {
                    auto* d = sub.getChannelPointer (c);
                    const auto* dry = irDry.getReadPointer (c);
                    for (int i = 0; i < m; ++i) d[i] = dry[i] * (1.0f - mix) + d[i] * mix;
                }
        }
    }
}

std::array<int, 5> SxGuitarRig::pedalOrderNow() const
{
    const std::array<int, 5> identity { 0, 1, 2, 3, 4 };
    std::array<int, 5> order = identity;
    std::array<bool, 5> used {};
    for (int s = 0; s < 5; ++s)
    {
        const int id = jlimit (0, 4, rounded ((float) param (pSlot0 + s)));
        if (used[(size_t) id]) return identity;
        used[(size_t) id] = true;
        order[(size_t) s] = id;
    }
    return order;
}

void SxGuitarRig::applyPedal (int id, float* l, float* r, int n)
{
    switch (id)
    {
        case 0: compStage (l, r, n); break;
        case 1: boostStage (0, l, n); boostStage (1, r, n); break;
        case 2: tubeScreamerStage (0, l, n); tubeScreamerStage (1, r, n); break;
        case 3: yellowOdStage (0, l, n); yellowOdStage (1, r, n); break;
        case 4: fuzzStage (0, l, n); fuzzStage (1, r, n); break;
        default: break;
    }
}

void SxGuitarRig::renderChunk (float* l, float* r, int n)
{
    for (int id : pedalOrderNow())
        applyPedal (id, l, r, n);
    const bool ampOn = param (pAmpOn) > 0.5f;
    if (ampOn)
        for (int c = 0; c < 2; ++c)
            ampStage (c, c == 0 ? l : r, n);
    if (ampOn)
        springStage (l, r, n);
    const bool irCab = irReady.load() && param (pIrOn) > 0.5f;
    if (param (pCabOn) > 0.5f && ! irCab)
        for (int c = 0; c < 2; ++c)
            cabStage (c, c == 0 ? l : r, n);
    timeStage (l, r, n);

    const float out = dB ((float) param (pOutput));
    float pk = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        l[i] *= out;
        r[i] *= out;
        pk = jmax (pk, std::abs (l[i]), std::abs (r[i]));
    }
    if (pk > peakOut.load()) peakOut.store (pk);
}

//==============================================================================
//== compressor: linked stereo detector, soft knee, fixed release
void SxGuitarRig::compStage (float* l, float* r, int n)
{
    if (param (pCompOn) < 0.5f)
    {
        compReductionDb.store (0.0f);
        return;
    }
    const double sr = sampleRateInner;
    const float sustain = (float) param (pCompSustain) / 100.0f;
    const float thr = -8.0f - 42.0f * sustain;
    const float ratio = 3.0f + 6.0f * sustain;
    const float knee = 6.0f;
    const float atk = (float) std::exp (-1.0 / (0.001 * param (pCompAttack) * sr));
    const float rel = (float) std::exp (-1.0 / (0.25 * sr));
    const float makeupDb = -0.4f * thr + (float) param (pCompLevel);

    float worst = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float lvl = jmax (std::abs (l[i]), std::abs (r[i]));
        compEnv = lvl > compEnv ? atk * compEnv + (1.0f - atk) * lvl
                                : rel * compEnv + (1.0f - rel) * lvl;
        const float over = Decibels::gainToDecibels (compEnv, -120.0f) - thr;
        float gr = 0.0f;
        if (over > knee * 0.5f)
            gr = over * (1.0f - 1.0f / ratio);
        else if (over > -knee * 0.5f)
        {
            const float t = over + knee * 0.5f;
            gr = t * t / (2.0f * knee) * (1.0f - 1.0f / ratio);
        }
        const float g = dB (makeupDb - gr);
        l[i] *= g;
        r[i] *= g;
        worst = jmax (worst, gr);
    }
    compReductionDb.store (worst);
}

//== clean boost: gain into a soft ceiling, treble shelf, level
void SxGuitarRig::boostStage (int c, float* x, int n)
{
    if (param (pBoostOn) < 0.5f) return;
    highShelf (boostTone[c], sampleRateInner, 2800.0, param (pBoostTreble));
    const float gain = dB (0.2f * (float) param (pBoostGain));
    const float level = dB ((float) param (pBoostLevel));
    for (int i = 0; i < n; ++i)
        x[i] = boostTone[c].process (cubicClip (x[i] * gain)) * level;
}

//== TS808 style: 220 Hz tighten, 720 Hz mid hump, one diode-ish stage, tone low-pass
void SxGuitarRig::tubeScreamerStage (int c, float* x, int n)
{
    if (param (pTsOn) < 0.5f) return;
    const double sr = sampleRateInner;
    const float drive = (float) param (pTsDrive) / 100.0f;
    const float tone = (float) param (pTsTone) / 100.0f;
    highPass (tsHp[c], sr, 220.0, 0.7);
    peaking (tsMid[c], sr, 720.0, 6.0, 1.2);
    lowPass (tsLp[c], sr, 1200.0 * std::pow (7.0, tone), 0.7);
    const float gain = dB (4.0f + 40.0f * drive);
    const float level = dB ((float) param (pTsLevel));
    for (int i = 0; i < n; ++i)
    {
        const float y = std::tanh (tsMid[c].process (tsHp[c].process (x[i])) * gain);
        x[i] = tsLp[c].process (y) * level;
    }
}

//== yellow overdrive: 700 Hz mid push, asymmetric clip, tone low-pass
void SxGuitarRig::yellowOdStage (int c, float* x, int n)
{
    if (param (pOdOn) < 0.5f) return;
    const double sr = sampleRateInner;
    const float drive = (float) param (pOdDrive) / 100.0f;
    const float tone = (float) param (pOdTone) / 100.0f;
    highPass (odHp[c], sr, 120.0, 0.7);
    peaking (odMid[c], sr, 700.0, 5.0, 1.3);
    lowPass (odLp[c], sr, 2000.0 * std::pow (4.0, tone), 0.7);
    const float gain = dB (6.0f + 34.0f * drive);
    const float bias = 0.04f;
    const float level = dB ((float) param (pOdLevel));
    for (int i = 0; i < n; ++i)
    {
        const float y = std::tanh (odMid[c].process (odHp[c].process (x[i])) * gain + bias) - std::tanh (bias);
        x[i] = odLp[c].process (y) * level;
    }
}

//== fuzz: two transistor-like stages, the first asymmetric, then a tone low-pass and a DC cut
void SxGuitarRig::fuzzStage (int c, float* x, int n)
{
    if (param (pFuzzOn) < 0.5f) return;
    const double sr = sampleRateInner;
    const float drive = (float) param (pFuzzDrive) / 100.0f;
    const float tone = (float) param (pFuzzTone) / 100.0f;
    highPass (fzHp[c], sr, 60.0, 0.7);
    lowPass (fzLp[c], sr, 900.0 * std::pow (9.0, tone), 0.7);
    highPass (fzOut[c], sr, 40.0, 0.7);
    const float gain = dB (12.0f + 46.0f * drive);
    const float bias = 0.3f;
    const float level = dB ((float) param (pFuzzLevel));
    for (int i = 0; i < n; ++i)
    {
        float y = fzHp[c].process (x[i]);
        y = std::tanh (y * gain + bias) - std::tanh (bias);
        y = std::tanh (y * 3.0f);
        x[i] = fzOut[c].process (fzLp[c].process (y)) * level;
    }
}

//==============================================================================
//== amp heads: voicing from the active model, then the per-channel tone stacks
void SxGuitarRig::setupAmp()
{
    const double sr = sampleRateInner;
    const int model = jlimit (0, (int) numAmps - 1, rounded ((float) param (pAmp)));

    AmpVoicing v;
    double bassHz = 100.0, midHz = 600.0, midQ = 0.9, trebHz = 3000.0, presHz = 4500.0;
    float bassDb = 0.0f, midDb = 0.0f, trebDb = 0.0f, presDb = 0.0f;

    switch (model)
    {
        case Blackface:
        {
            const bool vib = param (pFChannel) > 0.5;
            const float vol = (float) param (vib ? pFVVolume : pFNVolume);
            const bool bright = param (vib ? pFVBright : pFNBright) > 0.5;
            bassDb = 1.6f * ((float) param (vib ? pFVBass : pFNBass) - 5.0f);
            midDb = 1.4f * ((float) param (vib ? pFVMiddle : pFNMiddle) - 5.0f);
            trebDb = 1.6f * ((float) param (vib ? pFVTreble : pFNTreble) - 5.0f) + (bright ? 4.0f : 0.0f);
            bassHz = 110.0; midHz = 550.0; midQ = 0.9; trebHz = 3200.0;
            v.stages = 2;
            v.stageGain[0] = dB (-6.0f + 2.4f * vol + (vib ? 8.0f : 0.0f));
            v.stageGain[1] = dB (6.0f);
            v.stageBias[0] = 0.04f;
            v.stageBias[1] = 0.02f;
            v.inputHz = 90.0f;
            v.sag = 0.12f;
            v.powerDrive = 0.45f;
            v.outGain = 0.7f;
            v.vibrato = vib;
            v.vibratoDepth = vib ? 0.045f * (float) param (pFVIntensity) : 0.0f;
            v.vibratoHz = 1.0f + 0.9f * (float) param (pFVSpeed);
            v.springMix = vib ? 0.03f * (float) param (pFVReverb) : 0.0f;
            break;
        }
        case Jcm800:
        {
            const float trim = param (pJHighSens) > 0.5 ? 0.0f : -6.0f;
            bassDb = 2.0f * ((float) param (pJBass) - 5.0f);
            midDb = 1.6f * ((float) param (pJMiddle) - 5.0f);
            trebDb = 2.0f * ((float) param (pJTreble) - 5.0f);
            presDb = 1.6f * ((float) param (pJPresence) - 5.0f);
            bassHz = 100.0; midHz = 650.0; midQ = 0.9; trebHz = 3000.0; presHz = 4500.0;
            v.stages = 3;
            v.stageGain[0] = dB (trim + 4.0f + 2.6f * (float) param (pJPreamp));
            v.stageGain[1] = dB (16.0f);
            v.stageGain[2] = dB (14.0f);
            v.stageBias[0] = 0.06f;
            v.stageBias[1] = 0.12f;
            v.stageBias[2] = 0.08f;
            v.inputHz = 75.0f;
            v.sag = 0.08f;
            v.powerDrive = 1.0f;
            v.outGain = dB (-24.0f + 2.8f * (float) param (pJMaster));
            break;
        }
        case Plexi:
        {
            const bool jump = param (pPJump) > 0.5;
            bassDb = 2.0f * ((float) param (pPBass) - 5.0f);
            midDb = 1.2f * ((float) param (pPMiddle) - 5.0f);
            trebDb = 2.2f * ((float) param (pPTreble) - 5.0f);
            presDb = 1.4f * ((float) param (pPPresence) - 5.0f);
            bassHz = 100.0; midHz = 600.0; midQ = 0.8; trebHz = 3400.0; presHz = 5000.0;
            v.stages = 2;
            v.stageGain[0] = dB (-6.0f + 2.2f * (float) param (pPVolume1));
            v.stageGain[1] = dB (jump ? 14.0f + 2.6f * (float) param (pPVolume2) : 14.0f);
            v.stageBias[0] = 0.07f;
            v.stageBias[1] = 0.10f;
            v.inputHz = 80.0f;
            v.sag = 0.10f;
            v.powerDrive = 0.9f;
            v.outGain = dB (-6.0f);
            break;
        }
        case Rectifier:
        {
            const bool ch2 = param (pMChannel) > 0.5;
            const bool watt100 = param (pMWatt) > 0.5;
            const bool crush = param (ch2 ? pM2Crush : pM1Crush) > 0.5;
            const float gain = (float) param (ch2 ? pM2Gain : pM1Gain);
            const float master = (float) param (ch2 ? pM2Master : pM1Master);
            bassDb = 2.2f * ((float) param (ch2 ? pM2Bass : pM1Bass) - 5.0f);
            midDb = 1.8f * ((float) param (ch2 ? pM2Middle : pM1Middle) - 5.0f);
            trebDb = 2.2f * ((float) param (ch2 ? pM2Treble : pM1Treble) - 5.0f);
            presDb = 1.8f * ((float) param (ch2 ? pM2Presence : pM1Presence) - 5.0f);
            bassHz = 90.0; midHz = 750.0; midQ = 0.9; trebHz = 2800.0; presHz = 4000.0;
            v.stages = crush ? 3 : 2;
            v.stageGain[0] = dB (8.0f + 4.2f * gain + (ch2 ? 4.0f : 0.0f));
            v.stageGain[1] = dB (crush ? 14.0f : 8.0f);
            v.stageGain[2] = dB (12.0f);
            v.stageBias[0] = 0.03f;
            v.stageBias[1] = 0.06f;
            v.stageBias[2] = 0.08f;
            v.inputHz = crush ? 130.0f : 90.0f;
            v.sag = watt100 ? 0.10f : 0.18f;
            v.powerDrive = watt100 ? 0.75f : 1.25f;
            v.outGain = dB (-28.0f + 2.8f * master) * (watt100 ? 1.0f : 1.8f);
            break;
        }
        default:
            break;
    }

    amp = v;
    ampDcK = (float) (1.0 - std::exp (-2.0 * kPi * 35.0 / sr));
    for (int c = 0; c < 2; ++c)
    {
        highPass (ampIn[c], sr, v.inputHz, 0.7);
        lowShelf (ampBass[c], sr, bassHz, bassDb);
        peaking (ampMid[c], sr, midHz, midDb, midQ);
        highShelf (ampTreble[c], sr, trebHz, trebDb);
        highShelf (ampPresence[c], sr, presHz, presDb);
    }
}

void SxGuitarRig::ampStage (int c, float* x, int n)
{
    const AmpVoicing& v = amp;
    float* dc = ampDc[c];
    float lfo = ampLfo[c];
    const float lfoInc = v.vibratoHz / (float) sampleRateInner;

    for (int i = 0; i < n; ++i)
    {
        float y = ampIn[c].process (x[i]);

        if (v.vibrato)
        {
            y *= 1.0f - v.vibratoDepth * (0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * lfo));
            lfo += lfoInc;
            if (lfo >= 1.0f) lfo -= 1.0f;
        }

        // gain stages: each one clips with its own asymmetry, and a coupling cap removes the offset
        for (int s = 0; s < v.stages; ++s)
        {
            const float b = v.stageBias[s];
            y = std::tanh (y * v.stageGain[s] + b) - std::tanh (b);
            dc[s] += ampDcK * (y - dc[s]);
            y -= dc[s];
        }

        y = ampBass[c].process (y);
        y = ampMid[c].process (y);
        y = ampTreble[c].process (y);
        y = ampPresence[c].process (y);

        // power amp: loud passages sag the supply, then the output stage rounds off the peaks
        const float a = std::abs (y);
        ampSag[c] += (a - ampSag[c]) * (a > ampSag[c] ? 0.02f : 0.0015f);
        y *= 1.0f - v.sag * jmin (1.0f, 1.5f * ampSag[c]);
        y = std::tanh (y * v.powerDrive) / v.powerDrive;

        x[i] = y * v.outGain;
    }
    ampLfo[c] = lfo;
}

//== the Blackface vibrato channel has a reverb control, here a short spring-like room
void SxGuitarRig::springStage (float* l, float* r, int n)
{
    if (amp.springMix <= 0.001f) return;
    Reverb::Parameters p;
    p.roomSize = 0.25f;
    p.damping = 0.6f;
    p.wetLevel = amp.springMix;
    p.dryLevel = 1.0f - 0.5f * amp.springMix;
    p.width = 1.0f;
    p.freezeMode = 0.0f;
    ampSpring.setParameters (p);
    ampSpring.processStereo (l, r, n);
}

//==============================================================================
//== cabinets: coupling, the cabinet resonance and the box's own colour, mic position last
void SxGuitarRig::setupCab()
{
    const double sr = sampleRateInner;
    struct Cabinet { double hp, res, resDb, resQ, honkHz, honkDb, honkQ, lp; };
    static const Cabinet cabs[4] {
        { 75.0, 120.0, 3.0, 1.0, 2200.0, 2.5, 1.0, 6000.0 },   // Fender 2x12, open back
        { 85.0, 105.0, 4.0, 1.1, 1200.0, 4.0, 1.4, 4800.0 },   // Marshall 4x12, the mid honk
        { 90.0,  95.0, 3.5, 1.0, 2500.0, 3.0, 1.2, 5300.0 },   // Mesa 4x12, tight and bright
        { 100.0, 140.0, 5.0, 1.2, 1800.0, 2.5, 1.0, 4000.0 } }; // 1x12 open back
    const int type = jlimit (0, 3, rounded ((float) param (pCab)));
    const Cabinet& k = cabs[type];
    const bool offAxis = param (pMicOff) > 0.5;

    for (int c = 0; c < 2; ++c)
    {
        highPass (cabHp[c], sr, k.hp, 0.7);
        peaking (cabRes[c], sr, k.res, k.resDb, k.resQ);
        peaking (cabHonk[c], sr, k.honkHz, k.honkDb, k.honkQ);
        peaking (cabMic[c], sr, offAxis ? 3000.0 : 2800.0, offAxis ? -2.5 : 2.5, offAxis ? 1.0 : 1.5);
        lowPass (cabLp[c], sr, offAxis ? std::min (k.lp, 3600.0) : k.lp, 0.7);
    }
}

void SxGuitarRig::cabStage (int c, float* x, int n)
{
    for (int i = 0; i < n; ++i)
    {
        float y = cabHp[c].process (x[i]);
        y = cabRes[c].process (y);
        y = cabHonk[c].process (y);
        y = cabMic[c].process (y);
        x[i] = cabLp[c].process (y);
    }
}

void SxGuitarRig::timeStage (float* l, float* r, int n)
{
    const double sr = sampleRateInner;

    if (param (pTremOn) > 0.5f)
    {
        const float inc = (float) param (pTremRate) / (float) sr;
        const float depth = (float) param (pTremDepth) / 100.0f;
        for (int i = 0; i < n; ++i)
        {
            const float ph = tremPhase;
            l[i] *= 1.0f - depth * (0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * ph));
            r[i] *= 1.0f - depth * (0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * (ph + 0.5f)));
            tremPhase += inc;
            if (tremPhase >= 1.0f) tremPhase -= 1.0f;
        }
    }

    if (param (pChoOn) > 0.5f)
    {
        const float inc = (float) param (pChoRate) / (float) sr;
        const float depth = (float) param (pChoDepth) / 100.0f;
        const float mix = (float) param (pChoMix) / 100.0f;
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                float& xs = c == 0 ? l[i] : r[i];
                chorusRing[c][(size_t) chorusWrite] = xs;
                const float lfo = std::sin (MathConstants<float>::twoPi * (chorusPhase + 0.25f * (float) c));
                const float d = jlimit (1.0f, (float) chorusSize - 2.0f, (8.0f + 4.0f * depth * lfo) * (float) sr / 1000.0f);
                const int i0 = (int) d;
                const float fr = d - (float) i0;
                const int r0 = (chorusWrite - i0 + chorusSize) % chorusSize;
                const int r1 = (r0 - 1 + chorusSize) % chorusSize;
                const float wet = chorusRing[c][(size_t) r0] * (1.0f - fr) + chorusRing[c][(size_t) r1] * fr;
                xs = xs * (1.0f - mix) + wet * mix;
            }
            chorusWrite = (chorusWrite + 1) % chorusSize;
            chorusPhase += inc;
            if (chorusPhase >= 1.0f) chorusPhase -= 1.0f;
        }
    }

    if (param (pDlyOn) > 0.5f)
    {
        const int dSamples = jlimit (1, delaySize - 2, (int) ((float) param (pDlyTime) * (float) sr / 1000.0f));
        const float fb = (float) param (pDlyFeedback) / 100.0f;
        const float mix = (float) param (pDlyMix) / 100.0f;
        for (int i = 0; i < n; ++i)
        {
            const int rp = (delayWrite - dSamples + delaySize) % delaySize;
            for (int c = 0; c < 2; ++c)
            {
                float& xs = c == 0 ? l[i] : r[i];
                const float dl = delayRing[c][(size_t) rp];
                delayLp[c] += 0.35f * (dl - delayLp[c]);
                delayRing[c][(size_t) delayWrite] = xs + delayLp[c] * fb;
                xs = xs * (1.0f - mix) + dl * mix;
            }
            delayWrite = (delayWrite + 1) % delaySize;
        }
    }

    if (param (pRevOn) > 0.5f)
    {
        const float mix = (float) param (pRevMix) / 100.0f;
        const float decay = (float) param (pRevDecay) / 100.0f;
        const float tone = (float) param (pRevTone) / 100.0f;
        Reverb::Parameters p;
        p.roomSize = 0.2f + 0.7f * decay;
        p.damping = 0.8f - 0.6f * tone;
        p.wetLevel = mix;
        p.dryLevel = 1.0f - 0.5f * mix;
        p.width = 1.0f;
        p.freezeMode = 0.0f;
        roomReverb.setParameters (p);
        roomReverb.processStereo (l, r, n);
    }
}

//==============================================================================
//== cabinet IR and state / presets
bool SxGuitarRig::loadIRPath (const String& path, String& error)
{
    const File f (path);
    if (! f.existsAsFile())
    {
        error = TRANS ("IR file not found.");
        return false;
    }

    AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
    {
        error = TRANS ("Unsupported IR file (use WAV, AIFF or FLAC).");
        return false;
    }

    irConvolution.loadImpulseResponse (f, dsp::Convolution::Stereo::yes,
                                      dsp::Convolution::Trim::no, 0,
                                      dsp::Convolution::Normalise::yes);
    irPath = f.getFullPathName();
    irFileName = f.getFileNameWithoutExtension();
    irReady.store (true);

    // make the freshly loaded IR active for the next block
    dsp::ProcessSpec spec { sampleRate > 0.0 ? sampleRate : 44100.0, (juce::uint32) kMaxChunk, 2 };
    irConvolution.prepare (spec);
    return true;
}

bool SxGuitarRig::loadIRFile (const File& file, String& error)
{
    return loadIRPath (file.getFullPathName(), error);
}

void SxGuitarRig::clearIR()
{
    irConvolution.reset();
    irReady.store (false);
    irPath.clear();
    irFileName.clear();
}

void SxGuitarRig::getStateInformation (MemoryBlock& dest)
{
    SxEffectBase::getStateInformation (dest);
    if (auto xml = getXmlFromBinary (dest.getData(), (int) dest.getSize()))
    {
        if (irPath.isNotEmpty()) xml->setAttribute ("irPath", irPath);
        if (irFileName.isNotEmpty()) xml->setAttribute ("irName", irFileName);
        copyXmlToBinary (*xml, dest);
    }
}

void SxGuitarRig::setStateInformation (const void* data, int size)
{
    SxEffectBase::setStateInformation (data, size);
    if (auto xml = getXmlFromBinary (data, size))
    {
        const String path = xml->getStringAttribute ("irPath");
        irFileName = xml->getStringAttribute ("irName");
        if (path.isEmpty())
        {
            clearIR();
        }
        else if (path != irPath)
        {
            String ignored;
            if (! loadIRPath (path, ignored)) irReady.store (false);
        }
    }
}
}   // namespace sx
