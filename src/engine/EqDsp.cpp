#include "BuiltInEffects.h"

#include <complex>

namespace sx
{
using namespace juce;

//==============================================================================
//== SX EQ: eight bands, each one off / bell / shelf / cut / notch / band pass, cut slopes up to 96 dB/oct
namespace
{
    std::vector<SxEffectBase::ParamSpec> eqSpecs()
    {
        // The spec stores raw `const char*`, so both the ids and the names have to live in storage
        // that outlives the list; these vectors are filled once and stay alive for the process.
        static std::vector<String> idStore, nameStore;
        idStore.clear(); nameStore.clear();
        idStore.reserve (128); nameStore.reserve (128);

        std::vector<SxEffectBase::ParamSpec> v;
        v.reserve (91);
        auto add = [&] (const String& id, const String& nm, float mn, float mx, float df,
                        const char* suffix = "", float skew = 1.0f)
        {
            idStore.push_back (id);
            nameStore.push_back (nm);
            v.push_back ({ idStore.back().toRawUTF8(), nameStore.back().toRawUTF8(), mn, mx, df, suffix, skew });
        };

        static const float defFreq[8] { 80.0f, 300.0f, 1500.0f, 8000.0f, 500.0f, 2500.0f, 60.0f, 12000.0f };
        static const float defType[8] { 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        static const float defQ   [8] { 0.70f, 1.00f, 1.00f, 0.70f, 1.00f, 1.00f, 0.70f, 0.70f };
        static const bool  defOn  [8] { true, true, true, true, false, false, false, false };
        for (int b = 0; b < 8; ++b)
        {
            const String p ("b" + String (b));
            const String n (b + 1);
            add (p + "on",  "Band " + n + " On",        0.0f,   1.0f,   defOn[b] ? 1.0f : 0.0f);
            add (p + "t",   "Band " + n + " Type",      0.0f,   7.0f,   defType[b]);
            add (p + "f",   "Band " + n + " Freq",     20.0f,   20000.0f, defFreq[b], " Hz", 0.3f);
            add (p + "g",   "Band " + n + " Gain",    -18.0f,   18.0f,   0.0f, " dB");
            add (p + "q",   "Band " + n + " Q",         0.2f,   10.0f,   defQ[b]);
            add (p + "dyn", "Band " + n + " Dynamic",   0.0f,   1.0f,   0.0f);
            add (p + "thr", "Band " + n + " Threshold", -60.0f, 0.0f,   -24.0f, " dB");
            add (p + "rng", "Band " + n + " Range",     0.0f,   24.0f,  6.0f, " dB");
            add (p + "atk", "Band " + n + " Attack",    0.1f,   100.0f, 10.0f, " ms", 0.5f);
            add (p + "rel", "Band " + n + " Release",   10.0f,  1000.0f, 120.0f, " ms", 0.5f);
        }
        add ("solo", "Band Solo", 0.0f, 8.0f, 0.0f);
        add ("mode", "Phase Mode", 0.0f, 1.0f, 0.0f);        // 0 minimum, 1 linear
        for (int b = 0; b < 8; ++b)
            add (String ("b") + String (b) + "slope", "Band " + String (b + 1) + " Slope", 0.0f, 5.0f, 0.0f);
        add ("analyser", "Analyser", 0.0f, 2.0f, 0.0f);      // 0 pre, 1 post, 2 off
        return v;
    }

    // number of biquad sections of a cut slope (index 0 is the single section that uses the band's Q)
    constexpr int kSlopeSections[6] { 1, 2, 3, 4, 6, 8 };
}

SxEq::SxEq() : SxEffectBase ("SX EQ", eqSpecs(), 0.0)
{
    for (auto& r : dynReductionDb) r.store (0.0f);
}

bool SxEq::savePreset (const File& f) const
{
    XmlElement xml ("SXEQPreset");
    xml.setAttribute ("bands", kNumBands);
    for (int b = 0; b < kNumBands; ++b)
    {
        auto* be = xml.createNewChildElement ("Band" + String (b));
        be->setAttribute ("on", param (pOn (b)) > 0.5f);
        be->setAttribute ("type", (int) std::round (param (pType (b))));
        be->setAttribute ("freq", (double) param (pFreq (b)));
        be->setAttribute ("gain", (double) param (pGain (b)));
        be->setAttribute ("q", (double) param (pQ (b)));
        be->setAttribute ("slope", (int) std::round (param (pSlope (b))));
        be->setAttribute ("dyn", param (pDynOn (b)) > 0.5f);
        be->setAttribute ("thr", (double) param (pThr (b)));
        be->setAttribute ("rng", (double) param (pRange (b)));
        be->setAttribute ("atk", (double) param (pAtk (b)));
        be->setAttribute ("rel", (double) param (pRel (b)));
    }
    return xml.writeTo (f);
}

bool SxEq::loadPreset (const File& f)
{
    auto xml = XmlDocument::parse (f);
    if (xml == nullptr) return false;
    auto put = [this] (int idx, float v)
    {
        auto* raw = getParameters()[idx];
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (raw)) raw->setValueNotifyingHost (rp->convertTo0to1 (v));
    };
    bool any = false;
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto* be = xml->getChildByName ("Band" + String (b));
        if (be == nullptr) continue;
        any = true;
        put (pOn (b), be->getBoolAttribute ("on") ? 1.0f : 0.0f);
        put (pType (b), (float) be->getIntAttribute ("type"));
        put (pFreq (b), (float) be->getDoubleAttribute ("freq", 1000.0));
        put (pGain (b), (float) be->getDoubleAttribute ("gain"));
        put (pQ (b), (float) be->getDoubleAttribute ("q", 1.0));
        put (pSlope (b), (float) be->getIntAttribute ("slope", 0));
        put (pDynOn (b), be->getBoolAttribute ("dyn") ? 1.0f : 0.0f);
        put (pThr (b), (float) be->getDoubleAttribute ("thr", -24.0));
        put (pRange (b), (float) be->getDoubleAttribute ("rng", 6.0));
        put (pAtk (b), (float) be->getDoubleAttribute ("atk", 10.0));
        put (pRel (b), (float) be->getDoubleAttribute ("rel", 120.0));
    }
    return any;
}

//==============================================================================
// Filter design

SxEq::Biquad SxEq::Biquad::lowShelf (double sr, float hz, float gainDb, float q)
{
    const float A = std::pow (10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * MathConstants<float>::pi * jlimit (10.0f, (float) sr * 0.45f, hz) / (float) sr;
    const float cs = std::cos (w0), sn = std::sin (w0);
    const float alpha = sn / (2.0f * jmax (0.1f, q));
    const float beta = 2.0f * std::sqrt (A) * alpha;
    const float a0 = (A + 1.0f) + (A - 1.0f) * cs + beta;
    Biquad b;
    b.b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + beta) / a0;
    b.b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
    b.b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - beta) / a0;
    b.a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
    b.a2 = ((A + 1.0f) + (A - 1.0f) * cs - beta) / a0;
    return b;
}

SxEq::Biquad SxEq::Biquad::highShelf (double sr, float hz, float gainDb, float q)
{
    const float A = std::pow (10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * MathConstants<float>::pi * jlimit (10.0f, (float) sr * 0.45f, hz) / (float) sr;
    const float cs = std::cos (w0), sn = std::sin (w0);
    const float alpha = sn / (2.0f * jmax (0.1f, q));
    const float beta = 2.0f * std::sqrt (A) * alpha;
    const float a0 = (A + 1.0f) - (A - 1.0f) * cs + beta;
    Biquad b;
    b.b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + beta) / a0;
    b.b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
    b.b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - beta) / a0;
    b.a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
    b.a2 = ((A + 1.0f) - (A - 1.0f) * cs - beta) / a0;
    return b;
}

SxEq::Biquad SxEq::Biquad::peak (double sr, float hz, float gainDb, float q)
{
    const float A = std::pow (10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * MathConstants<float>::pi * jlimit (10.0f, (float) sr * 0.45f, hz) / (float) sr;
    const float cs = std::cos (w0), sn = std::sin (w0);
    const float alpha = sn / (2.0f * jmax (0.1f, q));
    const float a0 = 1.0f + alpha / A;
    Biquad b;
    b.b0 = (1.0f + alpha * A) / a0;
    b.b1 = -2.0f * cs / a0;
    b.b2 = (1.0f - alpha * A) / a0;
    b.a1 = -2.0f * cs / a0;
    b.a2 = (1.0f - alpha / A) / a0;
    return b;
}

SxEq::Biquad SxEq::Biquad::highPass (double sr, float hz, float q)
{
    const float w0 = 2.0f * MathConstants<float>::pi * jlimit (10.0f, (float) sr * 0.45f, hz) / (float) sr;
    const float cs = std::cos (w0), sn = std::sin (w0);
    const float alpha = sn / (2.0f * jmax (0.1f, q));
    const float a0 = 1.0f + alpha;
    Biquad b;
    b.b0 = (1.0f + cs) * 0.5f / a0;
    b.b1 = -(1.0f + cs) / a0;
    b.b2 = b.b0;
    b.a1 = -2.0f * cs / a0;
    b.a2 = (1.0f - alpha) / a0;
    return b;
}

SxEq::Biquad SxEq::Biquad::lowPass (double sr, float hz, float q)
{
    const float w0 = 2.0f * MathConstants<float>::pi * jlimit (10.0f, (float) sr * 0.45f, hz) / (float) sr;
    const float cs = std::cos (w0), sn = std::sin (w0);
    const float alpha = sn / (2.0f * jmax (0.1f, q));
    const float a0 = 1.0f + alpha;
    Biquad b;
    b.b0 = (1.0f - cs) * 0.5f / a0;
    b.b1 = (1.0f - cs) / a0;
    b.b2 = b.b0;
    b.a1 = -2.0f * cs / a0;
    b.a2 = (1.0f - alpha) / a0;
    return b;
}

SxEq::Biquad SxEq::Biquad::notch (double sr, float hz, float q)
{
    const float w0 = 2.0f * MathConstants<float>::pi * jlimit (10.0f, (float) sr * 0.45f, hz) / (float) sr;
    const float cs = std::cos (w0), sn = std::sin (w0);
    const float alpha = sn / (2.0f * jmax (0.1f, q));
    const float a0 = 1.0f + alpha;
    Biquad b;
    b.b0 = 1.0f / a0; b.b1 = -2.0f * cs / a0; b.b2 = 1.0f / a0;
    b.a1 = -2.0f * cs / a0; b.a2 = (1.0f - alpha) / a0;
    return b;
}

SxEq::Biquad SxEq::Biquad::bandPass (double sr, float hz, float q)
{
    const float w0 = 2.0f * MathConstants<float>::pi * jlimit (10.0f, (float) sr * 0.45f, hz) / (float) sr;
    const float cs = std::cos (w0), sn = std::sin (w0);
    const float alpha = sn / (2.0f * jmax (0.1f, q));
    const float a0 = 1.0f + alpha;
    Biquad b;
    b.b0 = alpha / a0; b.b1 = 0.0f; b.b2 = -alpha / a0;
    b.a1 = -2.0f * cs / a0; b.a2 = (1.0f - alpha) / a0;
    return b;
}

SxEq::Biquad SxEq::Biquad::forBand (double sr, int type, float hz, float gainDb, float q)
{
    switch (type)
    {
        case LowShelf:  return lowShelf (sr, hz, gainDb, q);
        case HighShelf: return highShelf (sr, hz, gainDb, q);
        case LowCut:    return highPass (sr, hz, q);
        case HighCut:   return lowPass (sr, hz, q);
        case Notch:     return notch (sr, hz, q);
        case BandPass:  return bandPass (sr, hz, q);
        default:        return peak (sr, hz, gainDb, q);
    }
}

// A cut with a slope is a Butterworth cascade: each section is one pole pair, and the Q of each pair
// is the one that keeps the sum maximally flat. Slope 0 is a single section with the band's own Q.
SxEq::Chain SxEq::chainFor (double sr, int type, float hz, float gainDb, float q, int slope)
{
    Chain c;
    const bool cut = type == LowCut || type == HighCut;
    if (cut && slope > 0)
    {
        const int s = jlimit (1, 5, slope);
        const int sections = kSlopeSections[s];
        const int poles = 2 * sections;
        c.n = sections;
        for (int k = 1; k <= sections; ++k)
        {
            const float qk = 1.0f / (2.0f * std::sin ((float) (2 * k - 1) * MathConstants<float>::pi / (float) (2 * poles)));
            c.s[k - 1] = type == LowCut ? Biquad::highPass (sr, hz, qk) : Biquad::lowPass (sr, hz, qk);
        }
        return c;
    }
    c.s[0] = Biquad::forBand (sr, type, hz, gainDb, q);
    c.n = 1;
    return c;
}

SxEq::Chain SxEq::chainFromSettings (double sr, const BandSettings& s)
{
    if (s[0] < 0.5f) return {};
    return chainFor (sr, (int) std::round (s[1]), s[2], s[3], s[4], (int) std::round (s[5]));
}

double SxEq::chainDb (const Chain& c, double hz, double sr)
{
    if (c.n == 0) return 0.0;
    const double w = 2.0 * MathConstants<double>::pi * hz / sr;
    const std::complex<double> z1 = std::polar (1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    double db = 0.0;
    for (int s = 0; s < c.n; ++s)
    {
        const auto& b = c.s[s];
        const auto num = (double) b.b0 + (double) b.b1 * z1 + (double) b.b2 * z2;
        const auto den = 1.0 + (double) b.a1 * z1 + (double) b.a2 * z2;
        db += 10.0 * std::log10 (std::norm (num / den) + 1.0e-12);
    }
    return db;
}

SxEq::BandSettings SxEq::readBand (int b) const
{
    return { param (pOn (b)), param (pType (b)), param (pFreq (b)), param (pGain (b)), param (pQ (b)), param (pSlope (b)) };
}

SxEq::SettingsKey SxEq::settingsKey() const
{
    SettingsKey k {};
    for (int b = 0; b < kNumBands; ++b) k[(size_t) b] = readBand (b);
    return k;
}

void SxEq::copyCoefs (Biquad& dst, const Biquad& src)
{
    dst.b0 = src.b0; dst.b1 = src.b1; dst.b2 = src.b2; dst.a1 = src.a1; dst.a2 = src.a2;
}

//==============================================================================
// Processing

void SxEq::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    constexpr int N = kAnalyserFft;
    if (fft == nullptr) fft = std::make_unique<dsp::FFT> (12);      // 2^12 = 4096
    if ((int) window.size() != N)
    {
        window.assign ((size_t) N, 0.0f);
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * (float) i / (float) (N - 1));
    }
    fftBuffer.assign ((size_t) N * 2, 0.0f);
    ring.assign ((size_t) N, 0.0f);
    ringPos = 0;
    hopCount = 0;
    std::fill (std::begin (smoothDb), std::end (smoothDb), -120.0f);
    float zero[SxEffectBase::kSpectrumBins] {};
    pushSpectrum (zero, SxEffectBase::kSpectrumBins);

    for (int b = 0; b < kNumBands; ++b)
    {
        bandEnv[b] = 0.0f;
        for (int k = 0; k < kMaxSections; ++k) bands[b].s[k].reset();
        sideChain[b].reset();
    }
    rebuild (true);

    linearConv.prepare ({ sr, (uint32) jmax (512, block), 2 });
    linearPrepared = true;
    linearReady = false;
}

void SxEq::rebuild (bool force)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto s = readBand (b);
        bandOn[b] = s[0] > 0.5f;
        bandDyn[b] = param (pDynOn (b)) > 0.5f;
        dynThr[b] = param (pThr (b));
        dynRange[b] = param (pRange (b));
        dynAtk[b] = param (pAtk (b));
        dynRel[b] = param (pRel (b));
        if (! force && s == lastKey[(size_t) b]) continue;
        lastKey[(size_t) b] = s;

        // new coefficients, same filter memory: a node that moves does not reset the filter, so no click
        const Chain fresh = chainFromSettings (sr, s);
        for (int k = 0; k < kMaxSections; ++k)
        {
            if (k < fresh.n) copyCoefs (bands[b].s[k], fresh.s[k]);
            else bands[b].s[k].reset();
        }
        bands[b].n = fresh.n;
        if (s[0] > 0.5f)
            copyCoefs (sideChain[b], Biquad::forBand (sr, (int) std::round (s[1]), s[2], s[3], s[4]));
    }
}

void SxEq::refreshCurves (bool effective) const
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const auto key = settingsKey();
    if (! curveValid || key != curveKey)
    {
        curveKey = key;
        curveValid = true;
        for (int b = 0; b < kNumBands; ++b)
            curveChains[b] = chainFromSettings (sr, key[(size_t) b]);
    }
    if (! effective) return;

    auto eff = key;
    for (int b = 0; b < kNumBands; ++b) eff[(size_t) b][3] -= dynReductionDb[b].load();
    if (! dynCurveValid || eff != dynCurveKey)
    {
        dynCurveKey = eff;
        dynCurveValid = true;
        for (int b = 0; b < kNumBands; ++b)
            dynCurveChains[b] = chainFromSettings (sr, eff[(size_t) b]);
    }
}

float SxEq::curveDb (float hz) const
{
    float out = 0.0f;
    responseDb (-1, &hz, 1, &out, false);
    return out;
}

float SxEq::curveDbEffective (float hz) const
{
    float out = 0.0f;
    responseDb (-1, &hz, 1, &out, true);
    return out;
}

void SxEq::responseDb (int band, const float* hz, int n, float* out, bool effective) const
{
    refreshCurves (effective);
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const Chain* chains = effective ? dynCurveChains : curveChains;
    for (int i = 0; i < n; ++i)
    {
        double sum = 0.0;
        if (band >= 0)
            sum = chainDb (chains[band], hz[i], sr);
        else
            for (int b = 0; b < kNumBands; ++b) sum += chainDb (chains[b], hz[i], sr);
        out[i] = (float) sum;
    }
}

// The analyser: a 4096-point Hann window that refreshes every 1024 samples, mapped onto 512
// logarithmic bins. Narrow bins interpolate between FFT bins, wide bins take the power average of
// the FFT bins they cover, so the low end is a curve rather than a staircase. Each bin then rises
// quickly and falls slowly, and neighbouring bins are averaged over about a sixth of an octave.
void SxEq::feedAnalyser (const AudioBuffer<float>& in, int n)
{
    constexpr int N = kAnalyserFft;
    const int chans = jmin (2, in.getNumChannels());
    const float* l = in.getReadPointer (0);
    const float* r = chans > 1 ? in.getReadPointer (1) : nullptr;
    for (int i = 0; i < n; ++i)
    {
        ring[(size_t) ringPos] = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
        ringPos = (ringPos + 1) & (N - 1);
        if (++hopCount >= kAnalyserHop)
        {
            hopCount = 0;
            analyse();
        }
    }
}

void SxEq::analyse()
{
    constexpr int N = kAnalyserFft;
    if (fft == nullptr || (int) window.size() != N) return;
    for (int i = 0; i < N; ++i)
        fftBuffer[(size_t) i] = ring[(size_t) ((ringPos + i) & (N - 1))] * window[(size_t) i];
    std::fill (fftBuffer.begin() + N, fftBuffer.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    const double binHz = sr / (double) N;
    const double amplitude = 4.0 / (double) N;         // a sine under a Hann window reads 4 / N times its FFT bin
    const int half = N / 2;

    float amp[SxEffectBase::kSpectrumBins] {};
    for (int i = 0; i < SxEffectBase::kSpectrumBins; ++i)
    {
        const double f0 = 20.0 * std::pow (1000.0, (double) i / SxEffectBase::kSpectrumBins);
        const double f1 = 20.0 * std::pow (1000.0, (double) (i + 1) / SxEffectBase::kSpectrumBins);
        const double kLo = f0 / binHz, kHi = f1 / binHz, kc = std::sqrt (kLo * kHi);
        float v;
        if (kHi - kLo < 1.0)
        {
            const int k0 = jlimit (1, half - 1, (int) kc);
            const int k1 = jmin (k0 + 1, half - 1);
            const float t = (float) jlimit (0.0, 1.0, kc - (double) k0);
            v = (1.0f - t) * fftBuffer[(size_t) k0] + t * fftBuffer[(size_t) k1];
        }
        else
        {
            const int a = jlimit (1, half - 1, (int) kLo);
            const int c = jlimit (a, half - 1, (int) kHi);
            double acc = 0.0;
            for (int k = a; k <= c; ++k) acc += (double) fftBuffer[(size_t) k] * fftBuffer[(size_t) k];
            v = (float) std::sqrt (acc / (double) (c - a + 1));
        }
        amp[i] = v * (float) amplitude;
    }

    for (int i = 0; i < SxEffectBase::kSpectrumBins; ++i)
    {
        const float target = Decibels::gainToDecibels (amp[i], -120.0f);
        const float coef = target > smoothDb[i] ? 0.35f : 0.88f;
        smoothDb[i] = coef * smoothDb[i] + (1.0f - coef) * target;
    }

    // each bin takes the strongest power of its neighbours: a tone keeps its level (an average would
    // spread it over the bins around it and lower it), and the display still looks smooth
    float power[SxEffectBase::kSpectrumBins] {};
    for (int i = 0; i < SxEffectBase::kSpectrumBins; ++i)
        power[i] = std::pow (10.0f, smoothDb[i] / 10.0f);
    float out[SxEffectBase::kSpectrumBins] {};
    constexpr int kHalfWidth = 1;
    for (int i = 0; i < SxEffectBase::kSpectrumBins; ++i)
    {
        float peakPower = 0.0f;
        for (int j = jmax (0, i - kHalfWidth); j <= jmin (SxEffectBase::kSpectrumBins - 1, i + kHalfWidth); ++j)
            peakPower = jmax (peakPower, power[j]);
        out[i] = std::sqrt (peakPower);
    }
    pushSpectrum (out, SxEffectBase::kSpectrumBins);
}

void SxEq::buildLinearIr()
{
    // impulse through the whole cascade, then make it symmetric: a linear phase FIR of 4096 taps
    constexpr int taps = 4096;
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    AudioBuffer<float> ir (1, taps);
    ir.clear();
    ir.setSample (0, 0, 1.0f);
    for (int b = 0; b < kNumBands; ++b)
    {
        auto chain = chainFromSettings (sr, readBand (b));
        for (int s = 0; s < chain.n; ++s)
            for (int i = 0; i < taps; ++i)
                ir.setSample (0, i, chain.s[s].process (0, ir.getSample (0, i)));
    }
    for (int i = 0; i < taps / 2; ++i)
    {
        const float v = 0.5f * (ir.getSample (0, i) + ir.getSample (0, taps - 1 - i));
        ir.setSample (0, i, v);
        ir.setSample (0, taps - 1 - i, v);
    }
    linearConv.loadImpulseResponse (std::move (ir), sr,
                                    juce::dsp::Convolution::Stereo::no,
                                    juce::dsp::Convolution::Trim::no,
                                    juce::dsp::Convolution::Normalise::no);
    linearReady = true;
    linearKey = settingsKey();
    linearWait = 0;
}

void SxEq::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    const int analyserMode = (int) std::round (param (pAnalyser()));
    if (analyserMode == 0) feedAnalyser (b, n);
    rebuild (false);

    // linear phase: the same curve as an FIR. The FIR is rebuilt only once the settings have been
    // still for a moment, so dragging a node never loads a new impulse response on every block.
    if (param (pMode()) > 0.5f)
    {
        const auto key = settingsKey();
        if (! linearReady)
            buildLinearIr();
        else if (key != linearKey)
        {
            linearWait += n;
            if (linearWait >= (int) (0.15 * sampleRate))
                buildLinearIr();
        }
        else
            linearWait = 0;

        juce::dsp::AudioBlock<float> blk (b.getArrayOfWritePointers(), (size_t) chans, (size_t) 0, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (blk);
        linearConv.process (ctx);
        if (analyserMode == 1) feedAnalyser (b, n);
        pushHistory (b.getMagnitude (0, 0, n));
        return;
    }

    const int solo = jlimit (0, kNumBands, (int) std::round (param (pSolo()))) - 1;
    const bool scOn = hasSidechain() && sidechainFeed->n > 0;

    // per-block constants of the dynamic bands
    float atkCoef[kNumBands] {}, relCoef[kNumBands] {}, gainDb[kNumBands] {};
    for (int band = 0; band < kNumBands; ++band)
    {
        atkCoef[band] = std::exp (-1.0f / jmax (1.0f, dynAtk[band] * 0.001f * (float) sampleRate));
        relCoef[band] = std::exp (-1.0f / jmax (1.0f, dynRel[band] * 0.001f * (float) sampleRate));
        gainDb[band] = param (pGain (band));
    }

    // Side-chain: when the host feeds one, every dynamic band follows that signal instead of its
    // own. The detection runs once per block, in mono.
    float amount[kNumBands] {};
    if (scOn)
    {
        const int m = jmin (n, sidechainFeed->n);
        for (int band = 0; band < kNumBands; ++band)
        {
            if (! bandOn[band] || ! bandDyn[band]) continue;
            float env = bandEnv[band];
            for (int i = 0; i < m; ++i)
            {
                const float sc = 0.5f * (sidechainFeed->l[i] + sidechainFeed->r[i]);
                const float banded = std::abs (sideChain[band].process (0, sc));
                const float coef = banded > env ? atkCoef[band] : relCoef[band];
                env = coef * env + (1.0f - coef) * banded;
            }
            bandEnv[band] = env;
            amount[band] = jlimit (0.0f, 1.0f, jmax (0.0f, Decibels::gainToDecibels (env, -96.0f) - dynThr[band]) / 12.0f);
        }
    }

    float* d0 = b.getWritePointer (0);
    float* d1 = chans > 1 ? b.getWritePointer (1) : nullptr;
    for (int i = 0; i < n; ++i)
    {
        float y[2] { d0[i], d1 != nullptr ? d1[i] : d0[i] };

        if (solo >= 0 && solo < kNumBands && bandOn[solo])
        {
            // monitor one band only: the same filter, nothing else in the chain
            for (int ch = 0; ch < chans; ++ch) y[ch] = runChain (bands[solo], ch, y[ch]);
            d0[i] = y[0];
            if (d1 != nullptr) d1[i] = y[1];
            continue;
        }

        for (int band = 0; band < kNumBands; ++band)
        {
            if (! bandOn[band]) continue;
            if (! bandDyn[band])
            {
                for (int ch = 0; ch < chans; ++ch) y[ch] = runChain (bands[band], ch, y[ch]);
                continue;
            }

            // dynamic band: follow the level of this band (the louder channel decides, one envelope
            // for both channels) and pull its gain back when it is louder than the threshold, so a
            // cut bites harder and a boost backs off
            if (! scOn)
            {
                float side = 0.0f;
                for (int ch = 0; ch < chans; ++ch)
                    side = jmax (side, std::abs (sideChain[band].process (ch, y[ch])));
                const float coef = side > bandEnv[band] ? atkCoef[band] : relCoef[band];
                bandEnv[band] = coef * bandEnv[band] + (1.0f - coef) * side;
                amount[band] = jlimit (0.0f, 1.0f, jmax (0.0f, Decibels::gainToDecibels (bandEnv[band], -96.0f) - dynThr[band]) / 12.0f);
            }
            // the band moves its gain toward more reduction: a boost backs off and a cut digs deeper.
            // Scaling the band's own contribution by m gives exactly that in dB terms.
            const float reduceDb = amount[band] * dynRange[band];
            const float gBand = Decibels::decibelsToGain (gainDb[band]);
            const float gTarget = Decibels::decibelsToGain (gainDb[band] - reduceDb);
            const float m = std::abs (gBand - 1.0f) > 1.0e-4f ? (gTarget - 1.0f) / (gBand - 1.0f) : 0.0f;
            for (int ch = 0; ch < chans; ++ch)
            {
                const float filtered = runChain (bands[band], ch, y[ch]);
                y[ch] = y[ch] + m * (filtered - y[ch]);
            }
        }

        d0[i] = y[0];
        if (d1 != nullptr) d1[i] = y[1];
    }

    for (int band = 0; band < kNumBands; ++band)
        dynReductionDb[band].store (bandOn[band] && bandDyn[band] ? amount[band] * dynRange[band] : 0.0f);

    if (analyserMode == 1) feedAnalyser (b, n);
    pushHistory (b.getMagnitude (0, 0, n));
}

} // namespace sx
