#pragma once

#include "AppState.h"
#include "engine/PluginManager.h"

namespace sx
{
//== plug-in picker (search) ===================================================================
// Inserting a plug-in used to mean walking the vendor submenu of the slot menu, which is
// unusable once a few hundred plug-ins are installed. The picker puts a search field in front of
// the list: type part of a name, manufacturer or category, press Enter.

/** The list a picker offers: the built-in synth first (instrument slots only), then every
    scanned plug-in of that kind. */
juce::Array<juce::PluginDescription> pluginPickerList (PluginManager& plugins, bool instrumentSlot);

/** Indexes into 'list' of the plug-ins that match every word of 'query', best match first.

    The ranks, best to worst: the name starts with the whole query, the name contains it, the
    name contains every word, the manufacturer contains every word, the rest of the description
    (category, descriptive name, format) contains them. An empty query matches everything,
    alphabetically. This is the picker's whole search behaviour, so it is what the self test
    checks without opening a window.
*/
/** The vendor folders of a list, "SoundX" (the built-in family) first, then the rest sorted. */
juce::StringArray pluginPickerVendors (const juce::Array<juce::PluginDescription>& list);

std::vector<int> searchPluginList (const juce::Array<juce::PluginDescription>& list, const juce::String& query);

/** The picker's panel without opening the dialog (self test / visual check). */
juce::Image renderPluginPickerPanel (const juce::Array<juce::PluginDescription>& list, bool instrumentSlot,
                                     const juce::String& query, bool anyScanned = true, bool replacing = false,
                                     bool multiWindows = false);

/** Opens the plug-in search. 'replaceUid' != 0 replaces that slot on the track, 0 adds a new one;
    trackId -1 targets the master bus. 'instrumentSlot' picks the instrument or the effect list. */
void showPluginPicker (AppState& app, int trackId, juce::int64 replaceUid, bool instrumentSlot);
/** Stack another instrument on a track (multiple instruments per track). */
void showAddInstrumentPicker (AppState& app, int trackId);
}
