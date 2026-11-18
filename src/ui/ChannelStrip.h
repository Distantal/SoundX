#pragma once

#include "AppState.h"
#include "Widgets.h"

namespace sx
{
class SlotButton : public juce::Button
{
public:
    SlotButton() : juce::Button ({}) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void setSlot (const PluginSlot* s, int stacked = 0)
    {
        filled = s != nullptr;
        if (s != nullptr)
        {
            text = s->desc.name;
            // "SX Synth +2" when this track stacks more instruments behind the first one
            if (stacked > 0) text += " +" + juce::String (stacked);
            bypassed = s->bypass;
            post = s->postFader;
        }
        else { text = {}; bypassed = false; post = false; }
        repaint();
    }
    void setPlaceholder (const juce::String& s) { placeholder = s; repaint(); }

    /** What a drag of this slot carries: "track", "uid" and "instrument". */
    std::function<juce::var()> dragSource;

    std::function<void()> onOpen, onMenu, onBypass;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        if (filled)
        {
            // post-fader master inserts are tinted so the position in the chain is obvious
            g.setColour (bypassed ? theme::panelDeep
                                  : (post ? theme::record.withAlpha (0.16f)
                                          : (down ? theme::accentSoft.darker (0.05f) : theme::accentSoft)));
            g.fillRoundedRectangle (r, 3.0f);
            if (post && ! bypassed)
            {
                g.setColour (theme::record.withAlpha (0.55f));
                g.drawRoundedRectangle (r, 3.0f, 1.0f);
            }
            g.setColour (bypassed ? theme::textFaint : (post ? theme::record : theme::accent));
            g.fillEllipse (r.getX() + 6.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f);
            g.setColour (bypassed ? theme::textFaint : theme::text);
            g.setFont (theme::font (12.0f));
            const juce::String label = post ? "POST " + text : text;
            g.drawFittedText (label, getLocalBounds().withTrimmedLeft (18).withTrimmedRight (14), juce::Justification::centredLeft, 1);
            juce::Path p;
            auto cx = r.getRight() - 9.0f, cy = r.getCentreY();
            p.startNewSubPath (cx - 3.0f, cy - 1.5f);
            p.lineTo (cx, cy + 1.5f);
            p.lineTo (cx + 3.0f, cy - 1.5f);
            g.setColour (theme::textDim);
            g.strokePath (p, juce::PathStrokeType (1.3f));
        }
        else
        {
            g.setColour (over ? theme::accentHover : theme::laneB);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (theme::lineSoft);
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.5f));
            g.drawText (placeholder, getLocalBounds(), juce::Justification::centred);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool wasDragging = dragStarted;
        dragStarted = false;
        if (wasDragging) return;                          // the drag & drop already did its job
        if (! getLocalBounds().contains (e.getPosition()) || e.mouseWasDraggedSinceMouseDown()) return;
        if (e.mods.isPopupMenu() || ! filled) { if (onMenu) onMenu(); return; }
        if (e.x < 18) { if (onBypass) onBypass(); return; }
        if (e.x > getWidth() - 16) { if (onMenu) onMenu(); return; }
        if (onOpen) onOpen();
    }

    // dragging a slot onto another slot / strip moves it (same track) or copies it (other track)
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! filled || dragSource == nullptr || dragStarted) return;
        if (e.getDistanceFromDragStart() < 6) return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            dragStarted = true;
            container->startDragging (dragSource(), this);
        }
    }

    void clicked() override {}

private:
    bool filled = false, bypassed = false, post = false, dragStarted = false;
    juce::String text, placeholder { "+" };
};

// Studio One style routing row: a small field with a caption ("OUT" / "SND") on the left, the
// destination name on the right, tinted with the destination's colour, plus a mini level slot
class RoutingRow : public juce::Button
{
public:
    RoutingRow() : juce::Button ({}) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void setContent (const juce::String& cap, const juce::String& value, juce::Colour tint, float level01 = -1.0f)
    {
        caption = cap; text = value; colour = tint; level = level01;
        setTooltip (cap + ": " + value);
        repaint();
    }

    /** Updates only the little level bar (dragging a send does not rebuild the row). */
    void setLevel (float level01)
    {
        level = level01;
        setTooltip (caption + ": " + text + (level >= 0.0f
                                             ? "  ·  " + (level <= 0.0f ? juce::String ("-inf")
                                                                        : juce::String (level * 66.0f - 60.0f, 1) + " dB")
                                             : juce::String()));
        repaint();
    }

    std::function<void()> onMenu;
    /** A send row can be dragged vertically to set any level (0..1), like a small fader. */
    std::function<void (float)> onLevelDrag;
    std::function<void()> onLevelDragStart;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        // send rows are tinted with the send colour so the sends section reads at a glance
        const bool isSend = level >= 0.0f;
        g.setColour (down ? theme::accentSoft
                          : isSend ? colour.withAlpha (0.13f)
                                   : (over ? theme::laneB.brighter (0.06f) : theme::laneB));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (over ? theme::accent.withAlpha (0.6f)
                          : isSend ? colour.withAlpha (0.45f) : theme::lineSoft);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);

        // colour strip of the destination on the left edge
        g.setColour (colour.withAlpha (0.9f));
        g.fillRoundedRectangle (r.getX() + 2.0f, r.getY() + 3.0f, 2.5f, r.getHeight() - 6.0f, 1.2f);

        g.setColour (isSend ? colour.withAlpha (0.85f) : theme::textFaint);
        g.setFont (theme::font (8.5f, true));
        g.drawText (caption, getLocalBounds().withTrimmedLeft (7).removeFromLeft (21), juce::Justification::centredLeft);

        g.setColour (colour);
        g.setFont (theme::font (10.0f, true));
        g.drawFittedText (text, getLocalBounds().withTrimmedLeft (28).withTrimmedRight (level >= 0.0f ? 8 : 4),
                          juce::Justification::centredLeft, 1);

        if (level >= 0.0f)
        {
            // the little send level slot Studio One shows beside a send
            auto bar = juce::Rectangle<float> ((float) getWidth() - 6.0f, r.getY() + 3.0f, 2.5f, r.getHeight() - 6.0f);
            g.setColour (theme::lineSoft);
            g.fillRoundedRectangle (bar, 1.2f);
            const float h = bar.getHeight() * juce::jlimit (0.0f, 1.0f, level);
            g.setColour (colour);
            g.fillRoundedRectangle (bar.withTrimmedTop (bar.getHeight() - h), 1.2f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (level >= 0.0f && onLevelDrag && e.mods.isLeftButtonDown())
        {
            dragStartLevel = level;
            levelDragged = false;
            return;
        }
        juce::Button::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (level >= 0.0f && onLevelDrag)
        {
            if (! levelDragged)
            {
                levelDragged = true;
                if (onLevelDragStart) onLevelDragStart();
            }
            // 120 px cover the whole 66 dB send range (Shift = fine adjustment)
            const float pxPerFullRange = e.mods.isShiftDown() ? 600.0f : 120.0f;
            const float dy = -(float) e.getDistanceFromDragStartY();   // up = louder
            const float newLevel = juce::jlimit (0.0f, 1.0f, dragStartLevel + dy / pxPerFullRange);
            if (std::abs (newLevel - level) > 0.001f)
            {
                setLevel (newLevel);
                onLevelDrag (newLevel);
            }
            return;
        }
        juce::Button::mouseDrag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (level >= 0.0f && onLevelDrag)
        {
            const bool wasDrag = levelDragged;
            levelDragged = false;
            if (! wasDrag && getLocalBounds().contains (e.getPosition()) && onMenu) onMenu();
            return;
        }
        if (! getLocalBounds().contains (e.getPosition()) || e.mouseWasDraggedSinceMouseDown()) return;
        if (onMenu) onMenu();
    }
    void clicked() override {}

private:
    juce::String caption, text;
    juce::Colour colour { theme::textDim };
    float level = -1.0f;
    float dragStartLevel = 0.0f;
    bool levelDragged = false;
};

// JUCE's Slider starts dragging on ANY mouse button, so a right-drag moved the volume
// fader. This version only ever reacts to the left button.
class LeftDragSlider : public juce::Slider
{
public:
    using juce::Slider::Slider;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isLeftButtonDown()) return;
        juce::Slider::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! e.mods.isLeftButtonDown()) return;
        juce::Slider::mouseDrag (e);
    }
};

// pan is shown and typed on a -100..+100 scale ("L 32" / "C" / "R 20")
juce::String panText (double normalised);
bool parsePanText (juce::String s, double& out);

class PhaseButton : public juce::Button
{
public:
    PhaseButton() : juce::Button ("Invert Phase")
    {
        setTooltip (TRANS ("Invert Phase") + " (Ø)");
        setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        g.setColour (on ? theme::warn : (down ? theme::accentSoft : over ? theme::accentHover : theme::white));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? theme::warn : theme::line);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.52f;
        auto c = r.getCentre();
        g.setColour (on ? theme::onAccent : theme::textDim);
        g.drawEllipse (c.x - d * 0.5f, c.y - d * 0.5f, d, d, 1.3f);
        g.drawLine (c.x - d * 0.42f, c.y + d * 0.42f, c.x + d * 0.42f, c.y - d * 0.42f, 1.4f);
    }
};

class ChannelStrip : public juce::Component,
                     public juce::DragAndDropTarget,
                     private Project::Listener,
                     private juce::Timer
{
public:
    // trackId == -1 creates the master strip
    ChannelStrip (AppState& a, int trackId);
    ~ChannelStrip() override;

    void setTrackId (int id);
    // the inspector shows tracks read-only: no automation switch belongs there at all.
    // When disallowed the button is detached from the strip, so no later setVisible()
    // (there are several layout passes) can bring it back.
    void setAutoButtonAllowed (bool ok)
    {
        autoButtonAllowed = ok;
        if (! ok)
        {
            autoBtn.setVisible (false);
            removeChildComponent (&autoBtn);
        }
        else if (autoBtn.getParentComponent() == nullptr)
        {
            addAndMakeVisible (autoBtn);
        }
        updateAutoButton();
    }
    int getTrackId() const { return trackId; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void refresh();
    /** Re-apply the theme colours held by the strip's own labels / buttons. */
    void refreshTheme();

    //== DragAndDropTarget: plug-in slots can be dragged between channel strips
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    /** The drop position a drag would land on: an index into this strip's insert list. */
    int dropTargetIndex() const { return dropIndex; }
    /** Where the drop indicator is drawn (invalid when no drag is over the strip). */
    juce::Rectangle<int> dropIndicatorBounds() const;
    /** Which insert slot the pointer is over (>= size = append at the end). */
    int dropIndexAt (const juce::Point<int>&) const;

    // layout introspection (used by the self test to check the routing block is visible)
    int visibleInsertSlots() const { int n = 0; for (auto& b : inserts) n += b.isVisible() ? 1 : 0; return n; }
    int visibleSendRows() const { int n = 0; for (auto& b : sendRows) n += b.isVisible() ? 1 : 0; return n; }
    bool slotButtonVisible() const { return slotButton.isVisible(); }
    bool outputRowVisible() const { return outputRow.isVisible(); }
    bool addSendRowVisible() const { return addSendRow.isVisible(); }
    bool panVisible() const { return pan.isVisible(); }          // the listen bus has no pan
    bool listenButtonShown() const { return listenSoloBtn.isVisible() && listenSoloBtn.getWidth() > 0; }
    juce::String listenButtonDebug() const
    {
        return juce::String (listenSoloBtn.isVisible() ? "vis" : "hidden") + " btn="
             + juce::String (listenSoloBtn.getX()) + "," + juce::String (listenSoloBtn.getY()) + " "
             + juce::String (listenSoloBtn.getWidth()) + "x" + juce::String (listenSoloBtn.getHeight())
             + " strip=" + juce::String (getWidth()) + "x" + juce::String (getHeight());
    }
    bool automationShown() const { return autoBtn.isVisible(); }
    bool meterVisible() const { return meter.isVisible(); }
    juce::Rectangle<int> meterBounds() const { return meter.getBounds(); }
    // the master strip shows M + MONO (its automation lives in the strip menu)
    bool masterSwitchesShown() const { return muteBtn.isVisible() && monoBtn.isVisible(); }
    bool masterMonoShown() const { return monoBtn.isVisible(); }
    bool masterAutoButtonShown() const { return autoBtn.isVisible(); }
    juce::Rectangle<int> autoButtonBounds() const { return autoBtn.getBounds(); }

    static constexpr int kInserts = 10;
    static constexpr int kSends = 3;

private:
    void projectMixChanged() override { refresh(); }
    void projectStructureChanged() override { refresh(); }
    void projectContentChanged() override { refresh(); }
    void timerCallback() override;
    void updateAutoButton();
    /** Height-tier visibility, shared by resized() and refresh(): a mix notification must never
        re-show a control that the current strip height has dropped (stale bounds stacked on top
        of the fader / routing rows). */
    void applyTierVisibility();
    void faderChanged();
    void panChanged();
    void setPanText (double normalised);
    void commitPanText();
    void commitVolumeText();
    void setDbText (const juce::String& s);
    void showInputMenu();
    void showNameMenu();
    void showMasterMenu();
    void showSendMenu (int fromTrackId, int targetId);
    void showOutputMenu();
    void showAddSendMenu();
    Track* track() const { return app.project.findTrack (trackId); }
    // trackId == -1 is the master strip: its channel lives in Project::masterTrack
    Track* channel() const { return trackId == -1 ? &app.project.masterTrack : app.project.findTrack (trackId); }

    AppState& app;
    int trackId;
    bool updating = false;
    bool autoButtonAllowed = true;
    bool monitorBusStrip = false;      // this strip is the listen bus (kept in sync by refresh())

    juce::Label nameLabel;
    SlotButton slotButton;
    SlotButton inserts[kInserts];
    // drag & drop state: which insert slot the pointer is over while a slot is being dragged
    int dropIndex = -1;
    RoutingRow sendRows[kSends];
    RoutingRow addSendRow;
    RoutingRow outputRow;
    LeftDragSlider pan { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    LeftDragSlider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::Label panLabel;      // shows L/R number; double-click to type one
    juce::Label dbLabel;
    LevelMeter meter;
    PanMeter panMeter;                     // where the channel sits in the stereo field
    StereoScope scope;                     // master only: the mix on a goniometer
    TextToggle muteBtn { "M", theme::accentDark, TRANS ("Mute") };
    PhaseButton phaseBtn;
    TextToggle monoBtn { "MONO", theme::accent, TRANS ("Mono") };   // master strip: mono / stereo
    TextToggle soloBtn { "S", theme::accent, TRANS ("Solo") };
    TextToggle armBtn { "R", theme::record, TRANS ("Record Enable") };
    TextToggle monBtn { "I", theme::accent, TRANS ("Input Monitoring") };
    TextToggle listenBtn { "ON", theme::warn, TRANS ("Listen bus on / off") };   // listen bus only
    TextToggle listenSoloBtn { "L", theme::warn, TRANS ("Listen through the listen bus") };
    juce::TextButton autoBtn;
    int lastInsertCount = -1;
};
}