#pragma once

#include "engine/Engine.h"

namespace sx
{
class PluginWindows
{
public:
    explicit PluginWindows (Engine& e);
    ~PluginWindows();

    void show (juce::int64 uid, const juce::String& title);
    /** Like show(), but on the next message-loop pass. Inserting a plug-in uses this so the new
        slot is laid out and painted before a slow plug-in editor is created; the message-loop
        callback is weak-reference safe, so closing the app in between is harmless. */
    void showAsync (juce::int64 uid, const juce::String& title);
    void close (juce::int64 uid);      // deferred, safe to call from the window itself
    void closeNow (juce::int64 uid);
    void closeAll();
    /** Single-panel mode hides the other windows when a new one is opened; multiple-panel mode
        leaves them all on screen. Set from the AppState setting (View > Plug-in Panels). */
    void setMultipleWindowsAllowed (bool allowed);
    bool multipleWindowsAllowed() const { return multipleWindows; }
    // repaint every open window (the title bar follows the light / dark theme)
    void refreshAll();
    bool isOpen (juce::int64 uid) const;
    bool isVisible (juce::int64 uid) const;
    /** True while at least one plug-in panel is on screen (the host must not steal the keyboard
        focus back from a plug-in's own native popup menus while this is the case). */
    bool hasVisibleWindow() const;
    // bounds of an open plug-in window (empty when it is not open): used by the self test
    // to check that windows open centred on screen
    juce::Rectangle<int> windowBounds (juce::int64 uid) const;
    // the window component itself (self test: snapshot what the title bar icons look like)
    juce::Component* windowComponent (juce::int64 uid) const;
    /** Self test: fire one of the title bar icons (0 power, 1 side-chain, 2 bypass). */
    void testTriggerIcon (juce::int64 uid, int icon);

    /** Called by the main component once: the plug-in panels become child components of this
        host, so they live inside the SoundX window instead of creating native top-level windows
        (one taskbar entry, and the arrangement keeps its keyboard focus). */
    void setHost (juce::Component* hostComponent);
    /** Re-clamps every panel after the host was resized. */
    void hostResized();
    juce::Component* hostComponent() const { return host; }
    juce::Rectangle<int> hostBounds() const;
    /** The editor component of an open panel (self test / host layout checks). */
    juce::Component* windowContent (juce::int64 uid) const;

    /** Host-wide shortcuts (space, Ctrl+..., F-keys) typed into a plug-in panel are handed to the
        main component here. Return true when the shortcut was consumed. */
    std::function<bool (const juce::KeyPress&)> onGlobalKey;

    /** Undo / redo typed into a plug-in window (the arrangement owns the shortcut otherwise). */
    std::function<void (bool redo)> onUndoRedo;

    /** The window toolbar asks the app for these: bypass a slot (saved) and pick its side-chain. */
    std::function<void (int trackId, juce::int64 uid, bool bypass)> onSetSlotBypass;
    std::function<void (juce::int64 uid, juce::Component& target)> showSidechainMenu;

private:
    class Window;
    void attachToHost (Window& w);
    void hideOthers (juce::int64 keepUid);
    /** Closed panels keep their editor alive (bounded LRU) so reopening a heavy plug-in is instant. */
    void purgeOldClosedWindows();
    Engine& engine;
    // Standalone use (self test, or before the main component hands over its layer): a virtual
    // screen-sized host keeps the panels as ordinary child components, not native windows.
    juce::Component fallbackHost;
    juce::Component* host = nullptr;
    std::vector<std::unique_ptr<Window>> windows;
    bool multipleWindows = false;
    juce::int64 lastShownUid = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (PluginWindows)
};
}
