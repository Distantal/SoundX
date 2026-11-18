#include "PianoRoll.h"
#include "SlotMenus.h"

namespace sx
{
using namespace juce;

namespace
{
    bool isBlack (int pitch)
    {
        const int p = pitch % 12;
        return p == 1 || p == 3 || p == 6 || p == 8 || p == 10;
    }

    String curveShapeName (CurveShape s)
    {
        switch (s)
        {
            case CurveShape::Line:     return TRANS ("Line");
            case CurveShape::Sine:     return TRANS ("Sine");
            case CurveShape::Triangle: return TRANS ("Triangle");
            case CurveShape::Saw:      return TRANS ("Sawtooth");
            case CurveShape::Free:     break;
        }
        return TRANS ("Free");
    }
}

//==============================================================================
double curveShapeValue (CurveShape shape, double t, double from, double to, double beats)
{
    t = juce::jlimit (0.0, 1.0, t);
    const double mid = 0.5 * (from + to);
    switch (shape)
    {
        case CurveShape::Line:
            return from + (to - from) * t;

        case CurveShape::Sine:
            // one full cycle: the drag's start value at both ends, the other value in the middle
            return mid + (from - mid) * std::cos (juce::MathConstants<double>::twoPi * t);

        case CurveShape::Triangle:
            // the same two extremes as the sine, with straight sides
            return from + (to - from) * (1.0 - std::abs (2.0 * t - 1.0));

        case CurveShape::Saw:
        {
            // one ramp per beat (at least one), each tooth jumping back to the start value
            const double teeth = juce::jlimit (1.0, 16.0, std::round (juce::jmax (1.0, beats)));
            const double u = t * teeth;
            return from + (to - from) * (u - std::floor (u));
        }

        case CurveShape::Free:
        default:
            break;
    }
    return from + (to - from) * t;
}

namespace
{
    String noteName (int pitch)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return String (names[pitch % 12]) + String (pitch / 12 - 2);
    }

    struct LaneOption { int cc; const char* name; };
    const LaneOption kLaneOptions[] = {
        { -1, "Velocity" }, { 1, "Modulation (CC1)" }, { 7, "Volume (CC7)" }, { 10, "Pan (CC10)" }, { 11, "Expression (CC11)" },
        { 64, "Sustain (CC64)" }, { 74, "Cutoff (CC74)" }, { kCCPitchBend, "Pitch Bend" }, { kCCAftertouch, "Aftertouch" } };

    String laneName (int cc)
    {
        for (auto& o : kLaneOptions) if (o.cc == cc) return TRANS (o.name);
        return "CC" + String (cc) + " " + controllerName (cc);
    }

    // every MIDI note that can sound inside [from, to] (absolute beats), for chord detection
    std::vector<chord::TimedNote> notesInRange (const Project& p, double from, double to)
    {
        std::vector<chord::TimedNote> out;
        for (auto& t : p.tracks)
        {
            if (t == nullptr) continue;
            for (auto& c : t->midiClips)
            {
                if (c.start + c.length < from - 1.0 || c.start > to + 1.0) continue;
                appendClipNotes (c, out);
            }
        }
        return out;
    }
}

//==============================================================================
// The keyboard strip beside the note grid, drawn after a real piano: the white keys form one
// continuous bed, the black keys are shorter and sit between the two white keys whose joint they
// cover, and that joint carries on in front of the black key (a real black key stops short of the
// white keys' front edge). The rows still follow the semitone grid, so every key stays on its own
// note row and a click is never ambiguous.
class PianoRollView::Keys : public Component
{
public:
    explicit Keys (PianoRollView& o) : owner (o) {}

    void paint (Graphics& g) override
    {
        const int kh = owner.keyH;
        const int sy = owner.scrollY();
        const int w = getWidth();
        const int h = getHeight();

        // the keyboard keeps its black and white look in the night theme too (see theme::keyWhite)
        g.fillAll (theme::keyWhite);

        const int p0 = jlimit (0, 127, 127 - (sy + h) / kh - 2);
        const int p1 = jlimit (0, 127, 127 - sy / kh + 2);
        auto rowTop = [kh, sy] (int p) { return (127 - p) * kh - sy; };

        // white key bed: a light gradient from the shaded back (left) to the bright front (right),
        // so the strip reads as one continuous white surface with the black keys standing on it
        {
            ColourGradient grad (theme::keyWhite.darker (0.06f), 0.0f, 0.0f,
                                 theme::keyWhite, (float) w, 0.0f, false);
            g.setGradientFill (grad);
            g.fillRect (0, 0, w, h);
        }

        // a held white key lights its whole shape: like the real key it reaches half a black key
        // above and below its own row (the part in front of a black key is the key's front)
        if (pressed >= 0 && ! isBlack (pressed))
        {
            const float topEdge    = (float) rowTop (pressed);
            const float bottomEdge = topEdge + (float) kh;
            const float low  = isBlack (pressed - 1) ? (float) rowTop (pressed - 1) + (float) kh * 0.5f
                                                     : bottomEdge;
            const float high = isBlack (pressed + 1) ? (float) rowTop (pressed + 1) + (float) kh * 0.5f
                                                     : topEdge;
            ColourGradient grad (theme::keyDown.darker (0.03f), 0.0f, 0.0f,
                                 theme::keyDown, (float) w, 0.0f, false);
            g.setGradientFill (grad);
            g.fillRect (0.0f, high, (float) w, jmax (0.0f, low - high));
        }

        // where two white keys meet without a black key between them (E-F and B-C) there is a real
        // joint across the whole key length
        for (int p = p0; p <= p1; ++p)
        {
            if (isBlack (p)) continue;
            if (p % 12 != 4 && p % 12 != 11) continue;
            g.setColour (theme::keyLine.withAlpha (0.9f));
            g.fillRect (0, rowTop (p) - 1, w, 1);
        }

        // black keys are shorter than the white ones: they stop in front of the white keys' front
        // edge, and the joint between the two white keys carries on in front of them
        const int bw = jlimit (14, jmax (14, w - 10), (int) std::round ((float) w * 0.62f));
        for (int p = p0; p <= p1; ++p)
        {
            if (! isBlack (p)) continue;
            g.setColour (theme::keyLine.withAlpha (0.7f));
            g.fillRect ((float) bw, (float) rowTop (p) + (float) kh * 0.5f, (float) (w - bw), 1.0f);
        }

        // the black keys themselves: raised, glossy, with a rounded front edge and a soft shadow
        // on the white key in front of them
        for (int p = p0; p <= p1; ++p)
        {
            if (! isBlack (p)) continue;

            const float top = (float) rowTop (p) + 1.0f;
            const float bottom = (float) rowTop (p) + (float) kh - 1.0f;
            const float height = jmax (2.0f, bottom - top);
            const float radius = jlimit (1.5f, 3.0f, (float) kh * 0.16f);
            const bool down = p == pressed;

            Path shape;
            shape.addRoundedRectangle (0.0f, top, (float) bw, height, radius, radius,
                                       false, true, false, true);

            if (down)
            {
                ColourGradient grad (theme::accent.brighter (0.3f), 0.0f, top,
                                     theme::accent.darker (0.35f), (float) bw, bottom, false);
                grad.addColour (0.55, theme::accent);
                g.setGradientFill (grad);
            }
            else
            {
                ColourGradient grad (theme::keyBlack.brighter (0.32f), 0.0f, top,
                                     theme::keyBlack.darker (0.55f), (float) bw, bottom, false);
                grad.addColour (0.45, theme::keyBlack);
                g.setGradientFill (grad);
            }
            g.fillPath (shape);

            // a thin bevel along the top and the front edge makes the key sit on the white keys
            g.setColour (down ? theme::onAccent.withAlpha (0.5f)
                              : theme::keyBlack.brighter (0.6f).withAlpha (0.9f));
            g.fillRect (1.0f, top, jmax (0.0f, (float) bw - radius - 1.5f), 1.0f);
            g.fillRect ((float) bw - 1.5f, top + radius, 1.0f, jmax (0.0f, height - radius * 2.0f));
            g.setColour (Colours::black.withAlpha (0.4f));
            g.fillRect (1.0f, bottom - 1.0f, jmax (0.0f, (float) bw - radius - 1.5f), 1.0f);

            // the black key casts a small shadow on the white key front beside it
            ColourGradient shadow (Colours::black.withAlpha (down ? 0.10f : 0.20f), (float) bw, 0.0f,
                                   Colours::transparentBlack, (float) bw + 4.0f, 0.0f, false);
            g.setGradientFill (shadow);
            g.fillRect ((float) bw, top, 4.0f, height);
        }

        // octave labels on the white key in front of the black keys
        for (int p = p0; p <= p1; ++p)
        {
            if (p % 12 != 0) continue;
            g.setColour (theme::keyLabel);
            g.setFont (theme::font (jlimit (7.5f, 10.5f, (float) kh * 0.72f)));
            g.drawText (noteName (p), bw, rowTop (p), jmax (1, w - bw - 3), kh,
                        Justification::centredRight);
        }

        g.setColour (theme::line);
        g.fillRect (w - 1, 0, 1, h);
    }

    void mouseDown (const MouseEvent& e) override { press (e); }
    void mouseDrag (const MouseEvent& e) override { press (e); }
    void mouseUp (const MouseEvent&) override { release(); }

private:
    void press (const MouseEvent& e)
    {
        const int p = jlimit (0, 127, 127 - (e.y + owner.scrollY()) / owner.keyH);
        if (p == pressed) return;
        release();
        pressed = p;
        owner.app.engine.noteOn (p, 100);
        repaint();
    }
    void release()
    {
        if (pressed >= 0) owner.app.engine.noteOff (pressed);
        pressed = -1;
        repaint();
    }
    PianoRollView& owner;
    int pressed = -1;
};

//==============================================================================
class PianoRollView::TimeRuler : public Component
{
public:
    explicit TimeRuler (PianoRollView& o) : owner (o) { setMouseCursor (MouseCursor::DraggingHandCursor); }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panelAlt);
        auto* c = owner.clip();
        auto& p = owner.app.project;
        const double ppb = owner.pxPerBeat;
        const int sx = owner.scrollX();
        const double origin = c != nullptr ? c->start : 0.0;
        g.setFont (theme::font (11.0f));
        const double b0 = (double) sx / ppb, b1 = (double) (sx + getWidth()) / ppb;
        const double fromAbs = jmax (0.0, origin + b0);
        if (owner.app.timeInSeconds)
        {
            // seconds timebase: a time scale in the ruler instead of bar numbers
            const double sec0 = p.beatsToSeconds (fromAbs), sec1 = p.beatsToSeconds (origin + b1);
            const double span = jmax (1.0e-3, sec1 - sec0);
            const double step = span > 240.0 ? 60.0 : span > 120.0 ? 30.0 : span > 60.0 ? 15.0 : span > 24.0 ? 10.0
                              : span > 12.0 ? 5.0 : span > 5.0 ? 2.0 : 1.0;

            for (double t = std::floor (sec0 / step) * step; t <= sec1 + 1.0e-9; t += step)
            {
                const float x = (float) ((p.secondsToBeats (jmax (0.0, t)) - origin) * ppb) - (float) sx;
                g.setColour (theme::gridBar);
                g.fillRect (x, 2.0f, 1.0f, (float) getHeight() - 2.0f);
                g.setColour (theme::textDim);
                g.drawText (tempomap::timeLabel (t), (int) x + 3, 0, 80, getHeight(), Justification::centredLeft);
            }

            g.setColour (theme::line);
            g.fillRect (0, getHeight() - 1, getWidth(), 1);
            const float px = (float) ((owner.app.engine.getPositionBeats() - origin) * ppb - sx);
            g.setColour (theme::accent);
            g.fillRect (px - 0.5f, 0.0f, 1.0f, (float) getHeight());
            return;
        }

        std::vector<tempomap::BarInfo> bars;
        tempomap::barStartsBetween (p.tempo, fromAbs, origin + b1, bars);
        if (bars.empty() || bars.front().beat > fromAbs + 1.0e-6)
        {
            // the visible area starts mid-bar: walk from the containing bar
            std::vector<tempomap::BarInfo> head;
            tempomap::barStartsBetween (p.tempo, jmax (0.0, fromAbs - 96.0), origin + b1, head);
            bars = head;
        }
        for (auto& b : bars)
        {
            const double w = (double) b.num * 4.0 / (double) b.den;
            const double beatLen = 4.0 / (double) b.den;
            const double bb = b.beat - origin;   // position inside the clip
            const float x = (float) (bb * ppb) - (float) sx;
            g.setColour (theme::gridBar);
            g.fillRect (x, 2.0f, 1.0f, (float) getHeight() - 2.0f);
            g.setColour (theme::textDim);
            g.drawText (String (tempomap::barPosition (p.tempo, b.beat).bar + 1), (int) x + 4, 0, 50, getHeight(), Justification::centredLeft);
            if (beatLen * ppb >= 30.0)
                for (double bt = beatLen; bt < w - 1.0e-6; bt += beatLen)
                {
                    g.setColour (theme::gridBeat);
                    g.fillRect ((float) ((bb + bt) * ppb - sx), (float) getHeight() - 7.0f, 1.0f, 7.0f);
                }
        }
        g.setColour (theme::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
        const float px = (float) ((owner.app.engine.getPositionBeats() - origin) * ppb - sx);
        g.setColour (theme::accent);
        g.fillRect (px - 0.5f, 0.0f, 1.0f, (float) getHeight());
    }

    // A click places the playhead right away: deferring the seek to mouse-up made a plain click
    // feel dead (and a 3 px hand tremor turned the gesture into a pan that discarded it). A drag
    // still scrolls the piano roll's time axis.
    void mouseDown (const MouseEvent& e) override
    {
        auto* c = owner.clip();
        const double origin = c != nullptr ? c->start : 0.0;
        const double beat = origin + ((double) e.x + owner.scrollX()) / owner.pxPerBeat;
        owner.app.engine.seekToBeats (jmax (0.0, e.mods.isAltDown() ? beat : snapTo (beat, owner.grid)));
        downX = e.x;
        downY = e.y;
        dragStartScroll = owner.scrollX();
        dragging = false;
        setMouseCursor (MouseCursor::DraggingHandCursor);
    }

    void mouseDrag (const MouseEvent& e) override
    {
        const int dx = e.x - downX, dy = e.y - downY;
        if (std::abs (dx) > 3 || std::abs (dy) > 3) dragging = true;
        if (dragging)
            owner.gridViewport.setViewPosition (jmax (0, dragStartScroll - dx), owner.scrollY());
    }

    void mouseUp (const MouseEvent&) override
    {
        dragging = false;
        setMouseCursor (MouseCursor::DraggingHandCursor);
    }

    // The bar ruler is the timeline's zoom strip: the wheel zooms the time axis horizontally,
    // anchored on the mouse (the bar under the cursor stays where it is). In the note grid the
    // same zoom needs Ctrl, because a plain wheel there has to scroll; on the ruler the wheel has
    // nothing else to do. Shift keeps scrolling, Alt zooms the pitch rows like it does in the grid.
    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (e.mods.isShiftDown())
            owner.gridViewport.setViewPosition (jmax (0, owner.scrollX() - (int) (w.deltaY * 300.0f)), owner.scrollY());
        else if (e.mods.isAltDown())
            owner.zoomVertical (w.deltaY > 0 ? 1 : -1, -1);
        else if (w.deltaY != 0.0f)
            owner.zoomHorizontal (w.deltaY > 0.0f ? 1.15 : 1.0 / 1.15, e.x + owner.scrollX());
        else if (w.deltaX != 0.0f)
            owner.gridViewport.setViewPosition (jmax (0, owner.scrollX() + (int) (w.deltaX * 300.0f)), owner.scrollY());
    }

private:
    PianoRollView& owner;
    int downX = 0, downY = 0;
    int dragStartScroll = 0;
    bool dragging = false;
};

//==============================================================================
// The chord row above the ruler: it reads the notes under it and names the chord that sounds at
// every moment, so the blocks follow the notes and change exactly where the harmony changes. It is
// entirely its own thing - the chord track of the arrangement neither feeds it nor is changed by
// it.
class PianoRollView::ChordStrip : public Component
{
public:
    explicit ChordStrip (PianoRollView& o) : owner (o) { setMouseCursor (MouseCursor::PointingHandCursor); }

    // x is local to the strip, whose left edge sits kKeysW pixels left of the note grid, so the
    // chord positions line up exactly with the bars under them
    double absAt (int x) const
    {
        auto* c = owner.clip();
        const double origin = c != nullptr ? c->start : 0.0;
        return origin + ((double) (x - kKeysW) + owner.scrollX()) / owner.pxPerBeat;
    }
    int xOf (double absBeat) const
    {
        auto* c = owner.clip();
        const double origin = c != nullptr ? c->start : 0.0;
        return kKeysW + (int) std::round ((absBeat - origin) * owner.pxPerBeat) - owner.scrollX();
    }

    void paint (Graphics& g) override
    {
        auto& app = owner.app;
        g.fillAll (theme::panelAlt);

        // the left cell is the row's name; everything right of it is the timeline
        auto label = getLocalBounds().removeFromLeft (kKeysW);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (8.5f, true));
        g.drawFittedText (TRANS ("CHORDS"), label.reduced (5, 0), Justification::centredLeft, 1);

        if (owner.clip() == nullptr)
        {
            g.setColour (theme::panelAlt);
            g.fillRect (label.withTrimmedLeft (kKeysW - 1));
            g.setColour (theme::line);
            g.fillRect (0, getHeight() - 1, getWidth(), 1);
            return;
        }

        const int lx = kKeysW;
        const double from = absAt (lx), to = absAt (getWidth());
        const auto segments = owner.buildChordSegments (from, to);
        const double play = app.engine.getPositionBeats();

        for (auto& s : segments)
        {
            const int x0 = jmax (lx, xOf (s.beat));
            const int x1 = jmin (getWidth(), xOf (s.beat + s.length));
            if (x1 <= lx || x0 >= getWidth()) continue;

            if (play >= s.beat - 1.0e-9 && play < s.beat + s.length - 1.0e-9)
            {
                g.setColour (theme::accent.withAlpha (0.15f));
                g.fillRect (x0, 1, jmax (0, x1 - x0), getHeight() - 3);
            }
            if (s.name.isEmpty()) continue;

            // a chord that lasts for several bars is repeated on their lines
            std::vector<double> spots { s.beat };
            std::vector<tempomap::BarInfo> bars;
            tempomap::barStartsBetween (app.project.tempo, s.beat + 1.0e-6, s.beat + s.length - 1.0e-6, bars);
            for (auto& b : bars)
                if (b.beat - spots.back() >= 70.0 / juce::jmax (1.0, owner.pxPerBeat)) spots.push_back (b.beat);

            for (size_t i = 0; i < spots.size(); ++i)
            {
                const int x = xOf (spots[i]);
                if (x > getWidth()) break;
                if (x > lx)
                {
                    g.setColour (theme::accent.withAlpha (i == 0 ? 0.55f : 0.18f));
                    g.fillRect (x, 3, 1, getHeight() - 6);
                }
                const int w = jmax (10, jmin (100, xOf (s.beat + s.length) - x - 3));
                g.setFont (theme::font (11.0f, i == 0));
                g.setColour (i == 0 ? theme::accentDark : theme::accent.withAlpha (0.45f));
                g.drawFittedText (s.name, jmax (lx, x) + 3, 0, w, getHeight(),
                                  Justification::centredLeft, 1);
            }
        }

        g.setColour (theme::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
        g.fillRect (kKeysW - 1, 0, 1, getHeight());
    }

    void mouseDown (const MouseEvent& e) override
    {
        // left click moves the playhead, like the ruler above the notes
        if (e.mods.isPopupMenu()) { showMenu (e.x); return; }
        if (e.x < kKeysW) return;
        double beat = absAt (e.x);
        if (! e.mods.isAltDown()) beat = snapTo (beat, owner.grid);
        owner.app.engine.seekToBeats (jmax (0.0, beat));
        owner.app.notifyView();
        repaint();
    }

    // A click on the chord row still locates the playhead, but dragging it never scrubs: the
    // playhead only changes on a click (the ruler above the notes handles drag-to-scroll).
    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        owner.gridViewport.mouseWheelMove (e.getEventRelativeTo (&owner.gridViewport), w);
    }

private:
    void showMenu (int)
    {
        // the row only reads the notes; the arrangement's chord track is a separate thing
        auto* app = &owner.app;
        PopupMenu m;
        m.addItem (1, TRANS ("Hide Chord Row"), true);

        SafePointer<ChordStrip> self (this);
        m.showMenuAsync (PopupMenu::Options(), [self, app] (int r)
        {
            if (r == 1 && self != nullptr)
            {
                app->pianoRollChords = false;
                app->notifyView();
            }
        });
    }

    PianoRollView& owner;
};

//==============================================================================
class PianoRollView::Grid : public Component
{
public:
    explicit Grid (PianoRollView& o) : owner (o) {}

    double xToBeat (int x) const { return (double) x / owner.pxPerBeat; }
    int yToPitch (int y) const { return jlimit (0, 127, 127 - y / owner.keyH); }
    Rectangle<float> noteRect (const Note& n) const
    {
        const double ppb = owner.pxPerBeat;
        return { (float) (n.start * ppb), (float) ((127 - n.pitch) * owner.keyH) + 1.0f, jmax (3.0f, (float) (n.length * ppb) - 1.0f),
                 (float) owner.keyH - 2.0f };
    }

    void paint (Graphics& g) override
    {
        auto clipB = g.getClipBounds();
        auto* c = owner.clip();
        auto& proj = owner.app.project;
        const int kh = owner.keyH;
        const double ppb = owner.pxPerBeat;

        const int p0 = jlimit (0, 127, 127 - clipB.getBottom() / kh - 1), p1 = jlimit (0, 127, 127 - clipB.getY() / kh + 1);
        for (int p = p0; p <= p1; ++p)
        {
            const int y = (127 - p) * kh;
            g.setColour (isBlack (p) ? theme::laneB.darker (0.025f) : theme::laneA);
            g.fillRect (clipB.getX(), y, clipB.getWidth(), kh);
            g.setColour (p % 12 == 0 ? theme::gridBar : theme::gridSub.withAlpha (0.8f));
            g.fillRect (clipB.getX(), y + kh - 1, clipB.getWidth(), 1);
        }

        if (c == nullptr)
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (14.0f));
            g.drawText (TRANS ("Double-click a MIDI region in the arrangement to edit it here."), getLocalBounds().withHeight (260),
                        Justification::centred);
            return;
        }

        // area past the end of the clip
        const int endX = (int) (c->length * ppb);
        if (endX < clipB.getRight())
        {
            g.setColour (theme::panelDeep.withAlpha (0.55f));
            g.fillRect (jmax (endX, clipB.getX()), clipB.getY(), clipB.getRight() - jmax (endX, clipB.getX()), clipB.getHeight());
        }

        const double origin = c->start;
        const double b0 = clipB.getX() / ppb, b1 = clipB.getRight() / ppb;
        const double fromAbs = jmax (0.0, origin + b0);
        std::vector<tempomap::BarInfo> bars;
        tempomap::barStartsBetween (proj.tempo, fromAbs, origin + b1, bars);
        if (bars.empty() || bars.front().beat > fromAbs + 1.0e-6)
        {
            std::vector<tempomap::BarInfo> head;
            tempomap::barStartsBetween (proj.tempo, jmax (0.0, fromAbs - 96.0), origin + b1, head);
            bars = head;
        }
        for (auto& b : bars)
        {
            const double w = (double) b.num * 4.0 / (double) b.den;
            const double beatLen = 4.0 / (double) b.den;
            const bool showSubBar = owner.grid > 0 && owner.grid * ppb >= 8.0 && owner.grid < beatLen - 1.0e-6;
            const double endB = jmin (b.beat + w, origin + b1);   // don't draw past the visible area
            for (double bt = 0.0; b.beat + bt < endB - 1.0e-6; bt += beatLen)
            {
                const double abs = b.beat + bt;
                if (abs < fromAbs - 1.0e-6) continue;   // partial bar at the left edge
                if (abs > fromAbs + 1.0e-6)
                {
                    const int x = (int) std::round ((abs - origin) * ppb);
                    g.setColour (theme::gridBeat);
                    g.fillRect (x, clipB.getY(), 1, clipB.getHeight());
                }
                if (showSubBar)
                    for (double s = owner.grid; s < beatLen - 1.0e-6; s += owner.grid)
                    {
                        const double ss = abs + s;
                        if (ss >= endB - 1.0e-6) break;
                        g.setColour (theme::gridSub.withAlpha (0.7f));
                        g.fillRect ((int) std::round ((ss - origin) * ppb), clipB.getY(), 1, clipB.getHeight());
                    }
            }
            if (b.beat >= fromAbs - 1.0e-6)
            {
                g.setColour (theme::gridBar);
                g.fillRect ((int) std::round ((b.beat - origin) * ppb), clipB.getY(), 1, clipB.getHeight());
            }
        }

        // notes from the other selected clips: visible for context, but not editable
        for (auto& gh : owner.ghosts)
        {
            if (gh.clipId == c->id) continue;
            auto* gtrack = proj.findTrack (gh.trackId);
            auto* gclip = gtrack != nullptr ? gtrack->findMidi (gh.clipId) : nullptr;
            if (gclip == nullptr) continue;
            const double gOffset = gclip->start - c->start;
            const Colour gcol = gtrack->colour;
            for (auto& n : gclip->notes)
            {
                Rectangle<float> gr ((float) ((gOffset + n.start) * ppb), (float) ((127 - n.pitch) * owner.keyH) + 1.0f,
                                     jmax (3.0f, (float) (n.length * ppb) - 1.0f), (float) owner.keyH - 2.0f);
                if (! gr.toNearestInt().intersects (clipB)) continue;
                g.setColour (gcol.withAlpha (0.20f));
                g.fillRoundedRectangle (gr, 2.0f);
            }
        }

        // notes
        auto* track = proj.findTrack (owner.app.editTrack);
        const Colour col = track != nullptr ? track->colour : theme::accent;
        g.setFont (theme::font (10.5f));
        for (auto& n : c->notes)
        {
            auto r = noteRect (n);
            if (! r.toNearestInt().intersects (clipB)) continue;
            const bool sel = owner.selNotes.count (n.id) > 0;
            const float a = 0.5f + 0.5f * (float) n.velocity / 127.0f;
            // a selected note is warm peach, not the accent blue: blue is the colour of the
            // playhead / tools / toggles, so a selection never reads as "this is the active thing"
            g.setColour (sel ? theme::selectionFill : col.withAlpha (a));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (sel ? theme::selectionEdge : col.darker (0.35f));
            g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
            if (r.getWidth() > 28.0f && owner.keyH >= 12)
            {
                g.setColour (sel ? theme::selectionText : theme::onAccent.withAlpha (0.92f));
                g.drawText (noteName (n.pitch), r.reduced (3.0f, 0.0f), Justification::centredLeft);
            }
            if (n.slide)
            {
                // FL-style slide marker: a small rising diagonal on the right edge
                g.setColour (sel ? theme::selectionText : theme::onAccent);
                const float x = r.getRight() - 5.0f;
                const float y = r.getBottom() - 3.0f;
                g.drawLine (x - 5.0f, y, x, y - jmin (6.0f, r.getHeight() - 2.0f), 1.4f);
            }
        }

        if (mode == Mode::Rubber)
        {
            // the marquee speaks the same language as the selection it is about to make
            g.setColour (theme::selectionEdge.withAlpha (0.12f));
            g.fillRect (rubber);
            g.setColour (theme::selectionEdge);
            g.drawRect (rubber, 1);
        }

        // the playhead x the timer invalidated (playXForPaint): never the live position, or a
        // late paint would draw it outside the repainted column (flicker / ghost trails)
        const int px = (int) (owner.playXForPaint >= 0
                                  ? owner.playXForPaint
                                  : (int) std::round ((owner.app.engine.getPositionBeats() - origin) * ppb));
        g.setColour (theme::accent);
        g.fillRect (px, clipB.getY(), 1, clipB.getHeight());
    }

    int hitNote (Point<int> p) const
    {
        auto* c = owner.clip();
        if (c == nullptr) return -1;
        for (size_t i = c->notes.size(); i-- > 0;)
            if (noteRect (c->notes[i]).expanded (0.0f, 0.5f).contains ((float) p.x, (float) p.y)) return (int) i;
        return -1;
    }

    void mouseMove (const MouseEvent& e) override
    {
        const int h = hitNote (e.getPosition());
        if (owner.tool == Tool::Eraser) { setMouseCursor (MouseCursor::NormalCursor); return; }
        if (owner.tool == Tool::Scissors) { setMouseCursor (drawCrosshairCursor()); return; }
        if (h >= 0 && noteRect (owner.clip()->notes[(size_t) h]).getRight() - (float) e.x < 7.0f) setMouseCursor (MouseCursor::LeftRightResizeCursor);
        else if (owner.tool == Tool::Pencil) setMouseCursor (drawCrosshairCursor());
        else setMouseCursor (MouseCursor::NormalCursor);
    }

    void audition (int pitch)
    {
        auto* track = owner.app.project.findTrack (owner.app.editTrack);
        if (track != nullptr) owner.app.engine.setSelectedTrack (track->id);
        owner.app.engine.auditionNote (pitch, owner.lastVelocity, 180);
    }

    void deleteNoteAt (int idx)
    {
        auto* c = owner.clip();
        if (c == nullptr || idx < 0 || idx >= (int) c->notes.size()) return;
        owner.selNotes.erase (c->notes[(size_t) idx].id);
        c->notes.erase (c->notes.begin() + idx);
    }

    Note* createNote (const MouseEvent& e)
    {
        auto* c = owner.clip();
        if (c == nullptr) return nullptr;
        auto& proj = owner.app.project;
        Note n;
        n.id = proj.newId();
        n.pitch = yToPitch (e.y);
        const double g = owner.grid > 0 ? owner.grid : 0.0625;
        n.start = jmax (0.0, std::floor (xToBeat (e.x) / g + 1.0e-6) * g);
        n.length = jmax (g, owner.lastLength);
        n.velocity = owner.lastVelocity;
        n.slide = owner.slideMode;
        c->notes.push_back (n);
        owner.ensureClipCovers (*c, n.start + n.length);
        owner.selNotes = { n.id };
        audition (n.pitch);
        return &c->notes.back();
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto* c = owner.clip();
        if (c == nullptr) return;
        auto& proj = owner.app.project;
        mode = Mode::None;
        moved = false;
        origs.clear();
        downPos = e.getPosition();
        downBeat = xToBeat (e.x);
        downPitch = yToPitch (e.y);

        const int hit = hitNote (e.getPosition());

        if (owner.tool == Tool::Scissors)
        {
            // click a note to cut it in two at that point (the grid snaps the cut, Alt = free)
            mode = Mode::None;
            if (hit >= 0)
            {
                double cut = xToBeat (e.x);
                if (! e.mods.isAltDown() && owner.grid > 0.0)
                    cut = std::round (cut / owner.grid) * owner.grid;   // snap to the piano roll grid
                owner.splitNoteAt (hit, cut);
            }
            return;
        }

        if (e.mods.isPopupMenu() || owner.tool == Tool::Eraser)
        {
            mode = Mode::Erase;
            if (hit >= 0) { proj.checkpoint(); checkpointed = true; deleteNoteAt (hit); owner.changed(); }
            else checkpointed = false;
            return;
        }

        if (hit >= 0)
        {
            auto& n = c->notes[(size_t) hit];
            const int id = n.id;
            if (e.mods.isShiftDown() && owner.selNotes.count (id)) { owner.selNotes.erase (id); repaint(); return; }
            if (! owner.selNotes.count (id))
            {
                if (! e.mods.isShiftDown()) owner.selNotes.clear();
                owner.selNotes.insert (id);
            }
            primary = id;
            mode = noteRect (n).getRight() - (float) e.x < 7.0f ? Mode::Resize : Mode::Move;
            duplicate = e.mods.isCtrlDown() && mode == Mode::Move;
            for (auto& m : c->notes)
                if (owner.selNotes.count (m.id)) origs.push_back ({ m.id, m.start, m.length, m.pitch });
            owner.lastVelocity = n.velocity;
            if (mode == Mode::Move) audition (n.pitch);
            repaint();
            owner.laneChanged();
            return;
        }

        if (owner.tool == Tool::Pencil)
        {
            proj.checkpoint();
            checkpointed = true;
            if (auto* n = createNote (e))
            {
                primary = n->id;
                origs.push_back ({ n->id, n->start, n->length, n->pitch });
                mode = Mode::Create;
                owner.changed();
            }
            return;
        }

        if (! e.mods.isShiftDown()) owner.selNotes.clear();
        baseSel = owner.selNotes;
        mode = Mode::Rubber;
        rubber = Rectangle<int> (e.getPosition(), e.getPosition());
        repaint();
        owner.laneChanged();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        auto* c = owner.clip();
        if (c == nullptr) return;
        auto& proj = owner.app.project;
        const double minLen = owner.grid > 0 ? owner.grid : 0.0625;

        if (mode == Mode::Erase)
        {
            const int hit = hitNote (e.getPosition());
            if (hit >= 0)
            {
                if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
                deleteNoteAt (hit);
                owner.changed();
            }
            return;
        }

        if (mode == Mode::Rubber)
        {
            rubber = Rectangle<int>::leftTopRightBottom (jmin (downPos.x, e.x), jmin (downPos.y, e.y), jmax (downPos.x, e.x), jmax (downPos.y, e.y));
            owner.selNotes = baseSel;
            for (auto& n : c->notes)
                if (noteRect (n).toNearestInt().intersects (rubber)) owner.selNotes.insert (n.id);
            repaint();
            owner.laneChanged();
            return;
        }

        if (mode == Mode::None) return;
        if (! moved && e.getDistanceFromDragStart() < 3 && mode != Mode::Create) return;

        if (! moved)
        {
            moved = true;
            if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
            if (duplicate)
            {
                std::set<int> newSel;
                std::vector<Note> copies;
                for (auto& o : origs)
                    for (auto& n : c->notes)
                        if (n.id == o.id) { auto cp = n; cp.id = proj.newId(); copies.push_back (cp); }
                for (auto& cp : copies) { c->notes.push_back (cp); newSel.insert (cp.id); }
                for (size_t i = 0; i < origs.size() && i < copies.size(); ++i)
                {
                    if (origs[i].id == primary) primary = copies[i].id;
                    origs[i].id = copies[i].id;
                }
                owner.selNotes = newSel;
            }
        }

        const auto* po = findOrig (primary);
        if (po == nullptr) return;
        const double beat = xToBeat (e.x);

        if (mode == Mode::Move)
        {
            double dB = snapTo (po->start + (beat - downBeat), owner.grid) - po->start;
            int dP = yToPitch (e.y) - downPitch;
            double minStart = 1.0e9;
            int lo = 127, hi = 0;
            for (auto& o : origs) { minStart = jmin (minStart, o.start); lo = jmin (lo, o.pitch); hi = jmax (hi, o.pitch); }
            dB = jmax (dB, -minStart);
            dP = jlimit (-lo, 127 - hi, dP);
            double maxEnd = 0.0;
            for (auto& o : origs)
                for (auto& n : c->notes)
                    if (n.id == o.id)
                    {
                        n.start = o.start + dB;
                        n.pitch = o.pitch + dP;
                        maxEnd = jmax (maxEnd, n.start + n.length);
                    }
            ensureCovered (*c, maxEnd);
            if (dP != lastDP) { lastDP = dP; audition (po->pitch + dP); }
        }
        else if (mode == Mode::Resize || mode == Mode::Create)
        {
            const double endNow = owner.grid > 0 ? std::ceil ((beat - 1.0e-6) / owner.grid) * owner.grid : beat;
            const double newLen = jmax (minLen, endNow - po->start);
            const double dL = newLen - po->length;
            double maxEnd = 0.0;
            for (auto& o : origs)
                for (auto& n : c->notes)
                    if (n.id == o.id)
                    {
                        n.length = jmax (minLen, o.length + dL);
                        maxEnd = jmax (maxEnd, n.start + n.length);
                        if (n.id == primary) owner.lastLength = n.length;
                    }
            ensureCovered (*c, maxEnd);
        }
        owner.changed();
    }

    void mouseUp (const MouseEvent&) override
    {
        mode = Mode::None;
        origs.clear();
        rubber = {};
        lastDP = 0;
        repaint();
        owner.laneChanged();
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        auto* c = owner.clip();
        if (c == nullptr || hitNote (e.getPosition()) >= 0) return;
        owner.app.project.checkpoint();
        createNote (e);
        owner.changed();
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            owner.zoomHorizontal (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.x);
        else if (e.mods.isAltDown())
            owner.zoomVertical (w.deltaY > 0 ? 1 : -1, e.y);
        else if (e.mods.isShiftDown())
            owner.gridViewport.setViewPosition (jmax (0, owner.scrollX() - (int) (w.deltaY * 300.0f)), owner.scrollY());
        else
            owner.gridViewport.mouseWheelMove (e.getEventRelativeTo (&owner.gridViewport), w);
    }

private:
    enum class Mode { None, Move, Resize, Create, Rubber, Erase };
    struct Orig { int id; double start, length; int pitch; };

    const Orig* findOrig (int id) const
    {
        for (auto& o : origs) if (o.id == id) return &o;
        return nullptr;
    }
    void ensureCovered (MidiClip& c, double end) { owner.ensureClipCovers (c, end); }

    PianoRollView& owner;
    Mode mode = Mode::None;
    bool moved = false, checkpointed = false, duplicate = false;
    Point<int> downPos;
    double downBeat = 0.0;
    int downPitch = 60, primary = 0, lastDP = 0;
    std::vector<Orig> origs;
    Rectangle<int> rubber;
    std::set<int> baseSel;
};

//==============================================================================
class PianoRollView::Lane : public Component
{
public:
    explicit Lane (PianoRollView& o) : owner (o) {}

    // --- stacked lanes ---------------------------------------------------------
    int laneCount() const { return jmax (1, (int) owner.laneControllers.size()); }
    int controllerAt (int i) const
    {
        const int n = (int) owner.laneControllers.size();
        return n > 0 ? owner.laneControllers[(size_t) jlimit (0, n - 1, i)] : -1;
    }
    int requestedTotal() const
    {
        int s = 0;
        for (int i = 0; i < laneCount(); ++i) s += owner.laneHeightAt (i);
        return jmax (1, s);
    }
    Rectangle<int> laneRect (int i) const
    {
        const int n = laneCount();
        const int total = requestedTotal();
        int above = 0;
        for (int k = 0; k < jlimit (0, n - 1, i); ++k) above += owner.laneHeightAt (k);
        const int h = owner.laneHeightAt (i);
        const int compH = jmax (1, getHeight());
        const int y0 = above * compH / total;
        const int y1 = jmin (compH, (above + h) * compH / total);
        return { 0, y0, getWidth(), jmax (1, y1 - y0) };
    }
    int laneAtY (int y) const
    {
        const int n = laneCount();
        for (int i = 0; i < n; ++i) if (laneRect (i).contains (0, y)) return i;
        return n - 1;
    }
    // index of the lane whose bottom border sits under y (interior borders only), else -1
    int borderAtY (int y) const
    {
        for (int i = 0; i < laneCount() - 1; ++i)
            if (std::abs (laneRect (i).getBottom() - y) <= 4) return i;
        return -1;
    }
    static int maxValueFor (int controller) { return controller < 0 ? 127 : controllerMax (controller); }
    Rectangle<int> area (int i) const { return laneRect (i).reduced (0, 8); }
    double xToBeat (int x) const { return (double) (x + owner.scrollX()) / owner.pxPerBeat; }
    float beatToX (double b) const { return (float) (b * owner.pxPerBeat - owner.scrollX()); }
    static int yToValue (int y, Rectangle<int> a, int maxV)
    {
        const double f = 1.0 - (double) (y - a.getY()) / (double) jmax (1, a.getHeight());
        return (int) std::round (jlimit (0.0, 1.0, f) * maxV);
    }
    static float valueToY (int v, Rectangle<int> a, int maxV)
    {
        return (float) a.getBottom() - (float) v / (float) jmax (1, maxV) * (float) a.getHeight();
    }
    // anchors: with Quantize on they snap to the grid; with it off they follow the mouse
    // exactly and the free drawing uses a dense step so the curve comes out smooth
    bool snapEnabled() const { return owner.grid > 0.0; }
    double snappedBeat (double beat) const { return snapEnabled() ? snapTo (beat, owner.grid) : jmax (0.0, beat); }
    double anchorStep() const
    {
        if (snapEnabled()) return owner.grid;
        // quantize off: one anchor per ~1.5 px, capped to a useful resolution so a long drag
        // stays light but still looks like a smooth drawn curve
        return jmax (1.0 / 512.0, jmin (1.0 / 96.0, 1.5 / jmax (1.0, owner.pxPerBeat)));
    }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::laneA);
        auto* track = owner.app.project.findTrack (owner.app.editTrack);
        const Colour col = track != nullptr ? track->colour : theme::accent;

        const int n = laneCount();
        for (int i = 0; i < n; ++i)
        {
            Graphics::ScopedSaveState ss (g);
            auto r = laneRect (i);
            g.reduceClipRegion (r);
            paintLane (g, i, r, col);
        }

        g.setColour (theme::line);
        g.fillRect (0, 0, getWidth(), 1);
        for (int i = 1; i < n; ++i)
            g.fillRect (0, laneRect (i).getY(), getWidth(), 1);

        // rubber band of the pointer tool (the same warm colour as the selection it makes)
        if (! marquee.isEmpty())
        {
            g.setColour (theme::selectionEdge.withAlpha (0.14f));
            g.fillRect (marquee);
            g.setColour (theme::selectionEdge);
            g.drawRect (marquee, 1);
        }

        if (tip.isNotEmpty())
        {
            g.setFont (theme::font (12.0f, true));
            const int w = GlyphArrangement::getStringWidthInt (g.getCurrentFont(), tip) + 12;
            auto tr = Rectangle<int> (tipPos.x + 10, jmax (2, tipPos.y - 24), w, 20);
            g.setColour (theme::tooltipBg);
            g.fillRoundedRectangle (tr.toFloat(), 4.0f);
            g.setColour (theme::onAccent);
            g.drawText (tip, tr, Justification::centred);
        }

        if (auto* c = owner.clip())
        {
            const float px = beatToX (owner.app.engine.getPositionBeats() - c->start);
            g.setColour (theme::accent);
            g.fillRect (px, 0.0f, 1.0f, (float) getHeight());
        }
    }

    void paintLane (Graphics& g, int idx, Rectangle<int> r, Colour col)
    {
        auto* c = owner.clip();
        const int controller = controllerAt (idx);
        auto a = r.reduced (0, 8);
        const int maxV = maxValueFor (controller);
        const double ppb = owner.pxPerBeat;
        const int sx = owner.scrollX();

        g.setColour (theme::laneA);
        g.fillRect (r);

        // lane name in the top-left corner of the strip
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        g.drawText (controller < 0 ? TRANS ("Velocity") : laneName (controller),
                    r.getX() + 6, r.getY() + 2, 170, 14, Justification::centredLeft);

        // grid
        if (c != nullptr)
        {
            auto& proj = owner.app.project;
            const double b0 = (double) sx / ppb, b1 = (double) (sx + getWidth()) / ppb;
            const double fromAbs = jmax (0.0, c->start + b0);
            std::vector<tempomap::BarInfo> bars;
            tempomap::barStartsBetween (proj.tempo, fromAbs, c->start + b1, bars);
            if (bars.empty() || bars.front().beat > fromAbs + 1.0e-6)
            {
                std::vector<tempomap::BarInfo> head;
                tempomap::barStartsBetween (proj.tempo, jmax (0.0, fromAbs - 96.0), c->start + b1, head);
                bars = head;
            }
            for (auto& b : bars)
            {
                const double w = (double) b.num * 4.0 / (double) b.den;
                const double beatLen = 4.0 / (double) b.den;
                const double endB = jmin (b.beat + w, c->start + b1);
                for (double bt = 0.0; b.beat + bt < endB - 1.0e-6; bt += beatLen)
                {
                    const double abs = b.beat + bt;
                    if (abs < fromAbs - 1.0e-6) continue;
                    g.setColour (abs <= fromAbs + 1.0e-6 ? theme::gridBar : theme::gridBeat);
                    g.fillRect (beatToX (abs - c->start), (float) r.getY(), 1.0f, (float) r.getHeight());
                }
            }
            const float endX = beatToX (c->length);
            if (endX < (float) getWidth())
            {
                g.setColour (theme::panelDeep.withAlpha (0.55f));
                g.fillRect (jmax (0.0f, endX), (float) r.getY(), (float) getWidth() - jmax (0.0f, endX), (float) r.getHeight());
            }
        }
        for (int i = 0; i <= 4; ++i)
        {
            g.setColour (theme::lineSoft);
            g.fillRect (0.0f, (float) a.getY() + (float) a.getHeight() * (float) i / 4.0f, (float) getWidth(), 1.0f);
        }
        if (c == nullptr) return;

        if (controller < 0)
        {
            for (auto& n : c->notes)
            {
                const float x = beatToX (n.start);
                if (x < -8.0f || x > (float) getWidth() + 8.0f) continue;
                const float y = valueToY (n.velocity, a, 127);
                const bool sel = owner.selNotes.count (n.id) > 0;
                g.setColour (sel ? theme::selectionEdge : col);
                g.fillRect (x - 1.0f, y, 3.0f, (float) a.getBottom() - y);
                g.fillEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f);
                if (sel)
                {
                    g.setColour (theme::onAccent);
                    g.fillEllipse (x - 2.0f, y - 2.0f, 4.0f, 4.0f);
                }
            }
        }
        else
        {
            std::vector<const CCPoint*> pts;
            for (auto& p : c->ccs) if (p.cc == controller) pts.push_back (&p);
            std::sort (pts.begin(), pts.end(), [] (const CCPoint* x, const CCPoint* y) { return x->beat < y->beat; });

            // Pitch bend springs back to the middle: the parts of the clip without an anchor are
            // drawn flat at the centre value (and the engine sends it there, see
            // appendControllerMessages), so drawing a bend only bends the part it covers.
            const bool rests = controllerRestsAtDefault (controller);
            const float restY = valueToY (controllerDefault (controller), a, maxV);
            const float clipEndX = jmax (beatToX (c->length), (float) getWidth());

            if (rests)
            {
                // the centre the bend returns to is worth a line of its own: the four grid lines of
                // the lane are too faint to read as "this is zero bend"
                g.setColour (theme::line);
                g.fillRect (0.0f, restY - 0.5f, (float) getWidth(), 1.0f);

                float curveEndX = clipEndX;
                for (auto* p : pts) curveEndX = jmax (curveEndX, beatToX (p->beat));

                Path path;
                path.startNewSubPath (0.0f, restY);
                float y = restY;
                for (auto* p : pts)
                {
                    const float x = beatToX (p->beat), v = valueToY (p->value, a, maxV);
                    if (x > 0.0f) path.lineTo (x, y);     // hold the previous value up to this anchor
                    path.lineTo (x, v);                   // then step to the new one
                    y = v;
                }
                // back to the centre right at the last anchor (a step, not a ramp: the engine sends
                // the centre value there as well), then it stays on the line
                if (! pts.empty()) path.lineTo (beatToX (pts.back()->beat), restY);
                path.lineTo (jmax (curveEndX, (float) getWidth()), restY);
                Path fill (path);
                fill.closeSubPath();
                g.setColour (col.withAlpha (pts.empty() ? 0.10f : 0.16f));
                g.fillPath (fill);
                g.setColour (col.withAlpha (pts.empty() ? 0.55f : 1.0f));
                g.strokePath (path, PathStrokeType (pts.empty() ? 1.2f : 1.5f));
            }
            else if (! pts.empty())
            {
                Path path;
                float x0 = beatToX (pts.front()->beat);
                path.startNewSubPath (x0, (float) a.getBottom());
                path.lineTo (x0, valueToY (pts.front()->value, a, maxV));
                for (size_t i = 0; i < pts.size(); ++i)
                {
                    const float x = beatToX (pts[i]->beat), y = valueToY (pts[i]->value, a, maxV);
                    path.lineTo (x, path.getCurrentPosition().y);
                    path.lineTo (x, y);
                }
                const float endX = jmax (beatToX (c->length), beatToX (pts.back()->beat));
                path.lineTo (endX, path.getCurrentPosition().y);
                Path fill (path);
                fill.lineTo (endX, (float) a.getBottom());
                fill.closeSubPath();
                g.setColour (col.withAlpha (0.16f));
                g.fillPath (fill);
                Path stroke (path);
                g.setColour (col);
                g.strokePath (stroke, PathStrokeType (1.5f));
            }

            if (! pts.empty())
            {
                for (auto* p : pts)
                {
                    const float x = beatToX (p->beat), y = valueToY (p->value, a, maxV);
                    if (x < -6.0f || x > (float) getWidth() + 6.0f) continue;
                    const bool sel = owner.selCC.count (p->id) > 0;
                    g.setColour (sel ? theme::selectionFill : theme::onAccent);
                    g.fillRect (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
                    g.setColour (sel ? theme::selectionEdge : col);
                    g.drawRect (x - 3.5f, y - 3.5f, 7.0f, 7.0f, sel ? 2.0f : 1.5f);
                    if (sel) { g.setColour (theme::selectionText); g.fillRect (x - 1.5f, y - 1.5f, 3.0f, 3.0f); }
                }
            }
            else
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::font (11.5f));
                auto hint = r.reduced (8, 0);
                if (rests) hint = hint.withTrimmedBottom (hint.getHeight() / 2);   // keep off the centre line
                g.drawFittedText (owner.tool == Tool::Pencil
                                      ? TRANS ("Click or drag to draw controller data. Shift+drag draws a ramp.")
                                      : TRANS ("Click an anchor to select it, drag in the lane to select several, right-click deletes."),
                                  hint, Justification::centred, 2);
            }
        }
    }

    int hitCC (Point<int> p, int controller, Rectangle<int> a) const
    {
        auto* c = owner.clip();
        if (c == nullptr) return -1;
        int best = -1;
        float bd = 10.0f;
        const int maxV = maxValueFor (controller);
        for (size_t i = 0; i < c->ccs.size(); ++i)
        {
            if (c->ccs[i].cc != controller) continue;
            const float dx = beatToX (c->ccs[i].beat) - (float) p.x, dy = valueToY (c->ccs[i].value, a, maxV) - (float) p.y;
            const float d = std::sqrt (dx * dx + dy * dy);
            if (d < bd) { bd = d; best = (int) i; }
        }
        return best;
    }

    int hitVelocity (Point<int> p) const
    {
        auto* c = owner.clip();
        if (c == nullptr) return -1;
        int best = -1;
        float bd = 8.0f;
        for (size_t i = 0; i < c->notes.size(); ++i)
        {
            const float d = std::abs (beatToX (c->notes[i].start) - (float) p.x);
            if (d < bd) { bd = d; best = (int) i; }
        }
        return best;
    }

    // adds an anchor (replacing one that sits on the same beat) and returns its id
    int setPoint (MidiClip& c, int controller, double beat, int value, Rectangle<int> a)
    {
        const double res = anchorStep();
        const int maxV = maxValueFor (controller);
        for (auto it = c.ccs.begin(); it != c.ccs.end();)
        {
            if (it->cc == controller && std::abs (it->beat - beat) < res * 0.5) it = c.ccs.erase (it);
            else ++it;
        }
        CCPoint p;
        p.id = owner.app.project.newId();
        p.cc = controller;
        p.beat = beat;
        p.value = jlimit (0, maxV, value);
        c.ccs.push_back (p);
        return p.id;
    }

    // Fills the range between two drag points with the selected curve shape, replacing whatever
    // controller data sat in it (so dragging around rebuilds the shape cleanly).
    void applyShape (MidiClip& c, Rectangle<int> a, double b0, int v0, double b1, int v1)
    {
        const double lo = jmin (b0, b1), hi = jmax (b0, b1);
        const double span = hi - lo;
        if (span < 1.0e-6)
        {
            strokeTail = setPoint (c, activeController, lo, v0, a);
            strokeCount = 1;
            return;
        }

        for (auto it = c.ccs.begin(); it != c.ccs.end();)
        {
            if (it->cc == activeController && it->beat >= lo - 1.0e-6 && it->beat <= hi + 1.0e-6) it = c.ccs.erase (it);
            else ++it;
        }

        const double step = anchorStep();
        const int steps = jlimit (1, 8192, (int) std::round (span / step));
        const int maxV = maxValueFor (activeController);
        strokeCount = 0;
        for (int i = 0; i <= steps; ++i)
        {
            const double u = (double) i / (double) steps;
            const double bb = lo + span * u;
            // a drag from right to left draws the shape mirrored, so it always starts at the point
            // the gesture started from
            const double shapeT = b1 >= b0 ? u : 1.0 - u;
            const int v = jlimit (0, maxV, (int) std::round (curveShapeValue (owner.curveShape, shapeT,
                                                                             (double) v0, (double) v1, span)));
            strokeTail = setPoint (c, activeController, bb, v, a);
            ++strokeCount;
        }
        owner.ensureClipCovers (c, hi);
    }

    // A stroke in a lane whose controller springs back to a default (pitch bend) ends on that
    // default: the last anchor is set to the centre value, so the curve returns to the middle
    // exactly where the pencil stopped instead of leaning on the rest of the clip.
    void finishStroke()
    {
        auto* c = owner.clip();
        const int tail = strokeTail;
        const int drawn = strokeCount;
        strokeTail = 0;
        strokeCount = 0;
        if (c == nullptr || tail == 0 || drawn < 2) return;   // a single click keeps the value it hit
        if (! controllerRestsAtDefault (activeController)) return;
        const int rest = controllerDefault (activeController);
        for (auto& p : c->ccs)
            if (p.id == tail)
            {
                if (p.value != rest) { p.value = rest; owner.changed(); }
                break;
            }
    }

    void mouseMove (const MouseEvent& e) override
    {
        if (e.y <= 5 || borderAtY (e.y) >= 0) { setMouseCursor (MouseCursor::UpDownResizeCursor); return; }
        // the pencil writes controller data, the pointer only selects
        setMouseCursor (owner.tool == Tool::Pencil ? drawCrosshairCursor() : MouseCursor::NormalCursor);
    }

    void mouseDown (const MouseEvent& e) override
    {
        // top border: drag to resize the whole CC panel
        if (e.y <= 5 && ! e.mods.isPopupMenu())
        {
            mode = Mode::ResizePanel;
            panelStartTotal = owner.lanePanelTotal();
            return;
        }

        // a border between two stacked lanes: drag to move the split between them
        const int border = borderAtY (e.y);
        if (border >= 0 && ! e.mods.isPopupMenu())
        {
            mode = Mode::ResizeLane;
            resizeIdx = border;
            return;
        }

        auto* c = owner.clip();
        if (c == nullptr) return;
        auto& proj = owner.app.project;
        activeLaneIdx = laneAtY (e.y);
        activeController = controllerAt (activeLaneIdx);
        activeRect = laneRect (activeLaneIdx);
        auto a = activeRect.reduced (0, 8);
        const int maxV = maxValueFor (activeController);
        mode = Mode::None;
        checkpointed = false;
        downBeat = xToBeat (e.x);
        downValue = yToValue (e.y, a, maxV);
        lastBeat = downBeat;
        lastValue = downValue;

        if (activeController < 0)
        {
            const int hit = hitVelocity (e.getPosition());
            if (hit >= 0)
            {
                auto& n = c->notes[(size_t) hit];
                if (! owner.selNotes.count (n.id))
                {
                    if (! e.mods.isShiftDown()) owner.selNotes.clear();
                    owner.selNotes.insert (n.id);
                }
                velOrigs.clear();
                for (auto& m : c->notes) if (owner.selNotes.count (m.id)) velOrigs.push_back ({ m.id, m.velocity });
                velPrimary = n.id;
                velDownValue = n.velocity;
                mode = Mode::Velocity;
                proj.checkpoint();
                checkpointed = true;
                applyVelocity (e);
            }
            else if (owner.tool == Tool::Pencil)
                mode = Mode::VelDraw;   // painting velocities across notes is a pencil gesture
            else
                mode = Mode::None;      // the pointer only selects notes / drags the one under it
            owner.app.engine.setSelectedTrack (owner.app.editTrack);
            owner.changed();
            return;
        }

        const int hit = hitCC (e.getPosition(), activeController, a);
        if (e.mods.isPopupMenu() || owner.tool == Tool::Eraser)
        {
            mode = Mode::Erase;
            if (hit >= 0) { proj.checkpoint(); checkpointed = true; c->ccs.erase (c->ccs.begin() + hit); owner.changed(); }
            return;
        }
        if (hit >= 0)
        {
            const int id = c->ccs[(size_t) hit].id;
            if (e.mods.isShiftDown() && owner.selCC.count (id)) { owner.selCC.erase (id); repaint(); return; }
            if (! owner.selCC.count (id))
            {
                if (! e.mods.isShiftDown()) owner.selCC.clear();
                owner.selCC.insert (id);
            }
            ccOrigs.clear();
            for (auto& p : c->ccs) if (owner.selCC.count (p.id)) ccOrigs.push_back ({ p.id, p.beat, p.value });
            ccPrimary = id;
            mode = Mode::PointDrag;
            repaint();
            return;
        }

        // the pointer tool only selects controller anchors (click, or drag a marquee): it never
        // writes data, so a click in an empty lane spot does not add an anchor
        if (owner.tool != Tool::Pencil)
        {
            if (! e.mods.isShiftDown()) owner.selCC.clear();   // Shift adds to the current selection
            baseSelCC = owner.selCC;
            marqueeStart = e.getPosition();
            marquee = Rectangle<int> (marqueeStart, marqueeStart);
            mode = Mode::CCRubber;
            repaint();
            return;
        }

        proj.checkpoint();
        checkpointed = true;
        owner.selCC.clear();

        if (e.mods.isShiftDown())
        {
            mode = Mode::Ramp;
            rampBase = c->ccs;
            rampStartBeat = snappedBeat (downBeat);
            rampStartValue = downValue;
        }
        else if (owner.curveShape != CurveShape::Free && activeController >= 0)
        {
            // the curve tool: the drag fills the range with the selected shape
            mode = Mode::Shape;
            shapeBase = c->ccs;
            shapeStartBeat = snappedBeat (downBeat);
            shapeStartValue = downValue;
            strokeTail = setPoint (*c, activeController, shapeStartBeat, downValue, a);
            strokeCount = 1;
        }
        else
        {
            mode = Mode::Draw;
            strokeTail = setPoint (*c, activeController, snappedBeat (downBeat), downValue, a);
            strokeCount = 1;
            owner.ensureClipCovers (*c, downBeat);
        }
        owner.changed();
        tip = String (downValue);
        tipPos = e.getPosition();
    }

    // batch selection: every anchor of this controller inside the rubber band (with Shift the
    // band adds to what was already selected)
    void applyCCMarquee (Rectangle<int> r, int controller, Rectangle<int> a)
    {
        auto* c = owner.clip();
        if (c == nullptr) return;
        owner.selCC = baseSelCC;
        const int maxV = maxValueFor (controller);
        for (auto& p : c->ccs)
        {
            if (p.cc != controller) continue;
            const int x = (int) std::lround (beatToX (p.beat));
            const int y = (int) std::lround (valueToY (p.value, a, maxV));
            if (r.contains (x, y)) owner.selCC.insert (p.id);
        }
    }

    void applyVelocity (const MouseEvent& e)
    {
        auto* c = owner.clip();
        if (c == nullptr) return;
        const int v = jlimit (1, 127, yToValue (e.y, activeRect.reduced (0, 8), 127));
        const int delta = v - velDownValue;
        for (auto& o : velOrigs)
            for (auto& n : c->notes)
                if (n.id == o.id) n.velocity = jlimit (1, 127, o.vel + delta);
        owner.lastVelocity = v;
        tip = String (v);
        tipPos = e.getPosition();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (mode == Mode::ResizePanel)
        {
            // measure the drag in screen pixels: the panel top moves while the component
            // itself grows, so component-local coordinates would lag the cursor
            const int dy = e.getScreenPosition().getY() - e.getMouseDownScreenPosition().getY();
            owner.resizeLanePanel (panelStartTotal - dy);
            return;
        }

        if (mode == Mode::ResizeLane)
        {
            // a splitter keeps the total height and moves space between the two adjacent lanes,
            // anchored exactly on the cursor even when the panel is squeezed
            const int n = laneCount();
            if (resizeIdx + 1 < n)
            {
                const int compH = jmax (1, getHeight());
                const int total = requestedTotal();
                int above = 0;
                for (int k = 0; k < resizeIdx; ++k) above += owner.laneHeightAt (k);
                int belowAfter = 0;
                for (int k = resizeIdx + 2; k < n; ++k) belowAfter += owner.laneHeightAt (k);
                int h = (int) std::round ((double) e.y * (double) total / (double) compH) - above;
                h = jlimit (40, total - 40 - above - belowAfter, h);
                owner.laneHeights[(size_t) resizeIdx] = h;
                owner.laneHeights[(size_t) resizeIdx + 1] = total - above - h - belowAfter;
                owner.resized();
                owner.lane->repaint();
            }
            return;
        }

        auto* c = owner.clip();
        if (c == nullptr) return;
        auto& proj = owner.app.project;
        const double beat = jmax (0.0, xToBeat (e.x));
        auto a = activeRect.reduced (0, 8);
        const int maxV = maxValueFor (activeController);
        const int value = yToValue (e.y, a, maxV);
        tipPos = e.getPosition();

        switch (mode)
        {
            case Mode::CCRubber:
            {
                marquee = Rectangle<int> (marqueeStart, e.getPosition());
                applyCCMarquee (marquee, activeController, a);
                repaint();
                return;
            }
            case Mode::Velocity:
                applyVelocity (e);
                break;
            case Mode::VelDraw:
            {
                if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
                const double lo = jmin (lastBeat, beat) - 1.0 / 64.0, hi = jmax (lastBeat, beat) + 1.0 / 64.0;
                for (auto& n : c->notes)
                    if (n.start >= lo && n.start <= hi)
                    {
                        const double f = hi > lo ? (n.start - lastBeat) / (beat - lastBeat + 1.0e-9) : 1.0;
                        const double vv = lastValue + (value - lastValue) * jlimit (0.0, 1.0, f);
                        n.velocity = jlimit (1, 127, (int) std::round (vv));
                    }
                tip = String (value);
                lastBeat = beat; lastValue = value;
                break;
            }
            case Mode::Draw:
            {
                const double step = anchorStep();
                const double from = snappedBeat (lastBeat), to = snappedBeat (beat);
                const double dir = to >= from ? 1.0 : -1.0;
                const int steps = jlimit (0, 8192, (int) std::round (std::abs (to - from) / step));
                for (int i = 0; i <= steps; ++i)
                {
                    const double b = from + dir * step * i;
                    const double f = steps > 0 ? (double) i / steps : 1.0;
                    if (b >= 0.0)
                    {
                        strokeTail = setPoint (*c, activeController, b, (int) std::round (lastValue + (value - lastValue) * f), a);
                        ++strokeCount;
                    }
                }
                owner.ensureClipCovers (*c, beat);
                tip = String (value);
                lastBeat = beat; lastValue = value;
                break;
            }
            case Mode::Shape:
            {
                // the curve tool rebuilds the shape from the stroke's start on every drag step
                c->ccs = shapeBase;
                applyShape (*c, a, shapeStartBeat, shapeStartValue, snappedBeat (beat), value);
                tip = String (value);
                lastBeat = beat; lastValue = value;
                break;
            }
            case Mode::Ramp:
            {
                c->ccs = rampBase;
                const double step = anchorStep();
                const double a0 = rampStartBeat, b0 = snappedBeat (beat);
                const double lo = jmin (a0, b0), hi = jmax (a0, b0);
                for (auto it = c->ccs.begin(); it != c->ccs.end();)
                {
                    if (it->cc == activeController && it->beat >= lo - 1.0e-6 && it->beat <= hi + 1.0e-6) it = c->ccs.erase (it);
                    else ++it;
                }
                const int steps = jlimit (1, 8192, (int) std::round ((hi - lo) / step));
                strokeCount = 0;
                for (int i = 0; i <= steps; ++i)
                {
                    const double bb = lo + (hi - lo) * i / steps;
                    const double f = hi > lo ? (bb - a0) / (b0 - a0) : 0.0;
                    CCPoint p;
                    p.id = proj.newId(); p.cc = activeController; p.beat = bb;
                    p.value = jlimit (0, maxV, (int) std::round (rampStartValue + (value - rampStartValue) * jlimit (0.0, 1.0, f)));
                    strokeTail = p.id;
                    ++strokeCount;
                    c->ccs.push_back (p);
                }
                owner.ensureClipCovers (*c, hi);
                tip = String (value);
                break;
            }
            case Mode::PointDrag:
            {
                if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
                const CCOrig* po = nullptr;
                for (auto& o : ccOrigs) if (o.id == ccPrimary) po = &o;
                if (po == nullptr) break;
                const double wanted = po->beat + (beat - downBeat);
                const double moved = (snapEnabled() && ! e.mods.isAltDown()) ? snapTo (wanted, owner.grid) : wanted;
                double dB = moved - po->beat;
                const int dV = value - downValue;
                double minB = 1.0e9;
                for (auto& o : ccOrigs) minB = jmin (minB, o.beat);
                dB = jmax (dB, -minB);
                for (auto& o : ccOrigs)
                    for (auto& p : c->ccs)
                        if (p.id == o.id)
                        {
                            p.beat = o.beat + dB;
                            p.value = jlimit (0, maxV, o.value + dV);
                        }
                tip = String (jlimit (0, maxV, po->value + dV));
                break;
            }
            case Mode::Erase:
            {
                const int hit = hitCC (e.getPosition(), activeController, a);
                if (hit >= 0)
                {
                    if (! checkpointed) { proj.checkpoint(); checkpointed = true; }
                    c->ccs.erase (c->ccs.begin() + hit);
                }
                break;
            }
            default: return;
        }
        owner.changed();
    }

    void mouseUp (const MouseEvent&) override
    {
        finishStroke();       // a bend stroke ends on the centre line
        mode = Mode::None;
        tip = {};
        marquee = {};
        repaint();
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (e.mods.isShiftDown())
            owner.gridViewport.setViewPosition (jmax (0, owner.scrollX() - (int) (w.deltaY * 300.0f)), owner.scrollY());
        else if (e.mods.isCtrlDown())
            owner.zoomHorizontal (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.x + owner.scrollX());
        else if (e.mods.isAltDown())
            owner.zoomVertical (w.deltaY > 0 ? 1 : -1, e.y);
    }

private:
    enum class Mode { None, Velocity, VelDraw, Draw, Shape, Ramp, PointDrag, Erase, ResizeLane, ResizePanel, CCRubber };
    struct VelOrig { int id; int vel; };
    struct CCOrig { int id; double beat; int value; };

    PianoRollView& owner;
    Mode mode = Mode::None;
    bool checkpointed = false;
    int activeLaneIdx = 0, activeController = -1;
    int resizeIdx = 0;
    int panelStartTotal = 0;
    Rectangle<int> activeRect;
    double downBeat = 0.0, lastBeat = 0.0, rampStartBeat = 0.0;
    int downValue = 0, lastValue = 0, rampStartValue = 0, velPrimary = 0, velDownValue = 0, ccPrimary = 0;
    std::vector<VelOrig> velOrigs;
    std::vector<CCOrig> ccOrigs;
    std::vector<CCPoint> rampBase, shapeBase;
    double shapeStartBeat = 0.0;
    int shapeStartValue = 0;
    int strokeTail = 0;                 // anchor the current stroke ended on (see finishStroke)
    int strokeCount = 0;                // anchors this stroke created (1 = a single click)
    Rectangle<int> marquee;             // rubber band over a controller lane (pointer tool)
    Point<int> marqueeStart;
    std::set<int> baseSelCC;            // selection a marquee drag started from
    String tip;
    Point<int> tipPos;
};

//==============================================================================
// left sidebar: lists every track that has a selected clip and switches the editable one
class PianoRollView::TrackList : public Component
{
public:
    explicit TrackList (PianoRollView& o) : owner (o) {}

    static constexpr int kRowH = 26;
    static constexpr int kHeaderH = 26;

    int rowAt (int y) const
    {
        if (y < kHeaderH) return -1;
        const int idx = (y - kHeaderH) / kRowH;
        return idx >= 0 && idx < (int) owner.ghosts.size() ? idx : -1;
    }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panelAlt);
        g.setColour (theme::line);
        g.fillRect (getWidth() - 1, 0, 1, getHeight());

        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        g.drawText (TRANS ("EDITABLE TRACK"), 10, 0, getWidth() - 20, kHeaderH, Justification::centredLeft);

        int y = kHeaderH;
        for (auto& gh : owner.ghosts)
        {
            auto* t = owner.app.project.findTrack (gh.trackId);
            if (t == nullptr) continue;
            const bool active = gh.trackId == owner.app.editTrack && gh.clipId == owner.app.editClip;
            auto row = Rectangle<int> (0, y, getWidth() - 1, kRowH);
            if (active)
            {
                g.setColour (theme::accentSoft);
                g.fillRect (row);
                g.setColour (theme::accent);
                g.fillRect (0, row.getY(), 3, row.getHeight());
            }
            g.setColour (t->colour);
            g.fillRoundedRectangle ((float) row.getX() + 9.0f, (float) row.getCentreY() - 4.0f, 8.0f, 8.0f, 2.0f);
            g.setColour (active ? theme::text : theme::textDim);
            g.setFont (theme::font (12.0f, active));
            auto* c = t->findMidi (gh.clipId);
            g.drawFittedText (c != nullptr ? c->name : t->name, row.getX() + 22, row.getY(), row.getWidth() - 28, row.getHeight(),
                              Justification::centredLeft, 1);
            y += kRowH;
        }

        if (owner.ghosts.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.5f));
            g.drawFittedText (TRANS ("Select MIDI clips in the arrangement to edit them here"),
                              Rectangle<int> (8, kHeaderH, getWidth() - 16, 60), Justification::topLeft, 3);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const int idx = rowAt (e.y);
        if (idx < 0) return;
        const auto gh = owner.ghosts[(size_t) idx];
        owner.setEditableClip (gh.trackId, gh.clipId);
    }

    void mouseMove (const MouseEvent& e) override
    {
        setMouseCursor (rowAt (e.y) >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
    }

private:
    PianoRollView& owner;
};

//==============================================================================
// The Humanize dialog: how far the velocity / the timing of the selected notes may wander.
// "Randomize" applies the amount at once, so the notes can be auditioned (play, listen, click
// again) without closing the dialog; every click draws a fresh set of deviations.
namespace
{
class HumanizePanel : public Component
{
public:
    HumanizePanel (const HumanizeSettings& initial, int selectedNotes, int clipNotes)
        : selected (selectedNotes), total (clipNotes)
    {
        auto label = [this] (Label& l, const String& t)
        {
            l.setText (t, dontSendNotification);
            l.setFont (theme::font (12.5f));
            l.setColour (Label::textColourId, theme::text);
            addAndMakeVisible (l);
        };
        label (velL, TRANS ("Velocity range"));
        label (timL, TRANS ("Timing (ticks)"));
        label (distL, TRANS ("Distribution"));

        vel.setRange (0.0, 64.0, 1.0);
        vel.setValue (jlimit (0, 64, initial.velocityRange), dontSendNotification);
        vel.setSliderStyle (Slider::LinearHorizontal);
        vel.setTextBoxStyle (Slider::TextBoxRight, false, 46, 22);
        vel.setTooltip (TRANS ("Largest velocity change a note may get (0 = leave the velocity alone)"));
        vel.onValueChange = [this] { updated(); };
        addAndMakeVisible (vel);

        tim.setRange (0.0, 240.0, 1.0);
        tim.setValue (jlimit (0, 240, initial.timingTicks), dontSendNotification);
        tim.setSliderStyle (Slider::LinearHorizontal);
        tim.setTextBoxStyle (Slider::TextBoxRight, false, 46, 22);
        tim.setTooltip (TRANS ("Largest timing change in MIDI ticks, 960 per beat (0 = keep the position)"));
        tim.onValueChange = [this] { updated(); };
        addAndMakeVisible (tim);

        dist.addItem (TRANS ("Bell curve (natural)"), 1);
        dist.addItem (TRANS ("Even spread (uniform)"), 2);
        dist.setSelectedId (initial.bell ? 1 : 2, dontSendNotification);
        dist.setTooltip (TRANS ("A bell keeps most notes close and lets a few move far; a uniform spread uses the whole range evenly"));
        addAndMakeVisible (dist);

        applyBtn.setButtonText (TRANS ("Randomize"));
        applyBtn.setTooltip (TRANS ("Apply the amount to the notes (each click draws a new set of deviations)"));
        applyBtn.onClick = [this] { if (onApply) onApply (settings()); };
        addAndMakeVisible (applyBtn);

        closeBtn.setButtonText (TRANS ("Close"));
        closeBtn.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeBtn);

        setSize (470, 292);
        updated();
    }

    std::function<void (const HumanizeSettings&)> onApply;
    std::function<void()> onClose;

    HumanizeSettings settings() const
    {
        HumanizeSettings s;
        s.velocityRange = (int) std::lround (vel.getValue());
        s.timingTicks = (int) std::lround (tim.getValue());
        s.bell = dist.getSelectedId() != 2;
        return s;
    }

    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);

        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (TRANS ("Humanize Velocity"), 16, 12, getWidth() - 32, 20, Justification::centredLeft);

        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawFittedText (targetText(), 16, 34, getWidth() - 32, 18, Justification::centredLeft, 1);

        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (TRANS ("Each note gets its own random change, so a pattern drawn at one fixed velocity stops sounding like a machine. The timing offset nudges notes off the grid so a block chord sounds strummed. Ctrl+Z undoes everything the dialog did."),
                          16, 176, getWidth() - 32, 54, Justification::topLeft, 3);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 0);
        r.removeFromTop (66);   // title and the "how many notes" line are painted
        auto row = [&r] (Label& l, Component& c)
        {
            auto rr = r.removeFromTop (26);
            l.setBounds (rr.removeFromLeft (132));
            c.setBounds (rr.withTrimmedLeft (8));
            r.removeFromTop (6);
        };
        row (velL, vel);
        row (timL, tim);
        row (distL, dist);

        auto buttons = getLocalBounds().reduced (16, 16).removeFromBottom (28);
        closeBtn.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        applyBtn.setBounds (buttons.removeFromRight (112));
    }

private:
    String targetText() const
    {
        if (selected <= 0)
            return TRANS ("Nothing selected - every note of the clip is humanised") + "  (" + String (total) + ")";
        return selected == 1 ? TRANS ("1 note selected")
                             : String (selected) + " " + TRANS ("notes selected");
    }

    void updated()
    {
        applyBtn.setEnabled (! settings().empty());
        repaint();
    }

    int selected = 0, total = 0;
    Label velL, timL, distL;
    Slider vel, tim;
    ComboBox dist;
    TextButton applyBtn, closeBtn;
};
}

//==============================================================================
PianoRollView::PianoRollView (AppState& a) : app (a)
{
    keys = std::make_unique<Keys> (*this);
    gridPtr = std::make_unique<Grid> (*this);
    lane = std::make_unique<Lane> (*this);
    timeRuler = std::make_unique<TimeRuler> (*this);
    chordStrip = std::make_unique<ChordStrip> (*this);

    gridViewport.setViewedComponent (gridPtr.get(), false);
    gridViewport.setScrollBarsShown (true, true);
    gridViewport.setScrollBarThickness (12);
    addAndMakeVisible (gridViewport);
    addAndMakeVisible (*keys);
    addAndMakeVisible (*lane);
    addAndMakeVisible (*timeRuler);
    addChildComponent (*chordStrip);

    for (auto* b : { &pointerBtn, &pencilBtn, &eraserBtn, &scissorsBtn }) { addAndMakeVisible (b); b->setClickingTogglesState (false); }
    pointerBtn.setTooltip (TRANS ("Pointer (1): click an anchor or drag a box to select; it never draws controller data"));
    pencilBtn.setTooltip (TRANS ("Pencil (2): draw notes and controller anchors (Shift+drag draws a ramp, quantize snaps them)"));
    eraserBtn.setTooltip (TRANS ("Eraser (3): click or drag to delete notes / controller anchors"));
    pointerBtn.onClick = [this] { setTool (Tool::Pointer); };
    pencilBtn.onClick = [this] { setTool (Tool::Pencil); };
    eraserBtn.onClick = [this] { setTool (Tool::Eraser); };
    scissorsBtn.setTooltip (TRANS ("Scissors (5): click a note to cut it in two at that point"));
    scissorsBtn.onClick = [this] { setTool (Tool::Scissors); };
    pointerBtn.setToggleState (true, dontSendNotification);

    int id = 1;
    for (auto& g : gridOptions()) gridBox.addItem (TRANS (g.name), id++);
    gridBox.setSelectedId (5, dontSendNotification);
    gridBox.setTooltip (TRANS ("Snap / grid resolution"));
    gridBox.onChange = [this]
    {
        grid = gridOptions()[(size_t) gridBox.getSelectedId() - 1].beats;
        repaint();
        gridComp()->repaint();
        lane->repaint();
    };
    addAndMakeVisible (gridBox);

    quantizeBtn.setTooltip (TRANS ("Quantize selected notes to the grid (Q)"));
    quantizeBtn.onClick = [this] { quantizeSelection(); };
    addAndMakeVisible (quantizeBtn);

    humanizeBtn.setClickingTogglesState (false);
    humanizeBtn.onClick = [this] { showHumanizeDialog(); };
    addAndMakeVisible (humanizeBtn);

    slideBtn.setClickingTogglesState (true);
    slideBtn.setTooltip (TRANS ("Slide notes: new notes glide from the previous pitch (FL-style)"));
    slideBtn.onClick = [this]
    {
        slideMode = slideBtn.getToggleState();
        setSlideOnSelection (slideMode);
    };
    addAndMakeVisible (slideBtn);

    laneBtn.setTooltip (TRANS ("Choose the lower editor: Velocity or controller (CC) data"));
    laneBtn.onClick = [this] { showLaneMenu(); };
    addAndMakeVisible (laneBtn);

    // what the pencil draws into a controller lane: by hand, or one of the ready-made shapes
    shapeBtn.onClick = [this] { showCurveShapeMenu(); };
    addAndMakeVisible (shapeBtn);
    setCurveShape (CurveShape::Free);

    zoomInBtn.onClick = [this] { zoomHorizontal (1.3); };
    zoomOutBtn.onClick = [this] { zoomHorizontal (1.0 / 1.3); };
    addAndMakeVisible (zoomInBtn);
    addAndMakeVisible (zoomOutBtn);

    zoomOutBtn.setTooltip (TRANS ("Zoom Out (horizontal)"));
    vZoomInBtn.onClick = [this] { zoomVertical (2); };
    vZoomOutBtn.onClick = [this] { zoomVertical (-2); };
    addAndMakeVisible (vZoomInBtn);
    addAndMakeVisible (vZoomOutBtn);

    fullscreenBtn.setClickingTogglesState (false);
    fullscreenBtn.onClick = [this] { if (onFullscreenToggle) onFullscreenToggle(); };
    addAndMakeVisible (fullscreenBtn);

    // the chord row switch (chords from the chord track / detected from the notes)
    chordBtn.setClickingTogglesState (false);
    chordBtn.setToggleState (app.pianoRollChords, dontSendNotification);
    chordBtn.onClick = [this]
    {
        app.pianoRollChords = ! app.pianoRollChords;
        chordBtn.setToggleState (app.pianoRollChords, dontSendNotification);
        resized();
        repaint();
    };
    addAndMakeVisible (chordBtn);

    // left sidebar: pick which of the selected tracks is the editable one
    trackList = std::make_unique<TrackList> (*this);
    addAndMakeVisible (*trackList);
    trackListBtn.setClickingTogglesState (false);
    trackListBtn.setToggleState (trackListVisible, dontSendNotification);
    trackListBtn.onClick = [this]
    {
        trackListVisible = ! trackListVisible;
        trackListBtn.setToggleState (trackListVisible, dontSendNotification);
        resized();
        repaint();
    };
    addAndMakeVisible (trackListBtn);

    updateLaneButton();
    app.project.addListener (this);
    app.addListener (this);
    rebuildTrackList();
    startTimerHz (30);
}

PianoRollView::~PianoRollView()
{
    app.removeListener (this);
    app.project.removeListener (this);
    gridViewport.setViewedComponent (nullptr, false);
}

MidiClip* PianoRollView::clip() const
{
    auto* t = app.project.findTrack (app.editTrack);
    return t != nullptr ? t->findMidi (app.editClip) : nullptr;
}

//==============================================================================
// chord row content
String PianoRollView::chordNameAt (double absBeat) const
{
    for (auto& s : buildChordSegments (absBeat, absBeat + 0.01))
        if (s.name.isNotEmpty() && s.beat <= absBeat + 1.0e-9 && s.beat + s.length > absBeat + 1.0e-9)
            return s.name;
    return {};
}

std::vector<PianoRollView::ChordSegment> PianoRollView::buildChordSegments (double fromAbs, double toAbs) const
{
    std::vector<ChordSegment> out;
    auto& proj = app.project;
    toAbs = jmin (toAbs, fromAbs + 2048.0);          // never walk an absurd range
    if (toAbs <= fromAbs) return out;

    const auto notes = notesInRange (proj, fromAbs, toAbs);
    if (notes.empty()) return out;

    // The set of sounding notes only changes when a note starts or ends, so every interval between
    // two such moments carries exactly one chord: the row's blocks therefore follow the notes and
    // change exactly where the harmony changes (no grid, no merging of two chords into one).
    struct Bound { double beat = 0.0; bool start = false; int pitch = 60; };
    std::vector<Bound> bounds;
    bounds.reserve (notes.size() * 2);
    for (auto& n : notes)
    {
        const double s = jmax (n.start, fromAbs);
        const double e = jmin (n.start + n.length, toAbs);
        if (e - s <= 1.0e-6) continue;
        bounds.push_back ({ s, true, n.pitch });
        bounds.push_back ({ e, false, n.pitch });
    }
    if (bounds.empty()) return out;
    std::sort (bounds.begin(), bounds.end(), [] (const Bound& a, const Bound& b)
    {
        if (std::abs (a.beat - b.beat) > 1.0e-9) return a.beat < b.beat;
        return (int) a.start < (int) b.start;        // a note that ends frees its slot first
    });

    std::multiset<int> sounding;                     // the pitches that actually sound
    int pcCount[12] = {};
    auto makeSegment = [&] (double a, double b)
    {
        ChordSegment s;
        s.beat = a;
        s.length = b - a;
        if (sounding.empty()) return s;              // a rest: nothing to name
        s.sounding = true;
        std::vector<std::pair<int, double>> weighted;
        for (int pc = 0; pc < 12; ++pc)
            if (pcCount[pc] > 0) weighted.push_back ({ pc, 1.0 });
        const auto found = chord::detectWeighted (weighted, *sounding.begin() % 12);
        s.name = found.valid ? found.name : String();
        return s;
    };

    size_t k = 0;
    double segStart = fromAbs;
    while (k < bounds.size())
    {
        const double beat = bounds[k].beat;
        if (beat > segStart + 1.0e-9) out.push_back (makeSegment (segStart, beat));
        // apply every note event that sits on this beat, then measure the interval up to the next
        while (k < bounds.size() && std::abs (bounds[k].beat - beat) <= 1.0e-9)
        {
            const auto& bd = bounds[k++];
            if (bd.start) { sounding.insert (bd.pitch); ++pcCount[bd.pitch % 12]; }
            else
            {
                const auto it = sounding.find (bd.pitch);
                if (it != sounding.end()) sounding.erase (it);
                if (pcCount[bd.pitch % 12] > 0) --pcCount[bd.pitch % 12];
            }
        }
        segStart = beat;
    }
    if (toAbs > segStart + 1.0e-9) out.push_back (makeSegment (segStart, toAbs));

    // a short harmony squeezed between the same chord on both sides is an ornament, not a change
    for (size_t i = 1; i + 1 < out.size(); ++i)
        if (out[i].sounding && out[i].name.isNotEmpty() && out[i].length < 1.0
            && out[i - 1].name.isNotEmpty() && out[i - 1].name == out[i + 1].name
            && out[i].name != out[i - 1].name)
            out[i].name = out[i - 1].name;

    // notes that are no chord of their own (a single bass note before the chord) take the name of
    // the chord next to them
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].sounding && out[i].name.isEmpty() && i > 0 && ! out[i - 1].name.isEmpty())
            out[i].name = out[i - 1].name;
    for (size_t i = out.size(); i-- > 0;)
        if (out[i].sounding && out[i].name.isEmpty() && i + 1 < out.size() && ! out[i + 1].name.isEmpty())
            out[i].name = out[i + 1].name;

    // merge neighbours that name the same chord, so a held chord is one block
    std::vector<ChordSegment> merged;
    for (auto& s : out)
    {
        if (! merged.empty() && merged.back().sounding == s.sounding && merged.back().name == s.name
            && std::abs (merged.back().beat + merged.back().length - s.beat) < 1.0e-6)
        {
            merged.back().length += s.length;
            continue;
        }
        merged.push_back (s);
    }
    return merged;
}

void PianoRollView::rebuildTrackList()
{
    ghosts.clear();
    auto& proj = app.project;
    for (int id : app.selectedClips)
        for (auto& t : proj.tracks)
            if (t->findMidi (id) != nullptr) { ghosts.push_back ({ t->id, id }); break; }

    // the editable clip must always be listed, even if the selection is elsewhere
    bool hasEditable = false;
    for (auto& gh : ghosts) if (gh.trackId == app.editTrack && gh.clipId == app.editClip) hasEditable = true;
    if (! hasEditable && app.editClip >= 0 && proj.findTrack (app.editTrack) != nullptr)
        ghosts.insert (ghosts.begin(), { app.editTrack, app.editClip });

    if (trackList != nullptr) trackList->repaint();
}

void PianoRollView::setEditableClip (int trackId, int clipId)
{
    if (trackId == app.editTrack && clipId == app.editClip) return;
    app.editTrack = trackId;
    app.editClip = clipId;
    selNotes.clear();
    selCC.clear();
    rebuildTrackList();
    updateContentSize();
    repaint();
    if (gridComp() != nullptr) gridComp()->repaint();
    if (lane != nullptr) lane->repaint();
}

void PianoRollView::selectionChanged()
{
    rebuildTrackList();
    if (gridComp() != nullptr) gridComp()->repaint();
    repaint();
}

void PianoRollView::ensureClipCovers (MidiClip& c, double endBeat)
{
    if (endBeat <= c.length) return;
    // extend to the end of the bar containing endBeat (meter-aware)
    auto& tempo = app.project.tempo;
    const int bar = tempomap::barPosition (tempo, c.start + endBeat).bar;
    const double nextBar = tempomap::beatOfBar (tempo, bar + 1);
    c.length = jmax (endBeat, nextBar - c.start);
}

void PianoRollView::changed() { app.project.sendContentChanged(); }

void PianoRollView::laneChanged() { lane->repaint(); }

void PianoRollView::setTool (Tool t)
{
    tool = t;
    pointerBtn.setToggleState (t == Tool::Pointer, dontSendNotification);
    pencilBtn.setToggleState (t == Tool::Pencil, dontSendNotification);
    eraserBtn.setToggleState (t == Tool::Eraser, dontSendNotification);
    scissorsBtn.setToggleState (t == Tool::Scissors, dontSendNotification);
    if (lane != nullptr) lane->repaint();       // the empty-lane hint and the cursor follow the tool
    updateLaneButton();
}

void PianoRollView::splitNoteAt (int noteIndex, double cutBeat)
{
    auto* c = clip();
    if (c == nullptr || noteIndex < 0 || noteIndex >= (int) c->notes.size()) return;

    auto& proj = app.project;
    Note& n = c->notes[(size_t) noteIndex];
    const double end = n.start + n.length;
    if (cutBeat <= n.start + 1.0e-6 || cutBeat >= end - 1.0e-6) return;

    proj.checkpoint();
    const double leftLen = cutBeat - n.start;
    Note right = n;
    right.id = proj.newId();
    right.start = cutBeat;
    right.length = end - cutBeat;
    n.length = leftLen;                       // the left half keeps the original id
    c->notes.push_back (right);
    selNotes = { n.id, right.id };
    changed();
}

bool PianoRollView::hasLane (int controller) const
{
    for (int cc : laneControllers) if (cc == controller) return true;
    return false;
}

int PianoRollView::lanePanelTotal() const
{
    int s = 0;
    for (int i = 0; i < (int) laneHeights.size(); ++i) s += laneHeights[(size_t) i];
    return jmax (40 * (int) jmax (1, (int) laneHeights.size()), s);
}

int PianoRollView::trackListWidth() const
{
    return trackListVisible ? jmin (150, jmax (90, getWidth() / 5)) : 0;
}

int PianoRollView::lanePanelCap() const
{
    // the CC panel may take up to 80% of the area below the toolbar
    return jmax (kLaneH / 2, (getHeight() - kToolbarH) * 4 / 5);
}

void PianoRollView::resizeLanePanel (int newTotal)
{
    const int n = jmax (1, (int) laneHeights.size());
    newTotal = jlimit (40 * n, lanePanelCap(), newTotal);
    const int oldTotal = lanePanelTotal();
    if (oldTotal == newTotal) return;

    const double ratio = (double) newTotal / (double) jmax (1, oldTotal);
    int acc = 0;
    for (int i = 0; i < n; ++i)
    {
        int h = (int) std::round (laneHeightAt (i) * ratio);
        if (i == n - 1) h = jmax (40, newTotal - acc);   // last lane takes the remainder exactly
        else h = jmax (40, h);
        laneHeights[(size_t) i] = h;
        acc += h;
    }
    resized();
    lane->repaint();
}

void PianoRollView::setLaneHeight (int index, int h)
{
    if (index < 0 || index >= (int) laneHeights.size()) return;
    h = jlimit (40, 600, h);
    if (laneHeights[(size_t) index] == h) return;
    laneHeights[(size_t) index] = h;
    resized();
    lane->repaint();
}

void PianoRollView::toggleLane (int controller)
{
    // keep (controller, height) pairs together while reordering
    std::vector<std::pair<int, int>> pairs;
    for (size_t i = 0; i < laneControllers.size(); ++i)
        pairs.push_back ({ laneControllers[i], laneHeightAt ((int) i) });

    auto it = std::find_if (pairs.begin(), pairs.end(), [controller] (const std::pair<int, int>& p) { return p.first == controller; });
    if (it != pairs.end())
    {
        if (pairs.size() > 1) pairs.erase (it);   // keep at least one lane
    }
    else
    {
        pairs.push_back ({ controller, kLaneH });
        std::sort (pairs.begin(), pairs.end(), [] (const std::pair<int, int>& a, const std::pair<int, int>& b)
                   { return (a.first < 0 ? -1 : a.first) < (b.first < 0 ? -1 : b.first); });   // velocity first
    }

    laneControllers.clear();
    laneHeights.clear();
    for (auto& p : pairs) { laneControllers.push_back (p.first); laneHeights.push_back (p.second); }
    selCC.clear();
    updateLaneButton();
    resized();
    lane->repaint();
}

void PianoRollView::updateLaneButton()
{
    StringArray names;
    for (int cc : laneControllers) names.add (cc < 0 ? TRANS ("Velocity") : laneName (cc));
    laneBtn.setButtonText (names.joinIntoString (", ") + "  v");
}

void PianoRollView::setCurveShape (CurveShape s)
{
    curveShape = s;
    shapeBtn.setButtonText (curveShapeName (s) + "  v");
    shapeBtn.setTooltip (TRANS ("Curve tool: with the pencil, drag in a controller lane to draw the selected shape")
                         + " (" + curveShapeName (CurveShape::Line) + " / " + curveShapeName (CurveShape::Sine) + " / "
                         + curveShapeName (CurveShape::Triangle) + " / " + curveShapeName (CurveShape::Saw) + ")");
}

void PianoRollView::showCurveShapeMenu()
{
    PopupMenu m;
    int id = 1;
    for (auto s : { CurveShape::Free, CurveShape::Line, CurveShape::Sine, CurveShape::Triangle, CurveShape::Saw })
        m.addItem (id++, curveShapeName (s), true, curveShape == s);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&shapeBtn), [this] (int r)
    {
        if (r <= 0) return;
        static const CurveShape shapes[] { CurveShape::Free, CurveShape::Line, CurveShape::Sine, CurveShape::Triangle, CurveShape::Saw };
        if (r <= (int) std::size (shapes)) setCurveShape (shapes[r - 1]);
    });
}

void PianoRollView::showLaneMenu()
{
    auto* c = clip();
    PopupMenu m;
    for (auto& o : kLaneOptions)
    {
        bool has = false;
        if (c != nullptr && o.cc >= 0) for (auto& p : c->ccs) if (p.cc == o.cc) { has = true; break; }
        m.addItem (1000 + o.cc + 1, TRANS (o.name) + (has ? "  *" : ""), true, hasLane (o.cc));
    }
    PopupMenu all;
    for (int cc = 0; cc < 128; ++cc)
    {
        bool has = false;
        if (c != nullptr) for (auto& p : c->ccs) if (p.cc == cc) { has = true; break; }
        String name = "CC" + String (cc);
        const auto cn = controllerName (cc);
        if (cn != TRANS ("Controller")) name += "  " + cn;
        all.addItem (1000 + cc + 1, name + (has ? "  *" : ""), true, hasLane (cc));
    }
    m.addSeparator();
    m.addSubMenu (TRANS ("All Controllers"), all);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (&laneBtn), [this] (int r)
    {
        if (r < 1000) return;
        toggleLane (r - 1001);
    });
}

void PianoRollView::fitToNotes()
{
    auto* c = clip();
    int lo = 60, hi = 72;
    if (c != nullptr && ! c->notes.empty())
    {
        lo = 127; hi = 0;
        for (auto& n : c->notes) { lo = jmin (lo, n.pitch); hi = jmax (hi, n.pitch); }
    }
    const int mid = (lo + hi) / 2;
    const int y = (127 - mid) * keyH - gridViewport.getMaximumVisibleHeight() / 2;
    gridViewport.setViewPosition (0, jmax (0, y));
}

void PianoRollView::editorClipChanged()
{
    selNotes.clear();
    selCC.clear();
    updateContentSize();
    fitToNotes();
    repaint();
    gridComp()->repaint();
    lane->repaint();
}

void PianoRollView::projectContentChanged()
{
    auto* c = clip();
    // drop selections of items that no longer exist
    if (c != nullptr)
    {
        std::set<int> ids, ccIds;
        for (auto& n : c->notes) ids.insert (n.id);
        for (auto& p : c->ccs) ccIds.insert (p.id);
        for (auto it = selNotes.begin(); it != selNotes.end();) it = ids.count (*it) ? std::next (it) : selNotes.erase (it);
        for (auto it = selCC.begin(); it != selCC.end();) it = ccIds.count (*it) ? std::next (it) : selCC.erase (it);
    }
    else { selNotes.clear(); selCC.clear(); }
    rebuildTrackList();   // tracks / clips may have been added or removed
    updateContentSize();
    gridComp()->repaint();
    lane->repaint();
    timeRuler->repaint();
    if (chordStrip != nullptr) chordStrip->repaint();   // the chord row follows edits to the notes
    repaint();
}

void PianoRollView::viewChanged()
{
    resized();
    repaint();
    if (chordStrip != nullptr)
    {
        chordBtn.setToggleState (app.pianoRollChords, dontSendNotification);
        chordStrip->repaint();
    }
    timeRuler->repaint();
}

void PianoRollView::updateContentSize()
{
    auto* c = clip();
    auto& tempo = app.project.tempo;
    const double origin = c != nullptr ? c->start : 0.0;
    const int endBar = tempomap::barPosition (tempo, c != nullptr ? c->start + c->length : 0.0).bar + 3;
    const double beats = jmax (tempomap::beatOfBar (tempo, jmax (4, endBar)) - origin, 16.0);
    const int w = jmax ((int) (beats * pxPerBeat), gridViewport.getMaximumVisibleWidth());
    const int h = 128 * keyH;
    if (gridComp()->getWidth() != w || gridComp()->getHeight() != h) gridComp()->setSize (w, h);
}

void PianoRollView::timerCallback()
{
    if (! isShowing()) return;
    if (lastScrollX != scrollX() || lastScrollY != scrollY())
    {
        lastScrollX = scrollX(); lastScrollY = scrollY();
        keys->repaint(); lane->repaint(); timeRuler->repaint();
        if (chordStrip != nullptr) chordStrip->repaint();   // the chord row scrolls with the notes
    }
    auto* c = clip();
    const double origin = c != nullptr ? c->start : 0.0;
    const juce::int64 x = (juce::int64) std::llround ((app.engine.getPositionBeats() - origin) * pxPerBeat);
    // publish the position for the paint BEFORE invalidating: the grid draws exactly this x,
    // which is the centre of the columns repainted below
    playXForPaint = x;
    if (x != lastPlayX)
    {
        auto old = lastPlayX;
        lastPlayX = x;
        gridComp()->repaint ((int) old - 2, 0, 6, gridComp()->getHeight());
        gridComp()->repaint ((int) x - 2, 0, 6, gridComp()->getHeight());
        lane->repaint();
        timeRuler->repaint();

        // follow the playhead while playing (same toggle as the arrangement)
        if (app.engine.isPlaying() && app.followPlayhead)
        {
            const int vw = gridViewport.getMaximumVisibleWidth();
            const int sx = gridViewport.getViewPositionX();
            if (x < sx || x > sx + vw - 40)
                gridViewport.setViewPosition (jmax (0, (int) x - vw / 8), gridViewport.getViewPositionY());
        }
    }
}

void PianoRollView::setFullscreen (bool f)
{
    fullscreenBtn.setToggleState (f, dontSendNotification);
    fullscreenBtn.setTooltip (TRANS (f ? "Exit Fullscreen (F11)" : "Fullscreen (F11)"));
}

void PianoRollView::zoomHorizontal (double factor, int anchorX)
{
    const double newPpb = jlimit (12.0, 600.0, pxPerBeat * factor);
    if (newPpb == pxPerBeat) return;

    int ax;
    if (anchorX >= 0)
        ax = anchorX;
    else
    {
        // keep the playhead still while zooming when it is on screen, else anchor at the view centre
        auto* c = clip();
        const double origin = c != nullptr ? c->start : 0.0;
        const int px = (int) std::llround ((app.engine.getPositionBeats() - origin) * pxPerBeat);
        if (px >= scrollX() && px <= scrollX() + gridViewport.getMaximumVisibleWidth())
            ax = px;
        else
            ax = scrollX() + gridViewport.getMaximumVisibleWidth() / 2;
    }

    const double beatAtAnchor = (double) ax / pxPerBeat;
    const int screenX = ax - scrollX();
    pxPerBeat = newPpb;
    resized();
    gridViewport.setViewPosition (jmax (0, (int) std::round (beatAtAnchor * pxPerBeat) - screenX), scrollY());
}

void PianoRollView::zoomVertical (int delta, int anchorY)
{
    const int newKeyH = jlimit (8, 28, keyH + delta);
    if (newKeyH == keyH) return;

    const int viewH = jmax (1, gridViewport.getMaximumVisibleHeight());
    int anchorPitch = -1, ay = 0;

    if (anchorY >= 0)
    {
        // keep the key row under the mouse still while zooming
        ay = jlimit (0, viewH - 1, anchorY);
        anchorPitch = jlimit (0, 127, 127 - (scrollY() + ay) / keyH);
    }
    else
    {
        // keep the selected notes still, falling back to the centre of the view
        auto* c = clip();
        if (c != nullptr && ! selNotes.empty())
        {
            int lo = 127, hi = 0;
            for (auto& n : c->notes) if (selNotes.count (n.id)) { lo = jmin (lo, n.pitch); hi = jmax (hi, n.pitch); }
            if (lo <= hi)
            {
                anchorPitch = (lo + hi) / 2;
                ay = jlimit (0, viewH - 1, (127 - anchorPitch) * keyH - scrollY());
            }
        }
        if (anchorPitch < 0)
        {
            ay = viewH / 2;
            anchorPitch = jlimit (0, 127, 127 - (scrollY() + ay) / keyH);
        }
    }

    keyH = newKeyH;
    resized();
    gridViewport.setViewPosition (gridViewport.getViewPositionX(), jmax (0, (127 - anchorPitch) * keyH - ay));
    gridComp()->repaint();
    keys->repaint();
    lane->repaint();
}

void PianoRollView::resized()
{
    auto r = getLocalBounds();
    auto tb = r.removeFromTop (kToolbarH).reduced (6, 4);
    pointerBtn.setBounds (tb.removeFromLeft (26)); tb.removeFromLeft (2);
    pencilBtn.setBounds (tb.removeFromLeft (26)); tb.removeFromLeft (2);
    eraserBtn.setBounds (tb.removeFromLeft (26)); tb.removeFromLeft (2);
    scissorsBtn.setBounds (tb.removeFromLeft (26)); tb.removeFromLeft (8);
    trackListBtn.setBounds (tb.removeFromLeft (26)); tb.removeFromLeft (8);
    gridBox.setBounds (tb.removeFromLeft (76)); tb.removeFromLeft (8);
    quantizeBtn.setBounds (tb.removeFromLeft (76)); tb.removeFromLeft (4);
    humanizeBtn.setBounds (tb.removeFromLeft (26)); tb.removeFromLeft (4);
    slideBtn.setBounds (tb.removeFromLeft (54)); tb.removeFromLeft (16);

    fullscreenBtn.setBounds (tb.removeFromRight (26)); tb.removeFromRight (8);
    vZoomInBtn.setBounds (tb.removeFromRight (26)); tb.removeFromRight (2);
    vZoomOutBtn.setBounds (tb.removeFromRight (26)); tb.removeFromRight (8);
    zoomInBtn.setBounds (tb.removeFromRight (26)); tb.removeFromRight (2);
    zoomOutBtn.setBounds (tb.removeFromRight (26));
    tb.removeFromRight (6);
    chordBtn.setBounds (tb.removeFromLeft (26)); tb.removeFromLeft (6);
    laneBtn.setBounds (tb.removeFromLeft (jmax (0, jmin (190, tb.getWidth())))); tb.removeFromLeft (6);
    shapeBtn.setBounds (tb.removeFromLeft (jmax (0, jmin (96, tb.getWidth()))));

    const int laneAreaH = jmin (lanePanelTotal(), lanePanelCap());
    auto laneArea = r.removeFromBottom (laneAreaH);

    // the editable-track sidebar sits on the far left, spanning the whole editor height
    if (trackListVisible)
    {
        auto listArea = r.removeFromLeft (trackListWidth());
        trackList->setBounds (listArea);
        trackList->setVisible (true);
    }
    else
    {
        trackList->setVisible (false);
        trackList->setBounds ({});
    }

    // the lane starts where the note grid starts (right of the sidebar AND the key column), so its
    // bar lines line up with the arrangement above it
    lane->setBounds (laneArea.withTrimmedLeft (r.getX() + kKeysW));
    // the chord row sits above the ruler and spans the whole editor width, so its label cell is
    // lined up with the keys column
    chordStrip->setVisible (chordStripShown());
    if (chordStripShown())
        chordStrip->setBounds (r.removeFromTop (kChordStripH));
    auto rulerRow = r.removeFromTop (kRulerH);
    timeRuler->setBounds (rulerRow.withTrimmedLeft (kKeysW));
    keys->setBounds (r.removeFromLeft (kKeysW));
    gridViewport.setBounds (r);
    updateContentSize();
    repaint();
}

void PianoRollView::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::line);
    g.fillRect (0, kToolbarH - 1, getWidth(), 1);

    auto* c = clip();
    g.setColour (theme::text);
    g.setFont (theme::font (12.5f, true));
    if (c != nullptr)
        g.drawText (c->name, getWidth() - 520, 0, 280, kToolbarH, Justification::centredRight);

    // left cell of ruler and the lane label column (they start where the editor's content starts,
    // so the lane's beat lines sit exactly under the bar lines of the note grid)
    auto lr = Rectangle<int> (trackListWidth(), getHeight() - lane->getHeight(), kKeysW, lane->getHeight());
    g.setColour (theme::panelAlt);
    g.fillRect (lr);
    g.setColour (theme::line);
    g.fillRect (lr.getRight() - 1, lr.getY(), 1, lr.getHeight());
    g.fillRect (lr.getX(), lr.getY(), lr.getWidth(), 1);
    const int nLanes = jmax (1, (int) laneControllers.size());
    int totalH = 0;
    for (int i = 0; i < nLanes; ++i) totalH += laneHeightAt (i);
    totalH = jmax (1, totalH);
    int above = 0;
    for (int i = 0; i < nLanes; ++i)
    {
        const int h = laneHeightAt (i);
        const int top = lr.getY() + lr.getHeight() * above / totalH;
        const int bottom = lr.getY() + lr.getHeight() * (above + h) / totalH;
        auto strip = Rectangle<int> (lr.getX(), top, lr.getWidth(), jmax (1, bottom - top));
        above += h;
        if (i > 0)
        {
            g.setColour (theme::line);
            g.fillRect (strip.getX(), strip.getY(), strip.getWidth(), 1);
        }
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        const int cc = laneControllers[(size_t) jlimit (0, nLanes - 1, i)];
        g.drawFittedText (cc < 0 ? TRANS ("Velocity") : laneName (cc), strip.reduced (4, 4), Justification::centredTop, 3);
    }

    g.setColour (theme::panelAlt);
    g.fillRect (0, rulerTop(), kKeysW, kRulerH);
    g.setColour (theme::line);
    g.fillRect (kKeysW - 1, rulerTop(), 1, kRulerH);
    g.fillRect (0, rulerTop() + kRulerH - 1, kKeysW, 1);
}

//==============================================================================
void PianoRollView::deleteSelection()
{
    auto* c = clip();
    if (c == nullptr) return;
    if (selNotes.empty() && selCC.empty()) return;
    app.project.checkpoint();
    c->notes.erase (std::remove_if (c->notes.begin(), c->notes.end(), [this] (const Note& n) { return selNotes.count (n.id) > 0; }), c->notes.end());
    c->ccs.erase (std::remove_if (c->ccs.begin(), c->ccs.end(), [this] (const CCPoint& p) { return selCC.count (p.id) > 0; }), c->ccs.end());
    selNotes.clear();
    selCC.clear();
    changed();
}

void PianoRollView::selectAll()
{
    auto* c = clip();
    if (c == nullptr) return;
    selNotes.clear();
    selCC.clear();
    if (hasLane (-1)) for (auto& n : c->notes) selNotes.insert (n.id);
    for (int cc : laneControllers)
        if (cc >= 0) for (auto& p : c->ccs) if (p.cc == cc) selCC.insert (p.id);
    if (selNotes.empty() && selCC.empty()) for (auto& n : c->notes) selNotes.insert (n.id);
    repaint();
    gridComp()->repaint();
    lane->repaint();
}

void PianoRollView::quantizeSelection()
{
    auto* c = clip();
    if (c == nullptr || grid <= 0.0) return;
    app.project.checkpoint();
    for (auto& n : c->notes)
        if (selNotes.empty() || selNotes.count (n.id))
        {
            n.start = jmax (0.0, snapTo (n.start, grid));
            n.length = jmax (grid * 0.25, n.length);
        }
    changed();
}

//==============================================================================
// Humanize: random deviations on the selected notes, so a pattern drawn at one fixed velocity
// starts to sound played. Everything the dialog does counts as one gesture: the first Randomize
// click pushes the undo step, later clicks only re-draw from the values the notes had when the
// dialog opened (see HumanizeSession).
PianoRollView::HumanizeSession PianoRollView::captureHumanizeSession() const
{
    HumanizeSession session;
    if (auto* c = clip()) humanize::capture (c->notes, session.seed);
    return session;
}

int PianoRollView::applyHumanize (const HumanizeSettings& s, HumanizeSession& session)
{
    auto* c = clip();
    if (c == nullptr || s.empty()) return 0;

    // with nothing selected the whole clip is humanised, exactly like Quantize
    int targets = 0;
    for (auto& n : c->notes) if (selNotes.empty() || selNotes.count (n.id) > 0) ++targets;
    if (targets == 0) return 0;

    if (! session.checkpointed) { app.project.checkpoint(); session.checkpointed = true; }
    const int changedNotes = humanize::apply (c->notes, selNotes, s, humanizeRng, &session.seed);
    changed();
    return changedNotes;
}

void PianoRollView::humanizeSelection (const HumanizeSettings& s)
{
    auto session = captureHumanizeSession();
    applyHumanize (s, session);
}

void PianoRollView::showHumanizeDialog()
{
    auto* c = clip();
    if (c == nullptr) return;

    int selected = 0;
    for (auto& n : c->notes) if (selNotes.count (n.id) > 0) ++selected;

    auto* panel = new HumanizePanel (app.humanize, selected, (int) c->notes.size());
    DialogWindow::LaunchOptions lo;
    lo.dialogTitle = TRANS ("Humanize Velocity");
    lo.content.setOwned (panel);
    lo.dialogBackgroundColour = theme::panel;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    auto* dw = lo.launchAsync();

    Component::SafePointer<PianoRollView> self (this);
    auto session = std::make_shared<HumanizeSession> (captureHumanizeSession());
    panel->onApply = [self, session] (const HumanizeSettings& s)
    {
        if (self.getComponent() == nullptr) return;
        self->app.humanize = s;                  // the H shortcut applies the amount used here
        self->applyHumanize (s, *session);
    };
    panel->onClose = [dw] { dw->exitModalState (0); };
}

Image renderHumanizePanel (const HumanizeSettings& s, int selectedNotes, int clipNotes)
{
    HumanizePanel panel (s, selectedNotes, clipNotes);
    return panel.createComponentSnapshot (panel.getLocalBounds(), true);
}

void PianoRollView::copySelection()
{
    auto* c = clip();
    if (c == nullptr) return;
    noteClipboard.clear();
    ccClipboard.clear();
    for (auto& n : c->notes) if (selNotes.count (n.id)) noteClipboard.push_back (n);
    for (auto& p : c->ccs) if (selCC.count (p.id)) ccClipboard.push_back (p);
}

void PianoRollView::pasteAtPlayhead()
{
    auto* c = clip();
    if (c == nullptr || (noteClipboard.empty() && ccClipboard.empty())) return;
    double lo = 1.0e9;
    for (auto& n : noteClipboard) lo = jmin (lo, n.start);
    for (auto& p : ccClipboard) lo = jmin (lo, p.beat);
    const double at = jmax (0.0, snapTo (app.engine.getPositionBeats() - c->start, grid));
    app.project.checkpoint();
    selNotes.clear();
    selCC.clear();
    double maxEnd = 0.0;
    for (auto n : noteClipboard)
    {
        n.id = app.project.newId(); n.start = at + (n.start - lo);
        maxEnd = jmax (maxEnd, n.start + n.length);
        selNotes.insert (n.id); c->notes.push_back (n);
    }
    for (auto p : ccClipboard)
    {
        p.id = app.project.newId(); p.beat = at + (p.beat - lo);
        maxEnd = jmax (maxEnd, p.beat);
        selCC.insert (p.id); c->ccs.push_back (p);
    }
    ensureClipCovers (*c, maxEnd);
    changed();
}

void PianoRollView::duplicateSelection()
{
    auto* c = clip();
    if (c == nullptr || selNotes.empty()) return;
    double lo = 1.0e9, hi = 0.0;
    for (auto& n : c->notes) if (selNotes.count (n.id)) { lo = jmin (lo, n.start); hi = jmax (hi, n.start + n.length); }
    const double shift = hi - lo;
    app.project.checkpoint();
    std::vector<Note> add;
    for (auto n : c->notes)
        if (selNotes.count (n.id)) { n.id = app.project.newId(); n.start += shift; add.push_back (n); }
    selNotes.clear();
    for (auto& n : add) { selNotes.insert (n.id); c->notes.push_back (n); }
    ensureClipCovers (*c, hi + shift);
    changed();
}

void PianoRollView::transposeSelection (int semitones)
{
    auto* c = clip();
    if (c == nullptr || selNotes.empty()) return;
    int lo = 127, hi = 0;
    for (auto& n : c->notes) if (selNotes.count (n.id)) { lo = jmin (lo, n.pitch); hi = jmax (hi, n.pitch); }
    semitones = jlimit (-lo, 127 - hi, semitones);
    if (semitones == 0) return;
    app.project.checkpoint();
    for (auto& n : c->notes) if (selNotes.count (n.id)) n.pitch += semitones;
    app.engine.auditionNote (jlimit (0, 127, (lo + hi) / 2 + semitones), lastVelocity, 150);
    changed();
}

void PianoRollView::nudgeSelection (double beats)
{
    auto* c = clip();
    if (c == nullptr || selNotes.empty()) return;
    double lo = 1.0e9, hiEnd = 0.0;
    for (auto& n : c->notes) if (selNotes.count (n.id)) { lo = jmin (lo, n.start); hiEnd = jmax (hiEnd, n.start + n.length); }
    if (lo + beats < 0.0) beats = -lo;
    app.project.checkpoint();
    for (auto& n : c->notes) if (selNotes.count (n.id)) n.start += beats;
    ensureClipCovers (*c, hiEnd + beats);
    changed();
}

void PianoRollView::reverseSelection()
{
    auto* c = clip();
    if (c == nullptr || selNotes.empty()) return;
    double lo = 1.0e9, hi = 0.0;
    for (auto& n : c->notes)
        if (selNotes.count (n.id)) { lo = jmin (lo, n.start); hi = jmax (hi, n.start + n.length); }
    if (! (hi > lo)) return;
    app.project.checkpoint();
    for (auto& n : c->notes)
        if (selNotes.count (n.id))
            n.start = lo + (hi - (n.start + n.length));   // mirror the phrase in time
    changed();
}

void PianoRollView::setSlideOnSelection (bool slide)
{
    auto* c = clip();
    if (c == nullptr || selNotes.empty()) return;
    bool changedAny = false;
    for (auto& n : c->notes)
        if (selNotes.count (n.id) && n.slide != slide) { changedAny = true; break; }
    if (! changedAny) return;
    app.project.checkpoint();
    for (auto& n : c->notes)
        if (selNotes.count (n.id)) n.slide = slide;
    changed();
}
}
