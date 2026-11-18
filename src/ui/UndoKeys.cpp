/*
  ==============================================================================

   Undo / redo shortcuts for the windows around the main one (plug-in editors and the plug-in
   manager). See UndoKeys.h for why this needs two mechanisms.

  ==============================================================================
*/

#include "UndoKeys.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <set>
#endif

namespace sx
{
using namespace juce;

namespace
{
    // Windows reports Ctrl+letter as the control character (Ctrl+Z = 26) with no text character,
    // exactly like the main window's key handler has to cope with: turn it back into its letter
    juce_wchar letterOf (const KeyPress& k)
    {
        const auto text = (juce_wchar) CharacterFunctions::toLowerCase (k.getTextCharacter());
        if (text != 0) return text;

        const int code = k.getKeyCode();
        if (code >= 'A' && code <= 'Z') return (juce_wchar) (code - 'A' + 'a');
        if (code >= 'a' && code <= 'z') return (juce_wchar) code;
        if (code >= 1 && code <= 26) return (juce_wchar) ('a' + code - 1);
        if (code >= 32 && code < 127) return (juce_wchar) code;
        return 0;
    }

    /** Ctrl+Z (undo), Ctrl+Shift+Z / Ctrl+Y (redo). */
    bool isUndoRedoKey (const KeyPress& k, bool& redo)
    {
        const auto mods = k.getModifiers();
        if (! mods.isCtrlDown() && ! mods.isCommandDown()) return false;

        const auto letter = letterOf (k);
        if (letter == 'z') { redo = mods.isShiftDown(); return true; }
        if (letter == 'y') { redo = true; return true; }
        return false;
    }
}

bool undoRedoKey (const KeyPress& key, bool& redo)
{
    return isUndoRedoKey (key, redo);
}

//==============================================================================
UndoKeyListener::UndoKeyListener (std::function<void (bool)> callback)
    : onUndoRedo (std::move (callback))
{
}

bool UndoKeyListener::keyPressed (const KeyPress& key, Component*)
{
    bool redo = false;
    if (! isUndoRedoKey (key, redo)) return false;
    if (onUndoRedo != nullptr) onUndoRedo (redo);
    return true;
}

#if JUCE_WINDOWS
//==============================================================================
namespace
{
    constexpr UINT kGetMessageHook = WH_GETMESSAGE;

    LRESULT CALLBACK pluginKeyHookProc (int code, WPARAM wp, LPARAM lp)
    {
        // Only react when the message is actually being taken out of the queue. The message pump
        // peeks first (PM_NOREMOVE) and then retrieves (PM_REMOVE), so handling both calls the
        // host twice for one key press - which turned one Space into "play, then stop again" and
        // made the transport look dead while a plug-in editor was open.
        if (code >= 0 && wp == PM_REMOVE)
            if (auto* msg = reinterpret_cast<MSG*> (lp))
                if (PluginKeyRouter::get().consumeMessage (msg))
                    return 1;    // do not dispatch it: the plug-in must not act on it as well

        return CallNextHookEx (nullptr, code, wp, lp);
    }
}

PluginKeyRouter& PluginKeyRouter::get()
{
    static PluginKeyRouter router;
    return router;
}

PluginKeyRouter::~PluginKeyRouter()
{
    windows.clear();          // never call back into a destroyed application
    updateHook();
}

void PluginKeyRouter::setCallback (std::function<void (bool)> callback)
{
    onUndoRedo = std::move (callback);
}

void PluginKeyRouter::setGlobalKeyCallback (std::function<bool (const KeyPress&)> callback)
{
    onGlobalKey = std::move (callback);
}

void PluginKeyRouter::addWindow (void* nativeWindow)
{
    if (nativeWindow == nullptr) return;
    windows.insert (nativeWindow);
    updateHook();
}

void PluginKeyRouter::removeWindow (void* nativeWindow)
{
    windows.erase (nativeWindow);
    updateHook();
}

void PluginKeyRouter::updateHook()
{
    if (! windows.empty() && hook == nullptr)
        hook = (void*) SetWindowsHookExW (kGetMessageHook, pluginKeyHookProc, nullptr, GetCurrentThreadId());

    if (windows.empty() && hook != nullptr)
    {
        UnhookWindowsHookEx ((HHOOK) hook);
        hook = nullptr;
    }
}

bool PluginKeyRouter::consumeMessage (void* nativeMessage)
{
    // with no application callback there is nothing to route the key to, so the plug-in keeps it
    if (windows.empty() || nativeMessage == nullptr
        || (onUndoRedo == nullptr && onGlobalKey == nullptr)) return false;

    auto& msg = *reinterpret_cast<MSG*> (nativeMessage);
    if (msg.message != WM_KEYDOWN) return false;

    const bool ctrl = (GetKeyState (VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;

    // A native panel is a WS_CHILD: the foreground window is the main window, so identify the
    // plug-in by walking the focus window's parent chain up to one of our panels.
    HWND focus = GetFocus();
    bool inPluginPanel = false;
    for (HWND h = focus; h != nullptr; h = GetParent (h))
        if (windows.count ((void*) h) != 0) { inPluginPanel = true; break; }
    if (! inPluginPanel) return false;

    // ...and only while the host window that owns that panel is in front
    HWND root = GetAncestor (focus, GA_ROOT);
    if (root == nullptr || root != GetForegroundWindow()) return false;

    // a plug-in's text field (preset name, search box) keeps its own Space / Ctrl+Z
    if (focus != nullptr)
    {
        wchar_t cls[64] = {};
        if (GetClassNameW (focus, cls, (int) numElementsInArray (cls)) > 0)
        {
            const String name (cls);
            if (name.equalsIgnoreCase ("Edit") || name.startsWithIgnoreCase ("RichEdit"))
                return false;
        }
    }

    // Space / transport / F-keys: hand the key to the host without letting the plug-in also act
    // on it. The callback runs on the message thread; the hook only decides to consume it.
    const bool hostShortcut = (msg.wParam == VK_SPACE && ! ctrl && ! alt)
                           || msg.wParam == VK_F2 || msg.wParam == VK_F4 || msg.wParam == VK_F11
                           || (msg.wParam == VK_RETURN && alt);
    if (hostShortcut && onGlobalKey != nullptr)
    {
        const auto callback = onGlobalKey;
        int flags = 0;
        if (ctrl)  flags |= ModifierKeys::ctrlModifier;
        if (shift) flags |= ModifierKeys::shiftModifier;
        if (alt)   flags |= ModifierKeys::altModifier;
        KeyPress k ((int) msg.wParam, ModifierKeys (flags), 0);
        MessageManager::callAsync ([callback, k] { if (callback != nullptr) callback (k); });
        return true;
    }

    const bool isZ = msg.wParam == 'Z';
    const bool isY = msg.wParam == 'Y';
    if (! ctrl || alt || (! isZ && ! isY) || onUndoRedo == nullptr) return false;

    if (onUndoRedo != nullptr)
    {
        const bool redo = isY || shift;
        const auto callback = onUndoRedo;
        // never run the undo from inside the message hook: the window that owns the plug-in is
        // in the middle of dispatching a key, and an undo may close (or reload) that very plug-in
        MessageManager::callAsync ([callback, redo] { callback (redo); });
    }

    return true;
}
#endif
}
