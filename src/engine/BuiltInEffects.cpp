#include "BuiltInEffects.h"
#include "GuitarRig.h"

namespace sx
{
using namespace juce;

//==============================================================================
namespace
{
    const std::vector<BuiltInEffectInfo> kEffects
    {
        { BuiltInEffect::Eq,            "SX EQ",             "SoundX 4-band visual equalizer",    "sx.builtin.eq",      0x53584551 },
        { BuiltInEffect::Compressor,    "SX Compressor",     "SoundX compressor",                 "sx.builtin.comp",    0x53584350 },
        { BuiltInEffect::OptoCompressor,"SX 2A Compressor",  "SoundX opto (valve) compressor",    "sx.builtin.comp2a",  0x53584332 },
        { BuiltInEffect::FetCompressor, "SX 76 Compressor",  "SoundX FET compressor",             "sx.builtin.comp76",  0x53584337 },
        { BuiltInEffect::Limiter,       "SX Limiter",        "SoundX brickwall limiter",          "sx.builtin.lim",     0x53584c49 },
        { BuiltInEffect::Convolver,     "SX Convolver",      "SoundX convolution reverb",         "sx.builtin.conv",    0x53584356 },
        { BuiltInEffect::AnalogReverb,  "SX Analog Reverb",  "SoundX analogue style reverb",      "sx.builtin.revA",    0x53585241 },
        { BuiltInEffect::DigitalReverb, "SX Digital Reverb", "SoundX digital (FDN) reverb",       "sx.builtin.revD",    0x53585244 },
        { BuiltInEffect::Delay,         "SX Delay",          "SoundX stereo delay",               "sx.builtin.delayFX", 0x5358444c },
        { BuiltInEffect::Chorus,        "SX Chorus",         "SoundX chorus",                     "sx.builtin.chorus",  0x53584348 },
        { BuiltInEffect::Flanger,       "SX Flanger",        "SoundX flanger",                    "sx.builtin.flang",   0x5358464c },
        { BuiltInEffect::Phaser,        "SX Phaser",         "SoundX phaser",                     "sx.builtin.phaser",  0x53585048 },
        { BuiltInEffect::Tremolo,       "SX Tremolo",        "SoundX tremolo",                    "sx.builtin.trem",    0x53585452 },
        { BuiltInEffect::Distortion,    "SX Distortion",     "SoundX distortion / drive",         "sx.builtin.dist",    0x53584449 },
        { BuiltInEffect::Stereo,        "SX Stereo",         "SoundX spatial placement, room and linked tracks", "sx.builtin.stereo",  0x53585354 },
        { BuiltInEffect::Meter,         "SX Meter",          "SoundX level and stereo field meter","sx.builtin.meter",    0x53584d45 },
        { BuiltInEffect::LoudnessMeter, "SX Loudness",       "SoundX peak / RMS / LUFS meter",     "sx.builtin.loudness", 0x53584c4f },
        { BuiltInEffect::StereoField,   "SX Stereo Field",   "SoundX goniometer / width meter",    "sx.builtin.field",    0x5358464c },
        { BuiltInEffect::Doubler,       "SX Doubler",        "SoundX four-voice doubler",          "sx.builtin.doubler",  0x53584442 },
        { BuiltInEffect::Tape,          "SX Tape",           "SoundX tape machine",                "sx.builtin.tape",     0x53585450 },
        { BuiltInEffect::Exciter,       "SX Exciter",        "SoundX harmonic exciter",            "sx.builtin.excite",   0x53584558 },
        { BuiltInEffect::Saturator,     "SX Saturator",      "SoundX saturation / distortion",     "sx.builtin.satur",    0x53585341 },
        { BuiltInEffect::MultibandComp, "SX Multiband",      "SoundX OTT style multiband comp",    "sx.builtin.mbcomp",   0x53584d42 },
        { BuiltInEffect::ChorusEnsemble,"SX Chorus Ensemble","SoundX three voice chorus",           "sx.builtin.chens",    0x53584345 },
        { BuiltInEffect::Maximizer,     "SX Maximizer",      "SoundX look-ahead maximiser",        "sx.builtin.maxim",    0x53584d58 },
        { BuiltInEffect::Tuner,         "SX Tuner",          "SoundX chromatic tuner",             "sx.builtin.tuner",    0x5358544e },
        { BuiltInEffect::Clipper,        "SX Clipper",        "SoundX oversampled clipper",         "sx.builtin.clipper",  0x5358434c },
        { BuiltInEffect::SslChannel,    "SX Console Channel","SSL-style channel strip: HPF, shelves, compressor", "sx.builtin.sslch", 0x53585343 },
        { BuiltInEffect::DeEsser,       "SX De-Esser",       "SoundX sibilance compressor",        "sx.builtin.deess",    0x53584445 },
        { BuiltInEffect::Gate,          "SX Gate",           "SoundX gate / downward expander",    "sx.builtin.gate",     0x53584741 },
        { BuiltInEffect::Denoiser,      "SX NS1 Denoiser",   "SoundX adaptive noise suppressor",   "sx.builtin.denois",   0x5358444e },
        { BuiltInEffect::Reverse,       "SX Reverse",        "Reverse-grain audio effect",         "sx.builtin.reverse",  0x53585256 },
        { BuiltInEffect::PitchCorrector,"SX Pitch Corrector", "SoundX auto-tune / pitch correction", "sx.builtin.pitchcorr", 0x53585043 },
        { BuiltInEffect::GuitarRig,     "SX Guitar Rig",     "SoundX guitar rig: pedals, amps, cabinets, time", "sx.builtin.guitarrig", 0x53584752 },
    };

    inline float onePoleCoeff (double sr, float hz)
    {
        return 1.0f - std::exp (-2.0f * MathConstants<float>::pi * jlimit (1.0f, 20000.0f, hz) / (float) jmax (8000.0, sr));
    }
}

const std::vector<BuiltInEffectInfo>& builtInEffects() { return kEffects; }

bool isBuiltInEffect (const PluginDescription& d)
{
    for (auto& e : kEffects)
        if (d.fileOrIdentifier == e.identifier) return true;
    return false;
}

BuiltInEffect builtInEffectKind (const PluginDescription& d)
{
    for (auto& e : kEffects)
        if (d.fileOrIdentifier == e.identifier) return e.id;
    return BuiltInEffect::Eq;
}

PluginSlot makeBuiltInEffectSlot (BuiltInEffect id)
{
    PluginSlot s;
    s.uid = makeUid();
    s.builtIn = true;
    for (auto& e : kEffects)
    {
        if (e.id != id) continue;
        s.desc.name = e.name;
        s.desc.descriptiveName = e.description;
        s.desc.pluginFormatName = "Built-in";
        s.desc.fileOrIdentifier = e.identifier;
        s.desc.manufacturerName = "SoundX";
        s.desc.isInstrument = false;
        s.desc.numInputChannels = 2;
        s.desc.numOutputChannels = 2;
        s.desc.uniqueId = e.uniqueId;
        break;
    }
    return s;
}

namespace
{
std::unique_ptr<AudioProcessor> createBuiltInEffectImpl (BuiltInEffect id)
{
    switch (id)
    {
        case BuiltInEffect::Eq:             return std::make_unique<SxEq>();
        case BuiltInEffect::Compressor:     return std::make_unique<SxCompressor>();
        case BuiltInEffect::OptoCompressor: return std::make_unique<SxOptoCompressor>();
        case BuiltInEffect::FetCompressor:  return std::make_unique<SxFetCompressor>();
        case BuiltInEffect::Convolver:      return std::make_unique<SxConvolver>();
        case BuiltInEffect::AnalogReverb:   return std::make_unique<SxAnalogReverb>();
        case BuiltInEffect::DigitalReverb:  return std::make_unique<SxDigitalReverb>();
        case BuiltInEffect::Limiter:        return std::make_unique<SxLimiter>();
        case BuiltInEffect::Stereo:         return std::make_unique<SxStereo>();
        case BuiltInEffect::Chorus:         return std::make_unique<SxChorus>();
        case BuiltInEffect::Delay:          return std::make_unique<SxDelay>();
        case BuiltInEffect::Distortion:     return std::make_unique<SxDistortion>();
        case BuiltInEffect::Tremolo:        return std::make_unique<SxTremolo>();
        case BuiltInEffect::Flanger:        return std::make_unique<SxFlanger>();
        case BuiltInEffect::Phaser:         return std::make_unique<SxPhaser>();
        case BuiltInEffect::Meter:          return std::make_unique<SxMeterEffect>();
        case BuiltInEffect::LoudnessMeter:  return std::make_unique<SxLoudnessMeter>();
        case BuiltInEffect::StereoField:    return std::make_unique<SxStereoFieldMeter>();
        case BuiltInEffect::Doubler:        return std::make_unique<SxDoubler>();
        case BuiltInEffect::Tape:           return std::make_unique<SxTape>();
        case BuiltInEffect::Exciter:        return std::make_unique<SxExciter>();
        case BuiltInEffect::Saturator:      return std::make_unique<SxSaturator>();
        case BuiltInEffect::MultibandComp:  return std::make_unique<SxMultibandComp>();
        case BuiltInEffect::ChorusEnsemble: return std::make_unique<SxChorusEnsemble>();
        case BuiltInEffect::Maximizer:      return std::make_unique<SxMaximizer>();
        case BuiltInEffect::Tuner:          return std::make_unique<SxTuner>();
        case BuiltInEffect::Clipper:        return std::make_unique<SxClipper>();
        case BuiltInEffect::SslChannel:     return std::make_unique<SxSslChannel>();
        case BuiltInEffect::DeEsser:        return std::make_unique<SxDeEsser>();
        case BuiltInEffect::Gate:           return std::make_unique<SxGate>();
        case BuiltInEffect::Denoiser:       return std::make_unique<SxDenoiser>();
        case BuiltInEffect::Reverse:        return std::make_unique<SxReverse>();
        case BuiltInEffect::PitchCorrector: return std::make_unique<SxPitchCorrector>();
        case BuiltInEffect::GuitarRig:      return std::make_unique<SxGuitarRig>();
    }
    return std::make_unique<SxEq>();
}
}   // namespace

std::unique_ptr<AudioProcessor> createBuiltInEffect (BuiltInEffect id)
{
    auto proc = createBuiltInEffectImpl (id);
    if (auto* base = dynamic_cast<SxEffectBase*> (proc.get())) base->kind = id;
    return proc;
}

//==============================================================================
SxEffectBase::SxEffectBase (const String& name, std::vector<ParamSpec> s, double tailSeconds)
    : effectName (name), specs (std::move (s)), tail (tailSeconds)
{
    for (auto& p : specs)
    {
        NormalisableRange<float> range (p.min, p.max);
        if (p.skew > 0.0f && std::abs (p.skew - 1.0f) > 1.0e-6f && p.def > p.min && p.def < p.max)
            range.setSkewForCentre (p.def);
        auto* param = new AudioParameterFloat (ParameterID { String (p.id), 1 }, String (p.name), range, p.def);
        addParameter (param);
        params.push_back (param);
    }
}

void SxEffectBase::getStateInformation (MemoryBlock& dest)
{
    XmlElement xml ("SX_EFFECT");
    for (int i = 0; i < numParams(); ++i)
    {
        auto* e = xml.createNewChildElement ("P");
        e->setAttribute ("i", i);
        e->setAttribute ("v", (double) param (i));
    }
    copyXmlToBinary (xml, dest);
}

bool SxEffectBase::savePresetFile (const File& f)
{
    MemoryBlock mb;
    getStateInformation (mb);
    return f.replaceWithData (mb.getData(), mb.getSize());
}

bool SxEffectBase::loadPresetFile (const File& f)
{
    MemoryBlock mb;
    if (! f.existsAsFile() || ! f.loadFileAsData (mb) || mb.getSize() < 8) return false;
    setStateInformation (mb.getData(), (int) mb.getSize());
    return true;
}

void SxEffectBase::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr) return;
    for (auto* e : xml->getChildWithTagNameIterator ("P"))
    {
        const int i = e->getIntAttribute ("i", -1);
        if (i >= 0 && i < numParams()) setParam (i, (float) e->getDoubleAttribute ("v"));
    }
}

void SxEffectBase::pushSpectrum (const float* bins, int n)
{
    for (int i = 0; i < kSpectrumBins; ++i)
        spectrum[(size_t) i].store (i < n ? bins[i] : 0.0f);
}


//==============================================================================
//== SX Compressor
SxCompressor::SxCompressor() : SxEffectBase ("SX Compressor", {
    { "thr", "Threshold", -60.0f, 0.0f, -18.0f, " dB" },
    { "ratio", "Ratio", 1.0f, 20.0f, 4.0f },
    { "atk", "Attack", 0.1f, 100.0f, 10.0f, " ms" },
    { "rel", "Release", 10.0f, 1000.0f, 150.0f, " ms" },
    { "knee", "Knee", 0.0f, 12.0f, 6.0f, " dB" },
    { "makeup", "Makeup", -12.0f, 24.0f, 0.0f, " dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxCompressor::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    envDb = 0.0f;
    atk = rel = 0.0f;
}

void SxCompressor::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float thr = param (0), ratio = jmax (1.0f, param (1)), knee = param (4);
    const float makeup = Decibels::decibelsToGain (param (5));
    const float mix = param (6) / 100.0f;
    const float atkCoeff = std::exp (-1.0f / jmax (1.0f, jmax (0.05f, param (2)) * 0.001f * sr));
    const float relCoeff = std::exp (-1.0f / jmax (1.0f, jmax (1.0f, param (3)) * 0.001f * sr));

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < chans; ++c) peak = jmax (peak, std::abs (b.getReadPointer (c)[i]));
        const float inDb = Decibels::gainToDecibels (peak, -100.0f);
        float over = inDb - thr;
        if (knee > 0.0f && over > -knee * 0.5f)
        {
            if (over < knee * 0.5f)
            {
                const float t = over + knee * 0.5f;
                over = (1.0f - 1.0f / ratio) * t * t / (2.0f * knee);
            }
            else over = (1.0f - 1.0f / ratio) * over;
        }
        else if (over <= 0.0f) over = 0.0f;
        const float targetGr = -over;
        const float coeff = targetGr > envDb ? atkCoeff : relCoeff;
        envDb = targetGr + coeff * (envDb - targetGr);
        const float g = Decibels::decibelsToGain (envDb) * makeup;
        for (int c = 0; c < chans; ++c)
        {
            auto* d = b.getWritePointer (c);
            d[i] = mix * (d[i] * g) + (1.0f - mix) * d[i];
        }
    }
    grDb.store (jmax (0.0f, -envDb));
    pushHistory (jmax (0.0f, -envDb));
}

//==============================================================================
//== SX 2A Compressor (opto / valve)
SxOptoCompressor::SxOptoCompressor() : SxEffectBase ("SX 2A Compressor", {
    { "peak", "Peak Reduction", 0.0f, 100.0f, 45.0f, " %" },
    { "gain", "Gain", -12.0f, 24.0f, 4.0f, " dB" },
    { "emphasis", "Detector HP", 0.0f, 100.0f, 35.0f, " %" },
    { "mix", "Dry", 0.0f, 100.0f, 100.0f, " %" },
    { "scgain", "SC Gain", -12.0f, 12.0f, 0.0f, " dB" },
    { "mode", "Comp / Limit", 0.0f, 1.0f, 0.0f, "" },
    { "inout", "In / Out", 0.0f, 1.0f, 1.0f, "" },
}, 0.0) {}

void SxOptoCompressor::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    envDb = releaseSlow = 0.0f;
    emphasisZ[0] = emphasisZ[1] = 0.0f;
}

void SxOptoCompressor::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float peakRed = param (0) / 100.0f;
    const float makeUp = Decibels::decibelsToGain (param (1));
    const float emphasis = param (2) / 100.0f;
    const float mix = param (6) > 0.5f ? jlimit (0.0f, 1.0f, param (3) / 100.0f) : 0.0f;
    const float scGain = Decibels::decibelsToGain (param (4));
    const bool limitMode = param (5) > 0.5f;
    const float atk = std::exp (-1.0f / jmax (1.0f, 0.010f * sr));
    const float relFast = std::exp (-1.0f / jmax (1.0f, 0.060f * sr));
    const float relSlow = std::exp (-1.0f / jmax (1.0f, 1.20f * sr));
    const float hfCoeff = onePoleCoeff (sampleRate, 2500.0f);
    const float thr = -6.0f - 26.0f * peakRed;
    const float ratio = limitMode ? 8.0f + 6.0f * peakRed : 2.5f + 2.5f * peakRed;

    for (int i = 0; i < n; ++i)
    {
        float det = 0.0f;
        for (int c = 0; c < chans; ++c)
        {
            const float x = b.getReadPointer (c)[i];
            emphasisZ[c] += hfCoeff * (x - emphasisZ[c]);
            det = jmax (det, std::abs (x + emphasis * emphasisZ[c] * 1.5f) * scGain);
        }
        const float inDb = Decibels::gainToDecibels (det, -90.0f);
        const float over = jmax (0.0f, inDb - thr);
        const float target = over * (1.0f - 1.0f / ratio);
        const float coeff = target > envDb ? atk : relFast;
        envDb = target + coeff * (envDb - target);
        releaseSlow = jmin (envDb, releaseSlow + (envDb - releaseSlow) * (1.0f - relSlow));
        const float g = Decibels::decibelsToGain (-(envDb * 0.62f + releaseSlow * 0.38f)) * makeUp;
        for (int c = 0; c < chans; ++c)
        {
            auto* d = b.getWritePointer (c);
            d[i] = mix * (d[i] * g) + (1.0f - mix) * d[i];
        }
    }
    grDb.store (jmax (0.0f, envDb));
    pushHistory (jmax (0.0f, envDb));
}

//==============================================================================
//== SX 76 Compressor (FET)
SxFetCompressor::SxFetCompressor() : SxEffectBase ("SX 76 Compressor", {
    { "input", "Input", -12.0f, 24.0f, 6.0f, " dB" },
    { "output", "Output", -12.0f, 24.0f, 0.0f, " dB" },
    { "attack", "Attack", 0.02f, 8.0f, 0.4f, " ms" },
    { "release", "Release", 40.0f, 1100.0f, 220.0f, " ms" },
    { "ratio", "Ratio", 4.0f, 20.0f, 8.0f, ":1" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxFetCompressor::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    envDb = 0.0f;
}

void SxFetCompressor::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float inGain = Decibels::decibelsToGain (param (0));
    const float outGain = Decibels::decibelsToGain (param (1));
    const float atkCoeff = std::exp (-1.0f / jmax (1.0f, jmax (0.02f, param (2)) * 0.001f * sr));
    const float relCoeff = std::exp (-1.0f / jmax (1.0f, jmax (20.0f, param (3)) * 0.001f * sr));
    const float ratio = jmax (1.0f, param (4));
    const float mix = jlimit (0.0f, 1.0f, param (5) / 100.0f);

    float outPeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < chans; ++c) peak = jmax (peak, std::abs (b.getReadPointer (c)[i] * inGain));
        const float inDb = Decibels::gainToDecibels (peak, -100.0f);
        const float over = jmax (0.0f, inDb + 18.0f);
        const float target = over * (1.0f - 1.0f / ratio);
        const float coeff = target > envDb ? atkCoeff : relCoeff;
        envDb = target + coeff * (envDb - target);
        const float g = Decibels::decibelsToGain (-envDb) * outGain;
        for (int c = 0; c < chans; ++c)
        {
            auto* d = b.getWritePointer (c);
            d[i] = mix * (d[i] * inGain * g) + (1.0f - mix) * d[i];
            outPeak = jmax (outPeak, std::abs (d[i]));
        }
    }
    grDb.store (jmax (0.0f, envDb));
    outDb.store (Decibels::gainToDecibels (outPeak, -100.0f));
    pushHistory (jmax (0.0f, envDb));
}

//==============================================================================
//== SX Convolver
SxConvolver::SxConvolver() : SxEffectBase ("SX Convolver", {
    { "size", "Size", 0.2f, 8.0f, 1.8f, " s" },
    { "predelay", "Pre-delay", 0.0f, 200.0f, 20.0f, " ms" },
    { "damp", "Damping", 500.0f, 16000.0f, 6000.0f, " Hz" },
    { "width", "Width", 0.0f, 200.0f, 100.0f, " %" },
    { "mix", "Mix", 0.0f, 100.0f, 35.0f, " %" },
}, 8.0)
{
    startTimerHz (4);
}

SxConvolver::~SxConvolver() { stopTimer(); }

void SxConvolver::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    dsp::ProcessSpec spec { sr, (uint32) jmax (32, block), 2 };
    conv.prepare (spec);
    preSize = (int) (sr * 0.25);
    predelay.setSize (2, preSize);
    predelay.clear();
    preWrite = 0;
    dryCopy.setSize (2, jmax (32, block));
    rebuildIr();
    lastSize = param (0);
    lastDamp = param (2);
    lastPre = (int) param (1);
}

void SxConvolver::rebuildIr()
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int len = jlimit (64, (int) (sr * 8.0), (int) (param (0) * sr));
    AudioBuffer<float> ir (2, len);
    Random rng (0x5eed);
    const float a = onePoleCoeff (sr, param (2));
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = ir.getWritePointer (ch);
        float lp = 0.0f;
        for (int i = 0; i < len; ++i)
        {
            const float t = (float) i / (float) len;
            const float noise = rng.nextFloat() * 2.0f - 1.0f;
            lp += a * (noise - lp);
            const float env = std::pow (1.0f - t, 2.2f) * std::exp (-3.0f * t);
            d[i] = lp * env;
        }
        const int ramp = jmin (len, (int) (sr * 0.004));
        for (int i = 0; i < ramp; ++i) d[i] *= (float) i / (float) jmax (1, ramp);
    }
    conv.loadImpulseResponse (std::move (ir), sr, dsp::Convolution::Stereo::yes,
                              dsp::Convolution::Trim::no, dsp::Convolution::Normalise::yes);
}

void SxConvolver::timerCallback()
{
    if (param (0) == lastSize && param (2) == lastDamp) return;
    lastSize = param (0);
    lastDamp = param (2);
    rebuildIr();
}

void SxConvolver::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    dryCopy.setSize (2, n, true, false, true);
    for (int c = 0; c < chans; ++c) dryCopy.copyFrom (c, 0, b, c, 0, n);

    const int preSamples = jlimit (0, jmax (0, preSize - 1), (int) (sampleRate * param (1) / 1000.0));
    for (int c = 0; c < chans; ++c)
    {
        auto* d = b.getWritePointer (c);
        auto* pd = predelay.getWritePointer (c);
        int w = preWrite;
        for (int i = 0; i < n; ++i)
        {
            pd[w] = d[i];
            const int r = (w - preSamples + preSize * 2) % preSize;
            d[i] = pd[r];
            w = (w + 1) % preSize;
        }
    }
    preWrite = (preWrite + n) % preSize;

    dsp::AudioBlock<float> block (b);
    dsp::ProcessContextReplacing<float> ctx (block);
    conv.process (ctx);

    const float width = param (3) / 100.0f;
    const float mix = jlimit (0.0f, 1.0f, param (4) / 100.0f);
    for (int i = 0; i < n; ++i)
    {
        const float wl = b.getReadPointer (0)[i];
        const float wr = chans > 1 ? b.getReadPointer (1)[i] : wl;
        const float mid = (wl + wr) * 0.5f, side = (wl - wr) * 0.5f * width;
        const float wetL = mid + side, wetR = mid - side;
        for (int c = 0; c < chans; ++c)
        {
            auto* d = b.getWritePointer (c);
            const float wet = c == 0 ? wetL : wetR;
            d[i] = mix * wet + (1.0f - mix) * dryCopy.getReadPointer (c)[i];
        }
    }
    pushHistory (b.getMagnitude (0, 0, n));
    pushStereo (b.getMagnitude (0, 0, n), chans > 1 ? b.getMagnitude (1, 0, n) : b.getMagnitude (0, 0, n));
}

//==============================================================================
//== SX Analog Reverb
SxAnalogReverb::SxAnalogReverb() : SxEffectBase ("SX Analog Reverb", {
    { "size", "Room Size", 0.0f, 100.0f, 55.0f, " %" },
    { "damp", "Damping", 0.0f, 100.0f, 45.0f, " %" },
    { "width", "Width", 0.0f, 100.0f, 100.0f, " %" },
    { "predelay", "Pre-delay", 0.0f, 200.0f, 15.0f, " ms" },
    { "mix", "Mix", 0.0f, 100.0f, 30.0f, " %" },
}, 6.0) {}

void SxAnalogReverb::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    juce::Reverb::Parameters p;
    p.roomSize = param (0) / 100.0f;
    p.damping = param (1) / 100.0f;
    p.width = param (2) / 100.0f;
    p.wetLevel = param (4) / 100.0f;
    p.dryLevel = 1.0f - param (4) / 100.0f;
    reverb.setParameters (p);
    preSize = (int) (sr * 0.25);
    predelay.setSize (2, preSize);
    predelay.clear();
    preWrite = 0;
    work.setSize (2, jmax (32, block));
}

void SxAnalogReverb::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    if (work.getNumSamples() < n) work.setSize (2, n, true, false, true);

    juce::Reverb::Parameters p;
    p.roomSize = jlimit (0.0f, 1.0f, param (0) / 100.0f);
    p.damping = jlimit (0.0f, 1.0f, param (1) / 100.0f);
    p.width = jlimit (0.0f, 1.0f, param (2) / 100.0f);
    p.wetLevel = jlimit (0.0f, 1.0f, param (4) / 100.0f);
    p.dryLevel = 1.0f - p.wetLevel;
    reverb.setParameters (p);

    const int preSamples = jlimit (0, jmax (0, preSize - 1), (int) (sampleRate * param (3) / 1000.0));
    for (int c = 0; c < chans; ++c)
    {
        auto* src = b.getReadPointer (c);
        auto* dst = work.getWritePointer (c);
        auto* pd = predelay.getWritePointer (c);
        int w = preWrite;
        for (int i = 0; i < n; ++i)
        {
            pd[w] = src[i];
            const int r = (w - preSamples + preSize * 2) % preSize;
            dst[i] = pd[r];
            w = (w + 1) % preSize;
        }
    }
    preWrite = (preWrite + n) % preSize;

    if (chans > 1) reverb.processStereo (work.getWritePointer (0), work.getWritePointer (1), n);
    else reverb.processMono (work.getWritePointer (0), n);

    for (int c = 0; c < chans; ++c) b.copyFrom (c, 0, work, c, 0, n);
    pushHistory (b.getMagnitude (0, 0, n));
    pushStereo (b.getMagnitude (0, 0, n), chans > 1 ? b.getMagnitude (1, 0, n) : b.getMagnitude (0, 0, n));
}

//==============================================================================
//== SX Digital Reverb (FDN)
SxDigitalReverb::SxDigitalReverb() : SxEffectBase ("SX Digital Reverb", {
    { "size", "Size", 0.0f, 100.0f, 60.0f, " %" },
    { "decay", "Decay", 0.0f, 100.0f, 55.0f, " %" },
    { "damp", "Damping", 0.0f, 100.0f, 40.0f, " %" },
    { "predelay", "Pre-delay", 0.0f, 200.0f, 12.0f, " ms" },
    { "mix", "Mix", 0.0f, 100.0f, 30.0f, " %" },
}, 8.0) {}

void SxDigitalReverb::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    const int base[kLines] { 1451, 1789, 2131, 2539 };
    const float scale = 0.5f + param (0) / 100.0f;
    for (int i = 0; i < kLines; ++i)
    {
        sizes[i] = jmax (64, (int) ((float) base[i] * scale * (float) sr / 44100.0f));
        lines[i].assign ((size_t) sizes[i], 0.0f);
        write[i] = 0;
    }
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < kLines; ++i) damp[c][i] = 0.0f;
    preSize = (int) (sr * 0.25);
    predelay.setSize (2, preSize);
    predelay.clear();
    preWrite = 0;
    work.setSize (2, jmax (32, block));
}

void SxDigitalReverb::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    if (work.getNumSamples() < n) work.setSize (2, n, true, false, true);

    const float feedback = jlimit (0.0f, 0.98f, 0.35f + 0.63f * param (1) / 100.0f);
    const float dampCoeff = jlimit (0.0f, 0.95f, param (2) / 100.0f);
    const float mix = jlimit (0.0f, 1.0f, param (4) / 100.0f);
    const int preSamples = jlimit (0, jmax (0, preSize - 1), (int) (sampleRate * param (3) / 1000.0));
    float tailPeak = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < chans; ++c)
        {
            auto* pd = predelay.getWritePointer (c);
            pd[preWrite] = b.getReadPointer (c)[i];
            const int r = (preWrite - preSamples + preSize * 2) % preSize;
            work.getWritePointer (c)[i] = pd[r];
        }
        preWrite = (preWrite + 1) % preSize;
        const float inMono = 0.5f * (work.getReadPointer (0)[i] + (chans > 1 ? work.getReadPointer (1)[i] : work.getReadPointer (0)[i]));

        float taps[kLines];
        for (int k = 0; k < kLines; ++k)
        {
            taps[k] = lines[k][(size_t) write[k]];
            write[k] = (write[k] + 1) % sizes[k];
        }
        const float a = taps[0], bb = taps[1], c2 = taps[2], d = taps[3];
        float m[4] { (a + bb + c2 + d) * 0.5f, (a - bb + c2 - d) * 0.5f,
                     (a + bb - c2 - d) * 0.5f, (a - bb - c2 + d) * 0.5f };
        for (int k = 0; k < kLines; ++k)
        {
            float v = m[k] * feedback + inMono * (k == 0 ? 0.5f : 0.35f);
            damp[0][k] += (1.0f - dampCoeff) * (v - damp[0][k]);
            lines[k][(size_t) write[k]] = damp[0][k];
        }
        const float wetL = m[0] + m[2] * 0.7f;
        const float wetR = m[1] + m[3] * 0.7f;
        for (int c = 0; c < chans; ++c)
        {
            auto* dst = b.getWritePointer (c);
            const float wet = c == 0 ? wetL : wetR;
            dst[i] = mix * wet + (1.0f - mix) * work.getReadPointer (c)[i];
        }
        tailPeak = jmax (tailPeak, jmax (std::abs (wetL), std::abs (wetR)));
    }
    pushHistory (tailPeak);
    pushStereo (b.getMagnitude (0, 0, n), chans > 1 ? b.getMagnitude (1, 0, n) : b.getMagnitude (0, 0, n));
}

//==============================================================================
//== SX Limiter
SxLimiter::SxLimiter() : SxEffectBase ("SX Limiter", {
    { "ceiling", "Ceiling", -12.0f, 0.0f, -0.3f, " dB" },
    { "release", "Release", 10.0f, 500.0f, 80.0f, " ms" },
    { "look", "Look-ahead", 0.0f, 10.0f, 2.0f, " ms" },
    { "input", "Input", 0.0f, 18.0f, 0.0f, " dB" },
    { "style", "Style", 0.0f, 3.0f, 0.0f, "" },      // 0 transparent, 1 punchy, 2 warm, 3 clip
}, 0.5) {}

void SxLimiter::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    const int lookSamples = jlimit (0, 4096, (int) (sr * param (2) / 1000.0));
    lookSize = jmax (lookSamples + jmax (32, block) + 16, (int) (sr * 0.02));
    look.setSize (2, lookSize);
    look.clear();
    write = 0;
    envDb = 0.0f;
    appliedLookahead = lookSamples;
    setLatencySamples (lookSamples);
}

void SxLimiter::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const int lookSamples = jlimit (0, 4096, (int) (sampleRate * param (2) / 1000.0));
    if (lookSamples != appliedLookahead)
    {
        appliedLookahead = lookSamples;
        setLatencySamples (lookSamples);
    }
    const float ceiling = Decibels::decibelsToGain (param (0));
    const float inGain = Decibels::decibelsToGain (param (3));
    const int style = jlimit (0, 3, (int) std::round (param (4)));
    // each style changes the knee, the release feel and how the transient is handled
    const float relMs = param (1) * (style == 1 ? 0.55f : style == 2 ? 1.6f : 1.0f);
    const float relCoeff = std::exp (-1.0f / jmax (1.0f, jmax (1.0f, relMs) * 0.001f * (float) sampleRate));
    const float knee = style == 1 ? 0.30f : (style == 2 ? 0.75f : 0.55f);
    float maxGr = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < chans; ++c)
            peak = jmax (peak, std::abs (b.getReadPointer (c)[i] * inGain));
        float overDb = jmax (0.0f, Decibels::gainToDecibels (peak, -100.0f) - param (0));
        // soft knee on the transparent / warm styles, hard on punchy / clip
        if (overDb < knee * 6.0f) overDb *= (overDb / (knee * 6.0f)) * 0.85f + 0.15f;
        envDb = overDb > envDb ? overDb : overDb + relCoeff * (envDb - overDb);
        maxGr = jmax (maxGr, envDb);
        const float g = Decibels::decibelsToGain (-envDb);

        for (int c = 0; c < chans; ++c)
        {
            auto* ringBuf = look.getWritePointer (c);
            const float delayed = ringBuf[(write - lookSamples + lookSize * 2) % lookSize];
            ringBuf[write] = b.getReadPointer (c)[i] * inGain;
            b.getWritePointer (c)[i] = jlimit (-ceiling, ceiling, delayed * g);
        }
        write = (write + 1) % lookSize;
    }
    grDb.store (maxGr);
    pushHistory (maxGr);
}


//==============================================================================
//== SX Chorus
SxChorus::SxChorus() : SxEffectBase ("SX Chorus", {
    { "rate", "Rate", 0.05f, 5.0f, 0.6f, " Hz" },
    { "depth", "Depth", 0.0f, 100.0f, 45.0f, " %" },
    { "delay", "Delay", 5.0f, 40.0f, 12.0f, " ms" },
    { "spread", "Spread", 0.0f, 180.0f, 90.0f, " deg" },
    { "mix", "Mix", 0.0f, 100.0f, 45.0f, " %" },
}, 0.0) {}

void SxChorus::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    size = (int) (sr * 0.08);
    for (int c = 0; c < 2; ++c) ring[c].assign ((size_t) size, 0.0f);
    write = 0;
    phase = 0.0f;
}

void SxChorus::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float rate = param (0), depth = param (1) / 100.0f, baseMs = param (2);
    const float spread = MathConstants<float>::pi / 180.0f * param (3);
    const float mix = jlimit (0.0f, 1.0f, param (4) / 100.0f);
    const float inc = rate / sr;

    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < chans; ++c)
        {
            const float x = b.getReadPointer (c)[i];
            ring[c][(size_t) write] = x;
            const float lfo = std::sin (MathConstants<float>::twoPi * (phase + (c == 1 ? spread / MathConstants<float>::twoPi : 0.0f)));
            const float ms = baseMs + depth * 6.0f * lfo;
            const float delaySamples = jlimit (1.0f, (float) size - 2.0f, ms * sr / 1000.0f);
            const int i0 = (int) delaySamples;
            const float frac = delaySamples - (float) i0;
            const int r0 = (write - i0 + size * 2) % size;
            const int r1 = (r0 - 1 + size) % size;
            const float wet = ring[c][(size_t) r0] * (1.0f - frac) + ring[c][(size_t) r1] * frac;
            b.getWritePointer (c)[i] = x * (1.0f - mix) + wet * mix;
        }
        write = (write + 1) % size;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    setLfoPhase (phase);
}

//==============================================================================
//== SX Delay (with tempo sync and ping-pong)
SxDelay::SxDelay() : SxEffectBase ("SX Delay", {
    { "time", "Time", 20.0f, 2000.0f, 350.0f, " ms" },
    { "fb", "Feedback", 0.0f, 95.0f, 35.0f, " %" },
    { "tone", "Tone", 500.0f, 12000.0f, 6000.0f, " Hz" },
    { "sync", "Sync", 0.0f, 1.0f, 0.0f, "" },
    { "note", "Note", 0.0f, 10.0f, 2.0f, "" },
    { "pingpong", "Ping-Pong", 0.0f, 1.0f, 0.0f, "" },
    { "mix", "Mix", 0.0f, 100.0f, 30.0f, " %" },
}, 4.0) {}

void SxDelay::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    size = (int) (sr * 2.1);
    for (int c = 0; c < 2; ++c) ring[c].assign ((size_t) size, 0.0f);
    write = 0;
    lp[0] = lp[1] = 0.0f;
}

void SxDelay::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;

    // tempo sync: the note value is a multiplier of a beat, so the delay follows the project tempo
    double ms = param (0);
    if (param (3) > 0.5)
    {
        static const double beats[11] { 2.0, 1.0, 0.5, 0.25, 0.125,   // 1/2 .. 1/32
                                        1.5, 0.75, 0.375,               // dotted 1/4, 1/8, 1/16
                                        2.0 / 3.0, 0.5 / 3.0 * 2.0, 0.25 / 3.0 * 2.0 };  // triplets
        const int idx = jlimit (0, 10, (int) std::round (param (4)));
        double bpm = 120.0;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
                if (auto t = pos->getBpm()) bpm = *t;
        ms = beats[idx] * 60000.0 / jmax (20.0, bpm);
    }
    const int dSamples = jlimit (1, size - 2, (int) (ms * sr / 1000.0f));
    const float fb = jlimit (0.0f, 0.95f, param (1) / 100.0f);
    const float toneA = onePoleCoeff (sampleRate, param (2));
    const bool pingpong = param (5) > 0.5f;
    const float mix = jlimit (0.0f, 1.0f, param (6) / 100.0f);

    for (int i = 0; i < n; ++i)
    {
        const int rp = (write - dSamples + size * 2) % size;
        for (int c = 0; c < chans; ++c)
        {
            const float x = b.getReadPointer (c)[i];
            float dl = ring[c][(size_t) rp];
            lp[c] += toneA * (dl - lp[c]);
            dl = lp[c];
            const int other = chans > 1 ? (c == 0 ? 1 : 0) : c;
            const float fbSrc = pingpong ? ring[other][(size_t) rp] : dl;
            ring[c][(size_t) write] = x + fbSrc * fb;
            b.getWritePointer (c)[i] = x * (1.0f - mix) + dl * mix;
        }
        write = (write + 1) % size;
    }
    pushHistory (jmax (b.getMagnitude (0, 0, n), chans > 1 ? b.getMagnitude (1, 0, n) : 0.0f));
}

//==============================================================================
//== SX Distortion
SxDistortion::SxDistortion() : SxEffectBase ("SX Distortion", {
    { "drive", "Drive", 0.0f, 40.0f, 12.0f, " dB" },
    { "tone", "Tone", 500.0f, 12000.0f, 5000.0f, " Hz" },
    { "output", "Output", -24.0f, 12.0f, 0.0f, " dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxDistortion::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    toneZ[0] = toneZ[1] = 0.0f;
}

void SxDistortion::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float drive = Decibels::decibelsToGain (param (0));
    const float out = Decibels::decibelsToGain (param (2));
    const float mix = jlimit (0.0f, 1.0f, param (3) / 100.0f);
    const float a = onePoleCoeff (sampleRate, param (1));

    for (int c = 0; c < chans; ++c)
    {
        auto* d = b.getWritePointer (c);
        for (int i = 0; i < n; ++i)
        {
            const float x = d[i];
            const float driven = x * drive;
            const float shaped = std::tanh (driven + 0.15f * driven * driven) - 0.06f;
            toneZ[c] += a * (shaped - toneZ[c]);
            const float wet = std::tanh (toneZ[c] * 1.1f) * out;
            d[i] = mix * wet + (1.0f - mix) * x;
        }
    }
    pushHistory (b.getMagnitude (0, 0, n));
}

//==============================================================================
//== SX Tremolo
SxTremolo::SxTremolo() : SxEffectBase ("SX Tremolo", {
    { "rate", "Rate", 0.1f, 12.0f, 4.5f, " Hz" },
    { "depth", "Depth", 0.0f, 100.0f, 60.0f, " %" },
    { "phase", "Stereo Phase", 0.0f, 180.0f, 0.0f, " deg" },
    { "shape", "Shape", 0.0f, 1.0f, 0.0f, "" },
}, 0.0) {}

void SxTremolo::prepareToPlay (double sr, int) { sampleRate = sr; phase = 0.0f; }

void SxTremolo::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float inc = param (0) / (float) sampleRate;
    const float depth = param (1) / 100.0f;
    const float phaseOff = param (2) / 360.0f;
    const bool square = param (3) > 0.5f;

    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < chans; ++c)
        {
            float p = phase + (c == 1 ? phaseOff : 0.0f);
            p -= std::floor (p);
            float mod = std::sin (MathConstants<float>::twoPi * p);
            if (square) mod = mod >= 0.0f ? 1.0f : -1.0f;
            b.getWritePointer (c)[i] *= 1.0f - depth * (0.5f - 0.5f * mod);
        }
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    setLfoPhase (phase);
    pushHistory (b.getMagnitude (0, 0, n));
}

//==============================================================================
//== SX Flanger
SxFlanger::SxFlanger() : SxEffectBase ("SX Flanger", {
    { "rate", "Rate", 0.05f, 5.0f, 0.35f, " Hz" },
    { "depth", "Depth", 0.0f, 100.0f, 60.0f, " %" },
    { "fb", "Feedback", -95.0f, 95.0f, 40.0f, " %" },
    { "delay", "Delay", 0.5f, 10.0f, 2.5f, " ms" },
    { "mix", "Mix", 0.0f, 100.0f, 50.0f, " %" },
}, 0.0) {}

void SxFlanger::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    size = (int) (sr * 0.05);
    for (int c = 0; c < 2; ++c) ring[c].assign ((size_t) size, 0.0f);
    write = 0;
    phase = 0.0f;
}

void SxFlanger::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float inc = param (0) / sr;
    const float depth = param (1) / 100.0f;
    const float fb = jlimit (-0.95f, 0.95f, param (2) / 100.0f);
    const float baseMs = param (3);
    const float mix = jlimit (0.0f, 1.0f, param (4) / 100.0f);

    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < chans; ++c)
        {
            const float x = b.getReadPointer (c)[i];
            const float lfo = std::sin (MathConstants<float>::twoPi * (phase + (c == 1 ? 0.25f : 0.0f)));
            const float ms = baseMs * (1.0f + depth * 3.0f * (0.5f + 0.5f * lfo));
            const float delaySamples = jlimit (1.0f, (float) size - 2.0f, ms * sr / 1000.0f);
            const int i0 = (int) delaySamples;
            const float frac = delaySamples - (float) i0;
            const int r0 = (write - i0 + size * 2) % size;
            const int r1 = (r0 - 1 + size) % size;
            const float wet = ring[c][(size_t) r0] * (1.0f - frac) + ring[c][(size_t) r1] * frac;
            ring[c][(size_t) write] = x + wet * fb;
            b.getWritePointer (c)[i] = x * (1.0f - mix) + wet * mix;
        }
        write = (write + 1) % size;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    setLfoPhase (phase);
}

//==============================================================================
//== SX Phaser
SxPhaser::SxPhaser() : SxEffectBase ("SX Phaser", {
    { "rate", "Rate", 0.05f, 5.0f, 0.4f, " Hz" },
    { "depth", "Depth", 0.0f, 100.0f, 70.0f, " %" },
    { "stages", "Stages", 2.0f, 8.0f, 4.0f, "" },
    { "fb", "Feedback", 0.0f, 95.0f, 45.0f, " %" },
    { "mix", "Mix", 0.0f, 100.0f, 50.0f, " %" },
}, 0.0) {}

void SxPhaser::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    for (int c = 0; c < 2; ++c)
    {
        for (int s = 0; s < 8; ++s) { apX1[c][s] = 0.0f; apY1[c][s] = 0.0f; }
        fb[c] = 0.0f;
    }
    phase = 0.0f;
}

void SxPhaser::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float inc = param (0) / sr;
    const float depth = param (1) / 100.0f;
    const int stages = jlimit (2, 8, (int) std::round (param (2)));
    const float fbAmt = jlimit (0.0f, 0.95f, param (3) / 100.0f);
    const float mix = jlimit (0.0f, 1.0f, param (4) / 100.0f);

    for (int i = 0; i < n; ++i)
    {
        const float lfo = 0.5f + 0.5f * std::sin (MathConstants<float>::twoPi * phase);
        const float hz = 200.0f * std::pow (20.0f, depth * lfo);
        const float t = std::tan (MathConstants<float>::pi * jlimit (40.0f, sr * 0.45f, hz) / sr);
        const float a = (t - 1.0f) / (t + 1.0f);
        for (int c = 0; c < chans; ++c)
        {
            const float x = b.getReadPointer (c)[i] + fb[c] * fbAmt;
            float y = x;
            for (int s = 0; s < stages; ++s)
            {
                const float out = a * y + apX1[c][s] - a * apY1[c][s];
                apX1[c][s] = y;
                apY1[c][s] = out;
                y = out;
            }
            fb[c] = jlimit (-1.0f, 1.0f, y);
            b.getWritePointer (c)[i] = x * (1.0f - mix) + y * mix;
        }
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    setLfoPhase (phase);
}

//==============================================================================
//== SX Meter: passes the audio through and publishes level + stereo field data
SxMeterEffect::SxMeterEffect() : SxEffectBase ("SX Meter", {
    { "hold", "Hold", 0.0f, 100.0f, 50.0f, " %" },
    { "scale", "Scale", -60.0f, -12.0f, -60.0f, " dB" },
}, 0.0)
{
    peakVals[0].store (0.0f); peakVals[1].store (0.0f);
    rmsVals[0].store (0.0f); rmsVals[1].store (0.0f);
}

void SxMeterEffect::prepareToPlay (double, int) {}

void SxMeterEffect::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float hold = jlimit (0.0f, 0.99f, param (0) / 100.0f);
    for (int c = 0; c < chans; ++c)
    {
        const float pk = b.getMagnitude (c, 0, n);
        peakVals[(size_t) c].store (jmax (pk, peakVals[(size_t) c].load() * hold));
        double sum = 0.0;
        const auto* d = b.getReadPointer (c);
        for (int i = 0; i < n; ++i) sum += (double) d[i] * d[i];
        rmsVals[(size_t) c].store ((float) std::sqrt (sum / jmax (1, n)));
    }
    if (chans > 1)
    {
        const auto* l = b.getReadPointer (0);
        const auto* r = b.getReadPointer (1);
        double sll = 0.0, srr = 0.0, slr = 0.0;
        for (int i = 0; i < n; ++i) { sll += (double) l[i] * l[i]; srr += (double) r[i] * r[i]; slr += (double) l[i] * r[i]; }
        const double den = std::sqrt (sll * srr) + 1.0e-12;
        corr.store ((float) jlimit (-1.0, 1.0, slr / den));
        const float sum = (float) (std::sqrt (sll) + std::sqrt (srr));
        bal.store (sum < 1.0e-6f ? 0.0f : (float) ((std::sqrt (srr) - std::sqrt (sll)) / sum));
    }
    pushHistory (peakVals[0].load());
    pushStereo (peakVals[0].load(), chans > 1 ? peakVals[1].load() : peakVals[0].load());
}

//== doubler ---------------------------------------------------------------------------------------
SxDoubler::SxDoubler() : SxEffectBase ("SX Doubler", {
    { "voices", "Voices", 2.0f, 4.0f, 4.0f, "" },
    { "spread", "Spread", 0.0f, 100.0f, 60.0f, " %" },
    { "detune", "Detune", 0.0f, 100.0f, 35.0f, " %" },
    { "mix", "Mix", 0.0f, 100.0f, 60.0f, " %" },
    { "level", "Level", -24.0f, 12.0f, 0.0f, " dB" },
}, 0.0)
{
    voices[0] = { 0.0f, 0.0f, 0.011f, 0.013f, 0.0f, 0.7f };
    voices[1] = { 0.0f, 0.0f, 0.017f, 0.019f, 1.7f, 0.5f };
    voices[2] = { 0.0f, 0.0f, 0.023f, 0.029f, 3.1f, 0.9f };
    voices[3] = { 0.0f, 0.0f, 0.031f, 0.037f, 4.6f, 0.6f };
}

void SxDoubler::prepareToPlay (double sr, int)
{
    const int n = jmax (1024, (int) (sr * 0.12));
    bufL.assign ((size_t) n, 0.0f);
    bufR.assign ((size_t) n, 0.0f);
    writePos = 0;
    feedbackZ[0] = feedbackZ[1] = 0.0f;
}

void SxDoubler::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int voicesOn = jlimit (2, 4, (int) std::round (param (0)));
    const float spread = param (1) / 100.0f;
    const float detune = param (2) / 100.0f;
    const float mix = param (3) / 100.0f;
    const float level = Decibels::decibelsToGain (param (4));

    auto* l = b.getWritePointer (0);
    auto* r = chans > 1 ? b.getWritePointer (1) : b.getWritePointer (0);
    const int size = (int) bufL.size();

    for (int i = 0; i < n; ++i)
    {
        const float dryL = l[i], dryR = r[i];
        float wetL = 0.0f, wetR = 0.0f;
        bufL[(size_t) writePos] = dryL;
        bufR[(size_t) writePos] = dryR;

        for (int v = 0; v < voicesOn; ++v)
        {
            auto& vo = voices[(size_t) v];
            vo.phase += (float) (0.25 * (double) vo.rate * detune / sr) * MathConstants<float>::twoPi;
            if (vo.phase > MathConstants<float>::twoPi) vo.phase -= MathConstants<float>::twoPi;
            const float drift = std::sin (vo.phase) * 0.0012f * detune;
            const float dL = jlimit (0.0005f, 0.09f, vo.baseL * (1.0f + spread * 0.5f) + drift);
            const float dR = jlimit (0.0005f, 0.09f, vo.baseR * (1.0f + spread * 0.5f) + drift);
            const int iL = (writePos - jmax (1, (int) (dL * (float) sr)) + size) % size;
            const int iR = (writePos - jmax (1, (int) (dR * (float) sr)) + size) % size;
            wetL += bufL[(size_t) iL];
            wetR += bufR[(size_t) iR];
        }
        wetL /= (float) voicesOn;
        wetR /= (float) voicesOn;

        l[i] = (dryL * (1.0f - mix) + wetL * mix) * level;
        if (chans > 1) r[i] = (dryR * (1.0f - mix) + wetR * mix) * level;
        writePos = (writePos + 1) % size;
    }
    pushHistory (b.getMagnitude (0, 0, n));
}

//== tape machine ----------------------------------------------------------------------------------
SxTape::SxTape() : SxEffectBase ("SX Tape", {
    { "drive", "Drive", 0.0f, 100.0f, 45.0f, " %" },
    { "bump", "Head Bump", 0.0f, 100.0f, 40.0f, " %" },
    { "wow", "Wow", 0.0f, 100.0f, 25.0f, " %" },
    { "flutter", "Flutter", 0.0f, 100.0f, 20.0f, " %" },
    { "hiss", "Hiss", 0.0f, 100.0f, 12.0f, " %" },
    { "tone", "Tone", 0.0f, 100.0f, 55.0f, " %" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxTape::prepareToPlay (double sr, int)
{
    const int n = jmax (1024, (int) (sr * 0.05));
    for (auto& l : line) l.assign ((size_t) n, 0.0f);
    writePos = 0;
    wowPhase = flutterPhase = 0.0f;
    std::fill (std::begin (lpZ), std::end (lpZ), 0.0f);
    std::fill (std::begin (bumpZ), std::end (bumpZ), 0.0f);
    std::fill (std::begin (hissZ), std::end (hissZ), 0.0f);
}

void SxTape::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const float drive = Decibels::decibelsToGain (param (0) * 0.18f);
    const float bump = param (1) / 100.0f;
    const float wowAmt = param (2) / 100.0f;
    const float flutAmt = param (3) / 100.0f;
    const float hiss = param (4) / 100.0f;
    const float tone = param (5) / 100.0f;
    const float mix = param (6) / 100.0f;
    const int size = (int) line[0].size();
    const float aLp = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * (3500.0f + tone * 9000.0f) / (float) sr);
    const float aBump = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * 90.0f / (float) sr);

    for (int i = 0; i < n; ++i)
    {
        wowPhase += (float) (0.7 / sr) * MathConstants<float>::twoPi;
        flutterPhase += (float) (5.3 / sr) * MathConstants<float>::twoPi;
        if (wowPhase > MathConstants<float>::twoPi) wowPhase -= MathConstants<float>::twoPi;
        if (flutterPhase > MathConstants<float>::twoPi) flutterPhase -= MathConstants<float>::twoPi;
        const float modMs = wowAmt * 2.6f * std::sin (wowPhase) + flutAmt * 0.35f * std::sin (flutterPhase);
        const int delay = jlimit (1, size - 1, (int) ((0.004f + modMs * 0.001f) * (float) sr));

        for (int ch = 0; ch < chans; ++ch)
        {
            const float dry = b.getSample (ch, i);
            auto& buf = line[(size_t) ch];
            buf[(size_t) writePos] = dry;
            const int rd = (writePos - delay + size) % size;
            float x = buf[(size_t) rd] * drive;
            // tape saturation: soft, slightly asymmetric
            x = std::tanh (x * 1.2f) / 1.2f + 0.04f * x * x;
            // head bump (low shelf) and HF loss
            bumpZ[ch] += aBump * (x - bumpZ[ch]);
            x += bumpZ[ch] * bump * 0.55f;
            lpZ[ch] += aLp * (x - lpZ[ch]);
            x = lpZ[ch];
            // hiss
            x += hiss * 0.006f * (rng.nextFloat() * 2.0f - 1.0f);
            b.setSample (ch, i, dry * (1.0f - mix) + x * mix);
        }
        writePos = (writePos + 1) % size;
    }
    pushHistory (b.getMagnitude (0, 0, n));
}

//== harmonic exciter ------------------------------------------------------------------------------
SxExciter::SxExciter() : SxEffectBase ("SX Exciter", {
    { "xlow", "Low / Mid", 60.0f, 900.0f, 220.0f, " Hz", 0.5f },
    { "xhigh", "Mid / High", 1200.0f, 9000.0f, 3200.0f, " Hz", 0.5f },
    { "low", "Low Amount", 0.0f, 100.0f, 0.0f, " %" },
    { "mid", "Mid Amount", 0.0f, 100.0f, 25.0f, " %" },
    { "high", "High Amount", 0.0f, 100.0f, 45.0f, " %" },
    { "drive", "Drive", 0.0f, 100.0f, 45.0f, " %" },
    { "tone", "Tone", 0.0f, 100.0f, 60.0f, " %" },
    { "mix", "Mix", 0.0f, 100.0f, 35.0f, " %" },
}, 0.0) {}

void SxExciter::prepareToPlay (double, int)
{
    for (int ch = 0; ch < 2; ++ch) { lowZ[ch] = midZ[ch] = toneZ[ch] = 0.0f; }
}

void SxExciter::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const float xLow = jlimit (60.0f, 900.0f, param (0));
    const float xHigh = jmax (xLow * 1.5f, jlimit (1200.0f, 9000.0f, param (1)));
    const float amt[3] { param (2) / 100.0f, param (3) / 100.0f, param (4) / 100.0f };
    const float drive = 1.0f + param (5) / 100.0f * 14.0f;
    const float tone = param (6) / 100.0f;
    const float mix = param (7) / 100.0f;
    const float aLow = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * xLow / (float) sr);
    const float aMid = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * xHigh / (float) sr);
    const float aTone = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * (3000.0f + tone * 12000.0f) / (float) sr);

    for (int ch = 0; ch < chans; ++ch)
    {
        auto* d = b.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const float dry = d[i];
            // three bands from two cascaded one pole crossovers
            lowZ[ch] += aLow * (dry - lowZ[ch]);
            const float low = lowZ[ch];
            const float rest = dry - low;
            midZ[ch] += aMid * (rest - midZ[ch]);
            const float mid = midZ[ch];
            const float high = rest - mid;

            float excite = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                if (amt[k] <= 0.0f) continue;
                const float x = k == 0 ? low : (k == 1 ? mid : high);
                excite += (std::tanh (x * drive) - x) * amt[k];
            }
            toneZ[ch] += aTone * (excite - toneZ[ch]);
            const float wet = toneZ[ch];
            d[i] = dry * (1.0f - mix) + (dry + wet) * mix;
        }
    }
    pushHistory (b.getMagnitude (0, 0, n));
}
//== saturation ------------------------------------------------------------------------------------
SxSaturator::SxSaturator() : SxEffectBase ("SX Saturator", {
    { "curve", "Curve", 0.0f, 3.0f, 0.0f, "" },
    { "drive", "Drive", 0.0f, 100.0f, 40.0f, " %" },
    { "tone", "Tone", 0.0f, 100.0f, 60.0f, " %" },
    { "out", "Output", -24.0f, 12.0f, 0.0f, " dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxSaturator::prepareToPlay (double, int)
{
    std::fill (std::begin (lpZ), std::end (lpZ), 0.0f);
    std::fill (std::begin (dcZ), std::end (dcZ), 0.0f);
}

void SxSaturator::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int curve = jlimit (0, 3, (int) std::round (param (0)));
    const float drive = 1.0f + param (1) / 100.0f * 20.0f;
    const float tone = param (2) / 100.0f;
    const float out = Decibels::decibelsToGain (param (3));
    const float mix = param (4) / 100.0f;
    const float aLp = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * (1800.0f + tone * 12000.0f) / (float) sr);
    const float aDc = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * 20.0f / (float) sr);

    for (int ch = 0; ch < chans; ++ch)
    {
        auto* d = b.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const float dry = d[i];
            float x = dry * drive;
            switch (curve)
            {
                case 1:  x = (1.0f + x) / (1.0f + std::abs (x)); break;                 // tube
                case 2:  x = std::tanh (x) + 0.15f * std::tanh (x * x * 0.5f); break;    // diode
                case 3:  x = std::sin (jlimit (-3.0f, 3.0f, x) * 0.8f); break;           // fold
                default: x = std::tanh (x * 0.9f) + 0.05f * x * x; break;                // tape
            }
            lpZ[ch] += aLp * (x - lpZ[ch]);
            x = lpZ[ch];
            dcZ[ch] += aDc * (x - dcZ[ch]);       // DC blocker
            x -= dcZ[ch];
            d[i] = (dry * (1.0f - mix) + x * mix) * out;
        }
    }
    pushHistory (b.getMagnitude (0, 0, n));
}

//== OTT style multiband ---------------------------------------------------------------------------
SxMultibandComp::SxMultibandComp() : SxEffectBase ("SX Multiband", {
    { "down", "Down Amount", 0.0f, 100.0f, 60.0f, " %" },
    { "up", "Up Amount", 0.0f, 100.0f, 40.0f, " %" },
    { "time", "Time", 0.0f, 100.0f, 45.0f, " %" },
    { "low", "Low", -12.0f, 12.0f, 0.0f, " dB" },
    { "mid", "Mid", -12.0f, 12.0f, 0.0f, " dB" },
    { "high", "High", -12.0f, 12.0f, 0.0f, " dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxMultibandComp::prepareToPlay (double, int)
{
    for (auto& b : bands) for (auto& s : b) { s.env = 0.0f; s.gr = 1.0f; }
    std::fill (std::begin (lp1), std::end (lp1), 0.0f);
    std::fill (std::begin (lp2), std::end (lp2), 0.0f);
    std::fill (std::begin (lp1b), std::end (lp1b), 0.0f);
    std::fill (std::begin (lp2b), std::end (lp2b), 0.0f);
}

void SxMultibandComp::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const float down = param (0) / 100.0f;
    const float up = param (1) / 100.0f;
    const float time = param (2) / 100.0f;
    const float trim[3] { Decibels::decibelsToGain (param (3)),
                          Decibels::decibelsToGain (param (4)),
                          Decibels::decibelsToGain (param (5)) };
    const float mix = param (6) / 100.0f;

    // linkwitz-ish two stage crossover at 250 Hz and 2.5 kHz
    const float aLow = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * 250.0f / (float) sr);
    const float aMid = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * 2500.0f / (float) sr);
    const float atk = 1.0f - std::exp (-1.0f / (0.010f * (float) sr));
    const float rel = 1.0f - std::exp (-1.0f / ((0.060f + time * 0.30f) * (float) sr));
    float worst = 0.0f;

    for (int ch = 0; ch < chans; ++ch)
    {
        auto* d = b.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const float dry = d[i];
            lp1[ch] += aLow * (dry - lp1[ch]);
            const float low = lp1[ch];
            const float rest = dry - low;
            lp2[ch] += aMid * (rest - lp2[ch]);
            const float mid = lp2[ch];
            const float high = rest - mid;
            const float bandVals[3] { low, mid, high };
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                auto& st = bands[k][ch];
                const float x = bandVals[k];
                const float level = std::abs (x);
                const float coef = level > st.env ? atk : rel;
                st.env += coef * (level - st.env);
                const float envDb = Decibels::gainToDecibels (st.env, -90.0f);
                // downward above -18 dB, upward below -36 dB, both scaled by their amounts
                const float over = jmax (0.0f, envDb + 18.0f);
                const float under = jmax (0.0f, -36.0f - envDb);
                float grDb = -over * 0.75f * down + under * 0.5f * up;
                grDb = jlimit (-24.0f, 18.0f, grDb);
                const float target = Decibels::decibelsToGain (grDb);
                st.gr += (target - st.gr) * (grDb < 0.0f ? atk : rel);
                sum += x * st.gr * trim[k];
                worst = jmin (worst, grDb);
            }
            d[i] = dry * (1.0f - mix) + sum * mix;
        }
    }
    grDb.store (-worst);
    pushHistory (b.getMagnitude (0, 0, n));
}

//== three voice chorus ----------------------------------------------------------------------------
SxChorusEnsemble::SxChorusEnsemble() : SxEffectBase ("SX Chorus Ensemble", {
    { "rate", "Rate", 0.05f, 4.0f, 0.45f, " Hz", 0.5f },
    { "depth", "Depth", 0.0f, 100.0f, 55.0f, " %" },
    { "spread", "Spread", 0.0f, 100.0f, 70.0f, " %" },
    { "tone", "Tone", 0.0f, 100.0f, 60.0f, " %" },
    { "mix", "Mix", 0.0f, 100.0f, 45.0f, " %" },
}, 0.0) {}

void SxChorusEnsemble::prepareToPlay (double sr, int)
{
    const int n = jmax (1024, (int) (sr * 0.06));
    for (auto& l : line) l.assign ((size_t) n, 0.0f);
    writePos = 0;
    phase[0] = 0.0f; phase[1] = 2.1f; phase[2] = 4.2f;
    toneZ[0] = toneZ[1] = 0.0f;
}

void SxChorusEnsemble::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const float rate = param (0);
    const float depth = param (1) / 100.0f;
    const float spreadMs = param (2) / 100.0f * 6.0f;
    const float tone = param (3) / 100.0f;
    const float mix = param (4) / 100.0f;
    const int size = (int) line[0].size();
    const float aTone = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * (2500.0f + tone * 9000.0f) / (float) sr);
    const float baseMs[3] { 11.0f, 17.0f, 23.0f };

    for (int i = 0; i < n; ++i)
    {
        for (int v = 0; v < 3; ++v)
        {
            phase[v] += (float) (rate * (1.0f + v * 0.13f) / sr) * MathConstants<float>::twoPi;
            if (phase[v] > MathConstants<float>::twoPi) phase[v] -= MathConstants<float>::twoPi;
        }
        for (int ch = 0; ch < chans; ++ch)
        {
            const float dry = b.getSample (ch, i);
            auto& buf = line[(size_t) ch];
            buf[(size_t) writePos] = dry;
            float wet = 0.0f;
            for (int v = 0; v < 3; ++v)
            {
                const float mod = std::sin (phase[v] + (ch == 1 ? spreadMs * 0.5f : 0.0f)) * depth * 3.0f;
                const int d = jlimit (1, size - 1, (int) ((baseMs[v] + mod) * 0.001f * (float) sr));
                wet += buf[(size_t) ((writePos - d + size) % size)];
            }
            wet /= 3.0f;
            toneZ[ch] += aTone * (wet - toneZ[ch]);
            wet = toneZ[ch];
            b.setSample (ch, i, dry * (1.0f - mix) + wet * mix);
        }
        writePos = (writePos + 1) % size;
    }
    pushHistory (b.getMagnitude (0, 0, n));
}

//== maximiser -------------------------------------------------------------------------------------
SxMaximizer::SxMaximizer() : SxEffectBase ("SX Maximizer", {
    { "gain", "Input Gain", -12.0f, 18.0f, 0.0f, " dB" },
    { "thr", "Threshold", -30.0f, 0.0f, -6.0f, " dB" },
    { "ceiling", "Ceiling", -6.0f, 0.0f, -0.3f, " dB" },
    { "release", "Release", 10.0f, 1000.0f, 180.0f, " ms", 0.6f },
    { "character", "Style", 0.0f, 3.0f, 0.0f, "" },   // 0 transparent, 1 punch, 2 warm, 3 clip
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, " %" },
}, 0.005) {}

void SxMaximizer::prepareToPlay (double sr, int)
{
    const int n = jmax (16, (int) (sr * 0.006));
    for (auto& l : look) l.assign ((size_t) n, 0.0f);
    lookPos = 0;
    releaseCoef = 1.0f - std::exp (-1.0f / (0.18f * (float) sr));
    gain = 1.0f;
}

void SxMaximizer::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const float inGain = Decibels::decibelsToGain (param (0));
    const float thr = Decibels::decibelsToGain (param (1));
    const float ceiling = Decibels::decibelsToGain (param (2));
    const int character = jlimit (0, 3, (int) std::round (param (4)));
    const float mix = param (5) / 100.0f;
    const int size = (int) look[0].size();
    releaseCoef = 1.0f - std::exp (-1.0f / ((param (3) * 0.001f) * (float) sampleRate + 1.0f));
    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < chans; ++ch) peak = jmax (peak, std::abs (b.getSample (ch, i) * inGain));
        // look-ahead peak hold so the gain is down before the transient arrives
        for (int ch = 0; ch < chans; ++ch) look[(size_t) ch][(size_t) lookPos] = b.getSample (ch, i) * inGain;
        float held = 0.0f;
        for (int k = 0; k < size; ++k)
            for (int ch = 0; ch < chans; ++ch)
                held = jmax (held, std::abs (look[(size_t) ch][(size_t) k]));
        const float target = held > thr ? thr / jmax (held, 1.0e-6f) : 1.0f;
        gain = target < gain ? target : gain + (target - gain) * releaseCoef;
        const float g = gain * (1.0f / jmax (inGain, 1.0e-6f));
        worst = jmin (worst, Decibels::gainToDecibels (gain, -60.0f));

        for (int ch = 0; ch < chans; ++ch)
        {
            const float dry = b.getSample (ch, i);
            float wet = look[(size_t) ch][(size_t) ((lookPos + 1) % size)] * gain;
            // character: soft knee / hard clip / tape-ish rounding
            if (character == 1 || character == 3) wet = jlimit (-ceiling, ceiling, wet);
            else if (character == 2) wet = std::tanh (wet / jmax (ceiling, 0.01f)) * ceiling;
            wet = jlimit (-ceiling, ceiling, wet);
            b.setSample (ch, i, dry * (1.0f - mix) + wet * mix);
        }
        lookPos = (lookPos + 1) % size;
    }
    grDb.store (-worst);

    const float outPeak = jmax (b.getMagnitude (0, 0, n), chans > 1 ? b.getMagnitude (1, 0, n) : 0.0f);
    lufs.store (Decibels::gainToDecibels (outPeak, -100.0f));
    pushHistory (outPeak);
}

//== tuner -----------------------------------------------------------------------------------------
SxTuner::SxTuner() : SxEffectBase ("SX Tuner", {
    { "ref", "Reference", 415.0f, 466.0f, 440.0f, " Hz" },
    { "mute", "Mute Output", 0.0f, 1.0f, 0.0f, "" },
}, 0.0) {}

void SxTuner::prepareToPlay (double, int)
{
    history.assign (4096, 0.0f);
    writePos = 0;
}

void SxTuner::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int size = (int) history.size();

    // keep the newest mono samples for the autocorrelation
    for (int i = 0; i < n; ++i)
    {
        float v = 0.0f;
        for (int ch = 0; ch < chans; ++ch) v += b.getSample (ch, i);
        history[(size_t) writePos] = v / jmax (1, chans);
        writePos = (writePos + 1) % size;
    }

    if (param (1) > 0.5f) b.clear();

    // autocorrelation over the last 2048 samples
    const int window = 2048;
    std::vector<float> x ((size_t) window);
    for (int i = 0; i < window; ++i)
        x[(size_t) i] = history[(size_t) ((writePos - window + i + size * 2) % size)];
    float energy = 0.0f;
    for (float v : x) energy += v * v;
    if (energy / (float) window < 1.0e-5f)
    {
        hasSignal.store (false);
        hz.store (0.0f);
        note.store (-1);
        centsOff.store (0.0f);
        pushHistory (0.0f);
        return;
    }
    const int minLag = (int) (sr / 1200.0), maxLag = jmin (window / 2, (int) (sr / 55.0));
    int bestLag = 0;
    float best = -1.0f;
    for (int lag = minLag; lag < maxLag; ++lag)
    {
        float sum = 0.0f;
        for (int i = 0; i < window - lag; ++i) sum += x[(size_t) i] * x[(size_t) (i + lag)];
        sum /= (float) (window - lag);
        if (sum > best) { best = sum; bestLag = lag; }
    }
    if (bestLag <= 0)
    {
        hasSignal.store (false);
        return;
    }
    const float hzFound = (float) (sr / (double) bestLag);
    const float ref = param (0);
    const double midi = 69.0 + 12.0 * std::log2 ((double) hzFound / (double) ref);
    const int nearest = (int) std::round (midi);
    hz.store (hzFound);
    note.store (nearest);
    centsOff.store ((float) ((midi - nearest) * 100.0));
    hasSignal.store (true);
    pushHistory (hzFound > 0.0f ? 1.0f : 0.0f);
}

//== clipper ---------------------------------------------------------------------------------------
SxClipper::SxClipper() : SxEffectBase ("SX Clipper", {
    { "clip", "Clip Point", -12.0f, 0.0f, -1.0f, " dB" },
    { "shape", "Shape", 0.0f, 2.0f, 0.0f, "" },          // 0 hard, 1 soft, 2 fold
    { "drive", "Drive", 0.0f, 24.0f, 6.0f, " dB" },
    { "os", "Oversampling", 1.0f, 3.0f, 2.0f, " x" },     // 1x, 2x, 4x
    { "tone", "Tone", 0.0f, 100.0f, 65.0f, " %" },
    { "out", "Output", -24.0f, 12.0f, 0.0f, " dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxClipper::prepareToPlay (double sr, int block)
{
    osFactor = 0;
    os.reset();
    std::fill (std::begin (lpZ), std::end (lpZ), 0.0f);
    juce::ignoreUnused (sr, block);
}

void SxClipper::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const float ceiling = Decibels::decibelsToGain (param (0));
    const int shape = jlimit (0, 2, (int) std::round (param (1)));
    const float drive = Decibels::decibelsToGain (param (2));
    const int factor = jlimit (1, 3, (int) std::round (param (3)));
    const float tone = param (4) / 100.0f;
    const float out = Decibels::decibelsToGain (param (5));
    const float mix = param (6) / 100.0f;

    // rebuild the oversampler when the factor changes
    if (osFactor != factor)
    {
        osFactor = factor;
        os.reset();
        if (factor > 1)
        {
            os = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) chans, factor - 1,
                                                                  juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
            os->initProcessing ((size_t) jmax (64, n));
        }
    }

    const float aLp = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * (1800.0f + tone * 14000.0f) / (float) sr);
    auto shaper = [&] (float x)
    {
        const float y = x * drive;
        switch (shape)
        {
            case 1:  return ceiling * std::tanh (y / jmax (ceiling, 1.0e-4f));                  // soft
            case 2:  return std::sin (jlimit (-3.0f, 3.0f, y / jmax (ceiling, 1.0e-4f)) * 0.9f) * ceiling;  // fold
            default: return jlimit (-ceiling, ceiling, y);                                       // hard
        }
    };

    float worst = 0.0f;
    AudioBuffer<float> clipped (chans, n);
    for (int ch = 0; ch < chans; ++ch)
    {
        auto* d = b.getWritePointer (ch);
        auto* w = clipped.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const float dry = d[i];
            const float shaped = shaper (dry);
            const float over = std::abs (dry * drive) - std::abs (shaped);
            if (over > 0.0f) worst = jmax (worst, Decibels::gainToDecibels (std::abs (dry * drive) / jmax (std::abs (shaped), 1.0e-6f)));
            w[i] = shaped;
        }
    }

    if (os != nullptr)
    {
        juce::dsp::AudioBlock<float> blk (clipped);
        auto up = os->processSamplesUp (blk);
        auto* upCh = up.getChannelPointer (0);
        for (size_t i = 0; i < up.getNumSamples(); ++i)
        {
            for (int ch = 0; ch < chans; ++ch)
                up.getChannelPointer ((size_t) ch)[i] = shaper (up.getChannelPointer ((size_t) ch)[i]);
        }
        juce::ignoreUnused (upCh);
        os->processSamplesDown (blk);
    }

    for (int ch = 0; ch < chans; ++ch)
    {
        auto* d = b.getWritePointer (ch);
        const auto* w = clipped.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
        {
            lpZ[ch] += aLp * (w[i] - lpZ[ch]);
            const float wet = lpZ[ch] * out;
            d[i] = d[i] * (1.0f - mix) + wet * mix;
        }
    }
    grDb.store (jlimit (0.0f, 36.0f, worst));
    pushHistory (b.getMagnitude (0, 0, n));
}

//==============================================================================
//== console channel: HPF + low / high shelves + soft-knee compressor
SxSslChannel::SxSslChannel() : SxEffectBase ("SX Console Channel", {
    { "hpf",     "HPF",             20.0f,  500.0f,  80.0f,  " Hz" },
    { "lfgain",  "LF Gain",        -15.0f,   15.0f,   0.0f,  " dB" },
    { "lffreq",  "LF Freq",         60.0f,  300.0f, 120.0f,  " Hz" },
    { "hfgain",  "HF Gain",        -15.0f,   15.0f,   0.0f,  " dB" },
    { "hffreq",  "HF Freq",       2000.0f, 12000.0f, 6000.0f, " Hz" },
    { "thr",     "Threshold",      -40.0f,    0.0f, -18.0f,  " dB" },
    { "ratio",   "Ratio",            1.0f,   10.0f,   4.0f,  "" },
    { "atk",     "Attack",           1.0f,   50.0f,  10.0f,  " ms" },
    { "rel",     "Release",         50.0f,  500.0f, 150.0f,  " ms" },
    { "makeup",  "Makeup",          -12.0f,   24.0f,   3.0f,  " dB" },
    { "output",  "Output",          -20.0f,   20.0f,   0.0f,  " dB" },
    { "mix",     "Mix",              0.0f,  100.0f, 100.0f,  " %" },
}, 0.0) {}

void SxSslChannel::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    envDb = 0.0f;
    hpZ[0] = hpZ[1] = lowZ[0] = lowZ[1] = 0.0f;
}

void SxSslChannel::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples(), chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float hpCoeff = onePoleCoeff (sampleRate, param (0));
    const float lfCoeff = onePoleCoeff (sampleRate, param (2));
    const float hfCoeff = onePoleCoeff (sampleRate, param (4));
    const float lfGain = Decibels::decibelsToGain (param (1));
    const float hfGain = Decibels::decibelsToGain (param (3));
    const float thr = param (5), ratio = jmax (1.0f, param (6));
    const float atk = std::exp (-1.0f / jmax (1.0f, param (7) * 0.001f * sr));
    const float rel = std::exp (-1.0f / jmax (1.0f, param (8) * 0.001f * sr));
    const float outGain = Decibels::decibelsToGain (param (9) + param (10));
    const float mix = param (11) / 100.0f;
    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float y[2] {};
        float peak = 0.0f;
        for (int c = 0; c < chans; ++c)
        {
            const float x = b.getReadPointer (c)[i];
            hpZ[c] += hpCoeff * (x - hpZ[c]);
            const float hp = x - hpZ[c];                    // high pass
            lowZ[c] += lfCoeff * (hp - lowZ[c]);
            const float low = lowZ[c];
            const float high = hp - low;
            y[c] = hp + low * (lfGain - 1.0f) + high * (hfGain - 1.0f);
            peak = jmax (peak, std::abs (y[c]));
        }
        const float inDb = Decibels::gainToDecibels (peak, -100.0f);
        const float over = jmax (0.0f, inDb - thr);
        const float target = over * (1.0f - 1.0f / ratio);
        const float coeff = target > envDb ? atk : rel;
        envDb = target + coeff * (envDb - target);
        worst = jmax (worst, envDb);
        const float g = Decibels::decibelsToGain (-envDb) * outGain;
        for (int c = 0; c < chans; ++c)
        {
            auto* d = b.getWritePointer (c);
            d[i] = d[i] * (1.0f - mix) + y[c] * g * mix;
        }
    }
    grDb.store (worst);
    pushHistory (worst);
}

//==============================================================================
//== de-esser
SxDeEsser::SxDeEsser() : SxEffectBase ("SX De-Esser", {
    { "freq",   "Frequency",   3000.0f, 12000.0f, 6500.0f, " Hz" },
    { "thr",    "Threshold",    -60.0f,    0.0f,  -30.0f,  " dB" },
    { "ratio",  "Ratio",          1.0f,   20.0f,    6.0f,  "" },
    { "range",  "Max Reduction", 0.0f,    24.0f,   14.0f,  " dB" },
    { "atk",    "Attack",         0.2f,   20.0f,    1.0f,  " ms" },
    { "rel",    "Release",       20.0f,  300.0f,   80.0f,  " ms" },
    { "listen", "Listen",         0.0f,    1.0f,    0.0f,  "" },
}, 0.0) {}

void SxDeEsser::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    envDb = 0.0f;
    lowZ[0] = lowZ[1] = 0.0f;
}

void SxDeEsser::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples(), chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float coeff = onePoleCoeff (sampleRate, param (0));
    const float thr = param (1), ratio = jmax (1.0f, param (2)), range = param (3);
    const float atk = std::exp (-1.0f / jmax (1.0f, param (4) * 0.001f * sr));
    const float rel = std::exp (-1.0f / jmax (1.0f, param (5) * 0.001f * sr));
    const bool listen = param (6) > 0.5f;
    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float low[2] {}, high[2] {};
        float peak = 0.0f;
        for (int c = 0; c < chans; ++c)
        {
            const float x = b.getReadPointer (c)[i];
            lowZ[c] += coeff * (x - lowZ[c]);
            low[c] = lowZ[c];
            high[c] = x - low[c];
            peak = jmax (peak, std::abs (high[c]));
        }
        const float detDb = Decibels::gainToDecibels (peak, -100.0f);
        const float over = jmax (0.0f, detDb - thr);
        const float target = jmin (range, over * (1.0f - 1.0f / ratio));
        const float a = target > envDb ? atk : rel;
        envDb = target + a * (envDb - target);
        worst = jmax (worst, envDb);
        const float g = Decibels::decibelsToGain (-envDb);
        for (int c = 0; c < chans; ++c)
        {
            auto* d = b.getWritePointer (c);
            d[i] = listen ? high[c] * g : low[c] + high[c] * g;
        }
    }
    grDb.store (worst);
    pushHistory (worst);
}

//==============================================================================
//== gate / expander
SxGate::SxGate() : SxEffectBase ("SX Gate", {
    { "thr",    "Threshold",  -80.0f,   0.0f,  -45.0f, " dB" },
    { "atk",    "Attack",       0.1f,  50.0f,    2.0f, " ms" },
    { "hold",   "Hold",         0.0f, 500.0f,   50.0f, " ms" },
    { "rel",    "Release",      5.0f, 1000.0f, 150.0f, " ms" },
    { "range",  "Range",      -80.0f,   0.0f,  -60.0f, " dB" },
    { "mix",    "Mix",          0.0f, 100.0f,  100.0f, " %" },
}, 0.0) {}

void SxGate::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    env = gain = 0.0f;
    open = false;
    holdSamples = 0;
}

void SxGate::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples(), chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float thr = param (0), atkMs = param (1), holdMs = param (2), relMs = param (3);
    const float rangeGain = Decibels::decibelsToGain (param (4));
    const float mix = param (5) / 100.0f;
    const float atk = std::exp (-1.0f / jmax (1.0f, atkMs * 0.001f * sr));
    const float rel = std::exp (-1.0f / jmax (1.0f, relMs * 0.001f * sr));
    const int holdLen = (int) std::lround (holdMs * 0.001f * sr);
    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < chans; ++c) peak = jmax (peak, std::abs (b.getReadPointer (c)[i]));
        const float db = Decibels::gainToDecibels (peak, -100.0f);
        if (db > thr) { open = true; holdSamples = holdLen; }
        else if (holdSamples > 0) --holdSamples;
        else if (db < thr - 3.0f) open = false;

        const float target = open ? 1.0f : rangeGain;
        const float coeff = target > gain ? atk : rel;
        gain = target + coeff * (gain - target);
        worst = jmax (worst, -Decibels::gainToDecibels (jmax (1.0e-6f, gain)));
        for (int c = 0; c < chans; ++c)
        {
            auto* d = b.getWritePointer (c);
            d[i] = d[i] * (1.0f - mix) + d[i] * gain * mix;
        }
    }
    grDb.store (worst);
    pushHistory (worst);
}

//==============================================================================
//== NS1-style denoiser
SxDenoiser::SxDenoiser() : SxEffectBase ("SX NS1 Denoiser", {
    { "reduction", "Reduction",  0.0f,  40.0f, 18.0f, " dB" },
    { "thr",       "Threshold", -80.0f,   0.0f, -50.0f, " dB" },
    { "atk",       "Attack",      0.5f,  50.0f,  5.0f, " ms" },
    { "rel",       "Release",    20.0f, 500.0f, 120.0f, " ms" },
    { "mix",       "Mix",         0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxDenoiser::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    env = floor = gain = 0.0f;
}

void SxDenoiser::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples(), chans = jmin (2, b.getNumChannels());
    const float sr = (float) sampleRate;
    const float reduction = param (0), thrDb = param (1);
    const float atk = std::exp (-1.0f / jmax (1.0f, param (2) * 0.001f * sr));
    const float rel = std::exp (-1.0f / jmax (1.0f, param (3) * 0.001f * sr));
    const float mix = param (4) / 100.0f;
    const float envRel = std::exp (-1.0f / jmax (1.0f, 0.10f * sr));
    const float floorRise = 1.0f - std::exp (-1.0f / jmax (1.0f, 2.0f * sr));
    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < chans; ++c) peak = jmax (peak, std::abs (b.getReadPointer (c)[i]));
        env = peak > env ? peak : env + (peak - env) * (1.0f - envRel);
        if (floor <= 0.0f) floor = jmax (1.0e-6f, env);
        else floor = env < floor ? env : floor + (env - floor) * floorRise;

        const float db = Decibels::gainToDecibels (env, -100.0f);
        const float below = jmax (0.0f, thrDb - db);            // how far under the threshold
        const float targetAtt = jmin (reduction, below * 1.4f);
        const float target = Decibels::decibelsToGain (-targetAtt);
        const float coeff = target < gain ? atk : rel;
        gain = target + coeff * (gain - target);
        worst = jmax (worst, -Decibels::gainToDecibels (jmax (1.0e-6f, gain)));
        for (int c = 0; c < chans; ++c)
        {
            auto* d = b.getWritePointer (c);
            d[i] = d[i] * (1.0f - mix) + d[i] * gain * mix;
        }
    }
    grDb.store (worst);
    pushHistory (worst);
}

//==============================================================================
//== reverse grain effect
SxReverse::SxReverse() : SxEffectBase ("SX Reverse", {
    { "time", "Grain",     50.0f, 1500.0f, 350.0f, " ms", 0.6f },
    { "fb",   "Feedback",   0.0f,   90.0f,  25.0f, " %" },
    { "tone", "Tone",       0.0f,  100.0f,  55.0f, " %" },
    { "mix",  "Mix",        0.0f,  100.0f,  50.0f, " %" },
}, 0.0) {}

void SxReverse::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    ringSize = jmax (2048, (int) (sampleRate * 2.5));
    for (int c = 0; c < 2; ++c) ring[c].assign ((size_t) ringSize, 0.0f);
    writePos = 0;
    phase = 0.0f;
    lp[0] = lp[1] = 0.0f;
    lastWet[0] = lastWet[1] = 0.0f;
}

void SxReverse::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples(), chans = jmin (2, b.getNumChannels());
    const int grain = jlimit (64, ringSize / 2, (int) (sampleRate * param (0) / 1000.0));
    const float fb = param (1) / 100.0f;
    const float mix = param (3) / 100.0f;
    const float tone = param (2) / 100.0f;
    const float lpCoeff = 0.01f + 0.35f * tone;

    for (int i = 0; i < n; ++i)
    {
        const int w = writePos;
        for (int c = 0; c < chans; ++c)
            ring[c][(size_t) w] = b.getReadPointer (c)[i] + lastWet[c] * fb;

        phase += 1.0f;
        if (phase >= (float) grain) phase -= (float) grain;
        const float phase2 = std::fmod (phase + (float) grain * 0.5f, (float) grain);
        const float w1 = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * phase / (float) grain);
        const float w2 = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * phase2 / (float) grain);
        const float wsum = jmax (1.0e-4f, w1 + w2);
        const int r1 = (w - 1 - (int) phase + ringSize * 2) % ringSize;
        const int r2 = (w - 1 - (int) phase2 + ringSize * 2) % ringSize;

        for (int c = 0; c < chans; ++c)
        {
            const float wet = (ring[c][(size_t) r1] * w1 + ring[c][(size_t) r2] * w2) / wsum;
            lp[c] += lpCoeff * (wet - lp[c]);
            lastWet[c] = lp[c];
            auto* d = b.getWritePointer (c);
            d[i] = d[i] * (1.0f - mix) + lp[c] * mix;
        }
        writePos = (w + 1) % ringSize;
    }
}

//==============================================================================
//== monophonic pitch correction (auto-tune)
SxPitchCorrector::SxPitchCorrector() : SxEffectBase ("SX Pitch Corrector", {
    { "root",   "Root",       0.0f, 11.0f, 0.0f, "" },
    { "scale",  "Scale (0=chromatic, 1=major, 2=minor)", 0.0f, 2.0f, 1.0f, "" },
    { "retune", "Retune Speed", 0.0f, 100.0f, 65.0f, " %" },
    { "mix",    "Mix",        0.0f, 100.0f, 100.0f, " %" },
}, 0.0) {}

void SxPitchCorrector::prepareToPlay (double sr, int)
{
    sampleRate = sr;
    ringSize = 16384;
    for (int c = 0; c < 2; ++c) ring[c].assign ((size_t) ringSize, 0.0f);
    analysis.assign (2048, 0.0f);
    writePos = 0;
    analysisWrite = 0;
    analysisCount = 0;
    phase = 0.0f;
    currentRatio = targetRatio = 1.0;
    detected.store (0.0f);
    cents.store (0.0f);
}

void SxPitchCorrector::analyse()
{
    constexpr int N = 1024;
    if (analysisCount < N) return;
    const int start = (analysisWrite - N + (int) analysis.size()) % (int) analysis.size();
    double energy = 0.0;
    for (int i = 0; i < N; ++i) { const double x = analysis[(size_t) ((start + i) % (int) analysis.size())]; energy += x * x; }
    if (energy < 1.0e-7) { detected.store (0.0f); targetRatio = 1.0; return; }

    int bestLag = 0;
    double best = 0.0;
    for (int lag = 40; lag <= N / 2; ++lag)
    {
        double corr = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i + lag < N; ++i)
        {
            const double a = analysis[(size_t) ((start + i) % (int) analysis.size())];
            const double b = analysis[(size_t) ((start + i + lag) % (int) analysis.size())];
            corr += a * b; e1 += a * a; e2 += b * b;
        }
        const double norm = std::sqrt (jmax (1.0e-12, e1 * e2));
        const double v = corr / norm;
        if (v > best) { best = v; bestLag = lag; }
    }
    if (bestLag <= 0 || best < 0.35)
    {
        detected.store (0.0f);
        targetRatio = 1.0;
        return;
    }

    const double hz = sampleRate / (double) bestLag;
    if (hz < 50.0 || hz > 1200.0) { detected.store (0.0f); targetRatio = 1.0; return; }

    const double midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
    const int root = jlimit (0, 11, (int) std::lround (param (0)));
    const int scale = jlimit (0, 2, (int) std::lround (param (1)));
    static const int major[] { 0, 2, 4, 5, 7, 9, 11 };
    static const int minor[] { 0, 2, 3, 5, 7, 8, 10 };

    auto allowed = [&] (int pc)
    {
        if (scale == 0) return true;
        const int rel = ((pc - root) % 12 + 12) % 12;
        if (scale == 1) return std::find (std::begin (major), std::end (major), rel) != std::end (major);
        return std::find (std::begin (minor), std::end (minor), rel) != std::end (minor);
    };
    auto nearestAllowed = [&] (int pc)
    {
        for (int d = 0; d <= 6; ++d)
        {
            if (allowed (pc + d)) return pc + d;
            if (allowed (pc - d)) return pc - d;
        }
        return pc;
    };

    int targetMidi = (int) std::lround (midi);
    if (! allowed (targetMidi)) targetMidi = nearestAllowed (targetMidi);
    const double targetHz = MidiMessage::getMidiNoteInHertz (targetMidi);
    targetRatio = jlimit (0.5, 2.0, targetHz / hz);
    detected.store ((float) hz);
    cents.store ((float) ((midi - targetMidi) * 100.0));
}

void SxPitchCorrector::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples(), chans = jmin (2, b.getNumChannels());
    const float mix = param (3) / 100.0f;
    const float retune = param (2) / 100.0f;
    const float smooth = 0.0008f + 0.03f * retune;
    constexpr int delay = 2048;
    static constexpr float kTwoPi = MathConstants<float>::twoPi;

    for (int i = 0; i < n; ++i)
    {
        const int w = writePos;
        float mono = 0.0f;
        for (int c = 0; c < chans; ++c)
        {
            ring[c][(size_t) w] = b.getReadPointer (c)[i];
            mono += b.getReadPointer (c)[i];
        }
        mono /= (float) jmax (1, chans);
        analysis[(size_t) analysisWrite] = mono;
        analysisWrite = (analysisWrite + 1) % (int) analysis.size();
        ++analysisCount;
        if ((analysisCount % 1024) == 0) analyse();

        currentRatio += (targetRatio - currentRatio) * smooth;
        phase += (float) ((currentRatio - 1.0) / (double) delay);
        if (phase >= 1.0f) phase -= 1.0f;
        if (phase < 0.0f) phase += 1.0f;
        const float phase2 = std::fmod (phase + 0.5f, 1.0f);
        const float w1 = 0.5f - 0.5f * std::cos (kTwoPi * phase);
        const float w2 = 0.5f - 0.5f * std::cos (kTwoPi * phase2);
        const float wsum = jmax (1.0e-4f, w1 + w2);
        const int p1 = (w - delay + (int) (phase * delay) + ringSize * 2) % ringSize;
        const int p2 = (w - delay + (int) (phase2 * delay) + ringSize * 2) % ringSize;

        const bool correcting = std::abs (targetRatio - 1.0) > 0.0005 && detected.load() > 0.0f;
        for (int c = 0; c < chans; ++c)
        {
            const float wet = (ring[c][(size_t) p1] * w1 + ring[c][(size_t) p2] * w2) / wsum;
            auto* d = b.getWritePointer (c);
            d[i] = correcting ? d[i] * (1.0f - mix) + wet * mix : d[i];
        }
        writePos = (w + 1) % ringSize;
    }
}
} // namespace sx