#include "Panels.h"

namespace sx
{
using namespace juce;

//==============================================================================
MixerView::MixerView (AppState& a) : app (a), master (a, -1), listen (a, -2)
{
    addAndMakeVisible (listen);
    listen.setVisible (false);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);
    addAndMakeVisible (master);
    app.project.addListener (this);
    app.addListener (this);
    rebuild();
}

MixerView::~MixerView()
{
    app.removeListener (this);
    app.project.removeListener (this);
}

void MixerView::rebuild()
{
    // the listen bus is not part of the scrolling row: it gets its own strip right of the master
    std::vector<Track*> inRow;
    for (auto& t : app.project.tracks)
    {
        if (t == nullptr || t->isListenBus) continue;
        // a stand-alone automation track has no channel: it belongs to the arranger only
        if (t->isAutomation()) continue;

        // a multi-output instrument's channels are shown / hidden per instrument (they keep
        // playing either way: hiding a channel only takes it out of the mixer)
        if (t->isOutputChannel())
        {
            auto* parent = app.project.findTrack (t->sourceTrack);
            if (parent != nullptr && ! parent->showOutputChannels) continue;
        }
        inRow.push_back (t.get());
    }
    const int n = (int) inRow.size();
    while ((int) strips.size() > n) strips.pop_back();
    while ((int) strips.size() < n)
    {
        strips.push_back (std::make_unique<ChannelStrip> (app, -2));
        content.addAndMakeVisible (*strips.back());
    }
    for (int i = 0; i < n; ++i)
        strips[(size_t) i]->setTrackId (inRow[(size_t) i]->id);

    // the listen bus gets its own strip on the right of the master
    if (auto* lb = app.project.listenBus())
    {
        listen.setTrackId (lb->id);
        listen.setVisible (true);
    }
    else
    {
        listen.setTrackId (-2);
        listen.setVisible (false);
    }
    resized();
}

void MixerView::paint (Graphics& g)
{
    g.fillAll (theme::panelAlt);
    // 1 px separator between the scrolling channel row and the fixed master strip. resized()
    // insets the viewport by one pixel so this column is not covered by any child; without it a
    // last strip that is partially scrolled out looked like it was painted *under* the master.
    if (master.getWidth() > 0)
    {
        g.setColour (theme::line);
        g.fillRect (master.getX() - 1, 0, 1, getHeight());
    }
}

void MixerView::resized()
{
    // A narrow window must still show every channel strip: auto-fit picks the widest width from
    // the strip-width ladder that fits next to the fixed master (and listen) strip. app.stripWidth
    // is only the upper bound - it is never widened here and never written back to the settings.
    const int count = (int) strips.size();
    const int fixed = listen.isVisible() ? 2 : 1;   // fixed strips to the right of the viewport
    // n channel strips of w + 'fixed' strips of (w + 8) + the 1 px separator before the master
    auto fits = [this, count, fixed] (int w)
    {
        return count * w + fixed * (w + 8) + 1 <= getWidth();
    };

    int sw = app.stripWidth > 0 ? app.stripWidth : stripWidthSteps().front();
    if (! fits (sw))
    {
        sw = stripWidthSteps().front();             // nothing fits: smallest step, the row scrolls
        for (int candidate : stripWidthSteps())
            if (candidate <= app.stripWidth && fits (candidate)) sw = candidate;
    }
    effectiveStripWidth = sw;

    auto r = getLocalBounds();
    if (listen.isVisible()) listen.setBounds (r.removeFromRight (sw + 8));   // right of the master
    master.setBounds (r.removeFromRight (sw + 8));
    r.removeFromRight (1);                          // the separator column painted by paint()
    viewport.setBounds (r);

    // Size the content twice: the first pass tells the viewport whether the horizontal scrollbar
    // is needed, the second uses the visible height that remains after it. Sizing the strips
    // before the bar was known left them one scrollbar (12 px) too tall, so the auto row was
    // hidden under the bar and no vertical scrollbar exists to reach it.
    const int contentW = jmax (count * sw, r.getWidth());
    content.setSize (jmax (contentW, 1), jmax (1, viewport.getHeight()));
    const int h = jmax (100, viewport.getMaximumVisibleHeight());
    content.setSize (jmax (contentW, 1), h);
    for (int i = 0; i < count; ++i)
        strips[(size_t) i]->setBounds (i * sw, 0, sw, h);
}

void MixerView::refreshTheme()
{
    for (auto& s : strips) if (s != nullptr) s->refreshTheme();
    master.refreshTheme();
    listen.refreshTheme();
    repaint();
}

void MixerView::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown())
    {
        // horizontal zoom of the channel strips: discrete ladder steps (node style), one
        // preset width per wheel notch, anchored on the strip under the cursor
        constexpr float kNotch = 120.0f / 512.0f;
        const float notches = w.deltaY / kNotch;
        if (notches == 0.0f) return;
        if ((notches > 0.0f) != (stripZoomAccum > 0.0f)) stripZoomAccum = 0.0f;
        stripZoomAccum += notches;
        const int whole = (int) std::floor (std::abs (stripZoomAccum) + 1.0e-3f);
        if (whole == 0) return;
        const int dir = stripZoomAccum > 0.0f ? 1 : -1;
        stripZoomAccum -= (float) (dir * whole);

        const int oldW = jmax (1, effectiveStripWidth);
        int newW = app.stripWidth;
        for (int i = 0; i < whole; ++i) newW = stepStripWidth (newW, dir);
        if (newW == app.stripWidth) return;

        const int contentX = e.x + viewport.getViewPositionX();
        const int idx = contentX / oldW;
        const int off = contentX - idx * oldW;
        app.stripWidth = newW;
        resized();
        // anchor on the strip under the cursor; auto-fit may have clamped the laid-out width, so
        // scale the offset to the width that was actually used
        const int newEffective = jmax (1, effectiveStripWidth);
        const int anchorX = idx * newEffective
                            + (int) std::round ((double) off * (double) newEffective / (double) oldW);
        viewport.setViewPosition (jmax (0, anchorX - e.x), viewport.getViewPositionY());
        return;
    }
    // a Viewport hands an unhandled wheel event back to its parent, which is this component:
    // guard the forward so that bounce cannot recurse into this handler forever
    if (inWheelForward) return;
    const ScopedValueSetter<bool> guard (inWheelForward, true);
    viewport.mouseWheelMove (e.getEventRelativeTo (&viewport), w);
}

//==============================================================================
InspectorView::InspectorView (AppState& a) : app (a), strip (a, -2), master (a, -1)
{
    // neither column of the inspector shows an automation switch: it belongs to the mixer
    strip.setAutoButtonAllowed (false);
    master.setAutoButtonAllowed (false);
    addAndMakeVisible (strip);
    addAndMakeVisible (master);
    for (int i = 1; i <= 16; ++i) channelBox.addItem (TRANS ("Ch ") + String (i), i);
    channelBox.onChange = [this]
    {
        if (updating) return;
        if (auto* t = app.track())
        {
            app.project.checkpoint();
            t->midiChannel = channelBox.getSelectedId();
            app.project.sendMixChanged();
            app.project.sendContentChanged();
        }
    };
    addAndMakeVisible (channelBox);

    // manual latency compensation in milliseconds (type a number, or use the arrows)
    delaySlider.setSliderStyle (Slider::IncDecButtons);
    delaySlider.setTextBoxStyle (Slider::TextBoxLeft, false, 62, 20);
    delaySlider.setRange (-2000.0, 2000.0, 1.0);
    delaySlider.setTextValueSuffix (" ms");
    delaySlider.setDoubleClickReturnValue (true, 0.0);
    delaySlider.setTooltip (TRANS ("Track delay in milliseconds - positive plays later, negative plays earlier"));
    delaySlider.onValueChange = [this]
    {
        if (updating) return;
        if (auto* t = app.track()) app.setTrackDelay (t->id, delaySlider.getValue());
    };
    addAndMakeVisible (delaySlider);

    app.project.addListener (this);
    app.addListener (this);
    refresh();
}

InspectorView::~InspectorView()
{
    app.removeListener (this);
    app.project.removeListener (this);
}

void InspectorView::refresh()
{
    updating = true;
    strip.setAutoButtonAllowed (false);      // re-assert: the strip is reused for every selection
    master.setAutoButtonAllowed (false);
    // selecting a stand-alone automation row shows the channel it drives (it has no channel of
    // its own); the master target falls back to the pinned master strip below
    int stripId = app.selectedTrack;
    if (auto* sel = app.track(); sel != nullptr && sel->isAutomation())
        stripId = sel->autoTargetTrack == 0 ? -2 : sel->autoTargetTrack;
    strip.setTrackId (stripId);
    auto* t = app.project.findTrack (stripId);
    strip.setVisible (t != nullptr);
    master.setVisible (true);
    const bool autoSel = t != nullptr && t->isAutomation();
    channelBox.setVisible (t != nullptr && ! autoSel && t->type == TrackType::Instrument);
    if (t != nullptr) channelBox.setSelectedId (t->midiChannel, dontSendNotification);
    delaySlider.setVisible (t != nullptr && ! autoSel);
    if (t != nullptr) delaySlider.setValue (t->delayMs, dontSendNotification);
    updating = false;

    // the strip appears / disappears with the selection, and the master takes the whole column
    // while nothing is selected: that is a layout change, not just a repaint (a track added to an
    // empty project used to leave the column with the master's full-width layout)
    resized();
    repaint();
}

void InspectorView::refreshTheme()
{
    strip.refreshTheme();
    master.refreshTheme();
    repaint();
}

void InspectorView::paint (Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::line);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true));
    g.drawText (TRANS ("INSPECTOR"), 12, 0, getWidth() - 24, 26, Justification::centredLeft);
    g.setColour (theme::line);
    g.fillRect (0, 26, getWidth(), 1);

    if (auto* t = app.track())
    {
        auto& pal = theme::trackPalette();
        auto a = swatchArea;
        const int sw = 16, gap = 4;
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        g.drawText (TRANS ("Delay"), delayCaption, Justification::centredLeft);

        g.drawText (t->isAutomation() ? TRANS ("Automation Track")
                                      : t->type == TrackType::Instrument ? TRANS ("Software Instrument") : TRANS ("Audio Track"),
                    12, 30, getWidth() - 24, 16, Justification::centredLeft);
        for (int i = 0; i < pal.size(); ++i)
        {
            auto s = Rectangle<int> (a.getX() + i * (sw + gap), a.getY(), sw, sw).toFloat();
            g.setColour (pal[i]);
            g.fillRoundedRectangle (s, 3.0f);
            if (t->colour == pal[i])
            {
                g.setColour (theme::onAccent);
                g.drawRoundedRectangle (s.reduced (2.0f), 2.0f, 1.5f);
            }
        }
    }
}

void InspectorView::mouseDown (const MouseEvent& e)
{
    if (auto* t = app.track())
    {
        auto& pal = theme::trackPalette();
        const int sw = 16, gap = 4;
        for (int i = 0; i < pal.size(); ++i)
            if (Rectangle<int> (swatchArea.getX() + i * (sw + gap), swatchArea.getY(), sw, sw).contains (e.getPosition()))
            {
                app.project.checkpoint();
                t->colour = pal[i];
                app.project.sendMixChanged();
                app.project.sendContentChanged();
                return;
            }
    }
}

void InspectorView::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (28);
    r.removeFromTop (20);
    swatchArea = r.removeFromTop (22).withTrimmedLeft (12);
    auto cb = r.removeFromTop (26).reduced (12, 2);
    channelBox.setBounds (cb.removeFromLeft (90));
    r.removeFromTop (2);
    {
        auto delayRow = r.removeFromTop (24).reduced (12, 1);
        delayCaption = delayRow.removeFromLeft (78);
        delaySlider.setBounds (delayRow.removeFromLeft (jmin (128, delayRow.getWidth())));
    }
    r.removeFromTop (4);
    const int w = r.getWidth() / 2;
    if (! strip.isVisible())
    {
        // nothing selected: the master keeps its own (right) half and the left half stays empty,
        // so it is the same width as next to a track and nothing jumps when a track is selected
        strip.setBounds ({});
        master.setBounds (r.withTrimmedLeft (w));
        return;
    }
    strip.setBounds (r.removeFromLeft (w));
    master.setBounds (r);
}
}
