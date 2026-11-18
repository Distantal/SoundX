#include "MediaBrowser.h"
#include "Importer.h"
#include "Theme.h"

namespace sx
{
using namespace juce;

namespace
{
constexpr int kTitleH = 22;
constexpr int kPathH = 16;
constexpr int kToolH = 26;
constexpr int kStatusH = 18;
constexpr int kInfoH = 92;          // name + two info lines + the waveform strip
constexpr int kWaveH = 34;

String fileSizeText (int64 bytes)
{
    if (bytes < 1024) return String (bytes) + " B";
    if (bytes < 1024 * 1024) return String (bytes / 1024.0, 1) + " KB";
    if (bytes < 1024LL * 1024 * 1024) return String (bytes / (1024.0 * 1024.0), 1) + " MB";
    return String (bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
}

String durationText (double seconds)
{
    if (seconds < 0.0) return "--:--";
    // floor (not round) for the whole seconds, so 1.5 s reads "00:01.500" and not "00:02.500"
    const int64 whole = (int64) std::floor (seconds);
    int ms = (int) std::llround ((seconds - (double) whole) * 1000.0);
    int64 total = whole;
    if (ms >= 1000) { ms -= 1000; ++total; }
    const int64 h = total / 3600, m = (total / 60) % 60, s = total % 60;

    String out;
    if (h > 0) out << h << ":";
    out << String ((int) m).paddedLeft ('0', 2) << ":" << String ((int) s).paddedLeft ('0', 2);
    if (h == 0) out << "." << String (ms).paddedLeft ('0', 3);
    return out;
}

String channelsText (int n)
{
    if (n == 1) return TRANS ("Mono");
    if (n == 2) return TRANS ("Stereo");
    return String (n) + " " + TRANS ("channels");
}

String sampleRateText (double sr)
{
    return sr >= 1000.0 ? String (sr / 1000.0, sr >= 10000.0 ? 1 : 2) + " kHz" : String (sr, 0) + " Hz";
}
}

MediaInfo describeMediaFile (const File& f, AudioFormatManager& formats)
{
    MediaInfo m;
    m.name = f.getFileName();
    m.size = fileSizeText (f.getSize());
    m.modified = f.getLastModificationTime().formatted ("%Y/%m/%d %H:%M");

    if (f.hasFileExtension ("mid;midi"))
    {
        MidiFile midi;
        FileInputStream in (f);
        if (! in.openedOk() || ! midi.readFrom (in))
            return m;

        midi.convertTimestampTicksToSeconds();
        m.valid = true;
        m.kind = "MIDI";

        int notes = 0;
        double bpm = 0.0;
        for (int t = 0; t < midi.getNumTracks(); ++t)
            if (auto* seq = midi.getTrack (t))
                for (int i = 0; i < seq->getNumEvents(); ++i)
                {
                    const auto& msg = seq->getEventPointer (i)->message;
                    if (msg.isNoteOn()) ++notes;
                    if (bpm <= 0.0 && msg.isTempoMetaEvent()) bpm = msg.getTempoSecondsPerQuarterNote();
                }

        m.lengthSeconds = midi.getLastTimestamp();
        m.length = durationText (m.lengthSeconds);
        m.detail = String (notes) + " " + TRANS ("notes") + "  ·  " + String (midi.getNumTracks()) + " "
                 + TRANS ("tracks");
        if (bpm > 0.0) m.detail << "  ·  " << String (60.0 / bpm, 1) << " BPM";
        return m;
    }

    std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr)
        return m;

    m.valid = true;
    m.audio = true;
    // "WAV file" -> "WAV", "FLAC file" -> "FLAC": the extension is already in the name column
    m.kind = reader->getFormatName().upToFirstOccurrenceOf (" ", false, false).toUpperCase();
    m.lengthSeconds = reader->sampleRate > 0.0 ? (double) reader->lengthInSamples / reader->sampleRate : 0.0;
    m.length = durationText (m.lengthSeconds) + "  ·  " + m.size;

    String detail;
    if (reader->bitsPerSample > 0) detail << String ((int) reader->bitsPerSample) << " bit  ·  ";
    detail << sampleRateText (reader->sampleRate) << "  ·  " << channelsText ((int) reader->numChannels);
    m.detail = detail;
    return m;
}

MediaBrowser::MediaBrowser (AppState& a) : app (a)
{
    addAndMakeVisible (upBtn);
    upBtn.setTooltip (TRANS ("Up one folder"));
    upBtn.onClick = [this]
    {
        const auto parent = folder.getParentDirectory();
        if (parent.isDirectory() && parent != folder) setFolder (parent);
    };

    addAndMakeVisible (refreshBtn);
    refreshBtn.setTooltip (TRANS ("Refresh"));
    refreshBtn.onClick = [this] { refresh(); };

    addAndMakeVisible (closeBtn);
    closeBtn.onClick = [this] { if (onClose) onClose(); };

    addAndMakeVisible (playBtn);
    playBtn.onClick = [this] { togglePreview(); };
    playBtn.setEnabled (false);

    rebuildRoots();
    addAndMakeVisible (roots);

    list.setRowHeight (22);
    list.setColour (ListBox::backgroundColourId, theme::laneB);
    list.setColour (ListBox::outlineColourId, theme::lineSoft);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);

    auto music = File::getSpecialLocation (File::userMusicDirectory);
    setFolder (music.isDirectory() ? music : File::getSpecialLocation (File::userHomeDirectory));
    startTimerHz (12);
}

MediaBrowser::~MediaBrowser()
{
    stopTimer();
    app.engine.stopPreview();
}

void MediaBrowser::visibilityChanged()
{
    // the audition is only for listening while the pool is open
    if (! isVisible()) app.engine.stopPreview();
}

void MediaBrowser::rebuildRoots()
{
    roots.clear (dontSendNotification);
    rootTargets.clear();
    roots.setTextWhenNothingSelected (TRANS ("Places"));

    int id = 1;
    auto addPlace = [this, &id] (const String& name, const File& f)
    {
        if (! f.isDirectory()) return;
        rootTargets[id] = f;
        roots.addItem (name + "  (" + f.getFullPathName() + ")", id++);
    };

    addPlace (TRANS ("Music"), File::getSpecialLocation (File::userMusicDirectory));
    addPlace (TRANS ("Documents"), File::getSpecialLocation (File::userDocumentsDirectory));
    addPlace (TRANS ("Desktop"), File::getSpecialLocation (File::userDesktopDirectory));
    addPlace (TRANS ("Downloads"), File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Downloads"));

    // every drive of the machine (a bare "C:" is a drive-relative path, so normalise it)
    Array<File> systemRoots;
    File::findFileSystemRoots (systemRoots);
    for (auto& root : systemRoots)
    {
        auto path = root.getFullPathName();
        if (path.endsWithChar (':')) path << "/";
        addPlace (path, File (path));
    }

    roots.onChange = [this]
    {
        const int sel = roots.getSelectedId();
        if (sel <= 0) return;
        if (auto it = rootTargets.find (sel); it != rootTargets.end())
            setFolder (it->second);
        roots.setSelectedId (0, dontSendNotification);
    };
}

void MediaBrowser::setFolder (const File& f)
{
    if (! f.isDirectory()) return;
    folder = f;
    refresh();
}

void MediaBrowser::refreshTheme()
{
    list.setColour (ListBox::backgroundColourId, theme::laneB);
    list.setColour (ListBox::outlineColourId, theme::lineSoft);
    list.repaint();
    repaint();
}

void MediaBrowser::refresh()
{
    app.engine.stopPreview();
    entries.clear();
    if (folder.isDirectory())
    {
        auto dirs = folder.findChildFiles (File::findDirectories, false);
        dirs.sort();
        for (auto& d : dirs)
            if (! d.getFileName().startsWithChar ('.') && ! d.getFileName().startsWithChar ('$') && ! d.isHidden())
                entries.push_back (d);

        auto files = folder.findChildFiles (File::findFiles, false);
        files.sort();
        for (auto& f : files)
            if (! f.isHidden() && isSupportedFile (f)) entries.push_back (f);
    }
    status = folder.isDirectory()
           ? String ((int) entries.size()) + " " + TRANS ("items")
           : TRANS ("Folder not available");
    upBtn.setEnabled (folder.getParentDirectory().isDirectory() && folder.getParentDirectory() != folder);
    list.updateContent();
    list.deselectAllRows();
    updateSelection();
    list.repaint();
    repaint();
}

bool MediaBrowser::isSupportedFile (const File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg;m4a;mid;midi");
}

void MediaBrowser::selectRow (int row)
{
    if (row < 0 || row >= (int) entries.size()) { list.deselectAllRows(); return; }
    list.selectRow (row);
}

void MediaBrowser::selectedRowsChanged (int)
{
    updateSelection();
}

void MediaBrowser::updateSelection()
{
    app.engine.stopPreview();
    selection = {};
    peaks.clear();
    peaksFor = File();

    const int row = list.getSelectedRow();
    if (row >= 0 && row < (int) entries.size())
    {
        const auto f = entries[(size_t) row];
        if (! f.isDirectory())
        {
            selection = describeMediaFile (f, app.engine.formats);
            if (selection.audio) buildPeaks (f);
        }
    }

    playBtn.setEnabled (selection.audio);
    playBtn.setIcon (Icon::Play);
    previewing = false;
    resized();               // the info panel appears / disappears with the selection
    repaint();
}

void MediaBrowser::buildPeaks (const File& f)
{
    if (peaksFor == f && ! peaks.empty()) return;

    std::unique_ptr<AudioFormatReader> reader (app.engine.formats.createReaderFor (f));
    peaks.clear();
    peaksFor = File();
    if (reader == nullptr || reader->lengthInSamples <= 0) return;

    constexpr int kCols = 220;
    peaks.reserve (kCols);
    const int64 total = reader->lengthInSamples;
    const int64 per = jmax<int64> (1, total / kCols);
    AudioBuffer<float> chunk (2, 4096);

    for (int c = 0; c < kCols; ++c)
    {
        const int64 start = (int64) c * total / kCols;
        float peak = 0.0f;
        // a few slices inside the column: enough to show the shape without reading the whole file
        for (int s = 0; s < 3; ++s)
        {
            const int64 at = start + (int64) s * per / 3;
            const int n = (int) jmin<int64> (chunk.getNumSamples(), total - at);
            if (n <= 0) break;
            const bool stereo = reader->numChannels > 1;
            reader->read (&chunk, 0, n, at, true, stereo);
            peak = jmax (peak, chunk.getMagnitude (0, 0, n));
            if (stereo) peak = jmax (peak, chunk.getMagnitude (1, 0, n));
        }
        peaks.push_back (jlimit (0.0f, 1.0f, peak));
    }
    peaksFor = f;
}

Rectangle<int> MediaBrowser::waveformArea() const
{
    auto r = infoRect.withTrimmedLeft (8).withTrimmedRight (8);
    return r.removeFromBottom (kWaveH).withTrimmedRight (30);
}

void MediaBrowser::timerCallback()
{
    const bool playing = app.engine.isPreviewActive();
    if (playing == previewing)
    {
        if (playing) repaint (waveformArea());     // move the progress line only
        return;
    }

    previewing = playing;
    playBtn.setIcon (playing ? Icon::Stop : Icon::Play);
    repaint (infoRect);
}

void MediaBrowser::togglePreview()
{
    if (! selection.audio) return;
    if (app.engine.isPreviewActive())
    {
        app.engine.stopPreview();
        previewing = false;
        playBtn.setIcon (Icon::Play);
        repaint (infoRect);
        return;
    }

    const int row = list.getSelectedRow();
    if (row < 0 || row >= (int) entries.size()) return;
    const auto f = entries[(size_t) row];
    String err;
    if (! app.engine.startPreview (f, err))
        if (err.isNotEmpty()) status = err;
    previewing = app.engine.isPreviewActive();
    playBtn.setIcon (previewing ? Icon::Stop : Icon::Play);
    repaint();
}

void MediaBrowser::resized()
{
    auto r = getLocalBounds();

    auto titleRow = r.removeFromTop (kTitleH);
    closeBtn.setBounds (titleRow.removeFromRight (kTitleH).reduced (2));
    titleRect = titleRow;

    pathRect = r.removeFromTop (kPathH).withTrimmedLeft (8).withTrimmedRight (8);

    toolRow = r.removeFromTop (kToolH).reduced (6, 0);
    auto tb = toolRow;
    upBtn.setBounds (tb.removeFromLeft (26));
    tb.removeFromLeft (4);
    refreshBtn.setBounds (tb.removeFromLeft (26));
    tb.removeFromLeft (6);
    roots.setBounds (tb);

    statusRect = r.removeFromBottom (kStatusH).withTrimmedLeft (8).withTrimmedRight (8);
    infoRect = r.removeFromBottom (selection.valid ? kInfoH : 0);
    listRect = r.reduced (5, 3);
    list.setBounds (listRect);

    auto wave = waveformArea();
    const int btn = 26;
    playBtn.setBounds (jmin (getWidth() - btn - 4, wave.getRight() + 4), wave.getY() + (wave.getHeight() - btn) / 2, btn, btn);
    playBtn.setVisible (selection.valid && selection.audio);
}

void MediaBrowser::paint (Graphics& g)
{
    g.fillAll (theme::panelAlt);
    g.setColour (theme::line);
    g.fillRect (0, 0, 1, getHeight());

    g.setColour (theme::text);
    g.setFont (theme::font (11.5f, true));
    g.drawText (TRANS ("MEDIA POOL"), titleRect.withTrimmedLeft (8).withTrimmedRight (kTitleH),
                Justification::centredLeft, true);

    g.setColour (theme::textDim);
    g.setFont (theme::font (10.0f));
    g.drawFittedText (folder.getFullPathName(), pathRect, Justification::centredLeft, 1);

    if (selection.valid)
    {
        auto r = infoRect.withTrimmedLeft (8).withTrimmedRight (8);
        g.setColour (theme::line);
        g.fillRect (infoRect.getX(), infoRect.getY(), getWidth(), 1);

        auto nameRow = r.removeFromTop (17);
        g.setColour (theme::text);
        g.setFont (theme::font (11.5f, true));
        g.drawFittedText (selection.name, nameRow, Justification::centredLeft, 1);

        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f));
        g.drawFittedText (selection.kind + "  ·  " + selection.detail, r.removeFromTop (14),
                          Justification::centredLeft, 1);
        g.drawFittedText (selection.length, r.removeFromTop (14), Justification::centredLeft, 1);

        // waveform preview of the selection, with the play position on top
        auto wave = waveformArea().toFloat();
        g.setColour (theme::laneB);
        g.fillRoundedRectangle (wave, 2.0f);
        if (! peaks.empty())
        {
            const float mid = wave.getCentreY(), hh = wave.getHeight() * 0.5f - 2.0f;
            g.setColour (selection.audio ? theme::accent.withAlpha (0.75f) : theme::textFaint);
            const float colW = wave.getWidth() / (float) peaks.size();
            for (size_t i = 0; i < peaks.size(); ++i)
            {
                const float h = jmax (1.0f, peaks[i] * hh);
                g.fillRect (wave.getX() + (float) i * colW, mid - h, jmax (1.0f, colW), h * 2.0f);
            }
        }
        const double prog = app.engine.previewProgress();
        if (prog >= 0.0)
        {
            const float x = wave.getX() + (float) prog * wave.getWidth();
            g.setColour (theme::accent);
            g.fillRect (x, wave.getY(), 1.5f, wave.getHeight());
        }
        g.setColour (theme::lineSoft);
        g.drawRoundedRectangle (wave, 2.0f, 1.0f);
    }

    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.0f));
    g.drawText (status, statusRect, Justification::centredLeft, true);
}

void MediaBrowser::paintListBoxItem (int row, Graphics& g, int width, int height, bool rowIsSelected)
{
    if (row < 0 || row >= (int) entries.size()) return;
    const auto& f = entries[(size_t) row];
    const bool isDir = f.isDirectory();

    if (rowIsSelected)
    {
        g.setColour (theme::accentSoft);
        g.fillRect (0, 0, width, height);
    }

    g.setColour (isDir ? theme::accentDark : theme::text);
    g.setFont (theme::font (11.5f, isDir));
    g.drawFittedText ((isDir ? String::fromUTF8 ("\xf0\x9f\x93\x81 ") : String()) + f.getFileName(),
                      Rectangle<int> (6, 0, width - 52, height), Justification::centredLeft, 1);

    g.setColour (theme::textFaint);
    g.setFont (theme::font (9.0f));
    g.drawFittedText (isDir ? String::fromUTF8 ("\xe2\x80\xba") : f.getFileExtension().toUpperCase(),
                      width - 44, 0, 38, height, Justification::centredRight, 1);
}

void MediaBrowser::importEntry (const File& f)
{
    app.engine.stopPreview();
    const double beat = tempomap::barStartAtOrBefore (app.project.tempo,
                                                     jmax (0.0, app.engine.getPositionBeats()));
    if (isMidiFile (f))
    {
        importMidiFile (app, f, beat);
        return;
    }
    auto* sel = app.project.findTrack (app.selectedTrack);
    const int target = (sel != nullptr && sel->type == TrackType::Audio) ? sel->id : -1;
    importAudioFile (app, f, beat, target);
}

void MediaBrowser::openRow (int row)
{
    if (row < 0 || row >= (int) entries.size()) return;
    const auto f = entries[(size_t) row];
    if (f.isDirectory()) { setFolder (f); return; }
    importEntry (f);
}

void MediaBrowser::listBoxItemDoubleClicked (int row, const MouseEvent&)
{
    openRow (row);
}
}
