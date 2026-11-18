#include "GuitarRig.h"

#include <algorithm>

namespace sx
{
using namespace juce;

//==============================================================================
// SX Guitar Rig editor. Flat and dim on purpose: solid fills, hairline rules, no gradients. Top to
// bottom: the pedalboard, the detail strip of the selected pedal, the amp selector and the amp head
// (laid out after the reference photos), then the cabinet row. Every knob and switch is bound to a
// parameter.
namespace
{
    const Colour kRigBg     { 0xff111317 };
    const Colour kRigPanel  { 0xff181b21 };
    const Colour kRigPanel2 { 0xff20242c };
    const Colour kRigLine   { 0xff282d37 };
    const Colour kRigInk    { 0xff0c0e12 };
    const Colour kRigText   { 0xffc4cad4 };
    const Colour kRigDim    { 0xff6a7384 };
    const Colour kRigAccent { 0xff4d7fb8 };
    const Colour kSilver    { 0xffb4bac4 };
    const Colour kLed       { 0xffe0705f };

    constexpr int kPanelW = 1040;
    constexpr int kPanelH = 740;

    enum class KnobStyle { Fender, Gold, Silver, Pedal };

    struct KnobDef { int param; const char* caption; };

    struct SectionDef
    {
        const char* title;
        int onParam;                      // -1: no switch in the detail strip
        std::vector<KnobDef> knobs;
    };

    struct PedalDef
    {
        const char* name;
        Colour body, text, knob;
        std::vector<int> leds;            // footswitch LEDs on the box (on/off parameters)
        std::vector<const char*> ledLabels;
        std::vector<SectionDef> sections;
    };

    const std::vector<PedalDef>& rigPedals()
    {
        using P = SxGuitarRig;
        static const std::vector<PedalDef> defs {
            { "FENDER COMP", Colour (0xff4a505a), Colour (0xffd0d5dd), Colour (0xff8d95a1),
              { P::pCompOn }, { "ON" },
              { { "", -1, { { P::pCompSustain, "Sustain" }, { P::pCompAttack, "Attack" }, { P::pCompLevel, "Level" } } } } },
            { "CLEAN BOOST", Colour (0xff2c4f70), Colour (0xffd0dbe8), Colour (0xff5f8fbd),
              { P::pBoostOn }, { "ON" },
              { { "", -1, { { P::pBoostGain, "Gain" }, { P::pBoostTreble, "Treble" }, { P::pBoostLevel, "Level" } } } } },
            { "TS808 OD", Colour (0xff2b5a40), Colour (0xffd0e6d8), Colour (0xff5fa27a),
              { P::pTsOn }, { "ON" },
              { { "", -1, { { P::pTsDrive, "Drive" }, { P::pTsTone, "Tone" }, { P::pTsLevel, "Level" } } } } },
            { "YELLOW OD", Colour (0xff7d6d2a), Colour (0xfff0e8c8), Colour (0xffc2aa4a),
              { P::pOdOn }, { "ON" },
              { { "", -1, { { P::pOdDrive, "Drive" }, { P::pOdTone, "Tone" }, { P::pOdLevel, "Level" } } } } },
            { "FUZZ", Colour (0xff6b2d2b), Colour (0xffeccfcd), Colour (0xffb55a55),
              { P::pFuzzOn }, { "ON" },
              { { "", -1, { { P::pFuzzDrive, "Fuzz" }, { P::pFuzzTone, "Tone" }, { P::pFuzzLevel, "Level" } } } } },
            { "REVERB DELAY CHORUS TREM", Colour (0xff22262e), Colour (0xffc4cad4), kRigAccent,
              { P::pRevOn, P::pDlyOn, P::pChoOn, P::pTremOn }, { "REV", "DLY", "CHO", "TRM" },
              { { "REVERB", P::pRevOn, { { P::pRevMix, "Mix" }, { P::pRevDecay, "Decay" }, { P::pRevTone, "Tone" } } },
                { "DELAY", P::pDlyOn, { { P::pDlyTime, "Time" }, { P::pDlyFeedback, "Repeats" }, { P::pDlyMix, "Mix" } } },
                { "CHORUS", P::pChoOn, { { P::pChoRate, "Rate" }, { P::pChoDepth, "Depth" }, { P::pChoMix, "Mix" } } },
                { "TREMOLO", P::pTremOn, { { P::pTremRate, "Rate" }, { P::pTremDepth, "Depth" } } } } },
        };
        return defs;
    }

    //==========================================================================
    /** A knob bound to one parameter: drag up / down, wheel, double-click to type, Alt-click to reset. */
    class RigKnob : public Component, private SettableTooltipClient
    {
    public:
        RigKnob (SxGuitarRig& e, int index, KnobStyle s, Colour accent, String cap)
            : fx (e), idx (index), style (s), accentColour (accent), caption (std::move (cap))
        {
            const auto& sp = fx.spec (idx);
            setTooltip (String (sp.name) + " (" + String (sp.min, 1) + " .. " + String (sp.max, 1) + String (sp.suffix) + ")");
            setMouseCursor (MouseCursor::UpDownResizeCursor);
        }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const auto valueArea = r.removeFromBottom (13.0f);
            const auto nameArea = r.removeFromBottom (13.0f);
            const float d = jmin (r.getWidth(), r.getHeight()) - 6.0f;
            const auto knob = r.withSizeKeepingCentre (d, d);
            const auto centre = knob.getCentre();
            const float norm = (float) fx.getParameters()[idx]->getValue();
            const float start = MathConstants<float>::pi * 1.25f;
            const float end = MathConstants<float>::pi * 2.75f;
            const float angle = start + norm * (end - start);

            Colour body, pointer, arcCol, captionCol;
            switch (style)
            {
                case KnobStyle::Fender:
                    body = Colour (0xff1d2026); pointer = Colour (0xffd9dde3);
                    arcCol = Colour (0xff9aa3b0); captionCol = Colour (0xff9aa1ad);
                    break;
                case KnobStyle::Gold:
                    body = Colour (0xff9c8358); pointer = Colour (0xff17140e);
                    arcCol = Colour (0xffc2a86f); captionCol = Colour (0xffb9a578);
                    break;
                case KnobStyle::Silver:
                    body = Colour (0xff8a919d); pointer = Colour (0xff15171b);
                    arcCol = Colour (0xffb4bcc8); captionCol = Colour (0xff9aa1ad);
                    break;
                case KnobStyle::Pedal:
                default:
                    body = accentColour.darker (0.35f);
                    pointer = accentColour.getBrightness() > 0.55f ? Colour (0xff111318) : Colour (0xffe6eaf0);
                    arcCol = accentColour; captionCol = kRigDim;
                    break;
            }

            const float ringR = d * 0.5f + 3.0f;
            Path track;
            track.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f, start, end, true);
            g.setColour (kRigLine);
            g.strokePath (track, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
            Path arc;
            arc.addCentredArc (centre.x, centre.y, ringR, ringR, 0.0f, start, angle, true);
            g.setColour (arcCol);
            g.strokePath (arc, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));

            g.setColour (body);
            g.fillEllipse (knob.reduced (3.0f));

            const auto tip = centre + Point<float> (std::cos (angle), std::sin (angle)) * (d * 0.34f);
            g.setColour (pointer);
            g.drawLine (Line<float> (centre, tip), 2.4f);

            g.setColour (captionCol);
            g.setFont (Font (FontOptions (11.0f)));
            g.drawFittedText (caption, nameArea.toNearestInt(), Justification::centred, 1);

            const auto& s = fx.spec (idx);
            const int decimals = (s.max - s.min) <= 12.0f ? 1 : 0;
            g.setColour (style == KnobStyle::Pedal ? kRigText : captionCol);
            g.setFont (Font (FontOptions (10.5f)).boldened());
            g.drawFittedText (String (fx.param (idx), decimals) + s.suffix, valueArea.toNearestInt(), Justification::centred, 1);
        }

        void mouseEnter (const MouseEvent&) override { hover = true; repaint(); }
        void mouseExit (const MouseEvent&) override { hover = false; repaint(); }

        void mouseDown (const MouseEvent& e) override
        {
            if (e.mods.isAltDown())
            {
                setNormalised (fx.spec (idx).def, true);
                return;
            }
            dragStartY = e.getPosition().y;
            startNorm = (float) fx.getParameters()[idx]->getValue();
            fx.getParameters()[idx]->beginChangeGesture();
        }

        void mouseDrag (const MouseEvent& e) override
        {
            const float delta = (float) (dragStartY - e.getPosition().y) / (e.mods.isShiftDown() ? 600.0f : 180.0f);
            auto* p = fx.getParameters()[idx];
            p->setValueNotifyingHost (jlimit (0.0f, 1.0f, startNorm + delta));
            repaint();
        }

        void mouseUp (const MouseEvent&) override
        {
            fx.getParameters()[idx]->endChangeGesture();
        }

        void mouseDoubleClick (const MouseEvent&) override { showValueEditor(); }

        void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) override
        {
            auto* p = fx.getParameters()[idx];
            p->beginChangeGesture();
            p->setValueNotifyingHost (jlimit (0.0f, 1.0f, p->getValue() + (w.deltaY > 0.0f ? 0.02f : -0.02f)));
            p->endChangeGesture();
            repaint();
        }

    private:
        void setNormalised (float realValue, bool gesture)
        {
            auto* p = fx.getParameters()[idx];
            if (gesture) p->beginChangeGesture();
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (realValue));
            if (gesture) p->endChangeGesture();
            repaint();
        }

        void showValueEditor()
        {
            const auto& s = fx.spec (idx);
            auto* aw = new AlertWindow (String (s.name), "Enter a value (" + String (s.min, 2) + " .. "
                                        + String (s.max, 2) + String (s.suffix) + ")", MessageBoxIconType::NoIcon);
            aw->addTextEditor ("v", String (fx.param (idx), 3), "");
            aw->addButton (TRANS ("OK"), 1, KeyPress (KeyPress::returnKey));
            aw->addButton (TRANS ("Cancel"), 0, KeyPress (KeyPress::escapeKey));
            SxGuitarRig* proc = &fx;
            const int index = idx;
            aw->enterModalState (true, ModalCallbackFunction::create ([proc, index, aw, s] (int result)
            {
                if (result != 1) return;
                const float v = jlimit (s.min, s.max, aw->getTextEditorContents ("v").getFloatValue());
                if (auto* p = proc->getParameters()[index])
                {
                    p->beginChangeGesture();
                    if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
                    p->endChangeGesture();
                }
            }), true);
        }

        SxGuitarRig& fx;
        int idx;
        KnobStyle style;
        Colour accentColour;
        String caption;
        int dragStartY = 0;
        float startNorm = 0.0f;
        bool hover = false;
    };

    //==========================================================================
    /** A row of segments; each one sets the parameter to its index (switches, selectors). */
    class RigChoice : public Component
    {
    public:
        RigChoice (SxGuitarRig& e, int index, std::initializer_list<const char*> names, Colour accent)
            : fx (e), idx (index), accentColour (accent)
        {
            for (auto* n : names) labels.add (n);
        }

        int current() const
        {
            return jlimit (0, jmax (0, labels.size() - 1), (int) std::round (fx.param (idx)));
        }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (kRigInk);
            g.fillRoundedRectangle (r, 3.0f);

            const int n = labels.size();
            if (n == 0) return;
            const float w = r.getWidth() / (float) n;
            const int cur = current();
            for (int i = 0; i < n; ++i)
            {
                auto seg = Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (2.0f);
                const bool on = i == cur;
                g.setColour (on ? accentColour.withAlpha (0.75f) : kRigPanel2);
                g.fillRoundedRectangle (seg, 2.0f);
                g.setColour (on ? kRigInk : kRigDim);
                g.setFont (Font (FontOptions (10.5f)).boldened());
                g.drawFittedText (labels[i], seg.toNearestInt(), Justification::centred, 1);
            }
        }

        void mouseDown (const MouseEvent& e) override
        {
            const int n = labels.size();
            if (n == 0) return;
            const int i = jlimit (0, n - 1, (int) ((float) e.x / jmax (1.0f, (float) getWidth()) * (float) n));
            if (auto* p = fx.getParameters()[idx])
            {
                p->beginChangeGesture();
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 ((float) i));
                p->endChangeGesture();
            }
            repaint();
        }

    private:
        SxGuitarRig& fx;
        int idx;
        Colour accentColour;
        StringArray labels;
    };

    //==========================================================================
    /** One stomp box: name, footswitch LEDs (click to toggle), body click selects it for editing. */
    class PedalBox : public Component
    {
    public:
        PedalBox (SxGuitarRig& e, int index, const PedalDef& d, bool canDrag,
                  std::function<void (int)> onSelect, std::function<void (int, int)> onDrop)
            : fx (e), pedalIndex (index), def (d), draggable (canDrag),
              select (std::move (onSelect)), drop (std::move (onDrop)) {}

        void setSelected (bool s) { selected = s; repaint(); }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (def.body);
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (selected ? def.text.withAlpha (0.8f) : kRigLine);
            g.drawRoundedRectangle (r, 6.0f, selected ? 2.0f : 1.0f);

            g.setColour (def.text);
            g.setFont (Font (FontOptions (12.5f)).boldened());
            g.drawFittedText (def.name, getLocalBounds().withTrimmedBottom (40).reduced (8, 6), Justification::centred, 2);

            const int n = (int) def.leds.size();
            for (int i = 0; i < n; ++i)
            {
                const auto cell = ledCell (i).toFloat();
                const bool on = fx.param (def.leds[(size_t) i]) > 0.5f;
                const auto dot = Rectangle<float> (10.0f, 10.0f).withCentre ({ cell.getCentreX(), cell.getY() + 10.0f });
                g.setColour (on ? kLed : def.text.withAlpha (0.16f));
                g.fillEllipse (dot);
                g.setColour (def.text.withAlpha (0.7f));
                g.setFont (Font (FontOptions (10.0f)).boldened());
                g.drawFittedText (def.ledLabels[(size_t) i], Rectangle<int> ((int) cell.getX(), (int) cell.getY() + 20, (int) cell.getWidth(), 14),
                                  Justification::centred, 1);
            }
        }

        void mouseDown (const MouseEvent& e) override
        {
            dragging = false;
            for (int i = 0; i < (int) def.leds.size(); ++i)
            {
                if (ledCell (i).contains (e.getPosition()))
                {
                    toggle (def.leds[(size_t) i]);
                    return;
                }
            }
            select (pedalIndex);
            grabOffsetX = e.getEventRelativeTo (getParentComponent()).x - getX();
        }

        // dragging moves the box with the pointer; on release it snaps to the slot under its centre
        void mouseDrag (const MouseEvent& e) override
        {
            if (! draggable) return;
            if (! dragging && std::abs (e.getDistanceFromDragStartX()) < 6) return;
            dragging = true;
            setTopLeftPosition (e.getEventRelativeTo (getParentComponent()).x - grabOffsetX, getY());
            toFront (false);
        }

        void mouseUp (const MouseEvent&) override
        {
            if (! dragging) return;
            dragging = false;
            const int stride = getWidth() + 8;
            const int slot = jlimit (0, 4, (getX() + getWidth() / 2 - 10) / stride);
            drop (pedalIndex, slot);
        }

    private:
        Rectangle<int> ledCell (int i) const
        {
            const int n = jmax (1, (int) def.leds.size());
            const int cellW = getWidth() / n;
            return { i * cellW, getHeight() - 40, cellW, 40 };
        }

        void toggle (int paramIndex)
        {
            if (auto* p = fx.getParameters()[paramIndex])
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
                p->endChangeGesture();
            }
            repaint();
        }

        SxGuitarRig& fx;
        int pedalIndex;
        const PedalDef& def;
        bool draggable;
        std::function<void (int)> select;
        std::function<void (int, int)> drop;
        bool selected = false;
        bool dragging = false;
        int grabOffsetX = 0;
    };

    //==========================================================================
    /** The strip under the pedalboard: the knobs of the selected pedal, or four sections for the
        time block (each with its own on / off switch). */
    class DetailStrip : public Component
    {
    public:
        explicit DetailStrip (SxGuitarRig& e) : fx (e) {}

        void showPedal (const PedalDef& d)
        {
            items.clear();
            itemSection.clear();
            itemIsSwitch.clear();
            titles.clear();
            heading = d.name;
            for (int s = 0; s < (int) d.sections.size(); ++s)
            {
                const auto& sec = d.sections[(size_t) s];
                titles.add (sec.title);
                if (sec.onParam >= 0)
                    addItem (new RigChoice (fx, sec.onParam, { "OFF", "ON" }, kRigAccent), s, true);
                for (auto& k : sec.knobs)
                    addItem (new RigKnob (fx, k.param, KnobStyle::Pedal, d.knob, k.caption), s, false);
            }
            resized();
            repaint();
        }

        void paint (Graphics& g) override
        {
            g.setColour (kRigPanel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

            const int ns = titles.size();
            if (ns == 0) return;
            const int secW = getWidth() / ns;
            for (int s = 0; s < ns; ++s)
            {
                if (s > 0)
                {
                    g.setColour (kRigLine);
                    g.drawVerticalLine (s * secW, 10.0f, (float) getHeight() - 10.0f);
                }
                g.setColour (kRigText);
                g.setFont (Font (FontOptions (11.5f)).boldened());
                const String text = ns == 1 ? heading : String (titles[s]);
                g.drawText (text, s * secW + 12, 3, secW - 100, 18, Justification::centredLeft);
            }
        }

        void resized() override
        {
            const int ns = titles.size();
            if (ns == 0) return;
            const int secW = getWidth() / ns;
            for (int s = 0; s < ns; ++s)
            {
                int nk = 0;
                for (int i = 0; i < items.size(); ++i)
                    if (itemSection[(size_t) i] == s && ! itemIsSwitch[(size_t) i]) ++nk;
                const int knobW = jmax (1, jmin (96, secW / jmax (1, nk)));
                const int x0 = s * secW + (secW - knobW * nk) / 2;
                int k = 0;
                for (int i = 0; i < items.size(); ++i)
                {
                    if (itemSection[(size_t) i] != s) continue;
                    if (itemIsSwitch[(size_t) i])
                        items[i]->setBounds (s * secW + secW - 80, 3, 72, 18);
                    else
                    {
                        items[i]->setBounds (x0 + k * knobW, 24, knobW, jmin (80, getHeight() - 26));
                        ++k;
                    }
                }
            }
        }

    private:
        void addItem (Component* c, int section, bool isSwitch)
        {
            items.add (c);
            addAndMakeVisible (c);
            itemSection.push_back (section);
            itemIsSwitch.push_back (isSwitch);
        }

        SxGuitarRig& fx;
        OwnedArray<Component> items;
        std::vector<int> itemSection;
        std::vector<bool> itemIsSwitch;
        StringArray titles;
        String heading;
    };

    //==========================================================================
    /** The amp head. Every model has its own controls, placed like the photographs: the Blackface
        control plate with both channels over a grille; the JCM800 and Plexi plates under the cloth;
        the Rectifier's two channel rows under the vent. Only the active model's controls show. */
    class AmpFace : public Component
    {
    public:
        explicit AmpFace (SxGuitarRig& e) : fx (e)
        {
            using P = SxGuitarRig;
            const Colour fenderAccent (0xffb4bac4);
            const Colour goldAccent (0xffc2a86f);
            const Colour silverAccent (0xffa3abb8);

            // Blackface Twin Reverb: normal channel (4 knobs), vibrato channel (7 knobs)
            choice (0, P::pFChannel, { "NORMAL", "VIBRATO" }, fenderAccent, { 650, 10, 360, 24 });
            knob (0, P::pFNVolume, "Volume", KnobStyle::Fender, fenderAccent, 60, 92);
            knob (0, P::pFNTreble, "Treble", KnobStyle::Fender, fenderAccent, 130, 92);
            knob (0, P::pFNMiddle, "Middle", KnobStyle::Fender, fenderAccent, 200, 92);
            knob (0, P::pFNBass, "Bass", KnobStyle::Fender, fenderAccent, 270, 92);
            choice (0, P::pFNBright, { "BRIGHT OFF", "BRIGHT ON" }, fenderAccent, { 30, 146, 280, 22 });
            knob (0, P::pFVVolume, "Volume", KnobStyle::Fender, fenderAccent, 400, 92);
            knob (0, P::pFVTreble, "Treble", KnobStyle::Fender, fenderAccent, 478, 92);
            knob (0, P::pFVMiddle, "Middle", KnobStyle::Fender, fenderAccent, 556, 92);
            knob (0, P::pFVBass, "Bass", KnobStyle::Fender, fenderAccent, 634, 92);
            knob (0, P::pFVReverb, "Reverb", KnobStyle::Fender, fenderAccent, 712, 92);
            knob (0, P::pFVSpeed, "Speed", KnobStyle::Fender, fenderAccent, 790, 92);
            knob (0, P::pFVIntensity, "Intensity", KnobStyle::Fender, fenderAccent, 868, 92);
            choice (0, P::pFVBright, { "BRIGHT OFF", "BRIGHT ON" }, fenderAccent, { 370, 146, 280, 22 });

            // JCM800 lead: presence, bass, middle, treble, master (VOLUME), pre-amp (VOLUME 2), sensitivity
            knob (1, P::pJPresence, "Presence", KnobStyle::Gold, goldAccent, 90, 300);
            knob (1, P::pJBass, "Bass", KnobStyle::Gold, goldAccent, 210, 300);
            knob (1, P::pJMiddle, "Middle", KnobStyle::Gold, goldAccent, 330, 300);
            knob (1, P::pJTreble, "Treble", KnobStyle::Gold, goldAccent, 450, 300);
            knob (1, P::pJMaster, "Volume", KnobStyle::Gold, goldAccent, 570, 300);
            knob (1, P::pJPreamp, "Pre-amp", KnobStyle::Gold, goldAccent, 690, 300);
            choice (1, P::pJHighSens, { "LOW INPUT", "HIGH INPUT" }, goldAccent, { 790, 286, 190, 26 });

            // Plexi: presence, bass, middle, treble, volume I, volume II (with the jumper)
            knob (2, P::pPPresence, "Presence", KnobStyle::Gold, goldAccent, 90, 300);
            knob (2, P::pPBass, "Bass", KnobStyle::Gold, goldAccent, 210, 300);
            knob (2, P::pPMiddle, "Middle", KnobStyle::Gold, goldAccent, 330, 300);
            knob (2, P::pPTreble, "Treble", KnobStyle::Gold, goldAccent, 450, 300);
            knob (2, P::pPVolume1, "Volume I", KnobStyle::Gold, goldAccent, 610, 300);
            knob (2, P::pPVolume2, "Volume II", KnobStyle::Gold, goldAccent, 740, 300);
            choice (2, P::pPJump, { "JUMP OFF", "JUMP ON" }, goldAccent, { 830, 286, 150, 26 });

            // Rectifier: two channel rows (master, presence, bass, middle, treble, gain) with Clean / Crush
            const int rows[2] { 170, 285 };
            for (int ch = 0; ch < 2; ++ch)
            {
                const int y = rows[ch];
                const int master = ch == 0 ? P::pM1Master : P::pM2Master;
                const int pres = ch == 0 ? P::pM1Presence : P::pM2Presence;
                const int bas = ch == 0 ? P::pM1Bass : P::pM2Bass;
                const int mid = ch == 0 ? P::pM1Middle : P::pM2Middle;
                const int tre = ch == 0 ? P::pM1Treble : P::pM2Treble;
                const int gain = ch == 0 ? P::pM1Gain : P::pM2Gain;
                const int crush = ch == 0 ? P::pM1Crush : P::pM2Crush;
                knob (3, master, "Master", KnobStyle::Silver, silverAccent, 90, y);
                knob (3, pres, "Presence", KnobStyle::Silver, silverAccent, 200, y);
                knob (3, bas, "Bass", KnobStyle::Silver, silverAccent, 310, y);
                knob (3, mid, "Middle", KnobStyle::Silver, silverAccent, 420, y);
                knob (3, tre, "Treble", KnobStyle::Silver, silverAccent, 530, y);
                knob (3, gain, "Gain", KnobStyle::Silver, silverAccent, 640, y);
                choice (3, crush, { "CLEAN", "CRUSH" }, silverAccent, { 760, y - 13, 120, 26 });
            }
            choice (3, P::pMChannel, { "CH 1", "CH 2" }, silverAccent, { 900, 157, 110, 26 });
            choice (3, P::pMWatt, { "20 W", "100 W" }, silverAccent, { 900, 272, 110, 26 });

            refreshLayout();
        }

        int model() const { return jlimit (0, 3, (int) std::round (fx.param (SxGuitarRig::pAmp))); }

        void refreshLayout()
        {
            const int m = model();
            for (auto& s : slots)
            {
                s.c->setVisible (s.model == m);
                if (s.model == m) s.c->setBounds (s.r);
            }
            repaint();
        }

        void paint (Graphics& g) override
        {
            switch (model())
            {
                case 0: paintFender (g); break;
                case 1: paintMarshall (g, false, getLocalBounds().toFloat()); break;
                case 2: paintMarshall (g, true, getLocalBounds().toFloat()); break;
                default: paintMesa (g); break;
            }
        }

        void resized() override { refreshLayout(); }

    private:
        struct Slot { Component* c; int model; Rectangle<int> r; };

        void knob (int model, int param, const char* cap, KnobStyle s, Colour accent, int cx, int cy)
        {
            add (new RigKnob (fx, param, s, accent, cap), model, Rectangle<int> (cx - 32, cy - 40, 64, 80));
        }

        void choice (int model, int param, std::initializer_list<const char*> labels, Colour accent, Rectangle<int> r)
        {
            add (new RigChoice (fx, param, labels, accent), model, r);
        }

        void add (Component* c, int model, Rectangle<int> r)
        {
            owned.add (c);
            addAndMakeVisible (c);
            slots.push_back ({ c, model, r });
        }

        static void paintFender (Graphics& g)
        {
            g.fillAll (Colour (0xff15171a));

            const auto plate = Rectangle<float> (10.0f, 6.0f, 1000.0f, 160.0f);
            g.setColour (Colour (0xff0d0e11));
            g.fillRoundedRectangle (plate, 5.0f);
            g.setColour (kRigLine);
            g.drawRoundedRectangle (plate, 5.0f, 1.0f);
            g.setColour (kSilver.withAlpha (0.75f));
            g.setFont (Font (FontOptions (11.0f)).boldened());
            g.drawText ("NORMAL", 24, 12, 200, 14, Justification::left);
            g.drawText ("VIBRATO", 388, 12, 200, 14, Justification::left);
            g.setColour (kRigLine);
            g.drawVerticalLine (350, 12.0f, 160.0f);

            const auto grille = Rectangle<float> (10.0f, 176.0f, 1000.0f, 166.0f);
            g.setColour (Colour (0xff3a3f48));
            g.fillRoundedRectangle (grille, 5.0f);
            g.setColour (Colour (0x30000000));
            for (float x = grille.getX(); x < grille.getRight(); x += 4.0f) g.drawVerticalLine ((int) x, grille.getY(), grille.getBottom());
            for (float y = grille.getY(); y < grille.getBottom(); y += 4.0f) g.drawHorizontalLine ((int) y, grille.getX(), grille.getRight());

            g.setColour (Colour (0xffd0d5dd));
            g.setFont (Font (FontOptions (44.0f)).italicised().boldened());
            g.drawText ("Fender", 40, 200, 320, 56, Justification::left);
            g.setFont (Font (FontOptions (24.0f)).italicised());
            g.drawText ("Twin Reverb", 700, 296, 300, 32, Justification::right);
        }

        static void paintMarshall (Graphics& g, bool plexi, Rectangle<float> area)
        {
            g.fillAll (plexi ? Colour (0xff1e1b17) : Colour (0xff0e0f11));

            if (! plexi)
            {
                const auto cloth = Rectangle<float> (10.0f, 8.0f, 1000.0f, 240.0f);
                g.setColour (Colour (0xff0a0b0c));
                g.fillRoundedRectangle (cloth, 5.0f);
                g.setColour (Colour (0x08ffffff));
                for (float y = cloth.getY(); y < cloth.getBottom(); y += 3.0f) g.drawHorizontalLine ((int) y, cloth.getX(), cloth.getRight());
            }
            else
            {
                g.setColour (Colour (0xff3c3428));
                g.fillRect (10.0f, 150.0f, 1000.0f, 1.0f);
            }

            g.setColour (Colour (0xffb9b2a2));
            g.setFont (Font (FontOptions (plexi ? 56.0f : 64.0f)).italicised().boldened());
            g.drawText ("Marshall", 280, plexi ? 56 : 80, 460, 80, Justification::centred);

            const auto plate = Rectangle<float> (10.0f, 258.0f, 1000.0f, 84.0f);
            g.setColour (plexi ? Colour (0xff3f3a2e) : Colour (0xff3a3327));
            g.fillRoundedRectangle (plate, 5.0f);
            g.setColour (Colour (0xff5a4e36));
            g.drawRoundedRectangle (plate, 5.0f, 1.0f);
            g.setColour (Colour (0xffa39777));
            g.setFont (Font (FontOptions (12.0f)).boldened());
            g.drawText (plexi ? "PLEXI  -  JUMP" : "JCM 800  LEAD SERIES", 790, 262, 220, 16, Justification::right);

            // off-white frame, so the head stands apart from the dark panel
            g.setColour (Colour (0xffc8ccd2));
            g.drawRoundedRectangle (area.reduced (1.5f), 6.0f, 3.0f);
        }

        static void paintMesa (Graphics& g)
        {
            g.fillAll (Colour (0xff0e0f11));

            const auto top = Rectangle<float> (10.0f, 8.0f, 1000.0f, 96.0f);
            {
                Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (top.toNearestInt());
                g.setColour (Colour (0xff16181c));
                g.fillRect (top);
                g.setColour (Colour (0x0affffff));
                for (int k = -100; k < 1100; k += 14) g.drawLine ((float) k, 8.0f, (float) k + 96.0f, 104.0f, 1.0f);
            }

            const auto vent = Rectangle<float> (300.0f, 26.0f, 420.0f, 52.0f);
            g.setColour (Colour (0xff3a3f48));
            g.fillRoundedRectangle (vent, 4.0f);
            g.setColour (Colour (0xff0a0b0d));
            for (int i = 0; i < 22; ++i)
                g.fillRoundedRectangle (310.0f + 18.0f * (float) i, 34.0f, 9.0f, 36.0f, 2.0f);

            const auto badge = Rectangle<float> (408.0f, 38.0f, 204.0f, 28.0f);
            g.setColour (Colour (0xff0a0b0d));
            g.fillRoundedRectangle (badge, 4.0f);
            g.setColour (kSilver);
            g.setFont (Font (FontOptions (12.0f)).boldened());
            g.drawText ("MESA ENGINEERING", badge.toNearestInt(), Justification::centred);

            const auto panel = Rectangle<float> (10.0f, 112.0f, 1000.0f, 230.0f);
            g.setColour (Colour (0xff0b0c0e));
            g.fillRoundedRectangle (panel, 5.0f);
            g.setColour (kRigLine);
            g.drawRoundedRectangle (panel, 5.0f, 1.0f);
            g.setColour (kSilver.withAlpha (0.75f));
            g.setFont (Font (FontOptions (11.0f)).boldened());
            g.drawText ("CH 1", 18, 118, 60, 14, Justification::left);
            g.drawText ("CH 2", 18, 233, 60, 14, Justification::left);
            g.setColour (kRigDim);
            g.setFont (Font (FontOptions (18.0f)).italicised().boldened());
            g.drawText ("Rectifier", 900, 306, 110, 22, Justification::centred);
        }

        SxGuitarRig& fx;
        OwnedArray<Component> owned;
        std::vector<Slot> slots;
    };

    //==========================================================================
    /** The whole panel at its design size; the editor scales it to the window. */
    class RigPanel : public Component, private Timer
    {
    public:
        explicit RigPanel (SxGuitarRig& e)
            : fx (e), detail (e), face (e),
              ampChoice (e, SxGuitarRig::pAmp, { "FENDER BLACKFACE TWIN", "MARSHALL JCM800", "MARSHALL PLEXI", "MESA RECTIFIER" }, kRigAccent),
              cabChoice (e, SxGuitarRig::pCab, { "FENDER 2x12", "MARSHALL 4x12", "MESA 4x12", "OPEN 1x12" }, kRigAccent),
              micChoice (e, SxGuitarRig::pMicOff, { "CLOSE MIC", "OFF-AXIS" }, kRigAccent),
              outKnob (e, SxGuitarRig::pOutput, KnobStyle::Silver, kRigAccent, "OUTPUT")
        {
            const auto& defs = rigPedals();
            for (int i = 0; i < (int) defs.size(); ++i)
            {
                // the first five boxes are the reorderable pedals; the time block stays last
                boxes.add (new PedalBox (fx, i, defs[(size_t) i], i < 5,
                                         [this] (int idx) { selectPedal (idx); },
                                         [this] (int id, int slot) { movePedal (id, slot); }));
                addAndMakeVisible (boxes.getLast());
            }
            addAndMakeVisible (detail);
            addAndMakeVisible (face);
            addAndMakeVisible (ampChoice);
            addAndMakeVisible (cabChoice);
            addAndMakeVisible (micChoice);
            addAndMakeVisible (outKnob);

            for (auto* b : { &ampPower, &cabPower, &irLoad, &irToggle, &saveBtn, &loadBtn })
            {
                b->setColour (TextButton::buttonColourId, kRigPanel2);
                b->setColour (TextButton::textColourOffId, kRigText);
                b->setMouseCursor (MouseCursor::PointingHandCursor);
                addAndMakeVisible (b);
            }
            irLabel.setColour (Label::textColourId, kRigText);
            irLabel.setFont (Font (FontOptions (11.0f)));
            irLabel.setJustificationType (Justification::centredLeft);
            addAndMakeVisible (irLabel);

            ampPower.onClick = [this] { toggleParam (SxGuitarRig::pAmpOn); };
            cabPower.onClick = [this] { toggleParam (SxGuitarRig::pCabOn); };
            irLoad.setButtonText ("LOAD IR");
            irLoad.setTooltip ("Load a cabinet impulse response (WAV / AIFF / FLAC)");
            irToggle.setTooltip ("Use the loaded IR in place of the modelled cabinet");
            saveBtn.setButtonText ("SAVE");
            saveBtn.setTooltip ("Save the whole rig, including the IR path, as a preset");
            loadBtn.setButtonText ("LOAD");
            loadBtn.setTooltip ("Load a guitar rig preset");
            irLoad.onClick  = [this] { chooseIR(); };
            irToggle.onClick = [this]
            {
                if (! fx.hasIR()) chooseIR();
                else toggleParam (SxGuitarRig::pIrOn);
            };
            saveBtn.onClick = [this] { savePreset(); };
            loadBtn.onClick = [this] { loadPreset(); };

            layoutBoxes();
            selectPedal (2);
            shownModel = face.model();
            refreshModuleButtons();
            setSize (kPanelW, kPanelH);
            startTimerHz (20);
        }

        ~RigPanel() override { stopTimer(); }

        void paint (Graphics& g) override
        {
            g.fillAll (kRigBg);

            auto head = getLocalBounds().removeFromTop (36);
            g.setColour (kRigPanel);
            g.fillRect (head);
            g.setColour (kRigLine);
            g.fillRect (0, 36, getWidth(), 1);
            g.setColour (kRigText);
            g.setFont (Font (FontOptions (14.0f)).boldened());
            g.drawText ("SX GUITAR RIG", head.reduced (14, 0), Justification::centredLeft);
            g.setColour (kRigDim);
            g.setFont (Font (FontOptions (11.0f)));
            g.drawText ("pedalboard  >  amp  >  cabinet  >  time     double-click a knob to type a value, Alt-click resets it",
                        head.reduced (160, 0), Justification::centredLeft);

            const auto m = meterBounds().toFloat();
            g.setColour (kRigInk);
            g.fillRoundedRectangle (m, 2.0f);
            const float db = Decibels::gainToDecibels (meterLevel, -60.0f);
            const float frac = jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            g.setColour (db > -3.0f ? Colour (0xffc0504a) : (db > -12.0f ? Colour (0xffc9a04a) : Colour (0xff5fae7e)));
            g.fillRoundedRectangle (m.withWidth (m.getWidth() * frac), 2.0f);
            g.setColour (kRigDim);
            g.setFont (Font (FontOptions (10.0f)));
            g.drawText ("OUT", (int) m.getX() - 34, (int) m.getY() - 2, 30, 14, Justification::right);
        }

        void resized() override
        {
            layoutBoxes();
            detail.setBounds (10, 160, 1020, 102);
            ampChoice.setBounds (10, 270, 620, 24);
            face.setBounds (10, 302, 1020, 350);
            cabChoice.setBounds (10, 664, 520, 28);
            micChoice.setBounds (540, 664, 220, 28);
            cabPower.setBounds (770, 664, 100, 28);
            outKnob.setBounds (900, 656, 64, 80);
            irLoad.setBounds (10, 706, 92, 24);
            irLabel.setBounds (108, 706, 420, 24);
            irToggle.setBounds (534, 706, 78, 24);
            saveBtn.setBounds (620, 706, 92, 24);
            loadBtn.setBounds (718, 706, 92, 24);
            ampPower.setBounds (638, 270, 90, 24);
        }

    private:
        void toggleParam (int index)
        {
            if (auto* p = fx.getParameters()[index])
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
                p->endChangeGesture();
            }
            refreshModuleButtons();
        }

        void refreshModuleButtons()
        {
            const bool amp = fx.param (SxGuitarRig::pAmpOn) > 0.5f;
            const bool cab = fx.param (SxGuitarRig::pCabOn) > 0.5f;
            const bool hasIr = fx.hasIR();
            const bool irOn = hasIr && fx.param (SxGuitarRig::pIrOn) > 0.5f;

            ampPower.setButtonText (amp ? "AMP ON" : "AMP BYPASS");
            ampPower.setColour (TextButton::buttonColourId, amp ? Colour (0xff2d5b45) : kRigPanel2);
            cabPower.setButtonText (cab ? "CAB ON" : "CAB BYPASS");
            cabPower.setColour (TextButton::buttonColourId, cab ? Colour (0xff2d5b45) : kRigPanel2);
            irToggle.setButtonText (hasIr ? (irOn ? "IR ON" : "IR OFF") : "NO IR");
            irToggle.setEnabled (hasIr);
            irToggle.setColour (TextButton::buttonColourId, irOn ? kRigAccent : kRigPanel2);
            irLabel.setText (hasIr ? fx.irName() : "NO CABINET IR LOADED", dontSendNotification);
        }

        void chooseIR()
        {
            chooser = std::make_unique<FileChooser> ("Load cabinet impulse response",
                                                     File::getSpecialLocation (File::userDocumentsDirectory),
                                                     "*.wav;*.aif;*.aiff;*.flac");
            SafePointer<RigPanel> safe (this);
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [safe] (const FileChooser& fc)
            {
                if (auto* self = safe.getComponent())
                {
                    const auto file = fc.getResult();
                    if (! file.existsAsFile()) return;
                    String error;
                    if (self->fx.loadIRFile (file, error))
                    {
                        if (auto* p = self->fx.getParameters()[SxGuitarRig::pIrOn])
                        {
                            p->beginChangeGesture();
                            p->setValueNotifyingHost (1.0f);
                            p->endChangeGesture();
                        }
                        self->refreshModuleButtons();
                    }
                    else
                    {
                        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon,
                                                         "Cabinet IR", error);
                    }
                }
            });
        }

        void savePreset()
        {
            auto dir = File::getSpecialLocation (File::userDocumentsDirectory)
                           .getChildFile ("SoundX").getChildFile ("Guitar Rig Presets");
            dir.createDirectory();
            chooser = std::make_unique<FileChooser> ("Save guitar rig preset",
                                                     dir.getChildFile ("My Rig.sxpreset"),
                                                     "*.sxpreset");
            SafePointer<RigPanel> safe (this);
            chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                                  | FileBrowserComponent::warnAboutOverwriting,
                                  [safe] (const FileChooser& fc)
            {
                if (auto* self = safe.getComponent())
                {
                    auto file = fc.getResult();
                    if (file == File()) return;
                    if (! file.hasFileExtension ("sxpreset")) file = file.withFileExtension ("sxpreset");
                    if (! self->fx.savePresetFile (file))
                        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon,
                                                         "Guitar Rig Preset", "Could not write the preset file.");
                }
            });
        }

        void loadPreset()
        {
            chooser = std::make_unique<FileChooser> ("Load guitar rig preset",
                                                     File::getSpecialLocation (File::userDocumentsDirectory),
                                                     "*.sxpreset");
            SafePointer<RigPanel> safe (this);
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [safe] (const FileChooser& fc)
            {
                if (auto* self = safe.getComponent())
                {
                    const auto file = fc.getResult();
                    if (! file.existsAsFile()) return;
                    if (! self->fx.loadPresetFile (file))
                        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon,
                                                         "Guitar Rig Preset", "Could not read the preset file.");
                    self->refreshModuleButtons();
                }
            });
        }

        Rectangle<int> meterBounds() const { return { 860, 13, 150, 10 }; }

        /** Puts each pedal box in its slot: the five pedals in the chain order, the time block last. */
        void layoutBoxes()
        {
            const int boxW = (1020 - 5 * 8) / 6;
            lastOrder = fx.pedalOrderNow();
            for (int s = 0; s < 5; ++s)
                boxes[lastOrder[(size_t) s]]->setBounds (10 + s * (boxW + 8), 42, boxW, 110);
            boxes[5]->setBounds (10 + 5 * (boxW + 8), 42, boxW, 110);
        }

        /** Moves one pedal to a slot (0 .. 4) by rewriting the slot parameters as one undoable gesture each. */
        void movePedal (int id, int slot)
        {
            const auto current = fx.pedalOrderNow();
            std::vector<int> order (current.begin(), current.end());
            const auto it = std::find (order.begin(), order.end(), id);
            if (it == order.end()) return;
            order.erase (it);
            order.insert (order.begin() + jlimit (0, 4, slot), id);

            for (int s = 0; s < 5; ++s)
            {
                if (auto* p = dynamic_cast<RangedAudioParameter*> (fx.getParameters()[SxGuitarRig::pSlot0 + s]))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost (p->convertTo0to1 ((float) order[(size_t) s]));
                    p->endChangeGesture();
                }
            }
            layoutBoxes();
        }

        void selectPedal (int idx)
        {
            selectedPedal = jlimit (0, boxes.size() - 1, idx);
            for (int i = 0; i < boxes.size(); ++i) boxes[i]->setSelected (i == selectedPedal);
            detail.showPedal (rigPedals()[(size_t) selectedPedal]);
        }

        void timerCallback() override
        {
            const int model = face.model();
            if (model != shownModel)
            {
                shownModel = model;
                face.refreshLayout();
            }

            // the order can also change from outside (undo, automation, a loaded project)
            if (fx.pedalOrderNow() != lastOrder) layoutBoxes();

            meterLevel = jmax (fx.takeOutputPeak(), meterLevel * 0.9f);
            repaint (meterBounds().expanded (40, 4));
            refreshModuleButtons();

            double stamp = 0.0;
            const auto& params = fx.getParameters();
            for (int i = 0; i < params.size(); ++i)
                stamp += params[i]->getValue() * (double) (i % 13 + 1);
            if (stamp != lastStamp)
            {
                lastStamp = stamp;
                repaint();
            }
        }

        SxGuitarRig& fx;
        OwnedArray<PedalBox> boxes;
        DetailStrip detail;
        AmpFace face;
        RigChoice ampChoice, cabChoice, micChoice;
        RigKnob outKnob;
        TextButton ampPower, cabPower, irLoad, irToggle, saveBtn, loadBtn;
        Label irLabel;
        std::unique_ptr<FileChooser> chooser;
        int selectedPedal = 2;
        int shownModel = 0;
        float meterLevel = 0.0f;
        double lastStamp = -1.0;
        std::array<int, 5> lastOrder { 0, 1, 2, 3, 4 };
    };

    //==========================================================================
    /** Resizable like the other built-in editors: the panel keeps its layout and is scaled to fit. */
    class GuitarRigEditor : public AudioProcessorEditor
    {
    public:
        explicit GuitarRigEditor (SxGuitarRig& e) : AudioProcessorEditor (&e), panel (e)
        {
            addAndMakeVisible (panel);
            setResizable (true, false);
            setResizeLimits (520, 370, 2080, 1480);
            setSize (kPanelW, kPanelH);
        }

        void paint (Graphics& g) override { g.fillAll (kRigBg); }

        void resized() override
        {
            const float s = jmin (getWidth() / (float) kPanelW, getHeight() / (float) kPanelH);
            panel.setTransform (AffineTransform::scale (s));
            panel.setBounds (0, 0, kPanelW, kPanelH);
        }

    private:
        RigPanel panel;
    };
}

AudioProcessorEditor* SxGuitarRig::createEditor() { return new GuitarRigEditor (*this); }
}   // namespace sx
