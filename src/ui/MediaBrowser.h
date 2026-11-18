#pragma once

#include <map>

#include "AppState.h"
#include "Widgets.h"

namespace sx
{
// What the media pool shows about the selected file (also used by the self test).
struct MediaInfo
{
    juce::String name, kind, detail, length, size, modified;
    double lengthSeconds = 0.0;
    bool audio = false;          // playable through the preview player
    bool valid = false;          // the file could be opened
};

/** Reads the selected file's properties (length, format, sample rate, bit depth, tempo ...). */
MediaInfo describeMediaFile (const juce::File& f, juce::AudioFormatManager& formats);

// Right-hand media pool: browses every local disk and imports files with a double click. The file
// under the list gets an info panel plus audition playback (play / stop, with a progress line over
// a peak preview of the waveform).
class MediaBrowser : public juce::Component,
                     private juce::ListBoxModel,
                     private juce::Timer
{
public:
    explicit MediaBrowser (AppState& a);
    ~MediaBrowser() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void refreshTheme();

    void setFolder (const juce::File& f);
    void refresh();
    void rebuildRoots();

    // the row the info panel describes (and the preview plays)
    void selectRow (int row);
    int selectedRow() const { return list.getSelectedRow(); }
    /** Starts / stops auditioning the selected file. */
    void togglePreview();
    bool isPreviewing() const { return app.engine.isPreviewActive(); }
    const MediaInfo& info() const { return selection; }

    std::function<void()> onClose;

private:
    // ListBoxModel
    int getNumRows() override { return (int) entries.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool rowIsSelected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override { openRow (row); }
    void selectedRowsChanged (int lastRowSelected) override;
    void openRow (int row);

    void timerCallback() override;
    void updateSelection();          // reads the info + the waveform peaks of the selected row
    void buildPeaks (const juce::File& f);
    juce::Rectangle<int> waveformArea() const;

    static bool isSupportedFile (const juce::File& f);
    void importEntry (const juce::File& f);

    AppState& app;
    juce::File folder;
    std::vector<juce::File> entries;
    std::map<int, juce::File> rootTargets;   // places combo id -> target folder

    // layout, computed in resized() and reused by paint() so nothing can overlap
    juce::Rectangle<int> titleRect, pathRect, toolRow, listRect, infoRect, statusRect;

    juce::TextButton upBtn { juce::String::fromUTF8 ("\xe2\x86\x91") };
    juce::TextButton refreshBtn { juce::String::fromUTF8 ("\xe2\x9f\xb3") };
    IconButton closeBtn { Icon::Close, TRANS ("Hide media pool") };
    IconButton playBtn { Icon::Play, TRANS ("Preview the selected file") };
    juce::ComboBox roots;
    juce::ListBox list { "mediapool", this };
    juce::String status;

    MediaInfo selection;
    std::vector<float> peaks;        // one column per pixel-ish slice of the selected file
    juce::File peaksFor;
    bool previewing = false;         // what the play button currently shows
};
}
