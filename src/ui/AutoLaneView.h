#pragma once

#include "Theme.h"
#include "../model/Model.h"

// One automation row, drawn and hit-tested the same way everywhere: the lane rows under a track
// in the arrange view and the lanes under the master bus row both go through this. The curve is
// a monotone polyline through the nodes (see Model.h): between two nodes it only rises or falls,
// never overshoots and never doubles back in time.
namespace sx::autolane
{
    constexpr int kPad = 8;   // top / bottom padding inside a row

    inline float yOf (float v, juce::Rectangle<int> r)
    {
        return (float) r.getY() + (float) kPad + (1.0f - juce::jlimit (0.0f, 1.0f, v)) * (float) (r.getHeight() - 2 * kPad);
    }

    inline float valueAt (int y, juce::Rectangle<int> r)
    {
        const float top = (float) r.getY() + (float) kPad;
        const float h = (float) juce::jmax (1, r.getHeight() - 2 * kPad);
        return juce::jlimit (0.0f, 1.0f, 1.0f - ((float) y - top) / h);
    }

    // x of a beat inside the row: 'xOffset' is -scrollX for a view drawn outside the scrolled
    // lane area, and 0 for the lane content itself (which the viewport already scrolls)
    inline float xOf (double beat, double pxPerBeat, int xOffset)
    {
        return (float) (beat * pxPerBeat) + (float) xOffset;
    }

    inline double beatAt (int x, double pxPerBeat, int xOffset)
    {
        return pxPerBeat > 0.0 ? (double) (x - xOffset) / pxPerBeat : 0.0;
    }

    inline juce::String valueText (const juce::String& target, float v)
    {
        if (target == "vol")
        {
            const float g = faderToGain (v);
            return g <= 0.0005f ? juce::String ("-inf dB") : juce::String (juce::Decibels::gainToDecibels (g), 1) + " dB";
        }
        if (target == "pan")
        {
            const int p = (int) std::round ((v * 2.0f - 1.0f) * 100.0f);
            return p == 0 ? juce::String ("C") : (p < 0 ? "L" + juce::String (-p) : "R" + juce::String (p));
        }
        return juce::String ((int) std::round (v * 100.0f)) + " %";
    }

    inline float midGuideValue (const juce::String& target)
    {
        return target == "pan" ? 0.5f : (target == "vol" ? 0.75f : 0.5f);
    }

    // draws one row: background, mid guide, the curve and its nodes. 'fillBackground' is false
    // when the curve is drawn over the track body (Studio One style): the clip stays visible and
    // only the guide, curve and nodes are painted on top of it.
    inline void paintRow (juce::Graphics& g, const AutoLane& lane, juce::Rectangle<int> r,
                          int xOffset, double pxPerBeat, int activePoint = -1,
                          bool fillBackground = true)
    {
        if (r.getHeight() <= 0) return;

        if (fillBackground)
        {
            g.setColour (theme::panel.withAlpha (0.65f));
            g.fillRect (r);
            g.setColour (theme::line);
            g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
        }

        g.setColour (theme::lineSoft.withAlpha (fillBackground ? 1.0f : 0.55f));
        g.fillRect ((float) r.getX(), yOf (midGuideValue (lane.target), r), (float) r.getWidth(), 1.0f);

        const auto& pts = lane.points;
        if (pts.empty())
        {
            if (fillBackground)
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::font (12.0f));
                g.drawText (TRANS ("Click to add automation points"),
                            r.withX (r.getX() + 12).withHeight (r.getHeight()), juce::Justification::centredLeft);
            }
            return;
        }

        // the curve: straight (monotone) segments between the nodes, extended to the row edges
        const double bL = beatAt (r.getX(), pxPerBeat, xOffset);
        const double bR = beatAt (r.getRight(), pxPerBeat, xOffset);
        juce::Path line;
        line.startNewSubPath ((float) r.getX(), yOf (evalAutoLane (pts, bL), r));
        for (auto& p : pts)
            if (p.beat > bL && p.beat < bR)
                line.lineTo (xOf (p.beat, pxPerBeat, xOffset), yOf (p.value, r));
        line.lineTo ((float) r.getRight(), yOf (evalAutoLane (pts, bR), r));

        if (fillBackground)
        {
            juce::Path fill (line);
            fill.lineTo ((float) r.getRight(), (float) r.getBottom());
            fill.lineTo ((float) r.getX(), (float) r.getBottom());
            fill.closeSubPath();
            g.setColour (theme::accent.withAlpha (0.10f));
            g.fillPath (fill);
        }
        else
        {
            // over clips the curve needs a dark halo to stay readable on any event colour
            g.setColour (theme::panelDeep.withAlpha (0.38f));
            g.strokePath (line, juce::PathStrokeType (4.0f));
        }
        g.setColour (theme::accent);
        g.strokePath (line, juce::PathStrokeType (fillBackground ? 1.6f : 2.2f));

        for (int i = 0; i < (int) pts.size(); ++i)
        {
            const float x = xOf (pts[(size_t) i].beat, pxPerBeat, xOffset);
            if (x < (float) r.getX() - 12.0f || x > (float) r.getRight() + 12.0f) continue;
            const float y = yOf (pts[(size_t) i].value, r);
            const bool active = i == activePoint;
            g.setColour (theme::onAccent);
            g.fillEllipse (x - 5.0f, y - 5.0f, 10.0f, 10.0f);
            g.setColour (active ? theme::accentDark : theme::accent);
            g.drawEllipse (x - 5.0f, y - 5.0f, 10.0f, 10.0f, active ? 2.5f : 1.8f);
        }
    }

    // index of the node within 'radius' pixels of p (-1 when none)
    inline int hitPoint (const AutoLane& lane, juce::Point<int> p, juce::Rectangle<int> r,
                         int xOffset, double pxPerBeat, float radius = 9.0f)
    {
        int best = -1;
        float bd = radius;
        for (int i = 0; i < (int) lane.points.size(); ++i)
        {
            const float dx = xOf (lane.points[(size_t) i].beat, pxPerBeat, xOffset) - (float) p.x;
            const float dy = yOf (lane.points[(size_t) i].value, r) - (float) p.y;
            const float d = std::sqrt (dx * dx + dy * dy);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
}
