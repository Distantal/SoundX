#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace sx
{
class AppState;

/** Folder holding the user's plug-in chain presets (next to settings and plug-in list). */
juce::File chainPresetFolder();

struct ChainPresetEntry
{
    juce::String name;
    juce::File file;
};

/** All readable *.sxchain files, sorted by display name. Malformed files are ignored. */
std::vector<ChainPresetEntry> listChainPresets();

/** Saves the insert chain of a track (trackId < 0 = master) under a display name. */
bool saveChainPreset (AppState& app, int trackId, const juce::String& name, juce::String& error);

/** Same, to an explicit file (used by the self test and for portable presets). */
bool writeChainPreset (AppState& app, int trackId, const juce::File& file,
                       const juce::String& name, juce::String& error);

/** Replaces the insert chain of a track (trackId < 0 = master) with a saved preset.
    The replacement is undoable as one step; if a plug-in cannot be loaded the old chain stays. */
bool applyChainPreset (AppState& app, int trackId, const juce::File& file, juce::String& error);

/** Adds "Save Plug-in Chain Preset..." and a "Load Plug-in Chain Preset" submenu to a menu.
    The items use lambda actions, so the caller does not need any new menu ids. */
void addChainPresetMenuItems (juce::PopupMenu& menu, AppState& app, int trackId);

/** Asks for a name and writes the current insert chain to the chain preset folder. */
void showSaveChainPresetDialog (AppState& app, int trackId);
}
