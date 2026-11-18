#include "Model.h"

// Off-line audio editing renderer: time-stretch (WSOLA) and transpose for audio clips.
// Everything here is non-destructive: the clip keeps its source material and only the derived
// buffer (AudioClip::rendered) is rebuilt when speed / pitch change.
namespace sx
{
using namespace juce;

namespace
{
    constexpr int kFrameMs = 45;     // WSOLA analysis window (approx.)

    // Windowed overlap-add time stretch: output duration = input duration * ratio.
    // The channel alignment is decided once on the mono mix so the stereo image is kept.
    void stretchBuffer (const AudioBuffer<float>& in, double sampleRate, int outSamples, AudioBuffer<float>& out)
    {
        const int ch = jmax (1, in.getNumChannels());
        const int inLen = in.getNumSamples();
        out.setSize (ch, jmax (1, outSamples), false, true, false);
        out.clear();
        if (inLen <= 0 || outSamples <= 0) return;

        if (std::abs ((double) outSamples / (double) inLen - 1.0) < 1.0e-9)
        {
            out.makeCopyOf (in, true);
            return;
        }

        const double sr = jmax (8000.0, sampleRate);
        const int frame = jlimit (256, 4096, (int) std::round (sr * (double) kFrameMs / 1000.0) & ~1);
        const int hop = frame / 2;
        const int search = jmax (2, frame / 4);
        const double ratio = (double) outSamples / (double) inLen;

        // Hann window (periodic) and its overlap sum
        std::vector<float> win ((size_t) frame);
        for (int i = 0; i < frame; ++i)
            win[(size_t) i] = 0.5f - 0.5f * (float) std::cos (MathConstants<double>::twoPi * (double) i / (double) frame);

        // mono guide used for the alignment search
        std::vector<float> guide ((size_t) inLen, 0.0f);
        for (int c = 0; c < in.getNumChannels(); ++c)
        {
            const float* p = in.getReadPointer (c);
            for (int i = 0; i < inLen; ++i) guide[(size_t) i] += p[i] / (float) jmax (1, in.getNumChannels());
        }

        std::vector<float> norm ((size_t) outSamples, 0.0f);
        int prevPos = 0;
        for (int k = 0; k * hop < outSamples; ++k)
        {
            const int outPos = k * hop;
            const int nominal = jlimit (0, jmax (0, inLen - frame), (int) std::llround ((double) outPos / jmax (1.0e-9, ratio)));
            int pos = nominal;

            if (k > 0)
            {
                // WSOLA: pick the segment that best continues the previous frame's tail
                const int cont = jlimit (0, jmax (0, inLen - hop), prevPos + hop);
                double bestScore = -1.0e30;
                for (int off = -search; off <= search; ++off)
                {
                    const int cand = jlimit (0, jmax (0, inLen - frame), nominal + off);
                    const int len = jmin (hop, inLen - jmax (cont, cand));
                    if (len <= 0) continue;
                    double score = 0.0;
                    const float* a = guide.data() + cand;
                    const float* b = guide.data() + cont;
                    for (int i = 0; i < len; ++i) score += (double) a[i] * (double) b[i];
                    if (score > bestScore) { bestScore = score; pos = cand; }
                }
            }

            const int n = jmin (frame, outSamples - outPos);
            for (int c = 0; c < ch; ++c)
            {
                const float* src = in.getReadPointer (jmin (c, in.getNumChannels() - 1), pos);
                float* dst = out.getWritePointer (c, outPos);
                for (int i = 0; i < n; ++i) dst[i] += src[i] * win[(size_t) i];
            }
            for (int i = 0; i < n; ++i) norm[(size_t) (outPos + i)] += win[(size_t) i];
            prevPos = pos;
        }

        for (int c = 0; c < ch; ++c)
        {
            float* dst = out.getWritePointer (c);
            for (int i = 0; i < outSamples; ++i)
            {
                const float g = norm[(size_t) i];
                dst[i] = g > 1.0e-4f ? dst[i] / g : 0.0f;
            }
        }
    }

    // linear-interpolation resample to 'outSamples' samples (changes speed and pitch together)
    void resampleBuffer (const AudioBuffer<float>& in, int outSamples, AudioBuffer<float>& out)
    {
        const int ch = jmax (1, in.getNumChannels());
        const int inLen = in.getNumSamples();
        out.setSize (ch, jmax (1, outSamples), false, true, false);
        if (inLen <= 0 || outSamples <= 0) { out.clear(); return; }
        const double step = (double) inLen / (double) outSamples;
        for (int c = 0; c < ch; ++c)
        {
            const float* src = in.getReadPointer (jmin (c, in.getNumChannels() - 1));
            float* dst = out.getWritePointer (c);
            for (int i = 0; i < outSamples; ++i)
            {
                const double sp = (double) i * step;
                const int i0 = jlimit (0, inLen - 1, (int) sp);
                const int i1 = jmin (inLen - 1, i0 + 1);
                const float f = (float) (sp - (double) i0);
                dst[i] = src[i0] + (src[i1] - src[i0]) * f;
            }
        }
    }
}

bool AudioClip::RenderKey::matches (const AudioClip& c) const
{
    return source == c.data.get()
        && std::abs (offsetSec - c.offsetSec) < 1.0e-6
        && std::abs (sourceLenSec - c.sourceLengthSec()) < 1.0e-6
        && std::abs (stretch - c.stretch) < 1.0e-9
        && std::abs (pitch - (double) c.pitchSemitones) < 1.0e-6;
}

void invalidateClipRender (AudioClip& clip)
{
    clip.rendered.reset();
    clip.renderKey = {};
}

bool ensureClipPlayback (AudioClip& clip)
{
    if (clip.data == nullptr) return false;
    if (! clip.isStretched())
    {
        clip.rendered.reset();
        clip.renderKey = {};
        return false;
    }
    if (clip.rendered != nullptr && clip.renderKey.matches (clip)) return true;

    const auto& src = clip.data->buffer;
    const double sr = jmax (1.0, clip.data->sampleRate);
    const int srcLen = src.getNumSamples();
    const int srcCh = jmax (1, src.getNumChannels());
    if (srcLen <= 0) return false;

    // the used source segment: offsetSec .. offsetSec + sourceLengthSec (clamped to the file)
    const int startSample = jlimit (0, srcLen, (int) std::llround (clip.offsetSec * sr));
    const int availSamples = jmax (0, srcLen - startSample);
    const int segSamples = jmin (availSamples, jmax (1, (int) std::llround (clip.sourceLengthSec() * sr)));
    const int outSamples = jmax (1, (int) std::llround (clip.lengthSec * sr));
    if (segSamples <= 0 || availSamples <= 0) return false;

    AudioBuffer<float> segment (srcCh, segSamples);
    for (int c = 0; c < srcCh; ++c)
        segment.copyFrom (c, 0, src, jmin (c, src.getNumChannels() - 1), startSample, segSamples);

    // pitch: stretch by (speed * pitchRatio), then resample by pitchRatio so the duration only
    // changes by the speed while the pitch moves by the transpose amount
    const double pitchRatio = std::pow (2.0, (double) clip.pitchSemitones / 12.0);
    AudioBuffer<float> rendered;
    if (std::abs (pitchRatio - 1.0) > 1.0e-6)
    {
        AudioBuffer<float> stretched;
        stretchBuffer (segment, sr, jmax (1, (int) std::llround ((double) outSamples * pitchRatio)), stretched);
        resampleBuffer (stretched, outSamples, rendered);
    }
    else
        stretchBuffer (segment, sr, outSamples, rendered);

    auto d = std::make_shared<AudioData>();
    d->buffer = std::move (rendered);
    d->sampleRate = sr;
    d->buildPeaks();
    clip.rendered = d;
    clip.renderKey.source = clip.data.get();
    clip.renderKey.offsetSec = clip.offsetSec;
    clip.renderKey.sourceLenSec = clip.sourceLengthSec();
    clip.renderKey.stretch = clip.stretch;
    clip.renderKey.pitch = (double) clip.pitchSemitones;
    return true;
}
}
