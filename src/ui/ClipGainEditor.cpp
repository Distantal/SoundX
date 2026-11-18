#include "ClipGainEditor.h"
#include "Theme.h"
#include "Waveform.h"

namespace sx
{
using namespace juce;

ClipGainEditor::ClipGainEditor (AppState& a) : app (a)
{
    setWantsKeyboardFocus (true);
    setVisible (false);

    modeBtn.setTooltip (TRANS ("Straight line drawing / freehand drawing"));
    addAndMakeVisible (eraseBtn);
    eraseBtn.setButtonText (TRANS ("Erase"));
    eraseBtn.setTooltip (TRANS ("Eraser: click or drag over anchors to delete them"));
    eraseBtn.setClickingTogglesState (true);
    eraseBtn.setColour (TextButton::buttonColourId, theme::white);
    eraseBtn.onClick = [this]
    {
        erasing = eraseBtn.getToggleState();
        eraseBtn.setColour (TextButton::buttonColourId, erasing ? theme::accentSoft : theme::white);
        setMouseCursor (erasing ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
    };
    modeBtn.onClick = [this]
    {
        app.clipGainFreehand = ! app.clipGainFreehand;
        refreshModeButtons();
    };
    addAndMakeVisible (modeBtn);

    snapBtn.setTooltip (TRANS ("Snap anchors to the arrange grid"));
    snapBtn.onClick = [this]
    {
        app.clipGainSnap = ! app.clipGainSnap;
        refreshModeButtons();
    };
    addAndMakeVisible (snapBtn);

    closeBtn.setButtonText (TRANS ("Close"));
    closeBtn.onClick = [this] { close(); };
    addAndMakeVisible (closeBtn);

    hzIn.setTooltip (TRANS ("Zoom In (horizontal)"));
    hzOut.setTooltip (TRANS ("Zoom Out (horizontal)"));
    vzIn.setTooltip (TRANS ("Zoom In (vertical)"));
    vzOut.setTooltip (TRANS ("Zoom Out (vertical)"));
    fitBtn.setButtonText (TRANS ("Fit"));
    fitBtn.setTooltip (TRANS ("Zoom to Fit Project"));
    hzIn.onClick = [this] { zoomHorizontal (1.4, plotRect().getCentreX()); };
    hzOut.onClick = [this] { zoomHorizontal (1.0 / 1.4, plotRect().getCentreX()); };
    vzIn.onClick = [this] { zoomVertical (1.4, plotRect().getCentreY()); };
    vzOut.onClick = [this] { zoomVertical (1.0 / 1.4, plotRect().getCentreY()); };
    fitBtn.onClick = [this] { fitView(); };
    for (auto* b : { &hzIn, &hzOut, &vzIn, &vzOut, &fitBtn }) addAndMakeVisible (*b);
}

void ClipGainEditor::refreshModeButtons()
{
    modeBtn.setButtonText (app.clipGainFreehand ? TRANS ("Free") : TRANS ("Line"));
    snapBtn.setButtonText (app.clipGainSnap ? TRANS ("Snap") : TRANS ("No Snap"));
    modeBtn.setColour (TextButton::buttonColourId, app.clipGainFreehand ? theme::accentSoft : theme::white);
    snapBtn.setColour (TextButton::buttonColourId, app.clipGainSnap ? theme::accentSoft : theme::white);
}

AudioClip* ClipGainEditor::clip() const
{
    auto* t = app.project.findTrack (trackId);
    return t != nullptr ? t->findAudio (clipId) : nullptr;
}

double ClipGainEditor::clipBeats() const
{
    auto* c = clip();
    if (c == nullptr) return 1.0;
    return jmax (0.25, app.project.clipBeats (c->start, c->lengthSec));
}

void ClipGainEditor::refreshTheme()
{
    eraseBtn.setColour (TextButton::buttonColourId, erasing ? theme::accentSoft : theme::white);
    modeBtn.setColour (TextButton::buttonColourId, app.clipGainFreehand ? theme::accentSoft : theme::white);
    snapBtn.setColour (TextButton::buttonColourId, app.clipGainSnap ? theme::accentSoft : theme::white);
    repaint();
}

void ClipGainEditor::open (int track, int clipToEdit)
{
    // validate first: a stale id must not retarget an editor that is already open
    auto* found = app.project.findTrack (track);
    auto* c = found != nullptr ? found->findAudio (clipToEdit) : nullptr;
    if (c == nullptr) return;
    trackId = track;
    clipId = clipToEdit;
    if (! c->gainLine)
    {
        app.project.checkpoint();
        c->gainLine = true;
        if (c->gainEnv.empty())
        {
            c->gainEnv.push_back ({ 0.0, c->gainDb });
            c->gainEnv.push_back ({ clipBeats(), c->gainDb });
        }
        app.project.sendContentChanged();
    }
    openState = true;
    dragIdx = -1;
    draggingAll = false;
    fadeDrag = 0;
    panning = false;
    fitView();          // every visit starts with the whole clip in view
    refreshModeButtons();
    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    repaint();
}

void ClipGainEditor::close()
{
    openState = false;
    setVisible (false);
    if (onClose) onClose();
}

Rectangle<float> ClipGainEditor::plotRect() const
{
    return getLocalBounds().toFloat().reduced (54.0f, 16.0f).withTrimmedTop (40.0f);
}

//==============================================================================
//-- view transform -----------------------------------------------------------
double ClipGainEditor::effViewBeats() const
{
    const double cb = clipBeats();
    return viewBeats > 0.0 ? jlimit (jmin (0.02, cb), cb, viewBeats) : cb;
}

void ClipGainEditor::zoomHorizontal (double factor, float anchorX)
{
    auto r = plotRect();
    if (r.getWidth() <= 1.0f) return;
    const double cb = clipBeats();
    double vb = effViewBeats() / factor;
    vb = jlimit (jmin (0.02, cb * 0.02), cb, vb);
    const double beatAtAnchor = xToBeat (anchorX);
    viewStart = beatAtAnchor - (double) ((anchorX - r.getX()) / r.getWidth()) * vb;
    viewStart = jlimit (0.0, jmax (0.0, cb - vb), viewStart);
    viewBeats = vb;
    repaint();
}

void ClipGainEditor::zoomVertical (double factor, float anchorY)
{
    // the dB range is always symmetric around 0 dB, so 0 dB (and a flat envelope)
    // stays exactly in the vertical middle of the waveform while zooming: the mouse
    // position deliberately does not become the anchor
    juce::ignoreUnused (anchorY);
    if (plotRect().getHeight() <= 1.0f) return;
    const float half = jlimit (3.0f, 36.0f, (dbSpan() * 0.5f) / (float) factor);
    dbTop = half;
    dbBottom = -half;
    repaint();
}

void ClipGainEditor::panView (double dBeats, float ddB)
{
    // only the time axis pans: the dB range stays centred on 0 dB
    juce::ignoreUnused (ddB);
    const double cb = clipBeats();
    const double vb = effViewBeats();
    viewStart = jlimit (0.0, jmax (0.0, cb - vb), viewStart + dBeats);
    repaint();
}

void ClipGainEditor::fitView()
{
    viewStart = 0.0;
    viewBeats = 0.0;
    // +-24 dB shows the whole clip gain range (-24 .. +12) with 0 dB dead centre
    dbTop = 24.0f;
    dbBottom = -24.0f;
    repaint();
}

//==============================================================================
//-- coordinate mapping --------------------------------------------------------
float ClipGainEditor::dbToY (float db) const
{
    auto r = plotRect();
    return r.getY() + (dbTop - db) / jmax (1.0f, dbSpan()) * r.getHeight();
}

float ClipGainEditor::yToDb (float y) const
{
    auto r = plotRect();
    return jlimit (jmin (dbBottom, -24.0f), jmax (dbTop, 12.0f),
                   dbTop - (y - r.getY()) / jmax (1.0f, r.getHeight()) * dbSpan());
}

float ClipGainEditor::beatToX (double beat) const
{
    auto r = plotRect();
    const double vb = effViewBeats();
    return r.getX() + (float) (vb > 0.0 ? (beat - viewStart) / vb : 0.0) * r.getWidth();
}

double ClipGainEditor::xToBeat (float x) const
{
    auto r = plotRect();
    const double vb = effViewBeats();
    const double cb = clipBeats();
    if (vb <= 0.0 || r.getWidth() <= 0.0f) return 0.0;
    return jlimit (0.0, cb, viewStart + (double) ((x - r.getX()) / r.getWidth()) * vb);
}

int ClipGainEditor::hitPoint (Point<int> p) const
{
    auto* c = clip();
    if (c == nullptr) return -1;
    int best = -1;
    float bestD = 11.0f;
    for (size_t i = 0; i < c->gainEnv.size(); ++i)
    {
        const float px = beatToX (c->gainEnv[i].beat);
        const float py = dbToY (c->gainEnv[i].db);
        const float d = jmax (std::abs ((float) p.x - px), std::abs ((float) p.y - py));
        if (d < bestD) { bestD = d; best = (int) i; }
    }
    return best;
}

int ClipGainEditor::hitFadeGrip (Point<int> p) const
{
    auto* c = clip();
    auto r = plotRect();
    if (c == nullptr || ! r.contains ((float) p.x, (float) p.y)) return 0;
    auto& proj = app.project;
    const double fadeInB = proj.secondsToBeats (proj.beatsToSeconds (c->start) + c->fadeIn) - c->start;
    const double fadeOutB = proj.secondsToBeats (proj.beatsToSeconds (c->start) + c->lengthSec)
                          - proj.secondsToBeats (proj.beatsToSeconds (c->start) + c->lengthSec - c->fadeOut);
    const float gx = beatToX (fadeInB), ox = beatToX (clipBeats() - fadeOutB);
    const bool topZone = (float) p.y < r.getY() + 22.0f;
    if (topZone && std::abs ((float) p.x - gx) < 10.0f) return 1;
    if (topZone && std::abs ((float) p.x - ox) < 10.0f) return 2;
    return 0;
}

//==============================================================================
void ClipGainEditor::resized()
{
    auto r = getLocalBounds().reduced (54, 10);
    auto row = r.removeFromTop (26);
    closeBtn.setBounds (row.removeFromRight (70));
    row.removeFromRight (6);
    fitBtn.setBounds (row.removeFromRight (64));
    row.removeFromRight (6);
    vzOut.setBounds (row.removeFromRight (64));
    row.removeFromRight (4);
    vzIn.setBounds (row.removeFromRight (64));
    row.removeFromRight (6);
    hzOut.setBounds (row.removeFromRight (56));
    row.removeFromRight (4);
    hzIn.setBounds (row.removeFromRight (56));
    row.removeFromRight (6);
    modeBtn.setBounds (row.removeFromRight (70));
    row.removeFromRight (6);
    eraseBtn.setBounds (row.removeFromRight (70));
    row.removeFromRight (6);
    snapBtn.setBounds (row.removeFromRight (80));
    // a resize must not leave pixels from the previous size behind (JUCE only
    // damages the newly exposed strips when a child grows)
    repaint();
}

void ClipGainEditor::paint (Graphics& g)
{
    g.fillAll (theme::panelDeep);
    auto* c = clip();
    if (c == nullptr)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (16.0f));
        g.drawText (TRANS ("No audio clip"), getLocalBounds(), Justification::centred);
        return;
    }
    auto& proj = app.project;

    auto plot = plotRect();
    g.setColour (theme::laneB);
    g.fillRoundedRectangle (plot, 4.0f);
    g.setColour (theme::line);
    g.drawRoundedRectangle (plot, 4.0f, 1.0f);

    // header
    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    g.drawText (c->name + "  -  " + TRANS ("Clip Gain Envelope"),
                getLocalBounds().reduced (54, 10).removeFromTop (26).withTrimmedRight (470),
                Justification::centredLeft);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText (TRANS ("Click to add an anchor, drag to move, Alt+drag moves the whole curve, right-click removes an anchor. "
                       "Wheel = zoom, Shift+wheel = scroll, Ctrl+wheel = dB zoom (always centred on 0 dB), middle-drag = pan, drag the top corners = fades. Esc closes."),
                getLocalBounds().reduced (54, 4).removeFromBottom (18),
                Justification::centredLeft);

    // dB guide lines (denser when zoomed out)
    const int dbStep = dbSpan() > 48 ? 12 : 6;
    const int loDb = (int) std::ceil (dbBottom / dbStep) * dbStep;
    const int hiDb = (int) std::floor (dbTop / dbStep) * dbStep;
    for (int db = loDb; db <= hiDb; db += dbStep)
    {
        const float y = dbToY ((float) db);
        if (y < plot.getY() || y > plot.getBottom()) continue;
        g.setColour (db == 0 ? theme::line : theme::lineSoft);
        g.fillRect (plot.getX(), y, plot.getWidth(), db == 0 ? 1.4f : 1.0f);
        g.setColour (theme::textFaint);
        g.setFont (theme::font (10.5f));
        g.drawText ((db > 0 ? "+" : "") + String (db) + " dB",
                    Rectangle<int> ((int) plot.getX() - 50, (int) y - 7, 46, 14),
                    Justification::centredRight);
    }

    // beat grid across the visible window
    const double vb = effViewBeats();
    const double cb = clipBeats();
    const double b0 = viewStart, b1 = viewStart + vb;
    int n, d;
    proj.meterAt (c->start + b0, n, d);
    const double beatLen = 4.0 / jmax (1, d);
    const double barLen = jmax (1.0, n * beatLen);
    // thin out the grid when deeply zoomed out
    const double gridStep = vb / plot.getWidth() > 0.25 ? barLen : beatLen;
    g.setFont (theme::font (10.0f));
    for (double b = std::floor (b0 / gridStep) * gridStep; b <= b1 + 1.0e-6; b += gridStep)
    {
        const double bc = jlimit (0.0, cb, b);
        const float x = beatToX (bc);
        const bool isBar = std::abs (std::fmod (bc + 1.0e-6, barLen)) < 1.0e-6;
        g.setColour (isBar ? theme::gridBar : theme::gridSub);
        g.fillRect (x, plot.getY(), isBar ? 1.3f : 1.0f, plot.getHeight());
        if (isBar)
        {
            g.setColour (theme::textFaint);
            g.drawText (String ((int) std::floor (bc / barLen) + 1) + "." + String (1 + (int) std::floor ((bc - std::floor (bc / barLen) * barLen) / beatLen)),
                        (int) x + 3, (int) plot.getY() + 2, 60, 13, Justification::centredLeft);
        }
    }

    // waveform, scaled by the envelope and clamped inside the plot. It follows the clip's
    // playback buffer, so speed / transpose / reverse edits are shown here as they are heard.
    // Waveform.h joins the columns into one anti-aliased outline (and follows the samples when
    // zoomed in); stereo clips get one lane per channel. The fade ramps are drawn on top of it
    // further down.
    if (c->data != nullptr)
    {
        const auto* pdata = c->playbackData();
        auto& data = pdata != nullptr ? *pdata : *c->data;
        const double clipStartSec = proj.beatsToSeconds (c->start);

        const auto secAtX = [&] (double x)
        {
            const double beatRel = xToBeat ((float) x);
            return clipBufferSeconds (*c, proj.beatsToSeconds (c->start + beatRel) - clipStartSec);
        };
        const auto gainAt = [&] (double x)
        {
            return (float) Decibels::decibelsToGain (evalClipGainDb (c->gainEnv, xToBeat ((float) x), c->gainDb));
        };

        g.setColour (theme::accent.withAlpha (0.45f));
        // stereo data with a plot tall enough for two lanes shows L on top and R below; a mono clip
        // (or a short plot) keeps the single mixed waveform
        if (data.buffer.getNumChannels() >= 2 && plot.getHeight() >= 14)
            waveform::drawStereo (g, plot.toFloat(), data, (int) plot.getX(), (int) plot.getRight(), secAtX, gainAt);
        else
            waveform::draw (g, plot.toFloat(), data, (int) plot.getX(), (int) plot.getRight(), secAtX, gainAt);
    }

    // fade in / out wedges with grips at the top corners (drag to change, double-click clears)
    {
        const double startSec = proj.beatsToSeconds (c->start);
        const double fadeInB = proj.secondsToBeats (startSec + c->fadeIn) - c->start;
        const double endSec = startSec + c->lengthSec;
        const double fadeOutB = proj.secondsToBeats (endSec) - proj.secondsToBeats (endSec - c->fadeOut);
        auto drawFade = [&] (bool fadeIn, double beats)
        {
            const float gx = beatToX (fadeIn ? beats : clipBeats() - beats);
            const float w = fadeIn ? (gx - plot.getX()) : (plot.getRight() - gx);
            if (w < 1.0f) return;
            Path p;
            if (fadeIn)
            {
                p.startNewSubPath (plot.getX(), plot.getBottom());
                p.lineTo (plot.getX() + w, plot.getY());
                p.lineTo (plot.getX() + w, plot.getBottom());
            }
            else
            {
                p.startNewSubPath (plot.getRight(), plot.getBottom());
                p.lineTo (plot.getRight() - w, plot.getY());
                p.lineTo (plot.getRight() - w, plot.getBottom());
            }
            p.closeSubPath();
            g.setColour (theme::accent.withAlpha (0.18f));
            g.fillPath (p);
            g.setColour (theme::accent.withAlpha (0.9f));
            if (fadeIn) g.drawLine (plot.getX(), plot.getBottom(), plot.getX() + w, plot.getY(), 1.7f);
            else        g.drawLine (plot.getRight(), plot.getBottom(), plot.getRight() - w, plot.getY(), 1.7f);
            const float hx = fadeIn ? plot.getX() + w : plot.getRight() - w;
            g.setColour (theme::accent);
            g.fillEllipse (hx - 5.0f, plot.getY() - 4.0f, 10.0f, 10.0f);
            g.setColour (theme::onAccent);
            g.fillEllipse (hx - 2.0f, plot.getY() - 1.0f, 4.0f, 4.0f);
        };
        drawFade (true, fadeInB);
        drawFade (false, fadeOutB);
    }

    // envelope
    if (c->gainEnv.empty())
    {
        g.setColour (theme::onAccent);
        g.drawLine (plot.getX(), dbToY (c->gainDb), plot.getRight(), dbToY (c->gainDb), 2.0f);
    }
    else
    {
        Path env;
        env.startNewSubPath (plot.getX(), dbToY (c->gainEnv.front().db));
        for (auto& p : c->gainEnv) env.lineTo (beatToX (p.beat), dbToY (p.db));
        env.lineTo (plot.getRight(), dbToY (c->gainEnv.back().db));
        g.setColour (theme::panelDeep.withAlpha (0.7f));
        g.strokePath (env, PathStrokeType (4.5f));
        g.setColour (theme::onAccent);
        g.strokePath (env, PathStrokeType (2.2f));
        for (auto& p : c->gainEnv)
        {
            const float px = beatToX (p.beat), py = dbToY (p.db);
            g.setColour (theme::panelDeep.withAlpha (0.8f));
            g.fillEllipse (px - 6.0f, py - 6.0f, 12.0f, 12.0f);
            g.setColour (theme::onAccent);
            g.fillEllipse (px - 4.0f, py - 4.0f, 8.0f, 8.0f);
        }
    }
}

//==============================================================================
//-- mouse ---------------------------------------------------------------------
void ClipGainEditor::mouseMove (const MouseEvent& e)
{
    if (hitFadeGrip (e.getPosition()) != 0) { setMouseCursor (MouseCursor::LeftRightResizeCursor); return; }
    setMouseCursor (hitPoint (e.getPosition()) >= 0 ? MouseCursor::DraggingHandCursor : MouseCursor::CrosshairCursor);
}

void ClipGainEditor::mouseDown (const MouseEvent& e)
{
    auto* c = clip();
    // eraser: delete the anchor under the cursor, and every anchor a drag passes over
    if (erasing)
    {
        if (auto* ec = clip(); ec != nullptr)
        {
            const int hi = hitPoint (e.getPosition());
            if (hi >= 0)
            {
                app.project.checkpoint();
                ec->gainEnv.erase (ec->gainEnv.begin() + hi);
                app.project.sendContentChanged();
                repaint();
            }
        }
        return;
    }

    if (c == nullptr) return;

    // middle button: pan the time axis
    if (e.mods.isMiddleButtonDown())
    {
        panning = true;
        panFrom = e.getPosition();
        panViewStart = viewStart;
        panViewBeats = effViewBeats();
        return;
    }

    if (! plotRect().contains (e.position)) return;

    // fade grips take priority over envelope anchors
    const int fade = hitFadeGrip (e.getPosition());
    if (fade != 0)
    {
        app.project.checkpoint();
        fadeDrag = fade;
        return;
    }

    const int hit = hitPoint (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
        {
            app.project.checkpoint();
            c->gainEnv.erase (c->gainEnv.begin() + hit);
            app.project.sendContentChanged();
            repaint();
        }
        return;
    }

    app.project.checkpoint();
    c->gainLine = true;          // an undo may have switched the envelope off behind our back
    dragStartY = e.y;
    dragBackup = c->gainEnv;

    if (e.mods.isAltDown())
    {
        draggingAll = true;
        dragIdx = -1;
        return;
    }

    if (hit >= 0)
    {
        dragIdx = hit;
        return;
    }

    // add a new anchor at the clicked position
    double absBeat = c->start + xToBeat (e.position.x);
    if (app.clipGainSnap && app.arrangeGrid > 0.0) absBeat = snapTo (absBeat, app.arrangeGrid);
    ClipGainPoint np { jlimit (0.0, clipBeats(), absBeat - c->start), yToDb (e.position.y) };
    auto& v = c->gainEnv;
    auto it = std::lower_bound (v.begin(), v.end(), np.beat,
                                [] (const ClipGainPoint& a, double b) { return a.beat < b; });
    it = v.insert (it, np);
    dragIdx = (int) (it - v.begin());
    app.project.sendContentChanged();
    repaint();
}

void ClipGainEditor::mouseDrag (const MouseEvent& e)
{
    auto* c = clip();
    if (c == nullptr) return;
    if (erasing)
    {
        if (auto* ec = clip(); ec != nullptr)
        {
            const int hi = hitPoint (e.getPosition());
            if (hi >= 0)
            {
                if (! dragBackup.empty()) app.project.checkpoint();
                ec->gainEnv.erase (ec->gainEnv.begin() + hi);
                app.project.sendContentChanged();
                repaint();
            }
        }
        return;
    }


    if (panning)
    {
        auto r = plotRect();
        const double beatsPerPx = panViewBeats / jmax (1.0f, r.getWidth());
        viewStart = jlimit (0.0, jmax (0.0, clipBeats() - panViewBeats),
                            panViewStart - (e.x - panFrom.x) * beatsPerPx);
        repaint();
        return;
    }

    if (fadeDrag != 0)
    {
        auto& proj = app.project;
        const double b = jmax (0.0, xToBeat (e.position.x));
        const double startSec = proj.beatsToSeconds (c->start);
        if (fadeDrag == 1)
            c->fadeIn = jlimit (0.0, 0.9 * c->lengthSec, proj.beatsToSeconds (c->start + b) - startSec);
        else
        {
            const double endSec = proj.beatsToSeconds (c->start + clipBeats());
            c->fadeOut = jlimit (0.0, 0.9 * c->lengthSec, endSec - proj.beatsToSeconds (c->start + b));
        }
        app.project.sendContentChanged();
        repaint();
        return;
    }

    if (draggingAll)
    {
        const float delta = (float) (dragStartY - e.y) * dbSpan() / jmax (1.0f, plotRect().getHeight());
        c->gainEnv = dragBackup;
        for (auto& p : c->gainEnv) p.db = jlimit (-24.0f, 12.0f, p.db + delta);
        app.project.sendContentChanged();
        repaint();
        return;
    }

    if (dragIdx < 0 || dragIdx >= (int) c->gainEnv.size()) return;
    double absBeat = c->start + xToBeat (e.position.x);
    if (app.clipGainSnap && app.arrangeGrid > 0.0) absBeat = snapTo (absBeat, app.arrangeGrid);
    double beat = jlimit (0.0, clipBeats(), absBeat - c->start);
    const float db = jlimit (-24.0f, 12.0f, yToDb (e.position.y));
    auto& v = c->gainEnv;

    if (app.clipGainFreehand && ! e.mods.isAltDown())
    {
        // freehand: drop points along the mouse path, kept sorted by time
        // freehand is continuous by definition: use the raw mouse position, not the grid
        const double freeBeat = jlimit (0.0, clipBeats(), xToBeat (e.position.x));
        auto it = std::lower_bound (v.begin(), v.end(), freeBeat,
                                    [] (const ClipGainPoint& p, double b) { return p.beat < b; });
        if (it != v.end() && std::abs (it->beat - freeBeat) < 1.0e-4) it->db = db;
        else v.insert (it, ClipGainPoint { freeBeat, db });
        app.project.sendContentChanged();
        repaint();
        return;
    }

    // anchors may never cross: clamp between the two neighbours
    const double lo = dragIdx > 0 ? v[(size_t) dragIdx - 1].beat + 1.0e-4 : 0.0;
    const double hi = dragIdx + 1 < (int) v.size() ? v[(size_t) dragIdx + 1].beat - 1.0e-4 : clipBeats();
    v[(size_t) dragIdx] = { jlimit (lo, jmax (lo, hi), beat), db };
    app.project.sendContentChanged();
    repaint();
}

void ClipGainEditor::mouseUp (const MouseEvent&)
{
    dragIdx = -1;
    draggingAll = false;
    fadeDrag = 0;
    panning = false;
    dragBackup.clear();
}

void ClipGainEditor::mouseDoubleClick (const MouseEvent& e)
{
    auto* c = clip();
    if (c == nullptr || ! plotRect().contains (e.position)) return;
    const int fade = hitFadeGrip (e.getPosition());
    if (fade != 0)
    {
        app.project.checkpoint();
        if (fade == 1) c->fadeIn = 0.0;
        else           c->fadeOut = 0.0;
        app.project.sendContentChanged();
        repaint();
    }
}

void ClipGainEditor::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    // wheel: zoom the time axis around the mouse
    // Shift+wheel: scroll left / right (down = later, like the arrange area)
    // Ctrl / Cmd+wheel: zoom the dB scale, so the envelope never moves by accident
    // a trackpad's horizontal swipe scrolls in either mode
    const double notches = (double) w.deltaY * 4.0;   // about 1.0 per wheel click
    if (e.mods.isShiftDown())
    {
        panView (-notches * effViewBeats() * 0.15, 0.0f);
        return;
    }
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        zoomVertical (std::pow (1.35, notches), e.position.y);
        return;
    }
    zoomHorizontal (std::pow (1.35, notches), e.position.x);
    if (std::abs (w.deltaX) > 0.005)
        panView (-(double) w.deltaX * effViewBeats() * 0.08, 0.0f);
}

bool ClipGainEditor::keyPressed (const KeyPress& k)
{
    const int code = k.getKeyCode();
    if (code == KeyPress::escapeKey) { close(); return true; }
    const double step = effViewBeats() * 0.12;
    if (code == KeyPress::leftKey)  { panView (-step, 0.0f); return true; }
    if (code == KeyPress::rightKey) { panView ( step, 0.0f); return true; }
    // the dB scale zooms around 0 dB, so the up / down keys work like the dB buttons
    if (code == KeyPress::upKey)    { zoomVertical (1.25, 0.0f); return true; }
    if (code == KeyPress::downKey)  { zoomVertical (1.0 / 1.25, 0.0f); return true; }
    if (k.getTextCharacter() == '+') { zoomHorizontal (1.4, plotRect().getCentreX()); return true; }
    if (k.getTextCharacter() == '-') { zoomHorizontal (1.0 / 1.4, plotRect().getCentreX()); return true; }
    if (k.getTextCharacter() == 'f' || k.getTextCharacter() == 'F') { fitView(); return true; }
    return false;
}
}
