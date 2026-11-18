#include "BuiltInEffects.h"

namespace sx
{
using namespace juce;

//==============================================================================
// The built-in editors: one dark panel, a big live display on top and a row of knobs below, in the
// spirit of the big console plug-ins (transfer curves, analyser, gain-reduction history, taps ...).
namespace
{
    const Colour kBg { 0xff16181d };
    const Colour kPanel { 0xff1f232b };
    const Colour kText { 0xffe9edf5 };
    const Colour kDim { 0xff8b97ab };
const Colour kPanel2 { 0xff232830 };

// a palette shared by every editor that draws bands / channels / traces
static const Colour kBandColours[8] {
    Colour (0xff5b9cf6), Colour (0xff39c5a8), Colour (0xffe6b33c), Colour (0xffe2705c),
    Colour (0xff9a7bf0), Colour (0xff53c2e8), Colour (0xff8fc94a), Colour (0xffea6fa8)
};
    const Colour kGrid { 0xff2a3040 };

    Colour accentFor (BuiltInEffect kind)
    {
        switch (kind)
        {
            case BuiltInEffect::Eq:            return Colour (0xff60a5fa);
            case BuiltInEffect::Compressor:    return Colour (0xff34d399);
            case BuiltInEffect::Convolver:     return Colour (0xffa78bfa);
            case BuiltInEffect::AnalogReverb:  return Colour (0xfff472b6);
            case BuiltInEffect::DigitalReverb: return Colour (0xff22d3ee);
            case BuiltInEffect::Limiter:       return Colour (0xfffb923c);
            case BuiltInEffect::Stereo:        return Colour (0xff38bdf8);
            case BuiltInEffect::Chorus:        return Colour (0xffa3e635);
            case BuiltInEffect::Delay:         return Colour (0xfffbbf24);
            case BuiltInEffect::Distortion:    return Colour (0xffef4444);
            case BuiltInEffect::Tremolo:       return Colour (0xffc084fc);
            case BuiltInEffect::Flanger:       return Colour (0xff2dd4bf);
            case BuiltInEffect::Phaser:        return Colour (0xff818cf8);
            case BuiltInEffect::SslChannel:    return Colour (0xffe8b04b);
            case BuiltInEffect::DeEsser:       return Colour (0xff56c8d8);
            case BuiltInEffect::Gate:          return Colour (0xff8ecf6a);
            case BuiltInEffect::Denoiser:      return Colour (0xffb3a1f0);
            case BuiltInEffect::Reverse:       return Colour (0xffe88b6a);
            case BuiltInEffect::PitchCorrector:return Colour (0xff6fd6a8);
        }
        return Colour (0xff60a5fa);
    }

    /** A round knob with an arc, a pointer and the value under it; drag / wheel / double-click. */
    class Knob : public Component, private SettableTooltipClient
    {
    public:
        Knob (SxEffectBase& e, int index, Colour accent) : effect (e), idx (index), accentColour (accent)
        {
            initialise();
        }

        /** The EQ editor re-points its knobs at the selected band and re-tints them. */
        void setCaption (const juce::String& c) { captionOverride = c; repaint(); }
        void setColourTint (Colour c) { accentColour = c; repaint(); }
        void setParameterIndex (int index)
        {
            idx = index;
            auto& s = effect.spec (idx);
            setTooltip (String (s.name) + " (" + String (s.min, 2) + " .. " + String (s.max, 2) + String (s.suffix) + ")");
            repaint();
        }

    private:
        juce::String captionOverride;

        void initialise()
        {
            setTooltip (effect.spec (idx).name);
            setMouseCursor (MouseCursor::UpDownResizeCursor);
            auto& s = effect.spec (idx);
            setTooltip (String (s.name) + " (" + String (s.min, 2) + " .. " + String (s.max, 2) + String (s.suffix) + ")");
        }

    public:
        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const auto nameArea = r.removeFromBottom (13.0f);
            const auto valueArea = r.removeFromBottom (13.0f);
            // the dial keeps a smaller share of the cell: the value and the caption stay readable
            // and the panels stop looking like a wall of knobs (the 2A / 76 have their own knobs)
            const float d = (jmin (r.getWidth(), r.getHeight()) - 6.0f) * 0.58f;
            auto knob = r.withSizeKeepingCentre (d, d);

            const float norm = (float) effect.getParameters()[(int) idx]->getValue();
            const float start = MathConstants<float>::pi * 1.25f;
            const float end = MathConstants<float>::pi * 2.75f;
            const float angle = start + norm * (end - start);

            // body
            g.setColour (kPanel.brighter (hover ? 0.10f : 0.0f));
            g.fillEllipse (knob);
            g.setColour (Colour (0xff0e1015));
            g.fillEllipse (knob.reduced (3.0f));
            g.setColour (kGrid);
            g.drawEllipse (knob.reduced (1.0f), 1.0f);

            // value arc
            Path arc;
            arc.addCentredArc (knob.getCentreX(), knob.getCentreY(), d * 0.5f - 1.0f, d * 0.5f - 1.0f,
                               0.0f, start, angle, true);
            g.setColour (accentColour.withAlpha (0.9f));
            g.strokePath (arc, PathStrokeType (jmax (1.8f, d * 0.045f), PathStrokeType::curved, PathStrokeType::rounded));

            // pointer
            const auto tip = knob.getCentre() + Point<float> (std::cos (angle), std::sin (angle)) * (d * 0.5f - d * 0.12f);
            g.setColour (kText);
            g.drawLine (knob.getCentreX(), knob.getCentreY(), tip.x, tip.y, jmax (1.6f, d * 0.035f));

            g.setFont (Font (9.5f));
            g.setColour (kDim);
            // a single space means "the faceplate prints this label itself"
            const String cap = captionOverride == " " ? String()
                             : (captionOverride.isNotEmpty() ? captionOverride
                                                             : juce::String (effect.spec (idx).name));
            if (cap.isNotEmpty())
                g.drawFittedText (cap, nameArea.toNearestInt(), Justification::centred, 1);

            const auto& s = effect.spec (idx);
            // read the real value back through the parameter so skewed ranges (frequency, time)
            // show the number the DSP actually uses
            float value = jmap (norm, s.min, s.max);
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (effect.getParameters()[(int) idx]))
                value = jlimit (s.min, s.max, rp->convertFrom0to1 (norm));
            g.setColour (kText);
            g.setFont (Font (10.0f).boldened());
            const int digits = std::abs (s.max - s.min) < 20.0f ? 2 : (std::abs (s.max - s.min) < 300.0f ? 0 : 0);
            g.drawFittedText (String (value, digits) + s.suffix, valueArea.toNearestInt(), Justification::centred, 1);
        }

        void mouseEnter (const MouseEvent&) override { hover = true; repaint(); }
        void mouseExit (const MouseEvent&) override { hover = false; repaint(); }

        void mouseDrag (const MouseEvent& e) override
        {
            const float delta = (float) (dragStart - e.getPosition().y) / (e.mods.isShiftDown() ? 600.0f : 180.0f);
            setNorm (jlimit (0.0f, 1.0f, startNorm + delta));
        }
        void mouseUp (const MouseEvent&) override
        {
            effect.getParameters()[(int) idx]->endChangeGesture();
        }
        // double-click types the value in, Alt+click goes back to the default
        void mouseDoubleClick (const MouseEvent&) override { showValueEditor(); }

        void mouseDown (const MouseEvent& e) override
        {
            if (e.mods.isAltDown())
            {
                auto* p = effect.getParameters()[(int) idx];
                p->beginChangeGesture();
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (effect.spec (idx).def));
                p->endChangeGesture();
                repaint();
                return;
            }
            dragStart = e.getPosition().y;
            startNorm = (float) effect.getParameters()[(int) idx]->getValue();
            effect.getParameters()[(int) idx]->beginChangeGesture();
        }
        void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) override
        {
            auto* p = effect.getParameters()[(int) idx];
            const float n = jlimit (0.0f, 1.0f, p->getValue() + (w.deltaY > 0.0f ? 0.02f : -0.02f));
            p->beginChangeGesture();
            p->setValueNotifyingHost (n);
            p->endChangeGesture();
            repaint();
        }

        void refresh() { if (! isMouseButtonDown()) repaint(); }

    private:
        void showValueEditor()
        {
            const auto& s = effect.spec (idx);
            auto* current = effect.getParameters()[(int) idx];
            const float value = current != nullptr
                ? (s.min + (float) current->getValue() * (s.max - s.min)) : s.def;
            auto* aw = new AlertWindow (String (s.name), "Enter a value (" + String (s.min, 2) + " .. "
                                        + String (s.max, 2) + String (s.suffix) + ")",
                                        MessageBoxIconType::NoIcon);
            aw->addTextEditor ("v", String (value, 3), "");
            aw->addButton (TRANS ("OK"), 1, KeyPress (KeyPress::returnKey));
            aw->addButton (TRANS ("Cancel"), 0, KeyPress (KeyPress::escapeKey));
            auto* self = this;
            aw->enterModalState (true, ModalCallbackFunction::create ([self, aw, s] (int result)
            {
                if (result != 1) return;
                const float v = jlimit (s.min, s.max, aw->getTextEditorContents ("v").getFloatValue());
                if (auto* p = self->effect.getParameters()[(int) self->idx])
                {
                    p->beginChangeGesture();
                    if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
                    p->endChangeGesture();
                }
                self->repaint();
            }), true);
        }

        void setNorm (float n)
        {
            auto* p = effect.getParameters()[(int) idx];
            p->setValueNotifyingHost (n);
            repaint();
        }
        SxEffectBase& effect;
        int idx = 0;
        Colour accentColour;
        float dragStart = 0.0f, startNorm = 0.0f;
        bool hover = false;
    };

    //==============================================================================
    /** The big display. Every effect gets its own drawing, all of them read live data the audio
        thread publishes (spectrum, gain reduction history, LFO phase, stereo pairs). */
    class Display : public Component
    {
    public:
        explicit Display (SxEffectBase& e) : effect (e) {}

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (Colour (0xff0e1015));
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (kGrid.withAlpha (0.8f));
            g.drawRoundedRectangle (r, 6.0f, 1.0f);

            auto area = r.reduced (8.0f);
            switch (effect.kind)
            {
                case BuiltInEffect::Eq:            paintEq (g, area); break;
                case BuiltInEffect::Compressor:    paintCompressor (g, area); break;
                case BuiltInEffect::SslChannel:    paintCompressor (g, area, effect.param (5), effect.param (6), 3.0f); break;
                case BuiltInEffect::DeEsser:       paintCompressor (g, area, effect.param (1), effect.param (2), 2.0f); break;
                case BuiltInEffect::Gate:          paintCompressor (g, area, effect.param (0), 2.0f, 0.0f); break;
                case BuiltInEffect::Denoiser:      paintCompressor (g, area, effect.param (1), 2.0f, 0.0f); break;
                case BuiltInEffect::Reverse:       paintDelay (g, area); break;
                case BuiltInEffect::PitchCorrector:paintTuner (g, area); break;
                case BuiltInEffect::Limiter:       paintLimiter (g, area); break;
                case BuiltInEffect::Convolver:     paintConvolver (g, area); break;
                case BuiltInEffect::AnalogReverb:
                case BuiltInEffect::DigitalReverb: paintReverb (g, area); break;
                case BuiltInEffect::Delay:         paintDelay (g, area); break;
                case BuiltInEffect::Distortion:    paintTransfer (g, area); break;
                case BuiltInEffect::Chorus:
                case BuiltInEffect::Flanger:
                case BuiltInEffect::Phaser:
                case BuiltInEffect::Tremolo:       paintModulation (g, area); break;
                case BuiltInEffect::Tape:          paintTape (g, area); break;
                case BuiltInEffect::Saturator:     paintCurveFamily (g, area); break;
                case BuiltInEffect::Exciter:       paintExciter (g, area); break;
                case BuiltInEffect::MultibandComp: paintMultiband (g, area); break;
                case BuiltInEffect::ChorusEnsemble:paintEnsemble (g, area); break;
                case BuiltInEffect::Maximizer:     paintMaximizer (g, area); break;
                case BuiltInEffect::Tuner:         paintTuner (g, area); break;
                case BuiltInEffect::Clipper:       paintClipper (g, area); break;
            }
        }

        //== mouse: the EQ bands are dragged straight on the curve ------------------------------
        void mouseDown (const MouseEvent& e) override
        {
            dragBand = effect.kind == BuiltInEffect::Eq ? bandAt (e.getPosition()) : -1;
            if (dragBand >= 0)
            {
                effect.getParameters()[bandFreqParam (dragBand)]->beginChangeGesture();
                effect.getParameters()[bandGainParam (dragBand)]->beginChangeGesture();
            }
        }
        void mouseDrag (const MouseEvent& e) override
        {
            if (dragBand < 0) return;
            const auto area = getLocalBounds().toFloat().reduced (9.0f);
            const float hz = xToFreq (area, (float) e.x);
            const float db = yToDb (area, (float) e.y);
            auto* fp = effect.getParameters()[bandFreqParam (dragBand)];
            auto* gp = effect.getParameters()[bandGainParam (dragBand)];
            const auto& fs = effect.spec (bandFreqParam (dragBand));
            const auto& gs = effect.spec (bandGainParam (dragBand));
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (fp)) fp->setValueNotifyingHost (rp->convertTo0to1 (jlimit (fs.min, fs.max, hz)));
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (gp)) gp->setValueNotifyingHost (rp->convertTo0to1 (jlimit (gs.min, gs.max, db)));
            repaint();
        }
        void mouseUp (const MouseEvent&) override
        {
            if (dragBand < 0) return;
            effect.getParameters()[bandFreqParam (dragBand)]->endChangeGesture();
            effect.getParameters()[bandGainParam (dragBand)]->endChangeGesture();
            dragBand = -1;
        }
        void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
        {
            // the wheel changes the Q of the band under the pointer
            if (effect.kind != BuiltInEffect::Eq) return;
            const int band = bandAt (e.getPosition());
            if (band != 1 && band != 2) return;
            auto* p = effect.getParameters()[qParam (band)];
            const float n = jlimit (0.0f, 1.0f, p->getValue() + (w.deltaY > 0.0f ? 0.03f : -0.03f));
            p->setValueNotifyingHost (n);
            repaint();
        }

    private:
        static int bandFreqParam (int band) { const int p[4] { 1, 3, 6, 9 }; return p[jlimit (0, 3, band)]; }
        static int bandGainParam (int band) { const int p[4] { 2, 4, 7, 10 }; return p[jlimit (0, 3, band)]; }
        static int qParam (int band) { return band == 1 ? 5 : 8; }
        static const char* bandLabel (int band) { const char* n[4] { "L", "M1", "M2", "H" }; return n[jlimit (0, 3, band)]; }

        static float freqToX (Rectangle<float> a, float hz)
        {
            return a.getX() + a.getWidth() * (std::log10 (jlimit (20.0f, 20000.0f, hz) / 20.0f) / 3.0f);
        }
        static float xToFreq (Rectangle<float> a, float x)
        {
            return 20.0f * std::pow (10.0f, 3.0f * (x - a.getX()) / jmax (1.0f, a.getWidth()));
        }
        static float dbToY (Rectangle<float> a, float db)
        {
            return a.getCentreY() - (db / 20.0f) * (a.getHeight() * 0.46f);
        }
        static float yToDb (Rectangle<float> a, float y)
        {
            return (a.getCentreY() - y) / jmax (1.0f, a.getHeight() * 0.46f) * 20.0f;
        }

        int bandAt (Point<int> p) const
        {
            auto area = getLocalBounds().toFloat().reduced (9.0f);
            for (int b = 0; b < 4; ++b)
            {
                const float x = freqToX (area, effect.param (bandFreqParam (b)));
                const float y = dbToY (area, effect.param (bandGainParam (b)));
                if (Point<float> (x, y).getDistanceFrom (p.toFloat()) < 16.0f) return b;
            }
            return -1;
        }

        //== EQ: analyser behind, response curve in front, draggable handles ---------------------
        void paintEq (Graphics& g, Rectangle<float> a)
        {
            const auto accent = accentFor (effect.kind);
            g.setColour (kGrid.withAlpha (0.55f));
            for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
                g.drawVerticalLine ((int) freqToX (a, hz), a.getY(), a.getBottom());
            for (float db : { -12.0f, -6.0f, 6.0f, 12.0f })
                g.drawHorizontalLine ((int) dbToY (a, db), a.getX(), a.getRight());
            g.setColour (kGrid);
            g.drawHorizontalLine ((int) dbToY (a, 0.0f), a.getX(), a.getRight());

            // analyser: the input spectrum, 96 log spaced bins
            Path spec;
            spec.startNewSubPath (a.getX(), a.getBottom());
            for (int i = 0; i < SxEffectBase::kSpectrumBins; ++i)
            {
                const float hz = 20.0f * std::pow (1000.0f, (float) i / (float) (SxEffectBase::kSpectrumBins - 1));
                const float db = Decibels::gainToDecibels (effect.spectrumAt (i), -72.0f);
                const float x = freqToX (a, hz);
                const float y = a.getBottom() - jlimit (0.0f, 1.0f, (db + 72.0f) / 72.0f) * a.getHeight();
                spec.lineTo (x, y);
            }
            spec.lineTo (a.getRight(), a.getBottom());
            spec.closeSubPath();
            g.setColour (Colour (0xff8899aa).withAlpha (0.20f));
            g.fillPath (spec);

            // response
            Path curve;
            bool first = true;
            for (float x = a.getX(); x <= a.getRight(); x += 1.5f)
            {
                const float y = dbToY (a, jlimit (-24.0f, 24.0f, effect.curveDb (xToFreq (a, x))));
                if (first) { curve.startNewSubPath (x, y); first = false; }
                else curve.lineTo (x, y);
            }
            Path fill (curve);
            fill.lineTo (a.getRight(), dbToY (a, 0.0f));
            fill.lineTo (a.getX(), dbToY (a, 0.0f));
            fill.closeSubPath();
            g.setColour (accent.withAlpha (0.18f));
            g.fillPath (fill);
            g.setColour (accent);
            g.strokePath (curve, PathStrokeType (2.2f));

            // band handles
            for (int b = 0; b < 4; ++b)
            {
                const float x = freqToX (a, effect.param (bandFreqParam (b)));
                const float y = dbToY (a, effect.param (bandGainParam (b)));
                g.setColour (accent.withAlpha (0.95f));
                g.fillEllipse (x - 6.0f, y - 6.0f, 12.0f, 12.0f);
                g.setColour (Colour (0xff0e1015));
                g.fillEllipse (x - 2.5f, y - 2.5f, 5.0f, 5.0f);
                g.setColour (kText);
                g.setFont (Font (10.0f).boldened());
                g.drawText (bandLabel (b), (int) x + 8, (int) y - 7, 24, 14, Justification::centredLeft);
            }

            g.setColour (kDim);
            g.setFont (Font (10.0f));
            for (auto& hz : { std::pair<float, const char*> { 50.0f, "50" }, { 100.0f, "100" }, { 500.0f, "500" },
                              { 1000.0f, "1k" }, { 5000.0f, "5k" }, { 10000.0f, "10k" } })
                g.drawText (hz.second, (int) freqToX (a, hz.first) - 12, (int) a.getBottom() - 12, 24, 12,
                            Justification::centred);
        }

        //== compressor: transfer curve, gain-reduction history, in/out meters -------------------
        void paintCompressor (Graphics& g, Rectangle<float> a, float thrOverride = -999.0f,
                              float ratioOverride = -1.0f, float kneeOverride = -1.0f)
        {
            const auto accent = accentFor (effect.kind);
            auto left = a.removeFromLeft (a.getWidth() * 0.52f).reduced (4.0f);
            const float thr = thrOverride > -900.0f ? thrOverride : effect.param (0);
            const float ratio = jmax (1.0f, ratioOverride > 0.0f ? ratioOverride : effect.param (1));
            const float knee = kneeOverride >= 0.0f ? kneeOverride : effect.param (4);

            g.setColour (kGrid.withAlpha (0.6f));
            for (float db : { -48.0f, -36.0f, -24.0f, -12.0f, 0.0f })
            {
                const float x = left.getX() + left.getWidth() * (db + 60.0f) / 60.0f;
                g.drawVerticalLine ((int) x, left.getY(), left.getBottom());
                const float y = left.getBottom() - left.getHeight() * (db + 60.0f) / 60.0f;
                g.drawHorizontalLine ((int) y, left.getX(), left.getRight());
            }

            Path curve;
            for (float in = -60.0f; in <= 0.0f; in += 0.5f)
            {
                float over = in - thr;
                float out = in;
                if (over > 0.0f)
                {
                    if (knee > 0.0f && over < knee * 0.5f)
                    {
                        const float t = over + knee * 0.5f;
                        out = in - (1.0f - 1.0f / ratio) * t * t / (2.0f * knee);
                    }
                    else out = in - (1.0f - 1.0f / ratio) * over;
                }
                else if (knee > 0.0f && over > -knee * 0.5f)
                {
                    const float t = over + knee * 0.5f;
                    out = in - (1.0f - 1.0f / ratio) * t * t / (2.0f * knee);
                }
                const float x = left.getX() + left.getWidth() * (in + 60.0f) / 60.0f;
                const float y = left.getBottom() - left.getHeight() * jlimit (0.0f, 1.0f, (out + 60.0f) / 60.0f);
                if (in <= -60.0f) curve.startNewSubPath (x, y);
                else curve.lineTo (x, y);
            }
            g.setColour (kDim.withAlpha (0.5f));
            g.strokePath (curve, PathStrokeType (1.0f, PathStrokeType::curved));
            Path diag;
            diag.startNewSubPath (left.getX(), left.getBottom());
            diag.lineTo (left.getRight(), left.getY());
            g.setColour (kGrid);
            g.strokePath (diag, PathStrokeType (1.0f));
            g.setColour (accent);
            g.strokePath (curve, PathStrokeType (2.2f));

            // gain reduction history
            auto right = a.reduced (4.0f);
            g.setColour (kGrid.withAlpha (0.6f));
            g.drawHorizontalLine ((int) right.getBottom(), right.getX(), right.getRight());
            Path gr;
            gr.startNewSubPath (right.getX(), right.getBottom());
            for (int i = 0; i < SxEffectBase::kHistSize; ++i)
            {
                const int at = ((int) (effect.gainReductionDb() * 0.0f) + i) % SxEffectBase::kHistSize;
                const float v = jlimit (0.0f, 24.0f, effect.historyAt ((i + 0) % SxEffectBase::kHistSize));
                juce::ignoreUnused (at);
                const float x = right.getX() + right.getWidth() * (float) i / (float) (SxEffectBase::kHistSize - 1);
                const float y = right.getBottom() - right.getHeight() * (v / 24.0f);
                gr.lineTo (x, y);
            }
            gr.lineTo (right.getRight(), right.getBottom());
            gr.closeSubPath();
            g.setColour (accent.withAlpha (0.25f));
            g.fillPath (gr);
            g.setColour (accent);
            g.strokePath (gr, PathStrokeType (1.6f));

            g.setColour (kText);
            g.setFont (Font (12.0f).boldened());
            g.drawText ("GR " + String (effect.gainReductionDb(), 1) + " dB",
                        right.toNearestInt().removeFromTop (16), Justification::centredRight);
        }

        //== limiter: gain reduction + ceiling ----------------------------------------------------
        void paintLimiter (Graphics& g, Rectangle<float> a)
        {
            const auto accent = accentFor (effect.kind);
            const float ceiling = effect.param (0);
            const float ceilingY = a.getBottom() - a.getHeight() * (jlimit (0.0f, 1.0f, (ceiling + 24.0f) / 24.0f));
            g.setColour (Colour (0xffef4444).withAlpha (0.85f));
            g.drawHorizontalLine ((int) ceilingY, a.getX(), a.getRight());
            g.setColour (kDim);
            g.setFont (Font (10.0f));
            g.drawText (String (ceiling, 1) + " dB", (int) a.getX() + 2, (int) ceilingY - 13, 60, 12,
                        Justification::centredLeft);

            Path gr;
            gr.startNewSubPath (a.getX(), a.getBottom());
            for (int i = 0; i < SxEffectBase::kHistSize; ++i)
            {
                const float v = jlimit (0.0f, 24.0f, effect.historyAt ((size_t) i));
                const float x = a.getX() + a.getWidth() * (float) i / (float) (SxEffectBase::kHistSize - 1);
                gr.lineTo (x, a.getBottom() - a.getHeight() * (v / 24.0f));
            }
            gr.lineTo (a.getRight(), a.getBottom());
            gr.closeSubPath();
            g.setColour (accent.withAlpha (0.22f));
            g.fillPath (gr);
            g.setColour (accent);
            g.strokePath (gr, PathStrokeType (1.8f));

            g.setColour (kText);
            g.setFont (Font (12.0f).boldened());
            g.drawText ("GR " + String (effect.gainReductionDb(), 1) + " dB", a.toNearestInt().removeFromTop (16),
                        Justification::centredRight);
        }

        //== convolver: the impulse response envelope --------------------------------------------
        void paintConvolver (Graphics& g, Rectangle<float> a)
        {
            const auto accent = accentFor (effect.kind);
            const float seconds = effect.param (0);
            const int n = jmax (32, (int) (a.getWidth()));
            Path ir;
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) i / (float) (n - 1);
                const float env = std::pow (1.0f - t, 2.2f) * std::exp (-3.0f * t);
                const float x = a.getX() + a.getWidth() * t;
                const float y = a.getCentreY() - env * a.getHeight() * 0.42f;
                if (i == 0) ir.startNewSubPath (x, y);
                else ir.lineTo (x, y);
            }
            Path mirror (ir);
            g.setColour (accent.withAlpha (0.35f));
            g.strokePath (ir, PathStrokeType (1.6f));
            g.setColour (kDim);
            g.setFont (Font (11.0f));
            g.drawText (String (seconds, 2) + " s impulse response", a.toNearestInt().removeFromTop (16),
                        Justification::centredLeft);
            g.setColour (kGrid);
            for (int i = 1; i < 8; ++i)
            {
                const float x = a.getX() + a.getWidth() * (float) i / 8.0f;
                g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
            }
            // the wet tail level of the last second
            Path tail;
            tail.startNewSubPath (a.getX(), a.getBottom());
            for (int i = 0; i < SxEffectBase::kHistSize; ++i)
            {
                const float v = jlimit (0.0f, 1.0f, effect.historyAt ((size_t) i));
                const float x = a.getX() + a.getWidth() * (float) i / (float) (SxEffectBase::kHistSize - 1);
                tail.lineTo (x, a.getBottom() - v * a.getHeight());
            }
            tail.lineTo (a.getRight(), a.getBottom());
            tail.closeSubPath();
            g.setColour (accent.withAlpha (0.16f));
            g.fillPath (tail);
        }

        //== reverbs: the decay tail -------------------------------------------------------------
        void paintReverb (Graphics& g, Rectangle<float> a)
        {
            const auto accent = accentFor (effect.kind);
            const float size = effect.param (0) / 100.0f;
            Path tail;
            tail.startNewSubPath (a.getX(), a.getBottom());
            const int n = 160;
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) i / (float) (n - 1);
                const float decay = std::exp (-t * (4.0f - 3.0f * size));
                const float x = a.getX() + a.getWidth() * t;
                tail.lineTo (x, a.getBottom() - decay * a.getHeight() * 0.85f);
            }
            tail.lineTo (a.getRight(), a.getBottom());
            tail.closeSubPath();
            g.setColour (accent.withAlpha (0.18f));
            g.fillPath (tail);

            Path live;
            live.startNewSubPath (a.getX(), a.getBottom());
            float peak = 0.0001f;
            for (int i = 0; i < SxEffectBase::kHistSize; ++i) peak = jmax (peak, effect.historyAt ((size_t) i));
            for (int i = 0; i < SxEffectBase::kHistSize; ++i)
            {
                const float v = jlimit (0.0f, 1.0f, effect.historyAt ((size_t) i) / peak);
                const float x = a.getX() + a.getWidth() * (float) i / (float) (SxEffectBase::kHistSize - 1);
                live.lineTo (x, a.getBottom() - v * a.getHeight() * 0.85f);
            }
            g.setColour (accent);
            g.strokePath (live, PathStrokeType (1.8f));

            g.setColour (kDim);
            g.setFont (Font (11.0f));
            g.drawText (TRANS ("Decay") + "  " + String (effect.param (1), 0) + " %", a.toNearestInt().removeFromTop (16),
                        Justification::centredLeft);
        }


        //== delay: the taps ----------------------------------------------------------------------
        void paintDelay (Rectangle<float> const&) {}
        void paintDelay (Graphics& g, Rectangle<float> a)
        {
            const auto accent = accentFor (effect.kind);
            const float ms = effect.param (0);
            const float fb = effect.param (1) / 100.0f;
            const bool pingpong = effect.param (3) > 0.5f;
            const float total = 2000.0f;
            float level = 1.0f;
            for (int tap = 0; tap < 12 && level > 0.01f; ++tap)
            {
                const float at = (ms * (float) (tap + 1)) / total;
                if (at > 1.0f) break;
                const float x = a.getX() + a.getWidth() * at;
                const float h = jmax (3.0f, a.getHeight() * 0.75f * level);
                g.setColour (accent.withAlpha (0.25f + 0.65f * level));
                const float y = pingpong && (tap & 1) ? a.getCentreY() : a.getBottom() - h;
                g.fillRoundedRectangle (x - 2.0f, y, 4.0f, h, 2.0f);
                level *= fb;
            }
            g.setColour (kGrid);
            g.drawHorizontalLine ((int) a.getBottom(), a.getX(), a.getRight());
            g.setColour (kDim);
            g.setFont (Font (11.0f));
            g.drawText (String (ms, 0) + " ms  ·  " + String (effect.param (1), 0) + " % feedback"
                        + (pingpong ? String ("  ·  ping-pong") : String()),
                        a.toNearestInt().removeFromTop (16), Justification::centredLeft);
        }

        //== distortion: the waveshaper curve ----------------------------------------------------
        void paintTransfer (Graphics& g, Rectangle<float> a)
        {
            const auto accent = accentFor (effect.kind);
            const float drive = Decibels::decibelsToGain (effect.param (0));
            Path curve;
            for (int i = 0; i <= 200; ++i)
            {
                const float in = (float) i / 100.0f - 1.0f;
                const float driven = in * drive;
                float out = std::tanh (driven + 0.15f * driven * driven) - 0.06f;
                out = std::tanh (out * 1.1f);
                const float x = a.getX() + a.getWidth() * (in + 1.0f) * 0.5f;
                const float y = a.getCentreY() - out * a.getHeight() * 0.45f;
                if (i == 0) curve.startNewSubPath (x, y);
                else curve.lineTo (x, y);
            }
            g.setColour (kGrid);
            g.drawHorizontalLine ((int) a.getCentreY(), a.getX(), a.getRight());
            g.drawVerticalLine ((int) a.getCentreX(), a.getY(), a.getBottom());
            Path diag;
            diag.startNewSubPath (a.getX(), a.getBottom());
            diag.lineTo (a.getRight(), a.getY());
            g.setColour (kGrid.withAlpha (0.6f));
            g.strokePath (diag, PathStrokeType (1.0f));
            g.setColour (accent);
            g.strokePath (curve, PathStrokeType (2.4f));
            g.setColour (kDim);
            g.setFont (Font (11.0f));
            g.drawText (TRANS ("Drive") + "  +" + String (effect.param (0), 1) + " dB",
                        a.toNearestInt().removeFromTop (16), Justification::centredLeft);
        }

        //== chorus / flanger / phaser / tremolo: the LFO and what it modulates -------------------
        //== the newer units ------------------------------------------------------------------------
        void paintTape (Graphics& g, Rectangle<float> a)
        {
            auto top = a.removeFromTop (a.getHeight() * 0.62f);
            g.setColour (kGrid.withAlpha (0.5f));
            g.drawLine (top.getX(), top.getCentreY(), top.getRight(), top.getCentreY(), 0.8f);
            g.drawLine (top.getCentreX(), top.getY(), top.getCentreX(), top.getBottom(), 0.8f);
            Path curve;
            for (float x = top.getX(); x <= top.getRight(); x += 1.0f)
            {
                const float in = jmap (x, top.getX(), top.getRight(), -2.2f, 2.2f);
                const float out = std::tanh (in * 1.2f) / 1.2f + 0.04f * in * in;
                const float y = top.getCentreY() - jlimit (-1.1f, 1.1f, out) * top.getHeight() * 0.42f;
                if (x <= top.getX() + 0.5f) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
            }
            g.setColour (kBandColours[2]);
            g.strokePath (curve, PathStrokeType (2.0f));
            g.setColour (kDim);
            g.setFont (Font (10.0f));
            g.drawText ("tape transfer  ·  wow / flutter / hiss", top.toNearestInt().removeFromTop (14), Justification::left);

            Path wow;
            const float wowAmt = effect.param (2) / 100.0f;
            const float flut = effect.param (3) / 100.0f;
            for (float x = a.getX(); x <= a.getRight(); x += 1.5f)
            {
                const float t = (x - a.getX()) / jmax (1.0f, a.getWidth()) * 12.0f;
                const float v = wowAmt * std::sin (t * 2.0f) + flut * 0.6f * std::sin (t * 9.0f);
                const float y = a.getCentreY() - v * a.getHeight() * 0.42f;
                if (x <= a.getX() + 0.5f) wow.startNewSubPath (x, y); else wow.lineTo (x, y);
            }
            g.setColour (kBandColours[5].withAlpha (0.9f));
            g.strokePath (wow, PathStrokeType (1.6f));
        }

        void paintCurveFamily (Graphics& g, Rectangle<float> a)
        {
            static const char* names[4] { "tape", "tube", "diode", "fold" };
            const int curve = jlimit (0, 3, (int) std::round (effect.param (0)));
            const float drive = 1.0f + effect.param (1) / 100.0f * 20.0f;
            g.setColour (kGrid.withAlpha (0.5f));
            g.drawLine (a.getX(), a.getCentreY(), a.getRight(), a.getCentreY(), 0.8f);
            g.drawLine (a.getCentreX(), a.getY(), a.getCentreX(), a.getBottom(), 0.8f);
            Path curvePath;
            for (float x = a.getX(); x <= a.getRight(); x += 1.0f)
            {
                float in = jmap (x, a.getX(), a.getRight(), -2.0f, 2.0f) * drive;
                float out;
                switch (curve)
                {
                    case 1:  out = (1.0f + in) / (1.0f + std::abs (in)); break;
                    case 2:  out = std::tanh (in) + 0.15f * std::tanh (in * in * 0.5f); break;
                    case 3:  out = std::sin (jlimit (-3.0f, 3.0f, in) * 0.8f); break;
                    default: out = std::tanh (in * 0.9f) + 0.05f * in * in; break;
                }
                const float y = a.getCentreY() - jlimit (-1.2f, 1.2f, out) * a.getHeight() * 0.42f;
                if (x <= a.getX() + 0.5f) curvePath.startNewSubPath (x, y); else curvePath.lineTo (x, y);
            }
            g.setColour (kBandColours[3]);
            g.strokePath (curvePath, PathStrokeType (2.2f));
            g.setColour (kDim);
            g.setFont (Font (11.0f));
            g.drawText (names[curve], a.toNearestInt().removeFromTop (16), Justification::left);
        }

        void paintExciter (Graphics& g, Rectangle<float> a)
        {
            // three bands with the harmonics each one contributes
            const float xLow = effect.param (0), xHigh = effect.param (1);
            const float amt[3] { effect.param (2) / 100.0f, effect.param (3) / 100.0f, effect.param (4) / 100.0f };
            auto plot = a.reduced (0.0f, 18.0f);
            const auto xFor = [&] (float hz)
            {
                return plot.getX() + plot.getWidth() * (std::log10 (jlimit (20.0f, 20000.0f, hz) / 20.0f) / 3.0f);
            };
            // band shading
            g.setColour (kBandColours[0].withAlpha (0.10f + 0.25f * amt[0]));
            g.fillRect (Rectangle<float> (xFor (20.0f), plot.getY(), xFor (xLow) - xFor (20.0f), plot.getHeight()));
            g.setColour (kBandColours[1].withAlpha (0.10f + 0.25f * amt[1]));
            g.fillRect (Rectangle<float> (xFor (xLow), plot.getY(), xFor (xHigh) - xFor (xLow), plot.getHeight()));
            g.setColour (kBandColours[2].withAlpha (0.10f + 0.25f * amt[2]));
            g.fillRect (Rectangle<float> (xFor (xHigh), plot.getY(), plot.getRight() - xFor (xHigh), plot.getHeight()));

            // generated harmonics per band, as bars
            for (int k = 0; k < 3; ++k)
            {
                if (amt[k] <= 0.0f) continue;
                const float from = k == 0 ? 40.0f : (k == 1 ? xLow : xHigh);
                const float to = k == 0 ? xLow : (k == 1 ? xHigh : 8000.0f);
                for (int h = 2; h <= 6; ++h)
                {
                    const float f = jlimit (20.0f, 20000.0f, from * (float) h);
                    if (f > 20000.0f) break;
                    const float x = xFor (f);
                    const float hgt = plot.getHeight() * amt[k] * (1.0f / (float) h) * 0.9f;
                    g.setColour (kBandColours[k].withAlpha (0.85f));
                    g.fillRect (Rectangle<float> (x - 1.5f, plot.getBottom() - hgt, 3.0f, hgt));
                }
                juce::ignoreUnused (to);
            }
            g.setColour (kGrid.withAlpha (0.35f));
            for (float hz : { 100.0f, 1000.0f, 10000.0f })
                g.drawVerticalLine ((int) xFor (hz), plot.getY(), plot.getBottom());
            g.setColour (kDim);
            g.setFont (Font (10.0f));
            g.drawText ("low " + String ((int) (amt[0] * 100)) + "%   mid " + String ((int) (amt[1] * 100))
                        + "%   high " + String ((int) (amt[2] * 100)) + "%",
                        a.toNearestInt().removeFromTop (16), Justification::left);
        }
        void paintMultiband (Graphics& g, Rectangle<float> a)
        {
            static const char* names[3] { "low", "mid", "high" };
            const float gr = effect.gainReductionDb();
            float x = a.getX();
            const float bandW = a.getWidth() / 3.0f;
            for (int k = 0; k < 3; ++k)
            {
                auto bar = Rectangle<float> (x + 6.0f, a.getY() + 16.0f, bandW - 12.0f, a.getHeight() - 32.0f);
                g.setColour (Colour (0xff14171b));
                g.fillRoundedRectangle (bar, 4.0f);
                const float fill = jlimit (0.0f, 1.0f, (gr / 24.0f) * (1.0f - 0.2f * k));
                g.setColour (kBandColours[k]);
                g.fillRoundedRectangle (bar.withHeight (bar.getHeight() * fill)
                                           .withY (bar.getBottom() - bar.getHeight() * fill), 4.0f);
                g.setColour (kDim);
                g.setFont (Font (9.0f));
                g.drawText (names[k], bar.toNearestInt().removeFromBottom (14), Justification::centred);
                x += bandW;
            }
            g.setColour (kText);
            g.setFont (Font (13.0f).boldened());
            g.drawText ("out " + String (gr, 1) + " / 24 dB", a.toNearestInt().removeFromTop (16), Justification::left);
        }

        void paintEnsemble (Graphics& g, Rectangle<float> a)
        {
            const float rate = effect.param (0);
            const float depth = effect.param (1) / 100.0f;
            for (int v = 0; v < 3; ++v)
            {
                Path p2;
                for (float x = a.getX(); x <= a.getRight(); x += 1.5f)
                {
                    const float t = (x - a.getX()) / jmax (1.0f, a.getWidth()) * 6.0f * MathConstants<float>::twoPi;
                    const float y = a.getCentreY()
                                  - std::sin (t * rate * 0.35f + (float) v * 2.1f) * depth * a.getHeight() * 0.30f
                                  - (float) (v - 1) * a.getHeight() * 0.16f;
                    if (x <= a.getX() + 0.5f) p2.startNewSubPath (x, y); else p2.lineTo (x, y);
                }
                g.setColour (kBandColours[v * 2].withAlpha (0.9f));
                g.strokePath (p2, PathStrokeType (1.5f));
            }
            g.setColour (kDim);
            g.setFont (Font (10.0f));
            g.drawText ("three voices", a.toNearestInt().removeFromTop (14), Justification::left);
        }

        void paintMaximizer (Graphics& g, Rectangle<float> a)
        {
            // rows: title, subtitle, then the gain reduction bar
            auto titleRow = a.removeFromTop (20.0f);
            g.setColour (kText);
            g.setFont (Font (13.0f).boldened());
            g.drawText ("GR " + String (effect.gainReductionDb(), 1) + " dB    ceiling "
                        + String (effect.param (2), 1) + " dB",
                        titleRow.toNearestInt(), Justification::left);
            auto subRow = a.removeFromTop (14.0f);
            g.setColour (kDim);
            g.setFont (Font (10.0f));
            g.drawText ("look-ahead limiter  ·  5 ms", subRow.toNearestInt(), Justification::left);
            a.removeFromTop (4.0f);

            auto bars = a;
            const float gr = effect.gainReductionDb();
            const float y = bars.getBottom() - jlimit (0.0f, 1.0f, (gr + 24.0f) / 24.0f) * bars.getHeight();
            g.setColour (Colour (0xff14171b));
            g.fillRoundedRectangle (bars, 4.0f);
            g.setGradientFill (ColourGradient (kBandColours[2].withAlpha (0.55f), bars.getX(), bars.getBottom(),
                                               kBandColours[2].withAlpha (0.05f), bars.getX(), bars.getY(), false));
            g.fillRoundedRectangle (bars.withTrimmedTop (jmax (0.0f, y - bars.getY())), 4.0f);
            g.setColour (Colours::white.withAlpha (0.75f));
            g.drawHorizontalLine ((int) y, bars.getX(), bars.getRight());
        }
        void paintClipper (Graphics& g, Rectangle<float> a)
        {
            static const char* shapes[3] { "hard", "soft", "fold" };
            const int shape = jlimit (0, 2, (int) std::round (effect.param (1)));
            const float ceiling = Decibels::decibelsToGain (effect.param (0));
            const float drive = Decibels::decibelsToGain (effect.param (2));
            g.setColour (kGrid.withAlpha (0.5f));
            g.drawLine (a.getX(), a.getCentreY(), a.getRight(), a.getCentreY(), 0.8f);
            g.drawLine (a.getCentreX(), a.getY(), a.getCentreX(), a.getBottom(), 0.8f);
            const float cy = a.getCentreY() - ceiling * a.getHeight() * 0.40f;
            g.setColour (kBandColours[3].withAlpha (0.45f));
            g.drawHorizontalLine ((int) cy, a.getX(), a.getRight());
            g.drawHorizontalLine ((int) (a.getCentreY() + (a.getCentreY() - cy)), a.getX(), a.getRight());
            Path curve;
            for (float x = a.getX(); x <= a.getRight(); x += 1.0f)
            {
                const float in = jmap (x, a.getX(), a.getRight(), -2.0f, 2.0f);
                const float d = in * drive;
                float outVal = 0.0f;
                switch (shape)
                {
                    case 1:  outVal = ceiling * std::tanh (d / jmax (ceiling, 1.0e-4f)); break;
                    case 2:  outVal = std::sin (jlimit (-3.0f, 3.0f, d / jmax (ceiling, 1.0e-4f)) * 0.9f) * ceiling; break;
                    default: outVal = jlimit (-ceiling, ceiling, d); break;
                }
                const float y = a.getCentreY() - jlimit (-1.4f, 1.4f, outVal) * a.getHeight() * 0.40f;
                if (x <= a.getX() + 0.5f) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
            }
            g.setColour (kBandColours[2]);
            g.strokePath (curve, PathStrokeType (2.2f));
            g.setColour (kDim);
            g.setFont (Font (10.0f));
            g.drawText (String (shapes[shape]) + "  ·  clip " + String (effect.param (0), 1) + " dB"
                        + "  ·  " + String ((int) std::round (effect.param (3))) + "x",
                        a.toNearestInt().removeFromTop (14), Justification::left);
        }
        void paintTuner (Graphics& g, Rectangle<float> a)
        {
            auto* tuner = dynamic_cast<SxTuner*> (&effect);
            const bool has = tuner != nullptr && tuner->signalPresent();
            const int note = tuner != nullptr ? tuner->noteIndex() : -1;
            const float cents = tuner != nullptr ? tuner->cents() : 0.0f;
            static const char* names[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            auto big = a.removeFromTop (a.getHeight() * 0.58f);
            g.setColour (has && note >= 0 ? kBandColours[1] : kDim);
            g.setFont (Font (46.0f).boldened());
            const String label = (has && note >= 0) ? String (names[((note % 12) + 12) % 12]) + String (note / 12 - 1)
                                                    : String ("--");
            g.drawFittedText (label, big.toNearestInt(), Justification::centred, 1);
            g.setColour (kDim);
            g.setFont (Font (11.0f));
            g.drawText (has ? String (cents, 1) + " cents" : "no signal",
                        big.toNearestInt().removeFromBottom (16), Justification::centred);
            auto track = a.withSizeKeepingCentre (a.getWidth() * 0.8f, 26.0f);
            g.setColour (Colour (0xff14171b));
            g.fillRoundedRectangle (track, 8.0f);
            g.setColour (kGrid);
            for (int i = -50; i <= 50; i += 10)
            {
                const float x = track.getCentreX() + (float) i / 50.0f * track.getWidth() * 0.5f;
                g.drawVerticalLine ((int) x, track.getY() + 4.0f, track.getY() + (i == 0 ? 22.0f : 10.0f));
            }
            g.setColour (has && std::abs (cents) < 5.0f ? kBandColours[1] : kBandColours[3]);
            const float nx = track.getCentreX() + jlimit (-50.0f, 50.0f, cents) / 50.0f * track.getWidth() * 0.46f;
            g.fillRoundedRectangle (nx - 3.0f, track.getY() + 2.0f, 6.0f, track.getHeight() - 4.0f, 3.0f);
        }

        static float logX (Rectangle<float> a, float hz)
        {
            return a.getX() + a.getWidth() * (std::log10 (jlimit (20.0f, 20000.0f, hz) / 20.0f) / 3.0f);
        }
        void paintModulation (Graphics& g, Rectangle<float> a)
        {
            const auto accent = accentFor (effect.kind);
            const float rate = effect.param (0);
            const float depth = effect.param (1) / 100.0f;
            const float cycles = jlimit (1.0f, 4.0f, rate);
            Path lfo;
            for (int i = 0; i <= 400; ++i)
            {
                const float t = (float) i / 400.0f;
                const float v = std::sin (MathConstants<float>::twoPi * t * cycles);
                const float x = a.getX() + a.getWidth() * t;
                const float y = a.getCentreY() - v * a.getHeight() * 0.35f * jmax (0.15f, depth);
                if (i == 0) lfo.startNewSubPath (x, y);
                else lfo.lineTo (x, y);
            }
            g.setColour (kGrid);
            g.drawHorizontalLine ((int) a.getCentreY(), a.getX(), a.getRight());
            g.setColour (accent.withAlpha (0.25f));
            g.strokePath (lfo, PathStrokeType (1.2f));
            g.setColour (accent);
            g.strokePath (lfo, PathStrokeType (2.0f));

            // the live phase marker
            if (effect.kind != BuiltInEffect::Tremolo || true)
            {
                const float ph = effect.lfoPhase();
                const float x = a.getX() + a.getWidth() * ph;
                const float v = std::sin (MathConstants<float>::twoPi * ph * cycles);
                const float y = a.getCentreY() - v * a.getHeight() * 0.35f * jmax (0.15f, depth);
                g.setColour (Colours::white);
                g.fillEllipse (x - 3.0f, y - 3.0f, 6.0f, 6.0f);
            }
            g.setColour (kDim);
            g.setFont (Font (11.0f));
            g.drawText (String (rate, 2) + " Hz  ·  depth " + String (effect.param (1), 0) + " %",
                        a.toNearestInt().removeFromTop (16), Justification::centredLeft);
        }

        SxEffectBase& effect;
        int dragBand = -1;
    };

    //==============================================================================
    //== presets, shared by every built-in editor ---------------------------------------------------
    struct FactoryPreset { BuiltInEffect kind; const char* name; int count; std::pair<int, float> values[6]; };

    static const FactoryPreset* factoryPresets (int& count)
    {
        static const FactoryPreset list[] {
            { BuiltInEffect::Compressor, "Vocal Glue", 4, { { 0, -22.0f }, { 1, 3.0f }, { 2, 12.0f }, { 5, 3.0f } } },
            { BuiltInEffect::Compressor, "Drum Punch", 4, { { 0, -16.0f }, { 1, 6.0f }, { 2, 2.0f }, { 5, 2.0f } } },
            { BuiltInEffect::Compressor, "Gentle Bus", 3, { { 0, -14.0f }, { 1, 2.0f }, { 3, 250.0f } } },
            { BuiltInEffect::Limiter, "Transparent", 3, { { 4, 0.0f }, { 2, 3.0f }, { 1, 120.0f } } },
            { BuiltInEffect::Limiter, "Loud Clip", 3, { { 4, 3.0f }, { 0, -0.2f }, { 1, 40.0f } } },
            { BuiltInEffect::Delay, "Ping Pong 1/8", 4, { { 2, 1.0f }, { 3, 0.0f }, { 1, 35.0f }, { 5, 30.0f } } },
            { BuiltInEffect::Delay, "Slap Back", 3, { { 0, 110.0f }, { 1, 12.0f }, { 5, 25.0f } } },
            { BuiltInEffect::Chorus, "Wide 80s", 3, { { 0, 0.35f }, { 1, 70.0f }, { 2, 55.0f } } },
            { BuiltInEffect::Tape, "Warm Tape", 4, { { 0, 55.0f }, { 1, 55.0f }, { 2, 30.0f }, { 5, 45.0f } } },
            { BuiltInEffect::Tape, "Lo-Fi", 4, { { 0, 75.0f }, { 4, 40.0f }, { 2, 65.0f }, { 5, 25.0f } } },
            { BuiltInEffect::Saturator, "Tube Warmth", 3, { { 0, 1.0f }, { 1, 35.0f }, { 2, 45.0f } } },
            { BuiltInEffect::Saturator, "Diode Drive", 3, { { 0, 2.0f }, { 1, 60.0f }, { 2, 60.0f } } },
            { BuiltInEffect::Maximizer, "Transparent Master", 3, { { 1, -8.0f }, { 3, 220.0f }, { 4, 0.0f } } },
            { BuiltInEffect::Maximizer, "Loud Master", 3, { { 0, 4.0f }, { 1, -12.0f }, { 4, 1.0f } } },
            { BuiltInEffect::MultibandComp, "OTT Full", 3, { { 0, 85.0f }, { 1, 60.0f }, { 2, 40.0f } } },
            { BuiltInEffect::Exciter, "Air", 3, { { 4, 65.0f }, { 5, 45.0f }, { 7, 30.0f } } },
            { BuiltInEffect::Exciter, "Full Range", 4, { { 2, 25.0f }, { 3, 35.0f }, { 4, 45.0f }, { 5, 55.0f } } },
        };
        count = (int) (sizeof (list) / sizeof (list[0]));
        return list;
    }

    static File presetFolder (BuiltInEffect kind)
    {
        return File::getSpecialLocation (File::userApplicationDataDirectory)
                   .getChildFile ("SoundX").getChildFile ("Presets")
                   .getChildFile ("kind" + String ((int) kind));
    }

    static void applyFactoryPreset (SxEffectBase& e, const FactoryPreset& p)
    {
        for (int i = 0; i < p.count; ++i)
            e.setParamPublic (p.values[i].first, p.values[i].second);
    }

    static void showPresetMenu (SxEffectBase& e, Component& target)
    {
        PopupMenu m;
        int count = 0;
        const auto* presets = factoryPresets (count);
        PopupMenu factory;
        int id = 100;
        std::vector<const FactoryPreset*> mine;
        for (int i = 0; i < count; ++i)
            if (presets[i].kind == e.kind) { mine.push_back (&presets[i]); factory.addItem (id++, presets[i].name); }
        m.addSubMenu (TRANS ("Factory Presets"), factory, ! mine.empty());

        const auto folder = presetFolder (e.kind);
        PopupMenu user;
        if (folder.isDirectory())
        {
            auto files = folder.findChildFiles (File::findFiles, false, "*.sxpreset");
            files.sort();
            for (int i = 0; i < files.size(); ++i) user.addItem (1000 + i, files[i].getFileNameWithoutExtension());
        }
        m.addSubMenu (TRANS ("User Presets"), user, user.getNumItems() > 0);
        m.addSeparator();
        m.addItem (1, TRANS ("Save Current Preset..."));

        auto* self = &e;
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (&target), [self, mine, folder] (int r)
        {
            if (r == 1)
            {
                folder.createDirectory();
                self->savePresetFile (folder.getChildFile (self->getName().replace (" ", "_") + ".sxpreset"));
                return;
            }
            if (r >= 100 && r < 100 + (int) mine.size())
            {
                for (int i = 0; i < mine[(size_t) (r - 100)]->count; ++i)
                    self->setParamPublic (mine[(size_t) (r - 100)]->values[i].first, mine[(size_t) (r - 100)]->values[i].second);
                return;
            }
            if (r >= 1000)
            {
                auto files = folder.findChildFiles (File::findFiles, false, "*.sxpreset");
                files.sort();
                const int i = r - 1000;
                if (i >= 0 && i < files.size()) self->loadPresetFile (files[i]);
            }
        });
    }

    /** A small icon button for the host row under the title (enable / side-chain / bypass). */
    class SxIconButton : public Component, public SettableTooltipClient
    {
    public:
        enum Icon { Power, Sidechain, Bypass };
        std::function<void()> onClick;

        SxIconButton (Icon i, const String& tip, Colour activeColour) : icon (i), onColour (activeColour)
        {
            setTooltip (tip);
            setMouseCursor (MouseCursor::PointingHandCursor);
        }

        void setActive (bool a) { if (active != a) { active = a; repaint(); } }

        void paint (Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (active ? onColour : Colour (0xff252a32));
            g.fillRoundedRectangle (b, 3.0f);
            g.setColour (active ? onColour.brighter (0.4f) : Colour (0xff39414c));
            g.drawRoundedRectangle (b, 3.0f, 1.0f);
            const auto c = b.getCentre();
            const float s = 4.2f, th = 1.3f;
            g.setColour (active ? Colour (0xff10131a) : Colour (0xffc9d1dc));
            if (icon == Power)
            {
                g.drawEllipse (c.x - s, c.y - s + 0.6f, s * 2.0f, s * 2.0f, th);
                g.drawLine (c.x, c.y - s - 1.2f, c.x, c.y - 0.4f, th);
            }
            else if (icon == Sidechain)
            {
                g.drawRect (c.x + 0.6f, c.y - s + 1.2f, s * 1.5f, s * 1.7f, th);
                g.drawLine (c.x - s - 1.0f, c.y, c.x - 0.6f, c.y, th);
                Path head;
                head.startNewSubPath (c.x - 2.4f, c.y - 1.9f);
                head.lineTo (c.x - 0.6f, c.y);
                head.lineTo (c.x - 2.4f, c.y + 1.9f);
                g.strokePath (head, PathStrokeType (th));
            }
            else
            {
                Path bend;
                bend.startNewSubPath (c.x - s - 0.6f, c.y + 2.4f);
                bend.lineTo (c.x - 0.8f, c.y + 2.4f);
                bend.quadraticTo (c.x + 2.4f, c.y + 2.4f, c.x + 2.4f, c.y - 1.0f);
                bend.lineTo (c.x + 2.4f, c.y - s + 0.6f);
                g.strokePath (bend, PathStrokeType (th));
                g.drawLine (c.x - s - 0.6f, c.y - 2.4f, c.x + s + 0.6f, c.y - 2.4f, th * 0.8f);
            }
        }

        void mouseUp (const MouseEvent& e) override
        {
            if (getLocalBounds().contains (e.getPosition()) && onClick != nullptr) onClick();
        }

    private:
        Icon icon;
        Colour onColour;
        bool active = false;
    };
    class SxBigEditor : public AudioProcessorEditor, private Timer
    {
    public:
        explicit SxBigEditor (SxEffectBase& e)
            : AudioProcessorEditor (&e), effect (e), accent (accentFor (e.kind))
        {
            display = std::make_unique<Display> (effect);
            addAndMakeVisible (*display);
            for (int i = 0; i < effect.numParams(); ++i)
            {
                auto* k = new Knob (effect, i, accent);
                knobs.add (k);
                addAndMakeVisible (k);
            }
            presetBtn.setButtonText (TRANS ("PRESET"));
            presetBtn.setColour (TextButton::buttonColourId, kPanel2);
            presetBtn.setColour (TextButton::textColourOffId, kText);
            presetBtn.setMouseCursor (MouseCursor::PointingHandCursor);
            presetBtn.onClick = [this] { showPresetMenu (effect, presetBtn); };
            addAndMakeVisible (presetBtn);

            setWantsKeyboardFocus (true);
            setResizable (true, false);   // the window provides the only resize grip
            setResizeLimits (420, 260, 1600, 900);
            setSize (effect.numParams() > 8 ? 760 : 660, 400);
            startTimerHz (30);
        }

        void paint (Graphics& g) override
        {
            g.fillAll (kBg);
            auto head = getLocalBounds().removeFromTop (46);
            g.setColour (kPanel);
            g.fillRect (head);
            g.setColour (accent);
            g.fillRect (head.getX(), head.getY(), 4, head.getHeight());
            g.setColour (kText);
            g.setFont (Font (17.0f).boldened());
            g.drawFittedText (effect.getName(), head.reduced (16, 0).removeFromLeft (head.getWidth() / 2),
                              Justification::centredLeft, 1);
            g.setColour (kDim);
            g.setFont (Font (11.0f));
            g.drawFittedText ("SoundX", head.reduced (16, 0).removeFromRight (120), Justification::centredRight, 1);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto header = r.removeFromTop (46);
            presetBtn.setBounds (header.removeFromRight (86).reduced (6, 11));
            auto knobArea = r.removeFromBottom (jlimit (84, 110, r.getHeight() / 3));
            display->setBounds (r.reduced (10, 8));
            knobArea.reduce (8, 4);
            const int n = jmax (1, knobs.size());
            const int perRow = jmin (n, jmax (4, knobArea.getWidth() / 64));
            const int rows = (n + perRow - 1) / perRow;
            const int w = knobArea.getWidth() / perRow;
            const int h = knobArea.getHeight() / jmax (1, rows);
            for (int i = 0; i < knobs.size(); ++i)
            {
                const int row = i / perRow, col = i % perRow;
                knobs[i]->setBounds (knobArea.getX() + col * w, knobArea.getY() + row * h, w, h);
            }
        }

    private:
        void timerCallback() override
        {
            const auto stamp = effect.displayStamp();
            if (stamp == lastStamp) return;
            lastStamp = stamp;
            for (auto* k : knobs) k->refresh();
            display->repaint();
        }

        SxEffectBase& effect;
        Colour accent;
        std::uint64_t lastStamp = 0;
        std::unique_ptr<Display> display;
        OwnedArray<Knob> knobs;
        TextButton presetBtn;
    };
}

//==============================================================================
AudioProcessorEditor* SxEffectBase::createEditor()
{
    return new SxBigEditor (*this);
}



//==============================================================================
//== SX EQ editor: analyser + response + freely added / removed bands
namespace
{
    const Colour kInkOnBlack { 0xffe9ebee };
    const Colour kInkOnSilver { 0xff2b2b2b };

    void pr (Graphics& g, const String& text, Rectangle<int> area, Colour c, float size,
             Justification j = Justification::centred, bool bold = true)
    {
        g.setColour (c);
        g.setFont (bold ? Font (size).boldened() : Font (size));
        g.drawFittedText (text, area, j, 1, 0.7f);
    }

    void screw (Graphics& g, Point<float> p, Colour panel)
    {
        g.setColour (panel.darker (0.5f));
        g.fillEllipse (p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f);
        g.setColour (panel.brighter (0.18f));
        g.fillEllipse (p.x - 3.4f, p.y - 3.4f, 6.8f, 6.8f);
        g.setColour (Colour (0xff2f2f2f).withAlpha (0.8f));
        g.drawLine (p.x - 3.0f, p.y + 1.0f, p.x + 3.0f, p.y - 1.0f, 1.0f);
    }

#include "EqEditor.inc"
    //==============================================================================
    /** Knob with the printed numbers and the fine ticks on the ring outside it - the way the
        originals print their scales. */
    class Knob2 : public Component, private SettableTooltipClient
    {
    public:
        enum class Finish { silver, black, cap };

        Knob2 (SxEffectBase& e, int index, Finish f, Colour ink, String caption, bool captionAbove,
               std::vector<String> numbers, float capFraction = 0.26f, float ringFactor = 1.85f,
               float captionSize = 10.0f, float startDeg = 225.0f, float sweepDeg = 270.0f)
            : effect (e), idx (index), finish (f), inkColour (ink), captionText (std::move (caption)),
              above (captionAbove), labels (std::move (numbers)), capFrac (capFraction), ringF (ringFactor),
              capFont (captionSize), start (startDeg * MathConstants<float>::pi / 180.0f),
              sweep (sweepDeg * MathConstants<float>::pi / 180.0f)
        {
            setMouseCursor (MouseCursor::UpDownResizeCursor);
            setTooltip (effect.spec (idx).name);
        }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            auto captionStrip = r.removeFromBottom (capFont + 10.0f);
            if (above)
            {
                captionStrip = getLocalBounds().toFloat().removeFromTop (capFont + 10.0f);
                r = getLocalBounds().toFloat().withTrimmedTop (capFont + 10.0f);
            }
            const float base = jmin (r.getWidth(), r.getHeight());
            const float capR = base * capFrac * 0.5f;
            const float numR = capR * ringF;
            const auto c = r.getCentre();
            const bool silver = finish == Finish::silver;
            const Colour ink = inkColour;

            // ---- flat printed scale: one thin arc, one tick grid shared by numbers and ticks ----
            if (! labels.empty())
            {
                const int majors = jmax (1, (int) labels.size() - 1);
                const int perMajor = 4;                       // four ticks per printed number
                const int ticks = majors * perMajor;

                Path arc;
                const int steps = 64;
                for (int i = 0; i <= steps; ++i)
                {
                    const float a2 = start + (float) i / (float) steps * sweep;
                    const auto p2 = c + Point<float> (std::cos (a2), std::sin (a2)) * (numR - 2.0f);
                    if (i == 0) arc.startNewSubPath (p2); else arc.lineTo (p2);
                }
                g.setColour (ink.withAlpha (0.28f));
                g.strokePath (arc, PathStrokeType (0.8f));

                for (int i = 0; i <= ticks; ++i)
                {
                    const float a2 = start + (float) i / (float) ticks * sweep;
                    const Point<float> d (std::cos (a2), std::sin (a2));
                    const bool major = (i % perMajor) == 0;
                    g.setColour (ink.withAlpha (major ? 0.8f : 0.42f));
                    g.drawLine ({ c + d * (numR - (major ? 6.0f : 3.5f)), c + d * (numR - 1.0f) }, 1.0f);
                }

                for (size_t i = 0; i < labels.size(); ++i)
                {
                    const float t = labels.size() > 1 ? (float) i / (float) (labels.size() - 1) : 0.0f;
                    const float a2 = start + t * sweep;
                    const auto p2 = c + Point<float> (std::cos (a2), std::sin (a2)) * (numR + 9.0f);
                    g.setColour (ink.withAlpha (0.70f));
                    g.setFont (Font (jmax (7.0f, base * 0.062f)).boldened().withExtraKerningFactor (0.04f));
                    g.drawFittedText (labels[i], Rectangle<int> ((int) p2.x - 15, (int) p2.y - 7, 30, 14),
                                      Justification::centred, 1, 0.8f);
                }
            }

            // ---- knob body: flat hardware style (no gradients, one outline, one pointer) --------
            const bool cap = finish == Finish::cap;
            const auto body = Rectangle<float> (capR * 2.0f, capR * 2.0f).withCentre (c);
            g.setColour (cap ? Colour (0xffc8ccd1) : (silver ? Colour (0xff9aa0a6) : Colour (0xff1b1c1f)));
            g.fillEllipse (body);
            g.setColour (cap ? Colour (0xff7f858c) : (silver ? Colour (0xff5d636a) : Colour (0xff3a3d42)));
            g.drawEllipse (body, 1.2f);

            if (cap)
            {
                // flat chrome cap, printed pointer
                const auto capDisc = Rectangle<float> (capR * 1.20f, capR * 1.20f).withCentre (c);
                g.setColour (Colour (0xffe2e5e9));
                g.fillEllipse (capDisc);
                g.setColour (Colour (0xff9aa0a6));
                g.drawEllipse (capDisc, 1.0f);
            }

            const float norm = (float) effect.getParameters()[(int) idx]->getValue();
            const float a3 = start + norm * sweep;
            const Point<float> d3 (std::cos (a3), std::sin (a3));
            g.setColour (cap || silver ? Colour (0xff22252a) : Colour (0xfff2f4f7));
            g.drawLine ({ c + d3 * (capR * (cap ? 0.12f : 0.16f)), c + d3 * (capR * (cap ? 0.66f : 0.94f)) },
                        cap ? 2.2f : 2.0f);
            if (cap)
            {
                g.setColour (Colour (0xff5d636a));
                g.drawLine ({ c + d3 * (capR * 0.88f), c + d3 * (capR * 0.98f) }, 2.4f);
            }

            // ---- flat caption ------------------------------------------------------------------
            g.setColour (ink.withAlpha (0.92f));
            g.setFont (Font (capFont).boldened().withExtraKerningFactor (0.08f));
            g.drawFittedText (captionText, captionStrip.toNearestInt(), Justification::centred, 1, 0.8f);
        }
        void mouseDown (const MouseEvent& e) override
        {
            if (e.mods.isAltDown())
            {
                auto* p = effect.getParameters()[(int) idx];
                p->beginChangeGesture();
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
                    p->setValueNotifyingHost (rp->convertTo0to1 (effect.spec (idx).def));
                p->endChangeGesture();
                repaint();
                return;
            }
            dragStart = e.getPosition().y;
            startNorm = (float) effect.getParameters()[(int) idx]->getValue();
            effect.getParameters()[(int) idx]->beginChangeGesture();
        }
        void mouseDrag (const MouseEvent& e) override
        {
            const float delta = (float) (dragStart - e.getPosition().y) / (e.mods.isShiftDown() ? 600.0f : 150.0f);
            effect.getParameters()[(int) idx]->setValueNotifyingHost (jlimit (0.0f, 1.0f, startNorm + delta));
            repaint();
        }
        void mouseUp (const MouseEvent&) override { effect.getParameters()[(int) idx]->endChangeGesture(); }
        void mouseDoubleClick (const MouseEvent&) override
        {
            const auto& s = effect.spec (idx);
            auto* aw = new AlertWindow (s.name, "Enter a value", MessageBoxIconType::NoIcon);
            aw->addTextEditor ("v", String (jmap ((float) effect.getParameters()[(int) idx]->getValue(), s.min, s.max), 3), "");
            aw->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
            aw->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
            auto* self = this;
            aw->enterModalState (true, ModalCallbackFunction::create ([self, aw, s] (int result)
            {
                if (result != 1) return;
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (self->effect.getParameters()[(int) self->idx]))
                    self->effect.getParameters()[(int) self->idx]->setValueNotifyingHost (
                        rp->convertTo0to1 (jlimit (s.min, s.max, aw->getTextEditorContents ("v").getFloatValue())));
                self->repaint();
            }), true);
        }
        void refresh() { repaint(); }

    private:
        SxEffectBase& effect;
        int idx = 0;
        Finish finish;
        Colour inkColour;
        String captionText;
        bool above = false;
        std::vector<String> labels;
        float capFrac = 0.26f, ringF = 1.85f, capFont = 10.0f, start = 0.0f, sweep = 0.0f;
        float dragStart = 0.0f, startNorm = 0.0f;
    };

    //==============================================================================
    /** A vertical metal toggle with a caption above and below (Limit / Comp style). */
    class Flip : public Component
    {
    public:
        Flip (String topLabel, String bottomLabel, std::function<bool()> getter, std::function<void (bool)> setter,
              Colour ink, bool sideways = false)
            : topText (std::move (topLabel)), bottomText (std::move (bottomLabel)),
              get (std::move (getter)), set (std::move (setter)), inkColour (ink), horizontal (sideways)
        {
            setMouseCursor (MouseCursor::PointingHandCursor);
        }
        void paint (Graphics& g) override
        {
            const bool on = get != nullptr && get();
            if (horizontal)
            {
                // labels either side of a horizontal switch, like the In / Out flip on the unit
                auto r = getLocalBounds();
                auto left = r.removeFromLeft (jmax (18, r.getWidth() / 3));
                auto right = r.removeFromRight (jmax (18, r.getWidth() / 3));
                pr (g, topText, left, inkColour, 7.5f);
                if (bottomText.isNotEmpty()) pr (g, bottomText, right, inkColour, 7.5f);
                const auto c = r.toFloat().getCentre();
                const float w = jmax (18.0f, (float) r.getWidth() - 4.0f), h = 11.0f;
                g.setColour (Colour (0xffd2d5d9));
                g.fillRoundedRectangle (c.x - w * 0.5f, c.y - h * 0.5f, w, h, h * 0.5f);
                g.setColour (Colour (0xff8d9297));
                g.drawRoundedRectangle (c.x - w * 0.5f, c.y - h * 0.5f, w, h, h * 0.5f, 1.0f);
                g.setColour (Colour (0xff23272b));
                g.fillRoundedRectangle (c.x - w * 0.5f + 1.5f, c.y - h * 0.5f + 1.5f, w - 3.0f, h - 3.0f, (h - 3.0f) * 0.5f);
                const float tipX = on ? c.x + w * 0.5f - 7.0f : c.x - w * 0.5f + 7.0f;
                g.setColour (Colour (0xffe7eaee));
                g.fillEllipse (tipX - 3.0f, c.y - 3.0f, 6.0f, 6.0f);
                return;
            }

            auto r = getLocalBounds();
            auto top = r.removeFromTop (13);
            auto bottom = r.removeFromBottom (13);
            pr (g, topText, top, inkColour, 9.0f);
            if (bottomText.isNotEmpty()) pr (g, bottomText, bottom, inkColour, 9.0f);
            const auto c = r.toFloat().getCentre();
            // chrome bezel
            g.setColour (Colour (0xffd2d5d9));
            g.fillEllipse (c.x - 7.0f, c.y - 16.0f, 14.0f, 32.0f);
            g.setColour (Colour (0xff8d9297));
            g.drawEllipse (c.x - 7.0f, c.y - 16.0f, 14.0f, 32.0f, 1.0f);
            g.setColour (Colour (0xff23272b));
            g.fillEllipse (c.x - 4.0f, c.y - 13.0f, 8.0f, 26.0f);
            // lever
            const float tipY = on ? c.y - 10.0f : c.y + 10.0f;
            g.setColour (Colour (0xffe7eaee));
            g.fillEllipse (c.x - 3.0f, tipY - 3.0f, 6.0f, 6.0f);
        }
        void mouseDown (const MouseEvent&) override
        {
            if (set != nullptr && get != nullptr) { set (! get()); repaint(); }
        }

    private:
        String topText, bottomText;
        std::function<bool()> get;
        std::function<void (bool)> set;
        Colour inkColour;
        bool horizontal = false;
    };

    //==============================================================================
    /** The framed meter; redZoneStart/End are fractions of the printed arc. */
    class Meter2 : public Component
    {
    public:
        Meter2 (SxEffectBase& e, String brandLine1, String brandLine2, Colour ink, Colour frame,
                float redFrom, float redTo, bool redScriptBrand)
            : effect (e), brand1 (std::move (brandLine1)), brand2 (std::move (brandLine2)), inkColour (ink),
              frameColour (frame), redStart (redFrom), redEnd (redTo), scriptBrand (redScriptBrand) {}

        void paint (Graphics& g) override
        {
            auto outer = getLocalBounds().toFloat();
            g.setColour (frameColour);
            g.fillRect (outer);
            g.setColour (frameColour.brighter (0.35f));
            g.drawRect (outer.reduced (1.0f), 1.0f);
            auto face = outer.reduced (jmax (6.0f, outer.getHeight() * 0.10f), jmax (5.0f, outer.getHeight() * 0.09f));
            g.setGradientFill (ColourGradient (Colour (0xfff0d49b), face.getX(), face.getY(),
                                               Colour (0xffdfbe7d), face.getRight(), face.getBottom(), false));
            g.fillRect (face);

            const auto pivot = Point<float> (face.getCentreX(), face.getBottom() + face.getHeight() * 0.02f);
            // keep the arc inside the face even when the meter is wide and flat
            const float radius = jmin (face.getWidth() * 0.40f, face.getHeight() * 0.78f);
            const float a0 = MathConstants<float>::pi * 1.15f, a1 = MathConstants<float>::pi * 1.85f;
            auto dirAt = [&] (float t) { const float a = a0 + t * (a1 - a0); return std::make_pair (Point<float> (std::cos (a), std::sin (a)), a); };

            g.setColour (inkColour);
            for (int i = 0; i <= 30; ++i)
            {
                const float t = (float) i / 30.0f;
                const auto d = dirAt (t).first;
                const bool major = (i % 5) == 0;
                g.drawLine ({ pivot + d * (radius - (major ? 7.0f : 3.5f)), pivot + d * radius }, major ? 1.1f : 0.7f);
            }
            static const char* nums[9] { "20", "10", "7", "5", "3", "1", "0", "+1", "+2" };
            for (int i = 0; i < 9; ++i)
            {
                const float t = (float) i / 8.0f;
                const auto d = dirAt (t).first;
                const auto p = pivot + d * (radius + 8.0f);
                pr (g, nums[i], Rectangle<int> ((int) p.x - 10, (int) p.y - 6, 20, 12), inkColour,
                    jmax (6.5f, face.getHeight() * 0.055f), Justification::centred, false);
            }

            // red zone band on the printed arc
            {
                // sampled along the same direction the printed numbers use, so the band always
                // lands where the numbers say it should
                Path band;
                const float rMid = radius - 4.5f;
                const int steps = 14;
                for (int i = 0; i <= steps; ++i)
                {
                    const float t = redStart + (redEnd - redStart) * (float) i / (float) steps;
                    const auto p = pivot + dirAt (t).first * rMid;
                    if (i == 0) band.startNewSubPath (p);
                    else band.lineTo (p);
                }
                g.setColour (Colour (0xffc8102e));
                g.strokePath (band, PathStrokeType (7.0f, PathStrokeType::curved, PathStrokeType::butt));
            }

            if (brand1.isNotEmpty())
            {
                const auto area = Rectangle<int> ((int) face.getX(), (int) (face.getY() + face.getHeight() * 0.05f),
                                                  (int) face.getWidth(), 16);
                if (scriptBrand)
                {
                    g.setColour (Colour (0xffc8102e));
                    g.setFont (Font (jmax (10.0f, face.getHeight() * 0.10f)).italicised().boldened());
                    g.drawFittedText (brand1, Rectangle<int> ((int) face.getX(), (int) (face.getBottom() - face.getHeight() * 0.30f),
                                                              (int) face.getWidth(), 16),
                                      Justification::centred, 1);
                }
                else pr (g, brand1, area, inkColour, jmax (8.0f, face.getHeight() * 0.06f));
            }
            if (brand2.isNotEmpty())
                pr (g, brand2, Rectangle<int> ((int) face.getX(), (int) (face.getBottom() - 15),
                                               (int) face.getWidth(), 13),
                    inkColour.withAlpha (0.85f), jmax (7.0f, face.getHeight() * 0.048f));

            // the needle: GR moves left from zero, VU swings right with level along the printed scale
            float t;
            if (reading == Reading::vu)
            {
                const float db = jlimit (-20.0f, 2.5f, vuDb);
                t = db <= 0.0f ? 0.75f * (db + 20.0f) / 20.0f : 0.75f + 0.25f * db / 2.0f;
            }
            else if (reading == Reading::gr)
                t = 0.68f - (jlimit (0.0f, 20.0f, needleGr) / 20.0f) * 0.62f;
            else
                t = 0.04f;
            t = jlimit (0.02f, 0.98f, t);
            const auto d = dirAt (t).first;
            g.setColour (Colour (0xff141414));
            g.drawLine ({ pivot + d * 8.0f, pivot + d * (radius - 3.0f) }, 1.9f);
            g.fillEllipse (pivot.x - 3.5f, pivot.y - 3.5f, 7.0f, 7.0f);

            g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.15f), face.getX(), face.getY(),
                                               Colours::transparentWhite, face.getRight(), face.getBottom(), false));
            g.fillRect (face);
        }

        void setGainReduction (float gr) { needleGr = jmax (gr, needleGr * 0.86f); reading = Reading::gr; }

        /** The output level in dBFS shown against a reference offset (+4 / +8 on the 1176). */
        void setOutputDb (float db, float ref) { vuDb = jmax (db - ref, vuDb - 0.7f); reading = Reading::vu; }

        /** Parks the needle at the left end (the OFF button). */
        void setRest() { reading = Reading::rest; }

    private:
        enum class Reading { gr, vu, rest };

        SxEffectBase& effect;
        String brand1, brand2;
        Colour inkColour, frameColour;
        float redStart = 0.0f, redEnd = 0.1f, needleGr = 0.0f, vuDb = -100.0f;
        bool scriptBrand = false;
        Reading reading = Reading::gr;
    };

    //==============================================================================
    //== SX-76: black faceplate with chrome knobs, a printed VU meter and two lever columns -------
    class FetEditor : public AudioProcessorEditor, private Timer
    {
    public:
        explicit FetEditor (SxFetCompressor& e)
            : AudioProcessorEditor (&e), effect (e),
              inKnob (e, 0, Knob2::Finish::cap, kInkOnBlack, "INPUT", false,
                      { "0", "6", "12", "18", "24", "30", "36", "48" }, 0.30f, 1.95f, 10.5f),
              outKnob (e, 1, Knob2::Finish::cap, kInkOnBlack, "OUTPUT", false,
                       { "0", "6", "12", "18", "24", "30", "36", "48" }, 0.30f, 1.95f, 10.5f),
              atkKnob (e, 2, Knob2::Finish::cap, kInkOnBlack, "ATTACK", false,
                       { "OFF", "1", "2", "3", "4", "5", "6", "7" }, 0.34f, 2.10f, 9.5f),
              relKnob (e, 3, Knob2::Finish::cap, kInkOnBlack, "RELEASE", false,
                       { "1", "2", "3", "4", "5", "6", "7" }, 0.34f, 2.10f, 9.5f),
              meter (e, "SX-76LN", "", Colour (0xff232420), Colour (0xff2c2c2c), 0.86f, 0.99f, false)
        {
            for (auto* c : std::initializer_list<Component*> { &inKnob, &outKnob, &atkKnob, &relKnob, &meter })
                addAndMakeVisible (*c);
            setWantsKeyboardFocus (true);
            setResizable (true, false);   // the window provides the only resize grip
            setResizeLimits (700, 175, 1600, 560);
            setSize (760, 190);   // same compact footprint as the 2A
            startTimerHz (30);
        }

        void paint (Graphics& g) override
        {
            const auto W = (float) getWidth(), H = (float) getHeight();

            // ---- flat black faceplate ----------------------------------------------------------
            g.fillAll (Colour (0xff151619));
            g.setColour (Colour (0xff2f3237));
            g.drawRect (getLocalBounds().toFloat().reduced (1.0f), 1.0f);
            for (auto pt : { Point<float> (20.0f, 20.0f), Point<float> (W - 20.0f, 20.0f),
                             Point<float> (20.0f, H - 20.0f), Point<float> (W - 20.0f, H - 20.0f) })
                chromeScrew (g, pt);

            // ---- lever columns ----------------------------------------------------------------
            drawButtonColumn (g, ratioSlot, ratioCaption, "RATIO", ratioLabels, activeRatioIndex());
            drawButtonColumn (g, meterSlot, meterCaption, "METER", meterLabels, meterMode);

            // ---- printed model line under the meter -------------------------------------------
            auto under = Rectangle<int> (meter.getX(), meter.getBottom() + 6,
                                         meter.getWidth(), jmax (14, (int) (H * 0.075f)));
            pr (g, "PEAK LIMITER", under.removeFromTop (under.getHeight() / 2), kInkOnBlack.withAlpha (0.85f), 9.5f);
            pr (g, "SOUNDX", under, kInkOnBlack, 12.0f);
        }

        void resized() override
        {
            const float W = (float) getWidth(), H = (float) getHeight();

            // one row, like the hardware: IN / OUT / ATTACK+RELEASE / RATIO / VU / METER
            const float knobTop = H * 0.12f;
            const float knobH = H * 0.62f;
            const float bigW = W * 0.20f, smallW = W * 0.11f;

            inKnob .setBounds ((int) (W * 0.045f), (int) knobTop, (int) bigW, (int) knobH);
            outKnob.setBounds ((int) (W * 0.245f), (int) knobTop, (int) bigW, (int) knobH);

            const float smallX = W * 0.455f;
            atkKnob.setBounds ((int) smallX, (int) (H * 0.10f), (int) smallW, (int) (H * 0.40f));
            relKnob.setBounds ((int) smallX, (int) (H * 0.50f), (int) smallW, (int) (H * 0.40f));

            // the meter comes first: the switch columns are exactly as tall as the meter face and
            // their four buttons are stacked without any gap, so the column reads as tall as the VU
            const int meterW = (int) (W * 0.215f);   // a touch wider than the plate layout needs
            const int meterH = (int) jmin ((float) (H * 0.52f), (float) meterW / 1.55f);
            const int meterX = (int) (W * 0.685f);
            const int meterY = (int) ((H - (float) meterH) * 0.42f);
            meter.setBounds (meterX, meterY, meterW, meterH);

            const int btn = jmax (9, meterH / 4);
            const int colW = btn;
            ratioSlot = Rectangle<int> ((int) (W * 0.622f), meterY, colW, btn * 4);
            ratioCaption = Rectangle<int> (ratioSlot.getX() - 18, ratioSlot.getY(),
                                           16, ratioSlot.getHeight());
            meterSlot = Rectangle<int> ((int) (W * 0.906f), meterY, colW, btn * 4);
            meterCaption = Rectangle<int> (meterSlot.getX() - 20, meterSlot.getY(),
                                           18, meterSlot.getHeight());
        }

        void mouseDown (const MouseEvent& e) override
        {
            const int r = leverIndexAt (ratioSlot, e.getPosition());
            if (r >= 0)
            {
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (effect.getParameters()[4]))
                    effect.getParameters()[4]->setValueNotifyingHost (rp->convertTo0to1 (ratiosValue[r]));
                repaint();
                return;
            }
            const int m = leverIndexAt (meterSlot, e.getPosition());
            if (m >= 0) { meterMode = m; repaint(); }
        }

    private:
        static constexpr float ratiosValue[4] { 20.0f, 12.0f, 8.0f, 4.0f };
        static const char* ratioLabels[4];
        static const char* meterLabels[4];

        float brush (int y) const
        {
            uint32_t h2 = (uint32_t) (y * 2654435761u);
            h2 ^= h2 >> 15;
            return (float) (h2 & 0xffff) / 65535.0f;
        }

        void chromeScrew (Graphics& g, Point<float> c)
        {
            g.setColour (Colour (0xff8f959c));
            g.fillEllipse (Rectangle<float> (12.0f, 12.0f).withCentre (c));
            g.setColour (Colour (0xff5a6067));
            g.drawEllipse (Rectangle<float> (12.0f, 12.0f).withCentre (c), 1.0f);
            g.setColour (Colour (0xff4a5058));
            g.drawLine (c.x - 3.2f, c.y - 2.0f, c.x + 3.2f, c.y + 2.0f, 1.2f);
        }

        int activeRatioIndex() const
        {
            for (int i = 0; i < 4; ++i)
                if (std::abs (effect.param (4) - ratiosValue[i]) < 0.5f) return i;
            return 0;
        }

        static int leverIndexAt (Rectangle<int> slot, Point<int> p)
        {
            if (! slot.expanded (10, 4).contains (p)) return -1;
            const float rowH = (float) slot.getHeight() / 4.0f;
            const int idx = (int) ((float) (p.y - slot.getY()) / rowH);
            return jlimit (0, 3, idx);
        }

        void drawButtonColumn (Graphics& g, Rectangle<int> slot, Rectangle<int> caption,
                               const String& title, const char* const* labels, int active)
        {
            // small vertical silkscreen title
            g.saveState();
            g.setColour (kInkOnBlack.withAlpha (0.85f));
            g.setFont (Font (jlimit (6.0f, 9.0f, (float) slot.getHeight() * 0.040f)).boldened()
                           .withExtraKerningFactor (0.12f));
            g.addTransform (AffineTransform::rotation (-MathConstants<float>::halfPi,
                                                        (float) caption.getCentreX(), (float) caption.getCentreY()));
            g.drawFittedText (title, caption.withSizeKeepingCentre (caption.getHeight(), caption.getWidth()),
                              Justification::centred, 1, 0.8f);
            g.restoreState();

            // four small black buttons, touching each other, centred in the column
            const float side = jmax (9.0f, (float) slot.getWidth());
            const float top = (float) slot.getY();
            const auto labelFont = Font (jlimit (6.5f, 10.0f, side * 0.52f)).boldened();

            for (int i = 0; i < 4; ++i)
            {
                const bool on = i == active;
                auto btn = Rectangle<float> ((float) slot.getCentreX() - side * 0.5f, top + (float) i * side,
                                             side, side);

                g.setColour (Colour (0xff0c0d0f));
                g.fillRect (btn);
                // red outline when selected, thin grey when not: the classic indicator frame
                g.setColour (on ? Colour (0xffd21f2f) : Colour (0xff34383e));
                g.drawRect (btn, on ? 1.6f : 1.0f);

                g.setColour (on ? Colour (0xfff2f4f6) : kInkOnBlack.withAlpha (0.62f));
                g.setFont (labelFont);
                g.drawFittedText (labels[i], Rectangle<int> (slot.getRight() + 4,
                                                             (int) btn.getCentreY() - 6, 26, 12),
                                  Justification::centredLeft, 1, 0.7f);
            }
            juce::ignoreUnused (title);
        }
        void timerCallback() override
        {
            if (meterMode == 0)      meter.setGainReduction (effect.gainReductionDb());
            else if (meterMode == 1) meter.setOutputDb (effect.takeOutputLevelDb(), 4.0f);
            else if (meterMode == 2) meter.setOutputDb (effect.takeOutputLevelDb(), 8.0f);
            else                     meter.setRest();
            meter.repaint();
            inKnob.refresh(); atkKnob.refresh(); relKnob.refresh(); outKnob.refresh();
        }

        SxFetCompressor& effect;
        Knob2 inKnob, outKnob, atkKnob, relKnob;
        Meter2 meter;
        Rectangle<int> ratioSlot, meterSlot, ratioCaption, meterCaption;
        int meterMode = 0;
    };

    const char* FetEditor::ratioLabels[4] { "20", "12", "8", "4" };
    const char* FetEditor::meterLabels[4] { "GR", "+8", "+4", "OFF" };
    //== SX-2A: the three-section grey faceplate ------------------------------------------------
    class OptoEditor : public AudioProcessorEditor, private Timer
    {
    public:
        explicit OptoEditor (SxOptoCompressor& e)
            : AudioProcessorEditor (&e), effect (e),
              gainKnob (e, 1, Knob2::Finish::black, kInkOnSilver, "GAIN", false,
                        { "0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100" }, 0.34f, 1.66f, 11.0f),
              peakKnob (e, 0, Knob2::Finish::black, kInkOnSilver, "PEAK REDUCTION", false,
                        { "0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100" }, 0.34f, 1.66f, 11.0f),
              dryKnob (e, 3, Knob2::Finish::black, kInkOnSilver, "Dry", false, {}, 0.42f, 1.0f, 8.5f),
              hpKnob (e, 2, Knob2::Finish::black, kInkOnSilver, "Detector HP", false, {}, 0.42f, 1.0f, 7.0f),
              scKnob (e, 4, Knob2::Finish::black, kInkOnSilver, "SC Gain", false, {}, 0.42f, 1.0f, 7.0f),
              meter (e, "SoundX", {}, Colour (0xff2b2b2b), Colour (0xffb9b9b9), 0.78f, 0.95f, true),
              modeToggle ("LIMIT", "COMP", [this] { return effect.param (5) > 0.5f; },
                          [this] (bool on) { setParam (5, on ? 1.0f : 0.0f); }, kInkOnSilver),
              ioToggle ("In", "Out", [this] { return effect.param (6) > 0.5f; },
                        [this] (bool on) { setParam (6, on ? 1.0f : 0.0f); }, kInkOnSilver, true)
        {
            for (auto* c : std::initializer_list<Component*> { &gainKnob, &peakKnob, &dryKnob, &hpKnob, &scKnob,
                                                               &meter, &modeToggle, &ioToggle })
                addAndMakeVisible (*c);
            setWantsKeyboardFocus (true);
            setResizable (true, false);   // the window provides the only resize grip
            setResizeLimits (700, 175, 1600, 560);
            setSize (760, 190);
            startTimerHz (30);
        }

        void paint (Graphics& g) override
        {
            const auto W = getWidth(), H = getHeight();
            const Colour panel (0xffd8d6d2);
            g.fillAll (panel);

            // the hammered texture: a sparse, flat speckle, darker and lighter dots
            g.setColour (Colours::black.withAlpha (0.045f));
            for (int y = 3; y < H; y += 6)
                for (int x = 3 + ((y / 6) % 2) * 3; x < W; x += 8)
                    g.fillRect (x, y, 1, 1);
            g.setColour (Colours::white.withAlpha (0.05f));
            for (int y = 6; y < H; y += 6)
                for (int x = 6 + ((y / 6) % 2) * 3; x < W; x += 8)
                    g.fillRect (x, y, 1, 1);

            g.setColour (panel.darker (0.4f));
            g.drawRect (getLocalBounds().toFloat().reduced (0.5f), 1.0f);
            // the bottom strip: a slightly darker band for the three trim knobs
            if (! bottomStrip.isEmpty())
            {
                g.setColour (panel.darker (0.06f));
                g.fillRect (bottomStrip);
                g.setColour (kInkOnSilver.withAlpha (0.22f));
                g.drawHorizontalLine (bottomStrip.getY(), 6.0f, (float) W - 6.0f);
            }

            // the two dividers that make the three sections (they stop at the bottom strip)
            const float dividerBottom = (float) (bottomStrip.isEmpty() ? H - 8 : bottomStrip.getY() - 2);
            g.setColour (kInkOnSilver.withAlpha (0.30f));
            g.drawVerticalLine (section1.getRight(), 6.0f, dividerBottom);
            g.drawVerticalLine (section2.getRight(), 6.0f, dividerBottom);

            // middle section: the model plate above the meter, the GR caption below it
            pr (g, "SX-2A", Rectangle<int> (section2.getX(), 5, section2.getWidth(), 20), kInkOnSilver, 15.0f);
            if (! grCaption.isEmpty())
                pr (g, "GR", grCaption, kInkOnSilver.withAlpha (0.85f), 9.0f);

            screw (g, { 16.0f, 16.0f }, panel);
            screw (g, { (float) W - 16.0f, 16.0f }, panel);
            screw (g, { 16.0f, (float) H - 16.0f }, panel);
            screw (g, { (float) W - 16.0f, (float) H - 16.0f }, panel);
        }

        void resized() override
        {
            const int W = getWidth(), H = getHeight();
            // three columns: gain side, meter, peak-reduction side (like the reference layout)
            const int s1 = (int) (W * 0.335f), s2 = (int) (W * 0.295f);
            section1 = Rectangle<int> (0, 0, s1, H);
            section2 = Rectangle<int> (s1, 0, s2, H);
            section3 = Rectangle<int> (s1 + s2, 0, W - s1 - s2, H);
            bottomStrip = {};                       // no bottom strip any more

            const int pad = jmax (4, (int) ((float) H * 0.03f));

            // ---- left column: Limit/Comp flip, big gain, dry ---------------------------------
            // the flip sits in the top left, the dry knob under it, the gain knob takes the rest
            const int flipW = jlimit (20, 34, (int) ((float) s1 * 0.13f));
            const int flipH = jlimit (34, 58, (int) ((float) H * 0.26f));
            modeToggle.setBounds (section1.getX() + (int) ((float) s1 * 0.12f), pad + 4, flipW, flipH);

            const int dryD = jlimit (32, 64, (int) ((float) H * 0.30f));
            dryKnob.setBounds (section1.getX() + (int) ((float) s1 * 0.04f),
                               H - pad - dryD, dryD, dryD);

            const int gainD = jmin ((int) ((float) H * 0.74f), (int) ((float) s1 * 0.56f));
            gainKnob.setBounds (section1.getRight() - gainD - (int) ((float) s1 * 0.05f),
                                (int) ((float) H * 0.50f) - gainD / 2, gainD, gainD);

            // ---- middle column: model line, meter, GR caption, in / out flip ------------------
            const int headerH = jlimit (16, 26, (int) ((float) H * 0.11f));
            const int meterW = (int) ((float) s2 * 0.86f);
            const int ioH = jlimit (24, 42, (int) ((float) H * 0.21f));
            const int grH = 11;
            const int meterH = jmin ((int) ((float) meterW / 1.9f),
                                     jmax (34, H - headerH - pad - ioH - grH - 10));
            const int meterY = headerH + 2;
            meter.setBounds (section2.getCentreX() - meterW / 2, meterY, meterW, meterH);
            grCaption = Rectangle<int> (section2.getX(), meter.getBottom() + 1, section2.getWidth(), grH);
            const int ioW = jlimit (66, 92, (int) ((float) section2.getWidth() * 0.40f));
            const int ioH2 = jlimit (15, 20, (int) ((float) H * 0.085f));
            ioToggle.setBounds (section2.getCentreX() - ioW / 2,
                                jmin (H - pad - ioH2, grCaption.getBottom() + 3), ioW, ioH2);

            // ---- right column: detector HP and SC gain at the edge, peak reduction in between --
            const int smallD = jlimit (38, 70, (int) ((float) H * 0.34f));
            const int edgeX = section3.getRight() - smallD - (int) ((float) section3.getWidth() * 0.10f);
            hpKnob.setBounds (edgeX, pad + 2, smallD, smallD);
            scKnob.setBounds (edgeX, H - pad - smallD, smallD, smallD);

            const int peakD = jmin ((int) ((float) H * 0.70f), (int) ((float) section3.getWidth() * 0.52f));
            peakKnob.setBounds (section3.getX() + (int) ((float) section3.getWidth() * 0.06f),
                                (int) ((float) H * 0.48f) - peakD / 2, peakD, peakD);
        }

    private:
        void setParam (int index, float v)
        {
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (effect.getParameters()[index]))
                effect.getParameters()[index]->setValueNotifyingHost (rp->convertTo0to1 (v));
            repaint();
        }
        void timerCallback() override
        {
            meter.setGainReduction (effect.gainReductionDb());
            meter.repaint();
            gainKnob.refresh(); peakKnob.refresh(); dryKnob.refresh(); hpKnob.refresh(); scKnob.refresh();
            modeToggle.repaint(); ioToggle.repaint();
        }
        SxOptoCompressor& effect;
        Knob2 gainKnob, peakKnob, dryKnob, hpKnob, scKnob;
        Meter2 meter;
        Flip modeToggle, ioToggle;
        Rectangle<int> section1, section2, section3, bottomStrip, grCaption;
    };
}

AudioProcessorEditor* SxOptoCompressor::createEditor() { return new OptoEditor (*this); }
AudioProcessorEditor* SxFetCompressor::createEditor() { return new FetEditor (*this); }
//== loudness meter editor -------------------------------------------------------------------------
#include "MeterEditors.inc"
//== doubler editor --------------------------------------------------------------------------------
namespace
{
    class DoublerEditor : public AudioProcessorEditor
    {
    public:
        explicit DoublerEditor (SxDoubler& e) : AudioProcessorEditor (&e), fx (e)
        {
            static const char* caps[5] { "Voices", "Spread", "Detune", "Mix", "Level" };
            for (int i = 0; i < 5; ++i)
            {
                auto* k = new Knob (fx, i, kBandColours[5]);
                k->setCaption (caps[i]);
                knobs.add (k);
                addAndMakeVisible (k);
            }
            setResizable (true, false);
            setResizeLimits (420, 200, 1100, 600);
            setSize (520, 260);
        }
        void paint (Graphics& g) override
        {
            g.fillAll (Colour (0xff14161a));
            auto head = getLocalBounds().removeFromTop (32);
            g.setColour (Colour (0xff1d2026));
            g.fillRect (head);
            g.setColour (kBandColours[5]);
            g.setFont (Font (13.0f).boldened());
            g.drawText ("SX DOUBLER", head.reduced (12, 0), Justification::centredLeft);
            g.setColour (kDim);
            g.setFont (Font (10.0f));
            g.drawText (TRANS ("four voices · short delays · slow drift"), head.reduced (12, 0),
                        Justification::centredRight);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (10, 6);
            r.removeFromTop (30);
            const int w = r.getWidth() / jmax (1, knobs.size());
            for (int i = 0; i < knobs.size(); ++i) knobs[i]->setBounds (r.getX() + i * w, r.getY(), w, r.getHeight());
        }
    private:
        SxDoubler& fx;
        OwnedArray<Knob> knobs;
    };
}
AudioProcessorEditor* SxDoubler::createEditor() { return new DoublerEditor (*this); }
// the seven new units use the generic knob editor (its display draws their visualisation)
AudioProcessorEditor* SxTape::createEditor()          { return new SxBigEditor (*this); }
AudioProcessorEditor* SxExciter::createEditor()       { return new SxBigEditor (*this); }
AudioProcessorEditor* SxSaturator::createEditor()     { return new SxBigEditor (*this); }
AudioProcessorEditor* SxMultibandComp::createEditor() { return new SxBigEditor (*this); }
AudioProcessorEditor* SxChorusEnsemble::createEditor(){ return new SxBigEditor (*this); }
AudioProcessorEditor* SxMaximizer::createEditor()     { return new SxBigEditor (*this); }
AudioProcessorEditor* SxTuner::createEditor()         { return new SxBigEditor (*this); }
AudioProcessorEditor* SxClipper::createEditor()       { return new SxBigEditor (*this); }
AudioProcessorEditor* SxSslChannel::createEditor()    { return new SxBigEditor (*this); }
AudioProcessorEditor* SxDeEsser::createEditor()       { return new SxBigEditor (*this); }
AudioProcessorEditor* SxGate::createEditor()          { return new SxBigEditor (*this); }
AudioProcessorEditor* SxDenoiser::createEditor()      { return new SxBigEditor (*this); }
AudioProcessorEditor* SxReverse::createEditor()       { return new SxBigEditor (*this); }
AudioProcessorEditor* SxPitchCorrector::createEditor(){ return new SxBigEditor (*this); }

//== guitar rig editor -----------------------------------------------------------------------------
AudioProcessorEditor* SxEq::createEditor() { return new EqEditor (*this); }
}   // namespace sx