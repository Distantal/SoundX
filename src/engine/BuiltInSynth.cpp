#include "BuiltInSynth.h"

namespace sx
{
using namespace juce;

namespace
{
    struct SynthSound : public SynthesiserSound
    {
        bool appliesToNote (int) override { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    class SynthVoice : public SynthesiserVoice
    {
    public:
        explicit SynthVoice (BuiltInSynth::Shared& s) : sh (s) {}

        bool canPlaySound (SynthesiserSound* s) override { return dynamic_cast<SynthSound*> (s) != nullptr; }

        void startNote (int midiNote, float velocity, SynthesiserSound*, int) override
        {
            freq = MidiMessage::getMidiNoteInHertz (midiNote);
            vel = velocity;
            phase1 = phase2 = 0.0;
            lp1 = lp2 = 0.0f;
            adsr.setSampleRate (getSampleRate());
            adsr.setParameters ({ sh.attack->get(), sh.decay->get(), sh.sustain->get(), sh.release->get() });
            adsr.noteOn();
        }

        void stopNote (float, bool allowTailOff) override
        {
            if (allowTailOff) { adsr.setParameters ({ sh.attack->get(), sh.decay->get(), sh.sustain->get(), sh.release->get() }); adsr.noteOff(); }
            else { adsr.reset(); clearCurrentNote(); }
        }

        void pitchWheelMoved (int) override {}
        void controllerMoved (int, int) override {}

        void renderNextBlock (AudioBuffer<float>& out, int start, int num) override
        {
            if (! adsr.isActive()) { clearCurrentNote(); return; }

            const double sr = getSampleRate();
            const int wave = sh.wave->getIndex();
            const double bendSemis = sh.bend.load() * sh.bendRange.load();
            const double det = sh.detune->get() / 1200.0;
            const double f1 = freq * std::pow (2.0, bendSemis / 12.0 - det * 0.5);
            const double f2 = freq * std::pow (2.0, bendSemis / 12.0 + det * 0.5);
            const double inc1 = f1 / sr, inc2 = f2 / sr;

            float cutoff = sh.cutoff->get();
            cutoff *= std::pow (2.0f, sh.mod.load() * 2.0f);
            cutoff = jlimit (30.0f, (float) (sr * 0.45), cutoff);
            const float g = std::tan (MathConstants<float>::pi * cutoff / (float) sr);
            const float k = 2.0f - 1.9f * sh.reso->get();
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;

            const float level = sh.gain->get() * vel * sh.vol.load() * sh.expr.load() * 0.35f;

            for (int i = 0; i < num; ++i)
            {
                const float s = (osc (wave, phase1) + osc (wave, phase2)) * 0.5f;
                phase1 += inc1; if (phase1 >= 1.0) phase1 -= 1.0;
                phase2 += inc2; if (phase2 >= 1.0) phase2 -= 1.0;

                const float v3 = s - lp2;
                const float v1 = a1 * lp1 + a2 * v3;
                const float v2 = lp2 + a2 * lp1 + a3 * v3;
                lp1 = 2.0f * v1 - lp1;
                lp2 = 2.0f * v2 - lp2;

                const float o = v2 * adsr.getNextSample() * level;
                for (int c = 0; c < out.getNumChannels(); ++c)
                    out.addSample (c, start + i, o);
            }

            if (! adsr.isActive()) clearCurrentNote();
        }

    private:
        static float osc (int wave, double p)
        {
            const float x = (float) p;
            switch (wave)
            {
                case 0: return 2.0f * x - 1.0f;
                case 1: return x < 0.5f ? 0.8f : -0.8f;
                case 2: return 4.0f * std::abs (x - 0.5f) - 1.0f;
                default: return std::sin (MathConstants<float>::twoPi * x);
            }
        }

        BuiltInSynth::Shared& sh;
        ADSR adsr;
        double freq = 440.0, phase1 = 0.0, phase2 = 0.0;
        float vel = 1.0f, lp1 = 0.0f, lp2 = 0.0f;
    };
}

BuiltInSynth::BuiltInSynth()
    : AudioProcessor (BusesProperties().withOutput ("Output", AudioChannelSet::stereo(), true)
                                      .withOutput ("Aux", AudioChannelSet::stereo(), true))
{
    addParameter (shared.wave = new AudioParameterChoice (ParameterID { "wave", 1 }, "Waveform", StringArray { "Saw", "Square", "Triangle", "Sine" }, 0));
    addParameter (shared.cutoff = new AudioParameterFloat (ParameterID { "cutoff", 1 }, "Cutoff", NormalisableRange<float> (40.0f, 18000.0f, 0.0f, 0.3f), 5000.0f));
    addParameter (shared.reso = new AudioParameterFloat (ParameterID { "reso", 1 }, "Resonance", 0.0f, 0.95f, 0.15f));
    addParameter (shared.detune = new AudioParameterFloat (ParameterID { "detune", 1 }, "Detune (cents)", 0.0f, 50.0f, 8.0f));
    addParameter (shared.attack = new AudioParameterFloat (ParameterID { "attack", 1 }, "Attack", NormalisableRange<float> (0.001f, 4.0f, 0.0f, 0.4f), 0.005f));
    addParameter (shared.decay = new AudioParameterFloat (ParameterID { "decay", 1 }, "Decay", NormalisableRange<float> (0.001f, 4.0f, 0.0f, 0.4f), 0.3f));
    addParameter (shared.sustain = new AudioParameterFloat (ParameterID { "sustain", 1 }, "Sustain", 0.0f, 1.0f, 0.7f));
    addParameter (shared.release = new AudioParameterFloat (ParameterID { "release", 1 }, "Release", NormalisableRange<float> (0.005f, 6.0f, 0.0f, 0.4f), 0.25f));
    addParameter (shared.gain = new AudioParameterFloat (ParameterID { "gain", 1 }, "Gain", 0.0f, 1.5f, 1.0f));

    synth.addSound (new SynthSound());
    for (int i = 0; i < 24; ++i)
        synth.addVoice (new SynthVoice (shared));
}

void BuiltInSynth::prepareToPlay (double sr, int)
{
    synth.setCurrentPlaybackSampleRate (sr);
}

void BuiltInSynth::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    buffer.clear();
    for (auto meta : midi)
    {
        auto m = meta.getMessage();
        if (m.isPitchWheel()) shared.bend = (m.getPitchWheelValue() - 8192) / 8192.0f;
        else if (m.isController())
        {
            const int num = m.getControllerNumber();
            const int raw = m.getControllerValue();
            const float v = raw / 127.0f;
            switch (num)
            {
                case 101: rpnMsb = raw; break;
                case 100: rpnLsb = raw; break;
                case 6:
                    if (rpnMsb == 0 && rpnLsb == 0)
                        shared.bendRange.store (jlimit (1.0f, 24.0f, (float) raw));
                    break;
                case 1:  shared.mod = v; break;
                case 7:  shared.vol = v; break;
                case 11: shared.expr = v; break;
                case 74: shared.mod = v; break;
                default: break;
            }
        }
    }
    synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());

    // the aux bus mirrors the main output: a second stereo out the host can route to its own
    // mixer channel (or run through its own insert chain), independent of the main pair
    if (buffer.getNumChannels() >= 4)
        for (int c = 2; c < jmin (4, buffer.getNumChannels()); ++c)
            buffer.copyFrom (c, 0, buffer, c - 2, 0, buffer.getNumSamples());
}

void BuiltInSynth::getStateInformation (MemoryBlock& dest)
{
    XmlElement e ("SXSynth");
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
            e.setAttribute (rp->getParameterID(), (double) rp->getValue());
    copyXmlToBinary (e, dest);
}

void BuiltInSynth::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
        for (auto* p : getParameters())
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p))
                if (x->hasAttribute (rp->getParameterID()))
                    rp->setValueNotifyingHost ((float) x->getDoubleAttribute (rp->getParameterID()));
}
}
