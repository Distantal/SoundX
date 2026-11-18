#include "BuiltInEffects.h"

namespace sx
{
using namespace juce;

namespace
{
    const Colour kSpBg     { 0xff14161a };
    const Colour kSpPanel  { 0xff1d2026 };
    const Colour kSpGrid   { 0xff2a2f37 };
    const Colour kSpText   { 0xffe9edf5 };
    const Colour kSpDim    { 0xff8b97ab };
    const Colour kSpAccent { 0xff38bdf8 };
    const Colour kSpPeer   { 0xff7b8aa3 };

    float wrapDeg (float a)
    {
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }

    //==========================================================================
    /** The room: a top view with the listener in the middle, the front at the top. The dragged track
        follows the pointer; the other tracks of its link group move with it, and the faint marks show
        where each track is actually placed after auto-spacing. */
    class SpaceEditor : public AudioProcessorEditor, private Timer
    {
    public:
        explicit SpaceEditor (SxStereo& s) : AudioProcessorEditor (&s), fx (s)
        {
            struct Row { int idx; const char* name; };
            static const Row rows[] {
                { SxStereo::pAzimuth, "Azimuth" }, { SxStereo::pDistance, "Distance" }, { SxStereo::pWidth, "Width" },
                { SxStereo::pRoom, "Room" }, { SxStereo::pSpacing, "Spacing" }, { SxStereo::pBass, "Bass Mono" },
                { SxStereo::pOutput, "Output" },
            };
            for (auto& row : rows)
            {
                const auto& sp = fx.spec (row.idx);
                auto* label = labels.add (new Label ({}, row.name));
                label->setFont (Font (FontOptions (11.0f)));
                label->setColour (Label::textColourId, kSpDim);
                label->setJustificationType (Justification::centredLeft);
                addAndMakeVisible (label);

                auto* sl = sliders.add (new Slider (Slider::LinearHorizontal, Slider::TextBoxRight));
                sl->setRange (sp.min, sp.max, 0.0);
                sl->setTextValueSuffix (sp.suffix);
                sl->setNumDecimalPlacesToDisplay (row.idx == SxStereo::pOutput ? 1 : 0);
                sl->setColour (Slider::trackColourId, kSpGrid);
                sl->setColour (Slider::thumbColourId, kSpAccent);
                sl->setColour (Slider::textBoxTextColourId, kSpText);
                sl->setColour (Slider::textBoxBackgroundColourId, kSpPanel);
                sl->setColour (Slider::textBoxOutlineColourId, kSpGrid);
                const int idx = row.idx;
                sl->onDragStart = [this, idx] { gesture (idx, true); };
                sl->onDragEnd = [this, idx] { gesture (idx, false); };
                sl->onValueChange = [this, idx, sl]
                {
                    if (! syncing) setParam (idx, (float) sl->getValue());
                };
                addAndMakeVisible (sl);
                sliderIndex.add (idx);
            }

            linkLabel.setText ("Link group", dontSendNotification);
            linkLabel.setFont (Font (FontOptions (11.0f)));
            linkLabel.setColour (Label::textColourId, kSpDim);
            addAndMakeVisible (linkLabel);
            for (int g = 0; g <= 8; ++g)
            {
                auto* b = groupButtons.add (new TextButton (g == 0 ? String ("-") : String (g)));
                b->setClickingTogglesState (true);
                b->setRadioGroupId (91);
                b->setColour (TextButton::buttonColourId, kSpPanel);
                b->setColour (TextButton::buttonOnColourId, kSpAccent.withAlpha (0.45f));
                b->setColour (TextButton::textColourOffId, kSpDim);
                b->setColour (TextButton::textColourOnId, Colours::white);
                b->onClick = [this, g] { setParam (SxStereo::pGroup, (float) g); };
                addAndMakeVisible (b);
            }

            setResizable (true, false);
            setResizeLimits (480, 380, 1100, 820);
            setSize (640, 480);
            syncControls();
            startTimerHz (30);
        }

        ~SpaceEditor() override { stopTimer(); }

        void paint (Graphics& g) override
        {
            g.fillAll (kSpBg);
            auto head = getLocalBounds().removeFromTop (34);
            g.setColour (kSpPanel);
            g.fillRect (head);
            g.setColour (kSpAccent);
            g.setFont (Font (FontOptions (13.0f)).boldened());
            g.drawText ("SX STEREO  -  SPACE", head.reduced (12, 0), Justification::centredLeft);
            g.setColour (kSpDim);
            g.setFont (Font (FontOptions (10.5f)));
            g.drawText ("drag the source  -  Shift for fine  -  wheel for distance  -  double-click for the centre",
                        head.reduced (12, 0), Justification::centredRight);

            paintRoom (g);

            // footer: where the track is, and how many tracks it is linked to
            const int grp = fx.group();
            const auto peers = fx.groupPeers();
            g.setColour (kSpDim);
            g.setFont (Font (FontOptions (10.5f)));
            g.drawText ("azimuth " + String (fx.param (SxStereo::pAzimuth), 0) + " deg   distance " + String (fx.param (SxStereo::pDistance), 0)
                        + " %   placed at " + String (fx.placedAzimuth(), 0) + " deg   level " + String (fx.levelDb(), 1)
                        + " dB" + (grp > 0 ? "   group " + String (grp) + ": " + String ((int) peers.size()) + " linked" : String()),
                        footer, Justification::centredLeft);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            r.removeFromTop (34);
            footer = r.removeFromBottom (26).reduced (12, 4);
            auto left = r.removeFromLeft (jmax (240, (int) (r.getWidth() * 0.55f))).reduced (10);
            const int side = jmin (left.getWidth(), left.getHeight());
            roomArea = left.withSizeKeepingCentre (side, side);
            radius = (float) roomArea.getWidth() * 0.5f - 22.0f;

            auto controls = r.reduced (8, 12);
            for (int i = 0; i < sliders.size(); ++i)
            {
                auto row = controls.removeFromTop (30);
                labels[i]->setBounds (row.removeFromLeft (86));
                sliders[i]->setBounds (row);
                controls.removeFromTop (4);
            }
            controls.removeFromTop (12);
            linkLabel.setBounds (controls.removeFromTop (18));
            controls.removeFromTop (4);
            auto buttons = controls.removeFromTop (26);
            const int bw = buttons.getWidth() / 9;
            for (int i = 0; i < groupButtons.size(); ++i)
                groupButtons[i]->setBounds (buttons.getX() + i * bw + 1, buttons.getY(), bw - 2, buttons.getHeight());
        }

        void mouseDown (const MouseEvent& e) override
        {
            if (! roomArea.contains (e.getPosition())) return;
            if (e.getNumberOfClicks() >= 2)
            {
                setPosition (fx.param (SxStereo::pAzimuth), 0.0f);
                return;
            }
            dragging = true;
            gesture (SxStereo::pAzimuth, true);
            gesture (SxStereo::pDistance, true);
            applyPointer (e.getPosition().toFloat(), e.mods.isShiftDown());
        }

        void mouseDrag (const MouseEvent& e) override
        {
            if (dragging) applyPointer (e.getPosition().toFloat(), e.mods.isShiftDown());
        }

        void mouseUp (const MouseEvent&) override
        {
            if (! dragging) return;
            dragging = false;
            gesture (SxStereo::pAzimuth, false);
            gesture (SxStereo::pDistance, false);
        }

        void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
        {
            if (! roomArea.contains (e.getPosition())) return;
            const float dist = jlimit (0.0f, 100.0f, fx.param (SxStereo::pDistance) + (w.deltaY > 0.0f ? 2.0f : -2.0f));
            setPosition (fx.param (SxStereo::pAzimuth), dist);
        }

    private:
        Point<float> pointAt (float az, float dist) const
        {
            const auto c = roomArea.toFloat().getCentre();
            const float rad = az * MathConstants<float>::pi / 180.0f;
            return { c.x + std::sin (rad) * radius * dist / 100.0f, c.y - std::cos (rad) * radius * dist / 100.0f };
        }

        /** Moves the dragged track to the pointer; the linked tracks move by the same step. */
        void applyPointer (Point<float> p, bool fine)
        {
            const auto c = roomArea.toFloat().getCentre();
            const float dx = p.x - c.x, dy = p.y - c.y;
            float dist = jlimit (0.0f, 100.0f, std::sqrt (dx * dx + dy * dy) / radius * 100.0f);
            float az = std::atan2 (dx, -dy) * 180.0f / MathConstants<float>::pi;
            const float curAz = fx.param (SxStereo::pAzimuth);
            const float curDist = fx.param (SxStereo::pDistance);
            if (fine)
            {
                az = wrapDeg (curAz + wrapDeg (az - curAz) * 0.25f);
                dist = curDist + (dist - curDist) * 0.25f;
            }
            setPosition (az, dist);
            if (fx.group() > 0)
                fx.shiftGroup (wrapDeg (az - curAz), dist - curDist);
        }

        void setPosition (float az, float dist)
        {
            setParam (SxStereo::pAzimuth, wrapDeg (az));
            setParam (SxStereo::pDistance, dist);
            repaint();
        }

        void paintRoom (Graphics& g)
        {
            const auto c = roomArea.toFloat().getCentre();
            g.setColour (Colour (0xff0e1015));
            g.fillEllipse (roomArea.toFloat());
            g.setColour (kSpGrid);
            g.drawEllipse (roomArea.toFloat().reduced (0.5f), 1.0f);
            for (float lev : { 0.25f, 0.5f, 0.75f })
                g.drawEllipse (c.x - radius * lev, c.y - radius * lev, radius * lev * 2.0f, radius * lev * 2.0f, 1.0f);
            for (int k = 0; k < 8; ++k)
            {
                const float rad = (float) k * MathConstants<float>::pi * 0.25f;
                g.drawLine (c.x, c.y, c.x + std::sin (rad) * radius, c.y - std::cos (rad) * radius, 1.0f);
            }
            g.setColour (kSpDim);
            g.setFont (Font (FontOptions (10.0f)));
            g.drawText ("FRONT", (int) c.x - 40, (int) (c.y - radius - 18), 80, 14, Justification::centred);
            g.drawText ("BACK", (int) c.x - 40, (int) (c.y + radius + 4), 80, 14, Justification::centred);
            g.drawText ("L", (int) (c.x - radius - 16), (int) c.y - 7, 14, 14, Justification::centred);
            g.drawText ("R", (int) (c.x + radius + 2), (int) c.y - 7, 14, 14, Justification::centred);

            // the listener
            g.setColour (kSpPanel);
            g.fillEllipse (c.x - 9.0f, c.y - 9.0f, 18.0f, 18.0f);
            g.setColour (kSpDim);
            g.drawEllipse (c.x - 9.0f, c.y - 9.0f, 18.0f, 18.0f, 1.0f);

            // every track of the project with this plug-in, at its own position and under its name; the
            // tracks of this one's link group are joined to it
            const int grp = fx.group();
            const auto self = pointAt (fx.param (SxStereo::pAzimuth), fx.param (SxStereo::pDistance));
            for (auto* o : fx.sceneMembers())
            {
                if (o == &fx) continue;
                const auto q = pointAt (o->param (SxStereo::pAzimuth), o->param (SxStereo::pDistance));
                const bool linked = grp > 0 && o->group() == grp;
                if (linked)
                {
                    g.setColour (kSpAccent.withAlpha (0.25f));
                    g.drawLine (self.x, self.y, q.x, q.y, 1.0f);
                }
                g.setColour (linked ? kSpAccent.withAlpha (0.8f) : kSpPeer);
                g.fillEllipse (q.x - 5.0f, q.y - 5.0f, 10.0f, 10.0f);
                drawName (g, { q.x, q.y + 9.0f }, o->trackName(), linked ? kSpText : kSpDim);
                const auto placed = pointAt (o->placedAzimuth(), o->param (SxStereo::pDistance));
                if (placed.getDistanceFrom (q) > 2.0f)
                {
                    g.setColour (kSpPeer.withAlpha (0.6f));
                    g.drawEllipse (placed.x - 6.0f, placed.y - 6.0f, 12.0f, 12.0f, 1.0f);
                }
            }

            // this track: the level as a ring, the placed position as a ghost when auto-spacing moved it
            const float lev = jlimit (0.0f, 1.0f, (fx.levelDb() + 60.0f) / 60.0f);
            g.setColour (kSpAccent.withAlpha (0.18f + 0.4f * lev));
            g.fillEllipse (self.x - 9.0f - 10.0f * lev, self.y - 9.0f - 10.0f * lev, 18.0f + 20.0f * lev, 18.0f + 20.0f * lev);
            const auto placed = pointAt (fx.placedAzimuth(), fx.param (SxStereo::pDistance));
            if (placed.getDistanceFrom (self) > 2.0f)
            {
                g.setColour (kSpAccent.withAlpha (0.6f));
                g.drawLine (self.x, self.y, placed.x, placed.y, 1.0f);
                g.drawEllipse (placed.x - 7.0f, placed.y - 7.0f, 14.0f, 14.0f, 1.0f);
            }
            g.setColour (kSpAccent);
            g.fillEllipse (self.x - 8.0f, self.y - 8.0f, 16.0f, 16.0f);
            g.setColour (Colours::white);
            g.drawEllipse (self.x - 8.0f, self.y - 8.0f, 16.0f, 16.0f, 1.5f);
            drawName (g, { self.x, self.y + 13.0f }, fx.trackName(), kSpText);
        }

        /** A track name under its dot: the name of the track, cut short; "Track" when it has none. */
        static void drawName (Graphics& g, Point<float> at, const String& name, Colour colour)
        {
            String shown = name.isEmpty() ? String ("Track") : name;
            if (shown.length() > 16) shown = shown.substring (0, 15) + "...";
            g.setColour (colour);
            g.setFont (Font (FontOptions (10.0f)).boldened());
            g.drawText (shown, (int) at.x - 64, (int) at.y, 128, 12, Justification::centred);
        }

        void gesture (int idx, bool begin)
        {
            if (auto* p = fx.getParameters()[idx])
            {
                if (begin) p->beginChangeGesture();
                else p->endChangeGesture();
            }
        }

        void setParam (int idx, float v)
        {
            if (auto* p = dynamic_cast<RangedAudioParameter*> (fx.getParameters()[idx]))
                fx.getParameters()[idx]->setValueNotifyingHost (p->convertTo0to1 (v));
        }

        /** Shows the parameters in the controls (not while the user is dragging one of them). */
        void syncControls()
        {
            syncing = true;
            for (int i = 0; i < sliders.size(); ++i)
                if (! sliders[i]->isMouseButtonDown())
                    sliders[i]->setValue (fx.param (sliderIndex[i]), dontSendNotification);
            const int g = fx.group();
            for (int i = 0; i < groupButtons.size(); ++i)
                groupButtons[i]->setToggleState (i == g, dontSendNotification);
            syncing = false;
        }

        void timerCallback() override
        {
            syncControls();

            const auto stamp = fx.displayStamp();
            if (stamp == lastStamp) return;
            lastStamp = stamp;
            repaint();
        }

        SxStereo& fx;
        OwnedArray<Label> labels;
        OwnedArray<Slider> sliders;
        Array<int> sliderIndex;
        Label linkLabel;
        OwnedArray<TextButton> groupButtons;
        Rectangle<int> roomArea, footer;
        float radius = 100.0f;
        bool dragging = false, syncing = false;
        std::uint64_t lastStamp = 0;
    };
}

AudioProcessorEditor* SxStereo::createEditor() { return new SpaceEditor (*this); }

} // namespace sx
