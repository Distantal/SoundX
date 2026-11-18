#include "BuiltInEffects.h"

namespace sx
{
using namespace juce;

namespace
{
    struct Coefs { double b0, b1, b2, a1, a2; };

    // The two K-weighting stages of ITU-R BS.1770: a high shelf of about +4 dB around 1.7 kHz, then a
    // high pass at 38 Hz. Built for the actual sample rate (RBJ cookbook form with the BS.1770 corners).
    Coefs kShelfCoefs (double sr)
    {
        const double f0 = 1681.974450955533, q = 0.7071752369554196;
        const double A = std::pow (10.0, 3.999843853973347 / 40.0);
        const double w0 = 2.0 * MathConstants<double>::pi * f0 / sr;
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double beta = 2.0 * std::sqrt (A) * (sn / (2.0 * q));
        const double a0 = (A + 1.0) - (A - 1.0) * cs + beta;
        return { A * ((A + 1.0) + (A - 1.0) * cs + beta) / a0,
                 -2.0 * A * ((A - 1.0) + (A + 1.0) * cs) / a0,
                 A * ((A + 1.0) + (A - 1.0) * cs - beta) / a0,
                 2.0 * ((A - 1.0) - (A + 1.0) * cs) / a0,
                 ((A + 1.0) - (A - 1.0) * cs - beta) / a0 };
    }

    Coefs kHighPassCoefs (double sr)
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double w0 = 2.0 * MathConstants<double>::pi * f0 / sr;
        const double cs = std::cos (w0), sn = std::sin (w0);
        const double alpha = sn / (2.0 * q);
        const double a0 = 1.0 + alpha;
        return { (1.0 + cs) * 0.5 / a0, -(1.0 + cs) / a0, (1.0 + cs) * 0.5 / a0, -2.0 * cs / a0, (1.0 - alpha) / a0 };
    }

    // loudness of a mean square (the sum of the channels), in LUFS
    double lufsOf (double meanSquare)
    {
        return meanSquare > 1.0e-10 ? -0.691 + 10.0 * std::log10 (meanSquare) : -100.0;
    }

    // Catmull-Rom between p1 and p2 at t in [0, 1]
    double catmull (double p0, double p1, double p2, double p3, double t)
    {
        return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t
                      + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
    }
}

//==============================================================================
//== loudness meter
SxLoudnessMeter::SxLoudnessMeter() : SxEffectBase ("SX Loudness", {
    { "hold", "Peak Hold", 0.0f, 99.0f, 50.0f, " %" },
    { "target", "Target", -40.0f, -6.0f, -14.0f, " LUFS" },
}, 0.0) {}

void SxLoudnessMeter::prepareToPlay (double sr, int)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;
    const auto shelfC = kShelfCoefs (sampleRate);
    const auto hpC = kHighPassCoefs (sampleRate);
    for (int c = 0; c < 2; ++c)
    {
        shelf[c] = {};
        shelf[c].b0 = shelfC.b0; shelf[c].b1 = shelfC.b1; shelf[c].b2 = shelfC.b2;
        shelf[c].a1 = shelfC.a1; shelf[c].a2 = shelfC.a2;
        highPass[c] = {};
        highPass[c].b0 = hpC.b0; highPass[c].b1 = hpC.b1; highPass[c].b2 = hpC.b2;
        highPass[c].a1 = hpC.a1; highPass[c].a2 = hpC.a2;
        for (auto& v : tp[c]) v = 0.0;
        peakCur[c] = peakHoldLeft[c] = rmsSq[c] = 0.0;
    }
    chunkEnergy = chunkPeak = 0.0;
    chunkSamples = 0;
    chunkLength = jmax (1, (int) (0.1 * sampleRate));
    chunkCount = chunkWrite = 0;
    chunkMs.fill (0.0);
    histCount.fill (0.0);
    histEnergy.fill (0.0);
    for (auto& h : histLufs) h.store (-100.0f);
    for (auto& h : histPeak) h.store (-100.0f);
    for (auto& h : histRms) h.store (-100.0f);
    loudPos.store (0);
    lufsMomentary.store (-100.0f);
    lufsShort.store (-100.0f);
    lufsIntegrated.store (-100.0f);
    truePeakHold.store (-100.0f);
}

void SxLoudnessMeter::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    if (n <= 0 || chans <= 0) return;
    const double sr = sampleRate;
    const double holdSamples = 3.0 * jlimit (0.0, 99.0, (double) param (0)) / 99.0 * sr;
    const double fall = std::pow (10.0, -12.0 * (double) n / (20.0 * sr));     // 12 dB per second
    const double rmsA = std::exp (-(double) n / (0.3 * sr));

    // one 100 ms step: the momentary, short-term and gated integrated loudness, and the history
    auto finishChunk = [&]
    {
        const double ms = chunkEnergy / (double) chunkSamples;
        chunkMs[(size_t) chunkWrite] = ms;
        chunkWrite = (chunkWrite + 1) % kChunks;
        if (chunkCount < kChunks) ++chunkCount;

        double sum4 = 0.0;
        for (int k = 0; k < jmin (4, chunkCount); ++k)
            sum4 += chunkMs[(size_t) ((chunkWrite - 1 - k + 2 * kChunks) % kChunks)];
        const double momentary = sum4 / (double) jmax (1, jmin (4, chunkCount));
        double sumAll = 0.0;
        for (double v : chunkMs) sumAll += v;
        const double shortTerm = sumAll / (double) jmax (1, chunkCount);
        const double L = lufsOf (momentary);
        lufsMomentary.store ((float) L);
        lufsShort.store ((float) lufsOf (shortTerm));

        // gated integrated loudness (BS.1770): blocks above -70 LUFS, then above the mean minus 10 LU
        if (chunkCount >= 4 && L > -70.0)
        {
            const int bin = jlimit (0, kHistBins - 1, (int) std::floor ((L + 70.0) * 10.0));
            histCount[(size_t) bin] += 1.0;
            histEnergy[(size_t) bin] += momentary;
        }
        double allE = 0.0, allN = 0.0;
        for (int k = 0; k < kHistBins; ++k) { allE += histEnergy[(size_t) k]; allN += histCount[(size_t) k]; }
        if (allN > 0.0)
        {
            const double relGate = lufsOf (allE / allN) - 10.0;
            double gE = 0.0, gN = 0.0;
            for (int k = 0; k < kHistBins; ++k)
            {
                const double centre = -70.0 + (k + 0.5) * 0.1;
                if (centre > relGate) { gE += histEnergy[(size_t) k]; gN += histCount[(size_t) k]; }
            }
            lufsIntegrated.store (gN > 0.0 ? (float) lufsOf (gE / gN) : -100.0f);
        }

        const int pos = loudPos.load();
        const double rmsNow = chans > 1 ? std::sqrt (0.5 * (rmsSq[0] + rmsSq[1])) : std::sqrt (rmsSq[0]);
        histLufs[(size_t) pos].store ((float) L);
        histPeak[(size_t) pos].store (Decibels::gainToDecibels ((float) chunkPeak, -100.0f));
        histRms[(size_t) pos].store (Decibels::gainToDecibels ((float) rmsNow, -100.0f));
        loudPos.store ((pos + 1) % kHistory);

        chunkEnergy = 0.0;
        chunkPeak = 0.0;
        chunkSamples = 0;
    };

    double blockPeak[2] {}, blockMs[2] {}, blockTp[2] {};
    const float* src[2] { b.getReadPointer (0), chans > 1 ? b.getReadPointer (1) : b.getReadPointer (0) };
    for (int i = 0; i < n; ++i)
    {
        double e = 0.0;
        for (int c = 0; c < chans; ++c)
        {
            const double x = src[c][i];
            blockMs[c] += x * x;
            const double a = std::abs (x);
            blockPeak[c] = jmax (blockPeak[c], a);
            chunkPeak = jmax (chunkPeak, a);

            auto& s1 = shelf[c];
            const double y1 = s1.b0 * x + s1.b1 * s1.x1 + s1.b2 * s1.x2 - s1.a1 * s1.y1 - s1.a2 * s1.y2;
            s1.x2 = s1.x1; s1.x1 = x; s1.y2 = s1.y1; s1.y1 = y1;
            auto& s2 = highPass[c];
            const double y2 = s2.b0 * y1 + s2.b1 * s2.x1 + s2.b2 * s2.x2 - s2.a1 * s2.y1 - s2.a2 * s2.y2;
            s2.x2 = s2.x1; s2.x1 = y1; s2.y2 = s2.y1; s2.y1 = y2;
            e += y2 * y2;

            // 4x true peak: the cubic through the last four samples, sampled between the middle two
            double* t = tp[c];
            t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = x;
            double tpk = jmax (std::abs (t[1]), std::abs (t[2]));
            for (double f : { 0.25, 0.5, 0.75 })
                tpk = jmax (tpk, std::abs (catmull (t[0], t[1], t[2], t[3], f)));
            blockTp[c] = jmax (blockTp[c], tpk);
        }
        chunkEnergy += e;
        ++chunkSamples;
        if (chunkSamples >= chunkLength) finishChunk();
    }

    for (int c = 0; c < chans; ++c)
    {
        const double pk = blockPeak[c];
        if (pk >= peakCur[c])
        {
            peakCur[c] = pk;
            peakHoldLeft[c] = holdSamples;
        }
        else if (peakHoldLeft[c] > 0.0)
            peakHoldLeft[c] -= n;
        else
            peakCur[c] *= fall;
        peakVals[(size_t) c].store ((float) peakCur[c]);

        rmsSq[c] = rmsA * rmsSq[c] + (1.0 - rmsA) * (blockMs[c] / (double) n);
        rmsVals[(size_t) c].store ((float) std::sqrt (rmsSq[c]));
        truePeakHold.store (jmax (truePeakHold.load(), Decibels::gainToDecibels ((float) blockTp[c], -100.0f)));
    }
    if (chans == 1)
    {
        peakVals[1].store (peakVals[0].load());
        rmsVals[1].store (rmsVals[0].load());
    }

    // the display stamp moves with every block, so the editor repaints while audio runs
    pushHistory (lufsMomentary.load());
    pushStereo (peakVals[0].load(), peakVals[1].load());
}

//==============================================================================
//== stereo field meter
SxStereoFieldMeter::SxStereoFieldMeter() : SxEffectBase ("SX Stereo Field", {
    { "hold", "Hold", 0.0f, 99.0f, 40.0f, " %" },
    { "zoom", "Zoom", 50.0f, 200.0f, 100.0f, " %" },
}, 0.0) {}

void SxStereoFieldMeter::prepareToPlay (double sr, int)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;
    sllS = srrS = slrS = smmS = sssS = 0.0;
    histSamples = 0;
    {
        SpinLock::ScopedLockType lock (scopeLock);
        scope.fill ({ 0.0f, 0.0f });
        scopeWrite = 0;
    }
    for (auto& h : corrHist) h.store (0.0f);
    for (auto& h : widthHist) h.store (0.0f);
    fieldPos.store (0);
}

void SxStereoFieldMeter::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = b.getNumSamples();
    const int chans = jmin (2, b.getNumChannels());
    if (n <= 0 || chans <= 0) return;
    const float* l = b.getReadPointer (0);
    const float* r = chans > 1 ? b.getReadPointer (1) : l;

    double sll = 0.0, srr = 0.0, slr = 0.0, smm = 0.0, sss = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double L = l[i], R = r[i];
        const double m = 0.5 * (L + R), s = 0.5 * (L - R);
        sll += L * L;
        srr += R * R;
        slr += L * R;
        smm += m * m;
        sss += s * s;
    }

    {
        SpinLock::ScopedLockType lock (scopeLock);
        for (int i = jmax (0, n - kScope); i < n; ++i)
        {
            scope[(size_t) scopeWrite] = { l[i], r[i] };
            scopeWrite = (scopeWrite + 1) % kScope;
        }
    }

    // about 300 ms of memory, so the readings are steady enough to read
    const double nn = (double) n;
    const double a = std::exp (-nn / (0.3 * sampleRate));
    sllS = a * sllS + (1.0 - a) * sll / nn;
    srrS = a * srrS + (1.0 - a) * srr / nn;
    slrS = a * slrS + (1.0 - a) * slr / nn;
    smmS = a * smmS + (1.0 - a) * smm / nn;
    sssS = a * sssS + (1.0 - a) * sss / nn;

    const double corrV = jlimit (-1.0, 1.0, slrS / (std::sqrt (sllS * srrS) + 1.0e-20));
    const double ampSum = std::sqrt (sllS) + std::sqrt (srrS);
    const double balV = ampSum < 1.0e-9 ? 0.0 : (std::sqrt (srrS) - std::sqrt (sllS)) / ampSum;
    // 100 % is a fully stereo signal (side as loud as mid), 0 % is mono
    const double widthV = jlimit (0.0, 200.0, 100.0 * std::sqrt (sssS / (smmS + sssS + 1.0e-20)) * std::sqrt (2.0));
    corr.store ((float) corrV);
    bal.store ((float) balV);
    width.store ((float) widthV);
    midLvl.store ((float) std::sqrt (smmS));
    sideLvl.store ((float) std::sqrt (sssS));

    histSamples += n;
    if (histSamples >= (int) (0.1 * sampleRate))
    {
        histSamples = 0;
        const int pos = fieldPos.load();
        corrHist[(size_t) pos].store ((float) corrV);
        widthHist[(size_t) pos].store ((float) widthV);
        fieldPos.store ((pos + 1) % kHistory);
    }

    // the display stamp moves with every block, so the editor repaints while audio runs
    pushHistory ((float) corrV);
    pushStereo ((float) std::sqrt (sllS), (float) std::sqrt (srrS));
}

void SxStereoFieldMeter::copyScope (std::array<std::pair<float, float>, kScope>& out) const
{
    SpinLock::ScopedLockType lock (scopeLock);
    for (int i = 0; i < kScope; ++i)
        out[(size_t) i] = scope[(size_t) ((scopeWrite + i) % kScope)];
}

} // namespace sx
