#include "ArrangeView.h"
#include "AutoLaneView.h"
#include "TimelineGrid.h"
#include "MasterAutoLane.h"
#include "ChordTrack.h"
#include "SlotMenus.h"
#include "Importer.h"
#include "Commands.h"
#include "Waveform.h"

namespace sx
{
using namespace juce;

namespace
{
    String valueText (const AutoLane* lane, const String& target, float v)
    {
        juce::ignoreUnused (lane);
        return autolane::valueText (target, v);
    }

    Colour tint (Colour c, float amount) { return theme::white.interpolatedWith (c, amount); }

    // one folder level indents the header by this many pixels (the folder itself also reserves
    // room for its disclosure arrow, so a child's name starts visibly further right)
    constexpr int kFolderIndentPx = 18;

    // Studio One style preset lengths for fades and crossfades (menu ids are offsets)
    constexpr double kFadePresets[]   = { 0.0, 0.01, 0.05, 0.1, 0.5, 1.0 };
    const char* const kFadePresetNames[] = { "0 s", "10 ms", "50 ms", "100 ms", "500 ms", "1 s" };
    constexpr double kXfadePresets[]  = { 0.01, 0.05, 0.1, 0.25, 0.5, 1.0 };
    const char* const kXfadePresetNames[] = { "10 ms", "50 ms", "100 ms", "250 ms", "500 ms", "1 s" };

    // gain of one half of a crossfade at position 0..1. The first clip ramps 1 -> 0,
    // the second 0 -> 1, and both curves always sum to a smooth (crossfaded) result.
    double xfadeGain (int curve, double t, bool outgoing)
    {
        t = jlimit (0.0, 1.0, t);
        switch (curve)
        {
            case 1:  return outgoing ? 1.0 - t : t;                                  // linear
            case 2:                                                                   // symmetric S-curve
            {
                const double s = t * t * (3.0 - 2.0 * t);
                return outgoing ? 1.0 - s : s;
            }
            default:
            {
                const double ang = 1.5707963267948966 * t;                            // equal power
                return outgoing ? std::cos (ang) : std::sin (ang);
            }
        }
    }

    // overlapping clip pairs covered by the current selection: the adjacent selected
    // clips, or the crossfade partner of a single selected clip, so that right-clicking
    // one side of a crossfade can size / reshape it
    std::vector<std::pair<AudioClip*, AudioClip*>> selectedXfadePairs (Project& proj, const std::set<int>& sel)
    {
        std::vector<std::pair<AudioClip*, AudioClip*>> out;
        for (auto& t : proj.tracks)
        {
            std::vector<AudioClip*> chosen;
            for (auto& c : t->audioClips) if (sel.count (c.id)) chosen.push_back (&c);
            if (chosen.empty()) continue;
            std::sort (chosen.begin(), chosen.end(),
                       [] (const AudioClip* a, const AudioClip* b) { return a->start < b->start; });
            const auto overlaps = [&] (AudioClip& a, AudioClip& b)
            {
                if (b.start <= a.start) return false;
                return proj.beatsToSeconds (a.start) + a.lengthSec - proj.beatsToSeconds (b.start) > 0.001;
            };
            if (chosen.size() == 1)
            {
                auto* c = chosen.front();
                for (auto& o : t->audioClips)
                {
                    if (o.id == c->id) continue;
                    if (overlaps (*c, o)) { out.push_back ({ c, &o }); break; }
                    if (overlaps (o, *c)) { out.push_back ({ &o, c }); break; }
                }
            }
            else
                for (size_t i = 0; i + 1 < chosen.size(); ++i)
                    if (overlaps (*chosen[i], *chosen[i + 1])) out.push_back ({ chosen[i], chosen[i + 1] });
        }
        return out;
    }

    struct ClipHit
    {
        int trackIdx = -1;
        Track* track = nullptr;
        int clipId = 0;
        bool audio = false;
        Rectangle<float> rect;
        explicit operator bool() const { return track != nullptr; }
    };
}

//==============================================================================
class ArrangeView::Ruler : public Component, public SettableTooltipClient
{
public:
    explicit Ruler (ArrangeView& o) : owner (o)
    {
        setMouseCursor (MouseCursor::PointingHandCursor);
        setTooltip (TRANS ("Click the bar strip to move the playhead; drag the bar strip to scroll the arrangement. Drag the loop strip to set / resize the cycle."));
    }

    void paint (Graphics& g) override
    {
        auto& app = owner.app;
        auto& p = app.project;
        const double ppb = app.pxPerBeat;
        const int sx = owner.scrollX();
        g.fillAll (theme::panelAlt);

        const double b0 = (double) sx / ppb, b1 = (double) (sx + getWidth()) / ppb;
        std::vector<tempomap::BarInfo> bars;
        tempomap::barStartsBetween (p.tempo, b0, b1, bars);
        // prepend the bar that contains the left edge so its label stays visible when scrolled
        if (bars.empty() || bars.front().beat > b0 + 1.0e-6)
        {
            std::vector<tempomap::BarInfo> head;
            tempomap::barStartsBetween (p.tempo, jmax (0.0, b0 - 96.0), b1, head);
            bars = head;
        }

        g.setFont (theme::font (11.5f));
        if (app.timeInSeconds)
        {
            // seconds timebase: the ruler is a time scale (ticks and m:ss labels) instead of bar
            // numbers. The grid of the lanes stays musical, only what the ruler says changes.
            const double sec0 = p.beatsToSeconds (b0), sec1 = p.beatsToSeconds (b1);
            const double span = jmax (1.0e-3, sec1 - sec0);
            const double step = span > 240.0 ? 60.0 : span > 120.0 ? 30.0 : span > 60.0 ? 15.0 : span > 24.0 ? 10.0
                              : span > 12.0 ? 5.0 : span > 5.0 ? 2.0 : 1.0;

            for (double t = std::floor (sec0 / step) * step; t <= sec1 + 1.0e-9; t += step)
            {
                const float x = (float) (p.secondsToBeats (jmax (0.0, t)) * ppb - sx);
                g.setColour (theme::gridBar);
                g.fillRect (x, 2.0f, 1.0f, (float) kTopH - 2.0f);
                g.setColour (theme::textDim);
                g.drawText (tempomap::timeLabel (t), (int) x + 3, 1, 80, kTopH - 4, Justification::centredLeft);
            }
        }
        else
        for (size_t i = 0; i < bars.size(); ++i)
        {
            const auto& b = bars[i];
            const double w = (double) b.num * 4.0 / (double) b.den;
            const float x = (float) (b.beat * ppb - sx);

            g.setColour (theme::gridBar);
            g.fillRect (x, (float) kTopH - 12.0f, 1.0f, 12.0f);
            g.fillRect (x, 2.0f, 1.0f, (float) kTopH - 2.0f);

            const double beatLen = 4.0 / (double) b.den;
            if (beatLen * ppb >= 14.0)
                for (double bt = beatLen; bt < w - 1.0e-6; bt += beatLen)
                {
                    g.setColour (theme::gridBeat);
                    g.fillRect ((float) ((b.beat + bt) * ppb - sx), (float) kTopH - 6.0f, 1.0f, 6.0f);
                }

            // tempo / meter segments exactly on a bar start get a marker chip
            TempoSegment* seg = p.findTempoSegment (b.beat);
            if (seg != nullptr)
            {
                auto chip = Rectangle<float> (x + 2.0f, 1.0f, 46.0f, 13.0f);
                g.setColour (theme::accentSoft);
                g.fillRoundedRectangle (chip, 3.0f);
                g.setColour (theme::accentDark);
                g.setFont (theme::font (9.5f, true));
                g.drawText (String (seg->bpm, seg->bpm == std::floor (seg->bpm) ? 0 : 1) + (seg->ramp ? " /" : ""),
                            chip.toNearestInt(), Justification::centred);
                auto mchip = Rectangle<float> (chip.getRight() + 2.0f, 1.0f, 32.0f, 13.0f);
                g.setColour (theme::white);
                g.fillRoundedRectangle (mchip, 3.0f);
                g.setColour (theme::accentDark);
                g.setFont (theme::font (9.5f, true));
                g.drawText (String (b.num) + "/" + String (b.den), mchip.toNearestInt(), Justification::centred);
                continue;   // chip replaces the bar number
            }

            g.setColour (theme::textDim);
            if (w * ppb >= 44.0)
                g.drawText (String (tempomap::barPosition (p.tempo, b.beat).bar + 1), (int) x + 4, 2, 60, kTopH - 6,
                            Justification::centredLeft);
        }

        // loop strip
        auto strip = Rectangle<float> (0.0f, (float) kTopH, (float) getWidth(), (float) getHeight() - kTopH);
        g.setColour (theme::lineSoft);
        g.fillRect (strip.withHeight (1.0f));
        const float lx0 = (float) (p.loopStart * ppb - sx), lx1 = (float) (p.loopEnd * ppb - sx);
        auto loopR = Rectangle<float> (lx0, strip.getY() + 3.0f, jmax (2.0f, lx1 - lx0), strip.getHeight() - 5.0f);
        g.setColour (p.loopOn ? theme::accent.withAlpha (0.85f) : theme::textFaint.withAlpha (0.16f));
        g.fillRoundedRectangle (loopR, 3.0f);
        // the two edges are the resize handles: a brighter bar makes it obvious that the cycle
        // can be dragged, even while the loop is switched off
        g.setColour (p.loopOn ? theme::accentDark : theme::textDim.withAlpha (0.55f));
        g.fillRect (lx0, loopR.getY(), 2.0f, loopR.getHeight());
        g.fillRect (lx1 - 2.0f, loopR.getY(), 2.0f, loopR.getHeight());
        if (p.loopOn)
        {
            g.setColour (theme::onAccent);
            g.setFont (theme::font (10.0f, true));
            g.drawText (TRANS ("CYCLE"), loopR.toNearestInt().reduced (4, 0), Justification::centredLeft);
        }

        g.setColour (theme::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        // playhead flag
        const float px = (float) (owner.app.engine.getPositionBeats() * ppb - sx);
        g.setColour (theme::accent);
        Path tri;
        tri.addTriangle (px - 5.0f, 0.0f, px + 5.0f, 0.0f, px, 8.0f);
        g.fillPath (tri);
        g.fillRect (px - 0.5f, 0.0f, 1.0f, (float) getHeight());
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto& p = owner.app.project;
        const double ppb = owner.app.pxPerBeat;
        const double beat = ((double) e.x + owner.scrollX()) / ppb;
        panMoved = false;

        // middle button (or Alt/Ctrl+left) pans without a click-seek; a plain left drag on the bar
        // strip also pans, while a plain click moves the playhead on release
        if (e.mods.isMiddleButtonDown() || ((e.mods.isAltDown() || e.mods.isCtrlDown()) && e.y < kTopH))
        {
            mode = Mode::Pan;
            downPos = e.getPosition();
            panStartScrollX = owner.laneViewport.getViewPositionX();
            panMoved = false;
            setMouseCursor (MouseCursor::DraggingHandCursor);
            return;
        }

        // right click on the bar strip: tempo / meter changes
        if (e.mods.isPopupMenu() && e.y < kTopH)
        {
            const double at = jmax (0.0, tempomap::barStartAtOrBefore (p.tempo, beat));
            auto* proj = &owner.app.project;
            TempoSegment* seg = proj->findTempoSegment (at);
            PopupMenu m;
            m.addItem (1, TRANS ("Insert Tempo Change..."));
            m.addItem (2, TRANS ("Insert Meter Change..."));
            if (seg != nullptr && seg->beat > 1.0e-6)
                m.addItem (3, TRANS ("Remove Tempo Change"), true);
            if (seg != nullptr && &proj->tempo.back() != seg)
                m.addItem (4, TRANS ("Linear Ramp to Next Point"), true, seg->ramp);
            m.showMenuAsync (PopupMenu::Options(), [proj, at] (int r)
            {
                if (r == 4)
                {
                    proj->checkpoint();
                    if (auto* s = proj->findTempoSegment (at)) proj->setTempoRamp (at, ! s->ramp);
                    proj->sendContentChanged();
                    return;
                }
                if (r == 1)
                {
                    const double cur = proj->bpmAt (at);
                    promptText (TRANS ("Insert Tempo Change"), TRANS ("Tempo in BPM (20 - 400)"), String (cur, 1),
                                [proj, at] (String s)
                    {
                        const double v = s.trim().getDoubleValue();
                        if (v < 20.0 || v > 400.0) return;
                        proj->checkpoint();
                        proj->setTempo (at, v, 0, 0);
                        proj->sendContentChanged();
                    });
                }
                else if (r == 2)
                {
                    const struct S { int n, d; } sigs[] = { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 },
                                                            { 3, 8 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 }, { 2, 2 } };
                    PopupMenu sm;
                    int id = 1, cn, cd;
                    proj->meterAt (at, cn, cd);
                    for (auto& s : sigs)
                        sm.addItem (id++, String (s.n) + "/" + String (s.d), true, cn == s.n && cd == s.d);
                    sm.showMenuAsync (PopupMenu::Options(), [proj, at] (int r2)
                    {
                        if (r2 <= 0 || r2 > 12) return;
                        static const int sg[12][2] = { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 },
                                                       { 3, 8 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 }, { 2, 2 } };
                        proj->checkpoint();
                        int bn, bd;
                        proj->meterAt (at, bn, bd);
                        proj->setTempo (at, proj->bpmAt (at), sg[r2 - 1][0], sg[r2 - 1][1]);
                        proj->sendContentChanged();
                    });
                }
                else if (r == 3)
                {
                    proj->checkpoint();
                    proj->removeTempo (at);
                    proj->sendContentChanged();
                }
            });
            mode = Mode::None;
            return;
        }

        // the lower strip is the loop lane: dragging anywhere on it edits the cycle directly
        // (no modifier needed - a user should not have to discover Shift to resize a cycle)
        if (e.mods.isPopupMenu()) { mode = Mode::None; return; }
        if (e.y >= kTopH)
        {
            const double e0 = p.loopStart * ppb - owner.scrollX(), e1 = p.loopEnd * ppb - owner.scrollX();
            const double edge = 8.0;
            if (std::abs (e.x - e1) < edge) mode = Mode::LoopEnd;
            else if (std::abs (e.x - e0) < edge) mode = Mode::LoopStart;
            else if (e.x > e0 && e.x < e1) { mode = Mode::LoopMove; moveOffset = beat - p.loopStart; }
            else
            {
                mode = Mode::LoopEnd;
                p.loopStart = jmax (0.0, snapTo (beat, owner.app.arrangeGrid));
                p.loopEnd = p.loopStart + jmax (0.25, owner.app.arrangeGrid);
                p.loopOn = true;
                p.sendContentChanged();
            }
            setMouseCursor (mode == Mode::LoopMove ? MouseCursor::DraggingHandCursor : MouseCursor::LeftRightResizeCursor);
            return;
        }

        // a click on the bar strip places the playhead right away - a seek deferred to mouse-up
        // feels dead, and a few pixels of hand tremor would turn the gesture into a pan that
        // discards it. A drag on the same strip moves the visible timeline instead.
        mode = Mode::Pan;
        downPos = e.getPosition();
        panStartScrollX = owner.laneViewport.getViewPositionX();
        panMoved = false;
        owner.app.engine.seekToBeats (jmax (0.0, snapTo (beat, owner.app.arrangeGrid)));
        setMouseCursor (MouseCursor::DraggingHandCursor);
    }

    void mouseDrag (const MouseEvent& e) override
    {
        auto& p = owner.app.project;

        if (mode == Mode::Pan)
        {
            const int dx = e.x - downPos.x;
            if (std::abs (dx) > 3 || std::abs (e.y - downPos.y) > 3) panMoved = true;
            if (panMoved)
                owner.laneViewport.setViewPosition (jmax (0, panStartScrollX - dx),
                                                    owner.laneViewport.getViewPositionY());
            return;
        }

        const double beat = ((double) e.x + owner.scrollX()) / owner.app.pxPerBeat;
        const double g = owner.app.arrangeGrid;
        const double minLen = g > 0 ? g : 0.25;
        switch (mode)
        {
            case Mode::LoopEnd:   p.loopEnd = jmax (p.loopStart + minLen, snapTo (beat, g)); p.sendContentChanged(); break;
            case Mode::LoopStart: p.loopStart = jlimit (0.0, p.loopEnd - minLen, snapTo (beat, g)); p.sendContentChanged(); break;
            case Mode::LoopMove:
            {
                const double len = p.loopEnd - p.loopStart;
                p.loopStart = jmax (0.0, snapTo (beat - moveOffset, g));
                p.loopEnd = p.loopStart + len;
                p.sendContentChanged();
                break;
            }
            default: break;
        }
    }

    void mouseUp (const MouseEvent&) override
    {
        panMoved = false;
        mode = Mode::None;
        setMouseCursor (MouseCursor::PointingHandCursor);
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (e.y >= kTopH)
        {
            owner.app.project.loopOn = ! owner.app.project.loopOn;
            owner.app.project.sendContentChanged();
        }
    }

    void mouseMove (const MouseEvent& e) override
    {
        auto& p = owner.app.project;
        const double ppb = owner.app.pxPerBeat;
        if (e.y < kTopH)
        {
            // the bar strip is both a click-to-seek target and a drag-to-scroll handle
            setMouseCursor (MouseCursor::DraggingHandCursor);
            return;
        }
        const double e0 = p.loopStart * ppb - owner.scrollX(), e1 = p.loopEnd * ppb - owner.scrollX();
        if (std::abs (e.x - e1) < 8 || std::abs (e.x - e0) < 8)
            setMouseCursor (MouseCursor::LeftRightResizeCursor);
        else if (e.x > e0 && e.x < e1)
            setMouseCursor (MouseCursor::DraggingHandCursor);
        else
            setMouseCursor (MouseCursor::CrosshairCursor);
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        // over the bar ruler the wheel zooms the timeline, anchored at the pointer
        if (e.mods.isCtrlDown())
            owner.zoomTrackHeights (ArrangeView::notchesOf (w), -1);
        else if (e.mods.isShiftDown())
            owner.laneViewport.setViewPosition (jmax (0, owner.scrollX() - (int) (w.deltaY * 300.0f)),
                                                owner.laneViewport.getViewPositionY());
        else
            owner.zoomPxPerBeat (ArrangeView::notchesOf (w), e.x + owner.scrollX());
        owner.ruler->repaint();
    }

    static constexpr int kTopH = 24;

private:
    enum class Mode { None, LoopEnd, LoopStart, LoopMove, Pan };
    ArrangeView& owner;
    Mode mode = Mode::None;
    double moveOffset = 0.0;
    Point<int> downPos;
    int panStartScrollX = 0;
    bool panMoved = false;
};

//==============================================================================
//==============================================================================
// Tempo track: the tempo map drawn and edited like an automation lane. Points are tempo
// segments; a segment with 'ramp' set changes linearly to the next point's bpm.
class ArrangeView::TempoLane : public Component
{
public:
    explicit TempoLane (ArrangeView& o) : owner (o) { setMouseCursor (drawCrosshairCursor()); }

    void paint (Graphics& g) override
    {
        auto& p = owner.app.project;
        const double ppb = owner.app.pxPerBeat;
        const int sx = owner.scrollX();
        g.fillAll (theme::laneA);

        if (! dragging) computeRange (lo, hi);

        // bar grid
        const double b0 = (double) sx / ppb, b1 = (double) (sx + getWidth()) / ppb;
        std::vector<tempomap::BarInfo> bars;
        tempomap::barStartsBetween (p.tempo, jmax (0.0, b0 - 96.0), b1, bars);
        g.setColour (theme::gridBeat.withAlpha (0.6f));
        for (auto& b : bars) g.fillRect ((float) (b.beat * ppb - sx), 0.0f, 1.0f, (float) getHeight());

        // bpm guides
        g.setFont (theme::font (9.5f));
        // guide spacing: the smallest "nice" step that keeps labels at least 20 px apart
        double step = 10.0;
        for (double cand : { 10.0, 20.0, 25.0, 50.0, 100.0, 200.0 })
        {
            step = cand;
            if (plotArea().getHeight() * cand / jmax (1.0, hi - lo) >= 20.0) break;
        }
        for (double v = std::ceil (lo / step) * step; v <= hi + 1.0e-6; v += step)
        {
            const float y = bpmToY (v);
            g.setColour (theme::lineSoft);
            g.fillRect (0.0f, y, (float) getWidth(), 1.0f);
            g.setColour (theme::textFaint);
            g.drawText (String ((int) v), 4, (int) y - 11, 40, 11, Justification::bottomLeft);
        }

        // the curve: hold or linear ramp between points, extended to the right edge
        const auto& tm = p.tempo;
        Path line;
        for (size_t i = 0; i < tm.size(); ++i)
        {
            const float x = (float) (tm[i].beat * ppb - sx), y = bpmToY (tm[i].bpm);
            if (i == 0)
            {
                line.startNewSubPath (jmin (x, -2.0f), y);
                if (x > -2.0f) line.lineTo (x, y);
            }
            else line.lineTo (x, y);   // vertical jump on a step change, no-op after a ramp
            if (i + 1 < tm.size())
            {
                const float nx = (float) (tm[i + 1].beat * ppb - sx);
                line.lineTo (nx, tm[i].ramp ? bpmToY (tm[i + 1].bpm) : y);
            }
            else
                line.lineTo ((float) getWidth() + 2.0f, y);
        }
        Path fill (line);
        fill.lineTo ((float) getWidth() + 2.0f, (float) getHeight());
        fill.lineTo (-2.0f, (float) getHeight());
        fill.closeSubPath();
        g.setColour (theme::accent.withAlpha (0.10f));
        g.fillPath (fill);
        g.setColour (theme::accent);
        g.strokePath (line, PathStrokeType (1.6f));

        // points (meter changes are squares: they can only move vertically)
        for (size_t i = 0; i < tm.size(); ++i)
        {
            const float x = (float) (tm[i].beat * ppb - sx), y = bpmToY (tm[i].bpm);
            if (x < -8.0f || x > (float) getWidth() + 8.0f) continue;
            const bool active = (int) i == dragIdx || (int) i == hoverIdx;
            auto box = Rectangle<float> (x - 5.0f, y - 5.0f, 10.0f, 10.0f);
            g.setColour (tm[i].ramp ? theme::accentSoft : theme::white);
            if (isMeterPoint ((int) i)) g.fillRect (box); else g.fillEllipse (box);
            g.setColour (active ? theme::accentDark : theme::accent);
            if (isMeterPoint ((int) i)) g.drawRect (box, active ? 2.5f : 1.8f);
            else g.drawEllipse (box, active ? 2.5f : 1.8f);
        }

        // value tip for the hovered / dragged point
        const int tipIdx = dragIdx >= 0 ? dragIdx : hoverIdx;
        if (tipIdx >= 0 && tipIdx < (int) tm.size())
        {
            const auto& s = tm[(size_t) tipIdx];
            const float x = (float) (s.beat * ppb - sx), y = bpmToY (s.bpm);
            const String txt = String (s.bpm, 1) + " BPM   " + TRANS ("Bar") + " " + String (tempomap::barPosition (p.tempo, s.beat).bar + 1)
                               + (s.ramp ? "  " + TRANS ("Ramp") : String());
            auto tip = Rectangle<float> (x + 9.0f, jlimit (1.0f, (float) getHeight() - 17.0f, y - 20.0f), 164.0f, 16.0f);
            if (tip.getRight() > (float) getWidth()) tip.setX (x - 173.0f);
            g.setColour (theme::accentDark);
            g.fillRoundedRectangle (tip, 3.0f);
            g.setColour (theme::onAccent);
            g.setFont (theme::font (10.5f, true));
            g.drawText (txt, tip.toNearestInt().reduced (5, 0), Justification::centredLeft);
        }
        else if (tm.size() == 1 && ! tm.front().ramp)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f));
            g.drawText (TRANS ("Click to add tempo points - lines between points ramp linearly. Right-click a point for options."),
                        Rectangle<int> (48, 1, getWidth() - 56, 14), Justification::centredLeft);
        }

        g.setColour (theme::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
        const float px = (float) (owner.app.engine.getPositionBeats() * ppb - sx);
        g.setColour (theme::accent);
        g.fillRect (px - 0.5f, 0.0f, 1.0f, (float) getHeight());
    }

    void mouseMove (const MouseEvent& e) override
    {
        const int h = hitPoint (e.getPosition());
        if (h != hoverIdx) { hoverIdx = h; repaint(); }
        setMouseCursor (h >= 0 ? MouseCursor::DraggingHandCursor : drawCrosshairCursor());
    }

    void mouseExit (const MouseEvent&) override { if (hoverIdx >= 0) { hoverIdx = -1; repaint(); } }

    void mouseDown (const MouseEvent& e) override
    {
        auto& p = owner.app.project;
        computeRange (lo, hi);
        const int hit = hitPoint (e.getPosition());
        if (e.mods.isPopupMenu()) { showMenu (hit, xToBeat (e.x)); return; }

        p.checkpoint();
        if (hit >= 0)
            dragIdx = hit;
        else
        {
            // add a point. Like automation, the line from the previous point ramps into it;
            // Shift+click adds a plain step change instead.
            const double beat = jmax (1.0e-3, snapBeat (xToBeat (e.x), e.mods.isAltDown()));
            if (p.findTempoSegment (beat) != nullptr) { dragIdx = indexAt (beat); }
            else
            {
                const auto* prev = p.findTempoSegmentAt (beat);
                const bool prevRamp = prev != nullptr && prev->ramp;
                const double prevBeat = prev != nullptr ? prev->beat : 0.0;
                p.setTempo (beat, yToBpm (e.y), 0, 0);
                if (! e.mods.isShiftDown())
                {
                    p.setTempoRamp (prevBeat, true);
                    p.setTempoRamp (beat, prevRamp);
                }
                dragIdx = indexAt (beat);
            }
            p.sendContentChanged();
        }
        dragging = true;
        repaint();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        auto& p = owner.app.project;
        if (dragIdx < 0 || dragIdx >= (int) p.tempo.size()) return;
        // the tempo lane edits a segment in place, so it has to bracket it the same way setTempo()
        // does: clips with timebase Seconds keep their absolute position
        const Project::TempoEdit keepSeconds (p);
        auto& s = p.tempo[(size_t) dragIdx];
        s.bpm = std::round (yToBpm (e.y) * 10.0) / 10.0;
        // horizontal: tempo-only points between their neighbours; the first point and meter
        // changes stay put so the bar grid never moves under the user
        if (dragIdx > 0 && ! isMeterPoint (dragIdx))
        {
            const double minB = p.tempo[(size_t) dragIdx - 1].beat + 1.0 / 16.0;
            const double maxB = dragIdx + 1 < (int) p.tempo.size() ? p.tempo[(size_t) dragIdx + 1].beat - 1.0 / 16.0 : 1.0e9;
            if (maxB > minB) s.beat = jlimit (minB, maxB, snapBeat (xToBeat (e.x), e.mods.isAltDown()));
        }
        p.syncLegacyTempo();
        p.sendContentChanged();
        repaint();
    }

    void mouseUp (const MouseEvent&) override
    {
        auto& p = owner.app.project;
        if (dragging)
        {
            tempomap::normalize (p.tempo);
            p.syncLegacyTempo();
            p.sendContentChanged();
        }
        dragging = false;
        dragIdx = -1;
        repaint();
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        const int hit = hitPoint (e.getPosition());
        if (hit >= 0) promptBpm (owner.app.project.tempo[(size_t) hit].beat);
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            owner.zoomTrackHeights (ArrangeView::notchesOf (w), e.y);
        else
            owner.laneViewport.mouseWheelMove (e.getEventRelativeTo (&owner.laneViewport), w);
    }

private:
    Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced (0.0f, 9.0f); }
    float bpmToY (double v) const
    {
        auto a = plotArea();
        return a.getBottom() - (float) ((v - lo) / jmax (1.0, hi - lo)) * a.getHeight();
    }
    double yToBpm (int y) const
    {
        auto a = plotArea();
        return jlimit (20.0, 400.0, lo + (double) (a.getBottom() - (float) y) / a.getHeight() * (hi - lo));
    }
    double xToBeat (int x) const { return ((double) x + owner.scrollX()) / owner.app.pxPerBeat; }
    double snapBeat (double b, bool free) const { return free ? b : snapTo (b, owner.app.arrangeGrid); }

    // the visible bpm range, padded around the map; frozen while dragging
    void computeRange (double& l, double& h) const
    {
        double mn = 1.0e9, mx = -1.0e9;
        for (auto& s : owner.app.project.tempo) { mn = jmin (mn, s.bpm); mx = jmax (mx, s.bpm); }
        l = jmax (20.0, std::floor ((mn - 30.0) / 10.0) * 10.0);
        h = jmin (400.0, std::ceil ((mx + 30.0) / 10.0) * 10.0);
        if (h - l < 80.0) { const double c = (l + h) * 0.5; l = jmax (20.0, c - 40.0); h = l + 80.0; }
    }

    bool isMeterPoint (int i) const
    {
        auto& tm = owner.app.project.tempo;
        return i > 0 && i < (int) tm.size()
               && (tm[(size_t) i].tsNum != tm[(size_t) i - 1].tsNum || tm[(size_t) i].tsDen != tm[(size_t) i - 1].tsDen);
    }

    int indexAt (double beat) const
    {
        auto& tm = owner.app.project.tempo;
        for (size_t i = 0; i < tm.size(); ++i) if (std::abs (tm[i].beat - beat) < 1.0e-6) return (int) i;
        return -1;
    }

    int hitPoint (Point<int> pt) const
    {
        auto& tm = owner.app.project.tempo;
        const double ppb = owner.app.pxPerBeat;
        const int sx = owner.scrollX();
        int best = -1;
        float bd = 9.0f;
        for (size_t i = 0; i < tm.size(); ++i)
        {
            const float dx = (float) (tm[i].beat * ppb - sx) - (float) pt.x;
            const float dy = bpmToY (tm[i].bpm) - (float) pt.y;
            const float d = std::sqrt (dx * dx + dy * dy);
            if (d < bd) { bd = d; best = (int) i; }
        }
        return best;
    }

    void promptBpm (double beat)
    {
        auto* proj = &owner.app.project;
        promptText (TRANS ("Set Tempo"), TRANS ("Tempo in BPM (20 - 400)"), String (proj->bpmAt (beat), 1), [proj, beat] (String s)
        {
            const double v = s.trim().getDoubleValue();
            if (v < 20.0 || v > 400.0) return;
            proj->checkpoint();
            proj->setTempo (beat, v, 0, 0);
            proj->sendContentChanged();
        });
    }

    void showMenu (int hit, double beat)
    {
        auto* proj = &owner.app.project;
        auto& tm = proj->tempo;
        // a click on the line edits the segment under the mouse
        const double at = hit >= 0 ? tm[(size_t) hit].beat : (proj->findTempoSegmentAt (beat) != nullptr ? proj->findTempoSegmentAt (beat)->beat : 0.0);
        const int idx = indexAt (at);
        const bool hasNext = idx >= 0 && idx + 1 < (int) tm.size();
        const bool ramp = idx >= 0 && tm[(size_t) idx].ramp;
        PopupMenu m;
        m.addItem (1, TRANS ("Linear Ramp to Next Point"), hasNext, ramp);
        m.addItem (2, TRANS ("Hold Tempo (Step)"), hasNext, ! ramp);
        m.addSeparator();
        m.addItem (3, TRANS ("Set Tempo..."), idx >= 0);
        if (hit < 0) m.addItem (4, TRANS ("Add Tempo Point Here"));
        m.addItem (5, TRANS ("Remove Tempo Point"), hit > 0);
        const double addAt = jmax (1.0e-3, snapBeat (beat, false));
        SafePointer<TempoLane> self (this);
        m.showMenuAsync (PopupMenu::Options(), [self, proj, at, addAt] (int r)
        {
            if (r == 1 || r == 2) { proj->checkpoint(); proj->setTempoRamp (at, r == 1); proj->sendContentChanged(); }
            else if (r == 3) { if (self != nullptr) self->promptBpm (at); }
            else if (r == 4) { proj->checkpoint(); proj->setTempo (addAt, proj->bpmAt (addAt), 0, 0); proj->sendContentChanged(); }
            else if (r == 5) { proj->checkpoint(); proj->removeTempo (at); proj->sendContentChanged(); }
        });
    }

    ArrangeView& owner;
    double lo = 60.0, hi = 180.0;
    int dragIdx = -1, hoverIdx = -1;
    bool dragging = false;
};

class ArrangeView::Lanes : public Component
{
public:
    struct Item { int clipId; bool audio; int trackId; int origTrackId; double origStart; double origLen; double origOffset; double origLenSec; };

    explicit Lanes (ArrangeView& o) : owner (o) { setWantsKeyboardFocus (false); }

    //-- painting ---------------------------------------------------------------
    void paint (Graphics& g) override
    {
        auto clip = g.getClipBounds();
        auto& app = owner.app;
        auto& proj = app.project;
        const double ppb = app.pxPerBeat;
        const int n = (int) proj.tracks.size();

        g.fillAll (theme::panelAlt);
        for (int i = 0; i < n; ++i)
        {
            auto& t = *proj.tracks[(size_t) i];
            if (! t.shownInArranger()) continue;
            const int y0 = owner.rowTop (i), h = owner.rowHeight (t);
            if (y0 + h < clip.getY() || y0 > clip.getBottom()) continue;
            g.setColour (i % 2 == 0 ? theme::laneA : theme::laneB);
            g.fillRect (clip.getX(), y0, clip.getWidth(), h);
            if (app.selectedTracks.count (t.id) > 0)
            {
                g.setColour (theme::accent.withAlpha (0.045f));
                g.fillRect (clip.getX(), y0, clip.getWidth(), h);
            }
            // packed tracks get a faint tint of the folder's colour so the hierarchy is visible
            // on the timeline side too, not only from the indented header
            if (t.folderParent != 0)
                if (auto* folder = proj.findTrack (t.folderParent))
                {
                    g.setColour (folder->colour.withAlpha (0.035f));
                    g.fillRect (clip.getX(), y0, clip.getWidth(), h);
                }
            g.setColour (theme::lineSoft);
            g.fillRect (clip.getX(), y0 + h - 1, clip.getWidth(), 1);
        }

        if (n == 0)
        {
            // the lanes are as wide as the zoom makes them, so the hint is centred in what the
            // viewport shows - not in the whole (possibly very wide) scrolling area
            g.setColour (theme::textFaint);
            g.setFont (theme::font (15.0f));
            auto vis = Rectangle<int> (owner.scrollX(), 0, jmax (200, owner.laneViewport.getMaximumVisibleWidth()), 120);
            g.drawText (TRANS ("No tracks. Use the + button or the Track menu to add one, or drop MIDI / audio files here."),
                        vis, Justification::centred);
        }
        else if (owner.app.hiddenTrackCount() == n)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (15.0f));
            g.drawText (TRANS ("All tracks are hidden - Track menu > Show All Hidden Tracks (Ctrl+Shift+H)"),
                        getLocalBounds().withHeight (120), Justification::centred);
        }

        paintGrid (g, clip);

        // drop indicator while a track header is being dragged
        if (owner.reorderDropIndex >= 0)
        {
            const int y = owner.reorderDropIndex >= n ? owner.totalRowsHeight() : owner.rowTop (owner.reorderDropIndex);
            g.setColour (theme::accent);
            g.fillRect ((float) clip.getX(), (float) y - 1.0f, (float) clip.getWidth(), 2.0f);
        }

        for (int i = 0; i < n; ++i)
        {
            auto& t = *proj.tracks[(size_t) i];
            if (! t.shownInArranger()) continue;
            const int y0 = owner.rowTop (i), h = owner.rowHeight (t);
            if (y0 + h < clip.getY() || y0 > clip.getBottom()) continue;

            if (t.isAutomation())
            {
                // a stand-alone automation track is one row-wide curve, not a clip lane
                if (auto* lane = t.automationLane())
                {
                    const auto row = autoRect (i, 0);
                    g.saveState();
                    g.reduceClipRegion (row);
                    autolane::paintRow (g, *lane, row, 0, ppb, activeAutoPoint (t, *lane));
                    g.restoreState();
                }
                continue;
            }

            // Studio One style: when a channel shows automation in its track row, the clips stay
            // visible but dimmed so the curve reads on top of them. In automation edit mode they
            // fade further back (the row then behaves like an automation lane).
            auto* overlayLane = t.inlineAutomationLane();
            const float overlayOpacity = t.automationEdit ? 0.16f : 0.38f;
            if (overlayLane != nullptr) { g.saveState(); g.setOpacity (overlayOpacity); }
            for (auto& c : t.audioClips)
            {
                if (c.takeGroup != 0 && ! c.takeActive) continue;   // only the selected take is shown
                paintAudio (g, t, c, i, clip);
            }
            paintCrossfades (g, t, i, clip);
            for (auto& c : t.midiClips)
            {
                if (c.takeGroup != 0 && ! c.takeActive) continue;
                paintMidi (g, t, c, i, clip);
            }
            drawTakeBadges (g, t, i, clip);
            if (overlayLane != nullptr) g.restoreState();

            if (overlayLane != nullptr)
            {
                const auto row = rowBodyRect (i);
                g.saveState();
                g.reduceClipRegion (row);
                autolane::paintRow (g, *overlayLane, row, 0, ppb,
                                    activeAutoPoint (t, *overlayLane), false);
                g.restoreState();
            }

            for (int ln = 0; ln < t.laneRowCount(); ++ln)
                paintAutomation (g, t, i, ln, clip);
        }

        // loop range tint
        if (proj.loopOn)
        {
            g.setColour (theme::accent.withAlpha (0.05f));
            g.fillRect ((int) (proj.loopStart * ppb), clip.getY(), (int) ((proj.loopEnd - proj.loopStart) * ppb), clip.getHeight());
            g.setColour (theme::accent.withAlpha (0.45f));
            g.fillRect ((int) (proj.loopStart * ppb), clip.getY(), 1, clip.getHeight());
            g.fillRect ((int) (proj.loopEnd * ppb) - 1, clip.getY(), 1, clip.getHeight());
        }

        // the scissors tool shows where it would cut: a dashed line at the mouse position
        if (owner.app.arrangeTool == ArrangeTool::Scissors && owner.cursorBeat >= 0.0 && mode == Mode::None)
        {
            const float cx = (float) (owner.cursorBeat * ppb);
            g.setColour (theme::accent.withAlpha (0.7f));
            for (int y = clip.getY(); y < clip.getBottom(); y += 6)
                g.fillRect (cx, (float) y, 1.0f, 3.0f);
        }

        if (mode == Mode::Rubber)
        {
            g.setColour (theme::accent.withAlpha (0.12f));
            g.fillRect (rubber);
            g.setColour (theme::accent);
            g.drawRect (rubber, 1);
        }

        if (mode == Mode::DrawClip)
        {
            auto* t = proj.findTrack (drawTrackId);
            if (t != nullptr)
            {
                const int idx = proj.indexOfTrack (drawTrackId);
                if (idx >= 0)
                {
                    auto r = Rectangle<float> ((float) (jmin (drawStart, drawEnd) * ppb), (float) owner.rowTop (idx) + 3.0f,
                                               (float) jmax (3.0, std::abs (drawEnd - drawStart) * ppb),
                                               (float) owner.trackH (*t) - 7.0f);
                    g.setColour (theme::accent.withAlpha (0.15f));
                    g.fillRoundedRectangle (r, 3.5f);
                    g.setColour (theme::accent);
                    g.drawRoundedRectangle (r, 3.5f, 1.5f);
                }
            }
        }

        if (autoTip.isNotEmpty())
        {
            g.setFont (theme::font (12.0f, true));
            auto w = GlyphArrangement::getStringWidthInt (g.getCurrentFont(), autoTip) + 12;
            auto r = Rectangle<int> (autoTipPos.x + 10, autoTipPos.y - 26, w, 20);
            g.setColour (theme::tooltipBg);
            g.fillRoundedRectangle (r.toFloat(), 4.0f);
            g.setColour (theme::onAccent);
            g.drawText (autoTip, r, Justification::centred);
        }

        // the playhead x the timer invalidated (playXForPaint): never the live position, or a
        // late paint would draw it outside the repainted column (flicker / ghost trails)
        const int px = (int) (owner.playXForPaint >= 0
                                  ? owner.playXForPaint
                                  : (juce::int64) std::llround (app.engine.getPositionBeats() * ppb));
        g.setColour (theme::accent);
        g.fillRect (px, clip.getY(), 2, clip.getHeight());
    }

    void paintGrid (Graphics& g, Rectangle<int> clip)
    {
        grid::paint (g, owner.app.project, owner.app.pxPerBeat, clip, 0, owner.app.arrangeGrid);
    }

    Rectangle<float> regionRect (int rowIdx, double start, double lenBeats) const
    {
        const double ppb = owner.app.pxPerBeat;
        const int trackHeight = owner.trackH (*owner.app.project.tracks[(size_t) jlimit (0, (int) owner.app.project.tracks.size() - 1, rowIdx)]);
        return { (float) (start * ppb), (float) owner.rowTop (rowIdx) + 3.0f, (float) jmax (3.0, lenBeats * ppb),
                 (float) trackHeight - 7.0f };
    }

    void paintRegionFrame (Graphics& g, Rectangle<float> r, Colour col, const String& name, bool selected, bool muted)
    {
        const float hdr = 15.0f;
        auto body = r.withTrimmedTop (hdr);
        const Colour c = muted ? theme::textFaint : col;
        g.setColour (tint (c, selected ? 0.33f : 0.18f));
        g.fillRoundedRectangle (r, 3.5f);
        g.setColour (selected ? c.darker (0.2f) : c);
        g.fillRoundedRectangle (r.withHeight (hdr), 3.5f);
        g.fillRect (r.getX(), r.getY() + hdr - 4.0f, r.getWidth(), 4.0f);
        g.setColour (theme::onAccent);
        g.setFont (theme::font (11.5f, true));
        if (r.getWidth() > 24.0f)
            g.drawText (name, r.withHeight (hdr).reduced (5.0f, 0.0f), Justification::centredLeft, true);
        g.setColour (selected ? theme::accentDark : c.darker (0.15f));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.5f, selected ? 2.0f : 1.0f);
        juce::ignoreUnused (body);
    }

    /** "Take 2 / 3" in the title bar of a clip that is part of a stack. */
    void drawTakeBadges (Graphics& g, Track& t, int rowIdx, Rectangle<int> clip)
    {
        auto& proj = owner.app.project;
        auto draw = [&] (double startBeat, double lenBeats, juce::int64 group, int index, bool active, int count)
        {
            if (group == 0 || count < 2) return;
            auto r = regionRect (rowIdx, startBeat, lenBeats);
            if (! r.toNearestInt().intersects (clip)) return;
            if (r.getWidth() < 56.0f) return;
            const String txt = TRANS ("Take") + " " + String (index + 1) + "/" + String (count);
            const auto font = theme::font (10.0f, true);
            const int tw = (int) std::ceil (GlyphArrangement::getStringWidth (font, txt)) + 8;
            auto badge = Rectangle<int> (r.toNearestInt().getRight() - tw - 3, (int) r.getY() + 3, tw, 13);
            g.setFont (font);
            g.setColour ((active ? theme::accent : theme::panelDeep).withAlpha (0.9f));
            g.fillRoundedRectangle (badge.toFloat(), 3.0f);
            g.setColour (theme::onAccent);
            g.drawFittedText (txt, badge.reduced (3, 0), Justification::centred, 1);
            juce::ignoreUnused (proj);
        };

        std::map<juce::int64, int> counts;
        for (auto& c : t.audioClips) if (c.takeGroup != 0) ++counts[c.takeGroup];
        for (auto& c : t.midiClips) if (c.takeGroup != 0) ++counts[c.takeGroup];
        for (auto& c : t.audioClips)
            if (c.takeGroup != 0 && c.takeActive)
                draw (c.start, proj.clipBeats (c.start, c.lengthSec), c.takeGroup, c.takeIndex, true, counts[c.takeGroup]);
        for (auto& c : t.midiClips)
            if (c.takeGroup != 0 && c.takeActive)
                draw (c.start, c.length, c.takeGroup, c.takeIndex, true, counts[c.takeGroup]);
    }

    void paintMidi (Graphics& g, Track& t, MidiClip& c, int rowIdx, Rectangle<int> clip)
    {
        auto r = regionRect (rowIdx, c.start, c.length);
        if (! r.toNearestInt().intersects (clip)) return;
        const bool sel = owner.app.selectedClips.count (c.id) > 0;
        paintRegionFrame (g, r, t.colour, c.name, sel, t.mute || c.muted);

        auto body = r.withTrimmedTop (17.0f).reduced (1.0f, 2.0f);
        if (body.getHeight() < 6.0f || c.notes.empty()) { paintCCHint (g, c, body); return; }
        int lo = 127, hi = 0;
        for (auto& n : c.notes) { lo = jmin (lo, n.pitch); hi = jmax (hi, n.pitch); }
        if (hi - lo < 11) { const int mid = (hi + lo) / 2; lo = mid - 6; hi = mid + 6; }
        const float nh = jlimit (1.5f, 6.0f, body.getHeight() / (float) (hi - lo + 1));
        const double ppb = owner.app.pxPerBeat;

        g.saveState();
        g.reduceClipRegion (body.toNearestInt());
        g.setColour (sel ? t.colour.darker (0.35f) : t.colour.darker (0.2f));
        for (auto& n : c.notes)
        {
            if (n.start >= c.length) continue;
            const float x = r.getX() + (float) (n.start * ppb);
            if (x > (float) clip.getRight() || x + (float) (n.length * ppb) < (float) clip.getX()) continue;
            const float y = body.getBottom() - ((float) (n.pitch - lo) + 1.0f) * (body.getHeight() / (float) (hi - lo + 1));
            g.fillRect (x, y, jmax (1.5f, (float) (jmin (n.length, c.length - n.start) * ppb) - 0.5f), nh);
        }
        g.restoreState();
        if (t.frozen)   // frozen clips are view-only
        {
            g.setColour (theme::panel.withAlpha (0.45f));
            g.fillRect (r.toNearestInt());
        }
    }

    void paintCCHint (Graphics&, MidiClip&, Rectangle<float>) {}

    void paintAudio (Graphics& g, Track& t, AudioClip& c, int rowIdx, Rectangle<int> clip)
    {
        auto& proj = owner.app.project;
        auto r = regionRect (rowIdx, c.start, proj.clipBeats (c.start, c.lengthSec));
        if (! r.toNearestInt().intersects (clip)) return;
        const bool sel = owner.app.selectedClips.count (c.id) > 0;
        paintRegionFrame (g, r, t.colour, c.name, sel, t.mute || c.muted);

        auto body = r.withTrimmedTop (16.0f).reduced (1.0f, 1.0f);
        if (c.data == nullptr)
        {
            g.setFont (theme::font (11.0f));
            if (c.loading)
            {
                // the clip is already on the timeline; its samples are still being decoded
                g.setColour (theme::textDim);
                g.drawText (TRANS ("Loading audio..."), body, Justification::centred);
            }
            else
            {
                g.setColour (theme::record);
                g.drawText (TRANS ("Missing file"), body, Justification::centred);
            }
            return;
        }

        // the waveform follows the clip's playback buffer: stretched / transposed clips show the
        // rendered audio, reversed clips are drawn mirrored (see clipBufferSeconds)
        // fall back to the source file when the speed / pitch render is missing or empty, so a
        // stale render can never blank the waveform
        auto& d = (c.rendered != nullptr && c.rendered->buffer.getNumSamples() > 0) ? *c.rendered
                                                                                  : *c.data;
        const double ppb = owner.app.pxPerBeat;
        const double startSec = proj.beatsToSeconds (c.start);
        // buffer time at a lane pixel - exact through tempo ramps, stretch and reverse
        const auto secAtX = [&] (double x) { return clipBufferSeconds (c, proj.beatsToSeconds (x / ppb) - startSec); };
        const int xs = jmax ((int) body.getX(), clip.getX());
        const int xe = jmin ((int) body.getRight(), clip.getRight());

        // the drawn waveform follows the clip gain envelope + fade ramps and never leaves the block
        const auto fadeEnvAt = [&] (double regionSec)
        {
            double e = 1.0;
            if (c.fadeIn > 1.0e-6 && regionSec < c.fadeIn)
            {
                const double x = jmax (0.0, regionSec) / c.fadeIn;
                e *= x * x * (3.0 - 2.0 * x);
            }
            if (c.fadeOut > 1.0e-6 && regionSec > c.lengthSec - c.fadeOut)
            {
                const double x = jlimit (0.0, 1.0, (c.lengthSec - regionSec) / c.fadeOut);
                e *= x * x * (3.0 - 2.0 * x);
            }
            return e;
        };

        g.setColour (sel ? t.colour.darker (0.35f) : t.colour.darker (0.1f));

        // the envelope follows the clip gain line and the fade ramps; Waveform.h joins the
        // per-column min/max into one anti-aliased outline (and reads the samples exactly when
        // zoomed in, instead of flattening everything into 128-sample blocks)
        const auto gainAt = [&] (double x)
        {
            const double beatRel = x / ppb - c.start;
            const float gainDb = evalClipGainDb (c.gainEnv, beatRel, c.gainDb);
            return (float) (fadeEnvAt (secAtX (x)) * Decibels::decibelsToGain (gainDb));
        };
        // a stereo clip shows its left and right channel as two stacked lanes; a mono clip (or a
        // body too short to split into two readable lanes) keeps the single mixed waveform
        if (d.buffer.getNumChannels() >= 2 && body.getHeight() >= 10.0f)
            waveform::drawStereo (g, body, d, xs, xe, secAtX, gainAt);
        else
            waveform::draw (g, body, d, xs, xe, secAtX, gainAt);

        // fade in / out shown as a tinted wedge with a bright ramp and a grab handle
        const double endSec = startSec + c.lengthSec;
        const float fInW = (float) jlimit (0.0, (double) r.getWidth(), (proj.secondsToBeats (startSec + c.fadeIn) - c.start) * ppb);
        const float fOutW = (float) jlimit (0.0, (double) r.getWidth(), (proj.secondsToBeats (endSec) - proj.secondsToBeats (endSec - c.fadeOut)) * ppb);
        const auto drawFade = [&] (bool fadeIn, float w)
        {
            if (w < 0.75f) return;
            Path p;
            if (fadeIn)
            {
                p.startNewSubPath (r.getX(), body.getBottom());
                p.lineTo (r.getX() + w, body.getY());
                p.lineTo (r.getX() + w, body.getBottom());
            }
            else
            {
                p.startNewSubPath (r.getRight(), body.getBottom());
                p.lineTo (r.getRight() - w, body.getY());
                p.lineTo (r.getRight() - w, body.getBottom());
            }
            p.closeSubPath();
            g.setColour (theme::accent.withAlpha (sel ? 0.26f : 0.18f));
            g.fillPath (p);
            g.setColour (theme::accent.withAlpha (0.9f));
            if (fadeIn) g.drawLine (r.getX(), body.getBottom(), r.getX() + w, body.getY(), 1.7f);
            else        g.drawLine (r.getRight(), body.getBottom(), r.getRight() - w, body.getY(), 1.7f);

            const float hx = fadeIn ? r.getX() + w : r.getRight() - w;
            g.setColour (theme::accent);
            g.fillEllipse (hx - 4.0f, body.getY() - 3.0f, 8.0f, 8.0f);
            g.setColour (theme::onAccent);
            g.fillEllipse (hx - 1.6f, body.getY() - 0.6f, 3.2f, 3.2f);
        };
        drawFade (true, fInW);
        drawFade (false, fOutW);

        // where the clip gain sits: this line is also a handle (drag up / down to change
        // the whole clip), like the gain line Studio One draws across an event
        if (! c.gainLine && body.getHeight() > 16.0f)
        {
            const float gy = gainDbToY (r, c.gainDb);
            const float x0 = r.getX() + 1.0f, x1 = r.getRight() - 1.0f;
            g.setColour (theme::panelDeep.withAlpha (0.45f));
            g.fillRect (x0, gy - 1.4f, x1 - x0, 3.2f);
            g.setColour (theme::onAccent.withAlpha (sel ? 0.85f : 0.55f));
            g.fillRect (x0, gy - 0.6f, x1 - x0, 1.4f);
        }

        // clip gain envelope drawn directly on the block (white line, per-clip, off by default)
        if (c.gainLine)
        {
            const float x0 = r.getX(), x1 = r.getRight();
            Path env;
            auto dbY = [&] (float db) { return gainDbToY (r, db); };
            if (c.gainEnv.empty())
            {
                env.startNewSubPath (x0, dbY (c.gainDb));
                env.lineTo (x1, dbY (c.gainDb));
            }
            else
            {
                env.startNewSubPath (x0, dbY (c.gainEnv.front().db));
                for (auto& pt : c.gainEnv)
                    env.lineTo (x0 + (float) (pt.beat * ppb), dbY (pt.db));
                env.lineTo (x1, dbY (c.gainEnv.back().db));
            }
            // dark underlay keeps the white line readable on light and dark themes
            g.setColour (theme::panelDeep.withAlpha (0.55f));
            g.strokePath (env, PathStrokeType (sel ? 3.4f : 2.8f));
            g.setColour (theme::onAccent);
            g.strokePath (env, PathStrokeType (sel ? 2.0f : 1.5f));

            for (auto& pt : c.gainEnv)
            {
                const float px = x0 + (float) (pt.beat * ppb), pyy = dbY (pt.db);
                if (px < clip.getX() - 6 || px > clip.getRight() + 6) continue;
                g.setColour (theme::panelDeep.withAlpha (0.6f));
                g.fillEllipse (px - 4.0f, pyy - 4.0f, 8.0f, 8.0f);
                g.setColour (theme::onAccent);
                g.fillEllipse (px - 2.4f, pyy - 2.4f, 4.8f, 4.8f);
            }

            if (sel)
            {
                const float shownDb = c.gainEnv.empty() ? c.gainDb : c.gainEnv.front().db;
                const String txt = (shownDb >= 0.0f ? "+" : "") + String (shownDb, 1) + " dB";
                const auto font = theme::font (10.0f, true);
                const int tw = (int) std::ceil (GlyphArrangement::getStringWidth (font, txt)) + 10;
                auto badge = Rectangle<int> ((int) r.getX() + 4, (int) body.getY() + 1, tw, 13);
                g.setFont (font);
                g.setColour (theme::panelDeep.withAlpha (0.8f));
                g.fillRoundedRectangle (badge.toFloat(), 3.0f);
                g.setColour (theme::onAccent);
                g.drawFittedText (txt, badge.reduced (4, 0), Justification::centred, 1);
            }
        }

        if (t.frozen)   // frozen clips are view-only
        {
            g.setColour (theme::panel.withAlpha (0.45f));
            g.fillRect (r.toNearestInt());
        }
    }

    // Crossfade between two overlapping audio clips: the two curves the engine really
    // applies, drawn above both clips, with a handle in the middle of the overlap that
    // drags the crossfade length (and a right-click menu for length / curve).
    void paintCrossfades (Graphics& g, Track& t, int rowIdx, Rectangle<int> clip)
    {
        auto& proj = owner.app.project;
        const double ppb = owner.app.pxPerBeat;
        for (auto& c : t.audioClips)
        {
            if (c.muted) continue;
            const auto r = regionRect (rowIdx, c.start, proj.clipBeats (c.start, c.lengthSec));
            if (! r.toNearestInt().intersects (clip)) continue;
            const auto body = r.withTrimmedTop (16.0f).reduced (1.0f, 1.0f);
            if (body.getHeight() < 8.0f) continue;
            const double thisEnd = c.start + proj.clipBeats (c.start, c.lengthSec);
            for (auto& o : t.audioClips)
            {
                if (o.id == c.id || o.start <= c.start || o.muted) continue;
                const double oEnd = o.start + proj.clipBeats (o.start, o.lengthSec);
                const double ovA = jmax (c.start, o.start), ovB = jmin (thisEnd, oEnd);
                if (ovB - ovA < 0.001) continue;
                const float x0 = (float) (ovA * ppb), x1 = (float) (ovB * ppb);
                if (x1 < (float) clip.getX() - 4.0f || x0 > (float) clip.getRight() + 4.0f) continue;
                const float h = body.getHeight();
                const bool sel = owner.app.selectedClips.count (c.id) > 0
                              || owner.app.selectedClips.count (o.id) > 0;
                g.setColour (theme::accent.withAlpha (sel ? 0.16f : 0.10f));
                g.fillRect (x0, body.getY(), x1 - x0, h);
                const int steps = jmax (2, (int) ((x1 - x0) / 6.0f));
                Path out, in;
                for (int i = 0; i <= steps; ++i)
                {
                    const double tt = (double) i / (double) steps;
                    const float x = x0 + (x1 - x0) * (float) tt;
                    const float yo = body.getBottom() - (float) xfadeGain (c.xfadeCurve, tt, true) * h;
                    const float yi = body.getBottom() - (float) xfadeGain (c.xfadeCurve, tt, false) * h;
                    if (i == 0) { out.startNewSubPath (x, yo); in.startNewSubPath (x, yi); }
                    else { out.lineTo (x, yo); in.lineTo (x, yi); }
                }
                g.setColour (t.colour.withAlpha (0.9f));
                g.strokePath (out, PathStrokeType (1.6f));
                g.setColour (theme::accent.withAlpha (0.9f));
                g.strokePath (in, PathStrokeType (1.6f));

                // grab handle in the middle of the overlap
                const float cx = (x0 + x1) * 0.5f, cy = body.getCentreY();
                if (x1 - x0 > 14.0f)
                {
                    g.setColour (theme::panelDeep.withAlpha (0.85f));
                    g.fillEllipse (cx - 6.5f, cy - 6.5f, 13.0f, 13.0f);
                    g.setColour (theme::accent);
                    g.drawEllipse (cx - 6.5f, cy - 6.5f, 13.0f, 13.0f, 1.6f);
                    g.drawLine (cx - 3.0f, cy - 3.0f, cx + 3.0f, cy + 3.0f, 1.4f);
                    g.drawLine (cx - 3.0f, cy + 3.0f, cx + 3.0f, cy - 3.0f, 1.4f);
                }
            }
        }
    }

    //-- automation ------------------------------------------------------------
    // the lane a click in row / lane column refers to: automation tracks own exactly one lane
    AutoLane* laneAt (int rowIdx, int laneIdx)
    {
        auto& tracks = owner.app.project.tracks;
        if (rowIdx < 0 || rowIdx >= (int) tracks.size()) return nullptr;
        auto& t = *tracks[(size_t) rowIdx];
        if (t.isAutomation()) return t.automationLane();
        if (laneIdx < 0) return t.inlineAutomationLane();   // Studio One style in-track curve
        return t.visibleLaneAt (laneIdx);
    }

    Rectangle<int> autoRect (int rowIdx, int laneIdx) const
    {
        auto& tracks = owner.app.project.tracks;
        if (rowIdx < 0 || rowIdx >= (int) tracks.size()) return {};
        auto& t = *tracks[(size_t) rowIdx];
        // the row height follows the track height (a fixed 70 px row used to overlap the next
        // track as soon as the track was taller or shorter than the default)
        if (t.isAutomation()) return rowBodyRect (rowIdx);
        if (laneIdx < 0) return rowBodyRect (rowIdx);        // automation drawn in the track row
        return { 0, owner.laneRowTop (rowIdx, laneIdx), getWidth(), jmax (1, owner.autoH (t) - 1) };
    }

    // the whole track body, in lane content coordinates (used by in-track automation)
    Rectangle<int> rowBodyRect (int rowIdx) const
    {
        if (rowIdx < 0 || rowIdx >= (int) owner.app.project.tracks.size()) return {};
        auto& t = *owner.app.project.tracks[(size_t) rowIdx];
        return { 0, owner.rowTop (rowIdx) + 1, getWidth(), jmax (1, owner.trackH (t) - 2) };
    }

    static float yToValue (int y, Rectangle<int> r) { return autolane::valueAt (y, r); }
    static float valueToY (float v, Rectangle<int> r) { return autolane::yOf (v, r); }

    // the node being dragged on this lane, so the shared painter can highlight it
    int activeAutoPoint (const Track& t, const AutoLane& lane) const
    {
        return mode == Mode::AutoPoint && autoTrackId == t.id && autoTarget == lane.target ? autoIdx : -1;
    }

    void paintAutomation (Graphics& g, Track& t, int rowIdx, int laneIdx, Rectangle<int> clip)
    {
        auto* lane = t.isAutomation() ? t.automationLane() : t.visibleLaneAt (laneIdx);
        if (lane == nullptr) return;
        auto r = autoRect (rowIdx, laneIdx);
        g.saveState();
        g.reduceClipRegion (r);
        // in the scrolled lane content the beat -> pixel origin is the content origin
        autolane::paintRow (g, *lane, r, 0, owner.app.pxPerBeat, activeAutoPoint (t, *lane));
        g.restoreState();
    }

    //-- hit testing -----------------------------------------------------------
    ClipHit hitClip (Point<int> p)
    {
        auto& proj = owner.app.project;
        const int idx = owner.trackIndexAtY (p.y);
        ClipHit h;
        if (idx < 0) return h;
        auto& t = *proj.tracks[(size_t) idx];
        if (p.y >= owner.rowTop (idx) + owner.trackH (t)) return h;

        for (size_t i = t.midiClips.size(); i-- > 0;)
        {
            auto r = regionRect (idx, t.midiClips[i].start, t.midiClips[i].length);
            if (r.contains ((float) p.x, (float) p.y)) { h.trackIdx = idx; h.track = &t; h.clipId = t.midiClips[i].id; h.audio = false; h.rect = r; return h; }
        }
        for (size_t i = t.audioClips.size(); i-- > 0;)
        {
            auto r = regionRect (idx, t.audioClips[i].start, proj.clipBeats (t.audioClips[i].start, t.audioClips[i].lengthSec));
            if (r.contains ((float) p.x, (float) p.y)) { h.trackIdx = idx; h.track = &t; h.clipId = t.audioClips[i].id; h.audio = true; h.rect = r; return h; }
        }
        return h;
    }

    void deleteClip (const ClipHit& h)
    {
        auto* t = h.track;
        if (t == nullptr) return;
        if (h.audio)
            t->audioClips.erase (std::remove_if (t->audioClips.begin(), t->audioClips.end(),
                                                 [&] (const AudioClip& c) { return c.id == h.clipId; }), t->audioClips.end());
        else
            t->midiClips.erase (std::remove_if (t->midiClips.begin(), t->midiClips.end(),
                                                [&] (const MidiClip& c) { return c.id == h.clipId; }), t->midiClips.end());
        owner.app.selectedClips.erase (h.clipId);
        if (owner.app.editClip == h.clipId) { owner.app.editClip = -1; owner.app.editTrack = -1; }
    }

    void toggleClipMute (const ClipHit& h)
    {
        if (h.audio) { if (auto* c = h.track->findAudio (h.clipId)) c->muted = ! c->muted; }
        else if (auto* c = h.track->findMidi (h.clipId)) c->muted = ! c->muted;
    }

    // padded fade / gain grip on an audio clip. grip: 1 = fade in, 2 = fade out, 3 = gain
    struct AudioGrip { int type = 0; AudioClip* clip = nullptr; Track* track = nullptr; Rectangle<float> rect; double secPerPx = 0.0; };

    AudioGrip hitAudioGrip (Point<int> p)
    {
        auto h = hitClip (p);
        AudioGrip f;
        if (! h || ! h.audio) return f;
        auto* c = h.track->findAudio (h.clipId);
        if (c == nullptr) return f;
        auto& proj = owner.app.project;
        const double ppb = owner.app.pxPerBeat;
        const double startSec = proj.beatsToSeconds (c->start), endSec = startSec + c->lengthSec;
        f.secPerPx = 60.0 / proj.bpmAt (c->start) / ppb;
        const float fx = h.rect.getX() + (float) ((proj.secondsToBeats (startSec + c->fadeIn) - c->start) * ppb);
        const float foX = h.rect.getRight() - (float) ((proj.secondsToBeats (endSec) - proj.secondsToBeats (endSec - c->fadeOut)) * ppb);
        const float fy = h.rect.getY() + 13.0f;   // the fade handles sit on top of the clip body
        const bool fin = std::abs ((float) p.x - fx) < 8.0f && std::abs ((float) p.y - fy) < 11.0f;
        const bool foutStart = std::abs ((float) p.x - foX) < 8.0f && std::abs ((float) p.y - fy) < 11.0f;
        const bool inCorner = c->fadeIn <= 0.0 && p.x - h.rect.getX() < 7 && p.y - h.rect.getY() < 13;
        const bool outCorner = c->fadeOut <= 0.0 && h.rect.getRight() - p.x < 7 && p.y - h.rect.getY() < 13;
        if ((fin || inCorner) && p.x >= h.rect.getX())                                        { f.type = 1; }
        else if ((foutStart || outCorner) && p.x < h.rect.getRight())                          { f.type = 2; }
        // overall clip gain: the strip along the top edge of the body (between the fade grips);
        // drag up / down to change the whole clip, like Studio One's clip gain handle
        if (f.type == 0
            && (float) p.x > h.rect.getX() + 16.0f && (float) p.x < h.rect.getRight() - 16.0f
            && (float) p.y >= h.rect.getY() + 9.0f && (float) p.y <= h.rect.getY() + 23.0f)     { f.type = 3; }
        // the gain line across the middle of the clip is a handle too (only while the
        // envelope is hidden: with the envelope on, the pencil edits the curve)
        if (f.type == 0 && ! c->gainLine
            && (float) p.x > h.rect.getX() + 2.0f && (float) p.x < h.rect.getRight() - 2.0f)
        {
            const float gy = gainDbToY (h.rect, c->gainDb);
            if (std::abs ((float) p.y - gy) <= 5.0f) f.type = 3;
        }
        if (f.type == 0) return f;
        f.clip = c;
        f.track = h.track;
        f.rect = h.rect;
        return f;
    }

    // the round handle in the middle of a crossfade (drag left / right to size it)
    struct XfadeHit
    {
        Track* track = nullptr;
        AudioClip* a = nullptr;   // earlier clip
        AudioClip* b = nullptr;   // later clip
        double overlapSec = 0.0;
        explicit operator bool() const { return track != nullptr && a != nullptr && b != nullptr; }
    };

    XfadeHit hitCrossfade (Point<int> p)
    {
        XfadeHit x;
        const int rowIdx = owner.trackIndexAtY (p.y);
        if (rowIdx < 0) return x;
        auto& proj = owner.app.project;
        auto& t = *proj.tracks[(size_t) rowIdx];
        const double ppb = owner.app.pxPerBeat;
        for (auto& c : t.audioClips)
        {
            if (c.muted) continue;
            const auto r = regionRect (rowIdx, c.start, proj.clipBeats (c.start, c.lengthSec));
            const auto body = r.withTrimmedTop (16.0f).reduced (1.0f, 1.0f);
            if (body.getHeight() < 8.0f) continue;
            const double thisEnd = c.start + proj.clipBeats (c.start, c.lengthSec);
            for (auto& o : t.audioClips)
            {
                if (o.id == c.id || o.start <= c.start || o.muted) continue;
                const double oEnd = o.start + proj.clipBeats (o.start, o.lengthSec);
                const double ovA = jmax (c.start, o.start), ovB = jmin (thisEnd, oEnd);
                if ((ovB - ovA) * ppb < 14.0) continue;
                const float cx = (float) ((ovA + ovB) * 0.5 * ppb), cy = body.getCentreY();
                if (std::abs ((float) p.x - cx) <= 8.0f && std::abs ((float) p.y - cy) <= 8.0f)
                {
                    x.track = &t;
                    x.a = &c;
                    x.b = &o;
                    x.overlapSec = proj.beatsToSeconds (ovB) - proj.beatsToSeconds (ovA);
                    return x;
                }
            }
        }
        return x;
    }

    // returns index of automation point near p (and sets lane), or -1
    int hitAutoPoint (Point<int> p, int rowIdx, int laneIdx, AutoLane*& laneOut)
    {
        laneOut = laneAt (rowIdx, laneIdx);
        if (laneOut == nullptr) return -1;
        auto r = autoRect (rowIdx, laneIdx);
        return autolane::hitPoint (*laneOut, p, r, 0, owner.app.pxPerBeat);
    }

    bool isAutoArea (Point<int> p, int& rowIdx, int& laneIdx)
    {
        rowIdx = owner.trackIndexAtY (p.y);
        if (rowIdx < 0) return false;
        auto& t = *owner.app.project.tracks[(size_t) rowIdx];
        if (t.isAutomation())
        {
            if (! t.shownInArranger()) return false;
            laneIdx = 0;
            return p.y >= owner.rowTop (rowIdx) && p.y < owner.rowTop (rowIdx) + owner.rowHeight (t);
        }
        // automation drawn in the track row: in edit mode the whole body belongs to the lane,
        // otherwise only a node under the cursor is grabbed (clips stay editable everywhere else)
        if (auto* overlay = t.inlineAutomationLane())
        {
            laneIdx = -1;
            if (t.automationEdit) return true;
            const auto body = rowBodyRect (rowIdx);
            return autolane::hitPoint (*overlay, p, body, 0, owner.app.pxPerBeat) >= 0;
        }
        const int lanes = t.laneRowCount();
        if (lanes == 0) return false;
        const int laneH = owner.autoH (t);
        const int off = p.y - owner.rowTop (rowIdx) - owner.trackH (t);
        if (off < 0 || off >= lanes * laneH) return false;
        laneIdx = jlimit (0, lanes - 1, off / laneH);
        return true;
    }

    //-- mouse -------------------------------------------------------------------
    void mouseMove (const MouseEvent& e) override
    {
        owner.cursorBeat = owner.xToBeat ((double) e.x);
        if (owner.app.arrangeTool == ArrangeTool::Pencil) { setMouseCursor (drawCrosshairCursor()); return; }
        if (owner.app.arrangeTool == ArrangeTool::Scissors) { setMouseCursor (drawCrosshairCursor()); return; }
        int row, lane;
        if (isAutoArea (e.getPosition(), row, lane)) { setMouseCursor (drawCrosshairCursor()); return; }
        if (hitCrossfade (e.getPosition())) { setMouseCursor (MouseCursor::LeftRightResizeCursor); return; }
        auto grip = hitAudioGrip (e.getPosition());
        if (grip.type != 0)
        {
            // the gain strip drags vertically, the fade grips horizontally
            setMouseCursor (grip.type == 3 ? MouseCursor::UpDownResizeCursor : MouseCursor::LeftRightResizeCursor);
            return;
        }
        auto h = hitClip (e.getPosition());
        if (h && (e.x - h.rect.getX() < 6.0f || h.rect.getRight() - e.x < 6.0f)) setMouseCursor (MouseCursor::LeftRightResizeCursor);
        else setMouseCursor (MouseCursor::NormalCursor);
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto& app = owner.app;
        auto& proj = app.project;
        mode = Mode::None;
        moved = false;
        checkpointed = false;
        downPos = e.getPosition();
        downBeat = owner.xToBeat (e.x);
        owner.cursorBeat = downBeat;   // 'S' (split at cursor) uses the last mouse position

        // middle button pans the arrangement in both directions (Studio One style)
        if (e.mods.isMiddleButtonDown())
        {
            mode = Mode::Pan;
            panStartScrollX = owner.laneViewport.getViewPositionX();
            panStartScrollY = owner.laneViewport.getViewPositionY();
            setMouseCursor (MouseCursor::DraggingHandCursor);
            return;
        }

        int row, laneIdx;
        if (isAutoArea (e.getPosition(), row, laneIdx))
        {
            auto& t = *proj.tracks[(size_t) row];
            app.selectTrack (t.id);
            AutoLane* lane = nullptr;
            const int hit = hitAutoPoint (e.getPosition(), row, laneIdx, lane);
            if (lane == nullptr) return;
            if (e.mods.isPopupMenu())
            {
                if (hit >= 0)
                {
                    proj.checkpoint();
                    lane->points.erase (lane->points.begin() + hit);
                    proj.sendContentChanged();
                }
                return;
            }
            autoTrackId = t.id;
            autoTarget = lane->target;
            t.activeLane = lane->target;
            if (hit >= 0)
            {
                autoIdx = hit;
            }
            else
            {
                proj.checkpoint();
                checkpointed = true;
                // a node lands on the grid and the curve stays monotone: two nodes never share a
                // beat, so nothing can double back between them
                const double nb = jmax (0.0, snapTo (owner.xToBeat (e.x), jmax (0.0625, app.arrangeGrid)));
                autoIdx = insertAutoPoint (lane->points, nb, yToValue (e.y, autoRect (row, laneIdx)));
                proj.sendContentChanged();
            }
            mode = Mode::AutoPoint;
            autoRow = row;
            autoLaneIdx = laneIdx;
            updateTip (e.getPosition());
            return;
        }

        auto h = hitClip (e.getPosition());
        // with the pencil active a right click deletes the clip under the cursor
        if (e.mods.isPopupMenu() && app.arrangeTool == ArrangeTool::Pencil && h)
        {
            proj.checkpoint();
            deleteClip (h);
            proj.sendContentChanged();
            owner.updateContentSize();
            repaint();
            return;
        }
        if (e.mods.isPopupMenu())
        {
            // right-clicking a gain envelope anchor removes just that anchor (not the clip)
            if (h && h.audio)
                if (auto* ac2 = h.track->findAudio (h.clipId))
                    if (ac2->gainLine)
                    {
                        const int gidx = hitGainPoint (*ac2, h.rect, e.getPosition(), app.pxPerBeat);
                        if (gidx >= 0)
                        {
                            proj.checkpoint();
                            ac2->gainEnv.erase (ac2->gainEnv.begin() + gidx);
                            proj.sendContentChanged();
                            repaint();
                            return;
                        }
                    }
            if (h)
            {
                if (app.selectedClips.count (h.clipId) == 0) app.selectClip (h.track->id, h.clipId, false);
                showClipMenu (h, e.getPosition());
            }
            else
            {
                const int idx = owner.trackIndexAtY (e.y);
                if (idx >= 0) app.selectTrack (proj.tracks[(size_t) idx]->id);
            }
            return;
        }

        // frozen tracks play a pre-render: the source clips stay visible but cannot be edited
        if (h && h.track->frozen)
        {
            app.selectClip (h.track->id, h.clipId, false);
            return;
        }

        //-- the editing tools (pencil / eraser / mute region) ---------------------
        if (app.arrangeTool == ArrangeTool::Pencil)
        {
            // dragging the tail of an existing clip sizes it, just like with the select tool
            if (h && h.rect.getRight() - e.x < 6.0f)
            {
                app.selectClip (h.track->id, h.clipId, false);
                if (app.selectedClips.count (h.clipId) == 0) return;
                primaryClip = h.clipId;
                mode = Mode::ResizeR;
                primaryTrackIdx = h.trackIdx;
                captureItems();
                return;
            }

            // a clip that shows its gain envelope hands the pencil to the envelope editor
            if (h && h.audio)
            {
                auto* ac = h.track->findAudio (h.clipId);
                if (ac != nullptr && ac->gainLine)
                {
                    const double ppb = app.pxPerBeat;
                    const double clipBeats = proj.clipBeats (ac->start, ac->lengthSec);
                    const int gi = hitGainPoint (*ac, h.rect, e.getPosition(), ppb);
                    app.selectClip (h.track->id, h.clipId, false);
                    proj.checkpoint();
                    checkpointed = true;
                    gainTrackId = h.track->id;
                    gainClipId = h.clipId;
                    gainRect = h.rect;
                    gainDragStartY = e.y;
                    gainBackup = ac->gainEnv;

                    if (e.mods.isAltDown())
                    {
                        // Alt-drag anywhere on the clip shifts the whole curve
                        mode = Mode::GainAll;
                    }
                    else if (gi >= 0)
                    {
                        gainPointIdx = gi;
                        mode = Mode::GainPoint;
                    }
                    else
                    {
                        ClipGainPoint np { gainSnapBeat (*ac, clipBeats, ppb, e.x, app.arrangeGrid, e.mods.isCtrlDown() || e.mods.isCommandDown()),
                                           yToGainDb (h.rect, (float) e.y) };
                        auto& v = ac->gainEnv;
                        v.push_back (np);
                        std::sort (v.begin(), v.end(), [] (const ClipGainPoint& a, const ClipGainPoint& b) { return a.beat < b.beat; });
                        gainPointIdx = 0;
                        for (size_t i = 0; i < v.size(); ++i)
                            if (std::abs (v[i].beat - np.beat) < 1.0e-9 && std::abs (v[i].db - np.db) < 1.0e-4)
                                gainPointIdx = (int) i;
                        mode = app.clipGainFreehand ? Mode::GainFree : Mode::GainPoint;
                    }
                    autoTip = String (yToGainDb (h.rect, (float) e.y), 1) + " dB";
                    autoTipPos = e.getPosition();
                    proj.sendContentChanged();
                    repaint();
                    return;
                }
            }
            const int idx = owner.trackIndexAtY (e.y);
            if (idx >= 0)
            {
                auto& t = *proj.tracks[(size_t) idx];
                if (t.type == TrackType::Instrument)
                {
                    mode = Mode::DrawClip;
                    drawTrackId = t.id;
                    moved = false;
                    const double g = app.arrangeGrid > 0 ? app.arrangeGrid : 0.25;
                    drawStart = snapTo (jmax (0.0, downBeat), g);
                    drawEnd = drawStart + g;
                    app.selectTrack (t.id);
                    repaint();
                    return;
                }
            }
            return;
        }
        if (app.arrangeTool == ArrangeTool::Eraser)
        {
            mode = Mode::Erase;
            // erasing on a clip that shows its gain envelope first removes an envelope point
            if (h && h.audio)
                if (auto* ac = h.track->findAudio (h.clipId))
                    if (ac->gainLine)
                    {
                        const int gi = hitGainPoint (*ac, h.rect, e.getPosition(), app.pxPerBeat);
                        if (gi >= 0)
                        {
                            proj.checkpoint();
                            checkpointed = true;
                            ac->gainEnv.erase (ac->gainEnv.begin() + gi);
                            proj.sendContentChanged();
                            return;
                        }
                    }
            if (h)
            {
                proj.checkpoint();
                checkpointed = true;
                deleteClip (h);
                proj.sendContentChanged();
            }
            return;
        }
        if (app.arrangeTool == ArrangeTool::Scissors)
        {
            // click a clip to cut it at that point: the cut snaps to the grid like every other edit
            mode = Mode::None;
            if (h)
            {
                double cut = owner.xToBeat (e.x);
                if (! e.mods.isAltDown()) cut = snapTo (cut, app.arrangeGrid);
                // split only the clip that was clicked, even when a whole group is selected
                app.selectClip (h.track->id, h.clipId, false);
                owner.splitAt (cut);
                owner.updateContentSize();
                repaint();
            }
            return;
        }

        if (app.arrangeTool == ArrangeTool::Mute)
        {
            mode = Mode::MuteDrag;
            muteTouched.clear();
            if (h)
            {
                proj.checkpoint();
                checkpointed = true;
                toggleClipMute (h);
                muteTouched.insert (h.clipId);
                proj.sendContentChanged();
            }
            return;
        }

        // the crossfade handle sizes the overlap between two clips. It is tested before
        // the gain line, because both sit in the middle of the clip body
        if (h && h.audio)
        {
            auto xh = hitCrossfade (e.getPosition());
            if (xh)
            {
                mode = Mode::Xfade;
                xfadeTrackId = xh.track->id;
                xfadeA = xh.a->id;
                xfadeB = xh.b->id;
                xfadeOrigBStart = xh.b->start;
                xfadeOrigOverlap = xh.overlapSec;
                downBeat = owner.xToBeat (e.x);
                app.selectTrack (xh.track->id);
                app.selectClip (xh.track->id, xh.a->id, false);
                app.selectClip (xh.track->id, xh.b->id, true);
                return;
            }
        }

        // fade / gain grips on audio clips (before selection / move logic)
        if (h && h.audio)
        {
            auto grip = hitAudioGrip (e.getPosition());
            if (grip.type != 0)
            {
                if (grip.type == 3 && grip.clip->gainLine && ! grip.clip->gainEnv.empty())
                {
                    // top-edge drag moves the whole envelope when the gain line is active
                    mode = Mode::GainAll;
                    gainTrackId = grip.track->id;
                    gainClipId = grip.clip->id;
                    gainRect = grip.rect;
                    gainDragStartY = e.y;
                    gainBackup = grip.clip->gainEnv;
                }
                else
                {
                    mode = grip.type == 3 ? Mode::ClipGain : Mode::Fade;
                    gripType = grip.type;
                    gripClipId = grip.clip->id;
                    gripTrackId = grip.track->id;
                    gripRect = grip.rect;
                    gripSecPerPx = grip.secPerPx;
                    origGainDb = grip.clip->gainDb;
                    gripStartY = e.y;
                    origFade = grip.type == 2 ? jlimit (0.0, grip.clip->lengthSec, grip.clip->fadeOut)
                                               : jlimit (0.0, grip.clip->lengthSec, grip.clip->fadeIn);
                    origFadeIn = grip.clip->fadeIn;
                    origFadeOut = grip.clip->fadeOut;
                }
                app.selectTrack (grip.track->id);
                if (app.selectedClips.count (grip.clip->id) == 0) app.selectClip (grip.track->id, grip.clip->id, false);
                return;
            }
        }

        if (h)
        {
            const bool alreadySel = app.selectedClips.count (h.clipId) > 0;
            if (e.mods.isShiftDown() || e.mods.isCtrlDown()) app.selectClip (h.track->id, h.clipId, true);
            else if (! alreadySel) app.selectClip (h.track->id, h.clipId, false);
            else app.selectTrack (h.track->id);
            if (app.selectedClips.count (h.clipId) == 0) return;

            primaryClip = h.clipId;
            if (e.x - h.rect.getX() < 6.0f) mode = Mode::ResizeL;
            else if (h.rect.getRight() - e.x < 6.0f) mode = Mode::ResizeR;
            else mode = Mode::Move;
            primaryTrackIdx = h.trackIdx;
            duplicateOnDrag = e.mods.isAltDown() && mode == Mode::Move;
            captureItems();
        }
        else
        {
            mode = Mode::Rubber;
            if (! e.mods.isShiftDown()) app.clearClipSelection();
            baseSelection = app.selectedClips;
            rubber = Rectangle<int> (e.getPosition(), e.getPosition());
            const int idx = owner.trackIndexAtY (e.y);
            if (idx >= 0) app.selectTrack (proj.tracks[(size_t) idx]->id);
        }
    }

    void captureItems()
    {
        items.clear();
        auto& proj = owner.app.project;
        for (auto& t : proj.tracks)
        {
            for (auto& c : t->midiClips)
                if (owner.app.selectedClips.count (c.id)) items.push_back ({ c.id, false, t->id, t->id, c.start, c.length, 0.0, 0.0 });
            for (auto& c : t->audioClips)
                if (owner.app.selectedClips.count (c.id))
                    items.push_back ({ c.id, true, t->id, t->id, c.start, proj.clipBeats (c.start, c.lengthSec), c.offsetSec, c.lengthSec });
        }
    }

    const Item* primaryItem() const
    {
        for (auto& i : items) if (i.clipId == primaryClip) return &i;
        return items.empty() ? nullptr : &items.front();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        auto& app = owner.app;
        auto& proj = app.project;

        if (mode == Mode::Pan)
        {
            const int dx = e.x - downPos.x, dy = e.y - downPos.y;
            owner.laneViewport.setViewPosition (jmax (0, panStartScrollX - dx),
                                                jmax (0, panStartScrollY - dy));
            return;
        }

        if (mode == Mode::AutoPoint)
        {
            auto* t = proj.findTrack (autoTrackId);
            auto* lane = t != nullptr ? t->findLane (autoTarget) : nullptr;
            if (lane == nullptr || autoIdx < 0 || autoIdx >= (int) lane->points.size()) return;
            if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
            auto r = autoRect (autoRow, autoLaneIdx);
            double b = owner.xToBeat (e.x);
            if (! e.mods.isAltDown()) b = snapTo (b, jmax (0.0625, app.arrangeGrid));
            // clamped between the neighbours: dragging a node can never make the curve double back
            moveAutoPoint (lane->points, autoIdx, b, yToValue (e.y, r));
            proj.sendContentChanged();
            updateTip (e.getPosition());
            return;
        }

        if (mode == Mode::Erase)
        {
            auto hit = hitClip (e.getPosition());
            if (hit)
            {
                if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
                deleteClip (hit);
                proj.sendContentChanged();
            }
            return;
        }

        if (mode == Mode::MuteDrag)
        {
            auto hit = hitClip (e.getPosition());
            if (hit && muteTouched.count (hit.clipId) == 0)
            {
                if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
                muteTouched.insert (hit.clipId);
                if (hit.audio) { if (auto* c = hit.track->findAudio (hit.clipId)) c->muted = true; }
                else if (auto* c = hit.track->findMidi (hit.clipId)) c->muted = true;
                proj.sendContentChanged();
            }
            return;
        }

        if (mode == Mode::DrawClip)
        {
            moved = true;
            const double g = app.arrangeGrid > 0 ? app.arrangeGrid : 0.25;
            const double b = snapTo (jmax (0.0, owner.xToBeat (e.x)), g);
            drawStart = jmin (drawStart, b);
            drawEnd = jmax (drawEnd, b);
            repaint();
            return;
        }

        if (mode == Mode::GainAll)
        {
            auto* t = proj.findTrack (gainTrackId);
            auto* ac = t != nullptr ? t->findAudio (gainClipId) : nullptr;
            if (ac == nullptr) return;
            if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
            const float delta = (float) (gainDragStartY - e.y) / 1.2f;
            ac->gainEnv = gainBackup;
            for (auto& p : ac->gainEnv) p.db = jlimit (-24.0f, 12.0f, p.db + delta);
            autoTip = (delta >= 0.0f ? "+" : "") + String (delta, 1) + " dB";
            autoTipPos = e.getPosition();
            proj.sendContentChanged();
            repaint();
            return;
        }

        if (mode == Mode::GainPoint || mode == Mode::GainFree)
        {
            auto* t = proj.findTrack (gainTrackId);
            auto* ac = t != nullptr ? t->findAudio (gainClipId) : nullptr;
            if (ac == nullptr) return;
            const double ppb = app.pxPerBeat;
            const double clipBeats = proj.clipBeats (ac->start, ac->lengthSec);
            const double beat = gainSnapBeat (*ac, clipBeats, ppb, e.x, app.arrangeGrid,
                                              e.mods.isCtrlDown() || e.mods.isCommandDown());
            const float db = yToGainDb (gainRect, (float) e.y);
            auto& v = ac->gainEnv;
            if (mode == Mode::GainPoint && gainPointIdx >= 0 && gainPointIdx < (int) v.size())
            {
                // anchors may never cross: clamp between the two neighbours
                const double lo = gainPointIdx > 0 ? v[(size_t) gainPointIdx - 1].beat + 1.0e-4 : 0.0;
                const double hi = gainPointIdx + 1 < (int) v.size() ? v[(size_t) gainPointIdx + 1].beat - 1.0e-4 : clipBeats;
                v[(size_t) gainPointIdx] = { jlimit (lo, jmax (lo, hi), beat), db };
            }
            else
            {
                // freehand: drop points along the mouse path, kept sorted by time
                auto it = std::lower_bound (v.begin(), v.end(), beat,
                                            [] (const ClipGainPoint& p, double b) { return p.beat < b; });
                if (it != v.end() && std::abs (it->beat - beat) < 1.0e-4) it->db = db;
                else v.insert (it, ClipGainPoint { beat, db });
            }
            autoTip = String (db, 1) + " dB";
            autoTipPos = e.getPosition();
            proj.sendContentChanged();
            repaint();
            return;
        }

        if (mode == Mode::Rubber)
        {
            rubber = Rectangle<int>::leftTopRightBottom (jmin (downPos.x, e.x), jmin (downPos.y, e.y), jmax (downPos.x, e.x), jmax (downPos.y, e.y));
            std::set<int> sel = baseSelection;
            for (size_t i = 0; i < proj.tracks.size(); ++i)
            {
                auto& t = *proj.tracks[i];
                for (auto& c : t.midiClips)
                    if (regionRect ((int) i, c.start, c.length).toNearestInt().intersects (rubber)) sel.insert (c.id);
                for (auto& c : t.audioClips)
                    if (regionRect ((int) i, c.start, proj.clipBeats (c.start, c.lengthSec)).toNearestInt().intersects (rubber)) sel.insert (c.id);
            }
            if (sel != app.selectedClips) { app.selectedClips = sel; app.notifySelection(); }
            repaint();
            return;
        }

        if (mode == Mode::Xfade)
        {
            auto* t = proj.findTrack (xfadeTrackId);
            auto* a = t != nullptr ? t->findAudio (xfadeA) : nullptr;
            auto* b = t != nullptr ? t->findAudio (xfadeB) : nullptr;
            if (a == nullptr || b == nullptr) return;
            if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
            // dragging left lengthens the crossfade: B starts earlier while its audio
            // content and its end stay exactly where they were
            const double secPerBeat = 60.0 / jmax (1.0, proj.bpmAt (xfadeOrigBStart));
            const double target = xfadeOrigOverlap + (downBeat - owner.xToBeat (e.x)) * secPerBeat;
            const double got = proj.resizeCrossfade (*a, *b, target);
            autoTip = TRANS ("Crossfade") + " " + String (got, 2) + " s";
            // the overlap can stop growing when the later clip has no head material left
            if (target > got + 0.005) autoTip += "  " + TRANS ("(limited by the source length)");
            autoTipPos = e.getPosition();
            proj.sendContentChanged();
            repaint();
            return;
        }

        if (mode == Mode::Fade || mode == Mode::ClipGain)
        {
            auto* t = proj.findTrack (gripTrackId);
            auto* c = t != nullptr ? t->findAudio (gripClipId) : nullptr;
            if (c == nullptr) return;
            if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
            if (mode == Mode::Fade)
            {
                const double cap = 0.9 * c->lengthSec;
                const double startSec = proj.beatsToSeconds (c->start);
                const double mouseSec = proj.beatsToSeconds (jmax (0.0, (double) e.x / app.pxPerBeat));
                const bool both = e.mods.isAltDown();   // Alt: both fades follow the drag
                if (gripType == 1)
                {
                    c->fadeIn = jlimit (0.0, cap, mouseSec - startSec);
                    if (both) c->fadeOut = jlimit (0.0, cap, c->fadeIn);
                }
                else
                {
                    c->fadeOut = jlimit (0.0, cap, startSec + c->lengthSec - mouseSec);
                    if (both) c->fadeIn = jlimit (0.0, cap, c->fadeOut);
                }
                const double sec = gripType == 1 ? c->fadeIn : c->fadeOut;
                const double beats = proj.secondsToBeats (startSec + sec) - c->start;
                autoTip = (gripType == 1 ? TRANS ("Fade In") : TRANS ("Fade Out")) + " "
                          + String (sec, 2) + " s  (" + String (beats, 2) + " " + TRANS ("beats") + ")";
            }
            else
            {
                // drag the clip-gain line up / down (up = louder); 1.2 px per dB keeps the
                // line exactly under the mouse
                const float dy = (float) (gripStartY - e.y);
                c->gainDb = jlimit (-24.0f, 12.0f, origGainDb + dy / 1.2f);
                autoTip = String (c->gainDb, 1) + " dB";
            }
            autoTipPos = e.getPosition();
            proj.sendContentChanged();
            repaint();
            return;
        }

        if (items.empty() || mode == Mode::None) return;
        if (! moved && e.getDistanceFromDragStart() < 3) return;
        if (! moved)
        {
            moved = true;
            proj.checkpoint();
            checkpointed = true;
            if (duplicateOnDrag) duplicateItems();
        }

        const auto* pi = primaryItem();
        if (pi == nullptr) return;
        const double grid = e.mods.isAltDown() && mode != Mode::Move ? 0.0 : app.arrangeGrid;
        const double beat = owner.xToBeat (e.x);

        if (mode == Mode::Move)
        {
            double delta = snapTo (pi->origStart + (beat - downBeat), grid) - pi->origStart;
            double minStart = 1.0e12;
            for (auto& it : items) minStart = jmin (minStart, it.origStart);
            if (minStart + delta < 0.0) delta = -minStart;

            const int dRow = owner.trackIndexAtY (e.y) - primaryTrackIdx;
            for (auto& it : items)
            {
                int ti = proj.indexOfTrack (it.origTrackId);
                int targetIdx = jlimit (0, (int) proj.tracks.size() - 1, ti + (owner.trackIndexAtY (e.y) < 0 ? 0 : dRow));
                auto* target = proj.tracks[(size_t) targetIdx].get();
                auto* cur = proj.findTrack (it.trackId);
                if (cur == nullptr) continue;
                const TrackType need = it.audio ? TrackType::Audio : TrackType::Instrument;
                if (target->type != need) target = cur;
                if (target != cur) moveClipTo (it, cur, target);
                if (it.audio)
                {
                    if (auto* c = target->findAudio (it.clipId)) c->start = it.origStart + delta;
                }
                else if (auto* c = target->findMidi (it.clipId)) c->start = it.origStart + delta;
            }
        }
        else if (mode == Mode::ResizeR)
        {
            const double endNow = snapTo (pi->origStart + pi->origLen + (beat - downBeat), grid);
            const double delta = endNow - (pi->origStart + pi->origLen);
            for (auto& it : items)
            {
                auto* t = proj.findTrack (it.trackId);
                if (t == nullptr) continue;
                if (it.audio)
                {
                    if (auto* c = t->findAudio (it.clipId))
                    {
                        const double maxLen = c->data != nullptr
                            ? (c->data->lengthSeconds() - c->offsetSec) / jmax (1.0e-9, c->sourceRate()) : 1.0e9;
                        const double newLen = proj.beatsToSeconds (it.origStart + it.origLen + delta) - proj.beatsToSeconds (it.origStart);
                        c->lengthSec = jlimit (0.02, jmax (0.02, maxLen), newLen);
                    }
                }
                else if (auto* c = t->findMidi (it.clipId))
                    c->length = jmax (grid > 0 ? grid : 0.125, it.origLen + delta);
            }
        }
        else if (mode == Mode::ResizeL)
        {
            double delta = snapTo (pi->origStart + (beat - downBeat), grid) - pi->origStart;
            for (auto& it : items)
            {
                auto* t = proj.findTrack (it.trackId);
                if (t == nullptr) continue;
                double d = jlimit (-it.origStart, it.origLen - 0.125, delta);
                if (it.audio)
                {
                    if (auto* c = t->findAudio (it.clipId))
                    {
                        const double s0 = proj.beatsToSeconds (it.origStart);
                        const double dSec = proj.beatsToSeconds (it.origStart + d) - s0;
                        const double rate = jmax (1.0e-9, c->sourceRate());
                        const double off = jmax (0.0, it.origOffset + dSec * rate);
                        const double consumed = (off - it.origOffset) / rate;   // timeline seconds
                        c->start = proj.secondsToBeats (s0 + consumed);
                        c->offsetSec = off;
                        c->lengthSec = jmax (0.02, it.origLenSec - consumed);
                    }
                }
                else if (auto* c = t->findMidi (it.clipId))
                {
                    // notes keep their absolute position: shift them relative to the new start
                    const double prevStart = c->start;
                    c->start = it.origStart + d;
                    c->length = it.origLen - d;
                    const double shift = prevStart - c->start;
                    if (std::abs (shift) > 1.0e-9)
                    {
                        for (auto& n : c->notes) n.start += shift;
                        for (auto& cc : c->ccs) cc.beat += shift;
                    }
                }
            }
        }
        proj.sendContentChanged();
        owner.updateContentSize();
    }

    void moveClipTo (Item& it, Track* from, Track* to)
    {
        if (it.audio)
        {
            for (size_t i = 0; i < from->audioClips.size(); ++i)
                if (from->audioClips[i].id == it.clipId)
                {
                    to->audioClips.push_back (std::move (from->audioClips[i]));
                    from->audioClips.erase (from->audioClips.begin() + (long long) i);
                    break;
                }
        }
        else
        {
            for (size_t i = 0; i < from->midiClips.size(); ++i)
                if (from->midiClips[i].id == it.clipId)
                {
                    to->midiClips.push_back (std::move (from->midiClips[i]));
                    from->midiClips.erase (from->midiClips.begin() + (long long) i);
                    break;
                }
        }
        it.trackId = to->id;
    }

    void duplicateItems()
    {
        auto& proj = owner.app.project;
        std::set<int> newSel;
        int newPrimary = primaryClip;
        for (auto& it : items)
        {
            auto* t = proj.findTrack (it.trackId);
            if (t == nullptr) continue;
            const int nid = proj.newId();
            if (it.audio)
            {
                if (auto* c = t->findAudio (it.clipId)) { auto copy = *c; copy.id = nid; t->audioClips.push_back (copy); }
            }
            else if (auto* c = t->findMidi (it.clipId))
            {
                auto copy = *c;
                copy.id = nid;
                for (auto& n : copy.notes) n.id = proj.newId();
                for (auto& cc : copy.ccs) cc.id = proj.newId();
                t->midiClips.push_back (copy);
            }
            if (it.clipId == primaryClip) newPrimary = nid;
            it.clipId = nid;
            newSel.insert (nid);
        }
        primaryClip = newPrimary;
        owner.app.selectedClips = newSel;
        owner.app.notifySelection();
    }

    void mouseUp (const MouseEvent& e) override
    {
        auto& app = owner.app;
        auto& proj = app.project;
        if (mode == Mode::DrawClip)
        {
            if (auto* t = proj.findTrack (drawTrackId))
            {
                const double g = app.arrangeGrid > 0 ? app.arrangeGrid : 0.25;
                double s = jmin (drawStart, drawEnd);
                double len = jmax (g, std::abs (drawEnd - drawStart));
                if (! moved) { s = snapTo (jmax (0.0, downBeat), g); len = g; }   // plain click: one grid cell
                proj.checkpoint();
                MidiClip c;
                c.id = proj.newId();
                c.name = t->name;
                c.start = s;
                c.length = len;
                t->midiClips.push_back (std::move (c));
                proj.sendContentChanged();
                app.selectClip (t->id, t->midiClips.back().id, false);
            }
        }
        else if (mode == Mode::GainPoint || mode == Mode::GainFree || mode == Mode::GainAll)
        {
            if (auto* t = proj.findTrack (gainTrackId))
                if (auto* ac = t->findAudio (gainClipId))
                {
                    std::sort (ac->gainEnv.begin(), ac->gainEnv.end(),
                               [] (const ClipGainPoint& a, const ClipGainPoint& b) { return a.beat < b.beat; });
                    proj.sendContentChanged();
                }
            gainPointIdx = -1;
            gainBackup.clear();
            autoTip = {};
            repaint();
        }
        else if (mode == Mode::AutoPoint || mode == Mode::Fade || mode == Mode::ClipGain || mode == Mode::Xfade)
        {
            autoTip = {};
            repaint();
        }
        else if (mode == Mode::Rubber)
        {
            rubber = {};
            repaint();
        }
        else if ((mode == Mode::Move || mode == Mode::ResizeL || mode == Mode::ResizeR) && ! moved && ! e.mods.isPopupMenu())
        {
            // plain click on an already selected clip in a multi-selection collapses it
            if (! e.mods.isShiftDown() && ! e.mods.isCtrlDown() && app.selectedClips.size() > 1)
                app.selectClip (app.selectedTrack, primaryClip, false);
        }
        items.clear();
        if (mode == Mode::Pan) setMouseCursor (MouseCursor::NormalCursor);
        mode = Mode::None;
        owner.updateContentSize();
        repaint();
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        auto& app = owner.app;
        auto& proj = app.project;
        // with the pencil / eraser / mute tools a double click has no extra meaning
        if (app.arrangeTool != ArrangeTool::Select) return;
        // double-click a grip clears it
        auto grip = hitAudioGrip (e.getPosition());
        if (grip.type != 0)
        {
            if (grip.type == 3)
            {
                bool anyGain = std::abs (grip.clip->gainDb) > 0.01f;
                if (grip.clip->gainLine)
                    for (auto& p : grip.clip->gainEnv) anyGain = anyGain || std::abs (p.db) > 0.01f;
                if (anyGain)   // when there is nothing to reset, fall through to the editor
                {
                    proj.checkpoint();
                    grip.clip->gainDb = 0.0f;
                    if (grip.clip->gainLine)
                        for (auto& p : grip.clip->gainEnv) p.db = 0.0f;
                    proj.sendContentChanged();
                    return;
                }
            }
            else
            {
                proj.checkpoint();
                if (grip.type == 1) grip.clip->fadeIn = 0.0;
                else grip.clip->fadeOut = 0.0;
                proj.sendContentChanged();
                return;
            }
        }
        if (hitCrossfade (e.getPosition())) return;   // no double-click action on the x-fade handle
        int row, laneIdx;
        if (isAutoArea (e.getPosition(), row, laneIdx))
        {
            AutoLane* lane = nullptr;
            const int hit = hitAutoPoint (e.getPosition(), row, laneIdx, lane);
            if (hit >= 0 && lane != nullptr)
            {
                proj.checkpoint();
                lane->points.erase (lane->points.begin() + hit);
                proj.sendContentChanged();
            }
            return;
        }
        auto h = hitClip (e.getPosition());
        if (h)
        {
            if (! h.audio)
            {
                app.openClipInEditor (h.track->id, h.clipId);
                return;
            }
            if (h.track->frozen) return;   // a frozen clip only plays its render
            // double-clicking an audio clip opens the fullscreen gain envelope editor
            if (auto* ac = h.track->findAudio (h.clipId))
            {
                if (! ac->gainLine)
                {
                    proj.checkpoint();
                    ac->gainLine = true;
                    if (ac->gainEnv.empty())
                    {
                        const double cb = proj.clipBeats (ac->start, ac->lengthSec);
                        ac->gainEnv.push_back ({ 0.0, ac->gainDb });
                        ac->gainEnv.push_back ({ cb, ac->gainDb });
                    }
                    proj.sendContentChanged();
                }
                app.selectClip (h.track->id, h.clipId, false);
                owner.repaintClipArea();
                if (owner.onEditClipEnvelope) owner.onEditClipEnvelope (h.track->id, h.clipId);
            }
            return;
        }
        const int idx = owner.trackIndexAtY (e.y);
        if (idx < 0) return;
        auto& t = *proj.tracks[(size_t) idx];
        if (t.type != TrackType::Instrument) return;

        proj.checkpoint();
        const double barStart = tempomap::barStartAtOrBefore (proj.tempo, jmax (0.0, owner.xToBeat (e.x)));
        int n = 4, d = 4;
        proj.meterAt (barStart, n, d);
        MidiClip c;
        c.id = proj.newId();
        c.name = t.name;
        c.start = barStart;
        c.length = n * 4.0 / d;
        const int cid = c.id;
        t.midiClips.push_back (std::move (c));
        proj.sendContentChanged();
        app.selectClip (t.id, cid, false);
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
        {
            // vertical track zoom: smooth up/down scaling of track rows
            owner.zoomTrackHeights (ArrangeView::notchesOf (w), e.y);
        }
        else if (e.mods.isAltDown())
        {
            // horizontal zoom with the same stepped ladder as the bar ruler
            owner.zoomPxPerBeat (ArrangeView::notchesOf (w), e.x);
        }
        else if (e.mods.isShiftDown())
        {
            auto pos = owner.laneViewport.getViewPosition();
            owner.laneViewport.setViewPosition (jmax (0, pos.x - (int) (w.deltaY * 300.0f)), pos.y);
        }
        else
            owner.laneViewport.mouseWheelMove (e.getEventRelativeTo (&owner.laneViewport), w);
    }

    //-- context menu ---------------------------------------------------------
    void showClipMenu (const ClipHit& h, Point<int> pos)
    {
        PopupMenu m;
        m.addItem (1, TRANS ("Rename..."));
        if (! h.audio) m.addItem (2, TRANS ("Open in Piano Roll"));

        // take stacks: every pass recorded over this range; the active one is the one you hear
        std::vector<int> takeClipIds;                 // ordered, id = 500 + position
        {
            juce::int64 group = 0;
            bool freshActive = true;
            if (h.audio) { if (auto* ac = h.track->findAudio (h.clipId)) { group = ac->takeGroup; freshActive = ac->takeActive; } }
            else         { if (auto* mc = h.track->findMidi (h.clipId))  { group = mc->takeGroup; freshActive = mc->takeActive; } }

            if (group != 0)
            {
                std::vector<std::pair<int, int>> members;      // takeIndex -> clip id
                int activeId = 0;
                for (auto& c : h.track->audioClips)
                    if (c.takeGroup == group) { members.push_back ({ c.takeIndex, c.id }); if (c.takeActive) activeId = c.id; }
                for (auto& c : h.track->midiClips)
                    if (c.takeGroup == group) { members.push_back ({ c.takeIndex, c.id }); if (c.takeActive) activeId = c.id; }

                if (members.size() > 1)
                {
                    std::sort (members.begin(), members.end());
                    PopupMenu tm;
                    for (size_t i = 0; i < members.size(); ++i)
                    {
                        takeClipIds.push_back (members[i].second);
                        tm.addItem (500 + (int) i, TRANS ("Take") + " " + String (members[i].first + 1),
                                    true, members[i].second == activeId);
                        juce::ignoreUnused (freshActive);
                    }
                    m.addSubMenu (TRANS ("Take"), tm);
                }
            }
        }

        m.addSeparator();
        m.addItem (3, TRANS ("Split at Playhead"));
        m.addItem (13, TRANS ("Split at Cursor"));
        m.addItem (4, TRANS ("Duplicate"));
        m.addItem (9, TRANS ("Merge Selected Clips"), owner.app.selectedClips.size() >= 2);
        m.addItem (5, TRANS ("Set Cycle to Selection"));
        if (h.audio)
        {
            const auto* ac = h.track->findAudio (h.clipId);
            const bool on = ac != nullptr && ac->gainLine;
            // is this clip part of a crossfade right now?
            bool inXfade = false;
            if (ac != nullptr)
            {
                auto& pr = owner.app.project;
                const double aEnd = ac->start + pr.clipBeats (ac->start, ac->lengthSec);
                for (auto& o : h.track->audioClips)
                {
                    if (o.id == ac->id) continue;
                    const double oEnd = o.start + pr.clipBeats (o.start, o.lengthSec);
                    const double ov = jmin (aEnd, oEnd) - jmax (ac->start, o.start);
                    if (ov > 0.001) inXfade = true;
                }
            }
            m.addSeparator();
            // event timebase: does this clip follow the musical grid (bars) or absolute time?
            {
                PopupMenu tb;
                tb.addItem (110, TRANS ("Bars (follows the tempo)"), true, ac != nullptr && ac->timebase == Timebase::Bars);
                tb.addItem (111, TRANS ("Seconds (absolute time)"), true, ac != nullptr && ac->timebase == Timebase::Seconds);
                m.addSubMenu (TRANS ("Timebase"), tb);
            }
            // ARA editors (Melodyne & co) are inserted straight onto this clip's track: they take
            // the audio of the track as an ARA document instead of capturing the insert stream
            {
                juce::Array<PluginDescription> araList;
                for (auto& d : owner.app.plugins.known.getTypes())
                    if (! d.isInstrument && PluginManager::isAraCapable (d)) araList.add (d);
                if (! araList.isEmpty())
                {
                    PopupMenu araMenu;
                    for (int i = 0; i < araList.size(); ++i)
                        araMenu.addItem (1000 + i, araList[i].name);
                    m.addSubMenu (TRANS ("Insert ARA Plug-in"), araMenu);
                }
            }
            m.addItem (14, TRANS ("Add Crossfade"));
            m.addItem (15, TRANS ("Fade In to Playhead"));
            m.addItem (16, TRANS ("Fade Out from Playhead"));
            m.addItem (17, TRANS ("Clear Fades"));
            m.addItem (18, TRANS ("Reset Clip Gain"));

            // Studio One style fade presets, applied to every selected audio clip
            PopupMenu fadeInMenu, fadeOutMenu;
            for (int i = 0; i < 6; ++i)
            {
                fadeInMenu.addItem (20 + i, kFadePresetNames[i],
                                    ac == nullptr || std::abs (ac->fadeIn - kFadePresets[i]) > 0.0005);
                fadeOutMenu.addItem (30 + i, kFadePresetNames[i],
                                     ac == nullptr || std::abs (ac->fadeOut - kFadePresets[i]) > 0.0005);
            }
            m.addSubMenu (TRANS ("Fade In Length"), fadeInMenu);
            m.addSubMenu (TRANS ("Fade Out Length"), fadeOutMenu);

            PopupMenu xfLenMenu, xfCurveMenu;
            for (int i = 0; i < 6; ++i) xfLenMenu.addItem (40 + i, kXfadePresetNames[i], inXfade);
            xfCurveMenu.addItem (50, TRANS ("Equal Power"), inXfade, ac != nullptr && ac->xfadeCurve == 0);
            xfCurveMenu.addItem (51, TRANS ("Linear"), inXfade, ac != nullptr && ac->xfadeCurve == 1);
            xfCurveMenu.addItem (52, TRANS ("Symmetric (S)"), inXfade, ac != nullptr && ac->xfadeCurve == 2);
            m.addSubMenu (TRANS ("Crossfade Length"), xfLenMenu, inXfade);
            m.addSubMenu (TRANS ("Crossfade Curve"), xfCurveMenu, inXfade);

            m.addSeparator();
            m.addItem (60, TRANS ("Clip Gain +1 dB"));
            m.addItem (61, TRANS ("Clip Gain -1 dB"));
            m.addItem (10, TRANS ("Gain Envelope"), true, on);
            m.addItem (11, TRANS ("Free-draw Gain"), on, owner.app.clipGainFreehand);
            m.addItem (12, TRANS ("Clear Gain Envelope"), on && ac != nullptr && ! ac->gainEnv.empty());

            // non-destructive audio editing: reverse, speed (time stretch) and transpose
            m.addSeparator();
            m.addItem (70, ac != nullptr && ac->reversed ? TRANS ("Un-reverse Audio") : TRANS ("Reverse Audio"));
            m.addItem (71, TRANS ("Reset Reverse / Speed / Transpose"),
                       ac != nullptr && ac->hasEdit());
            {
                PopupMenu stretch;
                for (size_t i = 0; i < audioStretchPresets().size(); ++i)
                    stretch.addItem (72 + (int) i, audioStretchPresetNames()[i],
                                     ac == nullptr || std::abs (ac->stretch - audioStretchPresets()[i]) > 1.0e-6);
                stretch.addSeparator();
                stretch.addItem (80, TRANS ("Enter Speed Factor..."));
                m.addSubMenu (TRANS ("Time Stretch (Speed)"), stretch);
                PopupMenu pitch;
                for (size_t i = 0; i < audioPitchPresets().size(); ++i)
                    pitch.addItem (90 + (int) i, TRANS ("Semitones") + ": " + audioPitchPresetNames()[i],
                                   ac == nullptr || (int) std::lround (ac->pitchSemitones) != audioPitchPresets()[i]);
                pitch.addSeparator();
                pitch.addItem (99, TRANS ("Enter Semitones..."));
                m.addSubMenu (TRANS ("Transpose"), pitch);
            }
        }
        m.addSeparator();
        m.addItem (6, TRANS ("Copy"));
        m.addItem (7, TRANS ("Cut"));
        m.addItem (8, TRANS ("Delete"));
        const int tid = h.track->id, cid = h.clipId;
        const bool audio = h.audio;
        auto* ov = &owner;
        m.showMenuAsync (PopupMenu::Options(), [ov, tid, cid, audio, pos, takeClipIds] (int r)
        {
            auto& app = ov->app;
            auto* t = app.project.findTrack (tid);
            if (t == nullptr) return;
            if (r >= 1000)
            {
                // ARA plug-in chosen from the clip menu: insert it on this clip's track
                juce::Array<PluginDescription> araList;
                for (auto& d : app.plugins.known.getTypes())
                    if (! d.isInstrument && PluginManager::isAraCapable (d)) araList.add (d);
                const int idx = r - 1000;
                if (idx < 0 || idx >= araList.size()) return;
                String err;
                if (! app.engine.addInsert (tid, araList[idx], err))
                    AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon,
                                                      TRANS ("Insert ARA Plug-in"), err);
                else if (auto* tr = app.project.findTrack (tid))
                    app.showInsertedPluginEditor (tr->inserts.empty() ? 0 : tr->inserts.back().uid);
                return;
            }
            switch (r)
            {
                case 1:
                {
                    String cur = audio ? (t->findAudio (cid) ? t->findAudio (cid)->name : String()) : (t->findMidi (cid) ? t->findMidi (cid)->name : String());
                    promptText (TRANS ("Rename Region"), {}, cur, [&app, tid, cid, audio] (String s)
                    {
                        auto* tr = app.project.findTrack (tid);
                        if (tr == nullptr || s.isEmpty()) return;
                        app.project.checkpoint();
                        if (audio) { if (auto* c = tr->findAudio (cid)) c->name = s; }
                        else if (auto* c = tr->findMidi (cid)) c->name = s;
                        app.project.sendContentChanged();
                    });
                    break;
                }
                case 2: app.openClipInEditor (tid, cid); break;
                case 70: ov->reverseSelectedAudio(); return;
                case 71: ov->resetSelectedAudioEdits(); return;
                case 80:
                    promptText (TRANS ("Time Stretch"), TRANS ("Speed factor: 2 = twice as long, 0.5 = twice as fast"),
                                String (1.0, 2), [ov] (String s)
                    {
                        const double v = s.trim().getDoubleValue();
                        if (v > 0.0) ov->setSelectedAudioStretch (v);
                    });
                    return;
                case 99:
                    promptText (TRANS ("Transpose"), TRANS ("Semitones (-24 .. +24)"), "0", [ov] (String s)
                    {
                        ov->setSelectedAudioPitch ((int) std::lround (s.trim().getDoubleValue()));
                    });
                    return;
                case 3: ov->splitAtPlayhead(); break;
                case 13: ov->splitAt (ov->xToBeat ((double) pos.x)); break;
                case 500: case 501: case 502: case 503: case 504:
                case 505: case 506: case 507: case 508:
                {
                    // switch the active take: every other clip in the stack goes quiet
                    const size_t which = (size_t) (r - 500);
                    if (which >= takeClipIds.size()) return;
                    const int want = takeClipIds[which];
                    auto* tr = app.project.findTrack (tid);
                    if (tr == nullptr) return;
                    app.project.checkpoint();
                    juce::int64 group = 0;
                    for (auto& c : tr->audioClips) if (c.id == want) group = c.takeGroup;
                    for (auto& c : tr->midiClips) if (c.id == want) group = c.takeGroup;
                    if (group == 0) return;
                    for (auto& c : tr->audioClips) if (c.takeGroup == group) c.takeActive = (c.id == want);
                    for (auto& c : tr->midiClips) if (c.takeGroup == group) c.takeActive = (c.id == want);
                    app.selectedClips.clear();
                    app.selectedClips.insert (want);
                    app.project.sendContentChanged();
                    ov->repaintClipArea();
                    return;
                }
                case 14: ov->addCrossfadeSelected(); break;
                case 15: ov->fadesToPlayhead (true); break;
                case 16: ov->fadesToPlayhead (false); break;
                case 17: ov->clearFadesSelected(); break;
                case 18: ov->resetClipGainSelected(); break;
                case 20: case 21: case 22: case 23: case 24: case 25:
                    ov->setFadesSelected (true, kFadePresets[r - 20]); break;
                case 30: case 31: case 32: case 33: case 34: case 35:
                    ov->setFadesSelected (false, kFadePresets[r - 30]); break;
                case 40: case 41: case 42: case 43: case 44: case 45:
                    ov->setCrossfadeSelected (kXfadePresets[r - 40]); break;
                case 50: ov->setCrossfadeCurveSelected (0); break;
                case 51: ov->setCrossfadeCurveSelected (1); break;
                case 52: ov->setCrossfadeCurveSelected (2); break;
                case 110: ov->setClipTimebaseSelected (cid, Timebase::Bars); break;
                case 111: ov->setClipTimebaseSelected (cid, Timebase::Seconds); break;
                case 60: ov->nudgeClipGainSelected (1.0f); break;
                case 61: ov->nudgeClipGainSelected (-1.0f); break;
                case 4: ov->duplicateSelected(); break;
                case 9: ov->mergeSelectedClips(); break;
                case 10:
                {
                    if (! audio) break;
                    auto* ac = t->findAudio (cid);
                    if (ac == nullptr) break;
                    app.project.checkpoint();
                    ac->gainLine = ! ac->gainLine;
                    if (ac->gainLine && ac->gainEnv.empty())
                    {
                        // start as a flat line through the middle of the clip
                        const double cb = app.project.clipBeats (ac->start, ac->lengthSec);
                        ac->gainEnv.push_back ({ 0.0, ac->gainDb });
                        ac->gainEnv.push_back ({ cb, ac->gainDb });
                    }
                    app.project.sendContentChanged();
                    ov->repaintClipArea();
                    break;
                }
                case 11:
                    app.clipGainFreehand = ! app.clipGainFreehand;
                    ov->repaintClipArea();
                    break;
                case 12:
                    if (auto* ac = t->findAudio (cid))
                    {
                        app.project.checkpoint();
                        ac->gainEnv.clear();
                        app.project.sendContentChanged();
                        ov->repaintClipArea();
                    }
                    break;
                case 5: ov->setLoopToSelection(); break;
                case 6: ov->copySelected(); break;
                case 7: ov->cutSelected(); break;
                case 8: ov->deleteSelected(); break;
                default:
                    if (r >= 72 && r < 72 + (int) audioStretchPresets().size())
                        ov->setSelectedAudioStretch (audioStretchPresets()[(size_t) (r - 72)]);
                    else if (r >= 90 && r < 90 + (int) audioPitchPresets().size())
                        ov->setSelectedAudioPitch (audioPitchPresets()[(size_t) (r - 90)]);
                    break;
            }
        });
    }

    void updateTip (Point<int> p)
    {
        auto* t = owner.app.project.findTrack (autoTrackId);
        auto* lane = t != nullptr ? t->findLane (autoTarget) : nullptr;
        if (lane == nullptr || autoIdx < 0 || autoIdx >= (int) lane->points.size()) return;
        autoTip = valueText (lane, autoTarget, lane->points[(size_t) autoIdx].value);
        autoTipPos = p;
    }

private:
    enum class Mode { None, Move, ResizeL, ResizeR, Rubber, AutoPoint, Fade, ClipGain, Erase, MuteDrag, DrawClip, GainPoint, GainFree, GainAll, Xfade, Pan };

    ArrangeView& owner;
    Mode mode = Mode::None;
    bool moved = false, checkpointed = false, duplicateOnDrag = false;
    Point<int> downPos;
    double downBeat = 0.0;
    int primaryClip = 0, primaryTrackIdx = 0;
    std::vector<Item> items;
    Rectangle<int> rubber;
    std::set<int> baseSelection;

    int autoTrackId = 0, autoIdx = -1, autoRow = 0, autoLaneIdx = 0;
    String autoTarget;
    String autoTip;
    Point<int> autoTipPos;
    int panStartScrollX = 0, panStartScrollY = 0;

    // pencil tool state (draw a new MIDI clip)
    int drawTrackId = 0;
    double drawStart = 0.0, drawEnd = 0.0;
    // mute tool state (clips already touched during this drag)
    std::set<int> muteTouched;

    // audio clip fade / gain grips
    int gripType = 0, gripClipId = 0, gripTrackId = 0;
    Rectangle<float> gripRect;
    double gripSecPerPx = 0.0, origFade = 0.0, origFadeIn = 0.0, origFadeOut = 0.0;
    float origGainDb = 0.0f;
    int gripStartY = 0;

    // crossfade length drag (the round handle in the middle of an overlap)
    int xfadeTrackId = 0, xfadeA = 0, xfadeB = 0;
    double xfadeOrigBStart = 0.0, xfadeOrigOverlap = 0.0;

    // clip gain envelope editing (per clip, drawn with the pencil tool)
    int gainTrackId = 0, gainClipId = 0, gainPointIdx = -1;
    Rectangle<float> gainRect;
    int gainDragStartY = 0;
    std::vector<ClipGainPoint> gainBackup;   // envelope before an Alt-drag of the whole curve

    // vertical mapping of the clip-gain envelope: 0 dB sits in the middle of the clip body
    static Rectangle<float> gainBody (const Rectangle<float>& r)
    {
        return r.withTrimmedTop (16.0f).reduced (1.0f, 1.0f);
    }
    static float gainDbToY (const Rectangle<float>& r, float db)
    {
        auto body = gainBody (r);
        return jlimit (body.getY() + 2.0f, body.getBottom() - 2.0f, body.getCentreY() - db * 1.2f);
    }
    static float yToGainDb (const Rectangle<float>& r, float y)
    {
        auto body = gainBody (r);
        return jlimit (-24.0f, 12.0f, (body.getCentreY() - y) / 1.2f);
    }

    // index of the gain envelope point under the mouse (-1 when none)
    static int hitGainPoint (const AudioClip& c, const Rectangle<float>& r, Point<int> p, double ppb)
    {
        int best = -1;
        float bestD = 9.0f;
        for (size_t i = 0; i < c.gainEnv.size(); ++i)
        {
            const float px = r.getX() + (float) (c.gainEnv[i].beat * ppb);
            const float py = gainDbToY (r, c.gainEnv[i].db);
            const float d = jmax (std::abs ((float) p.x - px), std::abs ((float) p.y - py));
            if (d < bestD) { bestD = d; best = (int) i; }
        }
        return best;
    }

    // clip-relative beat for a mouse x, snapped to the arrange grid (Ctrl bypasses the snap)
    static double gainSnapBeat (const AudioClip& c, double clipBeats, double ppb, int x,
                                double grid, bool noSnap)
    {
        double absBeat = (double) x / ppb;
        if (! noSnap && grid > 0.0) absBeat = snapTo (absBeat, grid);
        return jlimit (0.0, clipBeats, absBeat - c.start);
    }
};

//==============================================================================
static String meterDbText (float gain)
{
    if (gain <= 0.0005f) return "-inf";
    return String (Decibels::gainToDecibels (gain), 1);
}

// "Lead" / "Master" for the header of a stand-alone automation row
static String automationTargetName (const Project& p, const Track& t)
{
    if (t.autoTargetTrack == 0) return TRANS ("Master");
    if (auto* target = p.findTrack (t.autoTargetTrack)) return target->name;
    return "#" + String (t.autoTargetTrack);
}

class ArrangeView::TrackHeader : public Component
{
public:
    // boundTrack != nullptr means the channel is not in project.tracks: the pinned master bus
    // row hands in Project::masterTrack so it gets exactly the same header as a track.
    TrackHeader (ArrangeView& o, int id, Track* boundTrack = nullptr)
        : owner (o), trackId (id), bound (boundTrack),
          muteBtn ("M", theme::accentDark, TRANS ("Mute")), soloBtn ("S", theme::accent, TRANS ("Solo")),
          armBtn ("R", theme::record, TRANS ("Record Enable")), monBtn ("I", theme::accent, TRANS ("Input Monitoring")),
          freezeBtn ("F", theme::accent, TRANS ("Freeze / Unfreeze Track")),
          monoBtn ("MONO", theme::accent, TRANS ("Mono"))
    {
        nameLabel.setFont (theme::font (13.0f, true));
        nameLabel.setEditable (false, ! isMasterRow(), false);
        nameLabel.setInterceptsMouseClicks (false, false);
        nameLabel.onTextChange = [this]
        {
            if (auto* t = track())
            {
                if (nameLabel.getText().isEmpty()) { refresh(); return; }
                owner.app.project.checkpoint();
                t->name = nameLabel.getText();
                owner.app.project.sendMixChanged();
                owner.app.project.sendContentChanged();
            }
        };
        addAndMakeVisible (nameLabel);

        listenBtn.onClick = [this]
        {
            if (auto* t = track())
            {
                owner.app.project.checkpoint();
                t->listenSolo = listenBtn.getToggleState();
                owner.app.project.sendMixChanged();
            }
        };
        for (auto* b : { &muteBtn, &soloBtn, &armBtn, &monBtn, &listenBtn, &monoBtn }) addAndMakeVisible (b);
        // the master's mono switch (Studio One style phase check): fold the main output to mono
        monoBtn.onClick = [this]
        {
            if (auto* t = track())
            {
                owner.app.project.checkpoint();
                t->mono = monoBtn.getToggleState();
                owner.app.project.sendMixChanged();       // the audio thread switches immediately
                owner.app.project.sendContentChanged();   // saved with the project
            }
        };
        muteBtn.onClick = [this] { if (auto* t = track()) { owner.app.project.checkpoint(); t->mute = muteBtn.getToggleState(); if (t->mute) t->solo = false; owner.app.project.sendMixChanged(); owner.app.project.sendContentChanged(); } };
        soloBtn.onClick = [this] { if (auto* t = track()) { owner.app.project.checkpoint(); t->solo = soloBtn.getToggleState(); if (t->solo) t->mute = false; owner.app.project.sendMixChanged(); owner.app.project.sendContentChanged(); } };
        armBtn.onClick = [this] { if (auto* t = track()) { t->arm = armBtn.getToggleState(); owner.app.project.sendMixChanged(); } };
        monBtn.onClick = [this] { if (auto* t = track()) { t->monitor = monBtn.getToggleState(); owner.app.project.sendMixChanged(); } };
        freezeBtn.onClick = [this]
        {
            auto* t = track();
            if (t == nullptr || isMasterRow()) return;
            if (t->frozen) { owner.app.unfreezeTrack (trackId); return; }
            // choose whether the insert effects are baked into the freeze
            PopupMenu m;
            m.addSectionHeader (TRANS ("Freeze: effects are baked; add new effects after freezing"));
            m.addItem (1, TRANS ("Freeze Track (with effects)"));
            m.addItem (2, TRANS ("Freeze Track (no effects)"));
            auto* appPtr = &owner.app;
            const int tid = trackId;
            m.showMenuAsync (PopupMenu::Options().withTargetComponent (&freezeBtn),
                             [appPtr, tid] (int r)
            {
                if (r == 1) appPtr->freezeTrack (tid, true);
                else if (r == 2) appPtr->freezeTrack (tid, false);
            });
        };
        addAndMakeVisible (freezeBtn);

        autoBtn.setClickingTogglesState (false);
        autoBtn.onClick = [this]
        {
            if (auto* t = track())
            {
                if (isMasterRow()) showAutomationLanesMenu (owner.app, *t, 0, &autoBtn);
                else               showAutomationLanesMenu (owner.app, trackId, &autoBtn);
            }
        };
        addAndMakeVisible (autoBtn);

        vol.setRange (0.0, 1.0, 0.0);
        vol.setDoubleClickReturnValue (true, 0.75);
        vol.setSliderStyle (Slider::LinearHorizontal);
        vol.setTextBoxStyle (Slider::NoTextBox, true, 0, 0);
        vol.setTooltip (TRANS ("Volume"));
        vol.onDragStart = [this]
        {
            owner.app.project.checkpoint();
            if (! isMasterRow()) owner.app.engine.touchBegin (trackId, "vol");
        };
        vol.onValueChange = [this]
        {
            if (updating) return;
            if (auto* t = track())
            {
                const float v = (float) vol.getValue();
                const float g = faderToGain (v);
                t->volumeDb = g <= 0.0005f ? -100.0f : Decibels::gainToDecibels (g);
                if (! isMasterRow()) owner.app.engine.touchValueAuto (trackId, "vol", v);
                owner.app.project.sendMixChanged();
            }
        };
        vol.onDragEnd = [this] { if (! isMasterRow()) owner.app.engine.touchEnd (trackId, "vol"); };
        addAndMakeVisible (vol);

        meter.setTooltip (TRANS ("Live level and highest peak (click to reset)"));
        meter.setInterceptsMouseClicks (true, false);
        meter.onReset = [this] { owner.app.meters.resetMaximum (isMasterRow() ? -1 : trackId); };
        addAndMakeVisible (meter);

        refresh();
    }

    Track* track() const { return bound != nullptr ? bound : owner.app.project.findTrack (trackId); }
    int getTrackId() const { return trackId; }
    bool isMasterRow() const { return bound != nullptr; }

    void updateMeter()
    {
        if (! isShowing() || track() == nullptr) return;
        const auto& m = owner.app.meters.get (isMasterRow() ? -1 : trackId);
        meter.setState (m.levelL, m.levelR, m.rmsL, m.rmsR,
                        m.holdL, m.holdR, m.maxL, m.maxR, m.clipped);
        if (meter.isMouseOverOrDragging())
            meter.setTooltip (TRANS ("Level") + " " + meterDbText (jmax (m.levelL, m.levelR)) + " dB   "
                              + TRANS ("Peak") + " " + meterDbText (jmax (m.holdL, m.holdR)) + " dB   "
                              + TRANS ("Max") + " " + meterDbText (jmax (m.maxL, m.maxR)) + " dB");
    }

    juce::Rectangle<int> meterBounds() const { return meter.getBounds(); }
    bool meterVisible() const { return meter.isVisible(); }

    void refresh()
    {
        auto* t = track();
        if (t == nullptr) return;
        updating = true;
        const bool master = isMasterRow();
        const bool group = t->isGroupTrack();

        if (! nameLabel.isBeingEdited()) nameLabel.setText (t->name, dontSendNotification);
        nameLabel.setEditable (false, ! master, false);

        // a stand-alone automation track is not an audio channel: it has no fader / mute / solo
        // and no inserts, only the curve drawn in its body and the parameter it drives
        if (t->isAutomation())
        {
            muteBtn.setVisible (false); monoBtn.setVisible (false); soloBtn.setVisible (false);
            armBtn.setVisible (false); monBtn.setVisible (false); listenBtn.setVisible (false);
            freezeBtn.setVisible (false); autoBtn.setVisible (false);
            vol.setVisible (false); meter.setVisible (false); meter.setBounds ({});
            dBTextArea = {};
            const String targetName = automationTargetName (owner.app.project, *t);
            nameLabel.setTooltip (TRANS ("Automation") + ": " + targetName + " - " + t->automationTitle());
            updating = false;
            resized();
            repaint();
            return;
        }

        // one rule for every header: a channel only shows the switches that do something for it,
        // in the same order and at the same place (M S R I L A F). Track, bus, effect return and
        // the master bus row therefore line up and read the same way.
        muteBtn.setVisible (true);
        monoBtn.setVisible (master);
        monoBtn.setToggleState (t->mono, dontSendNotification);
        monoBtn.setTooltip (TRANS ("Mono: fold the main output to mono (phase check)"));
        soloBtn.setVisible (! master);
        armBtn.setVisible (! master && ! group);
        monBtn.setVisible (! master && t->type == TrackType::Audio);
        listenBtn.setVisible (! master && ! t->isListenBus && ! group);
        freezeBtn.setVisible (! master && (t->type == TrackType::Instrument || t->type == TrackType::Audio));

        muteBtn.setToggleState (t->mute, dontSendNotification);
        muteBtn.setTooltip (master ? TRANS ("Mute the master bus (the whole mix)") : TRANS ("Mute"));
        listenBtn.setToggleState (t->listenSolo, dontSendNotification);
        soloBtn.setToggleState (t->solo, dontSendNotification);
        armBtn.setToggleState (t->arm, dontSendNotification);
        monBtn.setToggleState (t->monitor, dontSendNotification);
        freezeBtn.setToggleState (t->frozen, dontSendNotification);
        freezeBtn.setTooltip (t->frozen
                              ? TRANS ("Frozen: the instrument and its baked effects play from audio. "
                                       "New effects and automation still work; click to unfreeze")
                              : TRANS ("Freeze / Unfreeze Track"));
        autoBtn.setToggleState (t->hasVisibleLanes(), dontSendNotification);
        autoBtn.setTooltip (TRANS ("Show / Hide Automation")
                            + (t->inlineAutomationLane() != nullptr
                                   ? "\n" + TRANS ("Edit Automation in Track")
                                   : String()));
        if (! vol.isMouseButtonDown())
            vol.setValue (t->volumeDb <= -99.0f ? 0.0 : gainToFader (Decibels::decibelsToGain (t->volumeDb)), dontSendNotification);

        if (master)
            nameLabel.setTooltip (TRANS ("Master bus") + "\n" + TRANS ("Volume, mute and automation for the whole mix"));
        else
        {
            String outName = TRANS ("Master");
            if (t->outputBusId != 0)
                if (auto* out = owner.app.project.findTrack (t->outputBusId)) outName = out->name;
                else outName = "#" + String (t->outputBusId);
            StringArray sendNames;
            for (auto& s : t->sends)
                if (auto* st = owner.app.project.findTrack (s.target)) sendNames.add (st->name);
                else sendNames.add ("#" + String (s.target));
            nameLabel.setTooltip (TRANS ("Output") + ": " + outName
                                + "\n" + TRANS ("Sends") + ": " + (sendNames.isEmpty() ? String ("-") : sendNames.joinIntoString (", ")));
        }

        updating = false;
        resized();
        repaint();
    }

    void paint (Graphics& g) override
    {
        auto* t = track();
        if (t == nullptr) return;
        const bool master = isMasterRow();
        const bool sel = ! master && owner.app.selectedTracks.count (trackId) > 0;
        const int th = owner.trackH (*t);
        const int ah = owner.autoH (*t);
        auto r = getLocalBounds();
        g.setColour (sel ? theme::accentSoft.withAlpha (0.7f) : theme::panel);
        g.fillRect (r);
        // automation edit mode: the row is an envelope editor, so outline it clearly
        if (! master && t->automationEdit && t->inlineAutomationLane() != nullptr)
        {
            g.setColour (theme::accent.withAlpha (0.75f));
            g.drawRect (0, 0, getWidth(), th, 2);
        }
        g.setColour (t->colour);
        g.fillRect (0, 0, sel ? 6 : 4, th);
        g.setColour (theme::line);
        g.fillRect (0, th - 1, getWidth(), 1);
        const int lanes = t->laneRowCount();
        for (int i = 0; i < lanes; ++i)
        {
            auto* lane = t->visibleLaneAt (i);
            if (lane == nullptr) continue;
            const int top = th + i * ah;
            g.setColour (i % 2 == 0 ? theme::panelAlt : theme::panelDeep.withAlpha (0.55f));
            g.fillRect (0, top, getWidth(), ah - 1);
            g.setColour (theme::line);
            g.fillRect (0, top, getWidth(), 1);
            g.setColour (theme::accent);
            g.fillRect (0, top + 6, 3, jmax (6, ah - 12));
            String label = lane->label.isNotEmpty() ? TRANS (lane->label)
                          : TRANS (lane->target == "vol" ? "Volume" : lane->target == "pan" ? "Pan" : lane->target);
            g.setColour (theme::text);
            g.setFont (theme::font (11.5f, true));
            // the lane row belongs to its track: inside a folder it steps in with the track
            const int laneTextX = 10 + jlimit (0, 4, t->folderDepth) * kFolderIndentPx;
            g.drawText (label, laneTextX, top + 5, getWidth() - laneTextX - 26, 16, Justification::centredLeft);
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f, true));
            g.drawText ("x", getWidth() - 24, top + 4, 16, 16, Justification::centred);
        }
        if (lanes > 0)
        {
            g.setColour (theme::line);
            g.fillRect (0, getHeight() - 1, getWidth(), 1);
        }

        //-- folder hierarchy: tree lines + the disclosure arrow --------------------------------
        const int depth = jlimit (0, 4, t->folderDepth);
        const int indent = depth * kFolderIndentPx;
        g.setColour (theme::lineSoft);
        for (int d = 1; d <= depth; ++d)
        {
            const float x = (float) (8 + (d - 1) * kFolderIndentPx + 4);
            g.fillRect (x, 0.0f, 1.0f, (float) jmin (th, getHeight()));
            if (d == depth) g.fillRect (x, 17.0f, 8.0f, 1.0f);   // stub into this row's name
        }
        if (t->isFolder && ! master)
        {
            // click the arrow (see mouseDown) to fold / unfold everything packed into this folder
            Path arrow;
            const float ax = (float) (8 + indent + 2), ay = 12.0f;
            if (t->folderOpen) arrow.addTriangle (ax, ay, ax + 8.0f, ay, ax + 4.0f, ay + 6.0f);
            else               arrow.addTriangle (ax, ay - 2.0f, ax + 6.0f, ay + 2.0f, ax, ay + 6.0f);
            g.setColour (theme::textDim);
            g.fillPath (arrow);
        }

        if (t->isAutomation())
        {
            // the curve row: a type icon plus the channel and parameter it drives
            if (th >= kCompactTrackH)
                drawIcon (g, Icon::Automation, Rectangle<float> ((float) (14 + indent), 8.0f, 20.0f, 20.0f), t->colour);
            if (th >= 42)
            {
                const String targetName = automationTargetName (owner.app.project, *t);
                g.setColour (theme::textDim);
                g.setFont (theme::font (10.5f));
                g.drawFittedText (targetName, 14 + indent, 28, getWidth() - 24 - indent, 14,
                                  Justification::centredLeft, 1);
            }
            g.setColour (theme::line);
            g.fillRect (0, jmax (0, th - 1), getWidth(), 1);
            g.fillRect (getWidth() - 1, 0, 1, getHeight());
            return;
        }

        // routing tag (Studio One style: a compact colour-coded destination tag in the name row)
        if (th >= kCompactTrackH && ! master)
        {
            auto& proj = owner.app.project;
            String busName;
            if (t->outputBusId != 0)
            {
                auto* b = proj.findTrack (t->outputBusId);
                busName = b != nullptr ? b->name : ("#" + String (t->outputBusId));
            }
            const int nSends = (int) t->sends.size();
            int x = getWidth() - 8;
            if (busName.isNotEmpty() || nSends > 0)
            {
                if (nSends > 0)
                {
                    const int w = 20;
                    auto br = Rectangle<float> ((float) (x - w), 8.0f, (float) w, 12.0f);
                    g.setColour (theme::sendTrackColour.withAlpha (0.16f));
                    g.fillRoundedRectangle (br, 3.0f);
                    g.setColour (theme::sendTrackColour);
                    g.setFont (theme::font (9.5f, true));
                    g.drawText ("S" + String (nSends), br.toNearestInt(), Justification::centred);
                    x -= w + 3;
                }
                if (busName.isNotEmpty())
                {
                    const int w = jlimit (24, 64, x - 40);
                    auto br = Rectangle<float> ((float) (x - w), 7.0f, (float) w, 14.0f);
                    g.setColour (theme::busTrackColour.withAlpha (0.16f));
                    g.fillRoundedRectangle (br, 3.0f);
                    g.setColour (theme::busTrackColour);
                    g.setFont (theme::font (9.5f, true));
                    g.drawFittedText (String::fromUTF8 ("\xe2\x86\x92 ") + busName,
                                      br.toNearestInt().reduced (3, 0), Justification::centredLeft, 1);
                }
            }
        }
        g.setColour (theme::line);
        g.fillRect (getWidth() - 1, 0, 1, getHeight());

        // the draggable border at the bottom of the track body
        g.setColour (theme::textDim.withAlpha (0.35f));
        g.fillRect (getWidth() * 0.35f, (float) th - 2.0f, getWidth() * 0.3f, 1.0f);

        // icon (short rows have no room for it)
        if (th >= kCompactTrackH)
        {
            auto ic = Rectangle<float> ((float) (14 + indent + (t->isFolder && ! master ? 10 : 0)), 8.0f, 20.0f, 20.0f);
            const Icon typeIcon = master ? Icon::Mixer
                                : t->isFolder ? Icon::Folder
                                : t->isBus() ? Icon::Mixer
                                : t->isSend() ? Icon::Plugin
                                : t->type == TrackType::Instrument ? Icon::Piano : Icon::Automation;
            drawIcon (g, typeIcon, ic, t->colour.darker (0.1f));
            if (t->type == TrackType::Audio && ! master)
            {
                g.setColour (t->colour.darker (0.1f));
                for (int i = 0; i < 5; ++i)
                {
                    const float h = 4.0f + (float) ((i * 7) % 5) * 2.5f;
                    g.fillRect (ic.getX() + 2.0f + (float) i * 3.6f, ic.getCentreY() - h * 0.5f, 2.0f, h);
                }
            }
        }

        // the fader value, in the same spot on every header (the master row's readout too)
        if (! dBTextArea.isEmpty())
        {
            const float norm = (float) vol.getValue();
            const float gain = faderToGain (norm);
            const String db = gain <= 0.0005f ? String ("-inf dB") : String (Decibels::gainToDecibels (gain), 1) + " dB";
            g.setColour (gain <= 0.0005f ? theme::textFaint : theme::textDim);
            g.setFont (theme::font (10.5f, true));
            g.drawText (db, dBTextArea, Justification::centredRight);
        }
    }

    void resized() override
    {
        auto* t = track();
        // packed tracks step in by 10 px per folder level (capped so deep nesting still leaves
        // room for the name and the switches); a folder also reserves room for its arrow
        const int indent = t != nullptr ? jlimit (0, 4, t->folderDepth) * kFolderIndentPx : 0;
        const bool folderArrow = t != nullptr && t->isFolder && ! isMasterRow();
        auto r = getLocalBounds().withTrimmedLeft (8 + indent + (folderArrow ? 10 : 0));
        const int th = t != nullptr ? owner.trackH (*t) : owner.app.trackHeight;
        dBTextArea = {};
        if (t != nullptr && t->isAutomation())
        {
            // the curve row: name on top (after the type icon), the channel it drives underneath
            auto top = r.removeFromTop (jmax (20, jmin (th - 2, 44)));
            top.removeFromLeft (28);
            nameLabel.setBounds (top.removeFromTop (jmin (24, top.getHeight())).reduced (2, 3));
            vol.setVisible (false);
            meter.setVisible (false);
            meter.setBounds ({});
            return;
        }
        if (th < kCompactTrackH)
        {
            // short rows: name and toggles share one line, the volume slider is hidden.
            // The same switches appear in the same order as in a tall header, only smaller,
            // so nothing is left behind at a stale position (that used to put L under the row).
            auto row = Rectangle<int> (r.getX() + 2, r.getY() + jmax (0, (th - 18) / 2), r.getWidth() - 4, jmin (18, th));
            r.removeFromTop (th);
            std::vector<Component*> compactBtns;
            for (auto* b : { (Component*) &muteBtn, (Component*) &monoBtn, (Component*) &soloBtn,
                             (Component*) &armBtn, (Component*) &monBtn, (Component*) &listenBtn,
                             (Component*) &autoBtn, (Component*) &freezeBtn })
                if (b->isVisible()) compactBtns.push_back (b);
            const int n = (int) compactBtns.size(), gap = 3;
            // keep at least ~60 px for the name, then shrink the switches into what is left.
            // They stay right-aligned as a group but in the same left-to-right order as the tall
            // header (M MONO S R I L A F), so the switches do not jump around when a row collapses.
            const int cw = n > 0 ? jlimit (15, 22, (row.getWidth() - 60 - gap * (n - 1)) / n) : 22;
            auto widthFor = [&] (Component* b) { return b == (Component*) &monoBtn ? jmax (cw, 34) : cw; };
            int total = 0;
            for (auto* b : compactBtns) total += widthFor (b) + gap;
            total -= gap;
            auto group = row.removeFromRight (jmin (row.getWidth(), total));
            for (auto* b : compactBtns) { b->setBounds (group.removeFromLeft (widthFor (b))); group.removeFromLeft (gap); }
            nameLabel.setBounds (row);
            vol.setVisible (false);
            meter.setVisible (false);
            meter.setBounds ({});
        }
        else
        {
            auto top = r.removeFromTop (th);
            auto nameRow = top.removeFromTop (30);
            nameRow.removeFromLeft (28);
            const bool routing = t != nullptr && ! isMasterRow() && (t->outputBusId != 0 || ! t->sends.empty());
            nameLabel.setBounds (nameRow.reduced (2, 3).withTrimmedRight (routing ? 98 : 6));

            auto btnRow = top.removeFromTop (24).reduced (4, 0);
            // the same switches, in the same order, on every header; they shrink when a channel
            // has many of them, so nothing is ever clipped off the right edge
            std::vector<Component*> btns;
            for (auto* b : { (Component*) &muteBtn, (Component*) &monoBtn, (Component*) &soloBtn,
                             (Component*) &armBtn, (Component*) &monBtn, (Component*) &listenBtn,
                             (Component*) &autoBtn, (Component*) &freezeBtn })
                if (b->isVisible()) btns.push_back (b);
            const int n = (int) btns.size(), gap = 4;
            const int bw = n > 0 ? jlimit (18, 30, (btnRow.getWidth() - gap * (n - 1)) / n) : 28;
            auto widthFor = [&] (Component* b) { return b == (Component*) &monoBtn ? jmax (bw, 44) : bw; };
            for (auto* b : btns) { b->setBounds (btnRow.removeFromLeft (widthFor (b))); btnRow.removeFromLeft (gap); }

            // the fader row ends in the same dB readout on every header: the slider shrinks to
            // make room for it instead of the value disappearing when a channel has many switches
            top.removeFromTop (2);
            auto volRow = top.reduced (6, 0).withHeight (jmin (20, jmax (0, top.getHeight())));
            dBTextArea = volRow.removeFromRight (jmin (58, volRow.getWidth() / 2));
            auto meterRow = volRow.removeFromBottom (jmin (5, volRow.getHeight()));
            vol.setBounds (volRow.withTrimmedRight (4));
            meter.setBounds (meterRow.withTrimmedRight (4));
            const bool showFader = th >= 70 && volRow.getHeight() >= 8;
            vol.setVisible (showFader);
            meter.setVisible (showFader && meter.getWidth() > 10);
        }

    }

    void mouseMove (const MouseEvent& e) override
    {
        auto* t = track();
        if (t == nullptr) return;
        const int th = owner.trackH (*t);
        // the body is draggable (move the track), only the bottom border resizes it
        if (std::abs (e.y - th) <= 4) setMouseCursor (MouseCursor::UpDownResizeCursor);
        else setMouseCursor (isMasterRow() ? MouseCursor::NormalCursor : MouseCursor::DraggingHandCursor);
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (resizing)
        {
            auto* t = track();
            if (t == nullptr) return;
            if (isMasterRow()) owner.setMasterRowHeight (resizeStartH + e.getDistanceFromDragStartY());
            else               owner.setTrackRowHeight (trackId, resizeStartH + e.getDistanceFromDragStartY());
            return;
        }
        if (! reorderReady) return;
        if (! reorderDragging && e.getDistanceFromDragStart() > 6)
        {
            reorderDragging = true;
            owner.reorderTrackId = trackId;
        }
        if (reorderDragging)
        {
            const int drop = owner.dropIndexForScreenY (e.getScreenY());
            owner.showReorderDrop (drop);
        }
    }

    void mouseUp (const MouseEvent&) override
    {
        resizing = false;
        reorderReady = false;
        if (reorderDragging)
        {
            reorderDragging = false;
            owner.endTrackReorder (true);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto* t = track();
        const bool master = isMasterRow();

        // right-click: the channel menu. The master row has its own (no rename / delete).
        if (e.mods.isPopupMenu())
        {
            if (master) showMasterMenu();
            else
            {
                if (owner.app.selectedTracks.count (trackId) == 0) owner.app.selectTrack (trackId);
                showTrackMenu (owner.app, trackId, this, [this] { nameLabel.showEditor(); });
            }
            return;
        }

        // left-click: plain = single, Ctrl = toggle, Shift = range from the previous track
        if (! master) owner.app.selectTrack (trackId, e.mods.isCtrlDown() || e.mods.isCommandDown(), e.mods.isShiftDown());

        // the folder disclosure arrow sits in the indentation before the name
        if (t != nullptr && t->isFolder && ! master)
        {
            const int indent = jlimit (0, 4, t->folderDepth) * kFolderIndentPx;
            if (Rectangle<int> (8 + indent, 2, 16, 24).contains (e.getPosition()))
            {
                owner.app.toggleFolderOpen (trackId);
                return;
            }
        }

        if (t != nullptr)
        {
            const int th = owner.trackH (*t);
            if (std::abs (e.y - th) <= 4)
            {
                // drag the border at the bottom of the track body to resize this row alone
                resizing = true;
                resizeStartH = th;
                return;
            }
            const int lanes = t->laneRowCount();
            const int ah = owner.autoH (*t);
            const int off = e.y - th;
            if (lanes > 0 && off >= 0 && off < lanes * ah)
            {
                const int laneIdx = jlimit (0, lanes - 1, off / ah);
                auto* lane = t->visibleLaneAt (laneIdx);
                if (lane != nullptr)
                {
                    const String target = lane->target;
                    if (e.x >= getWidth() - 30)   // the little cross hides the lane
                    {
                        lane->visible = false;
                        t->showAutomation = t->hasVisibleLanes();
                        owner.app.project.sendStructureChanged();
                        owner.app.project.sendContentChanged();
                        return;
                    }
                    PopupMenu lm;
                    lm.addItem (1, TRANS ("Hide Lane"));
                    lm.addItem (2, TRANS ("Remove Lane"));
                    lm.addSeparator();
                    lm.addItem (3, TRANS ("Add Automation..."));
                    lm.addItem (4, TRANS ("Move to Automation Track"));
                    auto* appPtr = &owner.app;
                    const int tid = master ? 0 : trackId;
                    lm.showMenuAsync (PopupMenu::Options().withTargetComponent (this), [appPtr, tid, target] (int r)
                    {
                        auto* tr = tid == 0 ? &appPtr->project.masterTrack : appPtr->project.findTrack (tid);
                        if (tr == nullptr) return;
                        if (r == 1) { if (auto* l = tr->findLane (target)) l->visible = false; }
                        else if (r == 2)
                        {
                            tr->lanes.erase (std::remove_if (tr->lanes.begin(), tr->lanes.end(),
                                                             [&] (const AutoLane& l) { return l.target == target; }), tr->lanes.end());
                        }
                        else if (r == 3) { showAutomationTargetMenu (*appPtr, tid, nullptr); return; }
                        else if (r == 4) { appPtr->moveAutomationToTrack (tid, target); return; }
                        tr->showAutomation = tr->hasVisibleLanes();
                        appPtr->project.sendStructureChanged();
                        appPtr->project.sendContentChanged();
                    });
                    return;
                }
            }
            // dragging anywhere else on the header body moves the track up / down the list
            if (! master) reorderReady = true;
        }
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (! isMasterRow() && e.y < 30) nameLabel.showEditor();
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            owner.zoomTrackHeights (ArrangeView::notchesOf (w), isMasterRow() ? -1 : e.y + getY());
        else
            owner.laneViewport.mouseWheelMove (e.getEventRelativeTo (&owner.laneViewport), w);
    }

private:
    // the master bus row's own menu: automation and showing / hiding the row itself
    void showMasterMenu()
    {
        PopupMenu m;
        m.addItem (1, TRANS ("Add Automation..."));
        m.addItem (2, TRANS ("Master Automation:") + " " + String (app2string (track() != nullptr ? track()->autoMode : AutoMode::Read)), false);
        m.addSeparator();
        m.addItem (3, TRANS ("Hide Master Bus Track"));
        auto* appPtr = &owner.app;
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (this), [appPtr] (int r)
        {
            if (r == 1) { showAutomationTargetMenu (*appPtr, 0, nullptr); return; }
            if (r == 3) { appPtr->showMasterTrack = false; appPtr->notifyView(); }
        });
    }

    static String app2string (AutoMode m)
    {
        const char* names[] = { "Off", "Read", "Touch" };
        return TRANS (names[jlimit (0, 2, (int) m)]);
    }

    ArrangeView& owner;
    int trackId;
    Track* bound = nullptr;      // the master bus row: a channel that is not in project.tracks
    bool updating = false;
    bool resizing = false;      // dragging the border under the track body
    bool reorderReady = false, reorderDragging = false;   // drag the header to move the track
    int resizeStartH = 0;
    Rectangle<int> dBTextArea;   // free space right of the switches, for the dB readout
    Label nameLabel;
    TextToggle muteBtn, soloBtn, armBtn, monBtn, freezeBtn;
    TextToggle monoBtn;   // master row only: mono / stereo switch for the main output
    TextToggle listenBtn { "L", theme::warn, TRANS ("Listen through the listen bus") };
    IconButton autoBtn { Icon::Automation, TRANS ("Show / Hide Automation") };
    Slider vol;
    LevelMeter meter { LevelMeter::Orientation::horizontal };
};

class ArrangeView::Headers : public Component
{
public:
    explicit Headers (ArrangeView& o) : owner (o) {}

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);
        // drop indicator while a track header is being dragged
        if (owner.reorderDropIndex >= 0)
        {
            const auto& tracks = owner.app.project.tracks;
            const int y = owner.reorderDropIndex >= (int) tracks.size()
                        ? owner.totalRowsHeight() : owner.rowTop (owner.reorderDropIndex);
            g.setColour (theme::accent);
            g.fillRect (0.0f, (float) y - 1.0f, (float) getWidth(), 2.0f);
        }
    }

    void rebuild()
    {
        auto& proj = owner.app.project;
        std::vector<std::unique_ptr<TrackHeader>> next;
        for (auto& t : proj.tracks)
        {
            std::unique_ptr<TrackHeader> h;
            for (auto& old : headersList)
                if (old != nullptr && old->getTrackId() == t->id) { h = std::move (old); break; }
            if (h == nullptr) { h = std::make_unique<TrackHeader> (owner, t->id); addAndMakeVisible (*h); }
            next.push_back (std::move (h));
        }
        headersList = std::move (next);
        layoutHeaders();
    }

    void layoutHeaders()
    {
        auto& proj = owner.app.project;
        for (size_t i = 0; i < headersList.size() && i < proj.tracks.size(); ++i)
            headersList[i]->setBounds (0, owner.rowTop ((int) i), getWidth(), owner.rowHeight (*proj.tracks[i]));
    }

    void refreshAll() { for (auto& h : headersList) h->refresh(); }
    void updateMeters() { for (auto& h : headersList) if (h != nullptr) h->updateMeter(); }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            owner.zoomTrackHeights (ArrangeView::notchesOf (w), e.y);
        else
            owner.laneViewport.mouseWheelMove (e.getEventRelativeTo (&owner.laneViewport), w);
    }

private:
    ArrangeView& owner;
    std::vector<std::unique_ptr<TrackHeader>> headersList;

public:
    /** The header of a track (self test / layout introspection). */
    juce::Component* headerForTrack (int trackId) const
    {
        for (auto& h : headersList)
            if (h != nullptr && h->getTrackId() == trackId) return h.get();
        return nullptr;
    }

    juce::Rectangle<int> meterBoundsForTrack (int trackId) const
    {
        for (auto& h : headersList)
            if (h != nullptr && h->getTrackId() == trackId) return h->meterBounds();
        return {};
    }

    bool meterVisibleForTrack (int trackId) const
    {
        for (auto& h : headersList)
            if (h != nullptr && h->getTrackId() == trackId) return h->meterVisible();
        return false;
    }
};

//==============================================================================
ArrangeView::ArrangeView (AppState& a) : app (a)
{
    ruler = std::make_unique<Ruler> (*this);
    tempoLane = std::make_unique<TempoLane> (*this);
    chordLane = std::make_unique<ChordLane> (*this);
    markerLane = std::make_unique<MarkerLane> (*this);
    masterLanes = std::make_unique<MasterAutoLane> (*this);
    // the master bus row header is the same component a track uses; it is bound to the master
    // channel instead of an id in project.tracks
    masterHeader = std::make_unique<TrackHeader> (*this, 0, &app.project.masterTrack);
    addChildComponent (*masterLanes);
    addChildComponent (*masterHeader);
    lanes = std::make_unique<Lanes> (*this);
    headers = std::make_unique<Headers> (*this);

    laneViewport.setViewedComponent (lanes.get(), false);
    laneViewport.setScrollBarsShown (true, true);
    laneViewport.setScrollBarThickness (12);
    headerViewport.setViewedComponent (headers.get(), false);
    headerViewport.setScrollBarsShown (false, false);
    // the header column follows every scroll of the lanes in the same frame (see syncScrollNow)
    laneViewport.onScroll = [this] { syncScrollNow(); };
    headerViewport.onScroll = [this]
    {
        if (! syncingScroll) laneViewport.setViewPosition (laneViewport.getViewPositionX(), headerViewport.getViewPositionY());
    };

    addAndMakeVisible (laneViewport);
    addAndMakeVisible (headerViewport);
    addAndMakeVisible (*ruler);
    addChildComponent (*tempoLane);
    addChildComponent (*chordLane);
    addChildComponent (*markerLane);
    addAndMakeVisible (addTrackBtn);
    addTrackBtn.setBackgroundVisible (true);
    addTrackBtn.onClick = [this]
    {
        PopupMenu m;
        m.addItem (1, TRANS ("New Software Instrument Track"));
        m.addItem (2, TRANS ("New Audio Track"));
        m.addItem (3, TRANS ("New Bus Track"));
        m.addItem (4, TRANS ("New Effect Send Track"));
        m.addItem (5, TRANS ("New Automation Track..."));
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (&addTrackBtn), [this] (int r)
        {
            if (r == 1) app.addTrack (TrackType::Instrument);
            else if (r == 2) app.addTrack (TrackType::Audio);
            else if (r == 3) app.addTrack (TrackType::Bus);
            else if (r == 4) app.addSendTrack();
            else if (r == 5)
            {
                // an automation track can control any channel: first pick the channel, then the
                // parameter (the same target menu the Auto button uses)
                auto targets = std::make_shared<std::vector<int>>();
                PopupMenu tm;
                targets->push_back (0);
                tm.addItem (1, TRANS ("Master"));
                int id = 2;
                for (auto& t : app.project.tracks)
                {
                    if (t == nullptr || t->isAutomation() || t->isListenBus) continue;
                    targets->push_back (t->id);
                    tm.addItem (id++, t->name);
                }
                auto* appPtr = &app;
                tm.showMenuAsync (PopupMenu::Options().withTargetComponent (&addTrackBtn), [appPtr, targets] (int r2)
                {
                    if (r2 <= 0 || r2 > (int) targets->size()) return;
                    showAutomationTargetMenu (*appPtr, (*targets)[(size_t) r2 - 1], nullptr, true);
                });
            }
        });
    };

    // the two switches for the rows above the tracks (both off by default)
    chordBtn.setClickingTogglesState (false);
    chordBtn.setBackgroundVisible (true);
    chordBtn.setToggleState (app.showChordTrack, dontSendNotification);
    chordBtn.onClick = [this]
    {
        app.showChordTrack = ! app.showChordTrack;
        app.notifyView();          // re-lays out the pinned rows and repaints the headers
    };
    addAndMakeVisible (chordBtn);

    markerBtn.setClickingTogglesState (false);
    markerBtn.setBackgroundVisible (true);
    markerBtn.setToggleState (app.showMarkerTrack, dontSendNotification);
    markerBtn.onClick = [this]
    {
        app.showMarkerTrack = ! app.showMarkerTrack;
        app.notifyView();
    };
    addAndMakeVisible (markerBtn);

    app.project.addListener (this);
    app.addListener (this);
    layoutRows();
    rebuildHeaders();
    startTimerHz (30);
}

ArrangeView::~ArrangeView()
{
    app.removeListener (this);
    app.project.removeListener (this);
    laneViewport.setViewedComponent (nullptr, false);
    headerViewport.setViewedComponent (nullptr, false);
}

int ArrangeView::scrollX() const { return laneViewport.getViewPositionX(); }

int ArrangeView::trackIndexAtY (int y) const
{
    auto& tracks = app.project.tracks;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (y >= rowTop ((int) i) && y < rowTop ((int) i) + rowHeight (*tracks[i])) return (int) i;
    return -1;
}

// the pinned master bus row: a track-sized row plus one sub-row per visible automation lane.
// It follows the same height ladder as the tracks, but never crowds them out: at least one track
// row (120 px) always stays for the scrolling tracks, and only whole lane rows are added, so the
// row is never left half clipped.
int ArrangeView::masterRowH() const
{
    if (! app.showMasterTrack) return 0;
    auto& mt = app.project.masterTrack;
    const int th = trackH (mt);
    const int ah = autoH (mt);
    const int full = th + mt.laneRowCount() * ah;
    const int avail = getHeight() - kRulerH - pinnedRowsH();
    if (avail <= 0) return full;
    const int cap = jmax (th, avail - jmax (120, th));   // room left for the tracks
    if (full <= cap) return full;
    const int lanesThatFit = jmax (0, (cap - th) / jmax (1, ah));
    return th + jmin (mt.laneRowCount(), lanesThatFit) * ah;
}

juce::Component* ArrangeView::masterLaneComponent() const { return masterLanes.get(); }

juce::Component* ArrangeView::masterHeaderComponent() const { return masterHeader.get(); }

juce::Component* ArrangeView::rulerComponent() const { return ruler.get(); }

juce::Component* ArrangeView::trackHeaderComponent (int trackId) const
{
    return headers != nullptr ? headers->headerForTrack (trackId) : nullptr;
}

int ArrangeView::trackRowHeight (int trackId) const
{
    if (auto* t = app.project.findTrack (trackId)) return rowHeight (*t);
    return 0;
}

juce::Component* ArrangeView::lanesComponent() const { return lanes.get(); }

juce::Rectangle<int> ArrangeView::trackRowBounds (int trackId) const
{
    const int idx = app.project.indexOfTrack (trackId);
    if (idx < 0) return {};
    auto& t = *app.project.tracks[(size_t) idx];
    return { 0, rowTop (idx), lanes != nullptr ? lanes->getWidth() : getWidth(), rowHeight (t) };
}

juce::Rectangle<int> ArrangeView::trackHeaderMeterBounds (int trackId) const
{
    return headers != nullptr ? headers->meterBoundsForTrack (trackId) : juce::Rectangle<int>();
}

bool ArrangeView::trackHeaderMeterVisible (int trackId) const
{
    return headers != nullptr && headers->meterVisibleForTrack (trackId);
}

juce::Component* ArrangeView::chordLaneComponent() const { return chordLane.get(); }

juce::Component* ArrangeView::markerLaneComponent() const { return markerLane.get(); }

juce::Rectangle<int> ArrangeView::chordLaneBounds() const
{
    if (chordLaneH() <= 0) return {};
    return { 0, chordLaneY(), getWidth(), chordLaneH() };
}

juce::Rectangle<int> ArrangeView::markerLaneBounds() const
{
    if (markerLaneH() <= 0) return {};
    return { 0, markerLaneY(), getWidth(), markerLaneH() };
}

juce::Rectangle<int> ArrangeView::trackAreaBounds() const { return laneViewport.getBounds(); }

juce::Rectangle<int> ArrangeView::masterRowBounds() const
{
    const int h = masterRowH();
    if (h <= 0) return {};
    return { 0, masterRowY(), getWidth(), h };
}

void ArrangeView::layoutMasterRow()
{
    const int h = masterRowH();
    laidOutMasterRowH = h;      // the height the arrangement currently reserves for the row
    const bool shown = h > 0;
    if (masterLanes != nullptr) masterLanes->setVisible (shown);
    if (masterHeader != nullptr) masterHeader->setVisible (shown);
    if (! shown) return;
    const int y = masterRowY();
    if (masterHeader != nullptr)
    {
        masterHeader->setBounds (0, y, kHeaderW, h);
        masterHeader->refresh();
    }
    if (masterLanes != nullptr) masterLanes->setBounds (kHeaderW, y, jmax (0, getWidth() - kHeaderW), h);
}

void ArrangeView::layoutRows()
{
    rowY.clear();
    int y = 0;
    for (auto& t : app.project.tracks)
    {
        rowY.push_back (y);
        y += rowHeight (*t);
    }
    rowY.push_back (y);
}

void ArrangeView::updateContentSize()
{
    auto& tempo = app.project.tempo;
    const int lastBar = tempomap::barPosition (tempo, jmax (app.project.endBeat(), app.project.loopEnd)).bar;
    const double beats = jmax (tempomap::beatOfBar (tempo, lastBar + 8), 32.0);
    const int w = jmax ((int) (beats * app.pxPerBeat), laneViewport.getMaximumVisibleWidth());
    const int h = jmax (totalRowsHeight() + 60, laneViewport.getMaximumVisibleHeight());
    if (lanes->getWidth() != w || lanes->getHeight() != h) lanes->setSize (w, h);
    // The lane viewport carries a horizontal scrollbar (and a vertical one when the tracks are
    // taller than the view), so its visible height is up to 12 px smaller than the header
    // viewport's. With both viewports at the same bounds and the same content height the LANE
    // could scroll 12 px further: scrolled to the bottom the headers stayed 12 px behind and the
    // lower tracks looked out of line with their clips. Give the headers that many extra pixels
    // of content so both scroll ranges end at the same place.
    const int laneScrollbarExtra = jmax (0, laneViewport.getHeight()
                                            - laneViewport.getMaximumVisibleHeight());
    const int headerContentH = h + laneScrollbarExtra;
    if (headers->getWidth() != kHeaderW || headers->getHeight() != headerContentH)
        headers->setSize (kHeaderW, headerContentH);
}

void ArrangeView::rebuildHeaders()
{
    layoutRows();
    updateContentSize();
    headers->rebuild();
    headers->refreshAll();
    headers->layoutHeaders();
    lanes->repaint();
}

void ArrangeView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (kRulerH);
    auto topLeft = top.removeFromLeft (kHeaderW);
    addTrackBtn.setBounds (topLeft.reduced (8, 7).removeFromLeft (24));
    // the chord / marker track switches sit at the right end of the track header strip
    auto switches = topLeft.reduced (8, 7);
    markerBtn.setBounds (switches.removeFromRight (24));
    switches.removeFromRight (2);
    chordBtn.setBounds (switches.removeFromRight (24));
    ruler->setBounds (top);

    tempoLane->setVisible (app.showTempoLane);
    if (app.showTempoLane)
        tempoLane->setBounds (r.removeFromTop (kTempoH).withTrimmedLeft (kHeaderW));

    // chord track + marker track: pinned rows just above the tracks, in the order
    // tempo -> chords -> markers -> master bus -> tracks
    chordLane->setVisible (app.showChordTrack);
    if (app.showChordTrack)
        chordLane->setBounds (r.removeFromTop (kChordH).withTrimmedLeft (kHeaderW));
    markerLane->setVisible (app.showMarkerTrack);
    if (app.showMarkerTrack)
        markerLane->setBounds (r.removeFromTop (kMarkerH).withTrimmedLeft (kHeaderW));

    // the master bus row is pinned above the track rows (Studio One style): it keeps the same
    // row height rules and lane sub-rows, but never scrolls away with the tracks
    r.removeFromTop (jmin (masterRowH(), r.getHeight()));
    headerViewport.setBounds (r.removeFromLeft (kHeaderW));
    laneViewport.setBounds (r);
    layoutMasterRow();
    updateContentSize();
    headers->layoutHeaders();
}

// drag & drop reorder: turn a screen y into the "drop before row N" slot
int ArrangeView::dropIndexForScreenY (int screenY) const
{
    const auto local = getLocalPoint (nullptr, Point<int> (0, screenY));
    const int lanesTop = kRulerH + pinnedRowsH() + masterRowH();
    const int contentY = local.y - lanesTop + laneViewport.getViewPositionY();
    const auto& tracks = app.project.tracks;
    int y = 0;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const int h = rowHeight (*tracks[i]);
        if (contentY < y + h / 2) return (int) i;
        y += h;
    }
    return (int) tracks.size();
}

void ArrangeView::showReorderDrop (int index)
{
    if (reorderDropIndex == index) return;
    reorderDropIndex = index;
    headers->repaint();
    lanes->repaint();
}

void ArrangeView::repaintClipArea()
{
    lanes->repaint();
    headers->repaint();
}

void ArrangeView::endTrackReorder (bool commit)
{
    const int drop = reorderDropIndex;
    const int id = reorderTrackId;
    reorderTrackId = 0;
    reorderDropIndex = -1;
    headers->repaint();
    lanes->repaint();
    if (commit && id != 0 && drop >= 0)
        app.moveTrackTo (id, drop);
}

void ArrangeView::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::line);
    g.fillRect (0, kRulerH - 1, kHeaderW, 1);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true));
    g.drawText (TRANS ("TRACKS"), 40, 0, 100, kRulerH - 1, Justification::centredLeft);

    if (app.showTempoLane)
    {
        // tempo track header: title + the (interpolated) tempo at the playhead
        auto h = Rectangle<int> (0, kRulerH, kHeaderW, kTempoH);
        g.setColour (theme::panelAlt);
        g.fillRect (h);
        g.setColour (theme::accent);
        g.fillRect (h.getX(), h.getY() + 8, 3, h.getHeight() - 16);
        g.setColour (theme::text);
        g.setFont (theme::font (12.5f, true));
        g.drawText (TRANS ("Tempo"), h.getX() + 14, h.getY() + 10, 120, 18, Justification::centredLeft);
        const double now = app.project.bpmAt (app.engine.getPositionBeats());
        g.setColour (theme::accentDark);
        g.setFont (theme::font (18.0f, true));
        g.drawText (String (now, 1), h.getX() + 14, h.getY() + 30, 90, 24, Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        g.drawText ("BPM", h.getX() + 84, h.getY() + 34, 40, 18, Justification::centredLeft);
        g.setColour (theme::line);
        g.fillRect (h.getX(), h.getBottom() - 1, h.getWidth(), 1);
        g.fillRect (h.getRight() - 1, h.getY(), 1, h.getHeight());
    }

    if (app.showChordTrack)
    {
        // chord track header: the chord that is in effect at the playhead
        auto h = Rectangle<int> (0, chordLaneY(), kHeaderW, kChordH);
        g.setColour (theme::panelAlt);
        g.fillRect (h);
        g.setColour (theme::accent);
        g.fillRect (h.getX(), h.getY() + 7, 3, h.getHeight() - 14);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f, true));
        g.drawText (TRANS ("CHORD TRACK"), h.getX() + 14, h.getY() + 4, 150, 12, Justification::centredLeft);

        const double now = app.engine.getPositionBeats();
        const auto* authored = app.project.chordAt (now);
        if (authored != nullptr)
        {
            g.setColour (theme::accentDark);
            g.setFont (theme::font (17.0f, true));
            g.drawFittedText (authored->symbol, h.getX() + 14, h.getY() + 17, 150, 22,
                              Justification::centredLeft, 1);
        }
        else
        {
            // no chord written here: offer what the notes currently play (probed per 1/16 note)
            const double probe = std::floor (now * 4.0) / 4.0;
            if (probe != probedChordBeat)
            {
                probedChordBeat = probe;
                const auto found = detectChordAt (app.project, probe, 1.0);
                probedChordName = found.valid ? found.name : juce::String();
            }
            g.setColour (probedChordName.isNotEmpty() ? theme::textDim : theme::textFaint);
            g.setFont (theme::font (15.0f, probedChordName.isNotEmpty()));
            g.drawFittedText (probedChordName.isNotEmpty() ? probedChordName : String (TRANS ("no chord")),
                              h.getX() + 14, h.getY() + 17, 150, 22, Justification::centredLeft, 1);
        }
        g.setColour (theme::line);
        g.fillRect (h.getX(), h.getBottom() - 1, h.getWidth(), 1);
        g.fillRect (h.getRight() - 1, h.getY(), 1, h.getHeight());
    }

    if (app.showMarkerTrack)
    {
        // marker track header: the section the playhead is in
        auto h = Rectangle<int> (0, markerLaneY(), kHeaderW, kMarkerH);
        g.setColour (theme::laneA);
        g.fillRect (h);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f, true));
        g.drawText (TRANS ("MARKERS"), h.getX() + 12, h.getY() + 3, 150, 11, Justification::centredLeft);
        const auto* section = app.project.sectionAt (app.engine.getPositionBeats());
        g.setColour (section != nullptr ? section->colour.darker (0.35f) : theme::textFaint);
        g.setFont (theme::font (12.0f, true));
        g.drawFittedText (section != nullptr ? section->name : String (TRANS ("-")),
                          h.getX() + 12, h.getY() + 14, 150, 14, Justification::centredLeft, 1);
        g.setColour (theme::line);
        g.fillRect (h.getX(), h.getBottom() - 1, h.getWidth(), 1);
        g.fillRect (h.getRight() - 1, h.getY(), 1, h.getHeight());
    }
}

void ArrangeView::projectContentChanged()
{
    resized();                  // re-reserves the pinned master row and re-lays out the headers
    updateContentSize();
    lanes->repaint();
    ruler->repaint();
    tempoLane->repaint();
    probedChordBeat = -1.0;     // the notes changed: the detected chord has to be probed again
    if (chordLane != nullptr) chordLane->repaint();
    if (markerLane != nullptr) markerLane->repaint();
    if (masterLanes != nullptr) masterLanes->repaint();
    repaint (0, kRulerH, kHeaderW, pinnedRowsH());
    headers->refreshAll();
}
void ArrangeView::projectMixChanged() { headers->refreshAll(); }
void ArrangeView::projectStructureChanged()
{
    rebuildHeaders();
    resized();                  // a new / hidden master lane changes the space the pinned row needs
    ruler->repaint();
    tempoLane->repaint();
    if (chordLane != nullptr) chordLane->repaint();
    if (markerLane != nullptr) markerLane->repaint();
}
void ArrangeView::selectionChanged() { lanes->repaint(); headers->refreshAll(); for (auto* c : headers->getChildren()) c->repaint(); }
void ArrangeView::viewChanged()
{
    resized();
    repaint();
    layoutRows();
    layoutMasterRow();
    updateContentSize();
    headers->layoutHeaders();
    lanes->repaint();
    ruler->repaint();
    tempoLane->repaint();
    if (chordLane != nullptr) chordLane->repaint();
    if (markerLane != nullptr) markerLane->repaint();
    if (masterLanes != nullptr) masterLanes->repaint();
    headers->refreshAll();
    chordBtn.setToggleState (app.showChordTrack, dontSendNotification);
    markerBtn.setToggleState (app.showMarkerTrack, dontSendNotification);
}

void ArrangeView::syncScrollNow()
{
    if (syncingScroll) return;
    const juce::ScopedValueSetter<bool> guard (syncingScroll, true);

    const auto pos = laneViewport.getViewPosition();
    if (headerViewport.getViewPositionY() != pos.y)
        headerViewport.setViewPosition (0, pos.y);

    if (pos != lastSyncedScroll)
    {
        lastSyncedScroll = pos;
        // everything that draws the timeline itself has to follow the scroll in the same frame
        ruler->repaint();
        tempoLane->repaint();
        if (chordLane != nullptr) chordLane->repaint();
        if (markerLane != nullptr) markerLane->repaint();
        if (masterLanes != nullptr) masterLanes->repaint();
        headers->refreshAll();
        repaint();
    }
}

void ArrangeView::timerCallback()
{
    // fallback for scrolls that do not come from the viewport (and for the transport / playhead)
    syncScrollNow();

    const double beats = app.engine.getPositionBeats();
    const juce::int64 x = (juce::int64) std::llround (beats * app.pxPerBeat);
    // publish the position for the paint BEFORE invalidating: the lanes draw exactly this x,
    // which is the centre of the columns repainted below
    playXForPaint = x;
    if (x != lastPlayX)
    {
        const auto pos = laneViewport.getViewPosition();
        auto old = lastPlayX;
        lastPlayX = x;
        lanes->repaint ((int) old - 3, 0, 8, lanes->getHeight());
        lanes->repaint ((int) x - 3, 0, 8, lanes->getHeight());
        ruler->repaint();
        if (masterLanes != nullptr) masterLanes->repaint();
        if (app.showTempoLane)
        {
            tempoLane->repaint();
            repaint (0, kRulerH, kHeaderW, kTempoH);   // live bpm readout during ramps
        }
        if (app.showChordTrack)
        {
            chordLane->repaint();
            repaint (0, chordLaneY(), kHeaderW, kChordH);     // live chord readout at the playhead
        }
        if (app.showMarkerTrack)
        {
            markerLane->repaint();
            repaint (0, markerLaneY(), kHeaderW, kMarkerH);
        }

        if (app.engine.isPlaying() && app.followPlayhead)
        {
            const int vw = laneViewport.getMaximumVisibleWidth();
            if (x < pos.x || x > pos.x + vw - 30)
                laneViewport.setViewPosition (jmax (0, (int) x - vw / 8), pos.y);
        }
    }

    if (headers != nullptr) headers->updateMeters();
    if (masterHeader != nullptr && masterHeader->isVisible()) masterHeader->updateMeter();
}

//==============================================================================
void ArrangeView::filesDropped (const StringArray& files, int x, int y)
{
    const double beat = jmax (0.0, ((double) (x - kHeaderW) + scrollX()) / app.pxPerBeat);
    const int idx = trackIndexAtY (y - kRulerH - pinnedRowsH() + laneViewport.getViewPositionY());
    const int tid = idx >= 0 ? app.project.tracks[(size_t) idx]->id : -1;

    // MIDI files and audio files may be mixed, and the whole drop is a single undo step: dropping
    // a folder of stems is one edit, so one Ctrl+Z takes them all back again
    Array<File> dropped;
    dropped.ensureStorageAllocated (files.size());
    for (auto& name : files) dropped.add (File (name));
    importFiles (app, dropped, beat, tid, midiImportSyncsTempo (app));
}

void ArrangeView::zoomBy (double factor)
{
    const double centre = ((double) scrollX() + laneViewport.getMaximumVisibleWidth() * 0.5) / app.pxPerBeat;
    app.setZoom (app.pxPerBeat * factor);
    laneViewport.setViewPosition (jmax (0, (int) (centre * app.pxPerBeat - laneViewport.getMaximumVisibleWidth() * 0.5)),
                                  laneViewport.getViewPositionY());
}

void ArrangeView::zoomToFit()
{
    const double end = jmax (tempomap::beatOfBar (app.project.tempo, 4), app.project.endBeat());
    app.setZoom ((double) laneViewport.getMaximumVisibleWidth() / (end + 1.0));
    laneViewport.setViewPosition (0, laneViewport.getViewPositionY());
}

//==============================================================================
// chord track / marker track entry: both open their row first, so the result is visible
void ArrangeView::enterChordAtPlayhead()
{
    if (! app.showChordTrack)
    {
        app.showChordTrack = true;
        app.notifyView();
    }
    if (chordLane != nullptr) chordLane->editChord (0, app.engine.getPositionBeats());
}

void ArrangeView::addSectionAtPlayhead()
{
    if (! app.showMarkerTrack)
    {
        app.showMarkerTrack = true;
        app.notifyView();
    }
    if (markerLane == nullptr) return;
    const int id = markerLane->addSectionAt (app.engine.getPositionBeats(), {});
    markerLane->editSectionName (id);
}

float ArrangeView::notchesOf (const MouseWheelDetails& w)
{
    // JUCE reports one mouse click as 120/512 of wheel travel; trackpads send fractions of that
    constexpr float kNotch = 120.0f / 512.0f;
    return w.deltaY / kNotch;
}

int ArrangeView::maxTrackHeight() const
{
    // half the screen: one track may fill half of the display height
    const auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay();
    return d != nullptr ? jmax (trackHeightSteps().front(), d->userArea.getHeight() / 2) : 400;
}

void ArrangeView::zoomTrackHeights (float wheelSteps, int anchorY)
{
    // one notch is one step; trackpads send many small deltas, which add up before stepping
    if (wheelSteps == 0.0f) return;
    if ((wheelSteps > 0.0f) != (trackZoomAccum > 0.0f)) trackZoomAccum = 0.0f;
    trackZoomAccum += wheelSteps;
    const int notches = (int) std::floor (std::abs (trackZoomAccum) + 1.0e-3f);
    if (notches == 0) return;
    const int dir = trackZoomAccum > 0.0f ? 1 : -1;
    trackZoomAccum -= (float) (dir * notches);

    int h = app.trackHeight;
    for (int i = 0; i < notches; ++i)
        h = stepTrackHeight (h, dir, maxTrackHeight());
    setTrackHeight (h, anchorY);
}

void ArrangeView::setTrackHeight (int newH, int anchorY)
{
    newH = jlimit (23, maxTrackHeight(), newH);
    if (newH == app.trackHeight) return;

    // tracks with their own height keep their proportion when the session height changes
    const double ratio = (double) newH / (double) jmax (1, app.trackHeight);
    app.trackHeight = newH;
    for (auto& t : app.project.tracks)
        if (t->height > 0)
            t->height = jlimit (23, maxTrackHeight(), (int) std::round (t->height * ratio));
    // the master bus row is part of the same height ladder
    if (app.project.masterTrack.height > 0)
        app.project.masterTrack.height = jlimit (23, maxTrackHeight(), (int) std::round (app.project.masterTrack.height * ratio));

    relayoutTrackHeights (anchorY);
}

void ArrangeView::setDefaultTrackHeight (int newH)
{
    setTrackHeight (newH, -1);
}

void ArrangeView::setAllTrackHeights (int newH)
{
    newH = jlimit (23, maxTrackHeight(), newH);
    app.trackHeight = newH;
    for (auto& t : app.project.tracks) t->height = 0;   // everyone follows the new default
    app.project.masterTrack.height = 0;
    relayoutTrackHeights();
}

void ArrangeView::setMasterRowHeight (int newH)
{
    newH = jlimit (23, maxTrackHeight(), newH);
    if (app.project.masterTrack.height == newH) return;
    app.project.masterTrack.height = newH;
    relayoutTrackHeights();
}

void ArrangeView::setTrackRowHeight (int trackId, int newH)
{
    auto* t = app.project.findTrack (trackId);
    if (t == nullptr) return;
    newH = jlimit (23, maxTrackHeight(), newH);
    if (t->height == newH) return;
    t->height = newH;
    relayoutTrackHeights();
}

void ArrangeView::relayoutTrackHeights (int anchorY)
{
    // keep the row under the anchor pinned to the same screen position while scaling
    int anchorIdx = -1;
    int anchorTopScreen = 0;
    if (anchorY >= 0)
    {
        anchorIdx = trackIndexAtY (anchorY);
        if (anchorIdx >= 0) anchorTopScreen = rowTop (anchorIdx) - laneViewport.getViewPositionY();
    }

    // the pinned master row follows the track height, so the space reserved above the viewports
    // changes too: resized() has to run, otherwise the master row would overlap the track rows
    const int masterDelta = masterRowH() - laidOutMasterRowH;

    layoutRows();
    resized();

    if (anchorIdx >= 0)
    {
        // compensate for the pinned row growing / shrinking so the anchored row stays under the
        // cursor on screen, not just at the same offset inside the viewport
        const int newY = rowTop (anchorIdx) - anchorTopScreen + masterDelta;
        laneViewport.setViewPosition (laneViewport.getViewPositionX(), jmax (0, newY));
    }
    lanes->repaint();
    headers->refreshAll();
    ruler->repaint();
    if (masterLanes != nullptr) masterLanes->repaint();
}

void ArrangeView::zoomPxPerBeat (float wheelSteps, int anchorX)
{
    // one notch is one step; trackpads send many small deltas, which add up before stepping
    if (wheelSteps == 0.0f) return;
    if ((wheelSteps > 0.0f) != (zoomAccum > 0.0f)) zoomAccum = 0.0f;
    zoomAccum += wheelSteps;
    const int notches = (int) std::floor (std::abs (zoomAccum) + 1.0e-3f);
    if (notches == 0) return;
    const int dir = zoomAccum > 0.0f ? 1 : -1;
    zoomAccum -= (float) (dir * notches);

    double ppb = app.pxPerBeat;
    for (int i = 0; i < notches; ++i)
        ppb = stepPxPerBeat (ppb, dir);
    setPxPerBeat (ppb, anchorX);
}

void ArrangeView::setPxPerBeat (double newPpb, int anchorX)
{
    newPpb = jlimit (3.0, 480.0, newPpb);
    if (std::abs (newPpb - app.pxPerBeat) < 1.0e-9) return;

    // keep the beat under the anchor pinned to the same screen position while scaling
    const int sx = laneViewport.getViewPositionX();
    const int ax = anchorX >= 0 ? anchorX : sx + laneViewport.getMaximumVisibleWidth() / 2;
    const double beatAtAnchor = (double) ax / app.pxPerBeat;
    const int screenX = ax - sx;

    app.setZoom (newPpb);   // notifies the view, which refreshes the content width
    laneViewport.setViewPosition (jmax (0, (int) std::round (beatAtAnchor * app.pxPerBeat) - screenX),
                                  laneViewport.getViewPositionY());
    ruler->repaint();
}

//==============================================================================
namespace
{
    template <typename F>
    void forSelected (AppState& app, F&& f)
    {
        for (auto& t : app.project.tracks)
        {
            for (auto& c : t->midiClips) if (app.selectedClips.count (c.id)) f (*t, &c, nullptr);
            for (auto& c : t->audioClips) if (app.selectedClips.count (c.id)) f (*t, nullptr, &c);
        }
    }
}

void ArrangeView::selectAll()
{
    app.selectedClips.clear();
    for (auto& t : app.project.tracks)
    {
        for (auto& c : t->midiClips) app.selectedClips.insert (c.id);
        for (auto& c : t->audioClips) app.selectedClips.insert (c.id);
    }
    app.notifySelection();
}

void ArrangeView::deleteSelected()
{
    if (app.selectedClips.empty()) return;
    auto& proj = app.project;
    proj.checkpoint();
    for (auto& t : proj.tracks)
    {
        auto& m = t->midiClips;
        m.erase (std::remove_if (m.begin(), m.end(), [&] (const MidiClip& c) { return app.selectedClips.count (c.id) > 0; }), m.end());
        auto& a = t->audioClips;
        a.erase (std::remove_if (a.begin(), a.end(), [&] (const AudioClip& c) { return app.selectedClips.count (c.id) > 0; }), a.end());
    }
    if (app.editClip != 0 && app.selectedClips.count (app.editClip)) { app.editClip = -1; app.editTrack = -1; }
    app.selectedClips.clear();
    proj.sendContentChanged();
    app.notifySelection();
}

void ArrangeView::setLoopToSelection()
{
    double lo = 1.0e12, hi = 0.0;
    forSelected (app, [&] (Track&, MidiClip* m, AudioClip* a)
    {
        if (m != nullptr) { lo = jmin (lo, m->start); hi = jmax (hi, m->start + m->length); }
        if (a != nullptr) { lo = jmin (lo, a->start); hi = jmax (hi, a->start + app.project.clipBeats (a->start, a->lengthSec)); }
    });
    if (hi <= lo) return;
    app.project.loopStart = lo;
    app.project.loopEnd = hi;
    app.project.loopOn = true;
    app.project.sendContentChanged();
}

void ArrangeView::mergeSelectedClips()
{
    if (app.selectedClips.size() < 2) return;
    auto& proj = app.project;

    std::map<int, std::vector<MidiClip*>> midiByTrack;
    std::map<int, std::vector<AudioClip*>> audioByTrack;
    for (auto& t : proj.tracks)
    {
        for (auto& c : t->midiClips) if (app.selectedClips.count (c.id) > 0) midiByTrack[t->id].push_back (&c);
        for (auto& c : t->audioClips) if (app.selectedClips.count (c.id) > 0) audioByTrack[t->id].push_back (&c);
    }

    bool any = false;
    for (auto& [tid, clips] : midiByTrack)
        if (clips.size() >= 2) { mergeMidiClips (tid, clips); any = true; }
    for (auto& [tid, clips] : audioByTrack)
        if (clips.size() >= 2) { mergeAudioClips (tid, clips); any = true; }

    if (any)
    {
        proj.sendContentChanged();
        app.notifySelection();
    }
}

void ArrangeView::mergeMidiClips (int trackId, const std::vector<MidiClip*>& clips)
{
    auto& proj = app.project;
    auto* t = proj.findTrack (trackId);
    if (t == nullptr || clips.size() < 2) return;

    double lo = 1.0e9, hi = 0.0;
    for (auto* c : clips) { lo = jmin (lo, c->start); hi = jmax (hi, c->start + c->length); }

    MidiClip merged;
    merged.id = proj.newId();
    merged.name = clips.front()->name;
    merged.start = lo;
    merged.length = hi - lo;
    for (auto* c : clips)
    {
        const double off = c->start - lo;
        for (auto n : c->notes) { n.id = proj.newId(); n.start += off; merged.notes.push_back (n); }
        for (auto p : c->ccs) { p.id = proj.newId(); p.beat += off; merged.ccs.push_back (p); }
    }
    std::sort (merged.notes.begin(), merged.notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    std::sort (merged.ccs.begin(), merged.ccs.end(), [] (const CCPoint& a, const CCPoint& b) { return a.beat < b.beat; });

    std::set<int> ids;
    for (auto* c : clips) ids.insert (c->id);
    proj.checkpoint();
    if (app.editTrack == trackId && ids.count (app.editClip) > 0) { app.editTrack = -1; app.editClip = -1; }
    for (int id : ids) app.selectedClips.erase (id);
    t->midiClips.erase (std::remove_if (t->midiClips.begin(), t->midiClips.end(),
                                        [&] (const MidiClip& c) { return ids.count (c.id) > 0; }), t->midiClips.end());
    t->midiClips.push_back (std::move (merged));
    app.selectedClips.insert (t->midiClips.back().id);
}

void ArrangeView::mergeAudioClips (int trackId, const std::vector<AudioClip*>& clips)
{
    auto& proj = app.project;
    auto* t = proj.findTrack (trackId);
    if (t == nullptr || clips.size() < 2) return;

    double lo = 1.0e9, hi = 0.0;
    for (auto* c : clips) { lo = jmin (lo, c->start); hi = jmax (hi, c->start + proj.clipBeats (c->start, c->lengthSec)); }

    const File folder = File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("SoundX").getChildFile ("Bounces");
    folder.createDirectory();
    const File out = folder.getNonexistentChildFile ("merged", ".wav");

    String err;
    if (! app.engine.renderTrackRangeToFile (trackId, lo, hi, out, err))
    {
        showError (TRANS ("Could not merge clips: ") + err);
        return;
    }

    AudioClip merged;
    merged.id = proj.newId();
    merged.name = clips.front()->name;
    merged.file = out;
    merged.start = lo;
    merged.lengthSec = proj.beatsToSeconds (hi) - proj.beatsToSeconds (lo);
    merged.offsetSec = 0.0;
    merged.gainDb = 0.0f;
    merged.fadeIn = merged.fadeOut = 0.0;
    if (proj.formats != nullptr) merged.data = proj.pool.load (out, *proj.formats);

    std::set<int> ids;
    for (auto* c : clips) ids.insert (c->id);
    proj.checkpoint();
    if (app.editTrack == trackId && ids.count (app.editClip) > 0) { app.editTrack = -1; app.editClip = -1; }
    for (int id : ids) app.selectedClips.erase (id);
    t->audioClips.erase (std::remove_if (t->audioClips.begin(), t->audioClips.end(),
                                         [&] (const AudioClip& c) { return ids.count (c.id) > 0; }), t->audioClips.end());
    t->audioClips.push_back (std::move (merged));
    app.selectedClips.insert (t->audioClips.back().id);
}

void ArrangeView::duplicateSelected()
{
    if (app.selectedClips.empty()) return;
    auto& proj = app.project;
    double lo = 1.0e12, hi = 0.0;
    forSelected (app, [&] (Track&, MidiClip* m, AudioClip* a)
    {
        if (m != nullptr) { lo = jmin (lo, m->start); hi = jmax (hi, m->start + m->length); }
        if (a != nullptr) { lo = jmin (lo, a->start); hi = jmax (hi, a->start + proj.clipBeats (a->start, a->lengthSec)); }
    });
    const double shift = hi - lo;
    proj.checkpoint();
    std::set<int> newSel;
    for (auto& t : proj.tracks)
    {
        std::vector<MidiClip> addM;
        std::vector<AudioClip> addA;
        for (auto& c : t->midiClips)
            if (app.selectedClips.count (c.id))
            {
                auto copy = c; copy.id = proj.newId(); copy.start += shift;
                for (auto& n : copy.notes) n.id = proj.newId();
                for (auto& cc : copy.ccs) cc.id = proj.newId();
                newSel.insert (copy.id); addM.push_back (copy);
            }
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id)) { auto copy = c; copy.id = proj.newId(); copy.start += shift; newSel.insert (copy.id); addA.push_back (copy); }
        for (auto& c : addM) t->midiClips.push_back (c);
        for (auto& c : addA) t->audioClips.push_back (c);
    }
    app.selectedClips = newSel;
    proj.sendContentChanged();
    app.notifySelection();
}

void ArrangeView::copySelected()
{
    clipboard.clear();
    forSelected (app, [&] (Track& t, MidiClip* m, AudioClip* a)
    {
        ClipCopy c;
        c.trackId = t.id;
        if (m != nullptr) { c.audio = false; c.midi = *m; }
        else { c.audio = true; c.audioClip = *a; }
        clipboard.push_back (std::move (c));
    });
}

void ArrangeView::pasteAtPlayhead()
{
    if (clipboard.empty()) return;
    auto& proj = app.project;
    double lo = 1.0e12;
    for (auto& c : clipboard) lo = jmin (lo, c.audio ? c.audioClip.start : c.midi.start);
    const double at = app.engine.getPositionBeats();
    proj.checkpoint();
    std::set<int> newSel;
    for (auto& c : clipboard)
    {
        Track* t = proj.findTrack (c.trackId);
        const TrackType need = c.audio ? TrackType::Audio : TrackType::Instrument;
        if (t == nullptr || t->type != need)
        {
            t = app.track();
            if (t == nullptr || t->type != need) continue;
        }
        if (c.audio)
        {
            auto copy = c.audioClip; copy.id = proj.newId(); copy.start = at + (copy.start - lo);
            newSel.insert (copy.id); t->audioClips.push_back (copy);
        }
        else
        {
            auto copy = c.midi; copy.id = proj.newId(); copy.start = at + (copy.start - lo);
            for (auto& n : copy.notes) n.id = proj.newId();
            for (auto& cc : copy.ccs) cc.id = proj.newId();
            newSel.insert (copy.id); t->midiClips.push_back (copy);
        }
    }
    app.selectedClips = newSel;
    proj.sendContentChanged();
    app.notifySelection();
}

void ArrangeView::splitAtPlayhead() { splitAt (app.engine.getPositionBeats()); }

void ArrangeView::splitAtCursor()
{
    // Studio One 'S': cut where the mouse last was in the lanes; falls back to the playhead
    if (cursorBeat < 0.0) { splitAtPlayhead(); return; }
    splitAt (cursorBeat);
}

void ArrangeView::splitAt (double cut)
{
    auto& proj = app.project;

    bool any = false;
    for (auto& t : proj.tracks)
    {
        const bool trackSel = app.selectedTracks.count (t->id) > 0 && app.selectedClips.empty();
        for (auto& c : t->midiClips)
            if ((app.selectedClips.count (c.id) || trackSel) && cut > c.start + 0.01 && cut < c.start + c.length - 0.01) any = true;
        for (auto& c : t->audioClips)
            if ((app.selectedClips.count (c.id) || trackSel) && cut > c.start + 0.01 && cut < c.start + proj.clipBeats (c.start, c.lengthSec) - 0.01) any = true;
    }
    if (! any) return;

    proj.checkpoint();
    std::set<int> newSel;
    for (auto& t : proj.tracks)
    {
        const bool trackSel = app.selectedTracks.count (t->id) > 0 && app.selectedClips.empty();
        std::vector<MidiClip> addM;
        std::vector<AudioClip> addA;
        for (auto& c : t->midiClips)
        {
            if (! (app.selectedClips.count (c.id) || trackSel) || ! (cut > c.start + 0.01 && cut < c.start + c.length - 0.01)) continue;
            const double rel = cut - c.start;
            MidiClip right;
            right.id = proj.newId();
            right.name = c.name;
            right.start = cut;
            right.length = c.length - rel;
            std::vector<Note> leftNotes;
            for (auto n : c.notes)
            {
                const double end = n.start + n.length;
                if (n.start < rel)
                {
                    Note l = n; l.length = jmin (n.length, rel - n.start);
                    leftNotes.push_back (l);
                    if (end > rel + 1.0e-6)
                    {
                        Note r2 = n; r2.id = proj.newId(); r2.start = 0.0; r2.length = end - rel;
                        right.notes.push_back (r2);
                    }
                }
                else { n.start -= rel; right.notes.push_back (n); }
            }
            std::vector<CCPoint> leftCC;
            for (auto cc : c.ccs)
            {
                if (cc.beat < rel) leftCC.push_back (cc);
                else { cc.beat -= rel; right.ccs.push_back (cc); }
            }
            c.notes = leftNotes;
            c.ccs = leftCC;
            c.length = rel;
            newSel.insert (c.id);
            newSel.insert (right.id);
            addM.push_back (std::move (right));
        }
        for (auto& c : t->audioClips)
        {
            const double lenBeats = proj.clipBeats (c.start, c.lengthSec);
            if (! (app.selectedClips.count (c.id) || trackSel) || ! (cut > c.start + 0.01 && cut < c.start + lenBeats - 0.01)) continue;
            const double cutSec = proj.beatsToSeconds (cut) - proj.beatsToSeconds (c.start);
            const double relBeat = cut - c.start;          // clip-relative position of the cut
            AudioClip right = c;
            right.id = proj.newId();
            right.start = cut;
            right.offsetSec = c.offsetSec + cutSec * c.sourceRate();
            right.lengthSec = c.lengthSec - cutSec;
            c.lengthSec = cutSec;

            // gain envelope: split at the cut and re-anchor both halves so the drawn line
            // (and the rendered gain) stays continuous instead of jumping at the seam
            if (! c.gainEnv.empty())
            {
                std::vector<ClipGainPoint> leftPts, rightPts;
                splitClipGainEnv (c.gainEnv, relBeat, c.gainDb, leftPts, rightPts);
                c.gainEnv = std::move (leftPts);
                right.gainEnv = std::move (rightPts);
            }
            else right.gainEnv.clear();

            // fades: only the real ends of the clip keep a fade - the cut itself never gets one
            c.fadeIn      = jlimit (0.0, 0.9 * c.lengthSec, c.fadeIn);
            right.fadeOut = jlimit (0.0, 0.9 * right.lengthSec, right.fadeOut);
            c.fadeOut = 0.0;
            right.fadeIn = 0.0;

            // the two halves touch, so the left one starts no crossfade with whatever
            // follows; the right half keeps the original marker for its own neighbour
            c.xfadeCurve = 0;

            newSel.insert (c.id);
            newSel.insert (right.id);
            addA.push_back (std::move (right));
        }
        for (auto& c : addM) t->midiClips.push_back (std::move (c));
        for (auto& c : addA) t->audioClips.push_back (std::move (c));
    }
    app.selectedClips = newSel;
    proj.sendContentChanged();
    app.notifySelection();
}

void ArrangeView::addCrossfadeSelected()
{
    auto& proj = app.project;
    constexpr double kFadeSec = 0.25;   // default crossfade length, capped by clip length and source

    auto pairApplies = [&] (AudioClip& a, AudioClip& b)
    {
        if (a.data == nullptr || b.data == nullptr || ! (b.start > a.start)) return false;
        const double aEndSec = proj.beatsToSeconds (a.start) + a.lengthSec;
        const double bStartSec = proj.beatsToSeconds (b.start);
        if (aEndSec - bStartSec > 0.005) return true;      // already overlapping
        const double ov = jmin (kFadeSec, 0.45 * a.lengthSec, 0.45 * b.lengthSec);
        return bStartSec - aEndSec <= ov;                  // close enough to bridge
    };

    bool any = false;
    for (auto& t : proj.tracks)
    {
        std::vector<AudioClip*> sel;
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id)) sel.push_back (&c);
        if (sel.size() < 2) continue;
        std::sort (sel.begin(), sel.end(), [] (const AudioClip* a, const AudioClip* b) { return a->start < b->start; });
        for (size_t i = 0; i + 1 < sel.size(); ++i)
            if (pairApplies (*sel[i], *sel[i + 1])) { any = true; break; }
    }
    if (! any) return;

    proj.checkpoint();
    for (auto& t : proj.tracks)
    {
        std::vector<AudioClip*> sel;
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id)) sel.push_back (&c);
        if (sel.size() < 2) continue;
        std::sort (sel.begin(), sel.end(), [] (const AudioClip* a, const AudioClip* b) { return a->start < b->start; });
        for (size_t i = 0; i + 1 < sel.size(); ++i)
            proj.crossfade (*sel[i], *sel[i + 1], kFadeSec);
    }
    proj.sendContentChanged();
    updateContentSize();
    repaintClipArea();
}

void ArrangeView::fadesToPlayhead (bool fadeIn)
{
    auto& proj = app.project;
    const double cut = app.engine.getPositionBeats();

    struct Job { AudioClip* c; double sec; };
    std::vector<Job> jobs;
    for (auto& t : proj.tracks)
    {
        const bool trackSel = app.selectedTracks.count (t->id) > 0 && app.selectedClips.empty();
        for (auto& c : t->audioClips)
        {
            if (! (app.selectedClips.count (c.id) || trackSel)) continue;
            const double lenB = proj.clipBeats (c.start, c.lengthSec);
            if (cut <= c.start + 0.005 || cut >= c.start + lenB - 0.005) continue;
            const double fromStart = proj.beatsToSeconds (cut) - proj.beatsToSeconds (c.start);
            const double fromEnd = proj.beatsToSeconds (c.start + lenB) - proj.beatsToSeconds (cut);
            jobs.push_back ({ &c, fadeIn ? fromStart : fromEnd });
        }
    }
    if (jobs.empty()) return;

    proj.checkpoint();
    for (auto& j : jobs)
        if (fadeIn) j.c->fadeIn  = jlimit (0.0, 0.9 * j.c->lengthSec, j.sec);
        else        j.c->fadeOut = jlimit (0.0, 0.9 * j.c->lengthSec, j.sec);
    proj.sendContentChanged();
    repaintClipArea();
}

void ArrangeView::clearFadesSelected()
{
    auto& proj = app.project;
    bool any = false;
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id) && (c.fadeIn > 0.0 || c.fadeOut > 0.0)) any = true;
    if (! any) return;
    proj.checkpoint();
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id)) { c.fadeIn = 0.0; c.fadeOut = 0.0; }
    proj.sendContentChanged();
    repaintClipArea();
}

void ArrangeView::setFadesSelected (bool fadeIn, double sec)
{
    auto& proj = app.project;
    bool any = false;
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id))
            {
                const double want = jlimit (0.0, 0.9 * c.lengthSec, sec);
                if (std::abs ((fadeIn ? c.fadeIn : c.fadeOut) - want) > 1.0e-4) any = true;
            }
    if (! any) return;
    proj.checkpoint();
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id))
            {
                const double want = jlimit (0.0, 0.9 * c.lengthSec, sec);
                if (fadeIn) c.fadeIn = want; else c.fadeOut = want;
            }
    proj.sendContentChanged();
    repaintClipArea();
}

void ArrangeView::setClipTimebaseSelected (int clickedClipId, Timebase tb)
{
    auto& proj = app.project;
    // the right-clicked clip plus every selected one (like the fade presets); when the clicked clip
    // is not part of the selection, only that one is meant
    const bool wholeSelection = app.selectedClips.count (clickedClipId) > 0;
    auto applies = [&] (const AudioClip& c)
    {
        return wholeSelection ? app.selectedClips.count (c.id) > 0 : c.id == clickedClipId;
    };

    bool any = false;
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (applies (c) && c.timebase != tb) any = true;
    if (! any) return;

    proj.checkpoint();
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (applies (c)) c.timebase = tb;
    proj.sendContentChanged();
    repaintClipArea();
}

void ArrangeView::setCrossfadeSelected (double sec)
{
    auto& proj = app.project;
    auto pairs = selectedXfadePairs (proj, app.selectedClips);
    if (pairs.empty()) return;
    proj.checkpoint();
    for (auto& p : pairs) proj.resizeCrossfade (*p.first, *p.second, sec);
    proj.sendContentChanged();
    repaintClipArea();
}

void ArrangeView::setCrossfadeCurveSelected (int curve)
{
    auto& proj = app.project;
    auto pairs = selectedXfadePairs (proj, app.selectedClips);
    if (pairs.empty()) return;
    // the shape is stored on the earlier clip of every pair (both sides are set so a
    // new crossfade started from either clip keeps the same shape)
    std::set<int> ids;
    for (auto& p : pairs) { ids.insert (p.first->id); ids.insert (p.second->id); }
    for (int id : app.selectedClips) ids.insert (id);
    bool any = false;
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (ids.count (c.id) && c.xfadeCurve != curve) any = true;
    if (! any) return;
    proj.checkpoint();
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (ids.count (c.id)) c.xfadeCurve = jlimit (0, 2, curve);
    proj.sendContentChanged();
    repaintClipArea();
}

void ArrangeView::nudgeClipGainSelected (float db)
{
    auto& proj = app.project;
    bool any = false;
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id)) any = true;
    if (! any) return;
    proj.checkpoint();
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id)) c.gainDb = jlimit (-24.0f, 12.0f, c.gainDb + db);
    proj.sendContentChanged();
    repaintClipArea();
}

//==============================================================================
// audio editing: every command works on the selected audio clips and is non-destructive, so
// undo / redo and saving restore the exact parameters (the rendered audio is rebuilt on demand)
void ArrangeView::applyToSelectedAudio (const std::function<void (AudioClip&)>& fn)
{
    auto& proj = app.project;
    std::vector<AudioClip*> clips;
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id) > 0) clips.push_back (&c);
    if (clips.empty()) return;

    proj.checkpoint();
    MouseCursor::showWaitCursor();      // the time-stretch render can take a moment
    for (auto* c : clips)
    {
        fn (*c);
        invalidateClipRender (*c);
        ensureClipPlayback (*c);        // rebuild the stretched / transposed audio right away
    }
    MouseCursor::hideWaitCursor();
    proj.sendContentChanged();
    app.engine.syncAll();
    lanes->repaint();
    headers->repaint();
}

void ArrangeView::reverseSelectedAudio()
{
    applyToSelectedAudio ([] (AudioClip& c) { c.reversed = ! c.reversed; });
}

void ArrangeView::setSelectedAudioStretch (double factor)
{
    factor = jlimit (0.1, 10.0, factor);
    applyToSelectedAudio ([factor] (AudioClip& c)
    {
        if (std::abs (c.stretch - factor) < 1.0e-9) return;
        // the used source material stays the same, only how long it plays on the timeline changes
        const double sourceLen = c.sourceLengthSec();
        c.stretch = factor;
        c.lengthSec = jmax (0.01, sourceLen * factor);
    });
}

void ArrangeView::setSelectedAudioPitch (int semitones)
{
    semitones = jlimit (-24, 24, semitones);
    applyToSelectedAudio ([semitones] (AudioClip& c) { c.pitchSemitones = (float) semitones; });
}

void ArrangeView::nudgeSelectedAudioPitch (int delta)
{
    applyToSelectedAudio ([delta] (AudioClip& c)
    {
        c.pitchSemitones = (float) jlimit (-24, 24, (int) std::lround (c.pitchSemitones) + delta);
    });
}

void ArrangeView::resetSelectedAudioEdits()
{
    applyToSelectedAudio ([] (AudioClip& c)
    {
        const double sourceLen = c.sourceLengthSec();
        c.reversed = false;
        c.pitchSemitones = 0.0f;
        c.stretch = 1.0;
        c.lengthSec = jmax (0.01, sourceLen);
    });
}
void ArrangeView::resetClipGainSelected()
{
    auto& proj = app.project;
    bool any = false;
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id)
                && (c.gainDb != 0.0f || (! c.gainEnv.empty() && c.gainLine))) any = true;
    if (! any) return;
    proj.checkpoint();
    for (auto& t : proj.tracks)
        for (auto& c : t->audioClips)
            if (app.selectedClips.count (c.id))
            {
                c.gainDb = 0.0f;
                if (c.gainLine)
                    for (auto& p : c.gainEnv) p.db = 0.0f;
            }
    proj.sendContentChanged();
    repaintClipArea();
}
}
