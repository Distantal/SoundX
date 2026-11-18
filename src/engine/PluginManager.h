#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginScanner.h"

namespace sx
{
class PluginManagerWindow;
class OutOfProcessScanner;

class PluginManager
{
public:
    PluginManager();
    ~PluginManager();

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;

    static juce::File dataDirectory();
    /** The project template a new session starts from (stored next to the settings and the
        plug-in list). */
    static juce::File templateFile();
    juce::File deadMansFile() const { return dataDirectory().getChildFile ("scan.crash"); }
    juce::PropertiesFile* properties() { return props.get(); }

    /** The background scan thread driven by the plug-in manager window. */
    PluginScanner& scanner() { return *scanThread; }

    void save();
    std::unique_ptr<juce::AudioPluginInstance> create (const juce::PluginDescription& d, double sampleRate, int blockSize,
                                                       juce::String& error);

    juce::Array<juce::PluginDescription> instruments() const;
    juce::Array<juce::PluginDescription> effects() const;

    //-- ARA ------------------------------------------------------------------------------------
    /** True when this build was made with the ARA SDK (JUCE_PLUGINHOST_ARA). */
    static bool hasAraSupport();
    /** "ARA 2.2.0" when ARA is compiled in, an empty string otherwise. */
    static juce::String araSupportString();
    /** Whether a plug-in announces the ARA extension (ARA rides on top of VST3 / AU, so the flag
        arrives with the plug-in's class information during the scan). */
    static bool isAraCapable (const juce::PluginDescription& d) { return d.hasARAExtension; }
    /** How many of the scanned plug-ins are ARA-capable. */
    int araCapableCount() const;

    // Adds installed plug-ins to a menu; map the chosen id back with indexFromMenuId() using the same list.
    void addMenuItems (juce::PopupMenu& menu, const juce::Array<juce::PluginDescription>& list) const;
    static int indexFromMenuId (const juce::Array<juce::PluginDescription>& list, int resultId);

    /** Plug-in entries start here: the slot menu has its own small ids (1..30), the built-ins use
        1600..1663, the layers 1000..1511 and the side-chain sources start at 30000, so 20000 keeps
        the plug-ins clear of every other range. */
    static constexpr int kPluginMenuBase = 20000;

    PluginManagerWindow* showScannerWindow();
    /** Repaint the open plug-in manager with the current light / dark palette. */
    void refreshTheme();
    /** Main window hands in its transparent overlay layer: the manager then lives inside the
        host (no extra taskbar entry), like the plug-in panels. */
    void setPanelHost (juce::Component* host);
    juce::Component* panelHostComponent() const { return panelHost; }

private:
    std::unique_ptr<juce::PropertiesFile> props;
    OutOfProcessScanner* outOfProcess = nullptr;   // owned by known (setCustomScanner)
    std::unique_ptr<PluginScanner> scanThread;
    std::unique_ptr<PluginManagerWindow> scannerWindow;
    juce::Component* panelHost = nullptr;
};
}
