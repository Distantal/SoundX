#include "SlotMenus.h"
#include "../engine/BuiltInEffects.h"
#include "ChainPresets.h"
#include "PluginPicker.h"

namespace sx
{
using namespace juce;

void showError (const String& message)
{
    MessageManager::callAsync ([message]
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "SoundX", message);
    });
}

namespace
{
    enum { kBuiltIn = 1, kEdit, kBypass, kRemove, kScan, kUp, kDown, kPostFader, kSearch,
    kBuiltInSampler,
    kSidechainNone = 30, kSidechainBase = 30000,
           kOutsShow, kOutsRemove, kAddLayer = 40, kLayerBase = 1000, kLayerCount = 512,
           // the built-in effect family inside the vendor submenu ("SoundX")
           kBuiltInEffectBase = 1600, kBuiltInEffectCount = 64,
           kReplaceBase = 100 };

    struct ModalWait
    {
        ModalWait() { MouseCursor::showWaitCursor(); }
        ~ModalWait() { MouseCursor::hideWaitCursor(); }
    };
}

bool isInstrumentLayerMenuId (int menuId)
{
    return menuId >= kLayerBase && menuId < kLayerBase + kLayerCount;
}

bool isBuiltInEffectMenuId (int menuId)
{
    return menuId >= kBuiltInEffectBase && menuId < kBuiltInEffectBase + kBuiltInEffectCount;
}

int builtInEffectMenuCount() { return (int) builtInEffects().size(); }

void showSidechainMenuFor (AppState& app, int64 uid, Component& target)
{
    PluginSlot* slot = nullptr;
    int trackId = -1;
    for (auto& t : app.project.tracks)
        if (t != nullptr && (slot = t->findSlot (uid)) != nullptr) { trackId = t->id; break; }
    if (slot == nullptr)
        for (auto& s : app.project.masterTrack.inserts) if (s.uid == uid) slot = &s;
    if (slot == nullptr) return;

    PopupMenu sc;
    sc.addItem (kSidechainNone, TRANS ("None"), true, slot->sidechainTrack == 0);
    for (auto& t : app.project.tracks)
    {
        if (t == nullptr || t->isListenBus) continue;
        if (t->id == trackId) continue;
        sc.addItem (kSidechainBase + t->id, t->name, true, slot->sidechainTrack == t->id);
    }
    sc.showMenuAsync (PopupMenu::Options().withTargetComponent (&target),
                      [&app, slot] (int r)
    {
        if (r != kSidechainNone && r < kSidechainBase) return;
        const int source = r == kSidechainNone ? 0 : r - kSidechainBase;
        if (slot->sidechainTrack == source) return;
        app.project.checkpoint();
        slot->sidechainTrack = source;
        app.engine.pluginLatencyChanged();   // rebuilds the snapshot so the new source is wired
    });
}
void showSlotMenu (AppState& app, int trackId, int64 uid, bool instrumentSlot, Component* anchor)
{
    auto* track = app.project.findTrack (trackId);
    PluginSlot* slot = nullptr;
    int insertIndex = -1;
    if (uid != 0)
    {
        if (track != nullptr) slot = track->findSlot (uid);
        else for (auto& s : app.project.masterTrack.inserts) if (s.uid == uid) slot = &s;
        if (slot == nullptr) return;
        auto& v = track != nullptr ? track->inserts : app.project.masterTrack.inserts;
        for (size_t i = 0; i < v.size(); ++i) if (v[i].uid == uid) insertIndex = (int) i;
    }

    PopupMenu m;
    Array<PluginDescription> list = instrumentSlot ? app.plugins.instruments() : app.plugins.effects();

    if (slot != nullptr)
    {
        m.addItem (kEdit, TRANS ("Open Plug-in Window"));
        m.addItem (kBypass, TRANS ("Bypass"), true, slot->bypass);
        // what this plug-in reports as latency, and that the mixer is compensating for it
        if (auto inst = app.engine.getInstance (slot->uid))
        {
            const int lat = inst->latencySamples.load();
            if (lat > 0)
            {
                const double ms = 1000.0 * (double) lat / jmax (1.0, app.engine.getSampleRate());
                m.addItem (-1, TRANS ("latency") + ": " + String (lat) + " " + TRANS ("samples")
                                 + " (" + String (ms, 1) + " ms) · " + TRANS ("compensated automatically"), false);
            }
        }
        const bool canBePostFader = track == nullptr || track->isBus() || track->isSend();
        if (canBePostFader)
            m.addItem (kPostFader, track == nullptr ? TRANS ("Post-fader (after the master fader)")
                                                    : TRANS ("Post-fader (after the bus fader)"),
                       true, slot->postFader);
        // side-chain source: another track's post-inserts signal feeds this plug-in's detector
        if (slot->builtIn)
        {
            PopupMenu sc;
            sc.addItem (kSidechainNone, TRANS ("None"), true, slot->sidechainTrack == 0);
            for (auto& t : app.project.tracks)
            {
                if (t == nullptr || t->isListenBus) continue;
                if (track != nullptr && t->id == track->id) continue;      // not itself
                sc.addItem (kSidechainBase + t->id, t->name, true, slot->sidechainTrack == t->id);
            }
            m.addSubMenu (TRANS ("Side-chain Input"), sc);
        }
        if (insertIndex >= 0)
        {
            auto& v = track != nullptr ? track->inserts : app.project.masterTrack.inserts;
            m.addItem (kUp, TRANS ("Move Up"), insertIndex > 0);
            m.addItem (kDown, TRANS ("Move Down"), insertIndex < (int) v.size() - 1);
        }
        m.addSeparator();
    }

    PopupMenu choose;
    if (instrumentSlot)
    {
        // both built-in instruments need no scan and always load
        choose.addItem (kBuiltIn, "SX Synth (" + TRANS ("built-in") + ")");
        choose.addItem (kBuiltInSampler, "SX Sampler (" + TRANS ("built-in") + ")");
    }
    // the built-in effect family gets its own SoundX folder, like a manufacturer
    if (! instrumentSlot)
    {
        PopupMenu soundx;
        const auto& builtIns = builtInEffects();
        for (size_t i = 0; i < builtIns.size(); ++i)
            soundx.addItem (kBuiltInEffectBase + (int) i, builtIns[i].name);
        choose.addSubMenu ("SoundX", soundx);
    }
    if (! list.isEmpty())
    {
        if (instrumentSlot || ! instrumentSlot) choose.addSeparator();
        app.plugins.addMenuItems (choose, list);
    }
    else if (! instrumentSlot)
        choose.addItem (-1, TRANS ("No effect plug-ins found - scan first"), false);

    // searching comes first: with a few hundred plug-ins the vendor submenu below is the slow path
    m.addItem (kSearch, instrumentSlot ? TRANS ("Search Instrument...") : TRANS ("Search Plug-in..."));
    m.addSubMenu (slot != nullptr ? TRANS ("Replace With") : (instrumentSlot ? TRANS ("Choose Instrument") : TRANS ("Add Effect")), choose);
    m.addSeparator();
    addChainPresetMenuItems (m, app, trackId);

    // several instruments on one track: add a layer, and pick which layer the slot menu works on
    if (instrumentSlot && track != nullptr && track->hasInstrument)
    {
        if (! app.hasOutputChannels (trackId))
            m.addItem (kAddLayer, TRANS ("Add Instrument to Track..."));

        if (track->numInstruments() > 1)
        {
            PopupMenu layers;
            // the primary instrument first, then the stacked ones, numbered in mixer order
            auto all = track->instruments();
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i] != nullptr)
                    layers.addItem (kLayerBase + (int) i,
                                    String ((int) i + 1) + ". " + all[i]->desc.name + (all[i]->bypass ? " (" + TRANS ("bypassed") + ")" : ""),
                                    true, all[i]->uid == uid);
            m.addSubMenu (TRANS ("Instruments") + " (" + String (track->numInstruments()) + ")", layers);
        }
    }

    // a multi-output instrument: its extra outputs can become mixer channels (Studio One style),
    // and they can be shown or hidden there without stopping them from playing
    if (slot != nullptr && instrumentSlot && track != nullptr && track->numInstruments() == 1)
    {
        const int pairs = app.instrumentOutputPairs (trackId);
        if (pairs > 0)
        {
            m.addSeparator();
            const bool shown = track->showOutputChannels && app.hasOutputChannels (trackId);
            m.addItem (kOutsShow, TRANS ("Output Channels in the Mixer") + " (" + String (pairs) + ")", true, shown);
            if (app.hasOutputChannels (trackId))
                m.addItem (kOutsRemove, TRANS ("Remove Output Channels"));
        }
    }

    if (slot != nullptr) m.addItem (kRemove, TRANS ("Remove"));
    m.addSeparator();
    m.addItem (kScan, TRANS ("Scan Plug-ins..."));

    auto* appPtr = &app;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (anchor).withMinimumWidth (180),
                     [appPtr, trackId, uid, instrumentSlot, list, insertIndex] (int r)
    {
        if (r == 0) return;
        auto& a = *appPtr;
        if (r == kScan) { a.plugins.showScannerWindow(); return; }
        if (r == kSearch) { showPluginPicker (a, trackId, uid, instrumentSlot); return; }
        if (r == kEdit) { a.showPluginEditor (uid); return; }
        if (r == kBypass)
        {
            PluginSlot* s = nullptr;
            if (auto* t = a.project.findTrack (trackId)) s = t->findSlot (uid);
            else for (auto& x : a.project.masterTrack.inserts) if (x.uid == uid) s = &x;
            if (s != nullptr) a.engine.setSlotBypass (trackId, uid, ! s->bypass);
            return;
        }
        if (r == kRemove) { a.engine.removeSlot (trackId, uid); return; }
        if (r == kAddLayer) { showAddInstrumentPicker (a, trackId); return; }
        if (isInstrumentLayerMenuId (r))
        {
            // switch the slot menu to another layer of this track: open its window
            if (auto* t = a.project.findTrack (trackId))
            {
                auto all = t->instruments();
                const int idx = r - kLayerBase;
                if (idx >= 0 && idx < (int) all.size() && all[(size_t) idx] != nullptr)
                    a.showPluginEditor (all[(size_t) idx]->uid);
            }
            return;
        }
        if (r == kOutsShow)
        {
            // first use creates the channels, later clicks just show / hide them
            if (! a.hasOutputChannels (trackId)) a.expandOutputChannels (trackId);
            else a.toggleOutputChannelsInMixer (trackId);
            return;
        }
        if (r == kOutsRemove) { a.collapseOutputChannels (trackId); return; }
        if (r == kSidechainNone || (r >= kSidechainBase && r < PluginManager::kPluginMenuBase))
        {
            const int source = r == kSidechainNone ? 0 : r - kSidechainBase;
            auto apply = [&] (PluginSlot& x) { a.project.checkpoint(); x.sidechainTrack = source; };
            auto* tr = a.project.findTrack (trackId);
            if (tr != nullptr && insertIndex >= 0 && insertIndex < (int) tr->inserts.size())
                apply (tr->inserts[(size_t) insertIndex]);
            else
                for (auto& x : a.project.masterTrack.inserts) if (x.uid == uid) apply (x);
            a.project.sendContentChanged();
            return;
        }
        if (r == kPostFader)
        {
            for (auto& x : a.project.masterTrack.inserts)
                if (x.uid == uid)
                {
                    a.project.checkpoint();
                    x.postFader = ! x.postFader;
                    a.project.sendMixChanged();
                    a.project.sendContentChanged();
                    break;
                }
            return;
        }
        if (r == kUp) { a.engine.moveInsert (trackId, insertIndex, insertIndex - 1); return; }
        if (r == kDown) { a.engine.moveInsert (trackId, insertIndex, insertIndex + 1); return; }

        PluginDescription desc;
        if (r == kBuiltIn) desc = makeBuiltInSynthSlot().desc;
        else if (r == kBuiltInSampler) desc = makeBuiltInSamplerSlot().desc;
        else if (isBuiltInEffectMenuId (r))
        {
            const int bi = r - kBuiltInEffectBase;
            const auto& builtIns = builtInEffects();
            if (bi < 0 || bi >= (int) builtIns.size()) return;
            desc = makeBuiltInEffectSlot (builtIns[(size_t) bi].id).desc;
        }
        else
        {
            const int idx = PluginManager::indexFromMenuId (list, r);
            if (idx < 0) return;
            desc = list[idx];
        }

        String err;
        bool ok;
        int64 newUid = 0;
        {
            ModalWait w;
            if (instrumentSlot) ok = a.engine.setInstrument (trackId, desc, err);
            else
            {
                const bool replace = uid != 0;
                ok = a.engine.addInsert (trackId, desc, err);
                if (ok) newUid = a.lastInsertUid (trackId);
                if (ok && replace) a.engine.removeSlot (trackId, uid);
            }
        }
        if (! ok) showError (TRANS ("Failed to load plug-in") + " \"" + desc.name + "\"\n" + err);
        else if (instrumentSlot)
        {
            if (auto* t = a.project.findTrack (trackId)) a.showInsertedPluginEditor (t->instrument.uid);
        }
        else
            a.showInsertedPluginEditor (newUid);
    });
}

void showTrackMenu (AppState& app, int trackId, Component* anchor, std::function<void()> onRename)
{
    auto* t = app.project.findTrack (trackId);
    if (t == nullptr) return;

    PopupMenu m;
    m.addItem (1, TRANS ("Rename"), onRename != nullptr);
    PopupMenu colours;
    auto& pal = theme::trackPalette();
    for (int i = 0; i < pal.size(); ++i)
    {
        // a real swatch icon: addColouredItem only tints the (empty) text, which looked blank
        auto swatch = std::make_unique<DrawableRectangle>();
        swatch->setFill (pal[i]);
        swatch->setRectangle (juce::Rectangle<float> (0.0f, 0.0f, 18.0f, 12.0f));
        colours.addItem (100 + i, String(), true, t->colour == pal[i], std::move (swatch));
    }
    m.addSubMenu (TRANS ("Colour"), colours);

    // output routing and sends (only meaningful for ordinary tracks / buses; an automation row
    // has no audio channel to route)
    std::vector<int> busIds, sendTargets, autoTargets;
    // the first entry is the master bus, then every channel an automation row could drive
    autoTargets.push_back (0);
    for (auto& o : app.project.tracks)
        if (o != nullptr && o->id != trackId && ! o->isAutomation() && ! o->isListenBus)
            autoTargets.push_back (o->id);
    if (! t->isAutomation())
    {
        for (auto& o : app.project.tracks)
        {
            if (o->isBus() && o->id != trackId) busIds.push_back (o->id);
            if (o->isGroupTrack() && o->id != trackId) sendTargets.push_back (o->id);
        }

        PopupMenu outs;
        outs.addItem (300, TRANS ("Master"), true, t->outputBusId == 0);
        for (int i = 0; i < (int) busIds.size(); ++i)
            if (auto* b = app.project.findTrack (busIds[(size_t) i]))
                outs.addItem (310 + i, b->name, true, t->outputBusId == busIds[(size_t) i]);
        m.addSubMenu (TRANS ("Output"), outs);

        PopupMenu sendMenu;
        const bool canSend = ! t->isSend();   // effect returns are parallel and never send out
        if (canSend && sendTargets.empty())
            sendMenu.addItem (-1, TRANS ("No bus or effect send track yet"), false);
        if (canSend)
            for (int i = 0; i < (int) sendTargets.size(); ++i)
            {
                bool has = false;
                for (auto& s : t->sends) has = has || s.target == sendTargets[(size_t) i];
                auto* target = app.project.findTrack (sendTargets[(size_t) i]);
                sendMenu.addItem (400 + i, target != nullptr ? target->name : String (sendTargets[(size_t) i]), true, has);
            }
        if (canSend) m.addSubMenu (TRANS ("Sends"), sendMenu);
        m.addSeparator();
        addChainPresetMenuItems (m, app, trackId);
    }
    m.addSeparator();
    m.addItem (2, TRANS ("Move Up"), app.project.indexOfTrack (trackId) > 0);
    m.addItem (3, TRANS ("Move Down"), app.project.indexOfTrack (trackId) < (int) app.project.tracks.size() - 1);
    m.addItem (4, TRANS ("Duplicate Track"));
    {
        const int nSel = (int) app.selectedTracks.size();
        const bool inSel = app.selectedTracks.count (trackId) > 0;
        m.addItem (5, inSel && nSel > 1 ? TRANS ("Delete Selected Tracks") + " (" + String (nSel) + ")"
                                        : TRANS ("Delete Track"));
    }
    m.addSeparator();
    if (! t->isGroupTrack() && ! t->isAutomation()) m.addItem (6, TRANS ("Group into Bus"));
    m.addItem (7, TRANS ("New Effect Send Track"));
    m.addSeparator();
    m.addItem (12, TRANS ("Create Folder from Selected Tracks"), ! app.selectedTracks.empty());
    if (t->isFolder)
    {
        m.addItem (13, TRANS ("Unpack Folder"));
        m.addItem (14, t->folderOpen ? TRANS ("Close Folder") : TRANS ("Open Folder"));
    }
    if (! t->isAutomation())
        m.addItem (15, TRANS ("Add Automation Track..."));
    else
    {
        m.addItem (16, TRANS ("Move Back into the Channel"));
        PopupMenu at;
        int id = 500;
        at.addItem (id++, TRANS ("Master"), true, t->autoTargetTrack == 0);
        for (size_t i = 1; i < autoTargets.size(); ++i)
            if (auto* target = app.project.findTrack (autoTargets[i]))
                at.addItem (id++, target->name, true, t->autoTargetTrack == autoTargets[i]);
        m.addSubMenu (TRANS ("Automation Target Track"), at);
    }
    m.addSeparator();
    if (t->type == TrackType::Instrument || t->type == TrackType::Audio)
    {
        if (t->frozen)
            m.addItem (10, TRANS ("Unfreeze Track"));
        else
        {
            PopupMenu fm;
            fm.addSectionHeader (TRANS ("Freeze: effects are baked; add new effects after freezing"));
            fm.addItem (10, TRANS ("Freeze Track (with effects)"));
            fm.addItem (11, TRANS ("Freeze Track (no effects)"));
            m.addSubMenu (TRANS ("Freeze Track"), fm);
        }
    }
    // hide / show. A menu opened on a multi-selection hides the whole selection; a hidden row can
    // only be reached through the mixer strip menu (hidden rows are still mixer channels), so it
    // has to offer "Show Track" to bring that one row back into the arranger.
    if (AppState::isArrangerTrack (*t))
    {
        const int nSelected = (int) app.selectedTracks.size();
        const bool hideSelection = nSelected > 1 && app.selectedTracks.count (trackId) > 0;
        m.addItem (8, t->hidden ? TRANS ("Show Track")
                                : (hideSelection ? TRANS ("Hide Selected Tracks") + " (" + String (nSelected) + ")"
                                                 : TRANS ("Hide Track")));
    }
    m.addItem (9, TRANS ("Show All Hidden Tracks"), app.hiddenTrackCount() > 0);

    auto* appPtr = &app;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (anchor),
                     [appPtr, trackId, onRename, busIds, sendTargets, autoTargets] (int r)
    {
        if (r == 1 && onRename) onRename();
        else if (r == 12)
        {
            std::vector<int> ids (appPtr->selectedTracks.begin(), appPtr->selectedTracks.end());
            if (ids.empty()) ids.push_back (trackId);
            appPtr->groupIntoFolder (ids);
        }
        else if (r == 13) appPtr->unpackFolder (trackId);
        else if (r == 14) appPtr->toggleFolderOpen (trackId);
        else if (r == 15) { showAutomationTargetMenu (*appPtr, trackId, nullptr, true); return; }
        else if (r == 16) { appPtr->inlineAutomationTrack (trackId); return; }
        else if (r == 10 || r == 11)   // freeze / unfreeze (10 = with effects or unfreeze, 11 = no effects)
        {
            auto* tr = appPtr->project.findTrack (trackId);
            if (tr != nullptr)
            {
                if (tr->frozen) appPtr->unfreezeTrack (trackId);
                else appPtr->freezeTrack (trackId, r == 10);
            }
        }
        else if (r >= 500)          // automation track: change the channel it drives
        {
            const int idx = r - 500;
            if (idx < (int) autoTargets.size())
                appPtr->setAutomationTargetTrack (trackId, autoTargets[(size_t) idx]);
        }
        else if (r >= 400)          // toggle a send into a bus / effect return
        {
            const int idx = r - 400;
            if (idx >= (int) sendTargets.size()) return;
            const int target = sendTargets[(size_t) idx];
            bool has = false;
            if (auto* tr = appPtr->project.findTrack (trackId))
                for (auto& s : tr->sends) has = has || s.target == target;
            if (has) appPtr->removeSend (trackId, target);
            else appPtr->addSend (trackId, target);
        }
        else if (r >= 310)          // route into a bus
        {
            const int idx = r - 310;
            if (idx >= (int) busIds.size()) return;
            if (auto* tr = appPtr->project.findTrack (trackId))
            {
                appPtr->project.checkpoint();
                tr->outputBusId = busIds[(size_t) idx];
                appPtr->project.sendContentChanged();
                appPtr->project.sendMixChanged();
            }
        }
        else if (r == 300)          // route straight to the master
        {
            if (auto* tr = appPtr->project.findTrack (trackId))
            {
                appPtr->project.checkpoint();
                tr->outputBusId = 0;
                appPtr->project.sendContentChanged();
                appPtr->project.sendMixChanged();
            }
        }
        else if (r >= 100 && r < 200)
        {
            // a colour picked on a multi-selection recolours every selected track
            const auto col = theme::trackPalette()[r - 100];
            auto& proj = appPtr->project;
            proj.checkpoint();
            bool applied = false;
            if (appPtr->selectedTracks.size() > 1 && appPtr->selectedTracks.count (trackId) > 0)
                for (int id : appPtr->selectedTracks)
                    if (auto* tr = proj.findTrack (id)) { tr->colour = col; applied = true; }
            if (! applied)
                if (auto* tr = proj.findTrack (trackId)) tr->colour = col;
            proj.sendMixChanged();
            proj.sendContentChanged();
        }
        else if (r == 2) appPtr->moveTrack (trackId, -1);
        else if (r == 3) appPtr->moveTrack (trackId, 1);
        else if (r == 4) appPtr->duplicateTrack (trackId);
        else if (r == 5)   // delete every selected track, not only the clicked one
        {
            std::vector<int> ids (appPtr->selectedTracks.begin(), appPtr->selectedTracks.end());
            if (ids.size() <= 1 || appPtr->selectedTracks.count (trackId) == 0) ids = { trackId };
            appPtr->deleteTracks (ids);
        }
        else if (r == 6)
        {
            std::vector<int> ids (appPtr->selectedTracks.begin(), appPtr->selectedTracks.end());
            if (ids.empty()) ids.push_back (trackId);
            appPtr->groupIntoBus (ids);
        }
        else if (r == 7) appPtr->addSendTrack();
        else if (r == 8)
        {
            auto* tr = appPtr->project.findTrack (trackId);
            if (tr == nullptr) return;
            if (tr->hidden) appPtr->setTracksHidden ({ trackId }, false);    // bring this one row back
            else
            {
                // the menu was opened on a multi-selection: hide every selected row, not only the
                // clicked one (a right-click inside the selection keeps the selection alive)
                std::vector<int> ids;
                if (appPtr->selectedTracks.size() > 1 && appPtr->selectedTracks.count (trackId) > 0)
                    ids.assign (appPtr->selectedTracks.begin(), appPtr->selectedTracks.end());
                else
                    ids.push_back (trackId);
                appPtr->setTracksHidden (ids, true);
            }
        }
        else if (r == 9) appPtr->setTracksHidden (appPtr->hiddenArrangerTrackIds(), false);
    });
}

// pick the channel: a project track by id, or the master bus (id 0)
static Track* channelFor (Project& p, int trackId)
{
    if (trackId == 0) return &p.masterTrack;
    return p.findTrack (trackId);
}
void showAutomationTargetMenu (AppState& app, Track& t, int trackId, Component* anchor, bool asAutomationTrack)
{
    struct Entry { String target, label; };
    auto entries = std::make_shared<std::vector<Entry>>();
    PopupMenu m;
    int id = 1;

    auto hasAutomationTrack = [&] (const String& target)
    {
        for (auto& o : app.project.tracks)
            if (o != nullptr && o->isAutomation() && o->autoTargetTrack == trackId && o->autoTarget == target)
                return true;
        return false;
    };

    auto add = [&] (PopupMenu& menu, const String& target, const String& label, const String& shown)
    {
        entries->push_back ({ target, label });
        auto* lane = t.findLane (target);
        menu.addItem (id++, shown, true, asAutomationTrack ? hasAutomationTrack (target)
                                                           : (lane != nullptr && lane->visible));
    };

    add (m, "vol", trackId == 0 ? "Master Volume" : "Volume", trackId == 0 ? "Master Volume" : "Volume");
    if (trackId != 0) add (m, "pan", "Pan", "Pan");

    auto& lt = app.engine.lastTouched;
    if (lt.uid != 0 && lt.trackId == trackId)
    {
        m.addSeparator();
        add (m, "p:" + String (lt.uid) + ":" + String (lt.index), lt.name, TRANS ("Last Touched") + ": " + lt.name);
    }

    auto addPlugin = [&] (const PluginSlot& s)
    {
        auto inst = app.engine.getInstance (s.uid);
        if (inst == nullptr) return;
        auto& params = inst->proc->getParameters();
        if (params.isEmpty()) return;

        PopupMenu sub;
        int shown = 0;
        PopupMenu page;
        int pageStart = 0;
        for (int i = 0; i < params.size(); ++i)
        {
            auto* p = params[i];
            if (! p->isAutomatable()) continue;
            auto name = p->getName (40);
            if (name.startsWith ("MIDI CC")) continue;
            add (page, "p:" + String (s.uid) + ":" + String (i), inst->name + ": " + name, name);
            if (++shown % 40 == 0)
            {
                sub.addSubMenu (String (pageStart + 1) + " - " + String (shown), page);
                page = PopupMenu();
                pageStart = shown;
            }
        }
        if (shown % 40 != 0) sub.addSubMenu (String (pageStart + 1) + " - " + String (shown), page);
        if (shown > 0)
        {
            m.addSeparator();
            m.addSubMenu (s.desc.name, sub);
        }
    };
    if (t.hasInstrument) addPlugin (t.instrument);
    for (auto& s : t.inserts) addPlugin (s);

    auto* appPtr = &app;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (anchor), [appPtr, trackId, entries, asAutomationTrack] (int r)
    {
        if (r <= 0 || r > (int) entries->size()) return;
        auto* tr = channelFor (appPtr->project, trackId);
        if (tr == nullptr) return;
        const auto& e = (*entries)[(size_t) r - 1];
        if (asAutomationTrack)
        {
            appPtr->addAutomationTrack (trackId, e.target, e.label);
            return;
        }
        tr->activeLane = e.target;
        auto& lane = tr->ensureLane (e.target, e.label);
        lane.visible = true;
        // adding a lane on a normal channel shows it in the track body and enters edit mode, so
        // the next click can place a node right there (the master keeps its separate lane rows).
        // Only one parameter is drawn in the track body, so the new lane becomes the shown one.
        if (trackId != 0)
        {
            tr->automationInTrack = true;
            tr->automationEdit = true;
            for (auto& l : tr->lanes) l.visible = l.target == e.target;
        }
        tr->showAutomation = tr->hasVisibleLanes();
        appPtr->project.sendContentChanged();
        appPtr->project.sendStructureChanged();
    });
}


void showAutomationTargetMenu (AppState& app, int trackId, Component* anchor, bool asAutomationTrack)
{
    if (auto* t = channelFor (app.project, trackId))
        showAutomationTargetMenu (app, *t, trackId, anchor, asAutomationTrack);
}

void showAutomationLanesMenu (AppState& app, Track& t, int trackId, Component* anchor)
{
    PopupMenu m;
    int id = 1;
    auto targets = std::make_shared<std::vector<String>>();
    for (auto& l : t.lanes)
    {
        targets->push_back (l.target);
        String label = l.label.isNotEmpty() ? l.label : (l.target == "vol" ? String ("Volume") : l.target == "pan" ? String ("Pan") : l.target);
        if (! l.points.empty()) label += "  (" + String ((int) l.points.size()) + ")";
        m.addItem (id++, label, true, l.visible);
    }
    if (! t.lanes.empty()) m.addSeparator();
    m.addItem (10000, TRANS ("Add Automation..."));
    m.addItem (10002, TRANS ("Add Automation Track..."));
    m.addItem (10003, TRANS ("Edit Automation in Track"), t.inlineAutomationLane() != nullptr, t.automationEdit);
    m.addItem (10001, TRANS ("Hide All Automation"), t.hasVisibleLanes());

    auto* appPtr = &app;
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (anchor), [appPtr, trackId, targets, anchor] (int r)
    {
        auto* tr = channelFor (appPtr->project, trackId);
        if (tr == nullptr) return;
        if (r == 10000) { showAutomationTargetMenu (*appPtr, *tr, trackId, anchor); return; }
        if (r == 10002) { showAutomationTargetMenu (*appPtr, *tr, trackId, anchor, true); return; }
        if (r == 10003)
        {
            // Studio One style: the row becomes an automation editor, the clips fade behind it
            // (an editing mode is not part of the project, so it does not dirty the session)
            tr->automationEdit = ! tr->automationEdit;
            appPtr->notifyView();
            return;
        }
        if (r == 10001)
        {
            for (auto& l : tr->lanes) l.visible = false;
            tr->showAutomation = false;
            tr->automationEdit = false;
            appPtr->project.sendStructureChanged();
            appPtr->project.sendContentChanged();
            return;
        }
        if (r < 1 || r > (int) targets->size()) return;
        const auto target = (*targets)[(size_t) r - 1];
        if (auto* lane = tr->findLane (target))
        {
            const bool turningOn = ! lane->visible;
            if (tr->automationInTrack)
            {
                // in-track display is one curve: selecting a parameter replaces the shown one
                if (turningOn)
                {
                    for (auto& l : tr->lanes) l.visible = l.target == target;
                    tr->activeLane = target;
                }
                else
                {
                    lane->visible = false;
                    if (! tr->hasVisibleLanes()) tr->automationEdit = false;
                }
            }
            else
            {
                lane->visible = ! lane->visible;
                tr->activeLane = target;
                if (! lane->visible && ! tr->hasVisibleLanes()) tr->automationEdit = false;
            }
        }
        tr->showAutomation = tr->hasVisibleLanes();
        appPtr->project.sendStructureChanged();
        appPtr->project.sendContentChanged();
    });
}

void showAutomationLanesMenu (AppState& app, int trackId, Component* anchor)
{
    if (auto* t = channelFor (app.project, trackId)) showAutomationLanesMenu (app, *t, trackId, anchor);
}
}
