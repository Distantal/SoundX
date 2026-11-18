#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>

namespace sx
{
/** True for Ctrl+Z (undo) and Ctrl+Shift+Z / Ctrl+Y (redo); 'redo' tells the two apart. */
bool undoRedoKey (const juce::KeyPress& key, bool& redo);

/** Ctrl+Z / Ctrl+Y (Cmd on macOS) for the windows that are not the main window.

    MainComponent::keyPressed owns these shortcuts for the arrangement, but a plug-in editor or
    the plug-in manager is a top level window of its own, so a shortcut typed there never reaches
    the main window's component tree and would do nothing at all - which is exactly the "I tweaked
    the plug-in and Ctrl+Z does not undo it" complaint.

    Two mechanisms cover the two cases:
      - UndoKeyListener attaches to a JUCE window and catches the keys that reach our components,
      - PluginKeyRouter catches the keys Windows posts straight to a plug-in's own native editor
        window (there is no way to see those from JUCE).
*/
class UndoKeyListener : public juce::KeyListener
{
public:
    explicit UndoKeyListener (std::function<void (bool redo)> callback);

    bool keyPressed (const juce::KeyPress& key, juce::Component* originatingComponent) override;

    std::function<void (bool redo)> onUndoRedo;
};

#if JUCE_WINDOWS
/** Routes Ctrl+Z / Ctrl+Y typed into a plug-in's own editor window to the application.

    A plug-in's editor is a native window with its own window procedure, so its keystrokes are
    never seen by any JUCE component. Windows does post them through this application's UI thread
    message queue, and a thread local message hook sees them just before they are dispatched:
    that is the only way to make the shortcut work while a plug-in has the keyboard focus.

    The hook is only installed while at least one plug-in editor is open, it only reacts while one
    of those windows is in front, and it keeps its hands off text fields (a plug-in's preset name
    box still gets its own Ctrl+Z).
*/
class PluginKeyRouter
{
public:
    static PluginKeyRouter& get();

    /** The application's undo / redo (called on the message thread). */
    void setCallback (std::function<void (bool redo)> callback);
    /** Host shortcuts (Space, F-keys, ...) typed into a native plug-in editor. Return true when
        the key was consumed by the host. */
    void setGlobalKeyCallback (std::function<bool (const juce::KeyPress&)> callback);

    /** Registers / unregisters the native window of a plug-in editor. */
    void addWindow (void* nativeWindow);
    void removeWindow (void* nativeWindow);

    /** True when Ctrl+Z / Ctrl+Y in this message should be taken over by the application. */
    bool consumeMessage (void* nativeMessage);

private:
    PluginKeyRouter() = default;
    ~PluginKeyRouter();
    void updateHook();

    std::function<void (bool redo)> onUndoRedo;
    std::function<bool (const juce::KeyPress&)> onGlobalKey;
    std::set<void*> windows;
    void* hook = nullptr;

    JUCE_DECLARE_NON_COPYABLE (PluginKeyRouter)
};
#endif
}
