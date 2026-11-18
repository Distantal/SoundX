#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace sx
{
class BuiltInSynth : public juce::AudioProcessor
{
public:
    BuiltInSynth();

    const juce::String getName() const override { return "SX Synth"; }
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        // main out (stereo) plus the aux out that mirrors it - two outs are what makes the synth a
        // multi-output instrument, so the "output channels in the mixer" feature has something to
        // expand without a third-party sampler installed
        if (l.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
        if (! l.getMainInputChannelSet().isDisabled()) return false;
        return l.outputBuses.size() < 2 || l.outputBuses[1].isDisabled()
            || l.outputBuses[1] == juce::AudioChannelSet::stereo();
    }

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int size) override;

    struct Shared
    {
        std::atomic<float> bend { 0.0f }, mod { 0.0f }, vol { 1.0f }, expr { 1.0f };
        std::atomic<float> bendRange { 2.0f };   // RPN 0 pitch-bend range in semitones
        juce::AudioParameterChoice* wave = nullptr;
        juce::AudioParameterFloat *cutoff = nullptr, *reso = nullptr, *attack = nullptr, *decay = nullptr,
                                  *sustain = nullptr, *release = nullptr, *detune = nullptr, *gain = nullptr;
    };

private:
    juce::Synthesiser synth;
    Shared shared;
    int rpnMsb = -1, rpnLsb = -1;
};

// A minimal test plug-in: passes the input through with a fixed 960-sample delay and
// reports that latency. Used by the self-test to verify plug-in delay compensation.
class BuiltInDelay : public juce::AudioProcessor
{
public:
    static constexpr int kDelay = 960;

    BuiltInDelay() { initialize(); }
    const juce::String getName() const override { return "SX Test Delay"; }
    void prepareToPlay (double, int samplesPerBlock) override
    {
        initialize (samplesPerBlock);
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const int n = b.getNumSamples();
        const int nch = b.getNumChannels();
        if (ring.getNumSamples() < n || ring.getNumChannels() != nch) initialize (juce::jmax (128, n));
        for (int c = 0; c < nch; ++c)
        {
            auto* out = b.getWritePointer (c);
            auto* rin = ring.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                const int w = (write + i) % ringSize;
                rin[w] = out[i];
                out[i] = rin[(w - kDelay + ringSize) % ringSize];
            }
        }
        write = (write + n) % ringSize;
    }
    using juce::AudioProcessor::processBlock;
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
    }

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& dest) override { dest.setSize (4); }
    void setStateInformation (const void*, int) override {}

private:
    void initialize (int block = 512)
    {
        setLatencySamples (kDelay);
        ringSize = kDelay + juce::jmax (256, block) + 64;
        ring.setSize (2, ringSize);
        ring.clear();
        write = 0;
    }

    juce::AudioBuffer<float> ring;
    int ringSize = 0, write = 0;
};
}
