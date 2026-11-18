#include "AraHost.h"

#if JUCE_PLUGINHOST_ARA && (JUCE_PLUGINHOST_VST3 || JUCE_PLUGINHOST_AU)

namespace sx
{
using namespace juce;

//==============================================================================
namespace
{
// Host side state of one ARA audio source: the clip's samples plus the book keeping the plug-in
// needs, mapped back from the opaque ARAAudioSourceHostRef the model object was created with.
struct AraSource
{
    std::shared_ptr<AudioData> data;
    int clipId = 0;

    int numChannels() const   { return data != nullptr ? jmax (1, data->buffer.getNumChannels()) : 1; }
    int64_t numSamples() const { return data != nullptr ? data->buffer.getNumSamples() : 0; }
    double sampleRate() const  { return data != nullptr && data->sampleRate > 0.0 ? data->sampleRate : 44100.0; }
};

struct AraReader
{
    AraSource* source = nullptr;
    bool use64 = false;
};

// Serves the audio of the project's clips to the plug-in.
class AraAudioAccess final : public ARA::Host::AudioAccessControllerInterface
{
public:
    std::vector<std::unique_ptr<AraSource>> sources;

    ARA::ARAAudioReaderHostRef createAudioReaderForSource (ARA::ARAAudioSourceHostRef hostRef,
                                                           bool use64BitSamples) noexcept override
    {
        auto* r = new AraReader();
        r->source = reinterpret_cast<AraSource*> (hostRef);
        r->use64 = use64BitSamples;
        return reinterpret_cast<ARA::ARAAudioReaderHostRef> (r);
    }

    bool readAudioSamples (ARA::ARAAudioReaderHostRef readerRef, ARA::ARASamplePosition position,
                           ARA::ARASampleCount count, void* const buffers[]) noexcept override
    {
        auto* r = reinterpret_cast<AraReader*> (readerRef);
        if (r == nullptr || r->source == nullptr || r->source->data == nullptr || buffers == nullptr)
            return false;

        const auto& buf = r->source->data->buffer;
        const int channels = jmax (1, buf.getNumChannels());
        const int64_t total = buf.getNumSamples();
        const int64_t pos = (int64_t) position;

        for (int ch = 0; ch < channels; ++ch)
        {
            if (buffers[ch] == nullptr) continue;
            const int srcCh = jmin (ch, buf.getNumChannels() - 1);
            for (int64_t i = 0; i < (int64_t) count; ++i)
            {
                const int64_t p = pos + i;
                const float v = (p >= 0 && p < total) ? buf.getSample (srcCh, (int) p) : 0.0f;
                if (r->use64) static_cast<double*> (buffers[ch])[i] = (double) v;
                else          static_cast<float*> (buffers[ch])[i] = v;
            }
        }

        return true;
    }

    void destroyAudioReader (ARA::ARAAudioReaderHostRef readerRef) noexcept override
    {
        delete reinterpret_cast<AraReader*> (readerRef);
    }
};

// Stores what the plug-in archives. Kept in memory for now: writing the archive into the project
// file is the next step of the ARA integration.
class AraArchiving final : public ARA::Host::ArchivingControllerInterface
{
public:
    juce::MemoryBlock stored;

    ARA::ARASize getArchiveSize (ARA::ARAArchiveReaderHostRef) noexcept override
    {
        return (ARA::ARASize) stored.getSize();
    }

    bool readBytesFromArchive (ARA::ARAArchiveReaderHostRef, ARA::ARASize position, ARA::ARASize length,
                               ARA::ARAByte buffer[]) noexcept override
    {
        if (buffer == nullptr || (size_t) position + (size_t) length > stored.getSize()) return false;
        std::memcpy (buffer, static_cast<const char*> (stored.getData()) + position, (size_t) length);
        return true;
    }

    bool writeBytesToArchive (ARA::ARAArchiveWriterHostRef, ARA::ARASize position, ARA::ARASize length,
                              const ARA::ARAByte buffer[]) noexcept override
    {
        if (buffer == nullptr) return false;
        const size_t end = (size_t) position + (size_t) length;
        if (end > stored.getSize()) stored.setSize (end, true);
        std::memcpy (static_cast<char*> (stored.getData()) + position, buffer, (size_t) length);
        return true;
    }

    void notifyDocumentArchivingProgress (float) noexcept override {}
    void notifyDocumentUnarchivingProgress (float) noexcept override {}

    ARA::ARAPersistentID getDocumentArchiveID (ARA::ARAArchiveReaderHostRef) noexcept override
    {
        return "SoundX-ARA-1";
    }
};
}

//==============================================================================
class AraHost::Impl
{
public:
    Impl() = default;
    ~Impl() { regions.clear(); playbackRenderer = {}; editorRenderer = {}; extension = {}; sequence.reset(); context.reset();
              modifications.clear(); audioSources.clear(); document.reset(); }

    void init (ARAFactoryWrapper factory, const juce::String& documentName)
    {
        if (factory.get() == nullptr) return;

        document = ARAHostDocumentController::create (std::move (factory), documentName,
                                                     std::move (audioAccess),
                                                     std::move (archiving));
        if (document == nullptr) return;

        const auto allRoles = ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole
                            | ARA::kARAEditorViewRole;
        extension = document->bindDocumentToPluginInstance (*instance, allRoles, allRoles);
        playbackRenderer = extension.getPlaybackRendererInterface();
        editorRenderer   = extension.getEditorRendererInterface();

        auto contextProps = ARAHostModel::MusicalContext::getEmptyProperties();
        contextProps.name = documentName.toRawUTF8();
        contextProps.orderIndex = 0;
        context = std::make_unique<ARAHostModel::MusicalContext> (
                      reinterpret_cast<ARA::ARAMusicalContextHostRef> (this),
                      document->getDocumentController(), contextProps);

        auto seqProps = ARAHostModel::RegionSequence::getEmptyProperties();
        seqProps.name = documentName.toRawUTF8();
        seqProps.orderIndex = 0;
        seqProps.musicalContextRef = context->getPluginRef();
        sequence = std::make_unique<ARAHostModel::RegionSequence> (
                       reinterpret_cast<ARA::ARARegionSequenceHostRef> (this),
                       document->getDocumentController(), seqProps);

        ready = true;
    }

    void clearModel()
    {
        // regions must leave the renderer before the model objects go away
        regions.clear();
        audioSources.clear();
        modifications.clear();
        if (audioAccessOwned != nullptr) audioAccessOwned->sources.clear();
    }

    AudioPluginInstance* instance = nullptr;
    std::unique_ptr<AraAudioAccess> audioAccess = std::make_unique<AraAudioAccess>();
    std::unique_ptr<AraArchiving> archiving  = std::make_unique<AraArchiving>();
    AraAudioAccess* audioAccessOwned = audioAccess.get();
    AraArchiving* archivingOwned   = archiving.get();

    std::unique_ptr<ARAHostDocumentController> document;
    ARAHostModel::PlugInExtensionInstance extension;
    ARAHostModel::PlaybackRendererInterface playbackRenderer;
    ARAHostModel::EditorRendererInterface editorRenderer;

    std::unique_ptr<ARAHostModel::MusicalContext> context;
    std::unique_ptr<ARAHostModel::RegionSequence> sequence;
    std::vector<std::unique_ptr<ARAHostModel::AudioSource>> audioSources;
    std::vector<std::unique_ptr<ARAHostModel::AudioModification>> modifications;
    std::vector<std::unique_ptr<ARAHostModel::PlaybackRegion>> regions;

    bool ready = false;
};

//==============================================================================
AraHost::AraHost() : impl (std::make_unique<Impl>()) {}
AraHost::~AraHost() = default;

bool AraHost::isAvailable() { return true; }
bool AraHost::isReady() const { return impl->ready; }
int AraHost::sourceCount() const { return (int) impl->audioSources.size(); }
int AraHost::regionCount() const { return (int) impl->regions.size(); }
bool AraHost::hasPlaybackRenderer() const { return impl->playbackRenderer.isValid(); }

void AraHost::attach (AudioPluginInstance& instance, const String& documentName,
                      std::function<void (bool)> onReady)
{
    impl->instance = &instance;

    createARAFactoryAsync (instance, [this, documentName, onReady] (ARAFactoryWrapper factory)
    {
        impl->init (std::move (factory), documentName);
        if (onReady != nullptr) onReady (impl->ready);
    });
}

void AraHost::sync (const Project& project, int trackId)
{
    if (! impl->ready) return;

    auto* track = const_cast<Project&> (project).findTrack (trackId);
    if (track == nullptr) return;

    impl->clearModel();

    auto& dc = impl->document->getDocumentController();
    const auto& clips = track->audioClips;

    for (auto& clip : clips)
    {
        if (clip.data == nullptr) continue;

        // ARA takes C strings and only promises to read them during the call, so keep them alive
        // for the whole iteration
        const String idStr (clip.id);
        const String nameStr (clip.name);

        auto source = std::make_unique<AraSource>();
        source->data = clip.data;
        source->clipId = clip.id;
        auto* sourcePtr = source.get();
        impl->audioAccessOwned->sources.push_back (std::move (source));

        auto sourceProps = ARAHostModel::AudioSource::getEmptyProperties();
        sourceProps.name = nameStr.toRawUTF8();
        sourceProps.persistentID = idStr.toRawUTF8();
        sourceProps.sampleCount = sourcePtr->numSamples();
        sourceProps.sampleRate = sourcePtr->sampleRate();
        sourceProps.channelCount = sourcePtr->numChannels();
        sourceProps.merits64BitSamples = false;

        auto audioSource = std::make_unique<ARAHostModel::AudioSource> (
                               reinterpret_cast<ARA::ARAAudioSourceHostRef> (sourcePtr), dc, sourceProps);
        impl->audioSources.push_back (std::move (audioSource));

        auto modProps = ARAHostModel::AudioModification::getEmptyProperties();
        modProps.name = nameStr.toRawUTF8();
        modProps.persistentID = idStr.toRawUTF8();
        auto modification = std::make_unique<ARAHostModel::AudioModification> (
                                reinterpret_cast<ARA::ARAAudioModificationHostRef> (sourcePtr),
                                dc, *impl->audioSources.back(), modProps);
        impl->modifications.push_back (std::move (modification));

        const double startSec = tempomap::beatsToSeconds (project.tempo, clip.start);

        auto regionProps = ARAHostModel::PlaybackRegion::getEmptyProperties();
        regionProps.transformationFlags = ARA::kARAPlaybackTransformationNoChanges;
        regionProps.startInModificationTime = clip.offsetSec;
        regionProps.durationInModificationTime = clip.lengthSec;
        regionProps.startInPlaybackTime = startSec;
        regionProps.durationInPlaybackTime = clip.lengthSec;
        regionProps.regionSequenceRef = impl->sequence->getPluginRef();
        regionProps.name = nameStr.toRawUTF8();

        auto region = std::make_unique<ARAHostModel::PlaybackRegion> (
                          reinterpret_cast<ARA::ARAPlaybackRegionHostRef> (sourcePtr),
                          dc, *impl->modifications.back(), regionProps);
        impl->playbackRenderer.add (*region);
        impl->regions.push_back (std::move (region));
    }
}

juce::MemoryBlock AraHost::archive()
{
    if (! impl->ready || impl->archivingOwned == nullptr) return {};

    impl->archivingOwned->stored.reset();
    auto& dc = impl->document->getDocumentController();
    const auto writer = reinterpret_cast<ARA::ARAArchiveWriterHostRef> (impl->archivingOwned);
    if (! dc.storeDocumentToArchive (writer)) return {};
    return impl->archivingOwned->stored;
}

void AraHost::restoreArchive (const juce::MemoryBlock& block)
{
    if (! impl->ready || impl->archivingOwned == nullptr || block.getSize() == 0) return;

    impl->archivingOwned->stored = block;
    auto& dc = impl->document->getDocumentController();
    const auto reader = reinterpret_cast<ARA::ARAArchiveReaderHostRef> (impl->archivingOwned);
    if (dc.beginRestoringDocumentFromArchive (reader))
        dc.endRestoringDocumentFromArchive (reader);
}

void AraHost::detach()
{
    impl->clearModel();
    impl->sequence.reset();
    impl->context.reset();
    impl->extension = {};
    impl->document.reset();
    impl->ready = false;
    impl->instance = nullptr;
}
}

//==============================================================================
#else

namespace sx
{
// no ARA SDK in this build: the class stays as an empty shell so the engine can keep the pointer
class AraHost::Impl {};

AraHost::AraHost() : impl (std::make_unique<Impl>()) {}
AraHost::~AraHost() = default;
juce::MemoryBlock AraHost::archive() { return {}; }
void AraHost::restoreArchive (const juce::MemoryBlock&) {}
bool AraHost::isAvailable() { return false; }
bool AraHost::isReady() const { return false; }
int AraHost::sourceCount() const { return 0; }
int AraHost::regionCount() const { return 0; }
bool AraHost::hasPlaybackRenderer() const { return false; }
void AraHost::attach (AudioPluginInstance&, const String&, std::function<void (bool)> onReady)
{
    if (onReady != nullptr) onReady (false);
}
void AraHost::sync (const Project&, int) {}
juce::MemoryBlock AraHost::archive()
{
    if (! impl->ready || impl->archivingOwned == nullptr) return {};

    impl->archivingOwned->stored.reset();
    auto& dc = impl->document->getDocumentController();
    const auto writer = reinterpret_cast<ARA::ARAArchiveWriterHostRef> (impl->archivingOwned);
    if (! dc.storeDocumentToArchive (writer)) return {};
    return impl->archivingOwned->stored;
}

void AraHost::restoreArchive (const juce::MemoryBlock& block)
{
    if (! impl->ready || impl->archivingOwned == nullptr || block.getSize() == 0) return;

    impl->archivingOwned->stored = block;
    auto& dc = impl->document->getDocumentController();
    const auto reader = reinterpret_cast<ARA::ARAArchiveReaderHostRef> (impl->archivingOwned);
    if (dc.beginRestoringDocumentFromArchive (reader))
        dc.endRestoringDocumentFromArchive (reader);
}

void AraHost::detach() {}
}
#endif
