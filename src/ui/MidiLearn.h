#pragma once

#include "AppState.h"

namespace sx
{
// MIDI learn: maps a hardware controller (CC knob / fader, or a button that sends a note) to a
// parameter. The window lists what is mapped, offers "learn" for each target, and deletes rows.
void showMidiLearnWindow (AppState& app);

// renders the panel without opening the window (self test / visual check)
juce::Image renderMidiLearnPanel (AppState& app);
}
