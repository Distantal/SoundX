#include "Model.h"

// WAV tempo metadata: writes (and reads) an "acid" chunk so a loop carries its tempo with it.
// The chunk is the standard one used by loop libraries: type, root note, number of beats,
// meter and tempo. RIFF readers that do not know it simply skip it.
namespace sx
{
using namespace juce;

namespace
{
    constexpr uint32 kAcidBody = 24;   // bytes of the chunk payload (excluding id + size)

    void put32 (std::vector<uint8>& v, uint32 x) { for (int i = 0; i < 4; ++i) v.push_back ((uint8) ((x >> (8 * i)) & 0xffu)); }
    void put16 (std::vector<uint8>& v, uint16 x) { v.push_back ((uint8) (x & 0xffu)); v.push_back ((uint8) ((x >> 8) & 0xffu)); }
    void putF32 (std::vector<uint8>& v, float f) { uint32 x; std::memcpy (&x, &f, 4); put32 (v, x); }

    uint32 get32 (const uint8* p) { return (uint32) p[0] | ((uint32) p[1] << 8) | ((uint32) p[2] << 16) | ((uint32) p[3] << 24); }
    uint16 get16 (const uint8* p) { return (uint16) ((uint16) p[0] | ((uint16) p[1] << 8)); }
    float getF32 (const uint8* p) { const uint32 x = get32 (p); float f; std::memcpy (&f, &x, 4); return f; }

    bool idIs (const uint8* p, const char* id) { return std::memcmp (p, id, 4) == 0; }
}

bool writeWavTempo (const File& file, double bpm, double beats)
{
    if (bpm <= 1.0 || bpm > 1000.0 || beats <= 0.0) return false;
    if (! file.existsAsFile() || ! file.hasFileExtension ("wav")) return false;

    auto in = file.createInputStream();
    if (in == nullptr) return false;
    std::vector<uint8> data ((size_t) in->getTotalLength());
    if (data.size() < 12 || in->read (data.data(), (int) data.size()) != (int) data.size()) return false;
    if (! idIs (data.data(), "RIFF") || ! idIs (data.data() + 8, "WAVE")) return false;

    // keep every chunk except a previous acid one (so re-tagging replaces the tempo)
    std::vector<uint8> out;
    out.reserve (data.size() + 32);
    out.insert (out.end(), data.begin(), data.begin() + 12);
    size_t pos = 12;
    while (pos + 8 <= data.size())
    {
        const uint32 size = get32 (data.data() + pos + 4);
        size_t total = 8 + (size_t) size;
        if (total < 8) break;
        if (pos + total > data.size()) total = data.size() - pos;   // truncated file: copy the rest
        const bool acid = idIs (data.data() + pos, "acid");
        if (! acid) out.insert (out.end(), data.begin() + (long long) pos, data.begin() + (long long) (pos + total));
        pos += total;
        if (total & 1) pos = jmin (pos + 1, data.size());           // chunks are word aligned
    }

    // append the new acid chunk
    std::vector<uint8> chunk;
    chunk.insert (chunk.end(), { 'a', 'c', 'i', 'd' });
    put32 (chunk, kAcidBody);
    put32 (chunk, 1);                                  // 1 = loop
    put16 (chunk, 60);                                 // root note (C3), informational
    put16 (chunk, 0);
    put32 (chunk, 0);
    put32 (chunk, (uint32) jlimit (1.0, 100000.0, std::round (beats)));   // number of beats
    put16 (chunk, 4);                                  // meter numerator (informational)
    put16 (chunk, 0);
    putF32 (chunk, (float) bpm);
    out.insert (out.end(), chunk.begin(), chunk.end());

    // fix up the RIFF size
    const uint32 riffSize = (uint32) (out.size() - 8);
    out[4] = (uint8) (riffSize & 0xffu);
    out[5] = (uint8) ((riffSize >> 8) & 0xffu);
    out[6] = (uint8) ((riffSize >> 16) & 0xffu);
    out[7] = (uint8) ((riffSize >> 24) & 0xffu);

    TemporaryFile tmp (file);
    if (! tmp.getFile().replaceWithData (out.data(), (int) out.size())) return false;
    return tmp.overwriteTargetFileWithTemporary();
}

bool readWavTempo (const File& file, double& bpm, double& beats)
{
    if (! file.existsAsFile()) return false;
    auto in = file.createInputStream();
    if (in == nullptr) return false;
    std::vector<uint8> data ((size_t) in->getTotalLength());
    if (data.size() < 12 || in->read (data.data(), (int) data.size()) != (int) data.size()) return false;
    if (! idIs (data.data(), "RIFF") || ! idIs (data.data() + 8, "WAVE")) return false;

    size_t pos = 12;
    while (pos + 8 <= data.size())
    {
        const uint32 size = get32 (data.data() + pos + 4);
        const size_t total = 8 + (size_t) size;
        if (total < 8 || pos + total > data.size()) break;
        if (idIs (data.data() + pos, "acid") && size >= kAcidBody)
        {
            const uint8* b = data.data() + pos + 8;
            beats = (double) get32 (b + 12);   // type(4) root(2) unk(2) unk(4) beats(4) meter(2+2) tempo(4)
            bpm = (double) getF32 (b + 20);
            return bpm > 1.0;
        }
        pos += total + (total & 1);
    }
    return false;
}
}
