#include "Importer.h"
#include "SlotMenus.h"

namespace sx
{
using namespace juce;

bool isMidiFile (const File& f) { return f.hasFileExtension ("mid;midi;smf"); }

namespace
{
    // beginUndoBatch / endUndoBatch, exception safe: the step is closed even if an import throws
    struct UndoBatch
    {
        explicit UndoBatch (Project& p) : project (p) { project.beginUndoBatch(); }
        ~UndoBatch() { project.endUndoBatch(); }

        Project& project;

        JUCE_DECLARE_NON_COPYABLE (UndoBatch)
    };
}

void importFiles (AppState& app, const Array<File>& files, double atBeat, int targetTrackId, bool midiSyncTempo)
{
    if (files.isEmpty()) return;

    // one drop = one edit: undoing the import of a folder of stems has to take all of them back at
    // once (one undo step per file is what this batch exists to prevent)
    const UndoBatch batch (app.project);

    for (auto& f : files)
    {
        if (isMidiFile (f)) importMidiFile (app, f, atBeat, midiSyncTempo);
        else                importAudioFile (app, f, atBeat, targetTrackId);
    }
}

bool importMidiFile (AppState& app, const File& f, double atBeat, bool syncTempo)
{
    auto& p = app.project;
    p.checkpoint();
    int added = 0;
    if (! p.importMidi (f, atBeat, added, syncTempo))
    {
        showError (TRANS ("Could not read any notes from this MIDI file.") + "\n" + f.getFullPathName());
        return false;
    }
    p.sendStructureChanged();
    p.sendContentChanged();
    p.sendMixChanged();
    if (! p.tracks.empty()) app.selectTrack (p.tracks[p.tracks.size() - (size_t) added]->id);
    return true;
}

bool midiImportSyncsTempo (AppState& app)
{
    auto* props = app.plugins.properties();
    if (props == nullptr) return true;
    const String stored = props->getValue ("midiImportTempo");
    // nothing picked yet: into an empty project the file's map is obviously wanted, into a project
    // that already has music the safe answer is to keep the project's own tempo
    if (stored.isEmpty()) return app.project.tracks.empty();
    return stored.getIntValue() != 0;
}

void setMidiImportSyncsTempo (AppState& app, bool sync)
{
    if (auto* props = app.plugins.properties())
    {
        props->setValue ("midiImportTempo", sync ? "1" : "0");
        props->saveIfNeeded();
    }
}

bool importAudioFile (AppState& app, const File& f, double atBeat, int targetTrackId)
{
    // the clip appears immediately; AppState decodes the samples in the background
    return app.importAudio (f, atBeat, targetTrackId);
}

void promptText (const String& title, const String& message, const String& initial, std::function<void (String)> callback)
{
    auto* aw = new AlertWindow (title, message, MessageBoxIconType::NoIcon);
    aw->addTextEditor ("text", initial);
    aw->addButton (TRANS ("OK"), 1, KeyPress (KeyPress::returnKey));
    aw->addButton (TRANS ("Cancel"), 0, KeyPress (KeyPress::escapeKey));
    aw->enterModalState (true, ModalCallbackFunction::create ([aw, callback] (int r)
    {
        if (r == 1 && callback) callback (aw->getTextEditorContents ("text"));
    }), true);
}
}
