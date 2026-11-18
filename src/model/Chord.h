#pragma once

#include <juce_core/juce_core.h>
#include <utility>
#include <vector>

// Chord symbols: reading the text a user types on the chord track ("Cmaj7", "F#m7b5", "Bb/D"),
// writing names for chords that were detected from notes, and detecting the chord that sounds
// inside a time window.
//
// The chord track always keeps the text exactly as it was typed: a symbol that is not a chord
// ("riff", "fill", "x2") still shows up there, it just carries no harmony. Everything that does
// understand a symbol (note highlighting, detection comparisons) goes through parse().
namespace sx::chord
{
    //-- pitch classes ---------------------------------------------------------------------------
    // 0 = C, 1 = C#/Db, ... 11 = B. Chords are spelled with flats by default (Bb rather than A#),
    // because that is how the symbols people write on a chord track read.
    juce::String pitchClassName (int pc, bool sharps = false);
    juce::String pitchClassNameWithOctave (int midiNote);   // 60 -> "C4"

    //-- chord qualities -------------------------------------------------------------------------
    struct Quality
    {
        const char* suffix;     // display suffix, "" for a plain major triad
        int notes;              // how many entries of 'intervals' are used
        int intervals[6];       // pitch classes above the root, starting at 0
    };
    const std::vector<Quality>& qualities();

    //-- parsing ---------------------------------------------------------------------------------
    struct Parsed
    {
        bool valid = false;             // the text was understood as a chord
        int root = 0;                   // 0..11
        int bass = 0;                   // slash bass (== root when there is none)
        bool slashBass = false;
        int quality = -1;               // index into qualities(), -1 when unknown
        std::vector<int> pitches;       // pitch classes of the chord tones (empty when unknown)
    };
    Parsed parse (const juce::String& text);

    // "Cmaj7", "Am7", "C/E". 'bass' < 0 means "no slash chord"
    juce::String name (int root, int quality, int bass = -1);

    // pitch classes of a chord symbol, empty when the text is not understood
    std::vector<int> pitchClassesOf (const juce::String& text);

    //-- detection -------------------------------------------------------------------------------
    struct Detection
    {
        bool valid = false;
        int root = 0;
        int quality = -1;
        int bass = 0;                   // slash bass; == root when the root is in the bass
        double score = 0.0;
        juce::String name;              // "Cmaj7" / "Am7" / "C/E"; empty when nothing was found
        std::vector<int> pitches;
    };

    // 'bassPitchClass' (0..11, -1 = unknown) breaks the C major / A minor kind of tie: the root
    // that actually sits in the bass wins.
    Detection detect (const std::vector<int>& pitchClasses, int bassPitchClass = -1);
    // weighted variant: one weight per sounding pitch class (how long / how loud it sounds)
    Detection detectWeighted (const std::vector<std::pair<int, double>>& weightedPitchClasses,
                              int bassPitchClass = -1);

    // one note of the material being analysed, in absolute timeline beats
    struct TimedNote
    {
        int pitch = 60;
        double start = 0.0;
        double length = 1.0;
    };
    // detects the chord sounding in [beat, beat + window]
    Detection detectInWindow (const std::vector<TimedNote>& notes, double beat, double window);
}
