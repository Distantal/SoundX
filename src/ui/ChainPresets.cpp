#include "ChainPresets.h"
#include "AppState.h"
#include "Importer.h"       // promptText
#include "SlotMenus.h"      // showError
#include "engine/PluginManager.h"

#include <algorithm>

namespace sx
{
using namespace juce;

namespace
{
    struct WaitCursor
    {
        WaitCursor() { MouseCursor::showWaitCursor(); }
        ~WaitCursor() { MouseCursor::hideWaitCursor(); }
    };

    struct ChainItem
    {
        PluginSlot slot;
        String sidechainName;
    };

    bool readChainFile (const File& file, String& name, std::vector<ChainItem>& items, String& error)
    {
        items.clear();
        name.clear();

        if (! file.existsAsFile())
        {
            error = TRANS ("The preset file does not exist.");
            return false;
        }

        std::unique_ptr<XmlElement> root (XmlDocument::parse (file));
        if (root == nullptr || ! root->hasTagName ("SXChainPreset"))
        {
            error = TRANS ("This is not a SoundX plug-in chain preset.");
            return false;
        }

        name = root->getStringAttribute ("name", file.getFileNameWithoutExtension()).trim();
        if (name.isEmpty()) name = file.getFileNameWithoutExtension();

        for (auto* e : root->getChildIterator())
        {
            if (e == nullptr || ! e->hasTagName ("Insert")) continue;

            ChainItem item;
            item.slot.bypass = e->getBoolAttribute ("bypass");
            item.slot.builtIn = e->getBoolAttribute ("builtIn");
            item.slot.postFader = e->getBoolAttribute ("post");
            item.slot.gainDb = (float) e->getDoubleAttribute ("gainDb", 0.0);
            item.slot.pan = (float) jlimit (-1.0, 1.0, e->getDoubleAttribute ("pan", 0.0));
            item.sidechainName = e->getStringAttribute ("sidechain");

            if (auto* d = e->getChildByName ("PLUGIN"))
                if (! item.slot.desc.loadFromXml (*d)) continue;

            if (item.slot.desc.fileOrIdentifier.isEmpty())
                continue;

            if (auto* state = e->getChildByName ("STATE"))
            {
                auto base64 = state->getAllSubText().trim();
                if (base64.isNotEmpty())
                    item.slot.state.fromBase64Encoding (base64);
            }

            items.push_back (std::move (item));
        }

        if (items.empty())
        {
            error = TRANS ("The preset contains no plug-ins.");
            return false;
        }

        return true;
    }

    bool captureChain (AppState& app, int trackId, std::vector<ChainItem>& items, String& error)
    {
        items.clear();

        std::vector<PluginSlot>* source = nullptr;
        if (trackId < 0)
            source = &app.project.masterTrack.inserts;
        else if (auto* t = app.project.findTrack (trackId))
            source = &t->inserts;

        if (source == nullptr)
        {
            error = TRANS ("Track not found.");
            return false;
        }

        if (source->empty())
        {
            error = TRANS ("The plug-in chain is empty.");
            return false;
        }

        for (auto& slot : *source)
        {
            ChainItem item;
            item.slot = slot;
            item.slot.uid = 0;
            item.slot.araState.reset();      // tied to the audio clips of the source track

            // The live instance owns the current settings; the slot's state is only the snapshot
            // from the last save, so ask the plug-in for its state now.
            if (auto inst = app.engine.getInstance (slot.uid))
                if (inst->proc != nullptr)
                    try
                    {
                        item.slot.state.reset();
                        inst->proc->getStateInformation (item.slot.state);
                    }
                    catch (...)
                    {
                    }

            if (slot.sidechainTrack != 0)
                if (auto* sourceTrack = app.project.findTrack (slot.sidechainTrack))
                    item.sidechainName = sourceTrack->name;

            items.push_back (std::move (item));
        }

        return true;
    }

    bool writeChainFile (const std::vector<ChainItem>& items, const File& file,
                         const String& name, String& error)
    {
        if (items.empty())
        {
            error = TRANS ("The plug-in chain is empty.");
            return false;
        }

        XmlElement root ("SXChainPreset");
        root.setAttribute ("version", 1);
        root.setAttribute ("name", name);

        for (auto& item : items)
        {
            auto* e = root.createNewChildElement ("Insert");
            e->setAttribute ("bypass", item.slot.bypass);
            e->setAttribute ("builtIn", item.slot.builtIn);
            if (item.slot.postFader) e->setAttribute ("post", true);
            if (std::abs (item.slot.gainDb) > 1.0e-4) e->setAttribute ("gainDb", (double) item.slot.gainDb);
            if (std::abs (item.slot.pan) > 1.0e-4) e->setAttribute ("pan", (double) item.slot.pan);
            if (item.sidechainName.isNotEmpty()) e->setAttribute ("sidechain", item.sidechainName);

            if (auto desc = item.slot.desc.createXml())
                e->addChildElement (desc.release());

            if (item.slot.state.getSize() > 0)
                e->createNewChildElement ("STATE")->addTextElement (item.slot.state.toBase64Encoding());
        }

        file.getParentDirectory().createDirectory();
        if (! root.writeTo (file))
        {
            error = TRANS ("Could not write the plug-in chain preset.");
            return false;
        }

        return true;
    }
}

//==============================================================================
File chainPresetFolder()
{
    auto folder = PluginManager::dataDirectory().getChildFile ("Chain Presets");
    folder.createDirectory();
    return folder;
}

std::vector<ChainPresetEntry> listChainPresets()
{
    std::vector<ChainPresetEntry> entries;

    const auto folder = chainPresetFolder();
    if (! folder.isDirectory()) return entries;

    for (auto& file : folder.findChildFiles (File::findFiles, false, "*.sxchain"))
    {
        String name, error;
        std::vector<ChainItem> items;
        if (! readChainFile (file, name, items, error)) continue;
        entries.push_back ({ name, file });
    }

    std::sort (entries.begin(), entries.end(), [] (const ChainPresetEntry& a, const ChainPresetEntry& b)
    {
        const int byName = a.name.compareIgnoreCase (b.name);
        return byName != 0 ? byName < 0 : a.file.getFileName().compareIgnoreCase (b.file.getFileName()) < 0;
    });

    return entries;
}

bool writeChainPreset (AppState& app, int trackId, const File& file,
                       const String& name, String& error)
{
    WaitCursor wait;
    std::vector<ChainItem> items;
    if (! captureChain (app, trackId, items, error)) return false;
    return writeChainFile (items, file, name.trim().isNotEmpty() ? name.trim() : file.getFileNameWithoutExtension(), error);
}

bool saveChainPreset (AppState& app, int trackId, const String& name, String& error)
{
    const auto trimmed = name.trim();
    if (trimmed.isEmpty())
    {
        error = TRANS ("Please enter a preset name.");
        return false;
    }

    auto safeName = File::createLegalFileName (trimmed);
    if (safeName.isEmpty()) safeName = "Chain";
    const File file = chainPresetFolder().getChildFile (safeName + ".sxchain");
    return writeChainPreset (app, trackId, file, trimmed, error);
}

bool applyChainPreset (AppState& app, int trackId, const File& file, String& error)
{
    String name;
    std::vector<ChainItem> items;
    if (! readChainFile (file, name, items, error)) return false;

    std::vector<PluginSlot> slots;
    slots.reserve (items.size());

    for (auto& item : items)
    {
        PluginSlot slot = item.slot;
        slot.sidechainTrack = 0;
        if (trackId >= 0) slot.postFader = false;   // master-only switch

        // Track ids are not portable: resolve the side-chain source by track name. A source that
        // does not exist in this project simply means "no side-chain".
        if (item.sidechainName.isNotEmpty())
            for (auto& t : app.project.tracks)
                if (t != nullptr && t->name == item.sidechainName)
                {
                    slot.sidechainTrack = t->id;
                    break;
                }

        if (slot.sidechainTrack == trackId) slot.sidechainTrack = 0;   // never side-chain itself
        slots.push_back (std::move (slot));
    }

    WaitCursor wait;
    return app.engine.replaceInsertChain (trackId, slots, error);
}

//==============================================================================
void addChainPresetMenuItems (PopupMenu& menu, AppState& app, int trackId)
{
    std::vector<PluginSlot>* chain = nullptr;
    if (trackId < 0)
        chain = &app.project.masterTrack.inserts;
    else if (auto* t = app.project.findTrack (trackId))
        chain = &t->inserts;

    PopupMenu chainMenu;

    PopupMenu::Item saveItem (TRANS ("Save Plug-in Chain Preset..."));
    saveItem.isEnabled = chain != nullptr && ! chain->empty();
    if (saveItem.isEnabled)
        saveItem.action = [&app, trackId] { showSaveChainPresetDialog (app, trackId); };
    chainMenu.addItem (saveItem);

    const auto entries = listChainPresets();
    PopupMenu loadMenu;
    for (auto& entry : entries)
    {
        PopupMenu::Item loadItem (entry.name);
        loadItem.action = [&app, trackId, file = entry.file]
        {
            String error;
            if (! applyChainPreset (app, trackId, file, error))
                showError (TRANS ("Could not load the plug-in chain preset:") + "\n" + error);
        };
        loadMenu.addItem (loadItem);
    }

    if (entries.empty())
        loadMenu.addItem (-1, TRANS ("No plug-in chain presets saved"), false);

    chainMenu.addSubMenu (TRANS ("Load Plug-in Chain Preset"), loadMenu, ! entries.empty());

    PopupMenu::Item folderItem (TRANS ("Open Chain Preset Folder"));
    folderItem.action = [] { chainPresetFolder().revealToUser(); };
    chainMenu.addItem (folderItem);

    menu.addSubMenu (TRANS ("Plug-in Chain Presets"), chainMenu);
}

void showSaveChainPresetDialog (AppState& app, int trackId)
{
    String initial = TRANS ("Chain");
    if (trackId < 0)
        initial = TRANS ("Master Chain");
    else if (auto* t = app.project.findTrack (trackId))
        initial = t->name + " " + TRANS ("Chain");

    auto* appPtr = &app;
    promptText (TRANS ("Save Plug-in Chain Preset"), TRANS ("Preset name:"), initial,
                [appPtr, trackId] (String name)
    {
        name = name.trim();
        if (name.isEmpty()) return;

        String error;
        if (! saveChainPreset (*appPtr, trackId, name, error))
            showError (TRANS ("Could not save the plug-in chain preset:") + "\n" + error);
    });
}
}
