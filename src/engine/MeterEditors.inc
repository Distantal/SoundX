// Loudness and stereo field editors. Included into BuiltInEffectsEditor.cpp at file scope, after the
// shared palette and helpers. Both draw from the meters' histories and sample scope.
namespace
{
    String levelText (float db)
    {
        return db <= -99.0f ? String ("--.-") : String (db, 1);
    }

    class LoudnessEditor : public AudioProcessorEditor, private Timer
    {
    public:
        explicit LoudnessEditor (SxLoudnessMeter& m) : AudioProcessorEditor (&m), meter (m)
        {
            static const char* names[5] { "PEAK", "RMS", "LUFS-M", "LUFS-S", "LUFS-I" };
            for (int i = 0; i < 5; ++i)
            {
                auto* b = new TextButton (names[i]);
                b->setClickingTogglesState (true);
                b->setRadioGroupId (77);
                b->setConnectedEdges (i > 0 ? Button::ConnectedOnLeft : 0);
                b->onClick = [this, i] { mode = i; repaint(); };
                buttons.add (b);
                addAndMakeVisible (b);
            }
            for (auto* b : buttons)
            {
                b->setColour (TextButton::buttonColourId, kPanel2);
                b->setColour (TextButton::buttonOnColourId, kBandColours[0].withAlpha (0.45f));
                b->setColour (TextButton::textColourOffId, kDim);
                b->setColour (TextButton::textColourOnId, Colours::white);
                b->setMouseCursor (MouseCursor::PointingHandCursor);
            }
            buttons[0]->setToggleState (true, dontSendNotification);
            setResizable (true, false);
            setResizeLimits (420, 340, 1400, 900);
            setSize (620, 480);
            startTimerHz (30);
        }

        ~LoudnessEditor() override { stopTimer(); }

        void paint (Graphics& g) override
        {
            g.fillAll (Colour (0xff14161a));
            auto head = getLocalBounds().removeFromTop (34);
            g.setColour (Colour (0xff1d2026));
            g.fillRect (head);
            g.setColour (Colour (0xff5b9cf6));
            g.setFont (Font (FontOptions (13.0f)).boldened());
            g.drawText ("SX LOUDNESS", head.reduced (12, 0), Justification::centredLeft);
            g.setColour (kDim);
            g.setFont (Font (FontOptions (10.5f)));
            g.drawText ("true peak " + levelText (meter.truePeakHoldDb()) + " dBTP", head.reduced (12, 0),
                        Justification::centredRight);

            auto body = getLocalBounds().withTrimmedTop (68).reduced (12, 8);

            // two level bars with the printed dB scale under them
            auto bars = body.removeFromTop (74);
            auto barL = bars.removeFromTop (24);
            bars.removeFromTop (6);
            auto barR = bars.removeFromTop (24);
            drawBar (g, barL, "L", meter.peak (0), meter.rms (0));
            drawBar (g, barR, "R", meter.peak (1), meter.rms (1));
            g.setFont (Font (FontOptions (8.5f)));
            g.setColour (kDim);
            for (int t : { -60, -48, -36, -24, -12, -6, 0 })
            {
                const int x = barX (barL, (float) t);
                g.drawText (String (t), x - 12, bars.getY() + 2, 24, 10, Justification::centred);
            }

            // the number of the selected quantity
            auto number = body.removeFromTop (60);
            g.setColour (kText);
            g.setFont (Font (FontOptions (46.0f)).boldened());
            g.drawText (levelText (currentValue()), number.removeFromLeft (240), Justification::centredLeft);
            g.setColour (kDim);
            g.setFont (Font (FontOptions (12.0f)));
            g.drawText (modeName() + "   " + (mode <= 1 ? "dBFS" : "LUFS"), number.reduced (4, 6), Justification::topRight);

            auto readout = body.removeFromTop (18);
            g.setFont (Font (FontOptions (10.5f)));
            g.setColour (kDim);
            const float peakMax = jmax (Decibels::gainToDecibels (meter.peak (0), -100.0f),
                                        Decibels::gainToDecibels (meter.peak (1), -100.0f));
            g.drawText ("M " + levelText (meter.momentaryLufs()) + "    S " + levelText (meter.shortTermLufs())
                        + "    I " + levelText (meter.integratedLufs()) + "    PK " + levelText (peakMax)
                        + "    TP " + levelText (meter.truePeakHoldDb()),
                        readout, Justification::centredLeft);

            // history: twelve seconds, oldest on the left
            auto frame = body.reduced (0, 6).toFloat();
            g.setColour (Colour (0xff0d0f12));
            g.fillRoundedRectangle (frame, 4.0f);
            g.setColour (Colour (0xff2a2f37));
            g.drawRoundedRectangle (frame, 4.0f, 1.0f);
            auto a = frame.reduced (30.0f, 8.0f);
            auto yFor = [&a] (float db) { return a.getBottom() - jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f) * a.getHeight(); };
            g.setFont (Font (FontOptions (9.0f)));
            for (int t = -60; t <= 0; t += 12)
            {
                g.setColour (Colour (0xff2a2f37));
                g.drawHorizontalLine ((int) yFor ((float) t), a.getX(), a.getRight());
                g.setColour (kDim);
                g.drawText (String (t), (int) frame.getX() + 2, (int) yFor ((float) t) - 6, 24, 12, Justification::right);
            }

            Path p;
            const int N = SxLoudnessMeter::kHistory;
            for (int i = 0; i < N; ++i)
            {
                const float x = a.getX() + a.getWidth() * (float) i / (float) (N - 1);
                const float y = yFor (jlimit (-60.0f, 0.0f, historyValue (i)));
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (Colour (0xff5b9cf6));
            g.strokePath (p, PathStrokeType (1.6f));

            if (mode >= 2)
            {
                const float targetDb = meter.param (1);
                g.setColour (Colour (0xffe6b33c).withAlpha (0.8f));
                g.drawHorizontalLine ((int) yFor (targetDb), a.getX(), a.getRight());
            }
            if (mode == 3 || mode == 4)
            {
                // the short-term or integrated value, as a marker across the history
                const float v = jlimit (-60.0f, 0.0f, currentValue());
                Path line;
                line.startNewSubPath (a.getX(), yFor (v));
                line.lineTo (a.getRight(), yFor (v));
                Path dashed;
                const float dashes[] { 4.0f, 4.0f };
                PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
                g.setColour (Colours::white.withAlpha (0.55f));
                g.strokePath (dashed, PathStrokeType (1.0f));
            }
            g.setColour (kDim);
            g.setFont (Font (FontOptions (9.0f)));
            g.drawText ("-12 s", (int) a.getX(), (int) a.getBottom() + 1, 40, 12, Justification::left);
            g.drawText ("now", (int) a.getRight() - 40, (int) a.getBottom() + 1, 40, 12, Justification::right);
        }

        void resized() override
        {
            auto row = getLocalBounds().withTrimmedTop (34).removeFromTop (26).reduced (10, 0);
            const int w = row.getWidth() / 5;
            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setBounds (row.getX() + i * w, row.getY(), w, row.getHeight());
        }

    private:
        static int barX (Rectangle<int> a, float db)
        {
            return a.getX() + (int) (jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f) * (float) a.getWidth());
        }

        static void drawBar (Graphics& g, Rectangle<int> area, const String& name, float peakLin, float rmsLin)
        {
            const float peakDb = Decibels::gainToDecibels (peakLin, -100.0f);
            const float rmsDb = Decibels::gainToDecibels (rmsLin, -100.0f);
            auto bar = area.withTrimmedLeft (18);
            g.setColour (Colour (0xff1b1f26));
            g.fillRoundedRectangle (bar.toFloat(), 3.0f);
            g.setColour (Colour (0xff2fae92));
            g.fillRect (Rectangle<int> (bar.getX() + 1, bar.getY() + 1, jmax (0, barX (bar, rmsDb) - bar.getX() - 1), bar.getHeight() - 2));
            g.setColour (Colours::white.withAlpha (0.12f));
            for (int t = -60; t <= 0; t += 6)
                g.drawVerticalLine (barX (bar, (float) t), (float) bar.getY(), (float) bar.getBottom());
            g.setColour (peakDb > -1.0f ? Colour (0xffd9534f) : Colours::white.withAlpha (0.9f));
            g.fillRect (Rectangle<int> (barX (bar, peakDb) - 1, bar.getY(), 3, bar.getHeight()));
            g.setColour (Colour (0xff2f353f));
            g.drawRoundedRectangle (bar.toFloat(), 3.0f, 1.0f);
            g.setColour (kDim);
            g.setFont (Font (FontOptions (10.0f)).boldened());
            g.drawText (name, area.withWidth (16), Justification::centred);
        }

        float currentValue() const
        {
            switch (mode)
            {
                case 0: return jmax (Decibels::gainToDecibels (meter.peak (0), -100.0f), Decibels::gainToDecibels (meter.peak (1), -100.0f));
                case 1: return jmax (Decibels::gainToDecibels (meter.rms (0), -100.0f), Decibels::gainToDecibels (meter.rms (1), -100.0f));
                case 2: return meter.momentaryLufs();
                case 3: return meter.shortTermLufs();
                default: return meter.integratedLufs();
            }
        }

        float historyValue (int i) const
        {
            if (mode == 0) return meter.historyPeak (i);
            if (mode == 1) return meter.historyRms (i);
            return meter.historyLufs (i);
        }

        String modeName() const
        {
            static const char* names[5] { "PEAK", "RMS", "MOMENTARY", "SHORT-TERM", "INTEGRATED" };
            return names[jlimit (0, 4, mode)];
        }

        void timerCallback() override
        {
            const auto stamp = meter.displayStamp();
            if (stamp == lastStamp) return;
            lastStamp = stamp;
            repaint();
        }

        SxLoudnessMeter& meter;
        std::uint64_t lastStamp = 0;
        OwnedArray<TextButton> buttons;
        int mode = 0;
    };
}

AudioProcessorEditor* SxLoudnessMeter::createEditor() { return new LoudnessEditor (*this); }

namespace
{
    class FieldEditor : public AudioProcessorEditor, private Timer
    {
    public:
        explicit FieldEditor (SxStereoFieldMeter& m) : AudioProcessorEditor (&m), meter (m)
        {
            static const char* names[4] { "GONIO", "VECTOR", "CORR", "WIDTH" };
            for (int i = 0; i < 4; ++i)
            {
                auto* b = new TextButton (names[i]);
                b->setClickingTogglesState (true);
                b->setRadioGroupId (78);
                b->setConnectedEdges (i > 0 ? Button::ConnectedOnLeft : 0);
                b->onClick = [this, i]
                {
                    if (mode != i) clearTrail = true;
                    mode = i;
                    repaint();
                };
                buttons.add (b);
                addAndMakeVisible (b);
            }
            for (auto* b : buttons)
            {
                b->setColour (TextButton::buttonColourId, kPanel2);
                b->setColour (TextButton::buttonOnColourId, kBandColours[4].withAlpha (0.45f));
                b->setColour (TextButton::textColourOffId, kDim);
                b->setColour (TextButton::textColourOnId, Colours::white);
                b->setMouseCursor (MouseCursor::PointingHandCursor);
            }
            buttons[0]->setToggleState (true, dontSendNotification);
            setResizable (true, false);
            setResizeLimits (380, 320, 1200, 900);
            setSize (560, 460);
            startTimerHz (30);
        }

        ~FieldEditor() override { stopTimer(); }

        void paint (Graphics& g) override
        {
            g.fillAll (Colour (0xff14161a));
            auto head = getLocalBounds().removeFromTop (34);
            g.setColour (Colour (0xff1d2026));
            g.fillRect (head);
            g.setColour (Colour (0xff9a7bf0));
            g.setFont (Font (FontOptions (13.0f)).boldened());
            g.drawText ("SX STEREO FIELD", head.reduced (12, 0), Justification::centredLeft);

            auto body = getLocalBounds().withTrimmedTop (68).reduced (12, 8);
            auto footer = body.removeFromBottom (22);
            g.setColour (kDim);
            g.setFont (Font (FontOptions (10.5f)));
            g.drawText ("corr " + String (meter.correlation(), 2)
                        + "    balance " + String (meter.balance() * 100.0f, 0) + " %"
                        + "    width " + String (meter.widthPercent(), 0) + " %"
                        + "    mid " + levelText (Decibels::gainToDecibels (meter.midLevel(), -100.0f))
                        + "  side " + levelText (Decibels::gainToDecibels (meter.sideLevel(), -100.0f)) + " dB",
                        footer, Justification::centredLeft);

            g.setColour (Colour (0xff0d0f12));
            g.fillRoundedRectangle (body.toFloat(), 5.0f);
            g.setColour (Colour (0xff2a2f37));
            g.drawRoundedRectangle (body.toFloat(), 5.0f, 1.0f);

            const auto area = bodyArea();
            if (mode == 0 || mode == 1)
            {
                if (! trail.isValid() || clearTrail) updateTrail();
                if (trail.isValid()) g.drawImageAt (trail, area.getX(), area.getY());
                paintPolarGrid (g, area);
            }
            else
                paintHistory (g, area, mode == 3);
        }

        void resized() override
        {
            auto row = getLocalBounds().withTrimmedTop (34).removeFromTop (26).reduced (10, 0);
            const int w = row.getWidth() / 4;
            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setBounds (row.getX() + i * w, row.getY(), w, row.getHeight());
        }

    private:
        Rectangle<int> bodyArea() const
        {
            auto body = getLocalBounds().withTrimmedTop (68).reduced (12, 8);
            body.removeFromBottom (22);
            return body.reduced (1);
        }

        float zoom() const { return jlimit (0.5f, 2.0f, meter.param (1) / 100.0f); }

        // The goniometer keeps a short afterglow: each frame the old pixels fade a little toward the
        // background, then the newest sample pairs are drawn on top.
        void updateTrail()
        {
            const auto area = bodyArea();
            if (area.isEmpty()) return;
            if (! trail.isValid() || trail.getWidth() != area.getWidth() || trail.getHeight() != area.getHeight() || clearTrail)
            {
                trail = Image (Image::RGB, area.getWidth(), area.getHeight(), true);
                trail.clear (trail.getBounds(), Colour (0xff0d0f12));
                clearTrail = false;
            }

            std::array<std::pair<float, float>, SxStereoFieldMeter::kScope> pairs;
            meter.copyScope (pairs);

            Graphics g (trail);
            g.setColour (Colour (0xff0d0f12).withAlpha (0.25f));
            g.fillAll();

            const float w = (float) area.getWidth(), h = (float) area.getHeight();
            const float radius = jmin (w, h) * 0.46f * zoom();
            const Point<float> c (w * 0.5f, h * 0.5f);
            g.setColour (Colour (0xff9a7bf0).withAlpha (0.6f));
            for (const auto& p : pairs)
            {
                float x, y;
                if (mode == 0) { x = (p.second - p.first) * 0.7071f; y = (p.first + p.second) * 0.7071f; }
                else           { x = p.first; y = p.second; }
                g.fillRect (Rectangle<float> (1.5f, 1.5f).withCentre ({ c.x + x * radius, c.y - y * radius }));
            }
        }

        void paintPolarGrid (Graphics& g, Rectangle<int> area)
        {
            const auto c = area.toFloat().getCentre();
            const float radius = jmin (area.getWidth(), area.getHeight()) * 0.46f * zoom();
            g.setColour (Colour (0xff2a2f37));
            g.drawEllipse (c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f, 1.0f);
            g.drawLine (c.x, c.y - radius, c.x, c.y + radius, 1.0f);
            g.drawLine (c.x - radius, c.y, c.x + radius, c.y, 1.0f);
            const float diag = radius * 0.7071f;
            g.setColour (Colour (0xff232830));
            g.drawLine (c.x - diag, c.y - diag, c.x + diag, c.y + diag, 1.0f);
            g.drawLine (c.x - diag, c.y + diag, c.x + diag, c.y - diag, 1.0f);

            g.setColour (kDim);
            g.setFont (Font (FontOptions (10.0f)).boldened());
            if (mode == 0)
            {
                g.drawText ("L", (int) (c.x - diag - 18), (int) (c.y - diag - 14), 14, 12, Justification::centred);
                g.drawText ("R", (int) (c.x + diag + 4), (int) (c.y - diag - 14), 14, 12, Justification::centred);
                g.drawText ("M", (int) c.x - 7, (int) (c.y - radius - 14), 14, 12, Justification::centred);
                g.drawText ("S", (int) (c.x + radius + 4), (int) c.y - 6, 14, 12, Justification::centred);
            }
            else
            {
                g.drawText ("L", (int) (c.x + radius + 4), (int) c.y - 6, 14, 12, Justification::centred);
                g.drawText ("R", (int) c.x - 7, (int) (c.y - radius - 14), 14, 12, Justification::centred);
            }
        }

        void paintHistory (Graphics& g, Rectangle<int> area, bool widthMode)
        {
            auto a = area.toFloat().reduced (34.0f, 16.0f);
            const float lo = widthMode ? 0.0f : -1.0f, hi = widthMode ? 200.0f : 1.0f;
            auto yFor = [&] (float v) { return a.getBottom() - jlimit (0.0f, 1.0f, (v - lo) / (hi - lo)) * a.getHeight(); };

            g.setFont (Font (FontOptions (9.0f)));
            for (int k = 0; k < 3; ++k)
            {
                const float v = widthMode ? 100.0f * (float) k : 1.0f - (float) k;
                g.setColour (Colour (0xff2a2f37));
                g.drawHorizontalLine ((int) yFor (v), a.getX(), a.getRight());
                g.setColour (kDim);
                g.drawText (widthMode ? String ((int) v) + " %" : String (v, 0), (int) area.getX() + 2,
                            (int) yFor (v) - 6, 30, 12, Justification::left);
            }

            Path p;
            const int N = SxStereoFieldMeter::kHistory;
            for (int i = 0; i < N; ++i)
            {
                const float v = widthMode ? meter.widthHistory (i) : meter.correlationHistory (i);
                const float x = a.getX() + a.getWidth() * (float) i / (float) (N - 1);
                const float y = yFor (v);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (Colour (0xff39c5a8));
            g.strokePath (p, PathStrokeType (1.6f));
            g.setColour (kDim);
            g.setFont (Font (FontOptions (9.0f)));
            g.drawText ("-12 s", (int) a.getX(), (int) a.getBottom() + 2, 40, 12, Justification::left);
            g.drawText ("now", (int) a.getRight() - 40, (int) a.getBottom() + 2, 40, 12, Justification::right);
        }

        void timerCallback() override
        {
            const auto stamp = meter.displayStamp();
            if (stamp == lastStamp) return;
            lastStamp = stamp;
            if (mode == 0 || mode == 1) updateTrail();
            repaint();
        }

        SxStereoFieldMeter& meter;
        std::uint64_t lastStamp = 0;
        OwnedArray<TextButton> buttons;
        Image trail;
        bool clearTrail = true;
        int mode = 0;
    };
}

AudioProcessorEditor* SxStereoFieldMeter::createEditor() { return new FieldEditor (*this); }
