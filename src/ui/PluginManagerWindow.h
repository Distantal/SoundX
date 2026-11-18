#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sx
{
class PluginManager;

/** The plug-in manager window: the list of scanned plug-ins plus the scan bar.

    The scan itself lives on PluginScanner's own thread. That is the point of this class: the
    window only reads progress counters and repaints, so loading a plug-in (or walking a folder
    with thousands of files) can never block the message thread. A blocked message thread is
    what makes Windows draw "not responding" in the title bar and offer to close the program.
*/
class PluginManagerWindow : public juce::DocumentWindow
{
public:
    PluginManagerWindow (PluginManager& pluginsToShow, std::function<void()> onClose);
    ~PluginManagerWindow() override;

    void closeButtonPressed() override;
    void activeWindowStatusChanged() override;

    /** The window is embedded in the main window's overlay (no native peer), so it never counts
        as the active top-level window and JUCE would leave the title-bar buttons disabled - a
        disabled close button silently swallows the click. This keeps them clickable. */
    void refreshTitleBarButtons();

    /** Scan one folder (recursively), and remember it for the next session unless told not to. */
    bool scanFolder (const juce::File& folder, bool rememberForNextSession = true);
    /** Scan every remembered / default plug-in folder. */
    bool scanEverything();
    /** Re-apply the light / dark palette while the window is open. */
    void refreshTheme();

private:
    class ListPanel;

    ListPanel* panel = nullptr;
    std::function<void()> closeCallback;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerWindow)
};
}
