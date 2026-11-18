#pragma once

#include "ArrangeView.h"
#include "Importer.h"
#include "TimelineGrid.h"
#include "../model/Chord.h"

namespace sx
{
using namespace juce;

//==============================================================================
// Chord track (Studio One style): every chord is its own small block, drawn like a note in the
// piano roll - from its position up to its own end, never stretched to the next chord. The text is
// free-form - "Cmaj7", "F#m7b5", "C/G", or even a remark - and the harmony it implies is what the
// piano roll and the harmony features work with (model/Chord.h).
//
//   click        place a one-bar chord block at the grid position
//   drag in empty space  draw the block as long as you drag, like a note with the pencil
//   drag the right edge  change the length of an existing block
//   drag the body        move the block; dropping it onto another chord's position replaces that one
//   double-click type the symbol
//   right-click  rename / delete / length of new blocks / clear
class ArrangeView::ChordLane : public juce::Component
{
public:
    static constexpr int kEdgeGrab = 7;    // px around a block's right edge that resize it

    explicit ChordLane (ArrangeView& o) : owner (o) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    //-- geometry / hit testing (also used by the self test) ---------------------
    double beatAt (int x) const { return ((double) x + owner.scrollX()) / owner.app.pxPerBeat; }
    double snapBeat (double b, bool free) const
    {
        return free ? juce::jmax (0.0, b) : juce::jmax (0.0, snapTo (b, owner.app.arrangeGrid));
    }
    // the beats a chord block covers: its own position up to its own end
    void chordExtent (int index, double& from, double& to) const
    {
        const auto& list = owner.app.project.chords;
        if (index < 0 || index >= (int) list.size()) { from = to = 0.0; return; }
        from = list[(size_t) index].beat;
        to = from + juce::jmax (kMinChordBeats, list[(size_t) index].length);
    }
    int chordIndexAtX (int x) const
    {
        const auto& list = owner.app.project.chords;
        const double beat = beatAt (x);
        for (int i = 0; i < (int) list.size(); ++i)
        {
            double b0 = 0.0, b1 = 0.0;
            chordExtent (i, b0, b1);
            if (beat >= b0 - 1.0e-9 && beat < b1 - 1.0e-9) return i;
        }
        return -1;
    }
    bool isNearEnd (int chordId, int x) const
    {
        if (chordId == 0) return false;
        const auto r = chipRect (chordId);
        return r.getWidth() > 0 && std::abs (x - r.getRight()) <= kEdgeGrab;
    }
    int chordIdAt (juce::Point<int> p) const
    {
        const int i = chordIndexAtX (p.x);
        return i >= 0 ? owner.app.project.chords[(size_t) i].id : 0;
    }
    juce::Rectangle<int> chipRect (int chordId) const
    {
        const auto& list = owner.app.project.chords;
        for (int i = 0; i < (int) list.size(); ++i)
        {
            if (list[(size_t) i].id != chordId) continue;
            double b0 = 0.0, b1 = 0.0;
            chordExtent (i, b0, b1);
            const double ppb = owner.app.pxPerBeat;
            const float x0 = (float) (b0 * ppb - owner.scrollX());
            const float x1 = (float) (b1 * ppb - owner.scrollX());
            return juce::Rectangle<int> ((int) std::round (x0) + 1, 5, juce::jmax (3, (int) std::round (x1 - x0) - 2),
                                         juce::jmax (4, getHeight() - 11));
        }
        return {};
    }

    // length in beats of a block placed at 'beat' (the "new chord length" setting, in bars)
    double newChordLength (double beat) const
    {
        auto& p = owner.app.project;
        const int bar = tempomap::barPosition (p.tempo, juce::jmax (0.0, beat)).bar;
        const int n = juce::jmax (1, owner.app.chordBars);
        return juce::jmax (kMinChordBeats,
                           tempomap::beatOfBar (p.tempo, bar + n) - tempomap::beatOfBar (p.tempo, bar));
    }

    // adds a chord block and returns its id (0 = nothing was added); the undo step is taken here
    int addChordAt (double beat, const juce::String& symbol, double length = 0.0)
    {
        auto& proj = owner.app.project;
        const auto s = symbol.trim();
        if (s.isEmpty()) return 0;
        beat = juce::jmax (0.0, beat);
        proj.checkpoint();
        auto& added = proj.addChord (beat, s, length > 0.0 ? length : newChordLength (beat));
        owner.app.lastChordSymbol = s;
        proj.sendContentChanged();
        repaint();
        return added.id;
    }

    // opens the chord editor: 'chordId' != 0 renames that chord, otherwise a new chord is placed
    // at 'beat' (used by the menu commands, which work even when the row is hidden)
    void editChord (int chordId, double beat) { promptChord (chordId, beat); }

    //-- painting ---------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        auto& app = owner.app;
        auto& proj = app.project;
        const double ppb = app.pxPerBeat;
        const int sx = owner.scrollX();
        auto clip = g.getClipBounds();
        const int h = getHeight();

        g.fillAll (theme::panelAlt);
        grid::paint (g, proj, ppb, clip, -sx, 0.0);

        if (proj.loopOn)
        {
            g.setColour (theme::accent.withAlpha (0.045f));
            g.fillRect ((int) (proj.loopStart * ppb) - sx, clip.getY(),
                        (int) ((proj.loopEnd - proj.loopStart) * ppb), clip.getHeight());
        }

        const auto& list = proj.chords;
        for (int i = 0; i < (int) list.size(); ++i)
        {
            double b0 = 0.0, b1 = 0.0;
            chordExtent (i, b0, b1);
            const float x0 = (float) (b0 * ppb - sx);
            const float x1 = (float) (b1 * ppb - sx);
            if (x1 < (float) clip.getX() - 2.0f || x0 > (float) clip.getRight() + 2.0f) continue;

            auto chip = juce::Rectangle<float> (x0 + 1.0f, 5.0f, juce::jmax (3.0f, x1 - x0 - 2.0f), (float) h - 11.0f);
            const bool active = list[(size_t) i].id == dragId || list[(size_t) i].id == resizeId
                                || list[(size_t) i].id == hoverId;
            const bool understood = chord::parse (list[(size_t) i].symbol).valid;

            g.setColour (active ? theme::accent.withAlpha (0.24f)
                                : (understood ? theme::accentSoft : theme::white));
            g.fillRoundedRectangle (chip, 4.0f);
            g.setColour (active ? theme::accent : theme::accent.withAlpha (0.35f));
            g.drawRoundedRectangle (chip.reduced (0.5f), 4.0f, active ? 1.6f : 1.0f);

            // the grip on the right edge says "this end can be dragged"
            if (chip.getWidth() > 14.0f)
            {
                g.setColour (theme::accent.withAlpha (active ? 0.55f : 0.25f));
                g.fillRoundedRectangle (chip.getRight() - 4.0f, chip.getY() + 6.0f, 2.0f, chip.getHeight() - 12.0f, 1.0f);
            }

            drawSymbols (g, list[(size_t) i].symbol, understood, b0, b1, chip);
        }

        if (list.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.5f));
            g.drawText (TRANS ("Click to place a chord block (drag to draw it longer), double-click it to type the symbol"),
                        juce::Rectangle<int> (10, 0, juce::jmax (10, getWidth() - 20), h), juce::Justification::centredLeft);
        }

        g.setColour (theme::line);
        g.fillRect (0, h - 1, getWidth(), 1);
        const int px = (int) std::round (app.engine.getPositionBeats() * ppb) - sx;
        if (px >= 0 && px < getWidth())
        {
            g.setColour (theme::accent);
            g.fillRect (px, clip.getY(), 2, clip.getHeight());
        }
    }

    //-- mouse ------------------------------------------------------------------
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int id = chordIdAt (e.getPosition());
        if (id != hoverId) { hoverId = id; repaint(); }
        setMouseCursor (isNearEnd (id, e.x) ? juce::MouseCursor::LeftRightResizeCursor
                                            : (id != 0 ? juce::MouseCursor::DraggingHandCursor
                                                       : juce::MouseCursor::PointingHandCursor));
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverId != 0) { hoverId = 0; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& proj = owner.app.project;
        const int index = chordIndexAtX (e.x);
        if (e.mods.isPopupMenu()) { showMenu (index, beatAt (e.x)); return; }

        dragMoved = false;
        dragBeat = 0.0;
        dragLength = 0.0;
        if (index < 0)
        {
            // an empty spot gets a one-bar block straight away; dragging right away draws it longer
            // (the same gesture as drawing a note with the pencil), double-click types the symbol
            const double beat = snapBeat (beatAt (e.x), e.mods.isAltDown());
            const int id = addChordAt (beat, owner.app.lastChordSymbol);
            dragId = id;
            dragFrom = beat;
            dragOffset = 0.0;
            dragLength = id != 0 ? newChordLength (beat) : 0.0;
            resizeId = id;
            return;
        }

        dragId = proj.chords[(size_t) index].id;
        dragFrom = proj.chords[(size_t) index].beat;
        dragOffset = beatAt (e.x) - dragFrom;
        dragLength = proj.chords[(size_t) index].length;
        resizeId = isNearEnd (dragId, e.x) ? dragId : 0;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragId == 0) return;
        auto& proj = owner.app.project;
        if (resizeId != 0)
        {
            // dragging the right edge changes the length of the block, like a note in the piano roll
            const double end = snapBeat (beatAt (e.x), e.mods.isAltDown());
            const double len = juce::jmax (kMinChordBeats, end - dragFrom);
            if (! dragMoved)
            {
                if (std::abs (len - dragLength) < 1.0e-6) return;
                proj.checkpoint();
                dragMoved = true;
            }
            dragLength = len;
            proj.resizeChord (dragId, len);
            proj.sendContentChanged();
            return;
        }

        const double target = snapBeat (beatAt (e.x) - dragOffset, e.mods.isAltDown());
        if (! dragMoved)
        {
            if (std::abs (target - dragFrom) < 1.0e-6) return;
            proj.checkpoint();
            dragMoved = true;
        }
        dragBeat = target;
        // while dragging the chord may pass over others; collisions are settled on release
        proj.moveChord (dragId, target, false);
        proj.sendContentChanged();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        auto& proj = owner.app.project;
        if (dragId != 0 && dragMoved)
        {
            if (resizeId != 0)
            {
                proj.resizeChord (dragId, dragLength);
                // a block that was stretched to whole bars becomes the length of the next one
                if (auto* c = proj.findChord (dragId))
                {
                    const double barLen = newChordLength (c->beat) / juce::jmax (1, owner.app.chordBars);
                    const double bars = barLen > 1.0e-6 ? c->length / barLen : 0.0;
                    const int whole = (int) std::round (bars);
                    if (whole >= 1 && whole <= 16 && std::abs (bars - (double) whole) < 0.02)
                        owner.app.chordBars = whole;
                }
            }
            else
            {
                proj.moveChord (dragId, dragBeat, true);
            }
            proj.sendContentChanged();
        }
        dragId = 0;
        resizeId = 0;
        dragMoved = false;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int index = chordIndexAtX (e.x);
        if (index >= 0) promptChord (owner.app.project.chords[(size_t) index].id, 0.0);
        else promptChord (0, snapBeat (beatAt (e.x), e.mods.isAltDown()));
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown()) owner.zoomTrackHeights (ArrangeView::notchesOf (w), -1);
        else owner.laneViewport.mouseWheelMove (e.getEventRelativeTo (&owner.laneViewport), w);
    }

private:
    void drawSymbols (juce::Graphics& g, const juce::String& symbol, bool understood,
                      double b0, double b1, juce::Rectangle<float> chip)
    {
        auto& app = owner.app;
        const double ppb = app.pxPerBeat;
        const int sx = owner.scrollX();

        // the symbol is repeated on every bar line inside the chip, like Studio One's chord track
        std::vector<double> spots { b0 };
        std::vector<tempomap::BarInfo> bars;
        tempomap::barStartsBetween (app.project.tempo, b0 + 1.0e-6, b1 - 1.0e-6, bars);
        for (auto& b : bars)
            if (b.beat - spots.back() >= 40.0 / juce::jmax (1.0, ppb)) spots.push_back (b.beat);

        g.saveState();
        g.reduceClipRegion (chip.toNearestInt().reduced (2, 0));
        for (size_t i = 0; i < spots.size(); ++i)
        {
            const float x = (float) (spots[i] * ppb - sx);
            const float nextX = (float) ((i + 1 < spots.size() ? spots[i + 1] : b1) * ppb - sx);
            const int w = juce::jmax (18, (int) (nextX - x) - 6);
            auto r = juce::Rectangle<int> ((int) std::round (x) + 8, (int) chip.getY(), w, (int) chip.getHeight());
            g.setFont (theme::font (i == 0 ? 13.5f : 12.0f, i == 0));
            g.setColour (i == 0 ? (understood ? theme::accentDark : theme::textDim)
                                : theme::accent.withAlpha (0.45f));
            g.drawFittedText (symbol, r, juce::Justification::centredLeft, 1);
        }
        g.restoreState();
    }

    void promptChord (int chordId, double beat)
    {
        auto* proj = &owner.app.project;
        auto* app = &owner.app;
        const auto* existing = chordId != 0 ? proj->findChord (chordId) : nullptr;
        juce::String initial;
        if (existing != nullptr) initial = existing->symbol;
        else if (const auto* at = proj->chordAt (beat); at != nullptr) initial = at->symbol;
        else initial = app->lastChordSymbol;
        const double length = newChordLength (beat);

        SafePointer<ChordLane> self (this);
        promptText (TRANS ("Chord Symbol"), TRANS ("C, Am7, F#m7b5, C/G, Cmaj9 ... (Esc = Cancel)"), initial,
                    [self, proj, app, chordId, beat, length] (juce::String text)
        {
            const auto s = text.trim();
            if (s.isEmpty()) return;
            proj->checkpoint();
            if (chordId != 0) proj->setChordSymbol (chordId, s);
            else proj->addChord (beat, s, length);
            app->lastChordSymbol = s;
            proj->sendContentChanged();
            if (self != nullptr) self->repaint();
        });
    }

    void showMenu (int index, double beat)
    {
        auto* proj = &owner.app.project;
        auto* app = &owner.app;
        const int id = index >= 0 ? proj->chords[(size_t) index].id : 0;
        const double at = index >= 0 ? proj->chords[(size_t) index].beat : snapBeat (beat, false);
        static const int kLengths[] { 1, 2, 4, 8 };          // bars
        constexpr int kLengthBase = 20;

        PopupMenu m;
        m.addItem (1, id != 0 ? TRANS ("Rename Chord...") : TRANS ("Enter Chord Here..."));
        m.addItem (2, TRANS ("Delete Chord"), id != 0);
        m.addSeparator();
        {
            // how long the block a click creates is
            PopupMenu len;
            for (int i = 0; i < 4; ++i)
                len.addItem (kLengthBase + i, String (kLengths[i]) + " " + TRANS (kLengths[i] == 1 ? "bar" : "bars"),
                             true, app->chordBars == kLengths[i]);
            m.addSubMenu (TRANS ("New Chord Length"), len);
        }
        m.addSeparator();
        m.addItem (3, TRANS ("Clear All Chords"), ! proj->chords.empty());

        SafePointer<ChordLane> self (this);
        m.showMenuAsync (PopupMenu::Options(), [self, proj, app, id, at] (int r)
        {
            if (r >= kLengthBase && r < kLengthBase + 4)
            {
                app->chordBars = kLengths[r - kLengthBase];
                return;
            }
            if (r == 1) { if (self != nullptr) self->promptChord (id, at); }
            else if (r == 2 && id != 0)
            {
                proj->checkpoint();
                proj->removeChord (id);
                proj->sendContentChanged();
            }
            else if (r == 3)
            {
                proj->checkpoint();
                proj->clearChords();
                proj->sendContentChanged();
            }
        });
    }

    ArrangeView& owner;
    int dragId = 0, hoverId = 0, resizeId = 0;
    double dragFrom = 0.0, dragOffset = 0.0, dragBeat = 0.0, dragLength = 0.0;
    bool dragMoved = false;
};

//==============================================================================
// Marker track (Studio One's arranger track): named arrangement sections drawn as ranges.
//
//   click on empty space   create a section there (four bars, name suggested, see the menu)
//   double-click           type the name
//   drag the body          move the section (it never runs over its neighbours)
//   drag the right edge    change the length of the section
//   right-click            rename / add after / length of new sections / delete
class ArrangeView::MarkerLane : public juce::Component
{
public:
    explicit MarkerLane (ArrangeView& o) : owner (o) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    static constexpr int kEdgeGrab = 7;    // px around a section's right edge that resize it

    double beatAt (int x) const { return ((double) x + owner.scrollX()) / owner.app.pxPerBeat; }

    // sections snap to bar lines: they mark the structure of the song, not a fine position
    double nearestBar (double beat) const
    {
        auto& p = owner.app.project;
        const double b = juce::jmax (0.0, beat);
        const int bar = tempomap::barPosition (p.tempo, b).bar;
        const double b0 = tempomap::beatOfBar (p.tempo, bar);
        const double b1 = tempomap::beatOfBar (p.tempo, bar + 1);
        return (b - b0) <= (b1 - b) ? b0 : b1;
    }
    double sectionLength (double beat) const
    {
        auto& p = owner.app.project;
        const int bar = tempomap::barPosition (p.tempo, beat).bar;
        const int n = juce::jmax (1, owner.app.sectionBars);
        return juce::jmax (kMinSectionBeats, tempomap::beatOfBar (p.tempo, bar + n) - tempomap::beatOfBar (p.tempo, bar));
    }

    juce::Rectangle<int> blockRect (int sectionId) const
    {
        for (auto& s : owner.app.project.sections)
        {
            if (s.id != sectionId) continue;
            const double ppb = owner.app.pxPerBeat;
            const int x0 = (int) std::round (s.beat * ppb - owner.scrollX());
            const int x1 = (int) std::round ((s.beat + s.length) * ppb - owner.scrollX());
            return juce::Rectangle<int> (x0, 4, juce::jmax (6, x1 - x0), juce::jmax (6, getHeight() - 9));
        }
        return {};
    }

    int sectionIdAt (juce::Point<int> p) const
    {
        const double beat = beatAt (p.x);
        for (auto& s : owner.app.project.sections)
            if (beat >= s.beat - 1.0e-9 && beat < s.beat + s.length - 1.0e-9) return s.id;
        return 0;
    }

    int addSectionAt (double beat, const juce::String& name)
    {
        auto& proj = owner.app.project;
        proj.checkpoint();
        auto& added = proj.addSection (nearestBar (beat), sectionLength (nearestBar (beat)), name);
        proj.sendContentChanged();
        repaint();
        return added.id;
    }

    // opens the name editor of a section (also used by the menu command)
    void editSectionName (int id) { promptName (id); }

    void paint (juce::Graphics& g) override
    {
        auto& app = owner.app;
        auto& proj = app.project;
        const double ppb = app.pxPerBeat;
        const int sx = owner.scrollX();
        auto clip = g.getClipBounds();
        const int h = getHeight();

        g.fillAll (theme::laneA);
        grid::paint (g, proj, ppb, clip, -sx, 0.0);

        for (auto& s : proj.sections)
        {
            const float x0 = (float) (s.beat * ppb - sx);
            const float x1 = (float) ((s.beat + s.length) * ppb - sx);
            if (x1 < (float) clip.getX() - 2.0f || x0 > (float) clip.getRight() + 2.0f) continue;

            const bool active = s.id == dragId || s.id == hoverId;
            auto block = juce::Rectangle<float> (x0 + 1.0f, 4.0f, juce::jmax (4.0f, x1 - x0 - 2.0f), (float) h - 9.0f);
            g.setColour (s.colour.withAlpha (active ? 0.34f : 0.20f));
            g.fillRoundedRectangle (block, 3.0f);
            g.setColour (s.colour.withAlpha (active ? 0.95f : 0.6f));
            g.drawRoundedRectangle (block.reduced (0.5f), 3.0f, active ? 1.6f : 1.0f);

            // the flag at the start of the section
            juce::Path flag;
            flag.startNewSubPath (block.getX(), block.getY());
            flag.lineTo (block.getX() + 9.0f, block.getY());
            flag.lineTo (block.getX() + 4.5f, block.getCentreY());
            flag.lineTo (block.getX() + 9.0f, block.getBottom());
            flag.lineTo (block.getX(), block.getBottom());
            flag.closeSubPath();
            g.setColour (s.colour.withAlpha (active ? 1.0f : 0.85f));
            g.fillPath (flag);
            g.fillRect (block.getX(), block.getY(), 1.5f, block.getHeight());

            g.saveState();
            g.reduceClipRegion (block.toNearestInt().reduced (1, 0));
            // the name is repeated on the bar lines of a long section, so it stays readable
            std::vector<double> spots { s.beat };
            std::vector<tempomap::BarInfo> bars;
            tempomap::barStartsBetween (proj.tempo, s.beat + 1.0e-6, s.beat + s.length - 1.0e-6, bars);
            for (auto& b : bars)
                if (b.beat - spots.back() >= 90.0 / juce::jmax (1.0, ppb)) spots.push_back (b.beat);
            for (size_t i = 0; i < spots.size(); ++i)
            {
                const int nameX = (int) std::round (spots[i] * ppb - sx) + 13;
                if (nameX > (int) block.getRight()) continue;
                auto r = juce::Rectangle<int> (nameX, (int) block.getY(), 100, (int) block.getHeight());
                g.setFont (theme::font (11.5f, i == 0));
                g.setColour (s.colour.darker (0.45f).withAlpha (i == 0 ? 0.95f : 0.45f));
                g.drawFittedText (s.name, r, juce::Justification::centredLeft, 1);
            }
            g.restoreState();
        }

        if (proj.sections.empty())
        {
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.0f));
            g.drawText (TRANS ("Click to add a section (Intro, Verse, Chorus ...), double-click it to name it"),
                        juce::Rectangle<int> (10, 0, juce::jmax (10, getWidth() - 20), h), juce::Justification::centredLeft);
        }

        g.setColour (theme::line);
        g.fillRect (0, h - 1, getWidth(), 1);
        const int px = (int) std::round (app.engine.getPositionBeats() * ppb) - sx;
        if (px >= 0 && px < getWidth())
        {
            g.setColour (theme::accent);
            g.fillRect (px, clip.getY(), 2, clip.getHeight());
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int id = sectionIdAt (e.getPosition());
        const bool nearEnd = isNearEnd (id, e.x);
        if (id != hoverId) { hoverId = id; repaint(); }
        setMouseCursor (nearEnd ? juce::MouseCursor::LeftRightResizeCursor
                                : (id != 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::PointingHandCursor));
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverId != 0) { hoverId = 0; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& proj = owner.app.project;
        const int id = sectionIdAt (e.getPosition());
        if (e.mods.isPopupMenu()) { showMenu (id, e.getPosition()); return; }

        if (id == 0)
        {
            addSectionAt (beatAt (e.x), {});
            return;
        }

        dragId = id;
        if (auto* s = proj.findSection (id))
        {
            resizeMode = isNearEnd (id, e.x);
            dragOffset = beatAt (e.x) - s->beat;
        }
        dragMoved = false;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragId == 0) return;
        auto& proj = owner.app.project;
        auto* s = proj.findSection (dragId);
        if (s == nullptr) return;
        if (! dragMoved)
        {
            proj.checkpoint();
            dragMoved = true;
        }
        if (resizeMode)
        {
            const double end = nearestBar (beatAt (e.x));
            proj.resizeSection (dragId, end - s->beat);
        }
        else
        {
            proj.moveSection (dragId, nearestBar (beatAt (e.x) - dragOffset));
        }
        proj.sendContentChanged();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragId = 0;
        dragMoved = false;
        resizeMode = false;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int id = sectionIdAt (e.getPosition());
        if (id != 0) promptName (id);
        else promptName (addSectionAt (beatAt (e.x), {}));
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown()) owner.zoomTrackHeights (ArrangeView::notchesOf (w), -1);
        else owner.laneViewport.mouseWheelMove (e.getEventRelativeTo (&owner.laneViewport), w);
    }

private:
    bool isNearEnd (int id, int x) const
    {
        if (id == 0) return false;
        const auto r = blockRect (id);
        return r.getWidth() > 0 && std::abs (x - r.getRight()) <= kEdgeGrab;
    }

    void promptName (int id)
    {
        if (id == 0) return;
        auto* proj = &owner.app.project;
        auto* s = proj->findSection (id);
        if (s == nullptr) return;
        SafePointer<MarkerLane> self (this);
        promptText (TRANS ("Section Name"), TRANS ("Intro, Verse, Chorus ... (Esc = Cancel)"), s->name,
                    [self, proj, id] (juce::String text)
        {
            const auto name = text.trim();
            if (name.isEmpty()) return;
            proj->checkpoint();
            proj->setSectionName (id, name);
            proj->sendContentChanged();
            if (self != nullptr) self->repaint();
        });
    }

    void showMenu (int id, juce::Point<int> pos)
    {
        auto* proj = &owner.app.project;
        auto* app = &owner.app;
        const double beat = beatAt (pos.x);
        static const int kLengths[] { 1, 2, 4, 8 };          // bars
        constexpr int kLengthBase = 20;

        PopupMenu m;
        m.addItem (1, id != 0 ? TRANS ("Rename Section...") : TRANS ("Add Section Here..."));
        m.addItem (2, TRANS ("Add Section After"), id != 0);
        m.addItem (3, TRANS ("Delete Section"), id != 0);
        m.addSeparator();
        {
            // how long a section made by a click is
            PopupMenu len;
            for (int i = 0; i < 4; ++i)
                len.addItem (kLengthBase + i, String (kLengths[i]) + " " + TRANS (kLengths[i] == 1 ? "bar" : "bars"),
                             true, app->sectionBars == kLengths[i]);
            m.addSubMenu (TRANS ("New Section Length"), len);
        }
        m.addSeparator();
        m.addItem (4, TRANS ("Clear All Sections"), ! proj->sections.empty());

        SafePointer<MarkerLane> self (this);
        m.showMenuAsync (PopupMenu::Options(), [self, proj, app, id, beat] (int r)
        {
            if (r >= kLengthBase && r < kLengthBase + 4)
            {
                app->sectionBars = kLengths[r - kLengthBase];
                return;
            }
            if (r == 1)
            {
                if (id != 0) { if (self != nullptr) self->promptName (id); }
                else if (self != nullptr) self->promptName (self->addSectionAt (beat, {}));
            }
            else if (r == 2 && id != 0)
            {
                proj->checkpoint();
                proj->addSectionAfter (id);
                proj->sendContentChanged();
            }
            else if (r == 3 && id != 0)
            {
                proj->checkpoint();
                proj->removeSection (id);
                proj->sendContentChanged();
            }
            else if (r == 4)
            {
                proj->checkpoint();
                proj->clearSections();
                proj->sendContentChanged();
            }
        });
    }

    ArrangeView& owner;
    int dragId = 0, hoverId = 0;
    bool resizeMode = false;
    double dragOffset = 0.0;
    bool dragMoved = false;
};
}
