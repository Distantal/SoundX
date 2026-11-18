#pragma once

#include "model/Model.h"
#include "PluginManager.h"
#include "AraHost.h"
#include "BuiltInSynth.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <set>
#include <map>

namespace sx
{
/** Fixed worker pool for the render graph: threads are created once, and each block only wakes
    them with a small callable. No per-job heap allocation is needed.

    The caller takes part in the work as well, and the workers spin for a short moment before
    they block: waking a thread through the operating system costs more than a small render
    block, and a worker that is late by a millisecond is exactly what made multi-core playback
    stutter at random. */
class RenderWorkerPool
{
public:
    RenderWorkerPool() = default;
    ~RenderWorkerPool();
    void start (int numberOfThreads);
    void stop();
    int threadCount() const { return (int) threads.size(); }

    /** Runs item 0..count-1 on the pool, with a FIXED lane owner per item: item i is always
        rendered by lane (i % laneCount), where lane 0 is the caller (the audio thread) and
        lanes 1..threadCount() are the pool's workers. Work stealing is deliberately not used:
        a track (and therefore its hosted plug-in's processBlock) must run on the same thread
        every block, which many VST2/VST3 plug-ins require for thread-local / COM / UI state.

        The callback lives on the caller's stack and is only invoked while runLanes() is
        waiting, so no heap allocated task object is needed and nothing is allocated on the
        audio thread. */
    template <typename Fn>
    void runLanes (int count, Fn fn)
    {
        if (count <= 0) return;
        runTask (count, [] (void* context, int index)
        {
            (*static_cast<Fn*> (context)) (index);
        }, &fn);
    }

private:
    using TaskFn = void (*) (void*, int);
    void runTask (int count, TaskFn task, void* context);
    void workerLoop (int lane);
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable done;
    // Each worker has its own wake-up event. Only the lanes that actually own an item of the
    // current job are signalled: waking every worker for a two-track level (and making the audio
    // thread wait for all of them) was a large part of the multi-core CPU load and its random
    // latency. A lane that has nothing to do stays blocked and costs no CPU at all.
    static constexpr int kMaxThreads = 16;
    std::array<std::unique_ptr<juce::WaitableEvent>, kMaxThreads> laneWake;
    std::array<std::atomic<bool>, kMaxThreads> laneWork {};
    // The job payload is stable while a job runs: the caller only publishes the next one after
    // every signalled lane has acknowledged the current one.
    std::atomic<TaskFn> currentFn { nullptr };
    std::atomic<void*> currentContext { nullptr };
    std::atomic<int> finishedWorkers { 0 };
    // caller lane (0) + every worker lane; set when the pool is (re)started
    std::atomic<int> laneCount { 1 };
    std::atomic<bool> stopping { false };
    std::atomic<int> itemCount { 0 };
    // How long a worker spins before it blocks: the hand-off must not cost a kernel wake-up for
    // every block, but a long spin burns a whole core between blocks. A short spin covers the
    // normal case (the audio thread is usually still inside the callback), the event covers the
    // rest; this keeps multi-core playback at roughly the CPU cost of the work itself.
    static constexpr int kSpinCycles = 512;
    static constexpr int kMinSpinCycles = 64;
};

class Engine;

struct PluginInstance : public juce::AudioProcessorListener
{
    ~PluginInstance() override { if (proc != nullptr) proc->removeListener (this); }

    std::unique_ptr<juce::AudioProcessor> proc;
    std::unique_ptr<AraHost> ara;   // set on audio tracks when the plug-in declares ARA
    juce::int64 uid = 0;
    int ownerTrack = -1; // -1 = master bus
    juce::String name;
    bool isInstrument = false;
    bool araBound() const { return ara != nullptr && ara->isReady(); }
    std::atomic<bool> bypass { false };
    std::atomic<bool> editorBypass { false };   // BYPASS button in the plug-in editor: listen-only, not saved
    std::atomic<bool> postFader { false };   // master inserts only: applied after the fader
    std::atomic<int> touchedParam { -1 };
    std::atomic<int> latencySamples { 0 }; // reported by the plug-in, used for PDC
    Engine* engine = nullptr;

    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override;
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override;
};

struct TrackRuntime
{
    static constexpr int kChannels = 64;     // room for multi-output instruments
    static constexpr int kMaxBlock = 2048;
    static constexpr int kPdcCap = 32768;    // maximum delay-compensation depth, stereo

    TrackRuntime() { buf.setSize (kChannels, kMaxBlock); accum.setSize (kChannels, kMaxBlock);
                     preBuf.setSize (kChannels, kMaxBlock); postBuf.setSize (kChannels, kMaxBlock);
                     delayLine.assign ((size_t) kPdcCap * 2, 0.0f); preDelayLine.assign ((size_t) kPdcCap * 2, 0.0f); }

    // fixed-size stereo ring buffer, allocated on the message thread only
    bool idleSkipActive = false;   // the idle-instrument path is currently bypassing this track

    void resetDelay() { std::fill (delayLine.begin(), delayLine.end(), 0.0f); std::fill (preDelayLine.begin(), preDelayLine.end(), 0.0f); write = 0; preWrite = 0;
                        padApplied = prePadApplied = -1; padFade = prePadFade = 0; }

    int trackId = 0;
    // side-chain feed: this track's post-inserts signal, kept for the block so a plug-in on
    // another track can use it as its detector input
    float scL[kMaxBlock] {}, scR[kMaxBlock] {};
    int scLen = 0;
    std::atomic<float> volNorm { 0.75f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> mute { false }, solo { false }, arm { false }, monitor { false };
    std::atomic<bool> listenSolo { false };      // this track is monitored through the listen bus
    // Send-solo support (see Engine::renderChunk): soloExplicit is the user's own solo (never the
    // auto-solo of a group that something feeding it is soloed into); feedNeeded marks a track
    // that has to keep rendering, with its dry output muted, so an explicitly soloed effect
    // return still gets the sends that feed it.
    std::atomic<bool> soloExplicit { false };
    std::atomic<bool> feedNeeded { false };
    // set while an ARA bound insert renders this track's audio itself (buildSnapshot then leaves
    // the raw clips out, so the plug-in's rendering is not doubled)
    std::atomic<bool> araActive { false };
    std::atomic<bool> isAudio { false };
    std::atomic<int> midiChannel { 1 };
    std::atomic<int> inputChannel { 0 };
    std::atomic<bool> inputMono { true };
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    // RMS accumulation and a clip latch: the meters show both, like a proper console
    std::atomic<float> rmsAccL { 0.0f }, rmsAccR { 0.0f };
    std::atomic<int> rmsCount { 0 };
    std::atomic<bool> clipped { false };
    std::atomic<float> autoVol { -1.0f }, autoPan { -1.0f };
    std::atomic<int> touched { 0 }; // 1 = volume, 2 = pan

    juce::AudioBuffer<float> buf;
    juce::AudioBuffer<float> accum;     // sum of everything routed / sent into a bus or send track
    juce::AudioBuffer<float> postBuf;   // post-fader / pre-PDC send tap (deferred routing)
    juce::MidiBuffer midi, noMidi, instrumentMidi;   // instrumentMidi: one copy per layer
    float lastGainL = 1.0f, lastGainR = 1.0f;
    bool lastPhase = false;
    bool wasAudible = true;      // mute/solo CPU skip: reset the plug-ins once on the way out
    bool routeSilent = true;     // the last deferred render produced nothing to route
    bool dryMuted = false;       // rendered only to feed sends: its dry output is not routed

    std::vector<float> delayLine;       // PDC ring buffer, interleaved stereo
    static constexpr int kPdcMask = kPdcCap - 1;
    int write = 0;
    // pre-fader sends need their own delay ring so they stay aligned with the dry path
    std::vector<float> preDelayLine;
    int preWrite = 0;
    juce::AudioBuffer<float> preBuf;
    // scratch for the post-fader send taps (they are taken before the track's PDC pad)
    juce::AudioBuffer<float> sendBuf { 2, kMaxBlock };

    // declick state for the two compensation rings (audio thread only): the delay these rings
    // apply changes when a plug-in is added, removed or changes its latency, and a changed delay
    // must never replay material that was already heard
    int padApplied = -1, padFade = 0;
    int prePadApplied = -1, prePadFade = 0;
};

struct MidiEv
{
    double beat = 0.0;
    double sec = 0.0;   // absolute time in seconds (rate independent), precomputed from the tempo map
    juce::MidiMessage msg;
};

struct AudioRegionSnap
{
    double startBeat = 0.0, lengthSec = 0.0, offsetSec = 0.0;
    float gain = 1.0f;
    double fadeIn = 0.0, fadeOut = 0.0;
    int fadeInCurve = 1, fadeOutCurve = 1;   // seconds
    // automatic crossfade with the neighbouring clip on the same track (equal power)
    double xfadeStart = -1.0, xfadeEnd = -1.0;   // region-local seconds
    bool xfadeOut = false;                       // true: ramp 1->0, false: ramp 0->1
    int xfadeCurve = 0;                          // 0 equal power, 1 linear, 2 S-curve
    std::vector<ClipGainPoint> gainEnv;          // clip gain envelope (clip-relative beats)
    bool reversed = false;                       // play the used source segment backwards
    std::shared_ptr<const AudioData> data;
};

struct LaneSnap
{
    enum Kind { Vol, Pan, Param } kind = Vol;
    std::vector<AutoPoint> points;
    std::shared_ptr<PluginInstance> inst;
    juce::AudioProcessorParameter* param = nullptr;
    int paramIndex = -1;
    mutable float lastApplied = -1.0f;
};

// one delay line per send (kept alive across snapshots by shared_ptr, owned by the message thread)
struct SendDelayState
{
    std::vector<float> ring;    // interleaved stereo, 2 * TrackRuntime::kPdcCap
    int write = 0, applied = -1, fade = 0;
    SendDelayState() : ring ((size_t) TrackRuntime::kPdcCap * 2, 0.0f) {}
};

struct SendSnap
{
    TrackRuntime* target = nullptr;
    float gain = 1.0f;
    bool pre = false;
    // a send taps the source *before* its PDC pad, so a latency plug-in on the return has to be
    // compensated on the send itself (otherwise the return comes back late and doubles the dry)
    int pad = 0;
    std::shared_ptr<SendDelayState> delay;
};

// one stacked instrument of a track: the plug-in plus the balance of that layer
struct InstrumentSnap
{
    std::shared_ptr<PluginInstance> inst;
    float gain = 1.0f, pan = 0.0f;
};

struct TrackSnap
{
    std::shared_ptr<TrackRuntime> rt;
    std::vector<MidiEv> events;
    std::vector<AudioRegionSnap> regions;
    std::vector<LaneSnap> lanes;
    AutoMode autoMode = AutoMode::Read;
    //== plug-in delay compensation (computed in Engine::buildSnapshot)
    int pdc = 0;        // latency from this track's source to the master output (own chain + the
                        // chains of every bus it plays through + the master chain)
    int ownPdc = 0;     // latency of this track's own plug-in chain, bypassed plug-ins included
    int fillPdc = 0;    // latency of the bypassed plug-ins: the host delays by this instead, so
                        // switching bypass on and off never moves the track in time
    int pad = 0;        // delay applied at this track's output so that all tracks line up
    bool frozen = false;   // pre-rendered: the regions below are the frozen audio, plugins are off
    int frozenLatency = 0; // latency that is already inside that frozen audio (instrument + baked chain)
    std::shared_ptr<PluginInstance> instrument;
    std::vector<InstrumentSnap> extraInstruments;   // instruments stacked on top of the first one
    std::vector<std::shared_ptr<PluginInstance>> inserts;
    std::vector<int> insertSidechain;   // side-chain source track per insert (0 = none)
    // side-chain source runtimes, resolved when the snapshot is built: the render thread must
    // never look a track up in Engine::runtimes (the message thread mutates that map)
    std::vector<std::shared_ptr<TrackRuntime>> insertSidechainRt;
    // routing
    bool isBus = false, isSend = false;
    bool phaseInvert = false;      // Ø: invert the channel polarity after the fader/pan
    bool hasOutputChildren = false; // multi-output instrument: keep rendering while muted
    bool isListenBus = false;        // rendered at the end of the master chain, not as a normal bus
    TrackRuntime* dest = nullptr;             // nullptr = master
    std::vector<SendSnap> sends;
    // multi-output instrument: this channel track plays a pair of the parent's plug-in output, so
    // it has to be rendered after that track and inherit the instrument's latency
    std::shared_ptr<TrackRuntime> sourceRt;   // nullptr = a normal track
    int sourceChannel = 0;
};

struct Snap
{
    std::vector<TempoSegment> tempo;    // full tempo / meter map
    int maxPdc = 0;                     // deepest source-to-master path: the whole mix is delayed by this
    int masterPdc = 0;                  // total latency of the master chain
    int masterFill = 0;                 // latency of the bypassed master inserts (host compensation)
    double bpm = 120.0;                 // mirror of tempo.front() for quick UI use
    int tsNum = 4, tsDen = 4;
    bool loopOn = false;
    double loopStartBeat = 0.0, loopEndBeat = 0.0;
    std::vector<TrackSnap> tracks;
    std::vector<int> order;             // render order: senders before their send targets, inputs before their bus
    // multi-core scheduling, precomputed when the snapshot is built: tracks in one level never
    // read another track's output, so a level can render concurrently. Empty = the graph has to
    // be rendered serially (onlyTrack renders, a cycle, ...). This must never be computed on the
    // audio thread: the vectors would allocate there, which is what caused the random dropouts.
    std::vector<std::vector<int>> levels;
    std::vector<std::shared_ptr<PluginInstance>> master;      // master inserts (pre- and post-fader by flag)
    // listen bus: monitors the master mix with its own level and inserts (post-fader by flag)
    std::vector<std::shared_ptr<PluginInstance>> listen;
    float listenGain = 1.0f;
    bool hasListen = false;
    std::vector<AutoPoint> masterVolAuto;   // master fader curve (empty = static fader)
    std::vector<AutoLane> masterLanes;      // master automation, applied to master inserts
    // master automation lanes that drive plug-in parameters: the instance and the parameter are
    // resolved when the snapshot is built (message thread), so the audio thread never has to look
    // a plug-in up in Engine::instances (which the message thread mutates)
    struct MasterParamLane
    {
        std::shared_ptr<PluginInstance> inst;
        juce::AudioProcessorParameter* param = nullptr;
        std::vector<AutoPoint> points;
    };
    std::vector<MasterParamLane> masterParamLanes;
    AutoMode masterAutoMode = AutoMode::Read;   // Off: master lanes and fader curve are ignored
    bool masterPhaseInvert = false;

};

struct ExportOptions
{
    enum Mode { Mix, Stems } mode = Mix;
    enum Format { Wav, Flac, Mp3 } format = Wav;
    int mp3Bitrate = 320;            // kbps, MP3 only
    juce::File target;               // file for Mix, directory for Stems
    double sampleRate = 44100.0;
    int bitDepth = 24;               // 16, 24 or 32 (float)
    bool normalize = false;
    bool useLoopRange = false;
    double tailSeconds = 2.0;
    bool stemsWithMasterFx = false;
    bool writeTempo = false;         // WAV only: store the tempo / beat count as an "acid" chunk
    // Studio One style: render in real time through the audio device instead of offline (needed
    // by plug-ins that do not work when processed faster than the clock). Uses the device's
    // sample rate; mixdown only.
    bool realtime = false;
    std::set<int> includeTracks;     // stems: only these track ids are exported (empty = all)
};

class Engine : public juce::AudioIODeviceCallback,
               public juce::MidiInputCallback,
               public juce::AudioPlayHead,
               public Project::Listener,
               private juce::Timer,
               private juce::AsyncUpdater
{
public:
    Engine (Project& p, PluginManager& pm);
    ~Engine() override;

    Project& project;
    PluginManager& plugins;
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formats;

    juce::String initialiseAudio();

    // folder used for recordings while the project has not been saved yet
    // (empty = the default per-session folder inside the system temp directory)
    void setRecordingBaseFolder (const juce::File& f) { recordBaseDir = f; }
    const juce::File& getRecordingBaseFolder() const { return recordBaseDir; }
    juce::File recordingFolder();
    // folder holding the temporary session data: unsaved recordings and autosave snapshots
    juce::File getCacheFolder();

    //== transport
    void play();
    void stop();
    void togglePlay() { isPlaying() ? stop() : play(); }
    void record();
    bool isPlaying() const { return playing.load(); }
    bool isRecording() const { return recording.load(); }
    double getPositionBeats() const;
    void setPositionBeats (double beats);
    /** User scrub / ruler seek: while playing, the audio thread fades the old block out and the
        new position in, instead of cutting the waveform at a random sample. */
    void seekToBeats (double beats);
    double getSampleRate() const { return sampleRate; }
    bool inCountIn() const { return posSamples.load() < 0 && playing.load(); }

    //== metering
    struct Meters { float peakL = 0.0f, peakR = 0.0f, rmsL = 0.0f, rmsR = 0.0f; bool clipped = false; };
    /** Reads and resets the meters of a track (or the master when trackId < 0). */
    void readMeters (int trackId, Meters& out);
    void readTrackPeaks (int trackId, float& l, float& r);
    void readMasterPeaks (float& l, float& r);
    /** The stereo image of the last few milliseconds: interleaved L/R pairs (goniometer). */
    void readScope (float* interleaved, int maxPairs);
    bool isMasterMuted() const { return masterMute.load(); }   // the state the audio thread uses
    /** Multi-core rendering: independent tracks render on a worker pool when the routing graph is
        flat (no buses, sends, side-chains or multi-output dependencies). */
    void setMultiCoreEnabled (bool on) { multiCore.store (on, std::memory_order_release); }
    bool isMultiCoreEnabled() const { return multiCore.load (std::memory_order_acquire); }
    /** Number of worker threads the render graph uses (0 = no pool, serial). */
    int multiCoreThreads() const { return renderWorkers; }
    /** Number of CPU cores taking part in the render: the audio thread plus the workers. */
    int multiCoreCores() const { return renderWorkers > 0 ? renderWorkers + 1 : 1; }
    /** How many cores auto mode would pick on this machine. */
    static int autoRenderCores();
    /** Selects how many cores render the graph (0 = auto, 1 = serial). Message thread only: the
        pool is restarted, so the audio callback is detached for a moment. */
    void setRenderCores (int cores);
    /** The selected setting: 0 = auto, otherwise the fixed core count. */
    int getRenderCoresSetting() const { return renderCoresSetting; }
    bool lastBlockWasMultiCore() const { return lastRenderParallel.load (std::memory_order_acquire); }
    /** Idle device release: an exclusive driver (ASIO / ASIO4ALL) would keep every other app
        silent for as long as SoundX is open. While the transport is stopped (and nothing needs
        the device) it is closed; the next play / record / preview re-opens it. */
    void setReleaseIdleDevice (bool on);
    bool getReleaseIdleDevice() const { return releaseIdleDevice.load (std::memory_order_acquire); }
    /** The idle release may have closed the device: bring it back before using it. */
    void ensureDeviceOpen();
    /** Same, for audition-style use (piano roll / musical typing): the reopen runs on a helper
        thread and the notes are parked until the device is back, so a slow driver (ASIO / WASAPI
        exclusive) can never stall the message thread. */
    void parkLiveNote (const juce::MidiMessage& m);
    void flushParkedLiveNotes();
    void openDeviceAsync();
    /** The audio settings dialog holds the device open while it is up. */
    std::atomic<bool> keepDeviceOpen { false };
    /** The piano roll auditions through the device; keep the driver open while that panel is
        visible so a key press never has to reopen an ASIO driver first. */
    void setPianoRollAuditioning (bool on)
    {
        pianoRollAuditioning.store (on, std::memory_order_release);
        deviceIdleSinceMs = 0;
    }
    bool isMasterMono() const { return masterMono.load(); }
    float getAutoVolNorm (int trackId) const;
    float getAutoPanNorm (int trackId) const;
    float getCpuLoad() const { return (float) deviceManager.getCpuUsage(); }
    // total delay of the signal path in samples (used for recording alignment and the self-test)
    int getPdcTotalSamples() const { return pdcTotalSamples.load(); }

    //== media pool preview
    // Auditioning a file (the media pool's play button): it plays through the same device, after
    // the tracks report and before the master chain, so it obeys the master fader - and it is
    // realtime only, so a mixdown never contains it.
    bool startPreview (const juce::File& file, juce::String& error);
    void stopPreview();
    bool isPreviewActive() const;
    /** 0..1 while a preview plays, -1 when nothing plays. Also false once the file ended. */
    double previewProgress() const;
    double previewPositionSeconds() const;

    //== MIDI learn
    // Controller moves arrive on the audio thread; they are queued and applied on the message
    // thread (30 Hz), which is where the model and the plug-in parameters may be touched.
    /** Applies one controller / button event to every matching mapping. Also consumes the event
        while a target is armed (that is what creates a mapping). Returns how many targets moved. */
    int applyController (int channel, int cc, int value, bool isNote = false);
    void setLearnTarget (const MidiTarget& t, const juce::String& label);
    void cancelLearn();
    bool isLearning() const { return learning; }
    juce::String learnLabel() const { return learningLabel; }
    /** Called on the message thread after a mapping was learned or a controller moved a target. */
    std::function<void()> onMidiMappingChanged;

    //== live input
    void setSelectedTrack (int id) { selectedTrack = id; }
    void noteOn (int note, int velocity);
    void noteOff (int note);
    void allLiveNotesOff();
    void auditionNote (int note, int velocity, int durationMs = 250);
    void sendLiveMessage (const juce::MidiMessage& m);

    //== plugins
    std::shared_ptr<PluginInstance> getInstance (juce::int64 uid) const;
    bool setInstrument (int trackId, const juce::PluginDescription& d, juce::String& error);
    /** Stacks another instrument on an instrument track: it receives the same MIDI and is mixed
        into the track (its own gain / pan / bypass) before the track's inserts. */
    bool addInstrument (int trackId, const juce::PluginDescription& d, juce::String& error);
    bool addInsert (int trackId, const juce::PluginDescription& d, juce::String& error); // trackId -1 = master
    /** Replaces a track's whole insert chain (master when trackId < 0) with 'chain'.

        Every plug-in is created before the old chain is touched, so a missing or broken plug-in
        leaves the current chain exactly as it was. The replacement is one undo step (the old
        plug-ins' states are captured by the checkpoint). Slot uids are regenerated; the desc,
        saved state, bypass, built-in flag, gain/pan and side-chain source come from 'chain'. */
    bool replaceInsertChain (int trackId, const std::vector<PluginSlot>& chain, juce::String& error);
    void removeSlot (int trackId, juce::int64 uid);
    void setSlotBypass (int trackId, juce::int64 uid, bool bypassed);
    void moveInsert (int trackId, int from, int to);
    // a plug-in reported another latency: re-align everything on the next block
    void pluginLatencyChanged() { requestSnapshotRebuild(); }
    std::function<void (juce::int64)> onSlotRemoved;
    std::function<void (const juce::String&)> onError;

    struct LastTouched { juce::int64 uid = 0; int index = -1; int trackId = -1; juce::String name; };
    LastTouched lastTouched;

    //== automation touch recording
    void touchBegin (int trackId, const juce::String& target);
    void touchValue (int trackId, const juce::String& target, float norm);
    /** For changes with no explicit gesture (wheel, external MIDI CC): starts a short-lived
        touch, records the point, and lets the timer commit it after the control stops moving. */
    void touchValueAuto (int trackId, const juce::String& target, float norm);
    void touchEnd (int trackId, const juce::String& target);
    void parameterGesture (PluginInstance& inst, int index, bool begin);
    void parameterChanged (PluginInstance& inst, int index, float value);
    juce::String describeParam (const PluginInstance& inst, int index) const;

    //== export
    bool exportAudio (const ExportOptions& opt, const std::function<bool (double)>& progress, juce::String& error);
    /** Real-time mixdown through the audio device (ExportOptions::realtime). */
    bool exportAudioRealtime (const ExportOptions& opt, const std::function<bool (double)>& progress, juce::String& error);
    // offline-render one track's output over a beat range into a WAV (used to merge audio clips
    // and to freeze a track). Runs safely off the message thread; 'progress' returns false to
    // cancel (the render then stops and the caller removes the file).
    bool renderTrackRangeToFile (int trackId, double startBeat, double endBeat, const juce::File& file,
                                 juce::String& error, bool flatFader = false, bool noPdcPad = false,
                                 bool bypassInserts = false, const std::function<bool (double)>& progress = {});
    /** Starts an offline render session on the message thread: detaches the audio callback,
        switches the plug-ins of 'trackIds' to non-realtime and prepares them once. The prepared
        render calls below may then run on a worker thread without ever calling back into the
        message thread - that is what makes cancelling safe (no deadlock with the progress UI).
        Returns false when another offline render is already running. */
    bool beginOfflineRender (const std::vector<int>& trackIds);
    void endOfflineRender();
    /** One track render inside a session started by beginOfflineRender(). 'snap' is built on the
        first call and reused for the following tracks, so freezing a multi-output instrument
        builds the project snapshot once instead of once per channel. */
    bool renderTrackRangeToFilePrepared (int trackId, double startBeat, double endBeat, const juce::File& file,
                                         juce::String& error, bool flatFader, bool noPdcPad, bool bypassInserts,
                                         const std::function<bool (double)>& progress,
                                         std::shared_ptr<Snap>& snap);
    /** Freezes several tracks in ONE render pass: a multi-output instrument and its output
        channels share the (expensive) instrument render instead of re-rendering it once per
        channel. 'files' receives one WAV per track id, in the same order. 'snap' is built on
        the first call and reused. */
    bool renderTracksRangeToFilesPrepared (const std::vector<int>& trackIds, double startBeat, double endBeat,
                                           const std::vector<juce::File>& files, juce::String& error,
                                           bool flatFader, bool noPdcPad, bool bypassInserts,
                                           const std::function<bool (double)>& progress,
                                           std::shared_ptr<Snap>& snap);

    //== project plumbing
    void syncAll();                   // after project load / undo
    void captureStates();
    // plug-in values for the undo stack (see Project::EditSnapshot): parameters are cheap and go
    // into every checkpoint, opaque state only for the plug-ins an edit is about to delete
    void captureParams (std::map<juce::int64, std::vector<float>>& out);
    void captureStatesFor (const std::set<juce::int64>& uids, std::map<juce::int64, juce::MemoryBlock>& out);
    void applyParams (const std::map<juce::int64, std::vector<float>>& values);

    // Project::Listener
    void projectContentChanged() override;
    void projectMixChanged() override;
    void projectStructureChanged() override;

    // AudioIODeviceCallback
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    // MidiInputCallback
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    // AudioPlayHead
    juce::Optional<PositionInfo> getPosition() const override;

private:
    struct RenderCtx
    {
        bool realtime = true;
        bool allowLoop = true;
        int onlyTrack = -1;        // >= 0: render just that track, ignoring mute/solo
        // >= 0 with onlyTrack: this track is an output channel; its source instrument track is
        // rendered as well (into its own buffer, but not into the mix) so the channel can be
        // frozen / exported on its own
        int onlyTrackSource = -1;
        // freeze of a whole instrument (parent + every output channel): ONE render pass runs
        // every track of *renderIds and captures each track of *printIds into its own file
        // (the frozen audio is read from the track's runtime buffer after the block). The old
        // single-track fields stay for the stem export, which prints one track per pass.
        const std::vector<int>* renderIds = nullptr;
        const std::vector<int>* printIds = nullptr;
        bool masterFx = true;
        bool ignoreMuteSolo = false;
        double sampleRate = 44100.0;
        bool flatFader = false;    // freeze renders: bake the insert chain, but keep the fader/pan neutral
        bool noPdcPad = false;     // freeze renders: do not pad this track for other tracks' latency
        bool bypassInserts = false;// freeze renders: leave the insert effects out of the render
    };

    struct Recorder
    {
        int trackId = 0;
        juce::File file;
        int firstChannel = 0, numChannels = 1;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        juce::int64 written = 0;
    };

    struct RecEvent { juce::int64 pos = 0; juce::uint8 data[3] {}; juce::uint8 len = 0; };

    void timerCallback() override;
    void handleAsyncUpdate() override;
    /** Requests a snapshot rebuild, but collapses bursts: a self-modulating plug-in (or a rapid
        automation write) can otherwise rebuild the whole snapshot dozens of times a second on the
        message thread, which is what made the menus and buttons feel unresponsive. */
    void requestSnapshotRebuild();

    std::shared_ptr<Snap> buildSnapshot();
    void publish (std::shared_ptr<Snap> s);
    void ensureRuntimes();
    void syncMixToRuntimes();
    void reconcilePlugins();
    // true when a track's insert chain holds an ARA bound plug-in: it renders the track's audio
    // itself, so the raw clips must not be played on top of it
    bool trackRendersAra (int trackId);
    // moves the ARA playback regions when the clips change (stopped transport only)
    void resyncAraDocuments();
    std::shared_ptr<PluginInstance> createInstance (PluginSlot& slot, int ownerTrack, juce::String& error);
    std::shared_ptr<PluginInstance> createInstanceImpl (PluginSlot& slot, int ownerTrack, juce::String& error);
    void configureBuses (juce::AudioProcessor& p, bool instrument);
    void prepareAll (double sr, int block, bool nonRealtime);
    void destroyInstancesNotIn (const std::set<juce::int64>& keep);

    void renderChunk (const Snap& s, const RenderCtx& ctx, juce::int64& pos, bool isPlaying, int n,
                      juce::AudioBuffer<float>& out, const juce::MidiBuffer* liveMidi, const float* const* inputs, int numIn);
    void feedRecorders (const float* const* inputs, int numIn, int n, juce::int64 pos);
    void finishRecording();
    void commitTouch (int trackId, const juce::String& target);
    juce::String laneLabel (Track& t, const juce::String& target) const;
    // recordings of an unsaved project land here; moving them into the project folder happens
    // automatically the first time the project is saved
    juce::File sessionRecordDir;
    // user-chosen base folder for those recordings (empty = the default session temp folder)
    juce::File recordBaseDir;

    double sampleRate = 44100.0;
    int deviceBlock = 512;
    std::atomic<bool> playing { false }, recording { false };
    std::atomic<juce::int64> posSamples { 0 };
    std::atomic<bool> flushNotes { false };
    // ruler/scrub seeks are handed to the audio thread; it fades the old position out and the new
    // one in so dragging the playhead never cuts a sample in half
    std::atomic<juce::int64> seekTargetSamples { 0 };
    std::atomic<bool> seekRequested { false };
    std::atomic<bool> seekFadeIn { false };
    std::atomic<int> selectedTrack { -1 };
    std::atomic<float> masterPeakL { 0.0f }, masterPeakR { 0.0f };
    std::atomic<float> masterRmsAccL { 0.0f }, masterRmsAccR { 0.0f };
    std::atomic<int> masterRmsCount { 0 };
    std::atomic<bool> masterClipped { false };
    // goniometer ring: the audio thread writes L/R pairs, the UI reads a copy
    static constexpr int kScopePairs = 512;
    std::array<std::atomic<float>, kScopePairs> scopeL {}, scopeR {};
    std::atomic<float> masterGain { 1.0f };
    std::atomic<bool> masterMute { false };   // the master mute wins over its fader curve
    std::atomic<bool> masterMono { false };   // fold the main output to mono (Studio One style)
public:
private:
    std::atomic<bool> metroOn { false };
    std::atomic<bool> anySolo { false };
    std::atomic<bool> anySoloSend { false };     // an effect-return (Send) track is explicitly soloed
    std::atomic<bool> anyListenSolo { false };   // any track monitored through the listen bus
    juce::int64 recordFrom = 0;
    // loop state captured when recording started, so a loop take can be split back into passes
    bool recLooping = false;
    double recLoopStartBeat = 0.0, recLoopEndBeat = 0.0;
    juce::int64 recLoopPassSamples = 0;
    double lastBpm = 120.0;

    // snapshot hand-over
    juce::SpinLock snapLock;
    std::shared_ptr<Snap> pending;
    std::atomic<std::shared_ptr<Snap>> audioSnap; // render-thread safe
    std::atomic<bool> hasPending { false };
    std::shared_ptr<Snap> editSnap; // last snapshot built on the message thread

    std::map<int, std::shared_ptr<TrackRuntime>> runtimes;
    // send delay lines, keyed by (track id, send index): the message thread creates / reuses them
    // while building a snapshot, and every snapshot keeps the ones it uses alive
    std::map<std::pair<int, int>, std::shared_ptr<SendDelayState>> sendDelays;
    std::map<juce::int64, std::shared_ptr<PluginInstance>> instances;

    // render state
    juce::AudioBuffer<float> mixBuf { 2, TrackRuntime::kMaxBlock };
    juce::AudioBuffer<float> metroBuf { 2, TrackRuntime::kMaxBlock };
    juce::MidiBuffer liveBuf, masterMidi;
    juce::MidiMessageCollector liveCollector;

    //== MIDI learn: controller events queued by the audio thread, applied by the message thread
    struct CcEvent { int channel = 0, cc = 0, value = 0; bool isNote = false; };
    juce::AbstractFifo ccFifo { 64 };
    std::array<CcEvent, 64> ccData;
    std::atomic<bool> midiLearnOn { false };   // the audio thread only queues while this is set
    bool learning = false;
    MidiTarget learnTarget;
    juce::String learningLabel;
    void pumpControllerFifo();

    //== media pool preview
    juce::CriticalSection previewLock;      // held while the audio thread mixes / the UI swaps it
    std::unique_ptr<juce::AudioTransportSource> previewSource;
    std::unique_ptr<juce::AudioFormatReaderSource> previewReader;
    juce::TimeSliceThread previewThread { "SoundX preview" };
    juce::AudioBuffer<float> previewBuf { 2, TrackRuntime::kMaxBlock };
    std::atomic<bool> previewOn { false };
    double previewLength = 0.0;
    float lastMasterGain = 1.0f;
    bool lastMasterPhase = false;
    float lastListenGain = 1.0f;
    float metroPhase = 0.0f;
    int metroRemaining = 0;
    float metroFreq = 1000.0f;
    juce::int64 lastMetroBeat = -1000000;
    std::vector<float> metroDelayLine;  // PDC ring for the metronome, interleaved stereo
    int metroDelayWrite = 0;
    int metroPadApplied = -1, metroPadFade = 0;
    std::vector<float> masterDelayLine; // PDC ring for bypassed master inserts, interleaved stereo
    int masterDelayWrite = 0;
    int masterPadApplied = -1, masterPadFade = 0;
    std::atomic<int> pdcTotalSamples { 0 };  // shown/used for recording alignment
    // A transport jump (play / stop / seek / loop wrap) asks the render thread to drop every PDC
    // ring before the next block. Only the latch is set from the message thread; the rings are
    // ever only cleared on the thread that renders (audio callback / offline render).
    std::atomic<bool> pdcFlushPending { false };
    void requestPdcFlush() noexcept { pdcFlushPending.store (true, std::memory_order_release); }
    std::vector<TempoSegment> lastTempo;     // previous tempo map, for keeping the beat position on edits

    // play head info (valid inside renderChunk)
    struct PH { juce::int64 samples = 0; double bpm = 120; int num = 4, den = 4; bool playing = false, recording = false;
                bool looping = false; double loopStart = 0, loopEnd = 0;
                double ppq = 0, ppqBarStart = 0; } ph;

    // recording
    juce::TimeSliceThread writerThread { "SoundX audio writer" };
    juce::SpinLock recLock;
    std::vector<std::unique_ptr<Recorder>> recorders;
    std::vector<int> midiRecTracks;
    juce::AbstractFifo recFifo { 8192 };
    std::vector<RecEvent> recFifoData = std::vector<RecEvent> (8192);
    std::vector<RecEvent> recEvents;
    std::atomic<bool> midiRecording { false };
    double recordStartBeat = 0.0;

    // audition
    struct Audition { int note; double offAt; };
    std::vector<Audition> auditions;

    // touch recording
    struct TouchRec { int trackId = 0; juce::String target; double startBeat = 0.0; std::vector<AutoPoint> pts; bool recordPoints = false;
                      bool implicit = false; double lastMs = 0.0; };
    std::vector<TouchRec> touches;

    // an offline render (export / freeze) runs on a worker thread and reads the model + snapshot
    // sources; the message thread must not rebuild or mutate them meanwhile
    std::atomic<bool> exporting { false };
    std::atomic<bool> deferredAsyncUpdate { false };
    std::atomic<bool> deferredStructure { false };
    // plug-ins switched to non-realtime by beginOfflineRender(), and the flag that keeps the
    // re-attached device callback from re-preparing the whole project again
    std::set<juce::int64> offlinePreparedUids;
    std::atomic<bool> skipNextDevicePrepare { false };
    bool offlineRestorePrepare = false;   // the session changed the rate and must re-prepare

    // Studio One style real-time mixdown: the audio callback writes the master output into this
    // buffer while the transport runs; the export thread polls the position and writes the file.
    struct RealtimeCaptureState
    {
        std::shared_ptr<juce::AudioBuffer<float>> buffer, finished;
        std::atomic<juce::int64> pos { 0 };
        std::atomic<bool> active { false }, done { false };
        juce::SpinLock lock;
        juce::int64 total = 0;
        double rate = 44100.0;

        void prepare (juce::int64 samples);
        void finish();
    };
    RealtimeCaptureState rtCapture;

    void releaseDeviceIfIdle();
    bool deviceIsBusy() const;
    std::atomic<bool> releaseIdleDevice { true };
    juce::int64 deviceIdleSinceMs = 0;
    int lastPreparedRate = 0;                 // sample rate prepareAll last used
    bool deviceReleased = false;
    // the idle close runs on a helper thread: this lock serializes it against re-opening the
    // device (play / record / preview / audio settings)
    juce::CriticalSection deviceToggleLock;
    std::thread deviceCloseThread;
    std::atomic<bool> deviceClosePending { false };
    // the async reopen used by auditioning (see openDeviceAsync); serialize the parked notes
    std::thread deviceOpenThread;
    std::atomic<bool> deviceOpenPending { false };
    juce::CriticalSection parkedLiveLock;
    std::vector<juce::MidiMessage> parkedLiveNotes;

    // Live MIDI (piano-roll keys, musical typing, MIDI input) keeps the driver open for a short
    // grace period. Without it the idle release closed the device while the user was auditioning,
    // and every new key reopened an ASIO driver (a visible/audible stutter).
    std::atomic<double> lastLiveMidiMs { 0.0 };
    // Note-ons minus note-offs: a held piano / MIDI note keeps its instrument awake and the
    // driver open even after the "last message" grace has passed.
    std::atomic<int> liveNotesHeld { 0 };
    std::atomic<bool> pianoRollAuditioning { false };
    // When the transport stopped. Instrument tracks with no live input are skipped on the audio
    // thread after a short tail grace period, so a big sampler does not burn a core on silence.
    std::atomic<double> stoppedSinceMs { 0.0 };

    juce::CriticalSection exportLock;
    //== multi-core render pool ---------------------------------------------------------------
    RenderWorkerPool renderPool;
    std::atomic<bool> multiCore { true };
    std::atomic<bool> lastRenderParallel { false };
    int renderWorkers = 0;
    int renderCoresSetting = 0;    // 0 = auto, else the core count the user picked
    void startRenderPool();

    //== snapshot rebuild throttle ============================================================
    // Coalesces high-frequency change notifications into at most one full snapshot rebuild per
    // debounce window. The flag / timestamp are atomic because a plug-in can report a latency
    // change (or a gesture) from its own thread, while the rebuild itself stays on the message
    // thread (Engine::timerCallback).
    std::atomic<bool> snapshotRebuildPending { false };
    std::atomic<juce::uint32> snapshotRebuildRequestMs { 0 };
    static constexpr juce::uint32 kSnapshotDebounceMs = 60;
    // Plug-in parameter gestures: a real knob drag fires gesture-begin once, but a self-modulating
    // plug-in can fire begin/end pairs dozens of times a second. Each begin used to serialise the
    // whole project to XML for the undo stack; now consecutive gestures on the same parameter
    // inside this window share one checkpoint.
    std::map<std::pair<juce::int64, int>, double> lastGestureCheckpointMs;
    static constexpr double kGestureCheckpointGapMs = 500.0;
};
}
