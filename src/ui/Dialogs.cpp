#include "Dialogs.h"
#include "SlotMenus.h"
#include "Importer.h"
#include <juce_audio_utils/juce_audio_utils.h>
#if SOUNDX_HAS_MANUAL
 #include "BinaryData.h"
#endif

namespace sx
{
using namespace juce;

namespace
{
    class ExportPanel : public Component
    {
    public:
        ExportPanel (AppState& a, ExportOptions::Mode m) : app (a), stemsMode (m == ExportOptions::Stems)
        {
            const auto mode = m;
            auto label = [this] (Label& l, const String& t)
            {
                l.setText (t, dontSendNotification);
                l.setFont (theme::font (13.0f));
                addAndMakeVisible (l);
            };
            label (modeL, TRANS ("Format"));
            label (formatL, TRANS ("File type"));
            label (rangeL, TRANS ("Range"));
            label (rateL, TRANS ("Sample rate"));
            label (bitsL, TRANS ("Bit depth"));
            label (mp3L, TRANS ("MP3 bitrate"));
            label (tailL, TRANS ("Tail (seconds)"));

            modeBox.addItem (TRANS ("Mixdown - single stereo file"), 1);
            modeBox.addItem (TRANS ("Stems - one file per track"), 2);
            modeBox.setSelectedId (mode == ExportOptions::Mix ? 1 : 2, dontSendNotification);
            modeBox.onChange = [this] { updateMode(); };
            addAndMakeVisible (modeBox);

            formatBox.addItem ("WAV", 1);
            formatBox.addItem ("FLAC", 2);
            formatBox.addItem ("MP3", 3);
            formatBox.setSelectedId (1, dontSendNotification);
            formatBox.onChange = [this] { updateFormat(); };
            addAndMakeVisible (formatBox);

            mp3Box.addItem ("128 kbps", 128);
            mp3Box.addItem ("192 kbps", 192);
            mp3Box.addItem ("256 kbps", 256);
            mp3Box.addItem ("320 kbps", 320);
            mp3Box.setSelectedId (320, dontSendNotification);
            addAndMakeVisible (mp3Box);
            updateFormat();

            rangeBox.addItem (TRANS ("Whole project"), 1);
            rangeBox.addItem (TRANS ("Cycle range"), 2);
            rangeBox.setSelectedId (app.project.loopOn ? 2 : 1, dontSendNotification);
            addAndMakeVisible (rangeBox);

            rateBox.addItem ("44100 Hz", 44100); rateBox.addItem ("48000 Hz", 48000);
            rateBox.addItem ("88200 Hz", 88200); rateBox.addItem ("96000 Hz", 96000);
            rateBox.setSelectedId (48000, dontSendNotification);
            addAndMakeVisible (rateBox);

            bitsBox.addItem ("16 bit", 16); bitsBox.addItem ("24 bit", 24); bitsBox.addItem (TRANS ("32 bit float"), 32);
            bitsBox.setSelectedId (24, dontSendNotification);
            addAndMakeVisible (bitsBox);

            tail.setRange (0.0, 10.0, 0.5);
            tail.setValue (2.0, dontSendNotification);
            tail.setSliderStyle (Slider::LinearHorizontal);
            tail.setTextBoxStyle (Slider::TextBoxRight, false, 50, 22);
            addAndMakeVisible (tail);

            normalize.setButtonText (TRANS ("Normalize to -1 dBFS"));
            addAndMakeVisible (normalize);
            writeTempoBtn.setButtonText (TRANS ("Write tempo to WAV (ACID chunk)"));
            writeTempoBtn.setTooltip (TRANS ("Stores the project tempo and the exported length in beats inside the WAV file"));
            addAndMakeVisible (writeTempoBtn);
            stemFx.setButtonText (TRANS ("Include master effects in stems"));
            stemFx.setEnabled (mode == ExportOptions::Stems);
            addAndMakeVisible (stemFx);

            // Studio One style: render through the audio device in real time instead of offline
            realtimeBtn.setButtonText (TRANS ("Real-time rendering (through the audio device)"));
            realtimeBtn.setTooltip (TRANS ("Plays the mix through the audio device and records it: as long as the song, "
                                           "at the device sample rate, but exactly what is heard during playback. "
                                           "Mixdown only."));
            realtimeBtn.onClick = [this] { updateMode(); };
            addAndMakeVisible (realtimeBtn);

            ok.setButtonText (TRANS ("Export..."));
            ok.onClick = [this]
            {
                ExportOptions o = collect();
                if (o.mode == ExportOptions::Stems && o.includeTracks.empty())
                {
                    showError (TRANS ("Select at least one track to export."));
                    return;
                }
                if (onAccept) onAccept (o);
            };
            cancel.setButtonText (TRANS ("Cancel"));
            cancel.onClick = [this] { if (onCancel) onCancel(); };
            addAndMakeVisible (ok);
            addAndMakeVisible (cancel);

            if (stemsMode) buildTrackList();
            if (stemsMode) setSize (660, 430);
            else setSize (440, 430);
        }

        // the checklist is created on demand so switching the format to Stems also shows it
        void buildTrackList()
        {
            if (! trackChecks.empty()) { trackList.setVisible (true); return; }
            for (auto& t : app.project.tracks)
            {
                // multi-output instruments: their mixer channels have no clips of their own, but
                // they can be exported as stems too (one checkbox per channel)
                const bool hasContent = ! t->midiClips.empty() || ! t->audioClips.empty();
                if (! hasContent && t->sourceTrack == 0) continue;

                const bool isChannel = t->sourceTrack != 0;
                String label = isChannel ? "    " + String::fromUTF8 ("\xe2\x86\xb3 ") + t->name : t->name;
                if (isChannel)
                {
                    const int ch = jmax (0, t->sourceChannel);
                    label += "  (ch " + String (ch + 1) + "/" + String (ch + 2) + ")";
                }
                if (t->hidden) label += "  (" + TRANS ("hidden") + ")";
                if (t->mute) label += "  (" + TRANS ("muted") + ")";
                auto* tb = new ToggleButton (label);
                tb->setToggleState (stemTrackCheckedByDefault (*t), dontSendNotification);
                tb->setColour (ToggleButton::textColourId, theme::text);
                tb->setColour (ToggleButton::tickColourId, theme::accent);
                if (isChannel)
                    tb->setTooltip (TRANS ("Include this output channel in the stems (unchecked by default)"));
                else if (t->hidden)
                    tb->setTooltip (TRANS ("Hidden track: unchecked by default"));
                else if (t->mute)
                    tb->setTooltip (TRANS ("Muted track: unchecked by default"));
                else
                    tb->setTooltip (TRANS ("Include this track in the stems"));
                trackChecks.push_back (std::unique_ptr<ToggleButton> (tb));
                trackIds.push_back (t->id);
                trackListContent.addAndMakeVisible (tb);
            }
            trackList.setViewedComponent (&trackListContent, false);
            trackList.setScrollBarsShown (true, false);
            trackList.setScrollBarThickness (11);
            addAndMakeVisible (trackList);
        }

        void updateFormat()
        {
            const int sel = formatBox.getSelectedId();
            const bool mp3 = sel == 3;
            mp3L.setVisible (mp3);
            mp3Box.setVisible (mp3);
            bitsL.setVisible (! mp3);
            bitsBox.setVisible (! mp3);
            // FLAC stores 16 or 24 bit only
            if (sel == 2 && bitsBox.getSelectedId() == 32) bitsBox.setSelectedId (24, dontSendNotification);
            resized();
            repaint();
        }

        void updateMode()
        {
            stemsMode = modeBox.getSelectedId() == 2;
            stemFx.setEnabled (stemsMode);
            trackList.setVisible (stemsMode && ! trackChecks.empty());
            if (stemsMode) buildTrackList();

            // real-time rendering is a mixdown feature; stems always render offline
            if (stemsMode) realtimeBtn.setToggleState (false, dontSendNotification);
            realtimeBtn.setEnabled (! stemsMode);
            rateBox.setEnabled (! realtimeBtn.getToggleState());

            const int w = stemsMode ? 660 : 440;
            const int h = 430;
            if (getWidth() != w || getHeight() != h)
            {
                setSize (w, h);
                // the dialog window follows the content so the checklist gets its room
                if (auto* dw = findParentComponentOfClass<DialogWindow>())
                    dw->setContentComponentSize (w, h);
            }
            resized();
            repaint();
        }

        ExportOptions collect() const
        {
            ExportOptions o;
            o.mode = modeBox.getSelectedId() == 1 ? ExportOptions::Mix : ExportOptions::Stems;
            o.useLoopRange = rangeBox.getSelectedId() == 2;
            o.sampleRate = rateBox.getSelectedId();
            o.bitDepth = bitsBox.getSelectedId();
            o.tailSeconds = tail.getValue();
            o.normalize = normalize.getToggleState();
            o.stemsWithMasterFx = stemFx.getToggleState();
            o.format = formatBox.getSelectedId() == 3 ? ExportOptions::Mp3
                     : formatBox.getSelectedId() == 2 ? ExportOptions::Flac : ExportOptions::Wav;
            o.writeTempo = writeTempoBtn.getToggleState() && o.format == ExportOptions::Wav;
            o.mp3Bitrate = jmax (96, mp3Box.getSelectedId());
            o.realtime = o.mode == ExportOptions::Mix && realtimeBtn.getToggleState();
            for (size_t i = 0; i < trackChecks.size(); ++i)
                if (trackChecks[i]->getToggleState()) o.includeTracks.insert (trackIds[i]);
            return o;
        }

        void paint (Graphics& g) override
        {
            g.fillAll (theme::panel);
            if (stemsMode)
            {
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.0f, true));
                g.drawText (TRANS ("TRACKS TO EXPORT"), 432, 12, getWidth() - 432 - 12, 18, Justification::centredLeft);

                g.setColour (theme::line);
                g.fillRect (416, 12, 1, getHeight() - 24);

                if (trackChecks.empty())
                {
                    g.setColour (theme::textFaint);
                    g.setFont (theme::font (12.0f));
                    g.drawText (TRANS ("No tracks with audio or MIDI content"),
                                Rectangle<int> (432, 40, getWidth() - 432 - 16, 40), Justification::topLeft);
                }
            }
        }

        void resized() override
        {
            auto full = getLocalBounds();
            auto r = full.reduced (20, 16);

            // in stems mode the left column stops before the checklist so nothing overlaps
            auto left = stemsMode ? r.withWidth (jmin (380, r.getWidth())) : r;
            auto row = [&left] (Label& l, Component& c)
            {
                auto rr = left.removeFromTop (28);
                l.setBounds (rr.removeFromLeft (120));
                c.setBounds (rr);
                left.removeFromTop (8);
            };
            row (modeL, modeBox);
            row (formatL, formatBox);
            row (rangeL, rangeBox);
            row (rateL, rateBox);
            row (bitsL, bitsBox);
            if (mp3Box.isVisible()) row (mp3L, mp3Box);
            row (tailL, tail);
            normalize.setBounds (left.removeFromTop (26));
            stemFx.setBounds (left.removeFromTop (26));
            realtimeBtn.setBounds (left.removeFromTop (26));
            // the tempo tag only exists for WAV
            writeTempoBtn.setVisible (formatBox.getSelectedId() == 1);
            if (writeTempoBtn.isVisible()) writeTempoBtn.setBounds (left.removeFromTop (26));

            auto b = r.removeFromBottom (30);
            ok.setBounds (b.removeFromRight (110));
            b.removeFromRight (8);
            cancel.setBounds (b.removeFromRight (90));

            if (stemsMode)
            {
                auto listArea = full;
                listArea.removeFromLeft (424);
                listArea.removeFromTop (34);        // room for the header text
                listArea.removeFromBottom (44);     // room for the buttons
                trackList.setBounds (listArea.reduced (8, 4));
                const int cw = jmax (60, trackList.getWidth() - 16);
                trackListContent.setSize (cw, jmax (trackList.getHeight(), (int) trackChecks.size() * 24));
                int y = 0;
                for (auto& tb : trackChecks) { tb->setBounds (0, y, cw, 22); y += 24; }
            }
        }

        std::function<void (ExportOptions)> onAccept;
        std::function<void()> onCancel;

    private:
        AppState& app;
        Label modeL, formatL, rangeL, rateL, bitsL, mp3L, tailL;
        ComboBox modeBox, formatBox, rangeBox, rateBox, bitsBox, mp3Box;
        Slider tail;
        ToggleButton normalize, stemFx, writeTempoBtn, realtimeBtn;
        TextButton ok, cancel;
        std::vector<std::unique_ptr<juce::ToggleButton>> trackChecks;
        std::vector<int> trackIds;
        Component trackListContent;
        Viewport trackList;
        bool stemsMode = false;
    };

}

void runExportJob (AppState& app, ExportOptions opts, bool showCompletion)
{
    auto exportTrace = [] (const String& s)
    {
        static const String path = SystemStats::getEnvironmentVariable ("SOUNDX_TRACE", {});
        if (path.isNotEmpty()) File (path).appendText (s + "\n");
    };

    struct ExportJob : public Thread
    {
        ExportJob (AppState& a, ExportOptions o) : Thread ("SoundX export"), app (a), opts (std::move (o)) {}

        void run() override
        {
            ok = app.engine.exportAudio (opts, [this] (double p)
            {
                progress.store (p);
                return ! cancel.load();
            }, error);
        }

        AppState& app;
        ExportOptions opts;
        std::atomic<double> progress { 0.0 };
        std::atomic<bool> cancel { false };
        bool ok = false;
        String error;
    };

    const auto uiStart = Time::getMillisecondCounterHiRes();
    exportTrace ("export ui: enter at 0 ms");
    app.engine.stop();
    exportTrace ("export ui: transport stopped at " + String (Time::getMillisecondCounterHiRes() - uiStart) + " ms");
    ExportJob job (app, opts);
    job.startThread();
    exportTrace ("export ui: thread started at " + String (Time::getMillisecondCounterHiRes() - uiStart) + " ms");

    // Quick exports finish before a window is worth showing. A progress window is only put up
    // for longer renders; the message loop below keeps running, so an export thread that needs
    // the message thread (device callback, prepare) is never blocked - and cancelling therefore
    // cannot deadlock (the old ThreadWithProgressWindow did exactly that and could crash).
    const auto began = Time::getMillisecondCounter();
    while (job.isThreadRunning() && Time::getMillisecondCounter() - began < 150)
        MessageManager::getInstance()->runDispatchLoopUntil (5);

    if (job.isThreadRunning())
    {
        double progressValue = 0.0;
        auto* aw = new AlertWindow (TRANS ("Exporting audio..."), {}, MessageBoxIconType::NoIcon);
        aw->addProgressBarComponent (progressValue);
        aw->addButton (TRANS ("Cancel"), 1);
        Component::SafePointer<AlertWindow> safe (aw);
        aw->enterModalState (true, ModalCallbackFunction::create ([&job] (int r)
        {
            if (r == 1) job.cancel.store (true);
        }), true);

        while (job.isThreadRunning())
        {
            progressValue = job.progress.load();
            MessageManager::getInstance()->runDispatchLoopUntil (10);
        }
        exportTrace ("export ui: progress loop done at " + String (Time::getMillisecondCounterHiRes() - uiStart) + " ms");
        if (safe != nullptr) safe->exitModalState (0);
        exportTrace ("export ui: window closed at " + String (Time::getMillisecondCounterHiRes() - uiStart) + " ms");
    }

    job.stopThread (5000);
    exportTrace ("export ui: thread stopped at " + String (Time::getMillisecondCounterHiRes() - uiStart) + " ms (ok="
                 + String ((int) job.ok) + ")");
    exportTrace ("export ui: before message box at " + String (Time::getMillisecondCounterHiRes() - uiStart) + " ms");
    if (showCompletion)
    {
        if (job.ok)
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "SoundX", TRANS ("Export finished.") + "\n" + opts.target.getFullPathName());
        else if (job.error != TRANS ("Export cancelled."))
            showError (job.error);
    }
    exportTrace ("export ui: returned at " + String (Time::getMillisecondCounterHiRes() - uiStart) + " ms");
}

bool stemTrackCheckedByDefault (const Track& t)
{
    // Hidden tracks still play, muted tracks still exist, and a multi-output instrument's extra
    // channels are often not wanted as separate stems: all three start unchecked, but the user
    // can tick them in the export window.
    return ! t.hidden && ! t.mute && t.sourceTrack == 0;
}

void showExportDialog (AppState& app, ExportOptions::Mode mode)
{
    if (app.project.tracks.empty()) { showError (TRANS ("The project is empty.")); return; }

    // Only one export window at a time. A second command (double click, shortcut + menu, or a
    // deferred launch racing another one) brings the existing dialog to the front instead of
    // stacking modal windows, which used to make the next click appear to do nothing.
    static Component::SafePointer<DialogWindow> currentExportDialog;
    if (currentExportDialog != nullptr)
    {
        if (currentExportDialog->isVisible())
        {
            currentExportDialog->toFront (true);
            currentExportDialog->grabKeyboardFocus();
            return;
        }
        currentExportDialog = nullptr;
    }

    auto* panel = new ExportPanel (app, mode);
    DialogWindow::LaunchOptions lo;
    lo.dialogTitle = TRANS ("Export Audio");
    lo.content.setOwned (panel);
    lo.dialogBackgroundColour = theme::panel;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    auto* dw = lo.launchAsync();
    currentExportDialog = dw;

    auto* appPtr = &app;
    panel->onCancel = [dw] { dw->exitModalState (0); };
    panel->onAccept = [appPtr, dw] (ExportOptions o)
    {
        dw->exitModalState (1);
        const String wildcard = o.format == ExportOptions::Mp3 ? "*.mp3"
                              : o.format == ExportOptions::Flac ? "*.flac" : "*.wav";
        auto chooser = std::make_shared<FileChooser> (o.mode == ExportOptions::Mix ? TRANS ("Export mixdown") : TRANS ("Choose a folder for the stems"),
                                                      File::getSpecialLocation (File::userMusicDirectory),
                                                      o.mode == ExportOptions::Mix ? wildcard : "");
        const int flags = o.mode == ExportOptions::Mix
                              ? FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting
                              : FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories;
        // The dialog's modal state is torn down asynchronously. Open the native file chooser on
        // the next message-loop turn so it becomes the active window instead of appearing behind
        // the main window (or swallowing the next click).
        MessageManager::callAsync ([appPtr, o, chooser, flags]() mutable
        {
            chooser->launchAsync (flags, [appPtr, o, chooser] (const FileChooser& fc) mutable
            {
                auto f = fc.getResult();
                if (f == File()) return;
                o.target = f;
                MessageManager::callAsync ([appPtr, o] { runExportJob (*appPtr, o); });
            });
        });
    };
}

//==============================================================================
// MIDI import: what the file says about tempo / meter, and whether to take it over
namespace
{
    String joinValues (const StringArray& values, int maxItems)
    {
        StringArray shown;
        for (int i = 0; i < values.size() && i < maxItems; ++i) shown.add (values[i]);
        auto text = shown.joinIntoString (juce::String::fromUTF8 (" \xe2\x86\x92 "));   // " -> "
        if (values.size() > maxItems) text += " ...";
        return text;
    }

    class MidiImportPanel : public Component
    {
    public:
        MidiImportPanel (AppState& a, const Array<File>& files) : app (a)
        {
            const auto& p = app.project;
            StringArray bpms, meters;
            int tempoChanges = 0, meterChanges = 0;
            bool anyMap = false;

            // the first file decides what the dialog shows (the setting applies to all of them)
            if (! files.isEmpty())
            {
                const auto map = readMidiTempoMap (files[0], p.bpm, p.tsNum, p.tsDen);
                anyMap = ! map.empty();
                for (size_t i = 0; i < map.size(); ++i)
                {
                    const auto& s = map[i];
                    const bool first = i == 0;
                    auto& prev = map[i == 0 ? 0 : i - 1];
                    if (first || std::abs (s.bpm - prev.bpm) > 1.0e-6)
                    {
                        bpms.add (String (s.bpm, s.bpm == std::floor (s.bpm) ? 0 : 1) + " BPM");
                        if (! first) ++tempoChanges;
                    }
                    if (first || s.tsNum != prev.tsNum || s.tsDen != prev.tsDen)
                    {
                        meters.add (String (s.tsNum) + "/" + String (s.tsDen));
                        if (! first) ++meterChanges;
                    }
                }
            }

            projectText = TRANS ("Project now") + ": " + String (p.bpm, p.bpm == std::floor (p.bpm) ? 0 : 1) + " BPM  "
                          + String (p.tsNum) + "/" + String (p.tsDen);

            if (! anyMap)
            {
                midiText = TRANS ("This MIDI file carries no tempo or time signature information.");
            }
            else
            {
                midiText = TRANS ("MIDI file") + ": " + TRANS ("Tempo") + " " + joinValues (bpms, 4)
                           + (tempoChanges > 0 ? " (" + String (tempoChanges) + " " + TRANS ("changes") + ")" : String())
                           + "   " + TRANS ("Time Signature") + " " + joinValues (meters, 4)
                           + (meterChanges > 0 ? " (" + String (meterChanges) + " " + TRANS ("changes") + ")" : String());
            }

            syncBtn.setButtonText (TRANS ("Write the MIDI's tempo and time signature into the project"));
            syncBtn.setEnabled (anyMap);
            syncBtn.setToggleState (anyMap && midiImportSyncsTempo (app), dontSendNotification);
            syncBtn.setColour (ToggleButton::textColourId, theme::text);
            syncBtn.setColour (ToggleButton::tickColourId, theme::accent);
            addAndMakeVisible (syncBtn);

            importBtn.setButtonText (TRANS ("Import"));
            importBtn.onClick = [this] { if (onAccept) onAccept (syncBtn.getToggleState() && syncBtn.isEnabled()); };
            cancelBtn.setButtonText (TRANS ("Cancel"));
            cancelBtn.onClick = [this] { if (onCancel) onCancel(); };
            addAndMakeVisible (importBtn);
            addAndMakeVisible (cancelBtn);

            title = files.size() == 1 ? files[0].getFileName()
                                      : String (files.size()) + " " + TRANS ("MIDI files");
            setSize (520, 212);
        }

        std::function<void (bool syncTempo)> onAccept;
        std::function<void()> onCancel;

        void paint (Graphics& g) override
        {
            g.fillAll (theme::panel);
            g.setColour (theme::text);
            g.setFont (theme::font (14.0f, true));
            g.drawFittedText (title, 16, 12, getWidth() - 32, 20, Justification::centredLeft, 1);

            g.setFont (theme::font (12.0f));
            g.setColour (theme::text);
            g.drawFittedText (midiText, 16, 38, getWidth() - 32, 34, Justification::topLeft, 2);

            g.setColour (theme::textDim);
            g.drawFittedText (projectText, 16, 72, getWidth() - 32, 18, Justification::centredLeft, 1);

            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.0f));
            g.drawFittedText (TRANS ("When off, only the notes, controllers and track names are imported and the project keeps its own tempo map."),
                              16, 126, getWidth() - 32, 30, Justification::topLeft, 2);
        }

        void resized() override
        {
            syncBtn.setBounds (14, 96, getWidth() - 28, 26);
            importBtn.setBounds (getWidth() - 16 - 96, getHeight() - 16 - 26, 96, 26);
            cancelBtn.setBounds (getWidth() - 16 - 96 - 8 - 96, getHeight() - 16 - 26, 96, 26);
        }

    private:
        AppState& app;
        ToggleButton syncBtn;
        TextButton importBtn, cancelBtn;
        String title, midiText, projectText;
    };
}

void showMidiImportDialog (AppState& app, const Array<File>& files, std::function<void (bool syncTempo)> then)
{
    if (files.isEmpty()) return;

    auto* panel = new MidiImportPanel (app, files);
    DialogWindow::LaunchOptions lo;
    lo.dialogTitle = TRANS ("Import MIDI");
    lo.content.setOwned (panel);
    lo.dialogBackgroundColour = theme::panel;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    auto* dw = lo.launchAsync();

    auto* appPtr = &app;
    panel->onCancel = [dw] { dw->exitModalState (0); };
    panel->onAccept = [appPtr, dw, then] (bool syncTempo)
    {
        setMidiImportSyncsTempo (*appPtr, syncTempo);   // remembered for drag & drop and next time
        dw->exitModalState (1);
        if (then) then (syncTempo);
    };
}

Image renderMidiImportPanel (AppState& app, const Array<File>& files)
{
    if (files.isEmpty()) return {};
    MidiImportPanel panel (app, files);
    return panel.createComponentSnapshot (panel.getLocalBounds(), true);
}

void showAudioSettings (AppState& app)
{
    // the dialog lets the user pick / configure a driver: hold the device open (and re-open it
    // if the idle release closed it) until the dialog is closed, or the selector would list no
    // running device and changes would not be audible
    app.engine.keepDeviceOpen = true;
    app.engine.ensureDeviceOpen();

    // clears the hold when the dialog window is destroyed (any way it is closed)
    struct HoldReleaser : public juce::ComponentListener
    {
        Engine& engine;
        explicit HoldReleaser (Engine& e) : engine (e) {}
        void componentBeingDeleted (juce::Component& c) override
        {
            c.removeComponentListener (this);
            engine.keepDeviceOpen = false;
            delete this;
        }
    };

    auto* sel = new AudioDeviceSelectorComponent (app.engine.deviceManager, 0, 16, 0, 2, true, false, true, false);
    sel->setSize (560, 460);
    DialogWindow::LaunchOptions lo;
    lo.dialogTitle = TRANS ("Audio / MIDI Settings");
    lo.content.setOwned (sel);
    lo.dialogBackgroundColour = theme::panel;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    auto* dw = lo.launchAsync();
    if (dw != nullptr) dw->addComponentListener (new HoldReleaser (app.engine));
}

//==============================================================================
// Project info: the user's title / author / genre / album / year / notes and a cover picture.
// Everything is written into the .sxp (the picture as base64 PNG), so a project file carries its
// own metadata and artwork.
namespace
{
    class CoverBox : public Component, public FileDragAndDropTarget
    {
    public:
        std::function<void (const File&)> onFile;
        std::function<void()> onClicked;
        void setImage (const Image& img) { image = img; repaint(); }

        bool isInterestedInFileDrag (const StringArray& files) override
        {
            return files.size() == 1 && ImageFileFormat::findImageFormatForFileExtension (File (files[0])) != nullptr;
        }
        void fileDragEnter (const StringArray&, int, int) override { over = true; repaint(); }
        void fileDragExit (const StringArray&) override { over = false; repaint(); }
        void filesDropped (const StringArray& files, int, int) override
        {
            over = false;
            if (files.isEmpty()) return;
            // take the first image that actually decodes
            for (auto& path : files)
                if (auto img = ImageFileFormat::loadFrom (File (path)); img.isValid())
                {
                    if (onFile) onFile (File (path));
                    break;
                }
            repaint();
        }
        void mouseDown (const MouseEvent&) override { if (onClicked) onClicked(); }

        void paint (Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (theme::laneB);
            g.fillRoundedRectangle (r, 3.0f);
            if (over)
            {
                g.setColour (theme::accent.withAlpha (0.25f));
                g.fillRoundedRectangle (r, 3.0f);
            }
            if (image.isValid())
            {
                auto area = r.reduced (2.0f);
                const float scale = jmin (area.getWidth() / (float) image.getWidth(),
                                          area.getHeight() / (float) image.getHeight());
                auto drawn = Rectangle<float> (image.getWidth() * scale, image.getHeight() * scale)
                                 .withCentre (area.getCentre());
                g.drawImage (image, drawn);
            }
            else
            {
                g.setColour (theme::textFaint);
                g.setFont (theme::font (10.5f));
                g.drawFittedText (TRANS ("Drop an image here, or click to choose one"),
                                  getLocalBounds().reduced (6), Justification::centred, 2);
            }
            g.setColour (over ? theme::accent : theme::lineSoft);
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
        }

    private:
        Image image;
        bool over = false;
    };

    class ProjectInfoPanel : public Component
    {
    public:
        explicit ProjectInfoPanel (AppState& a) : app (a)
        {
            auto& m = app.project.meta;
            auto add = [this] (TextEditor& e, const String& text, bool multi = false)
            {
                e.setText (text, false);
                e.setFont (theme::font (12.0f));
                e.setMultiLine (multi, multi);
                e.setReturnKeyStartsNewLine (multi);
                addAndMakeVisible (e);
            };
            add (titleE, m.title);
            add (authorE, m.author);
            add (genreE, m.genre);
            add (albumE, m.album);
            add (yearE, m.year);
            add (notesE, m.comment, true);

            addAndMakeVisible (cover);
            cover.setImage (m.cover);
            cover.onClicked = [this] { chooseImage(); };
            cover.onFile = [this] (const File& f) { loadImage (f); };

            addAndMakeVisible (pickBtn);
            pickBtn.setButtonText (TRANS ("Choose Image..."));
            pickBtn.onClick = [this] { chooseImage(); };
            addAndMakeVisible (clearBtn);
            clearBtn.setButtonText (TRANS ("Remove Image"));
            clearBtn.onClick = [this]
            {
                image = {};
                cover.setImage ({});
                repaint();
            };

            addAndMakeVisible (okBtn);
            okBtn.setButtonText (TRANS ("OK"));
            okBtn.onClick = [this] { accept(); };
            addAndMakeVisible (cancelBtn);
            cancelBtn.setButtonText (TRANS ("Cancel"));
            cancelBtn.onClick = [this] { if (onCancel) onCancel(); };

            image = m.cover;
            setSize (470, 430);
        }

        std::function<void()> onCancel, onAccept;

        void paint (Graphics& g) override
        {
            g.fillAll (theme::panel);
            g.setColour (theme::text);
            g.setFont (theme::font (13.0f, true));
            g.drawText (TRANS ("PROJECT INFO"), getLocalBounds().removeFromTop (26).withTrimmedLeft (12),
                        Justification::centredLeft);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f));
            for (int i = 0; i < (int) fieldRects.size(); ++i)
                g.drawText (fieldCaptions[(size_t) i], fieldRects[(size_t) i], Justification::centredLeft);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            r.removeFromTop (20);

            fieldCaptions.clear();
            fieldRects.clear();
            auto row = [&r, this] (const String& caption, TextEditor& e, int h = 24)
            {
                r.removeFromTop (4);
                auto line = r.removeFromTop (h);
                fieldCaptions.push_back (caption);
                fieldRects.push_back (line.removeFromLeft (86));
                e.setBounds (line);
            };

            row (TRANS ("Title"), titleE);
            row (TRANS ("Author"), authorE);
            row (TRANS ("Genre"), genreE);
            row (TRANS ("Album"), albumE);
            row (TRANS ("Year"), yearE);

            r.removeFromTop (6);
            auto coverRow = r.removeFromTop (150);
            fieldCaptions.push_back (TRANS ("Cover image"));
            fieldRects.push_back (coverRow.removeFromLeft (86));
            auto btns = coverRow.removeFromRight (110).reduced (6, 0);
            cover.setBounds (coverRow);
            pickBtn.setBounds (btns.removeFromTop (24));
            btns.removeFromTop (6);
            clearBtn.setBounds (btns.removeFromTop (24));

            r.removeFromTop (8);
            // notes: same caption column as the fields, taking whatever height is left
            {
                auto line = r.removeFromTop (jmax (40, r.getHeight() - 40));
                fieldCaptions.push_back (TRANS ("Notes"));
                fieldRects.push_back (line.removeFromLeft (86).withTrimmedTop (3));
                notesE.setBounds (line);
            }

            auto footer = getLocalBounds().removeFromBottom (38).reduced (12, 7);
            cancelBtn.setBounds (footer.removeFromRight (90));
            footer.removeFromRight (8);
            okBtn.setBounds (footer.removeFromRight (90));
        }

        void accept()
        {
            auto& p = app.project;
            auto& m = p.meta;
            const bool changed = m.title != titleE.getText().trim() || m.author != authorE.getText().trim()
                              || m.genre != genreE.getText().trim() || m.album != albumE.getText().trim()
                              || m.year != yearE.getText().trim() || m.comment != notesE.getText().trim()
                              || m.coverData != coverBytes;

            if (changed)
            {
                p.checkpoint();
                m.title = titleE.getText().trim();
                m.author = authorE.getText().trim();
                m.genre = genreE.getText().trim();
                m.album = albumE.getText().trim();
                m.year = yearE.getText().trim();
                m.comment = notesE.getText();
                if (image.isValid()) m.setCover (image);
                else m.clearCover();
                p.sendContentChanged();
            }
            if (onAccept) onAccept();
        }

    private:
        void chooseImage()
        {
            chooser = std::make_shared<FileChooser> (TRANS ("Choose a cover image"),
                                                     File::getSpecialLocation (File::userPicturesDirectory),
                                                     "*.png;*.jpg;*.jpeg;*.gif;*.bmp");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [this] (const FileChooser& fc)
                                  {
                                      const auto f = fc.getResult();
                                      if (f.existsAsFile()) loadImage (f);
                                  });
        }

        void loadImage (const File& f)
        {
            auto img = ImageFileFormat::loadFrom (f);
            if (! img.isValid()) return;
            // keep the project file small: at most 900 px on the long edge
            const int longest = jmax (img.getWidth(), img.getHeight());
            if (longest > 900)
            {
                const double s = 900.0 / (double) longest;
                img = img.rescaled (jmax (1, roundToInt (img.getWidth() * s)),
                                    jmax (1, roundToInt (img.getHeight() * s)),
                                    Graphics::highResamplingQuality);
            }
            image = img;
            cover.setImage (image);
            repaint();
        }

        AppState& app;
        TextEditor titleE, authorE, genreE, albumE, yearE, notesE;
        CoverBox cover;
        TextButton pickBtn, clearBtn, okBtn, cancelBtn;
        Image image;
        MemoryBlock coverBytes;
        std::shared_ptr<FileChooser> chooser;
        std::vector<String> fieldCaptions;
        std::vector<Rectangle<int>> fieldRects;
    };
}

void showProjectInfoDialog (AppState& app)
{
    auto* panel = new ProjectInfoPanel (app);
    DialogWindow::LaunchOptions lo;
    lo.dialogTitle = TRANS ("Project Info");
    lo.content.setOwned (panel);
    lo.dialogBackgroundColour = theme::panel;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    auto* dw = lo.launchAsync();
    panel->onCancel = [dw] { dw->exitModalState (0); };
    panel->onAccept = [dw] { dw->exitModalState (1); };
}

Image renderProjectInfoPanel (AppState& app)
{
    ProjectInfoPanel panel (app);
    panel.setSize (470, 430);
    panel.resized();
    return panel.createComponentSnapshot (panel.getLocalBounds(), true);
}

bool writeUserManual (const File& target)
{
#if SOUNDX_HAS_MANUAL
    if (target.existsAsFile() && target.getSize() == (int64) BinaryData::SoundXManual_pdfSize)
        return true;
    auto stream = std::unique_ptr<FileOutputStream> (target.createOutputStream());
    if (stream == nullptr) return false;
    if (! stream->write (BinaryData::SoundXManual_pdf, BinaryData::SoundXManual_pdfSize)) return false;
    stream->flush();
    return true;
#else
    juce::ignoreUnused (target);
    return false;
#endif
}

void showAboutDialog()
{
    AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "SoundX",
                                      String ("SoundX ") + JUCE_APPLICATION_VERSION_STRING + "\n\n"
                                      + TRANS ("A local DAW with MIDI / CC / automation editing, VST2 + VST3 hosting, recording, mixing and WAV / stem export.")
                                      + "\n\n"
                                      + TRANS ("Open source and free: use it for nothing, source code included, build and change it yourself.")
                                      + "\n\n"
                                      + TRANS ("Author: Distantal")
                                      + "\n"
                                      + TRANS ("Built with C++20 and JUCE 8.")
                                      + "\n\n"
                                      + TRANS ("Open source: free to use, the source is included.")
                                      + "\n"
                                      + TRANS ("The user manual is in Help > User Manual (PDF)."));
}

void showAddTracksDialog (AppState& app)
{
    auto* box = new AlertWindow (TRANS ("Add Multiple Tracks"), TRANS ("Create several tracks at once."), MessageBoxIconType::NoIcon);

    auto* countBox = new ComboBox ("count");
    const std::vector<int> counts { 1, 2, 4, 8, 16, 32 };
    for (int i = 0; i < (int) counts.size(); ++i) countBox->addItem (String (counts[(size_t) i]), i + 1);
    countBox->setSelectedId (3, dontSendNotification);   // 4
    countBox->setSize (120, 24);

    auto* typeBox = new ComboBox ("type");
    typeBox->addItem (TRANS ("Software Instrument"), 1);
    typeBox->addItem (TRANS ("Audio Track"), 2);
    typeBox->setSelectedId (1, dontSendNotification);
    typeBox->setSize (200, 24);

    box->addTextBlock (TRANS ("Count"));
    box->addCustomComponent (countBox);
    box->addTextBlock (TRANS ("Type"));
    box->addCustomComponent (typeBox);
    box->addButton (TRANS ("Add"), 1, KeyPress (KeyPress::returnKey));
    box->addButton (TRANS ("Cancel"), 0, KeyPress (KeyPress::escapeKey));

    auto* appPtr = &app;
    auto countsPtr = std::make_shared<std::vector<int>> (counts);
    box->enterModalState (true, ModalCallbackFunction::create ([appPtr, countsPtr, countBox, typeBox] (int r)
    {
        if (r == 1)
        {
            const int n = (*countsPtr)[(size_t) jlimit (0, (int) countsPtr->size() - 1, countBox->getSelectedId() - 1)];
            const TrackType type = typeBox->getSelectedId() == 2 ? TrackType::Audio : TrackType::Instrument;

            // one dialog = one undo step: adding sixteen tracks is one Ctrl+Z, not sixteen
            appPtr->project.beginUndoBatch();
            for (int i = 0; i < n; ++i) appPtr->addTrack (type);
            appPtr->project.endUndoBatch();
        }
    }), true);
}
}
