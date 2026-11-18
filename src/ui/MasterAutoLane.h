#pragma once

#include "ArrangeView.h"
#include "AutoLaneView.h"
#include "TimelineGrid.h"

namespace sx
{
// The lane area of the pinned master bus row (Studio One style): it sits above the scrolling
// track rows, right of the master's track-style header, and shows the master's automation lanes
// as sub-rows of the same height a track uses. Editing is identical to a track lane: click to
// add a node, drag to move it (clamped between its neighbours, so the curve stays monotone),
// right-click to remove it.
class ArrangeView::MasterAutoLane : public juce::Component
{
public:
    explicit MasterAutoLane (ArrangeView& o) : owner (o) { setMouseCursor (drawCrosshairCursor()); }

    //-- geometry -------------------------------------------------------------
    int trackHeight() const { return owner.trackH (master()); }
    int laneHeight() const { return owner.autoH (master()); }
    int laneCount() const { return master().visibleLaneCount(); }
    int rowHeight() const { return trackHeight() + laneCount() * laneHeight(); }

    juce::Rectangle<int> laneRect (int index) const
    {
        return { 0, trackHeight() + index * laneHeight(), getWidth(), laneHeight() - 1 };
    }

    int laneAtY (int y) const
    {
        const int th = trackHeight();
        const int lh = laneHeight();
        if (y < th || lh <= 0) return -1;
        const int i = (y - th) / lh;
        return i >= 0 && i < laneCount() ? i : -1;
    }

    //-- painting -------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        auto& app = owner.app;
        auto& proj = app.project;
        auto& mt = master();
        const int th = trackHeight();
        const int lh = laneHeight();
        const int sx = owner.scrollX();
        auto clip = g.getClipBounds();

        // row body: same alternating lane tint the track rows use for row 0
        g.fillAll (theme::laneA);
        // the row is the top of the timeline: the playhead / grid must line up with the ruler
        grid::paint (g, proj, app.pxPerBeat, clip, -sx, app.arrangeGrid);

        if (proj.loopOn)
        {
            const double ppb = app.pxPerBeat;
            g.setColour (theme::accent.withAlpha (0.05f));
            g.fillRect ((int) (proj.loopStart * ppb) - sx, clip.getY(), (int) ((proj.loopEnd - proj.loopStart) * ppb), clip.getHeight());
            g.setColour (theme::accent.withAlpha (0.45f));
            g.fillRect ((int) (proj.loopStart * ppb) - sx, clip.getY(), 1, clip.getHeight());
            g.fillRect ((int) (proj.loopEnd * ppb) - sx - 1, clip.getY(), 1, clip.getHeight());
        }

        // lane sub-rows, drawn with the shared lane painter so they match the track lanes
        for (int i = 0; i < laneCount(); ++i)
        {
            auto* lane = mt.visibleLaneAt (i);
            if (lane == nullptr) continue;
            auto r = laneRect (i);
            g.setColour (i % 2 == 0 ? theme::panelAlt : theme::panelDeep.withAlpha (0.55f));
            g.fillRect (r);
            autolane::paintRow (g, *lane, r, -sx, app.pxPerBeat, dragging() ? dragPoint : -1);
        }

        // separator under the whole master row + the playhead on top of everything
        g.setColour (theme::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
        const int px = (int) std::round (app.engine.getPositionBeats() * app.pxPerBeat) - sx;
        if (px >= 0 && px < getWidth())
        {
            g.setColour (theme::accent);
            g.fillRect (px, clip.getY(), 2, clip.getHeight());
        }

        if (tipText.isNotEmpty())
        {
            g.setFont (theme::font (12.0f, true));
            const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), tipText) + 12;
            auto r = juce::Rectangle<int> (tipPos.x + 10, juce::jlimit (1, juce::jmax (1, getHeight() - 21), tipPos.y - 26), w, 20);
            g.setColour (theme::tooltipBg);
            g.fillRoundedRectangle (r.toFloat(), 4.0f);
            g.setColour (theme::onAccent);
            g.drawText (tipText, r, juce::Justification::centred);
        }
    }

    //-- mouse ------------------------------------------------------------------
    void mouseMove (const juce::MouseEvent& e) override
    {
        if (updateTip (e.getPosition())) repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (tipText.isNotEmpty()) { tipText.clear(); repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& app = owner.app;
        auto& proj = app.project;
        dragLane = laneAtY (e.y);
        dragPoint = -1;
        if (dragLane < 0) return;
        auto* lane = master().visibleLaneAt (dragLane);
        if (lane == nullptr) return;

        auto r = laneRect (dragLane);
        const int sx = owner.scrollX();
        const float v = autolane::valueAt (e.y, r);
        const int hit = autolane::hitPoint (*lane, e.getPosition(), r, -sx, app.pxPerBeat);

        if (e.mods.isPopupMenu())
        {
            if (hit >= 0)   // right-click removes the node under the cursor
            {
                proj.checkpoint();
                lane->points.erase (lane->points.begin() + hit);
                proj.sendContentChanged();
                app.engine.syncAll();
                repaint();
            }
            return;
        }

        dragTarget = lane->target;
        proj.checkpoint();
        if (hit >= 0)
            dragPoint = hit;
        else
        {
            // a node lands on the grid; two nodes never share a beat, so the curve stays monotone
            const double b = juce::jmax (0.0, snapTo (beatOf (e.x), juce::jmax (0.0625, app.arrangeGrid)));
            dragPoint = insertAutoPoint (lane->points, b, v);
        }
        proj.sendContentChanged();
        app.engine.syncAll();
        updateTip (e.getPosition());
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragLane < 0 || dragPoint < 0) return;
        auto& app = owner.app;
        auto& proj = app.project;
        auto* lane = master().findLane (dragTarget);
        if (lane == nullptr || dragPoint >= (int) lane->points.size()) return;
        auto r = laneRect (dragLane);
        double b = beatOf (e.x);
        if (! e.mods.isAltDown()) b = snapTo (b, juce::jmax (0.0625, app.arrangeGrid));
        moveAutoPoint (lane->points, dragPoint, b, autolane::valueAt (e.y, r));
        proj.sendContentChanged();
        app.engine.syncAll();
        updateTip (e.getPosition());
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging()) repaint();
        dragLane = -1;
        dragPoint = -1;
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            owner.zoomTrackHeights (ArrangeView::notchesOf (w), -1);   // the pinned row is not a track row
        else
            owner.laneViewport.mouseWheelMove (e.getEventRelativeTo (&owner.laneViewport), w);
    }

private:
    Track& master() const { return owner.app.project.masterTrack; }
    bool dragging() const { return dragLane >= 0 && dragPoint >= 0; }
    double beatOf (int x) const { return autolane::beatAt (x, owner.app.pxPerBeat, -owner.scrollX()); }

    bool updateTip (juce::Point<int> p)
    {
        const int laneIdx = laneAtY (p.y);
        juce::String next;
        if (laneIdx >= 0)
            if (auto* lane = master().visibleLaneAt (laneIdx))
            {
                const int hit = autolane::hitPoint (*lane, p, laneRect (laneIdx), -owner.scrollX(), owner.app.pxPerBeat);
                if (hit >= 0)
                {
                    next = autolane::valueText (lane->target, lane->points[(size_t) hit].value);
                    tipPos = p;
                }
            }
        if (next == tipText) return false;
        tipText = next;
        return true;
    }

    ArrangeView& owner;
    int dragLane = -1;      // lane row being edited
    int dragPoint = -1;     // node index being dragged
    juce::String dragTarget;
    juce::String tipText;
    juce::Point<int> tipPos;
};
}
