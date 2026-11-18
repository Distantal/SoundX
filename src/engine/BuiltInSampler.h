#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace sx
{
/** SX Sampler: the built-in sampling instrument. It loads one audio file (WAV / AIFF / FLAC / MP3)
    into memory and plays it from the MIDI keyboard with up to 16 voices, linear interpolation, a
    root note, a simple attack / release envelope, a velocity sensitivity and an optional forward
    loop over a region of the sample.

    Like every other built-in it is a plain juce::AudioProcessor, so it travels through the same
    insert / automation / state code as a hosted VST: its parameters are real AudioParameters (the
    host's automation and undo capture reads getParameters()) and its state is XML with the sample
    path in it.
*/
class BuiltInSampler : public juce::AudioProcessor
{
public:
    BuiltInSampler();
    ~BuiltInSampler() override;

    //== AudioProcessor =======================================================================
    const juce::String getName() const override { return "SX Sampler"; }
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        // an instrument: one stereo output and nothing to record
        if (l.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
        if (! l.getMainInputChannelSet().isDisabled()) return false;
        return l.outputBuses.size() <= 1;
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int size) override;

    //== sample ===============================================================================
    /** Loads an audio file into memory. A missing / unreadable file (and an unsupported or empty
        one) leaves whatever was loaded before in place and puts a readable message in `error`. */
    bool loadSample (const juce::File& file, juce::String& error);
    void clearSample();

    //== read-only view (the editor and the self-test use these) ==============================
    juce::String getSamplePath() const;
    juce::String getSampleName() const;
    double getSampleLengthSeconds() const;
    double getSourceSampleRate() const;
    int getLoadedChannels() const;
    int getSampleLengthSamples() const;
    int getActiveVoiceCount() const { return activeVoices.load (std::memory_order_relaxed); }
    double getRootNote() const;
    int getSampleGeneration() const { return sampleGeneration.load (std::memory_order_relaxed); }

    /** The cached min / max envelope of the loaded sample: 2 floats (min, max) per bucket. */
    std::vector<float> copyWaveformPeaks() const;

    //== the host parameters (the editor attaches to these) ===================================
    juce::AudioParameterInt*   rootNoteParam  = nullptr;   // MIDI note the sample plays at
    juce::AudioParameterFloat* gainParam      = nullptr;   // dB
    juce::AudioParameterFloat* attackParam    = nullptr;   // ms
    juce::AudioParameterFloat* releaseParam   = nullptr;   // ms
    juce::AudioParameterFloat* velocityParam  = nullptr;   // % of velocity sensitivity
    juce::AudioParameterBool*  loopParam      = nullptr;   // Off / Forward
    juce::AudioParameterFloat* loopStartParam = nullptr;   // 0 .. 1 fraction of the sample
    juce::AudioParameterFloat* loopEndParam   = nullptr;   // 0 .. 1 fraction of the sample

    static constexpr int kNumVoices   = 16;
    static constexpr int kPeakBuckets = 1024;

private:
    struct Sample
    {
        juce::AudioBuffer<float> buffer;              // always 2 channels (mono is duplicated)
        juce::String path, name;
        double sourceSampleRate = 44100.0;
        double lengthSeconds = 0.0;
        int sourceChannels = 0;                       // as in the file (mono / stereo / more)
        int lengthSamples = 0;
        std::vector<float> peaks;                     // min / max pairs, see kPeakBuckets
    };

    struct Voice
    {
        enum class Stage { attack, sustain, release };

        bool active = false;
        bool sustained = false;       // note-off arrived while the sustain pedal was down
        int note = 60, channel = 1;
        float velocity = 1.0f;        // 0 .. 1
        double pos = 0.0;             // playhead in source samples
        float env = 0.0f;             // linear envelope, 0 .. 1
        float attackInc = 1.0f, releaseDec = 1.0f;
        Stage stage = Stage::attack;
        juce::uint64 age = 0;         // voice stealing: the oldest one goes first
    };

    struct BlockParams
    {
        double sr = 44100.0, rootNote = 60.0, fileRatio = 1.0;
        float gain = 1.0f, velSens = 0.6f;
        bool loop = false;
        double loopStart = 0.0, loopEnd = 0.0;   // in source samples
    };

    void buildPeaks (Sample& s) const;
    void handleMidi (const juce::MidiMessage& m, double sr);
    void startVoice (int note, int channel, float velocity, double sr);
    void releaseVoice (int note, int channel, double sr);
    void beginRelease (Voice& v, double sr);
    void releaseAllVoices (double sr);
    void stopAllVoices();
    void renderVoices (juce::AudioBuffer<float>& out, int startSample, int numSamples,
                       const Sample* s, const BlockParams& p);

    juce::AudioFormatManager formatManager;
    juce::CriticalSection loadLock;       // serialises loadSample / setStateInformation
    mutable juce::SpinLock sampleLock;    // guards `live` against the audio thread
    std::unique_ptr<Sample> live;
    std::atomic<int> sampleGeneration { 0 }, activeVoices { 0 };

    double currentSampleRate = 44100.0;
    std::array<Voice, kNumVoices> voices;
    juce::uint64 voiceAge = 0;
    float pitchBendSemitones = 0.0f;      // audio thread only
    bool sustainPedal = false;            // audio thread only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BuiltInSampler)
};
}
