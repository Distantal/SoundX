#pragma once

#include "model/Model.h"
#include "Theme.h"

namespace sx
{
/** Waveform drawing shared by the arrangement and the clip gain editor.

    Both views used to draw one hard edged rectangle per pixel column, taken from a cache of
    128-sample min/max blocks. That is fine when a column covers hundreds of samples, but it makes
    the envelope look stair stepped at every other zoom level:

      - a column was never allowed to cover less than one whole block, so zooming in flattened the
        waveform into 128-sample boxes,
      - each column was a separate (un-antialiased) rectangle, so the top and bottom edges were
        jagged wherever the level changed,
      - a silent column became a 1 px dash instead of a hairline through the middle.

    This draws the same min/max envelope as a single anti-aliased path instead: the columns are
    joined into one outline, a column that covers fewer samples than one peak block reads those
    samples exactly (so the curve follows the audio at high zoom instead of the block grid), and
    the peak cache is only used once a column really spans several blocks.

    A stereo clip shows its two channels as separate stacked lanes (drawStereo): drawChannel reads
    the per-channel peak cache / samples of one channel, and the zero line of each lane is its
    vertical centre.
*/
namespace waveform
{
    namespace detail
    {
        struct ColumnLevels
        {
            float lo = 0.0f;
            float hi = 0.0f;
        };

        /** True when 'channel' exists in the buffer and has a usable per-channel peak cache. */
        inline bool hasChannelCache (const AudioData& data, int channel)
        {
            return channel >= 0
                && data.hasChannelPeaks()
                && channel < data.buffer.getNumChannels()
                && (size_t) channel < data.channelPeakMin.size()
                && data.channelPeakMin[(size_t) channel].size() == data.channelPeakMax[(size_t) channel].size();
        }

        /** Min/max of one pixel column covering the time span [t0, t1); 'channel' < 0 mixes all channels.

            A column that covers fewer samples than one kPeakBlock reads those samples exactly, so
            the curve follows the audio when zoomed in; a wider column takes the cache. A clip whose
            per-channel cache was never rebuilt (old project) falls back to the mixed cache for its
            lanes - drawStereo draws one mixed waveform instead in that case.
        */
        inline ColumnLevels readColumn (const AudioData& data, int channel, double t0, double t1)
        {
            ColumnLevels out;

            const int numSamples = data.buffer.getNumSamples();
            const int numChannels = data.buffer.getNumChannels();
            if (numSamples <= 0 || numChannels <= 0) return out;

            const double sr = data.sampleRate > 0.0 ? data.sampleRate : 44100.0;
            const double s0 = juce::jlimit (0.0, (double) numSamples, t0 * sr);
            const double s1 = juce::jlimit (0.0, (double) numSamples, t1 * sr);

            if (s1 - s0 <= (double) AudioData::kPeakBlock)
            {
                // fewer samples than one cache block: read them, so the curve follows the audio
                const int i0 = (int) std::floor (s0);
                const int i1 = juce::jmax (i0 + 1, (int) std::ceil (s1));
                const int ch0 = juce::jmax (0, channel);
                const int ch1 = channel >= 0 ? juce::jmin (channel + 1, numChannels) : numChannels;

                for (int ch = ch0; ch < ch1; ++ch)
                {
                    const auto* d = data.buffer.getReadPointer (ch);
                    for (int s = i0; s < i1 && s < numSamples; ++s)
                    {
                        out.lo = juce::jmin (out.lo, d[s]);
                        out.hi = juce::jmax (out.hi, d[s]);
                    }
                }
                return out;
            }

            // the column spans at least one cache block: take the cached peaks instead of the samples
            const std::vector<float>* pmin = &data.peakMin;
            const std::vector<float>* pmax = &data.peakMax;
            if (channel >= 0 && hasChannelCache (data, channel))
            {
                pmin = &data.channelPeakMin[(size_t) channel];
                pmax = &data.channelPeakMax[(size_t) channel];
            }
            else if (channel >= 0 && channel < numChannels)
            {
                // old / hand-built AudioData may have no per-channel cache; read the channel
                // directly (with a stride cap) rather than showing the mixed pair in both lanes
                const int i0 = (int) std::floor (s0);
                const int i1 = (int) std::ceil (s1);
                const int stride = juce::jmax (1, (i1 - i0) / 4096);
                const auto* d = data.buffer.getReadPointer (channel);
                for (int s = i0; s < i1 && s < numSamples; s += stride)
                {
                    out.lo = juce::jmin (out.lo, d[s]);
                    out.hi = juce::jmax (out.hi, d[s]);
                }
                return out;
            }

            const int numBlocks = (int) juce::jmin (pmin->size(), pmax->size());
            if (numBlocks <= 0) return out;   // no cache at all: silent hairline, as before

            int b0 = (int) std::floor (t0 * sr / AudioData::kPeakBlock);
            int b1 = (int) std::ceil (t1 * sr / AudioData::kPeakBlock);
            b0 = juce::jlimit (0, numBlocks - 1, b0);
            b1 = juce::jlimit (b0 + 1, numBlocks, b1);

            for (int b = b0; b < b1; ++b)
            {
                out.lo = juce::jmin (out.lo, (*pmin)[(size_t) b]);
                out.hi = juce::jmax (out.hi, (*pmax)[(size_t) b]);
            }
            return out;
        }

        /** Fills one top / bottom value per column of the lane centred on 'mid'.

            The vectors are passed in so a two-lane draw can reuse the same allocation for both
            lanes: one allocation per paint call, exactly as the single waveform had.
        */
        template <typename SecAtX, typename GainAt>
        void fillLane (const AudioData& data, int channel, int x0, int x1, float mid, float amp,
                       SecAtX&& secAtX, GainAt&& gainAt, std::vector<float>& top, std::vector<float>& bottom)
        {
            const int numCols = x1 - x0;
            top.resize ((size_t) numCols);
            bottom.resize ((size_t) numCols);

            for (int i = 0; i < numCols; ++i)
            {
                const int x = x0 + i;
                const double tA = secAtX ((double) x);
                const double tB = secAtX ((double) x + 1.0);
                const auto levels = readColumn (data, channel, juce::jmin (tA, tB), juce::jmax (tA, tB));

                const float env = gainAt ((double) x);
                const float hiC = juce::jlimit (-1.0f, 1.0f, levels.hi * env);
                const float loC = juce::jlimit (-1.0f, 1.0f, levels.lo * env);

                float yTop = mid - hiC * amp;
                float yBottom = mid - loC * amp;

                // a silent column still shows a hairline instead of nothing
                if (yBottom - yTop < 1.0f)
                {
                    yTop = mid - 0.5f;
                    yBottom = mid + 0.5f;
                }

                top[(size_t) i] = yTop;
                bottom[(size_t) i] = yBottom;
            }
        }

        /** Joins the per-column edges into one closed outline and fills it in the current colour. */
        inline void fillOutline (juce::Graphics& g, int x0, int x1,
                                 const std::vector<float>& top, const std::vector<float>& bottom)
        {
            const int numCols = (int) top.size();
            if (numCols <= 0) return;

            juce::Path path;

            // top edge, left to right
            path.startNewSubPath ((float) x0, top.front());
            for (int i = 1; i < numCols; ++i)
                path.lineTo ((float) (x0 + i), top[(size_t) i]);

            // right hand edge, then the bottom edge back to the left
            path.lineTo ((float) x1, bottom.back());
            for (int i = numCols - 1; i >= 0; --i)
                path.lineTo ((float) (x0 + i), bottom[(size_t) i]);

            path.closeSubPath();
            g.fillPath (path);
        }
    }

    /** Draws the envelope of one channel into 'area' for the pixel columns [x0, x1).

        'channel' 0 / 1 is the left / right channel of a stereo clip; a negative channel draws the
        mixed envelope of every channel (the single waveform of a mono clip). secAtX maps a column's
        left edge to the time (in seconds) inside the clip's playback buffer - the caller owns tempo
        ramps, stretch and reverse. gainAt gives the gain applied to a column (clip gain envelope,
        fades). The colour must be set before calling; the zero line is the vertical centre of 'area'.
    */
    template <typename SecAtX, typename GainAt>
    void drawChannel (juce::Graphics& g, juce::Rectangle<float> area, const AudioData& data, int channel,
                      int x0, int x1, SecAtX&& secAtX, GainAt&& gainAt)
    {
        const int numSamples = data.buffer.getNumSamples();
        if (numSamples <= 0 || area.getHeight() < 2.0f) return;
        if (channel >= 0 && channel >= data.buffer.getNumChannels()) return;

        x0 = juce::jmax (x0, (int) std::floor (area.getX()));
        x1 = juce::jmin (x1, (int) std::ceil (area.getRight()));
        if (x1 <= x0) return;

        const float mid = area.getCentreY();
        const float amp = juce::jmax (1.0f, area.getHeight() * 0.5f - 1.0f);

        // one top and one bottom value per column, then a single outline: joining the columns is
        // what turns the stair steps into a smooth, anti-aliased edge
        std::vector<float> top, bottom;
        detail::fillLane (data, channel, x0, x1, mid, amp, secAtX, gainAt, top, bottom);
        detail::fillOutline (g, x0, x1, top, bottom);
    }

    /** Draws the envelope of 'data' into 'area' for the pixel columns [x0, x1).

        Mixed / mono envelope: every channel is folded into one waveform (the media pool and any
        caller that wants a single envelope keep using this).
    */
    template <typename SecAtX, typename GainAt>
    void draw (juce::Graphics& g, juce::Rectangle<float> area, const AudioData& data,
               int x0, int x1, SecAtX&& secAtX, GainAt&& gainAt)
    {
        // channel -1 = the all-channel mix
        drawChannel (g, area, data, -1, x0, x1, secAtX, gainAt);
    }

    /** Draws a stereo clip as two stacked lanes: channel 0 (L) on top, channel 1 (R) below.

        'area' is split at its vertical centre, each lane's zero line is the centre of its half, a
        thin divider separates the lanes and small faint L / R letters mark them when there is room.
        Without a per-channel peak cache (a clip from an old project) or in a body too short to split
        this falls back to the single mixed waveform of draw().
    */
    template <typename SecAtX, typename GainAt>
    void drawStereo (juce::Graphics& g, juce::Rectangle<float> area, const AudioData& data,
                     int x0, int x1, SecAtX&& secAtX, GainAt&& gainAt)
    {
        const int numSamples = data.buffer.getNumSamples();
        if (numSamples <= 0) return;

        if (area.getHeight() < 8.0f || data.buffer.getNumChannels() < 2)
        {
            draw (g, area, data, x0, x1, secAtX, gainAt);
            return;
        }

        x0 = juce::jmax (x0, (int) std::floor (area.getX()));
        x1 = juce::jmin (x1, (int) std::ceil (area.getRight()));
        if (x1 <= x0) return;

        const float midY = area.getCentreY();
        const auto laneL = area.withBottom (midY);
        const auto laneR = area.withTop (midY);

        // the column scratch buffers are shared by both lanes: one allocation per paint call
        std::vector<float> top, bottom;
        detail::fillLane (data, 0, x0, x1, laneL.getCentreY(),
                          juce::jmax (1.0f, laneL.getHeight() * 0.5f - 1.0f), secAtX, gainAt, top, bottom);
        detail::fillOutline (g, x0, x1, top, bottom);

        detail::fillLane (data, 1, x0, x1, laneR.getCentreY(),
                          juce::jmax (1.0f, laneR.getHeight() * 0.5f - 1.0f), secAtX, gainAt, top, bottom);
        detail::fillOutline (g, x0, x1, top, bottom);

        // faint zero line of each lane, so a silent channel still reads as a channel (the fill
        // already leaves a hairline, this makes the middle of both lanes visible)
        g.setColour (theme::textFaint.withAlpha (0.28f));
        g.fillRect (area.getX(), laneL.getCentreY() - 0.5f, area.getWidth(), 1.0f);
        g.fillRect (area.getX(), laneR.getCentreY() - 0.5f, area.getWidth(), 1.0f);

        // thin separator between the lanes (the same subtle dark line the gain line underlay uses)
        g.setColour (theme::panelDeep.withAlpha (0.45f));
        g.fillRect (area.getX(), midY - 0.5f, area.getWidth(), 1.0f);

        // the two lanes are separated by the zero lines; no per-clip "L / R" text: at arrangement
        // zoom levels it turns every audio block into visual noise, especially around the playhead
    }
}
}
