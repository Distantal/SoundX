#pragma once

#include "../model/Model.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>

namespace sx
{
/** ARA hosting for one audio track's insert slot (Melodyne, Auto-Tune, Revoice ...).

    ARA lets a plug-in read the audio files and the timeline of the project directly instead of
    capturing the insert stream. This class owns the host side: the ARA document, one audio source
    per audio clip (backed by the clip's samples) and one playback region per clip at its timeline
    position, and binds the plug-in instance to that document with the playback / editor roles.

    Lifecycle: one AraHost per ARA capable insert. The AudioPluginInstance passed to attach() must
    outlive the AraHost (the engine keeps the instance and the AraHost together in the same slot).
*/
class AraHost
{
public:
    AraHost();
    ~AraHost();

    /** True when the build was made with the ARA SDK. */
    static bool isAvailable();

    /** Starts the binding for 'instance'. onReady is called on the message thread - with false
        when the plug-in turned out to have no ARA factory after all. */
    void attach (juce::AudioPluginInstance& instance, const juce::String& documentName,
                 std::function<void (bool)> onReady);

    bool isReady() const;
    /** How many audio sources / playback regions the document currently holds. */
    int sourceCount() const;
    int regionCount() const;
    /** True when the bound instance fulfils the playback renderer role. */
    bool hasPlaybackRenderer() const;

    /** (Re)builds the document model from the audio clips of 'trackId'. */
    void sync (const Project& project, int trackId);

    /** Asks the plug-in to write its document into the host archive and returns the bytes, so the
        host can store the pitch/audio edits in the project file. Empty when ARA is not bound. */
    juce::MemoryBlock archive();

    /** Loads an archive back into the bound document (after sync(), so the sources and regions the
        archive refers to exist again). */
    void restoreArchive (const juce::MemoryBlock& block);

    void detach();

private:
    class Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AraHost)
};
}
