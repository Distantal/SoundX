#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sx
{
/** Removes the thin accent-coloured frame that Windows 11 draws around the focused window.

    The app paints its own title bar, but the DWM still draws a system border on top of it (the
    blue line around the whole window). Setting DWMWA_BORDER_COLOR to the "none" colour gets rid
    of it; the call is a no-op on older Windows versions and on macOS / Linux.
*/
void removePlatformWindowBorder (juce::Component& component);
}
