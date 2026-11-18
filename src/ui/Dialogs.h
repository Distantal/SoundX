#pragma once

#include "AppState.h"

namespace sx
{
void showExportDialog (AppState& app, ExportOptions::Mode mode);
/** Default tick state of a track in the stems export checklist: hidden tracks, muted tracks and
    the extra output channels of a multi-output instrument start unchecked (the user can still
    tick them by hand). */
bool stemTrackCheckedByDefault (const Track& t);
/** Runs an export behind the progress window (the export dialog calls this). Exposed so the self
    test can exercise the progress window and its Cancel button end to end. 'showCompletion' puts
    up the "Export finished" / error box; the self test turns it off so nothing waits for a click. */
void runExportJob (AppState& app, ExportOptions opts, bool showCompletion = true);
void showAudioSettings (AppState& app);
void showAddTracksDialog (AppState& app);
void showAboutDialog();

// Project info (title / author / genre / album / year / notes / cover picture), stored in the .sxp
void showProjectInfoDialog (AppState& app);
// renders the panel without opening the dialog (self test / visual check)
juce::Image renderProjectInfoPanel (AppState& app);

/** Writes the manual that is compiled into the exe to 'target'. False when the file cannot be
    written or this build has no manual. */
bool writeUserManual (const juce::File& target);

// MIDI import options: shows the file's tempo / time signature map next to the project's, and
// asks whether it should be written into the project. 'then' runs with the chosen setting.
void showMidiImportDialog (AppState& app, const juce::Array<juce::File>& files,
                           std::function<void (bool syncTempo)> then);

// renders the import options panel without opening the dialog (self test / visual check)
juce::Image renderMidiImportPanel (AppState& app, const juce::Array<juce::File>& files);
}
