#pragma once

#include "BuiltInEffects.h"

#include <array>

namespace sx
{
//==============================================================================
/** One direct-form biquad. The coefficients are written by the helpers in GuitarRig.cpp (RBJ
    cookbook); the state is what lets the filter run from one block to the next. */
struct RigBiquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    inline float process (float x) noexcept
    {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
    void reset() noexcept { x1 = x2 = y1 = y2 = 0.0f; }
};

//==============================================================================
/** SX Guitar Rig: a pedalboard, four amp heads, four cabinets and a time block, in the order of a
    real rig (pedals, then amp, cabinet and time effects). The heads and cabinets are built from the
    same stages as the originals: input coupling, gain stages with asymmetric clipping, a tone stack,
    power-amp sag and speaker resonances. They are modelled voicings, not captured samples. The chain
    runs at twice the project rate so the clipping does not alias, and the latency is reported to the
    engine like any other plug-in latency. Every module is an ordinary parameter, so automation,
    project state and undo work as they do for the other built-in effects. */
class SxGuitarRig : public SxEffectBase
{
public:
    enum Amp { Blackface = 0, Jcm800, Plexi, Rectifier, numAmps };
    enum Cab { FenderTwin = 0, Greenback4x12, Vintage4x12, Open1x12, numCabs };

    /** Every parameter, in signal-flow order. The spec table in GuitarRig.cpp follows the same order. */
    enum Param : int
    {
        // pedalboard
        pCompOn, pCompSustain, pCompAttack, pCompLevel,
        pBoostOn, pBoostGain, pBoostTreble, pBoostLevel,
        pTsOn, pTsDrive, pTsTone, pTsLevel,
        pOdOn, pOdDrive, pOdTone, pOdLevel,
        pFuzzOn, pFuzzDrive, pFuzzTone, pFuzzLevel,
        // time block: reverb, delay, chorus, tremolo
        pRevOn, pRevMix, pRevDecay, pRevTone,
        pDlyOn, pDlyTime, pDlyFeedback, pDlyMix,
        pChoOn, pChoRate, pChoDepth, pChoMix,
        pTremOn, pTremRate, pTremDepth,
        // amp heads
        pAmp,
        pFChannel, pFNVolume, pFNTreble, pFNMiddle, pFNBass, pFNBright,
        pFVVolume, pFVTreble, pFVMiddle, pFVBass, pFVReverb, pFVSpeed, pFVIntensity, pFVBright,
        pJPreamp, pJPresence, pJBass, pJMiddle, pJTreble, pJMaster, pJHighSens,
        pPVolume1, pPVolume2, pPJump, pPPresence, pPBass, pPMiddle, pPTreble,
        pMChannel,
        pM1Gain, pM1Master, pM1Presence, pM1Bass, pM1Middle, pM1Treble, pM1Crush,
        pM2Gain, pM2Master, pM2Presence, pM2Bass, pM2Middle, pM2Treble, pM2Crush,
        pMWatt,
        // cabinet and output
        pCab, pMicOff, pOutput,
        // pedal order: the five pedals in the order they run (0 comp, 1 boost, 2 TS808, 3 yellow OD, 4 fuzz)
        pSlot0, pSlot1, pSlot2, pSlot3, pSlot4,
        // module bypass (pedals and time effects already have their own On switches) and IR cab
        pAmpOn, pCabOn, pIrOn, pIrMix,
        kNumParams
    };

    SxGuitarRig();
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay (double sr, int block) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    float gainReductionDb() const override { return compReductionDb.load(); }

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** Load a cabinet impulse response (wav / aiff / flac). The file is used as the cabinet in
        place of the modelled speakers when IR is switched on. */
    bool loadIRFile (const juce::File& file, juce::String& error);
    void clearIR();
    bool hasIR() const noexcept { return irReady.load(); }
    juce::String irName() const { return irFileName; }

    /** The five pedals in the order they run now. A slot list that is not a permutation falls back
        to the default order, so a bad automation or state never drops or doubles a pedal. */
    std::array<int, 5> pedalOrderNow() const;

    /** The largest output level since the last call; the editor's meter reads it. */
    float takeOutputPeak() noexcept { return peakOut.exchange (0.0f); }

private:
    static constexpr int kMaxChunk = 4096;

    struct AmpVoicing
    {
        int stages = 2;
        float stageGain[3] { 1.0f, 1.0f, 1.0f };
        float stageBias[3] { 0.0f, 0.0f, 0.0f };
        float inputHz = 90.0f;
        float sag = 0.1f, powerDrive = 1.0f, outGain = 1.0f;
        bool vibrato = false;
        float vibratoDepth = 0.0f, vibratoHz = 0.0f;
        float springMix = 0.0f;
    };

    void renderChunk (float* l, float* r, int n);
    void applyPedal (int id, float* l, float* r, int n);
    void compStage (float* l, float* r, int n);
    void boostStage (int c, float* x, int n);
    void tubeScreamerStage (int c, float* x, int n);
    void yellowOdStage (int c, float* x, int n);
    void fuzzStage (int c, float* x, int n);
    void setupAmp();
    void ampStage (int c, float* x, int n);
    void springStage (float* l, float* r, int n);
    void setupCab();
    void cabStage (int c, float* x, int n);
    void timeStage (float* l, float* r, int n);

    double sampleRateInner = 88200.0;
    juce::dsp::Oversampling<float> oversampling { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };

    // pedals
    float compEnv = 0.0f;
    RigBiquad boostTone[2];
    RigBiquad tsHp[2], tsMid[2], tsLp[2];
    RigBiquad odHp[2], odMid[2], odLp[2];
    RigBiquad fzHp[2], fzLp[2], fzOut[2];
    std::atomic<float> compReductionDb { 0.0f }, peakOut { 0.0f };

    // amp head
    AmpVoicing amp;
    float ampDcK = 0.01f;
    RigBiquad ampIn[2], ampBass[2], ampMid[2], ampTreble[2], ampPresence[2];
    float ampDc[2][3] {}, ampSag[2] {}, ampLfo[2] {};

    // cabinet
    RigBiquad cabHp[2], cabRes[2], cabHonk[2], cabMic[2], cabLp[2];

    // cabinet IR: convolved at the project rate, after the 2x oversampled amp / cab chain
    juce::dsp::Convolution irConvolution;
    juce::AudioBuffer<float> irDry;
    std::atomic<bool> irReady { false };
    juce::String irPath, irFileName;
    bool loadIRPath (const juce::String& path, juce::String& error);

    // time block
    float tremPhase = 0.0f, chorusPhase = 0.0f, delayLp[2] {};
    std::vector<float> chorusRing[2], delayRing[2];
    int chorusSize = 0, chorusWrite = 0, delaySize = 0, delayWrite = 0;
    juce::Reverb roomReverb, ampSpring;
};
}
