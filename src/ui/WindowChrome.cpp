#include "WindowChrome.h"

#if JUCE_WINDOWS
 #include <dwmapi.h>
 #ifndef DWMWA_BORDER_COLOR
  #define DWMWA_BORDER_COLOR 34
 #endif
 #ifndef DWMWA_COLOR_NONE
  #define DWMWA_COLOR_NONE 0xFFFFFFFE
 #endif
#endif

namespace sx
{
void removePlatformWindowBorder (juce::Component& component)
{
   #if JUCE_WINDOWS
    if (auto* peer = component.getPeer())
        if (auto* handle = peer->getNativeHandle())
        {
            const DWORD noBorder = DWMWA_COLOR_NONE;
            DwmSetWindowAttribute ((HWND) handle, DWMWA_BORDER_COLOR,
                                   &noBorder, (DWORD) sizeof (noBorder));
        }
   #else
    juce::ignoreUnused (component);
   #endif
}
}
