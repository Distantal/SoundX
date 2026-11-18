#include "PluginManager.h"
#include "PluginScanProcess.h"
#include "ui/PluginManagerWindow.h"
#include "ui/Theme.h"

namespace sx
{
using namespace juce;

File PluginManager::dataDirectory()
{
    // %APPDATA%\SoundX on Windows, ~/Library/Application Support/SoundX on macOS (where macOS
    // expects application data; ~/Library itself would be unconventional)
    auto base = File::getSpecialLocation (File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    auto d = base.getChildFile ("SoundX");
    d.createDirectory();
    return d;
}

File PluginManager::templateFile()
{
    return dataDirectory().getChildFile ("project-template.sxp");
}

PluginManager::PluginManager()
{
    formats.addFormat (new VST3PluginFormat());
   #if JUCE_PLUGINHOST_VST
    formats.addFormat (new VSTPluginFormat());
   #endif

    // Never load a plug-in binary inside the DAW process just to look at it: a scan that
    // crashes, hangs or pops up a window would take the whole session with it. The custom
    // scanner runs every scan in a throw-away helper process instead.
    auto processScanner = std::make_unique<OutOfProcessScanner>();
    outOfProcess = processScanner.get();          // the scan thread asks it to skip a file
    known.setCustomScanner (std::move (processScanner));

    PropertiesFile::Options o;
    o.applicationName = "SoundX";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    o.folderName = dataDirectory().getFullPathName();
    o.storageFormat = PropertiesFile::storeAsXML;
    props = std::make_unique<PropertiesFile> (o);

    // seed scan locations that JUCE does not search by default
   #if JUCE_PLUGINHOST_VST
    if (! props->containsKey ("lastPluginScanPath_VST"))
    {
        VSTPluginFormat vstFormat;
        FileSearchPath vst2 (vstFormat.getDefaultLocationsToSearch());
        auto common = File::getSpecialLocation (File::globalApplicationsDirectory).getChildFile ("Common Files");
        vst2.addIfNotAlreadyThere (common.getChildFile ("VST2"));
        vst2.addIfNotAlreadyThere (common.getChildFile ("VST"));
        auto env = SystemStats::getEnvironmentVariable ("VST_PATH", {});
        for (auto& p : StringArray::fromTokens (env, ";", "")) if (p.isNotEmpty()) vst2.addIfNotAlreadyThere (File (p));
        vst2.removeNonExistentPaths();
        props->setValue ("lastPluginScanPath_VST", vst2.toString());
    }
   #endif

    if (auto xml = parseXML (dataDirectory().getChildFile ("plugins.xml")))
        known.recreateFromXml (*xml);

    // scanning runs on this thread, never on the message thread: loading a plug-in takes long
    // enough to freeze the window (and get "not responding" from Windows)
    scanThread = std::make_unique<PluginScanner> (formats, known, *outOfProcess, props.get(), deadMansFile());
}

PluginManager::~PluginManager()
{
    // the window polls the scan, so it has to go first; then the scan thread can be stopped
    scannerWindow.reset();
    scanThread->cancel();
    scanThread.reset();
    save();
}

void PluginManager::save()
{
    if (auto xml = known.createXml())
        xml->writeTo (dataDirectory().getChildFile ("plugins.xml"));
    if (props != nullptr)
        props->saveIfNeeded();
}

std::unique_ptr<AudioPluginInstance> PluginManager::create (const PluginDescription& d, double sr, int bs, String& error)
{
    try
    {
        auto inst = formats.createPluginInstance (d, sr, bs, error);

        // JUCE only says "Unable to load VST-3 plug-in file". On macOS the loader knows exactly
        // why (other architecture, broken signature, hardened runtime without the library
        // validation entitlement - see installer/SoundX.entitlements), and that message is the
        // difference between a bug report and a guess.
        if (inst == nullptr)
            if (const auto why = describeBundleLoadProblem (d.fileOrIdentifier); why.isNotEmpty())
                error << "\n" << why;

        return inst;
    }
    catch (const std::exception& e)
    {
        error = TRANS ("Plug-in crashed during load: ") + String (e.what());
        return nullptr;
    }
    catch (...)
    {
        error = TRANS ("Plug-in crashed during load (unknown exception)");
        return nullptr;
    }
}

Array<PluginDescription> PluginManager::instruments() const
{
    Array<PluginDescription> r;
    for (auto& d : known.getTypes()) if (d.isInstrument) r.add (d);
    return r;
}

Array<PluginDescription> PluginManager::effects() const
{
    Array<PluginDescription> r;
    for (auto& d : known.getTypes()) if (! d.isInstrument) r.add (d);
    return r;
}

bool PluginManager::hasAraSupport()
{
   #if JUCE_PLUGINHOST_ARA
    return true;
   #else
    return false;
   #endif
}

String PluginManager::araSupportString()
{
   #if JUCE_PLUGINHOST_ARA
    return "ARA " + String (SOUNDX_ARA_SDK_VERSION);
   #else
    return {};
   #endif
}

int PluginManager::araCapableCount() const
{
    int n = 0;
    for (auto& d : known.getTypes()) if (isAraCapable (d)) ++n;
    return n;
}

void PluginManager::addMenuItems (PopupMenu& menu, const Array<PluginDescription>& list) const
{
    // one submenu per manufacturer, sorted by name, with ids that cannot be confused with the
    // slot menu commands (KnownPluginList would number them from 1, which collides with them)
    struct Entry { int index; String manufacturer, name; };
    std::vector<Entry> entries;
    entries.reserve ((size_t) list.size());
    for (int i = 0; i < list.size(); ++i)
    {
        auto& d = list.getReference (i);
        entries.push_back ({ i, d.manufacturerName.isEmpty() ? String ("Unknown") : d.manufacturerName, d.name });
    }
    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
    {
        if (a.manufacturer != b.manufacturer) return a.manufacturer.compareIgnoreCase (b.manufacturer) < 0;
        return a.name.compareIgnoreCase (b.name) < 0;
    });

    size_t i = 0;
    while (i < entries.size())
    {
        const auto manufacturer = entries[i].manufacturer;
        PopupMenu sub;
        while (i < entries.size() && entries[i].manufacturer == manufacturer)
        {
            sub.addItem (kPluginMenuBase + entries[i].index, entries[i].name);
            ++i;
        }
        menu.addSubMenu (manufacturer, sub);
    }
}

int PluginManager::indexFromMenuId (const Array<PluginDescription>& list, int resultId)
{
    const int idx = resultId - kPluginMenuBase;
    return (idx >= 0 && idx < list.size()) ? idx : -1;
}

PluginManagerWindow* PluginManager::showScannerWindow()
{
    if (scannerWindow == nullptr)
        scannerWindow = std::make_unique<PluginManagerWindow> (*this, [this]
        {
            save();
            if (scannerWindow != nullptr) scannerWindow->setVisible (false);
        });

    if (panelHost != nullptr)
    {
        // reparent into the main window's overlay: no native peer, no taskbar entry
        scannerWindow->removeFromDesktop();
        panelHost->addAndMakeVisible (*scannerWindow);
        const auto area = panelHost->getLocalBounds();
        const int w = juce::jmin (860, juce::jmax (620, area.getWidth() - 40));
        const int h = juce::jmin (540, juce::jmax (400, area.getHeight() - 40));
        scannerWindow->setBounds (area.getCentreX() - w / 2, area.getCentreY() - h / 2, w, h);
    }

    scannerWindow->setVisible (true);
    scannerWindow->toFront (true);
    return scannerWindow.get();
}

void PluginManager::refreshTheme()
{
    if (scannerWindow != nullptr) scannerWindow->refreshTheme();
}

void PluginManager::setPanelHost (juce::Component* host)
{
    panelHost = host;
    if (scannerWindow != nullptr && host != nullptr && scannerWindow->isVisible())
        showScannerWindow();
}
}
