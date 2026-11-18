#include "Chord.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace sx::chord
{
using namespace juce;

namespace
{
    // chord symbols are spelled the way musicians write them: sharps for C#/F#, flats for Eb/Ab/Bb
    const char* const kPreferredNames[12] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    const char* const kSharpNames[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    int pcOf (int value) { return ((value % 12) + 12) % 12; }

    // A..G -> pitch class, anything else -1
    int letterToPitch (juce_wchar c)
    {
        switch (c)
        {
            case 'C': case 'c': return 0;
            case 'D': case 'd': return 2;
            case 'E': case 'e': return 4;
            case 'F': case 'f': return 5;
            case 'G': case 'g': return 7;
            case 'A': case 'a': return 9;
            case 'B': case 'b': return 11;
            default: return -1;
        }
    }

    // reads a bare note name ("F#", "Bb", "c") and returns its pitch class (-1 when it is not one)
    int noteNameToPitch (const String& text)
    {
        const auto t = text.trim();
        if (t.isEmpty()) return -1;
        int pc = letterToPitch (t[0]);
        if (pc < 0) return -1;
        for (int i = 1; i < t.length(); ++i)
        {
            const juce_wchar c = t[i];
            if (c == '#' || c == 0x266f) ++pc;                    // '#' or the musical sharp sign
            else if (c == 'b' || c == 0x266d) --pc;               // flat (lower case b)
            else return -1;
        }
        return pcOf (pc);
    }

    // Rewrites the unicode chord shorthand (∆, Δ7, °, ø, ø7, ♭, ♯) into plain text. It walks the
    // string one character at a time, because the symbols are single characters there while their
    // replacements are several.
    String foldUnicode (const String& in)
    {
        String out;
        for (int i = 0; i < in.length(); ++i)
        {
            const juce_wchar c = in[i];
            const bool withSeven = i + 1 < in.length() && in[i + 1] == '7';
            switch (c)
            {
                case 0x2206: case 0x0394:                    // increment / greek delta: maj7
                    out << "maj";
                    if (withSeven) { out << "7"; ++i; }
                    break;
                case 0x00b0: case 0x00ba:                    // degree / masculine ordinal: dim
                    out << "dim";
                    break;
                case 0x00f8: case 0x2205:                    // o with stroke / empty set: half dim
                    out << "m7b5";
                    if (withSeven) ++i;
                    break;
                case 0x266d: out << "b"; break;              // flat sign
                case 0x266f: out << "#"; break;              // sharp sign
                default: out << String::charToString (c); break;
            }
        }
        return out;
    }

    // Folds the many ways a chord suffix is written onto one canonical form:
    //   "", "maj", "m/M"/"min", "dim"/"°", "aug"/"+", "sus", "add", "7#5", "m7b5"/"ø" ...
    // A leading capital M means major ("CM7" = Cmaj7) while a lower case m means minor ("Cm7"),
    // which is why the case is resolved before the text is lower-cased.
    String canonSuffix (const String& suffix)
    {
        String s = foldUnicode (suffix.trim());
        if (s.isEmpty()) return {};

        const bool startsSpecial = s.startsWithIgnoreCase ("maj") || s.startsWithIgnoreCase ("min")
                                   || s.startsWithIgnoreCase ("dim") || s.startsWithIgnoreCase ("aug")
                                   || s.startsWithIgnoreCase ("sus") || s.startsWithIgnoreCase ("add")
                                   || s.startsWithIgnoreCase ("half") || s.startsWithIgnoreCase ("alt")
                                   || s.startsWithIgnoreCase ("mmaj") || s.startsWithIgnoreCase ("no");
        if (! startsSpecial && s.startsWithChar ('M'))
            s = "maj" + s.substring (1);

        s = s.removeCharacters ("()[] \t");
        s = s.toLowerCase();
        s = s.replace ("half-diminished", "m7b5").replace ("halfdiminished", "m7b5")
             .replace ("half-dim", "m7b5").replace ("halfdim", "m7b5")
             .replace ("diminished", "dim").replace ("augmented", "aug")
             .replace ("major", "maj").replace ("minor", "min")
             .replace ("m7-5", "m7b5")
             .replace ("7sus4", "7sus")
             .replace ("sus4", "sus");
        if (s.startsWithChar ('-')) s = "m" + s.substring (1);     // "C-7" = Cm7
        s = s.replace ("min", "m");                                // "min7" -> "m7"
        if (s.startsWith ("ma") && s.length() > 2 && ! s.startsWith ("maj")
            && CharacterFunctions::isDigit (s[2]))                 // "ma7" -> "maj7", but not "madd9"
            s = "maj" + s.substring (2);
        return s;
    }

    int qualityIndex (const String& suffix)
    {
        const auto& qs = qualities();
        for (int i = 0; i < (int) qs.size(); ++i)
            if (suffix == qs[(size_t) i].suffix) return i;
        return -1;
    }

    int qualityForSuffix (const String& suffix)
    {
        // every accepted way of writing a quality maps onto the suffix it is stored as, so the
        // table above stays the single place that defines a chord
        static const std::pair<const char*, const char*> aliases[]
        {
            { "", "" }, { "maj", "" }, { "^", "" }, { "M", "" },
            { "m", "m" }, { "-", "m" }, { "min", "m" },
            { "dim", "dim" }, { "o", "dim" },
            { "aug", "aug" }, { "+", "aug" },
            { "sus2", "sus2" },
            { "sus", "sus4" }, { "sus4", "sus4" },
            { "5", "5" },
            { "6", "6" },
            { "m6", "m6" },
            { "maj7", "maj7" }, { "ma7", "maj7" },
            { "7", "7" }, { "dom7", "7" },
            { "m7", "m7" },
            { "m7b5", "m7b5" },
            { "dim7", "dim7" },
            { "mmaj7", "mmaj7" }, { "mmaj", "mmaj7" },
            { "7#5", "7#5" }, { "aug7", "7#5" }, { "7+", "7#5" },
            { "7b5", "7b5" },
            { "7sus", "7sus4" },
            { "9", "9" },
            { "maj9", "maj9" },
            { "m9", "m9" },
            { "add9", "add9" }, { "9add", "add9" },
            { "madd9", "madd9" },
            { "6/9", "6/9" }, { "69", "6/9" },
            { "11", "11" },
            { "m11", "m11" },
            { "13", "13" },
            { "m13", "m13" },
            { "maj13", "maj13" },
            { "7b9", "7b9" },
            { "7#9", "7#9" },
            { "9sus", "9sus4" }, { "9sus4", "9sus4" }
        };
        const auto canon = canonSuffix (suffix);
        for (auto& a : aliases)
            if (canon == a.first) return qualityIndex (a.second);
        return -1;
    }

    void buildPitches (Parsed& p)
    {
        p.pitches.clear();
        if (p.quality < 0 || p.quality >= (int) qualities().size()) return;
        const auto& q = qualities()[(size_t) p.quality];
        for (int i = 0; i < q.notes && i < 6; ++i)
        {
            const int pc = pcOf (p.root + q.intervals[i]);
            if (std::find (p.pitches.begin(), p.pitches.end(), pc) == p.pitches.end())
                p.pitches.push_back (pc);
        }
    }
}

const std::vector<Quality>& qualities()
{
    // ordered by how common the symbol is: detection breaks near-ties in favour of an early entry
    static const std::vector<Quality> list
    {
        { "",        3, { 0, 4, 7 } },
        { "m",       3, { 0, 3, 7 } },
        { "7",       4, { 0, 4, 7, 10 } },
        { "m7",      4, { 0, 3, 7, 10 } },
        { "maj7",    4, { 0, 4, 7, 11 } },
        { "dim",     3, { 0, 3, 6 } },
        { "aug",     3, { 0, 4, 8 } },
        { "sus4",    3, { 0, 5, 7 } },
        { "sus2",    3, { 0, 2, 7 } },
        { "6",       4, { 0, 4, 7, 9 } },
        { "m6",      4, { 0, 3, 7, 9 } },
        { "m7b5",    4, { 0, 3, 6, 10 } },
        { "dim7",    4, { 0, 3, 6, 9 } },
        { "mmaj7",   4, { 0, 3, 7, 11 } },
        { "5",       2, { 0, 7 } },
        { "add9",    4, { 0, 2, 4, 7 } },
        { "9",       5, { 0, 2, 4, 7, 10 } },
        { "maj9",    5, { 0, 2, 4, 7, 11 } },
        { "m9",      5, { 0, 2, 3, 7, 10 } },
        { "7sus4",   4, { 0, 5, 7, 10 } },
        { "7#5",     4, { 0, 4, 8, 10 } },
        { "7b5",     4, { 0, 4, 6, 10 } },
        { "madd9",   4, { 0, 2, 3, 7 } },
        { "6/9",     5, { 0, 2, 4, 7, 9 } },
        { "11",      6, { 0, 2, 4, 5, 7, 10 } },
        { "m11",     6, { 0, 2, 3, 5, 7, 10 } },
        { "13",      6, { 0, 2, 4, 7, 9, 10 } },
        { "m13",     6, { 0, 2, 3, 7, 9, 10 } },
        { "maj13",   6, { 0, 2, 4, 7, 9, 11 } },
        { "7b9",     4, { 0, 1, 4, 7, 10 } },
        { "7#9",     4, { 0, 3, 4, 7, 10 } },
        { "9sus4",   5, { 0, 2, 5, 7, 10 } }
    };
    return list;
}

String pitchClassName (int pc, bool sharps)
{
    return String (sharps ? kSharpNames[pcOf (pc)] : kPreferredNames[pcOf (pc)]);
}

String pitchClassNameWithOctave (int midiNote)
{
    return pitchClassName (midiNote % 12, false) + String (midiNote / 12 - 1);
}

Parsed parse (const String& text)
{
    Parsed p;
    const auto t = text.trim();
    if (t.isEmpty()) return p;

    const int letter = letterToPitch (t[0]);
    if (letter < 0) return p;

    int i = 1;
    int acc = 0;
    while (i < t.length())
    {
        const juce_wchar c = t[i];
        if (c == '#' || c == 0x266f) { ++acc; ++i; }
        else if (c == 'b' || c == 0x266d) { --acc; ++i; }
        else break;
    }
    p.root = pcOf (letter + acc);
    p.bass = p.root;

    String rest = t.substring (i).trim();

    // slash bass ("C/E", "Am7/G"); "6/9" keeps its slash because "9" is not a note name
    const int slash = rest.lastIndexOfChar ('/');
    if (slash >= 0)
    {
        const auto candidate = rest.substring (slash + 1).trim();
        const int bassPc = noteNameToPitch (candidate);
        if (bassPc >= 0)
        {
            p.bass = bassPc;
            p.slashBass = true;
            rest = rest.substring (0, slash).trim();
        }
    }

    p.quality = qualityForSuffix (rest);
    p.valid = p.quality >= 0;
    if (p.valid) buildPitches (p);
    else p.pitches.clear();
    return p;
}

String name (int root, int quality, int bass)
{
    if (quality < 0 || quality >= (int) qualities().size()) return {};
    String n = pitchClassName (root) + qualities()[(size_t) quality].suffix;
    if (bass >= 0 && pcOf (bass) != pcOf (root)) n += "/" + pitchClassName (bass);
    return n;
}

std::vector<int> pitchClassesOf (const String& text)
{
    return parse (text).pitches;
}

Detection detect (const std::vector<int>& pitchClasses, int bassPitchClass)
{
    std::vector<std::pair<int, double>> weighted;
    weighted.reserve (pitchClasses.size());
    for (int pc : pitchClasses) weighted.push_back ({ pc, 1.0 });
    return detectWeighted (weighted, bassPitchClass);
}

Detection detectWeighted (const std::vector<std::pair<int, double>>& input, int bassPitchClass)
{
    Detection best;

    double w[12] = {};
    for (auto& n : input)
        if (n.first >= 0)
            w[pcOf (n.first)] = jmax (w[pcOf (n.first)], jmax (0.0, n.second));

    double mx = 0.0;
    for (double v : w) mx = jmax (mx, v);
    if (mx <= 0.0) return best;

    // passing tones and ornaments sound for a fraction of the window: they do not define the chord
    const double thresh = mx * 0.3;
    int present = 0;
    for (double v : w) if (v >= thresh) ++present;
    if (present < 2) return best;

    const auto& qs = qualities();
    double bestScore = -1.0e9;
    for (int root = 0; root < 12; ++root)
    {
        for (int qi = 0; qi < (int) qs.size(); ++qi)
        {
            const auto& q = qs[(size_t) qi];
            if (q.notes < 2) continue;

            bool tones[12] = {};
            int toneCount = 0;
            for (int k = 0; k < q.notes && k < 6; ++k)
            {
                const int pc = pcOf (root + q.intervals[k]);
                if (! tones[pc]) { tones[pc] = true; ++toneCount; }
            }

            int hits = 0, misses = 0, extras = 0;
            for (int pc = 0; pc < 12; ++pc)
            {
                const bool on = w[pc] >= thresh;
                if (tones[pc]) { if (on) ++hits; else ++misses; }
                else if (on) ++extras;
            }
            if (hits < 2) continue;

            double score = 2.0 * (double) hits - 1.4 * (double) misses - 2.2 * (double) extras;
            if (misses == 0 && extras == 0) score += 2.0;          // every note is a chord tone
            if (bassPitchClass >= 0)
            {
                if (pcOf (bassPitchClass) == root) score += 0.9;
                else if (tones[pcOf (bassPitchClass)]) score += 0.15;
            }
            score -= 0.01 * (double) qi;                            // stable tie-break: simpler name wins
            juce::ignoreUnused (toneCount);

            if (score > bestScore + 1.0e-9)
            {
                bestScore = score;
                best.valid = true;
                best.root = root;
                best.quality = qi;
                best.score = score;
            }
        }
    }

    if (! best.valid) return best;

    const auto& q = qs[(size_t) best.quality];
    best.pitches.clear();
    bool tones[12] = {};
    for (int k = 0; k < q.notes && k < 6; ++k)
    {
        const int pc = pcOf (best.root + q.intervals[k]);
        if (! tones[pc]) { tones[pc] = true; best.pitches.push_back (pc); }
    }
    const int bass = bassPitchClass >= 0 ? pcOf (bassPitchClass) : best.root;
    best.bass = (tones[bass] && bass != best.root) ? bass : best.root;
    best.name = name (best.root, best.quality, best.bass == best.root ? -1 : best.bass);
    return best;
}

Detection detectInWindow (const std::vector<TimedNote>& notes, double beat, double window)
{
    std::vector<std::pair<int, double>> weighted;
    int bassPc = -1;
    int bassPitch = 1000;

    if (window <= 1.0e-9) return {};
    for (auto& n : notes)
    {
        const double s = jmax (n.start, beat);
        const double e = jmin (n.start + n.length, beat + window);
        if (e - s <= 1.0e-9) continue;
        // a note that fills the window counts fully, a short passing one only in proportion
        const double wt = jlimit (0.0, 1.0, (e - s) / window);
        weighted.push_back ({ pcOf (n.pitch), wt });
        if (n.pitch < bassPitch) { bassPitch = n.pitch; bassPc = pcOf (n.pitch); }
    }
    return detectWeighted (weighted, bassPc);
}
}
