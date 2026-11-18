#pragma once

#include "AppState.h"

namespace sx
{
// 'syncTempo' imports the file's own tempo / time signature map; the drag & drop path passes the
// remembered setting, the menu path asks in the import dialog.
bool importMidiFile (AppState& app, const juce::File& f, double atBeat, bool syncTempo = true);
bool importAudioFile (AppState& app, const juce::File& f, double atBeat, int targetTrackId);

/** Imports all of these files as one edit: the whole drop is a single undo step, so undoing a
    folder of stems takes one Ctrl+Z (like Studio One), not one per file. MIDI and audio files may
    be mixed; `targetTrackId` receives audio clips when it is an audio track (-1 adds a track per
    file), `midiSyncTempo` is used for the MIDI files in the list. */
void importFiles (AppState& app, const juce::Array<juce::File>& files, double atBeat,
                  int targetTrackId = -1, bool midiSyncTempo = true);

void promptText (const juce::String& title, const juce::String& message, const juce::String& initial,
                 std::function<void (juce::String)> callback);
bool isMidiFile (const juce::File& f);

// remembered "sync the MIDI's tempo / time signature on import" setting
bool midiImportSyncsTempo (AppState& app);
void setMidiImportSyncsTempo (AppState& app, bool sync);
}
