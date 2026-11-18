#pragma once

#include "Theme.h"
#include "../model/Model.h"

// The bar / beat / sub-beat grid of the timeline. Shared so that every strip showing time -
// the scrolled lane area and the pinned rows above it (tempo, master bus) - lines up exactly.
namespace sx::grid
{
    // 'xOffset' maps beat 0 to a pixel column: 0 for content that is scrolled by a viewport,
    // -scrollX for a component that draws itself at the current scroll position.
    inline void paint (juce::Graphics& g, const Project& p, double pxPerBeat, juce::Rectangle<int> clip,
                       int xOffset, double subGrid)
    {
        if (pxPerBeat <= 0.0) return;
        const double b0 = (double) (clip.getX() - xOffset) / pxPerBeat;
        const double b1 = (double) (clip.getRight() - xOffset) / pxPerBeat;

        std::vector<tempomap::BarInfo> bars;
        tempomap::barStartsBetween (p.tempo, b0, b1, bars);
        if (bars.empty())
            tempomap::barStartsBetween (p.tempo, juce::jmax (0.0, b0 - 96.0), b1, bars);

        for (size_t i = 0; i < bars.size(); ++i)
        {
            const auto& b = bars[i];
            const double beatLen = 4.0 / (double) b.den;
            const double w = (double) b.num * 4.0 / (double) b.den;
            const bool showBeats = beatLen * pxPerBeat >= 16.0;
            const double sub = subGrid > 0.0 ? subGrid : 0.25;
            const bool showSub = sub * pxPerBeat >= 14.0 && sub < beatLen - 1.0e-6;

            if (showBeats)
                for (double bt = 0.0; bt < w - 1.0e-6; bt += beatLen)
                {
                    const int x = (int) std::round ((b.beat + bt) * pxPerBeat) + xOffset;
                    if (bt > 1.0e-6)
                    {
                        g.setColour (theme::gridBeat.withAlpha (0.7f));
                        g.fillRect (x, clip.getY(), 1, clip.getHeight());
                    }
                    if (showSub)
                        for (double s = sub; s < beatLen - 1.0e-6; s += sub)
                        {
                            g.setColour (theme::gridSub.withAlpha (0.6f));
                            g.fillRect ((int) std::round ((b.beat + bt + s) * pxPerBeat) + xOffset, clip.getY(), 1, clip.getHeight());
                        }
                }
            g.setColour (theme::gridBar);
            g.fillRect ((int) std::round (b.beat * pxPerBeat) + xOffset, clip.getY(), 1, clip.getHeight());
        }
    }
}
