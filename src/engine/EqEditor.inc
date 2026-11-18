// SX EQ editor. Included into BuiltInEffectsEditor.cpp inside its anonymous namespace, so it can use
// the shared Knob class and palette defined there.
//
// The graph is drawn from cached paths: the response curves are rebuilt only when a setting or the
// graph size changes, the spectrum a few times a second. Dragging a node only updates a pending
// position, which the timer applies once per frame, so a fast mouse never floods the processor.
class EqEditor : public AudioProcessorEditor, private Timer
{
public:
    explicit EqEditor (SxEq& e) : AudioProcessorEditor (&e), eq (e)
    {
        static const char* captions[5] { "Freq", "Gain", "Q", "Thr", "Range" };
        for (int i = 0; i < 5; ++i)
        {
            auto* k = new Knob (eq, SxEq::pFreq (0), kBandColours[0]);
            k->setCaption (captions[i]);
            knobs.add (k);
            addAndMakeVisible (k);
        }
        dynBtn.setButtonText ("DYN");
        dynBtn.setClickingTogglesState (true);
        dynBtn.onClick = [this]
        {
            const int b = sel();
            setParam (SxEq::pDynOn (b), eq.param (SxEq::pDynOn (b)) > 0.5f ? 0.0f : 1.0f);
            repaint();
        };
        addAndMakeVisible (dynBtn);

        shapeBtn.onClick = [this] { showShapeMenu (sel(), shapeBtn.getScreenPosition()); };
        addAndMakeVisible (shapeBtn);

        slopeBtn.onClick = [this] { showSlopeMenu (sel(), slopeBtn.getScreenPosition()); };
        addAndMakeVisible (slopeBtn);

        addBtn.setButtonText ("+");
        addBtn.setTooltip (TRANS ("Add a band in the middle of the graph"));
        addBtn.onClick = [this] { addBandAt (innerArea().getCentreX(), innerArea().getCentreY()); };
        addAndMakeVisible (addBtn);

        soloBtn.setButtonText ("S");
        soloBtn.setTooltip (TRANS ("Solo this band: monitor only what the band does"));
        soloBtn.setColour (TextButton::buttonOnColourId, kBandColours[2].withAlpha (0.35f));
        soloBtn.onClick = [this]
        {
            const int b = sel();
            const bool on = eq.param (SxEq::pSolo()) > 0.5f && (int) std::round (eq.param (SxEq::pSolo())) == b + 1;
            setParam (SxEq::pSolo(), on ? 0.0f : (float) (b + 1));
            repaint();
        };
        addAndMakeVisible (soloBtn);

        grabBtn.setButtonText (TRANS ("Grab"));
        grabBtn.setTooltip (TRANS ("Freeze the analyser and click a peak to place a band there"));
        grabBtn.setClickingTogglesState (true);
        grabBtn.setColour (TextButton::buttonOnColourId, kBandColours[4].withAlpha (0.35f));
        grabBtn.onClick = [this]
        {
            grabFrozen = grabBtn.getToggleState();
            if (grabFrozen)
                for (int i = 0; i < SxEffectBase::kSpectrumBins; ++i) frozenSpec[i] = eq.spectrumAt (i);
            repaint();
        };
        addAndMakeVisible (grabBtn);

        presetBtn.setButtonText (TRANS ("PRESET"));
        presetBtn.setTooltip (TRANS ("Factory and user EQ presets"));
        presetBtn.onClick = [this] { showPresetMenu(); };
        addAndMakeVisible (presetBtn);

        static const char* analyserNames[3] { "PRE", "POST", "OFF" };
        static const char* analyserTips[3] { "Analyse the signal before the EQ", "Analyse the signal after the EQ",
                                             "Switch the analyser off" };
        for (int i = 0; i < 3; ++i)
        {
            analyserBtn[i].setButtonText (analyserNames[i]);
            analyserBtn[i].setTooltip (TRANS (analyserTips[i]));
            analyserBtn[i].onClick = [this, i]
            {
                setParam (SxEq::pAnalyser(), (float) i);
                repaint();
            };
            addAndMakeVisible (analyserBtn[i]);
        }

        for (auto* b : { &shapeBtn, &slopeBtn, &dynBtn, &addBtn, &soloBtn, &grabBtn, &presetBtn,
                         &analyserBtn[0], &analyserBtn[1], &analyserBtn[2] })
        {
            b->setColour (TextButton::buttonColourId, kPanel2);
            b->setColour (TextButton::buttonOnColourId, kBandColours[0].withAlpha (0.30f));
            b->setColour (TextButton::textColourOffId, kText);
            b->setColour (TextButton::textColourOnId, Colours::white);
            b->setMouseCursor (MouseCursor::PointingHandCursor);
        }

        soloBtn.setColour (TextButton::buttonOnColourId, kBandColours[2].withAlpha (0.35f));
        grabBtn.setColour (TextButton::buttonOnColourId, kBandColours[4].withAlpha (0.35f));
        setWantsKeyboardFocus (true);
        setResizable (true, false);   // the window provides the only resize grip
        setResizeLimits (620, 380, 1800, 1000);
        setSize (880, 520);
        setKnobsToBand();
        startTimerHz (60);
    }

    ~EqEditor() override { stopTimer(); }

    //== presets ---------------------------------------------------------------------------------
    struct PresetBand { float on, type, freq, gain, q; };
    struct Preset { const char* name; PresetBand bands[8]; };

    static const std::vector<Preset>& factoryPresets()
    {
        static const std::vector<Preset> list
        {
            { "Flat", { { 1, 0, 80, 0, 0.7f }, { 1, 0, 300, 0, 1 }, { 1, 0, 1500, 0, 1 }, { 1, 2, 8000, 0, 0.7f },
                        { 0, 0, 500, 0, 1 }, { 0, 0, 2500, 0, 1 }, { 0, 0, 60, 0, 0.7f }, { 0, 0, 12000, 0, 0.7f } } },
            { "Vocal Presence", { { 1, 3, 90, 0, 0.7f }, { 1, 0, 300, -2.5f, 1.2f }, { 1, 0, 3200, 3.5f, 1.1f },
                                  { 1, 2, 9000, 2.5f, 0.7f }, { 1, 0, 800, 1.5f, 1.4f }, { 1, 5, 1200, 0, 2.5f },
                                  { 0, 0, 60, 0, 0.7f }, { 0, 0, 12000, 0, 0.7f } } },
            { "Bass Boost", { { 1, 1, 90, 4.5f, 0.7f }, { 1, 0, 45, 3.0f, 1.0f }, { 1, 3, 32, 0, 0.7f },
                              { 1, 0, 900, -1.5f, 1.2f }, { 0, 0, 500, 0, 1 }, { 0, 0, 2500, 0, 1 },
                              { 0, 0, 60, 0, 0.7f }, { 0, 0, 12000, 0, 0.7f } } },
            { "Air", { { 1, 3, 70, 0, 0.7f }, { 1, 2, 12000, 3.0f, 0.7f }, { 1, 2, 16000, 2.0f, 0.6f },
                       { 1, 0, 6000, -1.0f, 1.0f }, { 0, 0, 500, 0, 1 }, { 0, 0, 2500, 0, 1 },
                       { 0, 0, 60, 0, 0.7f }, { 0, 0, 12000, 0, 0.7f } } },
            { "Telephone", { { 1, 3, 320, 0, 1.2f }, { 1, 4, 3200, 0, 1.0f }, { 1, 0, 1200, 3.0f, 1.2f },
                             { 0, 0, 8000, 0, 0.7f }, { 0, 0, 500, 0, 1 }, { 0, 0, 2500, 0, 1 },
                             { 0, 0, 60, 0, 0.7f }, { 0, 0, 12000, 0, 0.7f } } },
            { "Master Glue", { { 1, 0, 40, 1.5f, 0.6f }, { 1, 0, 120, -1.0f, 1.0f }, { 1, 0, 2600, 1.0f, 1.2f },
                               { 1, 0, 9000, 1.5f, 0.8f }, { 1, 3, 24, 0, 0.7f }, { 0, 0, 2500, 0, 1 },
                               { 0, 0, 60, 0, 0.7f }, { 0, 0, 12000, 0, 0.7f } } },
            { "Recessed Mids", { { 1, 0, 700, -3.5f, 1.0f }, { 1, 0, 1500, -2.5f, 1.2f }, { 1, 1, 100, 1.5f, 0.7f },
                                 { 1, 2, 10000, 2.0f, 0.7f }, { 0, 0, 500, 0, 1 }, { 0, 0, 2500, 0, 1 },
                                 { 0, 0, 60, 0, 0.7f }, { 0, 0, 12000, 0, 0.7f } } },
            { "Mud Cut", { { 1, 3, 50, 0, 0.7f }, { 1, 0, 250, -4.0f, 1.1f }, { 1, 0, 420, -2.5f, 1.3f },
                           { 1, 0, 3000, 1.0f, 1.0f }, { 0, 0, 500, 0, 1 }, { 0, 0, 2500, 0, 1 },
                           { 0, 0, 60, 0, 0.7f }, { 0, 0, 12000, 0, 0.7f } } },
        };
        return list;
    }

    static File userPresetFolder()
    {
        return File::getSpecialLocation (File::userApplicationDataDirectory)
                   .getChildFile ("SoundX").getChildFile ("EQ Presets");
    }

    void applyPreset (const Preset& pr)
    {
        for (int b = 0; b < SxEq::kNumBands; ++b)
        {
            const auto& pb = pr.bands[b];
            setParam (SxEq::pOn (b), pb.on);
            setParam (SxEq::pType (b), pb.type);
            setParam (SxEq::pFreq (b), pb.freq);
            setParam (SxEq::pGain (b), pb.gain);
            setParam (SxEq::pQ (b), pb.q);
            setParam (SxEq::pSlope (b), 0.0f);
            setParam (SxEq::pDynOn (b), 0.0f);
        }
        setParam (SxEq::pSolo(), 0.0f);
        setKnobsToBand();
        repaint();
    }

    void applyPresetFile (const File& f)
    {
        if (eq.loadPreset (f))
        {
            setKnobsToBand();
            repaint();
        }
    }

    void savePreset()
    {
        auto* aw = new AlertWindow (TRANS ("Save EQ Preset"), TRANS ("Name for this preset:"), MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", "My EQ", "");
        aw->addButton (TRANS ("Save"), 1, KeyPress (KeyPress::returnKey));
        aw->addButton (TRANS ("Cancel"), 0, KeyPress (KeyPress::escapeKey));
        auto* self = this;
        aw->enterModalState (true, ModalCallbackFunction::create ([self, aw] (int r)
        {
            if (r == 1)
            {
                const String name = aw->getTextEditorContents ("name").trim();
                if (name.isNotEmpty()) self->writePresetFile (name);
            }
        }), true);
    }

    void writePresetFile (const String& name)
    {
        auto folder = userPresetFolder();
        folder.createDirectory();
        eq.savePreset (folder.getChildFile (File::createLegalFileName (name) + ".sxeq"));
    }

    void showPresetMenu()
    {
        PopupMenu m;
        const auto& factory = factoryPresets();
        PopupMenu fmenu;
        for (int i = 0; i < (int) factory.size(); ++i) fmenu.addItem (100 + i, factory[(size_t) i].name);
        m.addSubMenu (TRANS ("Factory Presets"), fmenu);

        PopupMenu umenu;
        auto folder = userPresetFolder();
        if (folder.isDirectory())
        {
            auto files = folder.findChildFiles (File::findFiles, false, "*.sxeq");
            files.sort();
            for (int i = 0; i < files.size(); ++i) umenu.addItem (1000 + i, files[i].getFileNameWithoutExtension());
        }
        m.addSubMenu (TRANS ("User Presets"), umenu, umenu.getNumItems() > 0);
        m.addSeparator();
        m.addItem (1, TRANS ("Save Current Preset..."));
        auto* self = this;
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (&presetBtn), [self] (int r)
        {
            if (r == 1) { self->savePreset(); return; }
            if (r >= 100 && r < 100 + (int) factoryPresets().size())
            {
                self->applyPreset (factoryPresets()[(size_t) (r - 100)]);
                return;
            }
            if (r >= 1000)
            {
                auto files = userPresetFolder().findChildFiles (File::findFiles, false, "*.sxeq");
                files.sort();
                const int i = r - 1000;
                if (i >= 0 && i < files.size()) self->applyPresetFile (files[i]);
            }
        });
    }

    //== painting ----------------------------------------------------------------------------------
    void paint (Graphics& g) override
    {
        g.fillAll (kBg);
        auto head = getLocalBounds().removeFromTop (44);
        g.setColour (kPanel);
        g.fillRect (head);
        g.setColour (kBandColours[0]);
        g.fillRect (head.getX(), head.getY(), 3, head.getHeight());
        g.setColour (kText);
        g.setFont (Font (FontOptions (16.0f)).boldened());
        g.drawText ("SX EQ", head.reduced (16, 0), Justification::centredLeft);

        // the side-chain badge: this EQ is listening to another track
        if (eq.hasSidechain())
        {
            auto chip = Rectangle<int> (head.getRight() - 108, head.getY() + 13, 28, 18);
            g.setColour (kBandColours[2].withAlpha (0.30f));
            g.fillRoundedRectangle (chip.toFloat(), 3.0f);
            g.setColour (kBandColours[2]);
            g.setFont (Font (FontOptions (10.0f)).boldened());
            g.drawFittedText ("SC", chip, Justification::centred, 1);
        }
        g.setColour (kDim);
        g.setFont (Font (FontOptions (11.0f)));
        const int s = sel();
        const int t = (int) std::round (eq.param (SxEq::pType (s)));
        const bool dyn = eq.param (SxEq::pDynOn (s)) > 0.5f;
        g.drawFittedText (String ("Band ") + String (s + 1) + "  -  " + SxEq::typeName (t)
                          + (dyn ? "  -  Dynamic " + String (eq.bandDynDb (s), 1) + " dB" : "")
                          + "  -  drag a node, Shift for fine, wheel for Q, double-click to add; chip switch = band on/off",
                          head.reduced (16, 0), Justification::centredRight, 1, 0.75f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (44);
        auto bottom = r.removeFromBottom (146).reduced (8, 3);
        chipsArea = r.removeFromTop (22).reduced (2, 0);
        graphArea = r.reduced (8, 4);

        auto right = bottom.removeFromRight (168).reduced (4, 4);
        presetBtn.setBounds (right.removeFromTop (22));
        right.removeFromTop (3);
        auto row1 = right.removeFromTop (22);
        shapeBtn.setBounds (row1.removeFromLeft (row1.getWidth() / 2 - 2));
        row1.removeFromLeft (4);
        slopeBtn.setBounds (row1);
        right.removeFromTop (3);
        auto row2 = right.removeFromTop (22);
        dynBtn.setBounds (row2.removeFromLeft (row2.getWidth() - 30));
        row2.removeFromLeft (3);
        addBtn.setBounds (row2);
        right.removeFromTop (3);
        auto row3 = right.removeFromTop (22);
        soloBtn.setBounds (row3.removeFromLeft (30));
        row3.removeFromLeft (3);
        grabBtn.setBounds (row3);
        right.removeFromTop (3);
        auto row4 = right.removeFromTop (22);
        for (int i = 0; i < 3; ++i)
            analyserBtn[i].setBounds (row4.removeFromLeft (row4.getWidth() / (3 - i)));

        const auto left = bottom;
        const int w = jmax (52, left.getWidth() / 5);
        for (int i = 0; i < knobs.size(); ++i)
            knobs[i]->setBounds (left.getX() + i * w, left.getY(), w, left.getHeight());

        curveArea = {};   // the graph changed size: the paths are rebuilt
    }

    void paintOverChildren (Graphics& g) override
    {
        refreshCaches();
        if (spectrumLine.isEmpty() && (grabFrozen || eq.param (SxEq::pAnalyser()) < 1.5f))
            rebuildSpectrum (innerArea());
        paintGraph (g, graphArea.toFloat());
        paintChips (g);
    }

    void paintGraph (Graphics& g, Rectangle<float> a)
    {
        const auto inner = a.withTrimmedLeft (dBW).withTrimmedTop (freqH);
        g.setColour (Colour (0xff101317));
        g.fillRoundedRectangle (a, 6.0f);

        // ruler strips
        g.setColour (Colour (0xff181c22));
        g.fillRect (Rectangle<float> (a.getX(), a.getY(), a.getWidth(), freqH));
        g.fillRect (Rectangle<float> (a.getX(), a.getY(), dBW, a.getHeight()));
        g.setColour (kGrid.withAlpha (0.5f));
        g.drawVerticalLine ((int) inner.getX(), a.getY(), a.getBottom());
        g.drawHorizontalLine ((int) inner.getY(), a.getX(), a.getRight());

        // frequency grid and labels
        static const float marks[] { 20, 30, 40, 50, 70, 100, 200, 300, 500, 700, 1000, 2000, 3000,
                                     5000, 7000, 10000, 20000 };
        g.setFont (Font (FontOptions (9.5f)));
        for (float hz : marks)
        {
            const float x = freqToX (inner, hz);
            const bool major = hz == 100 || hz == 1000 || hz == 10000 || hz == 20 || hz == 20000;
            g.setColour (kGrid.withAlpha (major ? 0.75f : 0.35f));
            g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom());
            g.setColour (kGrid.withAlpha (major ? 0.9f : 0.55f));
            g.drawVerticalLine ((int) x, a.getY(), a.getY() + (major ? 7.0f : 4.0f));
            if (major)
            {
                const String label = hz >= 1000.0f
                    ? String (hz / 1000.0f, std::fmod (hz, 1000.0f) < 1.0f ? 0 : 1) + "k"
                    : String ((int) hz);
                g.setColour (kDim);
                g.drawText (label, (int) x - 16, (int) a.getY() + 8, 32, 12, Justification::centred);
            }
        }

        // dB grid
        for (float db : { -24.0f, -18.0f, -12.0f, -6.0f, 6.0f, 12.0f, 18.0f, 24.0f })
        {
            const float y = dbToY (inner, db);
            g.setColour (kGrid.withAlpha (0.3f));
            g.drawHorizontalLine ((int) y, inner.getX(), inner.getRight());
            g.setColour (kDim);
            g.drawText (String ((int) db), 4, (int) y - 6, (int) dBW - 8, 12, Justification::right);
        }
        g.setColour (kGrid.withAlpha (0.9f));
        g.drawHorizontalLine ((int) dbToY (inner, 0.0f), inner.getX(), inner.getRight());
        g.setColour (kDim);
        g.drawText ("0", 4, (int) dbToY (inner, 0.0f) - 6, (int) dBW - 8, 12, Justification::right);

        // analyser: a smooth filled curve, drawn from the cached path
        if (eq.param (SxEq::pAnalyser()) < 1.5f || grabFrozen)
        {
            g.setGradientFill (ColourGradient (Colour (0xff5d7fa6).withAlpha (0.40f), 0.0f, inner.getY(),
                                               Colour (0xff5d7fa6).withAlpha (0.03f), 0.0f, inner.getBottom(), false));
            g.fillPath (spectrumFill);
            g.setColour (Colour (0xff9fb6cf).withAlpha (0.55f));
            g.strokePath (spectrumLine, PathStrokeType (1.1f));
        }

        // every active band on its own, thin and in its colour; the selected one is brighter
        for (int b = 0; b < SxEq::kNumBands; ++b)
        {
            if (eq.param (SxEq::pOn (b)) < 0.5f) continue;
            const bool hot = b == sel() || b == hoverBand;
            g.setColour (kBandColours[b % 8].withAlpha (hot ? 0.85f : 0.30f));
            g.strokePath (bandPaths[b], PathStrokeType (hot ? 1.4f : 1.0f));
        }

        // the total response: a light fill to 0 dB, a soft glow and a sharp line
        g.setGradientFill (ColourGradient (Colour (0xff5b9cf6).withAlpha (0.20f), 0.0f, inner.getY(),
                                           Colour (0xff5b9cf6).withAlpha (0.03f), 0.0f, inner.getBottom(), false));
        g.fillPath (curveFill);
        g.setColour (Colour (0xff5b9cf6).withAlpha (0.16f));
        g.strokePath (curvePath, PathStrokeType (5.0f));
        g.setColour (Colour (0xff8fc0ff));
        g.strokePath (curvePath, PathStrokeType (1.8f));

        // the curve including the current dynamic reductions: dashed, so the movement is visible
        if (dynShown)
        {
            Path dashed;
            const float dashes[] { 5.0f, 4.0f };
            PathStrokeType (1.2f).createDashedStroke (dashed, dynPath, dashes, 2);
            g.setColour (Colours::white.withAlpha (0.7f));
            g.strokePath (dashed, PathStrokeType (1.0f));
        }

        // grabbed peaks: little triangles so a preset can be dialled in peak by peak
        if (grabFrozen)
        {
            for (int k = 0; k < numPeaks; ++k)
            {
                const int i = peaks[k];
                const float x = freqToX (inner, binHz (i));
                const float y = inner.getBottom()
                    - jlimit (0.0f, 1.0f, (Decibels::gainToDecibels (frozenSpec[i], -90.0f) + 90.0f) / 90.0f) * inner.getHeight();
                Path tri;
                tri.addTriangle (x - 4.0f, y - 8.0f, x + 4.0f, y - 8.0f, x, y - 2.0f);
                g.setColour (kBandColours[6].withAlpha (0.9f));
                g.fillPath (tri);
            }
        }

        // band nodes
        for (int b = 0; b < SxEq::kNumBands; ++b)
        {
            if (eq.param (SxEq::pOn (b)) < 0.5f) continue;
            const float x = freqToX (inner, eq.param (SxEq::pFreq (b)));
            const float y = dbToY (inner, jlimit (-26.0f, 26.0f, eq.param (SxEq::pGain (b))));
            const bool sel1 = b == sel();
            const bool dyn = eq.param (SxEq::pDynOn (b)) > 0.5f;
            const float r = sel1 ? 10.0f : 8.0f;
            if (dyn)
            {
                g.setColour (kBandColours[b % 8].withAlpha (0.9f));
                for (int k = 0; k < 8; ++k)
                {
                    const float ang = (float) k / 8.0f * MathConstants<float>::twoPi;
                    const float cx = x + std::cos (ang) * (r + 3.5f), cy = y + std::sin (ang) * (r + 3.5f);
                    g.fillEllipse (cx - 1.1f, cy - 1.1f, 2.2f, 2.2f);
                }
                // the dynamic ring: how much the band is being moved right now
                const float red = eq.bandDynDb (b);
                const float range = jmax (1.0f, eq.param (SxEq::pRange (b)));
                if (red > 0.05f)
                {
                    const float sweep = MathConstants<float>::twoPi * jlimit (0.0f, 1.0f, red / range);
                    Path ring;
                    ring.addCentredArc (x, y, r + 5.0f, r + 5.0f, 0.0f,
                                        MathConstants<float>::pi * 1.5f, MathConstants<float>::pi * 1.5f + sweep, true);
                    g.setColour (Colours::white.withAlpha (0.9f));
                    g.strokePath (ring, PathStrokeType (2.2f, PathStrokeType::curved, PathStrokeType::butt));
                }
            }
            if (b == hoverBand && ! sel1)
            {
                g.setColour (Colours::white.withAlpha (0.35f));
                g.drawEllipse (x - r - 3.0f, y - r - 3.0f, (r + 3.0f) * 2.0f, (r + 3.0f) * 2.0f, 1.0f);
            }
            g.setColour (kBandColours[b % 8]);
            g.fillEllipse (x - r, y - r, r * 2.0f, r * 2.0f);
            g.setColour (sel1 ? Colours::white : Colour (0xcc0e1015));
            g.drawEllipse (x - r, y - r, r * 2.0f, r * 2.0f, sel1 ? 2.0f : 1.2f);
            g.setColour (Colour (0xff10131a));
            g.setFont (Font (FontOptions (10.0f)).boldened());
            g.drawText (String (b + 1), (int) (x - r), (int) (y - r), (int) (r * 2), (int) (r * 2), Justification::centred);
        }
    }

    void paintChips (Graphics& g)
    {
        const int n = SxEq::kNumBands;
        for (int b = 0; b < n; ++b)
        {
            auto r = chipBounds (b);
            const bool on = eq.param (SxEq::pOn (b)) > 0.5f;
            const bool selChip = b == sel();
            g.setColour (on ? kBandColours[b].withAlpha (selChip ? 0.85f : 0.45f)
                            : kPanel2.withAlpha (0.6f));
            g.fillRoundedRectangle (r.toFloat(), 3.0f);
            if (selChip)
            {
                g.setColour (Colours::white.withAlpha (0.9f));
                g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.2f);
            }

            // independent on / off switch for this frequency point: a small pill on the left
            // edge of the chip; clicking the rest of the chip only selects the band for the knobs
            const auto sw = chipSwitchBounds (b);
            const float trackH = (float) sw.getHeight();
            g.setColour (on ? Colours::white.withAlpha (0.88f) : Colour (0xff101318));
            g.fillRoundedRectangle (sw.toFloat(), trackH * 0.5f);
            const float kr = trackH * 0.38f;
            const float kx = on ? (float) sw.getRight() - kr - 1.5f
                                : (float) sw.getX() + kr + 1.5f;
            g.setColour (on ? kBandColours[b].darker (0.35f) : kDim.withAlpha (0.75f));
            g.fillEllipse (kx - kr, (float) sw.getCentreY() - kr, kr * 2.0f, kr * 2.0f);

            g.setColour (on ? Colours::white : kDim.withAlpha (0.7f));
            g.setFont (Font (FontOptions (9.5f)).boldened());
            const float hz = eq.param (SxEq::pFreq (b));
            const String label = String (b + 1) + "  " + (hz >= 1000.0f ? String (hz / 1000.0f, 1) + "k"
                                                                       : String ((int) hz) + "Hz");
            g.drawFittedText (label, r.withTrimmedLeft (22), Justification::centred, 1, 0.8f);
        }
    }

    //== timer: applies the pending drag, rebuilds what changed, repaints the graph -------------
    void timerCallback() override
    {
        applyDrag();
        const bool curvesChanged = refreshCaches();
        const bool dyn = dynShown;

        ++tick;
        const bool spectrumOn = grabFrozen || eq.param (SxEq::pAnalyser()) < 1.5f;
        const bool spectrumDue = spectrumOn && (tick % 2 == 0 || grabFrozen);
        if (spectrumDue) rebuildSpectrum (innerArea());

        if (curvesChanged) repaint();
        else if (spectrumDue || dragBand >= 0 || dyn) repaint (graphArea);

        if (eq.param (SxEq::pDynOn (sel())) > 0.5f)
            dynBtn.setButtonText ("DYN " + String (eq.bandDynDb (sel()), 1));
        else
            dynBtn.setButtonText ("DYN");
    }

private:
    /** Rebuilds the response paths when a setting or the graph size changed. Returns true if it did. */
    bool refreshCaches()
    {
        const auto inner = innerArea();
        bool changed = false;
        std::array<float, 96> now {};
        const int np = jmin ((int) now.size(), eq.numParams());
        for (int i = 0; i < np; ++i) now[(size_t) i] = eq.param (i);
        if (now != lastSnap || inner != curveArea)
        {
            lastSnap = now;
            curveArea = inner;
            rebuildCurves (inner);
            changed = true;
        }
        const bool dyn = anyDynamicReduction();
        if (dyn || dynShown)
        {
            rebuildDyn (inner);
            dynShown = dyn;
            changed = true;
        }
        return changed;
    }

    void rebuildCurves (Rectangle<float> inner)
    {
        const int N = jlimit (96, 1024, (int) inner.getWidth());
        xs.resize ((size_t) N);
        hzs.resize ((size_t) N);
        dbs.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            xs[(size_t) i] = inner.getX() + inner.getWidth() * (float) i / (float) (N - 1);
            hzs[(size_t) i] = xToFreq (inner, xs[(size_t) i]);
        }

        eq.responseDb (-1, hzs.data(), N, dbs.data(), false);
        curvePath = pathFor (inner, N);
        curveFill = curvePath;
        curveFill.lineTo (inner.getRight(), dbToY (inner, 0.0f));
        curveFill.lineTo (inner.getX(), dbToY (inner, 0.0f));
        curveFill.closeSubPath();

        for (int b = 0; b < SxEq::kNumBands; ++b)
        {
            if (eq.param (SxEq::pOn (b)) < 0.5f) { bandPaths[b].clear(); continue; }
            eq.responseDb (b, hzs.data(), N, dbs.data(), false);
            bandPaths[b] = pathFor (inner, N);
        }
    }

    void rebuildDyn (Rectangle<float> inner)
    {
        const int N = (int) hzs.size();
        if (N < 2) return;
        eq.responseDb (-1, hzs.data(), N, dbs.data(), true);
        dynPath = pathFor (inner, N);
    }

    Path pathFor (Rectangle<float> inner, int N)
    {
        Path p;
        for (int i = 0; i < N; ++i)
        {
            const float y = dbToY (inner, jlimit (-26.0f, 26.0f, dbs[(size_t) i]));
            if (i == 0) p.startNewSubPath (xs[(size_t) i], y); else p.lineTo (xs[(size_t) i], y);
        }
        return p;
    }

    void rebuildSpectrum (Rectangle<float> inner)
    {
        constexpr int N = SxEffectBase::kSpectrumBins;
        spectrumLine.clear();
        for (int i = 0; i < N; ++i)
        {
            const float amp = grabFrozen ? frozenSpec[i] : eq.spectrumAt (i);
            const float db = Decibels::gainToDecibels (amp, -90.0f);
            const float y = inner.getBottom() - jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f) * inner.getHeight();
            const float x = freqToX (inner, binHz (i));
            if (i == 0) spectrumLine.startNewSubPath (x, y); else spectrumLine.lineTo (x, y);
        }
        spectrumFill = spectrumLine;
        spectrumFill.lineTo (inner.getRight(), inner.getBottom());
        spectrumFill.lineTo (inner.getX(), inner.getBottom());
        spectrumFill.closeSubPath();

        numPeaks = 0;
        if (grabFrozen)
        {
            for (int i = 2; i < N - 2 && numPeaks < 32; ++i)
            {
                const float v = frozenSpec[i];
                if (v <= frozenSpec[i - 1] || v < frozenSpec[i + 1]) continue;
                if (Decibels::gainToDecibels (v, -90.0f) < -66.0f) continue;     // ignore the noise floor
                peaks[numPeaks++] = i;
            }
        }
    }

    bool anyDynamicReduction() const
    {
        for (int b = 0; b < SxEq::kNumBands; ++b)
            if (eq.param (SxEq::pDynOn (b)) > 0.5f && eq.bandDynDb (b) > 0.05f) return true;
        return false;
    }

    /** Applies the node position the mouse asked for; called once per timer tick. */
    void applyDrag()
    {
        if (! dragPending || dragBand < 0) return;
        dragPending = false;
        const auto inner = innerArea();
        setParam (SxEq::pFreq (dragBand), jlimit (20.0f, 20000.0f, xToFreq (inner, dragX)));
        setParam (SxEq::pGain (dragBand), jlimit (-18.0f, 18.0f, yToDb (inner, dragY)));
    }

    void handleGraphMouseDown (const MouseEvent& e)
    {
        const auto inner = innerArea();
        if (! graphArea.contains (e.getPosition())) return;
        const int band = bandAt (e.getPosition());
        if (band >= 0)
        {
            selected = band;
            setKnobsToBand();
            if (e.mods.isPopupMenu()) { showShapeMenu (band, e.getScreenPosition()); return; }
            if (e.getNumberOfClicks() >= 2)
            {
                setParam (SxEq::pOn (band), eq.param (SxEq::pOn (band)) > 0.5f ? 0.0f : 1.0f);
                repaint();
                return;
            }
            dragBand = band;
            dragX = freqToX (inner, eq.param (SxEq::pFreq (band)));
            dragY = dbToY (inner, jlimit (-26.0f, 26.0f, eq.param (SxEq::pGain (band))));
            lastMouse = e.getPosition().toFloat();
            dragPending = false;
            eq.getParameters()[SxEq::pFreq (band)]->beginChangeGesture();
            eq.getParameters()[SxEq::pGain (band)]->beginChangeGesture();
            repaint (graphArea);
            return;
        }
        if (! inner.contains (e.getPosition().toFloat())) return;
        if (grabFrozen)
        {
            // place a band on the clicked peak: the analyser is frozen, so what is under the cursor
            // is exactly the peak that was grabbed
            int best = 0;
            float bestDb = -1.0e9f;
            const float pos = std::log10 (xToFreq (inner, (float) e.x) / 20.0f) / 3.0f * (float) SxEffectBase::kSpectrumBins - 0.5f;
            const int binAt = jlimit (0, SxEffectBase::kSpectrumBins - 1, (int) std::round (pos));
            for (int i = jmax (0, binAt - 6); i <= jmin (SxEffectBase::kSpectrumBins - 1, binAt + 6); ++i)
            {
                const float db = Decibels::gainToDecibels (frozenSpec[i], -90.0f);
                if (db > bestDb) { bestDb = db; best = i; }
            }
            addBandAt (freqToX (inner, binHz (best)), (float) e.y);
            return;
        }
        selected = -1;
        repaint();
    }

    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (chipsArea.contains (e.getPosition()))
        {
            const int b = chipAt (e.getPosition());
            if (b < 0) return;
            if (e.mods.isPopupMenu()) { showShapeMenu (b, e.getScreenPosition()); return; }
            selected = b;
            setKnobsToBand();
            if (chipSwitchBounds (b).contains (e.getPosition()))
            {
                // each frequency point has its own switch: the rest of the chip only selects it
                setParam (SxEq::pOn (b), eq.param (SxEq::pOn (b)) > 0.5f ? 0.0f : 1.0f);
            }
            repaint();
            return;
        }
        handleGraphMouseDown (e);
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (dragBand < 0) return;
        const float k = e.mods.isShiftDown() ? 0.25f : 1.0f;
        const auto p = e.getPosition().toFloat();
        dragX += (p.x - lastMouse.x) * k;
        dragY += (p.y - lastMouse.y) * k;
        lastMouse = p;
        dragPending = true;
    }

    void mouseUp (const MouseEvent&) override
    {
        if (dragBand < 0) return;
        applyDrag();
        eq.getParameters()[SxEq::pFreq (dragBand)]->endChangeGesture();
        eq.getParameters()[SxEq::pGain (dragBand)]->endChangeGesture();
        dragBand = -1;
    }

    void mouseMove (const MouseEvent& e) override
    {
        const int b = graphArea.contains (e.getPosition()) ? bandAt (e.getPosition()) : -1;
        if (b != hoverBand)
        {
            hoverBand = b;
            repaint (graphArea);
        }

        const int chip = chipAt (e.getPosition());
        setMouseCursor (chip >= 0 && chipSwitchBounds (chip).contains (e.getPosition())
                            ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
    }

    void mouseExit (const MouseEvent&) override
    {
        if (hoverBand >= 0)
        {
            hoverBand = -1;
            repaint (graphArea);
        }
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (! graphArea.contains (e.getPosition())) return;
        if (bandAt (e.getPosition()) >= 0) return;      // handled in mouseDown
        addBandAt ((float) e.x, (float) e.y);
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        const int band = bandAt (e.getPosition());
        if (band < 0) return;
        auto* p = eq.getParameters()[SxEq::pQ (band)];
        p->beginChangeGesture();
        p->setValueNotifyingHost (jlimit (0.0f, 1.0f, p->getValue() + (w.deltaY > 0.0f ? 0.02f : -0.02f)));
        p->endChangeGesture();
        selected = band;
        setKnobsToBand();
        repaint (graphArea);
    }

    bool keyPressed (const KeyPress& k) override
    {
        const int step = k.getKeyCode() == KeyPress::tabKey ? (k.getModifiers().isShiftDown() ? -1 : 1) : 0;
        if (step != 0)
        {
            selected = (sel() + step + SxEq::kNumBands) % SxEq::kNumBands;
            setKnobsToBand();
            repaint();
            return true;
        }
        if (k.getKeyCode() == KeyPress::deleteKey || k.getKeyCode() == KeyPress::backspaceKey)
        {
            setParam (SxEq::pOn (sel()), 0.0f);
            setKnobsToBand();
            repaint();
            return true;
        }
        if (k.getTextCharacter() == 'd' || k.getTextCharacter() == 'D')
        {
            setParam (SxEq::pDynOn (sel()), eq.param (SxEq::pDynOn (sel())) > 0.5f ? 0.0f : 1.0f);
            setKnobsToBand();
            repaint();
            return true;
        }
        if (k.getTextCharacter() == 's' || k.getTextCharacter() == 'S')
        {
            const bool on = eq.param (SxEq::pSolo()) > 0.5f && (int) std::round (eq.param (SxEq::pSolo())) == sel() + 1;
            setParam (SxEq::pSolo(), on ? 0.0f : (float) (sel() + 1));
            repaint();
            return true;
        }
        return false;
    }

    void setParam (int index, float v)
    {
        auto* p = eq.getParameters()[index];
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
    }

    /** The selected band, clamped: the graph can be clicked on empty space. */
    int sel() const { return jlimit (0, SxEq::kNumBands - 1, selected); }

    void setKnobsToBand()
    {
        const int s = sel();
        const int idx[5] { SxEq::pFreq (s), SxEq::pGain (s), SxEq::pQ (s), SxEq::pThr (s), SxEq::pRange (s) };
        for (int i = 0; i < knobs.size(); ++i)
        {
            knobs[i]->setParameterIndex (idx[i]);
            knobs[i]->setColourTint (kBandColours[s % 8]);
        }
        shapeBtn.setButtonText (SxEq::typeName ((int) std::round (eq.param (SxEq::pType (s)))));
        dynBtn.setToggleState (eq.param (SxEq::pDynOn (s)) > 0.5f, dontSendNotification);

        const int type = (int) std::round (eq.param (SxEq::pType (s)));
        const bool cut = type == SxEq::LowCut || type == SxEq::HighCut;
        slopeBtn.setEnabled (cut);
        slopeBtn.setButtonText (cut ? (int) std::round (eq.param (SxEq::pSlope (s))) == 0
                                          ? String ("12 dB/oct, Q")
                                          : String (SxEq::slopeName ((int) std::round (eq.param (SxEq::pSlope (s)))))
                                    : String ("slope: n/a"));

        const int an = jlimit (0, 2, (int) std::round (eq.param (SxEq::pAnalyser())));
        for (int i = 0; i < 3; ++i) analyserBtn[i].setToggleState (i == an, dontSendNotification);
    }

    void addBandAt (float x, float y)
    {
        const auto inner = innerArea();
        int free = -1;
        for (int b = 0; b < SxEq::kNumBands; ++b)
            if (eq.param (SxEq::pOn (b)) < 0.5f) { free = b; break; }
        if (free < 0) return;
        setParam (SxEq::pFreq (free), jlimit (20.0f, 20000.0f, xToFreq (inner, x)));
        setParam (SxEq::pGain (free), jlimit (-18.0f, 18.0f, yToDb (inner, y)));
        setParam (SxEq::pQ (free), 1.0f);
        setParam (SxEq::pType (free), (float) SxEq::Bell);
        setParam (SxEq::pSlope (free), 0.0f);
        setParam (SxEq::pOn (free), 1.0f);
        selected = free;
        setKnobsToBand();
        repaint();
    }

    void showShapeMenu (int band, Point<int> where)
    {
        if (band < 0) return;
        PopupMenu m;
        const int type = (int) std::round (eq.param (SxEq::pType (band)));
        for (int i = 0; i < 7; ++i) m.addItem (i + 1, SxEq::typeName (i), true, i == type);
        m.addSeparator();
        m.addItem (20, TRANS ("Dynamic Band"), true, eq.param (SxEq::pDynOn (band)) > 0.5f);
        m.addItem (21, TRANS ("Reset Band"));
        m.addItem (22, TRANS ("Remove Band"));
        auto* self = this;
        m.showMenuAsync (PopupMenu::Options().withTargetScreenArea ({ where.x, where.y, 1, 1 }), [self, band] (int r)
        {
            if (r >= 1 && r <= 7) self->setParam (SxEq::pType (band), (float) (r - 1));
            else if (r == 20) self->setParam (SxEq::pDynOn (band), self->eq.param (SxEq::pDynOn (band)) > 0.5f ? 0.0f : 1.0f);
            else if (r == 21) { self->setParam (SxEq::pGain (band), 0.0f); self->setParam (SxEq::pQ (band), 1.0f); self->setParam (SxEq::pSlope (band), 0.0f); }
            else if (r == 22) self->setParam (SxEq::pOn (band), 0.0f);
            self->selected = band;
            self->setKnobsToBand();
            self->repaint();
        });
    }

    void showSlopeMenu (int band, Point<int> where)
    {
        if (band < 0) return;
        PopupMenu m;
        const int cur = jlimit (0, 5, (int) std::round (eq.param (SxEq::pSlope (band))));
        for (int i = 0; i < 6; ++i)
            m.addItem (i + 1, i == 0 ? String ("12 dB/oct, Q") : String (SxEq::slopeName (i)), true, i == cur);
        auto* self = this;
        m.showMenuAsync (PopupMenu::Options().withTargetScreenArea ({ where.x, where.y, 1, 1 }), [self, band] (int r)
        {
            if (r >= 1 && r <= 6) self->setParam (SxEq::pSlope (band), (float) (r - 1));
            self->setKnobsToBand();
            self->repaint();
        });
    }

    Rectangle<float> innerArea() const
    {
        return graphArea.toFloat().withTrimmedLeft (dBW).withTrimmedTop (freqH).reduced (1.0f);
    }
    static float binHz (int i)
    {
        return 20.0f * std::pow (1000.0f, ((float) i + 0.5f) / (float) SxEffectBase::kSpectrumBins);
    }
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
        return a.getCentreY() - (db / 26.0f) * (a.getHeight() * 0.47f);
    }
    static float yToDb (Rectangle<float> a, float y)
    {
        return (a.getCentreY() - y) / jmax (1.0f, a.getHeight() * 0.47f) * 26.0f;
    }
    int chipWidth() const { return jmax (46, chipsArea.getWidth() / SxEq::kNumBands); }

    Rectangle<int> chipBounds (int b) const
    {
        const int w = chipWidth();
        return { chipsArea.getX() + b * w, chipsArea.getY(), w - 3, chipsArea.getHeight() - 2 };
    }

    /** The little pill on the left side of a chip: its own band on/off switch. */
    Rectangle<int> chipSwitchBounds (int b) const
    {
        auto r = chipBounds (b);
        return r.removeFromLeft (22).reduced (2, 3);
    }

    int chipAt (Point<int> p) const
    {
        if (! chipsArea.contains (p)) return -1;
        return jlimit (0, SxEq::kNumBands - 1, (p.x - chipsArea.getX()) / jmax (1, chipWidth()));
    }

    int bandAt (Point<int> p) const
    {
        const auto inner = innerArea();
        for (int b = SxEq::kNumBands - 1; b >= 0; --b)
        {
            if (eq.param (SxEq::pOn (b)) < 0.5f) continue;
            const float x = freqToX (inner, eq.param (SxEq::pFreq (b)));
            const float y = dbToY (inner, jlimit (-26.0f, 26.0f, eq.param (SxEq::pGain (b))));
            if (Point<float> (x, y).getDistanceFrom (p.toFloat()) < 14.0f) return b;
        }
        return -1;
    }

    SxEq& eq;
    OwnedArray<Knob> knobs;
    TextButton shapeBtn, slopeBtn, dynBtn, addBtn, soloBtn, grabBtn, presetBtn;
    TextButton analyserBtn[3];
    bool grabFrozen = false;
    float frozenSpec[SxEffectBase::kSpectrumBins] {};
    Rectangle<int> graphArea, chipsArea;
    int selected = 0, dragBand = -1, hoverBand = -1, tick = 0;
    int peaks[32] {}, numPeaks = 0;
    float dragX = 0.0f, dragY = 0.0f;
    Point<float> lastMouse;
    bool dragPending = false;
    static constexpr float dBW = 34.0f, freqH = 22.0f;

    // cached graph: the paths are in the coordinates of curveArea
    Rectangle<float> curveArea;
    std::array<float, 96> lastSnap {};
    std::vector<float> xs, hzs, dbs;
    Path curvePath, curveFill, dynPath, spectrumLine, spectrumFill;
    Path bandPaths[SxEq::kNumBands];
    bool dynShown = false;
};
