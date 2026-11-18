#pragma once

#include "AppState.h"

namespace sx
{
// uid == 0 means "empty slot": offer to load a new plug-in. trackId == -1 targets the master bus.
void showSlotMenu (AppState& app, int trackId, juce::int64 uid, bool instrumentSlot, juce::Component* anchor);

// instrument layer ids used by the slot menu. Plug-in menu items live in a completely different
// range (KnownPluginList uses a large base id), so the two must never be mixed up - a collision
// here is what silently swallowed "choose instrument" clicks.
bool isInstrumentLayerMenuId (int menuId);
/** True for the built-in effect entries of the "SoundX" folder in the slot menu. */
bool isBuiltInEffectMenuId (int menuId);
int builtInEffectMenuCount();

// automation menus. The trackId forms take a project track id (or 0 for the master bus row);
// the Track& forms let the caller hand in the master bus directly. asAutomationTrack creates a
// stand-alone automation row instead of an inline lane below the channel.
void showAutomationTargetMenu (AppState& app, int trackId, juce::Component* anchor, bool asAutomationTrack = false);
void showAutomationTargetMenu (AppState& app, Track& track, int trackId, juce::Component* anchor, bool asAutomationTrack = false);
// all automation lanes of the channel: tick = visible, check an item to show/hide its row
void showAutomationLanesMenu (AppState& app, int trackId, juce::Component* anchor);
void showAutomationLanesMenu (AppState& app, Track& track, int trackId, juce::Component* anchor);

// side-chain source picker for one built-in slot (uid): used by the editor toolbar button
void showSidechainMenuFor (AppState& app, juce::int64 uid, juce::Component& target);

void showError (const juce::String& message);
void showTrackMenu (AppState& app, int trackId, juce::Component* anchor, std::function<void()> onRename);
}
