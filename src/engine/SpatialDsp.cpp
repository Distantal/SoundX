#include "BuiltInEffects.h"

#include <algorithm>
#include <cstdint>

namespace sx
{
using namespace juce;

namespace
{
    // Every live SX Stereo registers here, so the tracks of a link group can find each other. The audio
    // thread only tries the lock: when the message thread holds it, the auto-spacing keeps its last value.
    struct SceneRegistry
    {
        SpinLock lock;
        std::vector<SxStereo*> members;
    };

    SceneRegistry& scene()
    {
        static SceneRegistry registry;
        return registry;
    }

    float wrapDegrees (float a)
    {
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }
}

SxStereo::SxStereo() : SxEffectBase ("SX Stereo", {
    { "width", "Width", 0.0f, 200.0f, 100.0f, " %" },
    { "bassmono", "Bass Mono", 0.0f, 400.0f, 0.0f, " Hz" },
    { "output", "Output", -12.0f, 12.0f, 0.0f, " dB" },
    { "az", "Azimuth", -180.0f, 180.0f, 0.0f, " deg" },
    { "dist", "Distance", 0.0f, 100.0f, 0.0f, " %" },
    { "room", "Room", 0.0f, 100.0f, 0.0f, " %" },
    { "spacing", "Spacing", 0.0f, 100.0f, 0.0f, " %" },
    { "group", "Link Group", 0.0f, 8.0f, 0.0f, "" },
}, 2.0)
{
    SpinLock::ScopedLockType locked (scene().lock);
    scene().members.push_back (this);
}

SxStereo::~SxStereo()
{
    SpinLock::ScopedLockType locked (scene().lock);
    auto& m = scene().members;
    m.erase (std::remove (m.begin(), m.end(), this), m.end());
}

void SxStereo::prepareToPlay (double sr, int block)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;
    for (auto* f : { &bassL, &bassR, &air })
        f->z[0] = f->z[1] = 0.0f;
    delayL = {};
    delayR = {};
    reverb.setSampleRate ((float) sampleRate);
    reverb.reset();
    reverbUsed = false;
    spaceOffset = 0.0f;
    wet.setSize (2, jmax (block, 512), false, true, true);
}

std::vector<SxStereo*> SxStereo::groupPeers() const
{
    std::vector<SxStereo*> out;
    const int grp = group();
    if (grp <= 0) return out;
    SpinLock::ScopedLockType locked (scene().lock);
    for (auto* o : scene().members)
        if (o != this && o->group() == grp) out.push_back (o);
    return out;
}

std::vector<SxStereo*> SxStereo::sceneMembers() const
{
    SpinLock::ScopedLockType locked (scene().lock);
    return scene().members;
}

void SxStereo::setParamGesture (int index, float value)
{
    if (auto* p = params[(size_t) index])
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}

void SxStereo::shiftGroup (float dAzimuth, float dDistance)
{
    for (auto* o : groupPeers())
    {
        o->setParamGesture (pAzimuth, wrapDegrees (o->param (pAzimuth) + dAzimuth));
        o->setParamGesture (pDistance, jlimit (0.0f, 100.0f, o->param (pDistance) + dDistance));
    }
}

void SxStereo::processBlock (AudioBuffer<float>& b, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    if (b.getNumChannels() < 2) return;
    const int n = b.getNumSamples();
    if (n <= 0) return;
    const float sr = (float) sampleRate;
    if (wet.getNumSamples() < n) wet.setSize (2, n, false, false, true);

    const float width = param (pWidth) / 100.0f;
    const float outGain = Decibels::decibelsToGain (param (pOutput));
    const float bassHz = param (pBass);
    const float baseAz = param (pAzimuth);
    const float dist = jlimit (0.0f, 1.0f, param (pDistance) / 100.0f);
    const float room = jlimit (0.0f, 1.0f, param (pRoom) / 100.0f);
    const float spacing = jlimit (0.0f, 1.0f, param (pSpacing) / 100.0f);
    const int grp = group();

    // Auto-spacing: loud tracks of the same group that sit near this one push it away. The neighbours'
    // base positions are used (not their placed ones), so the pushes settle instead of chasing each other.
    float push = 0.0f;
    if (grp > 0 && spacing > 0.0f)
    {
        SpinLock::ScopedTryLockType locked (scene().lock);
        if (locked.isLocked())
            for (auto* o : scene().members)
            {
                if (o == this || o->group() != grp) continue;
                const float d = wrapDegrees (baseAz - o->param (pAzimuth));
                const float weight = std::exp (-(d * d) / (35.0f * 35.0f))
                                   * jlimit (0.0f, 1.0f, (o->levelDb() + 55.0f) / 40.0f);
                // the sign is the side to push to; on an exact tie the lower address goes left, so the pair splits
                const float sign = std::abs (d) > 0.5f ? (d > 0.0f ? 1.0f : -1.0f)
                                                       : ((std::uintptr_t) this < (std::uintptr_t) o ? 1.0f : -1.0f);
                push += sign * weight;
            }
    }
    const float target = spacing * 45.0f * jlimit (-2.0f, 2.0f, push);
    spaceOffset += (target - spaceOffset) * (1.0f - std::exp (-(float) n / (0.2f * sr)));
    const float az = wrapDegrees (baseAz + spaceOffset);
    placedAz.store (az);

    // placement: the mid is panned with a gain of 1 at the centre (so the centre is transparent), the side
    // carries the stereo image; the far ear hears the sound a little later (up to 0.66 ms); behind the head
    // the sound is darker and a little quieter; far away it is quieter, darker and drier.
    const float rad = az * MathConstants<float>::pi / 180.0f;
    const float x = jlimit (-1.0f, 1.0f, std::sin (rad));
    const float front = std::cos (rad);
    const float sq2 = std::sqrt (2.0f);
    const float gL = sq2 * std::cos ((x + 1.0f) * MathConstants<float>::pi * 0.25f);
    const float gR = sq2 * std::sin ((x + 1.0f) * MathConstants<float>::pi * 0.25f);
    const float rear = jmax (0.0f, -front);
    const float gMid = (1.0f - 0.45f * dist) * (1.0f - 0.16f * rear);
    const float sideScale = width * (1.0f - 0.6f * dist);
    const float airHz = jlimit (800.0f, 20000.0f, 20000.0f * std::pow (0.2f, dist) * (1.0f - 0.7f * rear));
    const bool airOn = airHz < 19000.0f;
    air.a = airOn ? 1.0f - std::exp (-MathConstants<float>::twoPi * airHz / sr) : 0.0f;
    const float delayLSamples = jmax (0.0f, x) * 0.00066f * sr;
    const float delayRSamples = jmax (0.0f, -x) * 0.00066f * sr;
    const bool bassOn = bassHz > 1.0f;
    bassL.a = bassR.a = bassOn ? 1.0f - std::exp (-MathConstants<float>::twoPi * bassHz / sr) : 0.0f;

    // the room: a reverb fed from the mid in proportion to distance times room; once it has been used it
    // keeps running so that its tail fades out instead of stopping
    const float send = dist * room * 0.6f;
    if (send > 0.0f) reverbUsed = true;
    if (reverbUsed)
    {
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.25f + 0.6f * room;
        rp.damping = 0.4f;
        rp.wetLevel = 1.0f;
        rp.dryLevel = 0.0f;
        rp.width = 1.0f;
        rp.freezeMode = 0.0f;
        reverb.setParameters (rp);
    }

    float* l = b.getWritePointer (0);
    float* r = b.getWritePointer (1);
    float* wl = wet.getWritePointer (0);
    float* wr = wet.getWritePointer (1);
    double levelSum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float li = l[i], ri = r[i];
        levelSum += (double) li * li + (double) ri * ri;

        float mid = 0.5f * (li + ri);
        float side = 0.5f * (li - ri);
        if (bassOn) side -= bassL.low (0, side);
        side *= sideScale;
        if (airOn) mid = air.low (0, mid);

        float ol = gL * gMid * mid + side;
        float orr = gR * gMid * mid - side;
        delayL.write (ol);
        ol = delayL.read (delayLSamples);
        delayR.write (orr);
        orr = delayR.read (delayRSamples);
        l[i] = ol;
        r[i] = orr;
        wl[i] = send * mid;
        wr[i] = send * mid;
    }

    if (reverbUsed)
    {
        reverb.processStereo (wl, wr, n);
        for (int i = 0; i < n; ++i)
        {
            l[i] += wl[i];
            r[i] += wr[i];
        }
    }

    for (int i = 0; i < n; ++i)
    {
        l[i] *= outGain;
        r[i] *= outGain;
    }

    const float rms = (float) std::sqrt (levelSum / (2.0 * (double) n));
    level.store (Decibels::gainToDecibels (rms, -100.0f));
    pushHistory (rms);
    pushStereo (b.getMagnitude (0, 0, n), b.getMagnitude (1, 0, n));
}

} // namespace sx
