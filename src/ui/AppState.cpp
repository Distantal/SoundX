#include "AppState.h"
#include "PluginWindows.h"
#include "SlotMenus.h"
#include "UndoKeys.h"
#include <juce_gui_basics/juce_gui_basics.h>   // ThreadWithProgressWindow (freeze progress)
#include <atomic>
#include <thread>

namespace sx
{
struct AppState::ImportContext
{
    std::mutex mutex;
    AppState* app = nullptr;
};

AppState::AppState (Project& p, Engine& e, PluginManager& pm) : project (p), engine (e), plugins (pm), meters (p, e)
{
    importContext = std::make_shared<ImportContext>();
    importContext->app = this;

    if (auto* props = plugins.properties())
    {
        timeInSeconds = props->getBoolValue ("timeInSeconds", false);
        openPluginEditorOnInsert = props->getBoolValue ("openPluginEditorOnInsert", true);
        multiPluginWindows = props->getBoolValue ("multiPluginWindows", false);
    }

    windows = std::make_unique<PluginWindows> (engine);
    windows->setMultipleWindowsAllowed (multiPluginWindows);
    engine.onSlotRemoved = [this] (juce::int64 uid) { windows->closeNow (uid); };

    // Ctrl+Z / Ctrl+Y while a plug-in window has the focus: the shortcut belongs to the
    // arrangement, but a plug-in editor is a window of its own (see UndoKeys.h)
    windows->onUndoRedo = [this] (bool redo)
    {
        if (redo) project.redo(); else project.undo();
    };

    // the enable / side-chain / bypass toolbar inside a built-in editor window
    windows->onSetSlotBypass = [this] (int trackId, juce::int64 uid, bool bypass)
    {
        engine.setSlotBypass (trackId, uid, bypass);
    };
    windows->showSidechainMenu = [this] (juce::int64 uid, juce::Component& target)
    {
        showSidechainMenuFor (*this, uid, target);
    };
#if JUCE_WINDOWS
    PluginKeyRouter::get().setCallback (windows->onUndoRedo);
#endif
}

AppState::~AppState()
{
    // stop background decodes before the model / engine go away: pending completions must not
    // touch a half-destroyed AppState
    if (importContext != nullptr)
    {
        std::lock_guard<std::mutex> l (importContext->mutex);
        importContext->app = nullptr;
    }
    if (importPool != nullptr)
    {
        // wait for a running decode (a long file can take a few seconds); the jobs only touch the
        // pool cache, which lives in the project, and never this object
        importPool->removeAllJobs (false, 60000);
        importPool.reset();
    }

    meters.stop();
    engine.onSlotRemoved = nullptr;
    windows->onUndoRedo = nullptr;
#if JUCE_WINDOWS
    PluginKeyRouter::get().setCallback (nullptr);
#endif
    windows.reset();
}

//==============================================================================
void MeterCache::start()
{
    if (++clients > 1) return;
    running = true;

    // The engine keeps a maximum between reads. Flush anything left over from before the UI
    // existed so the first frame does not show a stale peak from a previous session.
    if (! flushed)
    {
        Engine::Meters scratch;
        engine.readMeters (-1, scratch);
        for (auto& t : project.tracks) engine.readMeters (t->id, scratch);
        flushed = true;
    }

    startTimerHz (30);
}

void MeterCache::stop()
{
    if (clients > 0) --clients;
    if (clients > 0 || ! running) return;
    stopTimer();
    running = false;
    flushed = false;      // the next start flushes any peak that accumulated while the UI was gone
}

const MeterDisplay& MeterCache::get (int trackId) const
{
    static const MeterDisplay empty;
    auto it = values.find (trackId);
    return it != values.end() ? it->second : empty;
}

void MeterCache::resetMaximum (int trackId)
{
    auto it = values.find (trackId);
    if (it == values.end()) return;
    it->second.maxL = 0.0f;
    it->second.maxR = 0.0f;
    it->second.holdL = 0.0f;
    it->second.holdR = 0.0f;
    it->second.clipped = false;
}

void MeterCache::timerCallback()
{
    updateChannel (-1);
    for (auto& t : project.tracks)
        if (t != nullptr) updateChannel (t->id);
}

void MeterCache::updateChannel (int trackId)
{
    Engine::Meters m;
    engine.readMeters (trackId, m);

    auto& d = values[trackId];
    d.levelL = juce::jmax (m.peakL, d.levelL * 0.86f);
    d.levelR = juce::jmax (m.peakR, d.levelR * 0.86f);
    d.rmsL = m.rmsL;
    d.rmsR = m.rmsR;
    d.holdL = juce::jmax (m.peakL, d.holdL - 0.02f);
    d.holdR = juce::jmax (m.peakR, d.holdR - 0.02f);
    d.maxL = juce::jmax (d.maxL, m.peakL);
    d.maxR = juce::jmax (d.maxR, m.peakR);
    if (m.clipped || m.peakL > 1.0f || m.peakR > 1.0f) d.clipped = true;
}

void AppState::setTimeInSeconds (bool on)
{
    if (timeInSeconds == on) return;

    timeInSeconds = on;
    if (auto* props = plugins.properties())
    {
        props->setValue ("timeInSeconds", on ? "1" : "0");
        props->saveIfNeeded();
    }
    notifyView();       // the rulers and the transport readout follow
}

void AppState::setOpenPluginEditorOnInsert (bool on)
{
    if (openPluginEditorOnInsert == on) return;
    openPluginEditorOnInsert = on;
    if (auto* props = plugins.properties())
    {
        props->setValue ("openPluginEditorOnInsert", on ? "1" : "0");
        props->saveIfNeeded();
    }
}

void AppState::setMultiPluginWindows (bool on)
{
    if (multiPluginWindows == on) return;
    multiPluginWindows = on;
    if (windows != nullptr) windows->setMultipleWindowsAllowed (on);
    if (auto* props = plugins.properties())
    {
        props->setValue ("multiPluginWindows", on ? "1" : "0");
        props->saveIfNeeded();
    }
}

void AppState::selectTrack (int id, bool additive, bool range)
{
    if (id < 0) return;

    if (range && selectedTrack >= 0)
    {
        // extend the selection between the previous anchor and this track
        const int a = project.indexOfTrack (selectedTrack);
        const int b = project.indexOfTrack (id);
        if (a >= 0 && b >= 0)
            for (int i = juce::jmin (a, b); i <= juce::jmax (a, b); ++i)
                selectedTracks.insert (project.tracks[(size_t) i]->id);
    }
    else if (additive)
    {
        if (selectedTracks.count (id) > 0) selectedTracks.erase (id);
        else selectedTracks.insert (id);
    }
    else
        selectedTracks = { id };

    selectedTrack = id;
    engine.setSelectedTrack (id);
    notifySelection();
}

void AppState::selectClip (int trackId, int clipId, bool additive)
{
    if (! additive) selectedClips.clear();
    if (additive && selectedClips.count (clipId) > 0) selectedClips.erase (clipId);
    else selectedClips.insert (clipId);
    selectTrack (trackId);
    notifySelection();
}

void AppState::clearClipSelection()
{
    if (selectedClips.empty()) return;
    selectedClips.clear();
    notifySelection();
}

void AppState::openClipInEditor (int trackId, int clipId)
{
    editTrack = trackId;
    editClip = clipId;
    selectTrack (trackId);
    listeners.call ([] (Listener& l) { l.editorClipChanged(); });
    requestPanel ("pianoroll");
}

void AppState::setZoom (double ppb)
{
    pxPerBeat = juce::jlimit (3.0, 480.0, ppb);
    notifyView();
}

Track& AppState::addTrack (TrackType type)
{
    project.checkpoint();
    auto& t = project.addTrack (type);
    const int id = t.id;
    project.sendStructureChanged();
    project.sendContentChanged();
    selectTrack (id);
    return *project.findTrack (id);
}

void AppState::deleteTrack (int id)
{
    deleteTracks ({ id });
}

int AppState::groupIntoBus (const std::vector<int>& ids)
{
    std::vector<int> members;
    for (int id : ids)
        if (auto* t = project.findTrack (id))
            if (! t->isGroupTrack() && ! t->isAutomation())
                members.push_back (id);
    if (members.empty()) return 0;

    project.checkpoint();
    auto& bus = project.addTrack (TrackType::Bus);
    const int busId = bus.id;
    for (int id : members)
        if (auto* t = project.findTrack (id))
            t->outputBusId = busId;

    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    selectTrack (busId);
    return busId;
}

int AppState::groupIntoFolder (const std::vector<int>& ids)
{
    // members in project order, plus everything already packed into a selected folder: a folder
    // always travels with its children, otherwise they would be left behind when it is moved.
    std::set<int> memberSet (ids.begin(), ids.end());
    // the listen bus is a monitoring stage, not a track to pack into a folder
    for (auto& t : project.tracks) if (t != nullptr && t->isListenBus) memberSet.erase (t->id);
    for (bool grew = true; grew;)
    {
        grew = false;
        for (auto& t : project.tracks)
            if (t != nullptr && t->folderParent != 0 && memberSet.count (t->folderParent) > 0
                && memberSet.count (t->id) == 0)
            {
                memberSet.insert (t->id);
                grew = true;
            }
    }

    std::vector<int> members;
    for (auto& t : project.tracks)
        if (t != nullptr && memberSet.count (t->id) > 0 && ! t->isListenBus)
            members.push_back (t->id);
    if (members.empty()) return 0;

    project.checkpoint();
    int folderCount = 0;
    for (auto& t : project.tracks) if (t != nullptr && t->isFolder) ++folderCount;

    auto& createdFolder = project.addTrack (TrackType::Bus);
    const int folderId = createdFolder.id;
    createdFolder.isFolder = true;
    createdFolder.folderOpen = true;
    createdFolder.outputBusId = 0;
    createdFolder.name = TRANS ("Folder") + " " + juce::String (folderCount + 1);

    auto folderPtr = std::move (project.tracks.back());
    project.tracks.pop_back();

    // pull the members out, keeping their relative order; the folder takes the place of the
    // first selected track and everything selected follows it as one block
    std::vector<std::unique_ptr<Track>> packed, rest;
    int insertAt = 0;
    bool seenMember = false;
    for (auto& t : project.tracks)
    {
        if (t == nullptr) continue;
        if (memberSet.count (t->id) > 0)
        {
            seenMember = true;
            packed.push_back (std::move (t));
        }
        else
        {
            if (! seenMember) ++insertAt;
            rest.push_back (std::move (t));
        }
    }

    std::vector<std::unique_ptr<Track>> out;
    out.reserve (rest.size() + packed.size() + 1);
    for (int i = 0; i < (int) rest.size(); ++i)
    {
        if (i == insertAt) out.push_back (std::move (folderPtr));
        out.push_back (std::move (rest[(size_t) i]));
    }
    if (insertAt >= (int) rest.size()) out.push_back (std::move (folderPtr));
    // folderPos is the index of the one folder row in front of the packed block
    size_t pos = (size_t) insertAt + 1;
    for (auto& m : packed)
        out.insert (out.begin() + (long long) pos++, std::move (m));
    project.tracks = std::move (out);

    for (auto& t : project.tracks)
        if (t != nullptr && memberSet.count (t->id) > 0)
        {
            // only the top level of the selection is re-parented; a folder that was selected
            // together with its children keeps its own children, so nesting is preserved
            bool hasSelectedAncestor = false;
            for (int p = t->folderParent, guard = 0; p != 0 && guard < 64; ++guard)
            {
                if (memberSet.count (p) > 0) { hasSelectedAncestor = true; break; }
                auto* parent = project.findTrack (p);
                p = parent != nullptr ? parent->folderParent : 0;
            }
            if (hasSelectedAncestor) continue;
            t->folderParent = folderId;
            // a packed channel now feeds the folder bus; automation tracks only carry a curve,
            // they have no audio to route
            if (! t->isAutomation()) t->outputBusId = folderId;
        }

    project.refreshFolders();
    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    selectedTracks.clear();
    selectedTrack = folderId;
    selectedTracks.insert (folderId);
    engine.setSelectedTrack (selectedTrack);
    notifySelection();
    notifyView();
    return folderId;
}

bool AppState::unpackFolder (int folderId)
{
    auto* folder = project.findTrack (folderId);
    if (folder == nullptr || ! folder->isFolder) return false;

    // removeTrack() already promotes the children one level up and keeps them feeding the
    // folder's destination, so unpacking never silences or re-routes the material.
    const auto children = project.folderChildren (folderId);
    project.checkpoint();
    project.removeTrack (folderId);
    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    selectedTracks.clear();
    selectedTrack = children.empty() ? -1 : children.front();
    if (selectedTrack >= 0) selectedTracks.insert (selectedTrack);
    engine.setSelectedTrack (selectedTrack);
    notifySelection();
    notifyView();
    return true;
}

void AppState::toggleFolderOpen (int folderId)
{
    auto* folder = project.findTrack (folderId);
    if (folder == nullptr || ! folder->isFolder) return;
    project.checkpoint();
    folder->folderOpen = ! folder->folderOpen;
    project.refreshFolders();
    project.sendStructureChanged();
    project.sendContentChanged();
}

int AppState::addAutomationTrack (int targetTrackId, const juce::String& target, const juce::String& label)
{
    auto* targetTrack = targetTrackId == 0 ? &project.masterTrack : project.findTrack (targetTrackId);
    if (targetTrack == nullptr || target.isEmpty()) return 0;

    // one automation track per parameter: selecting the existing one is friendlier than stacking
    // a second curve with the same destination
    for (auto& t : project.tracks)
        if (t != nullptr && t->isAutomation() && t->autoTargetTrack == targetTrackId && t->autoTarget == target)
        {
            selectTrack (t->id);
            return t->id;
        }

    const juce::String title = label.isNotEmpty() ? label
                       : target == "vol" ? juce::String ("Volume")
                       : target == "pan" ? juce::String ("Pan") : target;
    project.checkpoint();
    {
        auto& created = project.addTrack (TrackType::Automation, title);
        created.autoTargetTrack = targetTrackId;
        created.autoTarget = target;
        created.autoLabel = title;
        auto& lane = created.ensureLane (target, title);
        lane.visible = true;
        // the inline lane (if any) moves into the new row: one curve, one place to edit it
        if (auto* src = targetTrack->findLane (target))
        {
            lane.points = src->points;
            auto& v = targetTrack->lanes;
            v.erase (std::remove_if (v.begin(), v.end(), [&] (const AutoLane& l) { return l.target == target; }), v.end());
            targetTrack->showAutomation = targetTrack->hasVisibleLanes();
            // the curve no longer belongs to the channel row: leave automation edit mode
            targetTrack->automationEdit = false;
        }
    }

    const int newId = project.tracks.back() != nullptr ? project.tracks.back()->id : 0;
    if (auto* created = project.findTrack (newId))
    {
        created->folderParent = targetTrackId == 0 ? 0
                              : (targetTrack->isFolder ? targetTrackId : targetTrack->folderParent);
    }
    // lift the new row out of the end of the list and place it under the channel it drives
    if (newId != 0 && targetTrackId != 0)
    {
        auto ptr = std::move (project.tracks.back());
        project.tracks.pop_back();
        int at = project.indexOfTrack (targetTrackId);
        at = at < 0 ? (int) project.tracks.size() : at + 1;
        at = juce::jlimit (0, (int) project.tracks.size(), at);
        project.tracks.insert (project.tracks.begin() + at, std::move (ptr));
    }

    project.refreshFolders();
    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    selectedTracks.clear();
    selectedTrack = newId;
    if (newId > 0) selectedTracks.insert (newId);
    engine.setSelectedTrack (selectedTrack);
    notifySelection();
    notifyView();
    return newId;
}

int AppState::moveAutomationToTrack (int trackId, const juce::String& target)
{    // trackId 0 is the master bus row: it has lanes too, but is not in project.tracks
    if (trackId == 0)
    {
        const AutoLane* lane = nullptr;
        for (auto& l : project.masterTrack.lanes) if (l.target == target) lane = &l;
        return addAutomationTrack (0, target, lane != nullptr ? lane->label : juce::String());
    }
    auto* t = project.findTrack (trackId);
    if (t == nullptr) return 0;
    auto* lane = t->findLane (target);
    const juce::String label = lane != nullptr ? lane->label : juce::String();
    return addAutomationTrack (trackId, target, label);
}

bool AppState::inlineAutomationTrack (int autoTrackId)
{
    auto* at = project.findTrack (autoTrackId);
    if (at == nullptr || ! at->isAutomation()) return false;
    auto* lane = at->automationLane();
    if (lane == nullptr) return false;
    auto* target = at->autoTargetTrack == 0 ? &project.masterTrack : project.findTrack (at->autoTargetTrack);
    if (target == nullptr) return false;

    const int targetId = at->autoTargetTrack;
    const juce::String targetName = at->autoTarget, label = at->autoLabel;
    const auto points = lane->points;
    project.checkpoint();
    auto& dst = target->ensureLane (targetName, label);
    dst.points = points;
    dst.visible = true;
    if (target->automationInTrack)
        for (auto& l : target->lanes) l.visible = l.target == targetName;
    target->showAutomation = target->hasVisibleLanes();
    target->activeLane = targetName;
    target->automationInTrack = targetId != 0;   // the master keeps its lane rows
    target->automationEdit = targetId != 0;
    project.removeTrack (autoTrackId);
    project.refreshFolders();
    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    selectedTracks.clear();
    selectedTrack = targetId == 0 ? -1 : targetId;
    if (selectedTrack > 0) selectedTracks.insert (selectedTrack);
    engine.setSelectedTrack (selectedTrack);
    notifySelection();
    notifyView();
    return true;
}

bool AppState::setAutomationTargetTrack (int autoTrackId, int targetTrackId)
{
    auto* at = project.findTrack (autoTrackId);
    if (at == nullptr || ! at->isAutomation()) return false;
    auto* target = targetTrackId == 0 ? &project.masterTrack : project.findTrack (targetTrackId);
    if (target == nullptr || target->isAutomation()) return false;
    if (at->autoTargetTrack == targetTrackId) return true;

    project.checkpoint();
    at->autoTargetTrack = targetTrackId;
    // the row stays in the same list position, but follows the target's folder so the hierarchy
    // still shows which channel it controls
    at->folderParent = targetTrackId == 0 ? 0 : (target->isFolder ? target->id : target->folderParent);

    // put the automation row right below the channel it drives (or at the end for the master),
    // so changing the target does not leave it visually inside a different folder
    {
        const int idx = project.indexOfTrack (autoTrackId);
        if (idx >= 0)
        {
            auto ptr = std::move (project.tracks[(size_t) idx]);
            project.tracks.erase (project.tracks.begin() + idx);
            int atIndex = targetTrackId == 0 ? (int) project.tracks.size()
                                             : project.indexOfTrack (targetTrackId) + 1;
            atIndex = juce::jlimit (0, (int) project.tracks.size(), atIndex);
            project.tracks.insert (project.tracks.begin() + atIndex, std::move (ptr));
        }
    }

    project.refreshFolders();
    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    notifyView();
    return true;
}

int AppState::addSendTrack(){
    project.checkpoint();
    auto& s = project.addTrack (TrackType::Send);
    const int id = s.id;
    project.sendStructureChanged();
    project.sendContentChanged();
    selectTrack (id);
    return id;
}

void AppState::addSend (int fromTrackId, int targetId)
{
    auto* t = project.findTrack (fromTrackId);
    auto* target = project.findTrack (targetId);
    if (t == nullptr || target == nullptr || fromTrackId == targetId) return;
    if (t->isSend()) return;              // effect returns are parallel and cannot send again
    if (! target->isGroupTrack()) return; // sends only feed buses / effect returns
    for (auto& s : t->sends) if (s.target == targetId) return;
    project.checkpoint();
    t->sends.push_back (SendSlot { targetId, -6.0f, false });
    project.sendContentChanged();
    project.sendMixChanged();
}

void AppState::removeSend (int fromTrackId, int targetId)
{
    auto* t = project.findTrack (fromTrackId);
    if (t == nullptr) return;
    auto& v = t->sends;
    const auto before = v.size();
    v.erase (std::remove_if (v.begin(), v.end(), [targetId] (const SendSlot& s) { return s.target == targetId; }), v.end());
    if (v.size() == before) return;
    project.checkpoint();
    project.sendContentChanged();
    project.sendMixChanged();
}

void AppState::setSendLevel (int fromTrackId, int targetId, float levelDb)
{
    auto* t = project.findTrack (fromTrackId);
    if (t == nullptr) return;
    for (auto& s : t->sends)
        if (s.target == targetId) { s.levelDb = levelDb; project.sendContentChanged(); return; }
}

void AppState::setSendPre (int fromTrackId, int targetId, bool pre)
{
    auto* t = project.findTrack (fromTrackId);
    if (t == nullptr) return;
    for (auto& s : t->sends)
        if (s.target == targetId) { s.preFader = pre; project.sendContentChanged(); return; }
}

void AppState::deleteTracks (const std::vector<int>& ids)
{
    if (ids.empty()) return;
    project.checkpoint();
    const int firstIdx = project.indexOfTrack (ids.front());
    for (int id : ids)
    {
        if (editTrack == id) { editTrack = -1; editClip = -1; }
        project.removeTrack (id);
    }
    selectedClips.clear();
    selectedTracks.clear();
    const int n = (int) project.tracks.size();
    selectedTrack = n > 0 ? project.tracks[(size_t) juce::jlimit (0, n - 1, firstIdx)]->id : -1;
    if (selectedTrack >= 0) selectedTracks.insert (selectedTrack);
    engine.setSelectedTrack (selectedTrack);
    project.sendStructureChanged();
    project.sendContentChanged();
    notifySelection();
    listeners.call ([] (Listener& l) { l.editorClipChanged(); });
}

void AppState::duplicateTrack (int id)
{
    duplicateTracks ({ id });
}

void AppState::duplicateTracks (const std::vector<int>& ids)
{
    if (ids.empty()) return;
    engine.captureStates();
    project.checkpoint();

    std::set<int> newIds;
    int lastNew = -1;
    // iterate in reverse so each copy lands right after its source
    for (auto it = ids.rbegin(); it != ids.rend(); ++it)
    {
        auto* src = project.findTrack (*it);
        if (src == nullptr) continue;

        auto t = std::make_unique<Track> (*src);
        t->id = project.newId();
        t->name = src->name + " copy";
        t->arm = false;
        t->automationEdit = false;   // a transient edit mode does not travel with a duplicate
        if (t->hasInstrument) t->instrument.uid = makeUid();
        for (auto& s : t->inserts) s.uid = makeUid();
        t->lanes.clear();
        // an automation track is its curve: the copy has to keep it
        if (t->isAutomation() && ! src->lanes.empty()) t->lanes = src->lanes;
        for (auto& c : t->midiClips) c.id = project.newId();
        for (auto& c : t->audioClips) c.id = project.newId();

        const int idx = project.indexOfTrack (*it);
        lastNew = t->id;
        newIds.insert (t->id);
        project.tracks.insert (project.tracks.begin() + idx + 1, std::move (t));
    }
    project.sendStructureChanged();
    project.sendContentChanged();
    selectedTracks = std::move (newIds);
    selectedTrack = lastNew;
    engine.setSelectedTrack (selectedTrack);
    notifySelection();
}

void AppState::moveTrack (int id, int delta)
{
    const int idx = project.indexOfTrack (id);
    const int to = idx + delta;
    if (idx < 0 || to < 0 || to >= (int) project.tracks.size()) return;
    project.checkpoint();
    std::swap (project.tracks[(size_t) idx], project.tracks[(size_t) to]);
    project.sendStructureChanged();
    project.sendContentChanged();
}

void AppState::moveTrackTo (int id, int insertIndex)
{
    auto& v = project.tracks;
    const int n = (int) v.size();
    const int from = project.indexOfTrack (id);
    if (from < 0) return;

    // a folder travels with all of its packed tracks; moving one row never leaves children behind
    std::set<int> block { id };
    if (auto* t = project.findTrack (id); t != nullptr && t->isFolder)
    {
        for (bool grew = true; grew;)
        {
            grew = false;
            for (auto& o : v)
                if (o != nullptr && o->folderParent != 0 && block.count (o->folderParent) > 0
                    && block.count (o->id) == 0)
                {
                    block.insert (o->id);
                    grew = true;
                }
        }
    }

    std::vector<int> original;
    original.reserve (v.size());
    for (auto& t : v) original.push_back (t->id);

    const int slot = juce::jlimit (0, n, insertIndex);
    std::vector<int> restIds, blockIds;
    for (int id : original) (block.count (id) > 0 ? blockIds : restIds).push_back (id);

    // insertion index among the tracks that stay
    int dest = 0;
    for (int i = 0; i < slot && i < n; ++i)
        if (block.count (original[(size_t) i]) == 0) ++dest;

    // work out the new order before touching the vector: checkpoint() serialises the project,
    // so it has to see the old list (moving unique_ptrs out first left null rows behind)
    std::vector<int> newOrder;
    newOrder.reserve (original.size());
    for (int i = 0; i <= (int) restIds.size(); ++i)
    {
        if (i == dest) newOrder.insert (newOrder.end(), blockIds.begin(), blockIds.end());
        if (i < (int) restIds.size()) newOrder.push_back (restIds[(size_t) i]);
    }
    if (newOrder == original) return;

    project.checkpoint();

    std::vector<std::unique_ptr<Track>> moving, rest;
    for (auto& t : v)
        if (t != nullptr && block.count (t->id) > 0) moving.push_back (std::move (t));
        else if (t != nullptr) rest.push_back (std::move (t));

    std::vector<std::unique_ptr<Track>> out;
    out.reserve (v.size());
    for (int i = 0; i <= (int) rest.size(); ++i)
    {
        if (i == dest)
            for (auto& m : moving) out.push_back (std::move (m));
        if (i < (int) rest.size()) out.push_back (std::move (rest[(size_t) i]));
    }

    project.tracks = std::move (out);
    project.refreshFolders();
    project.sendStructureChanged();
    project.sendContentChanged();
}

namespace
{
    // latency of one plug-in slot as the engine currently reports it
    int slotLatency (Engine& engine, juce::int64 uid)
    {
        if (auto inst = engine.getInstance (uid))
            return juce::jmax (0, inst->latencySamples.load());
        return 0;
    }
}

void AppState::freezeTrack (int trackId, bool withFx)
{
    // every exit refreshes the view: the header F button reads t->frozen, and a cancelled or
    // failed freeze leaves the model untouched - without this the button would stay lit
    struct Refresh { AppState& a; ~Refresh() { a.notifyView(); } } refresh { *this };

    auto* t = project.findTrack (trackId);
    if (t == nullptr || t->frozen || freezeBusy) return;
    if (t->type == TrackType::Bus || t->type == TrackType::Send || t->isAutomation()) return;

    // A multi-output instrument is frozen as a whole: its own output plus one render for every
    // mixer channel it feeds. The channel tracks have no clips of their own, so they share the
    // instrument track's content range.
    std::vector<int> targets { trackId };
    for (auto& o : project.tracks)
        if (o != nullptr && o->sourceTrack == trackId && ! o->frozen)
            targets.push_back (o->id);

    auto* source = t;
    if (t->sourceTrack != 0) source = project.findTrack (t->sourceTrack);
    if (source == nullptr) return;

    // content range + a short tail so reverb / delay tails survive the freeze. A manual track
    // delay shifts the content in time, and the frozen audio has to cover the shifted range too.
    double startBeat = 1.0e9, contentEnd = 0.0;
    for (auto& c : source->midiClips)
    {
        startBeat = juce::jmin (startBeat, c.start);
        contentEnd = juce::jmax (contentEnd, c.start + c.length);
    }
    for (auto& c : source->audioClips)
    {
        startBeat = juce::jmin (startBeat, c.start);
        contentEnd = juce::jmax (contentEnd, tempomap::secondsToBeats (project.tempo,
                                          tempomap::beatsToSeconds (project.tempo, c.start) + c.lengthSec));
    }
    if (startBeat > 1.0e8) return;                       // nothing to freeze
    const double spb = juce::jmax (1.0e-4, tempomap::beatsToSeconds (project.tempo, 1.0)
                                           - tempomap::beatsToSeconds (project.tempo, 0.0));
    const double delayBeats = (source->delayMs / 1000.0) / spb;
    startBeat = juce::jmax (0.0, startBeat + delayBeats);
    const double endB = contentEnd + delayBeats;
    if (endB <= startBeat + 1.0e-6) return;
    constexpr double tailSec = 2.0;
    const double endBeat = tempomap::secondsToBeats (project.tempo,
                                                     tempomap::beatsToSeconds (project.tempo, endB) + tailSec);

    const juce::File dir = project.file.existsAsFile() ? Project::audioFilesFolderFor (project.file)
                                                       : engine.getCacheFolder();
    dir.createDirectory();

    // latency that is already inside each render, so the engine can line the frozen audio up
    int instrumentLatency = 0;
    if (source->hasInstrument) instrumentLatency += slotLatency (engine, source->instrument.uid);
    for (auto& layer : source->extraInstruments) instrumentLatency += slotLatency (engine, layer.uid);
    int parentInsertLatency = 0;
    for (auto& sl : source->inserts) parentInsertLatency += slotLatency (engine, sl.uid);

    struct FrozenTarget
    {
        juce::File file;
        std::shared_ptr<AudioData> data;
        int bakedLatency = 0;
        int renderTrackId = 0;
        bool renderWithFx = true;
    };
    std::vector<FrozenTarget> renders;

    // plan every render first (files, ids, latencies), then hand the actual audio work to
    // either the progress window's thread or - for the self test - this thread
    for (int id : targets)
    {
        auto* target = project.findTrack (id);
        if (target == nullptr) continue;
        const bool isChild = target->sourceTrack != 0;
        // the parent chain of an output channel is part of that channel's sound and is always
        // rendered; only the channel's own effects follow the "with / without effects" choice
        int childInserts = 0;
        if (isChild)
            for (auto& sl : target->inserts) childInserts += slotLatency (engine, sl.uid);

        FrozenTarget r;
        r.renderTrackId = id;
        r.renderWithFx = withFx;
        r.bakedLatency = instrumentLatency + parentInsertLatency;
        if (isChild)
            r.bakedLatency += withFx ? childInserts : 0;
        else
            r.bakedLatency = instrumentLatency + (withFx ? parentInsertLatency : 0);

        // reuse the track's previous freeze file (undo of an unfreeze keeps it around); a new
        // name would pile up one WAV per freeze
        if (target->frozenFile.existsAsFile()) target->frozenFile.deleteFile();
        r.file = target->frozenFile != juce::File()
                     ? target->frozenFile
                     : dir.getNonexistentChildFile ("Freeze " + juce::File::createLegalFileName (target->name), ".wav");
        renders.push_back (std::move (r));
    }
    if (renders.empty()) return;

    freezeBusy = true;
    juce::String err;
    bool rendered = true;

    // Everything the render thread needs is prepared here, on the message thread: the device
    // callback is detached and only the plug-ins involved are switched to non-realtime. The
    // worker below therefore never calls back into the message thread, which is what made
    // cancelling a freeze deadlock the UI before.
    std::vector<int> prepIds;
    for (auto& r : renders)
    {
        prepIds.push_back (r.renderTrackId);
        if (auto* tt = project.findTrack (r.renderTrackId))
            if (tt->sourceTrack != 0) prepIds.push_back (tt->sourceTrack);
    }

    if (! engine.beginOfflineRender (prepIds))
    {
        for (auto& r : renders) r.file.deleteFile();
        freezeBusy = false;
        if (onError) onError (TRANS ("Another export or freeze is already running."));
        return;
    }

    {
        struct OfflineSession
        {
            Engine& e;
            ~OfflineSession() { e.endOfflineRender(); }
        } session { engine };

        // one snapshot for the whole freeze: parent + every output channel otherwise converted
        // all MIDI events of the project again for each one
        std::shared_ptr<Snap> snap;

        if (showFreezeProgress && juce::MessageManager::existsAndIsCurrentThread())
        {
            // Render on a worker thread: some instruments dislike having processBlock called from
            // the message thread (the UI thread also has their editor), and a long render otherwise
            // looks like a hang. The worker only checks its cancel flag, never the message thread,
            // so cancelling cannot deadlock. Quick renders (the usual case) finish during the
            // short probe below and get no progress window at all; only a longer render shows one.
            struct FreezeTask : juce::Thread
            {
                FreezeTask (Engine& e, std::vector<int> ids, std::vector<juce::File> fs, double s, double en, bool noFx)
                    : juce::Thread ("SoundX freeze"), engine (e), trackIds (std::move (ids)),
                      files (std::move (fs)), start (s), end (en), bypassInserts (noFx) {}

                void run() override
                {
                    // ONE render pass for parent + every output channel: the shared instrument
                    // runs once and each printed track is captured from its runtime buffer
                    auto report = [this] (double p)
                    {
                        progress.store (p);
                        return ! cancel.load();
                    };
                    ok = engine.renderTracksRangeToFilesPrepared (trackIds, start, end, files, error,
                                                                  true, true, bypassInserts, report, snap);
                }

                Engine& engine;
                std::vector<int> trackIds;
                std::vector<juce::File> files;
                double start = 0.0, end = 0.0;
                bool bypassInserts = false;
                juce::String error;
                std::shared_ptr<Snap> snap;
                std::atomic<double> progress { 0.0 };
                std::atomic<bool> cancel { false };
                bool ok = true;
            };

            std::vector<int> renderIds;
            std::vector<juce::File> renderFiles;
            for (auto& r : renders)
            {
                renderIds.push_back (r.renderTrackId);
                renderFiles.push_back (r.file);
            }
            FreezeTask task (engine, renderIds, renderFiles, startBeat, endBeat, ! renders[0].renderWithFx);
            task.startThread();

            const auto started = juce::Time::getMillisecondCounter();
            while (task.isThreadRunning() && juce::Time::getMillisecondCounter() - started < 200)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);

            if (task.isThreadRunning())
            {
                double progressValue = 0.0;
                auto* aw = new juce::AlertWindow (TRANS ("Freezing track..."), {}, juce::MessageBoxIconType::NoIcon);
                aw->addProgressBarComponent (progressValue);
                aw->addButton (TRANS ("Cancel"), 1);
                juce::Component::SafePointer<juce::AlertWindow> safe (aw);
                aw->enterModalState (true, juce::ModalCallbackFunction::create ([&task] (int r)
                {
                    if (r == 1) task.cancel.store (true);
                }), true);

                while (task.isThreadRunning())
                {
                    progressValue = task.progress.load();
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
                }
                if (safe != nullptr) safe->exitModalState (0);
            }

            task.stopThread (3000);
            rendered = task.ok && ! task.cancel.load();
            err = task.error;
        }
        else
        {
            std::vector<int> renderIds;
            std::vector<juce::File> renderFiles;
            for (auto& r : renders)
            {
                renderIds.push_back (r.renderTrackId);
                renderFiles.push_back (r.file);
            }
            rendered = engine.renderTracksRangeToFilesPrepared (renderIds, startBeat, endBeat, renderFiles, err,
                                                                true, true, ! renders[0].renderWithFx, {}, snap);
        }
    }

    if (! rendered)
    {
        for (auto& r : renders) r.file.deleteFile();
        freezeBusy = false;
        if (onError && ! err.startsWith (TRANS ("Export cancelled.")))
            onError (TRANS ("Freeze failed:") + " " + err);
        return;
    }

    // decode the frozen files in parallel: each one is a full WAV on disk, so a big multi-output
    // freeze would otherwise spend its time reading them one after the other
    {
        std::atomic<int> failed { 0 };
        const int jobs = juce::jlimit (1, 4, (int) renders.size());
        std::vector<std::thread> loaders;
        for (int k = 0; k < jobs; ++k)
            loaders.emplace_back ([&renders, &failed, &engine = engine, &pool = project.pool, k, jobs] ()
            {
                for (size_t i = (size_t) k; i < renders.size(); i += (size_t) jobs)
                {
                    renders[i].data = pool.load (renders[i].file, engine.formats);
                    if (renders[i].data == nullptr) failed.fetch_add (1);
                }
            });
        for (auto& l : loaders) l.join();
        if (failed.load() != 0)
        {
            for (auto& done : renders) done.file.deleteFile();
            freezeBusy = false;
            return;
        }
    }
    freezeBusy = false;

    // the plug-ins that are about to leave the live chain keep their sound: the slot state is
    // needed when unfreeze (or an undo of the freeze) brings them back. checkpointRemoving
    // captures exactly those slots (state + ARA archive) instead of every plug-in in the project.
    {
        std::set<juce::int64> bakedUids;
        if (withFx)
            for (auto& r : renders)
                if (auto* target = project.findTrack (r.renderTrackId))
                    for (auto& sl : target->inserts) bakedUids.insert (sl.uid);
        if (! bakedUids.empty()) project.checkpointRemoving (bakedUids);
        else project.checkpoint();
    }

    for (auto& r : renders)
    {
        auto* target = project.findTrack (r.renderTrackId);
        if (target == nullptr) { r.file.deleteFile(); continue; }
        if (withFx)
        {
            // move the baked chain out of the live one: effects inserted after this point process
            // the frozen audio in real time (unfreeze puts it back in front of them)
            for (auto& sl : target->inserts) target->frozenChain.push_back (std::move (sl));
            target->inserts.clear();
        }
        target->frozen = true;
        target->frozenNoFx = ! withFx;
        target->frozenStartBeat = startBeat;
        target->frozenFile = r.file;
        target->frozenData = r.data;
        target->frozenBakedLatency = r.bakedLatency;
    }

    project.sendContentChanged();
    project.sendMixChanged();
    engine.syncAll();          // drop the baked plug-in instances right away (that is the point)
    notifySelection();
}

void AppState::unfreezeTrack (int trackId)
{
    auto* t = project.findTrack (trackId);
    if (t == nullptr || ! t->frozen) return;

    // freezing a multi-output instrument froze its mixer channels too: unfreeze them together
    std::vector<int> ids { trackId };
    for (auto& o : project.tracks)
        if (o != nullptr && o->sourceTrack == trackId && o->frozen)
            ids.push_back (o->id);

    project.checkpoint();
    for (int id : ids)
    {
        auto* target = project.findTrack (id);
        if (target == nullptr || ! target->frozen) continue;
        // the baked chain comes back in front of whatever was inserted while the track was frozen
        if (! target->frozenChain.empty())
        {
            std::vector<PluginSlot> restored;
            restored.reserve (target->frozenChain.size() + target->inserts.size());
            for (auto& sl : target->frozenChain) restored.push_back (std::move (sl));
            for (auto& sl : target->inserts) restored.push_back (std::move (sl));
            target->inserts = std::move (restored);
            target->frozenChain.clear();
        }
        target->frozen = false;
        target->frozenNoFx = false;
        target->frozenStartBeat = 0.0;
        target->frozenBakedLatency = 0;
        target->frozenData.reset();
        // Keep the render file: undoing the unfreeze has to be able to play it again. The file is
        // deleted by the next freeze of this track (which reuses the path) or by Project::saveTo
        // once the track is no longer frozen and the undo history is about to be committed.
    }
    project.sendContentChanged();
    project.sendMixChanged();
    engine.syncAll();          // recreate the plug-ins that come back from the frozen chain
    notifySelection();
}

void AppState::freezeToggle (int trackId)
{
    if (auto* t = project.findTrack (trackId))
    {
        if (t->frozen) unfreezeTrack (trackId);
        else freezeTrack (trackId);
    }
}

//==============================================================================
//== audio import: the clip goes in at once, the samples are decoded on a worker thread
bool AppState::importAudio (const juce::File& f, double atBeat, int targetTrackId)
{
    std::unique_ptr<juce::AudioFormatReader> reader (engine.formats.createReaderFor (f));
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        if (onError) onError (TRANS ("Unsupported or unreadable audio file.") + "\n" + f.getFullPathName());
        return false;
    }
    const double sr = juce::jmax (1.0, reader->sampleRate);
    const double lengthSec = (double) reader->lengthInSamples / sr;

    project.checkpoint();
    Track* t = project.findTrack (targetTrackId);
    if (t == nullptr || t->type != TrackType::Audio)
        t = &project.addTrack (TrackType::Audio, f.getFileNameWithoutExtension());

    AudioClip c;
    c.id = project.newId();
    c.name = f.getFileNameWithoutExtension();
    c.file = f;
    c.start = juce::jmax (0.0, atBeat);
    c.lengthSec = lengthSec;
    c.loading = true;                 // the samples follow from the decode thread
    const int tid = t->id;
    t->audioClips.push_back (std::move (c));
    project.sendStructureChanged();
    project.sendContentChanged();
    selectTrack (tid);

    // decode on the background pool: a folder of stems is decoded in parallel, and the window
    // keeps painting while a long file is read
    if (importPool == nullptr)
        importPool = std::make_unique<juce::ThreadPool> (
            juce::ThreadPoolOptions{}
                .withThreadName ("SoundX audio import")
                .withNumberOfThreads (juce::jlimit (1, 3, (int) juce::SystemStats::getNumPhysicalCpus() - 1)));
    auto ctx = importContext;
    Project* proj = &project;
    Engine* eng = &engine;
    importPool->addJob ([proj, eng, f, ctx]
    {
        auto data = proj->pool.load (f, eng->formats);
        juce::MessageManager::callAsync ([ctx, f, data]
        {
            AppState* app = nullptr;
            {
                const std::lock_guard<std::mutex> l (ctx->mutex);
                app = ctx->app;
            }
            if (app != nullptr) app->finishAudioImport (f, data);
        });
    });
    return true;
}

void AppState::finishAudioImport (const juce::File& f, const std::shared_ptr<AudioData>& data)
{
    bool changed = false;
    for (auto& t : project.tracks)
    {
        if (t == nullptr) continue;
        for (auto& c : t->audioClips)
        {
            if (! c.loading || c.file != f) continue;
            c.loading = false;
            if (data != nullptr) c.data = data;
            changed = true;
        }
    }

    if (data == nullptr)
    {
        if (onError) onError (TRANS ("Unsupported or unreadable audio file.") + "\n" + f.getFullPathName());
    }
    if (changed) project.sendContentChanged();
}

void AppState::setTrackDelay (int trackId, double ms)
{
    auto* t = project.findTrack (trackId);
    if (t == nullptr) return;
    ms = juce::jlimit (-2000.0, 2000.0, ms);
    if (std::abs (t->delayMs - ms) < 1.0e-6) return;
    project.checkpoint();
    t->delayMs = ms;
    project.sendContentChanged();
    project.sendMixChanged();
}

namespace
{
    // The visible row closest to the old selection: after a hide the selection should stay next to
    // where it was instead of jumping to the top of the session.
    int nearestVisibleTrackId (Project& project, int oldIndex)
    {
        const int n = (int) project.tracks.size();
        if (n == 0) return -1;
        oldIndex = juce::jlimit (0, n - 1, juce::jmax (0, oldIndex));
        auto shown = [&project] (int i)
        {
            return project.tracks[(size_t) i] != nullptr && project.tracks[(size_t) i]->shownInArranger();
        };
        for (int d = 0; d < n; ++d)
        {
            const int lo = oldIndex - d, hi = oldIndex + d;
            if (lo >= 0 && shown (lo)) return project.tracks[(size_t) lo]->id;
            if (d > 0 && hi < n && shown (hi)) return project.tracks[(size_t) hi]->id;
        }
        return -1;
    }
}

void AppState::setTracksHidden (const std::vector<int>& ids, bool hidden)
{
    // Multi-output mixer channels keep their 'hidden' flag untouched: they are arranger-hidden by
    // design and managed by the "output channels in the mixer" switch of their instrument track.
    std::vector<int> changed;
    for (int id : ids)
        if (auto* t = project.findTrack (id))
            if (isArrangerTrack (*t) && t->hidden != hidden) changed.push_back (id);
    if (changed.empty()) return;
    project.checkpoint();
    for (int id : changed)
        if (auto* t = project.findTrack (id)) t->hidden = hidden;
    project.sendStructureChanged();
    project.sendContentChanged();

    // An invisible row cannot be clicked and must not be the target of the next "selected tracks"
    // command: drop the rows that just left the arranger from the selection and move the anchor to
    // the nearest visible row (or to nothing when every row is hidden).
    const int anchorIndex = selectedTrack >= 0 ? project.indexOfTrack (selectedTrack) : -1;
    if (selectedTrack >= 0)
        if (auto* anchor = project.findTrack (selectedTrack);
            anchor == nullptr || ! anchor->shownInArranger())
            selectedTrack = nearestVisibleTrackId (project, anchorIndex);
    for (auto it = selectedTracks.begin(); it != selectedTracks.end();)
    {
        auto* t = project.findTrack (*it);
        if (t == nullptr || ! t->shownInArranger()) it = selectedTracks.erase (it);
        else ++it;
    }
    if (selectedTrack >= 0) selectedTracks.insert (selectedTrack);
    else if (! selectedTracks.empty()) selectedTrack = *selectedTracks.begin();
    engine.setSelectedTrack (selectedTrack);
    notifySelection();
}

int AppState::hiddenTrackCount() const
{
    int n = 0;
    for (auto& t : project.tracks)
        if (t != nullptr && t->hidden && isArrangerTrack (*t)) ++n;
    return n;
}

std::vector<int> AppState::hiddenArrangerTrackIds() const
{
    std::vector<int> ids;
    for (auto& t : project.tracks)
        if (t != nullptr && t->hidden && isArrangerTrack (*t)) ids.push_back (t->id);
    return ids;
}

std::vector<int> AppState::selectedClipTrackIds() const
{
    std::vector<int> ids;
    if (selectedClips.empty()) return ids;
    for (auto& t : project.tracks)
    {
        if (t == nullptr) continue;
        bool owns = false;
        for (auto& c : t->midiClips)
            if (selectedClips.count (c.id) > 0) { owns = true; break; }
        if (! owns)
            for (auto& c : t->audioClips)
                if (selectedClips.count (c.id) > 0) { owns = true; break; }
        if (owns) ids.push_back (t->id);
    }
    return ids;
}

//==============================================================================
bool AppState::moveOrCopySlot (int fromTrackId, juce::int64 uid, bool instrument, int toTrackId, int toIndex)
{
    auto* fromTrack = fromTrackId < 0 ? nullptr : project.findTrack (fromTrackId);
    auto* toTrack = toTrackId < 0 ? nullptr : project.findTrack (toTrackId);
    if (fromTrackId >= 0 && fromTrack == nullptr) return false;
    if (toTrackId >= 0 && toTrack == nullptr) return false;

    PluginSlot* src = fromTrackId < 0 ? nullptr : fromTrack->findSlot (uid);
    if (src == nullptr && fromTrackId >= 0) return false;
    if (fromTrackId < 0)
        for (auto& s : project.masterTrack.inserts) if (s.uid == uid) src = &s;
    if (src == nullptr) return false;

    //== same track: move (reorder) ------------------------------------------------------------
    if (fromTrackId == toTrackId)
    {
        if (instrument) return false;                     // the instrument slot has no order to change
        auto& vec = toTrackId < 0 ? project.masterTrack.inserts : toTrack->inserts;
        int from = -1;
        for (size_t i = 0; i < vec.size(); ++i) if (vec[i].uid == uid) from = (int) i;
        if (from < 0 || from == toIndex) return false;
        toIndex = juce::jlimit (0, (int) vec.size() - 1, toIndex);
        if (from == toIndex) return false;
        engine.moveInsert (toTrackId, from, toIndex);
        return true;
    }

    //== another track: copy the plug-in with what it currently holds ---------------------------
    // the live instance owns the real settings, so take its state first (the slot's saved state
    // is only a snapshot from the last save)
    juce::MemoryBlock state = src->state;
    if (auto inst = engine.getInstance (uid))
        if (inst->proc != nullptr)
            try { state.reset(); inst->proc->getStateInformation (state); } catch (...) {}

    const auto desc = src->desc;
    const bool builtIn = src->builtIn;
    const float gainDb = src->gainDb;
    const float pan = src->pan;

    project.beginUndoBatch();
    juce::String err;
    bool ok = false;
    {
        ok = instrument ? engine.addInstrument (toTrackId, desc, err) : engine.addInsert (toTrackId, desc, err);
    }

    if (ok)
    {
        // the new slot is appended: move it to the drop position, then hand over the state
        auto& vec = toTrackId < 0 ? project.masterTrack.inserts : toTrack->inserts;
        if (! instrument)
        {
            const int last = (int) vec.size() - 1;
            const int want = juce::jlimit (0, last, toIndex);
            if (last >= 0 && want != last) engine.moveInsert (toTrackId, last, want);
        }
        else if (toTrack == nullptr || ! toTrack->hasInstrument)
        {
            // addInstrument falls back to the primary slot on a track without any instrument
        }

        PluginSlot* copy = nullptr;
        if (instrument)
        {
            // the copy is either the primary slot (track was empty) or the last stacked one
            copy = toTrack != nullptr ? (toTrack->extraInstruments.empty() ? (toTrack->hasInstrument ? &toTrack->instrument : nullptr)
                                                                          : &toTrack->extraInstruments.back())
                                      : nullptr;
        }
        else if (! vec.empty())
            copy = &vec[(size_t) juce::jlimit (0, (int) vec.size() - 1, toIndex)];

        if (copy != nullptr)
        {
            copy->state = state;
            copy->builtIn = builtIn;
            copy->bypass = src->bypass;      // a bypassed plug-in is copied bypassed
            copy->gainDb = gainDb;
            copy->pan = pan;
            if (auto inst = engine.getInstance (copy->uid))
                if (inst->proc != nullptr && state.getSize() > 0)
                    try { inst->proc->setStateInformation (state.getData(), (int) state.getSize()); } catch (...) {}
            project.sendStructureChanged();
        }
    }

    // a failed load is reported through the batch: undo it so the project is unchanged
    if (! ok && onError) onError (TRANS ("Could not copy the plug-in:") + " " + err);
    project.endUndoBatch();
    return ok;
}

//==============================================================================
namespace
{
    /** The extra output buses of an instrument: channel offset in its output + bus name. */
    struct ExtraOut { int channel; juce::String name; };

    std::vector<ExtraOut> extraOutputsOf (juce::AudioProcessor* p)
    {
        std::vector<ExtraOut> out;
        if (p == nullptr) return out;

        int offset = 0;
        for (int i = 0; i < p->getBusCount (false); ++i)
        {
            auto* b = p->getBus (false, i);
            if (b == nullptr) continue;
            const int chans = (int) b->getCurrentLayout().size();
            // bus 0 is the main pair (0/1); every further stereo bus becomes a mixer channel.
            // Mono buses are skipped: a channel track mixes a stereo pair.
            if (i > 0 && chans >= 2) out.push_back ({ offset, b->getName() });
            offset += chans;
        }
        return out;
    }
}

int AppState::instrumentOutputPairs (int trackId)
{
    auto* t = project.findTrack (trackId);
    if (t == nullptr || ! t->hasInstrument) return 0;
    // stacked instruments mix into the track's main pair: their extra outputs cannot be split
    if (t->numInstruments() > 1) return 0;
    auto inst = engine.getInstance (t->instrument.uid);
    return inst != nullptr ? (int) extraOutputsOf (inst->proc.get()).size() : 0;
}

int AppState::expandOutputChannels (int trackId)
{
    auto* t = project.findTrack (trackId);
    if (t == nullptr || ! t->hasInstrument) return 0;
    if (t->frozen)
    {
        // the frozen render is the instrument's main pair: a channel added now would have no
        // input (unfreezing first re-renders all channels together)
        if (onError) onError (TRANS ("Unfreeze the track before adding output channels."));
        return 0;
    }
    if (t->numInstruments() > 1)
    {
        if (onError) onError (TRANS ("Output channels need a track with a single instrument: "
                                     "remove the stacked instruments first."));
        return 0;
    }
    auto inst = engine.getInstance (t->instrument.uid);
    if (inst == nullptr) return 0;

    const auto extras = extraOutputsOf (inst->proc.get());
    if (extras.empty()) return 0;

    const juce::String baseName = t->name;
    int made = 0;
    for (auto& e : extras)
    {
        bool exists = false;
        for (auto& o : project.tracks)
            if (o != nullptr && o->sourceTrack == trackId && o->sourceChannel == e.channel) { exists = true; break; }
        if (exists) continue;

        if (made == 0) project.checkpoint();     // the whole expansion is one undo step
        const juce::String name = baseName + " " + (e.name.isNotEmpty() ? e.name
                                                                       : juce::String ("Out ") + juce::String (e.channel / 2 + 1));
        project.addOutputChannel (trackId, e.channel, name);
        ++made;
    }

    // re-fetch: adding tracks reallocates the track list
    if (auto* parent = project.findTrack (trackId)) parent->showOutputChannels = true;
    if (made > 0)
    {
        project.sendStructureChanged();
        project.sendContentChanged();
        project.sendMixChanged();
        engine.syncAll();
        notifyView();
    }
    return made;
}

int AppState::collapseOutputChannels (int trackId)
{
    if (project.numOutputChannels (trackId) == 0) return 0;
    project.checkpoint();
    const int n = project.removeOutputChannels (trackId);
    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    engine.syncAll();
    notifySelection();
    return n;
}

void AppState::toggleOutputChannelsInMixer (int trackId)
{
    auto* t = project.findTrack (trackId);
    if (t == nullptr) return;

    if (project.numOutputChannels (trackId) == 0) { expandOutputChannels (trackId); return; }

    t->showOutputChannels = ! t->showOutputChannels;
    project.sendStructureChanged();     // the mixer rebuilds from this
    project.sendMixChanged();
}

void AppState::showPluginEditor (juce::int64 uid)
{
    juce::String title;
    for (auto& t : project.tracks)
        if (t->findSlot (uid) != nullptr) { title = t->findSlot (uid)->desc.name + " - " + t->name; break; }
    windows->show (uid, title);
}

juce::int64 AppState::lastInsertUid (int trackId) const
{
    if (trackId < 0)
        return project.masterTrack.inserts.empty() ? 0 : project.masterTrack.inserts.back().uid;

    if (auto* t = project.findTrack (trackId))
        if (! t->inserts.empty())
            return t->inserts.back().uid;

    return 0;
}

void AppState::showInsertedPluginEditor (juce::int64 uid)
{
    if (! openPluginEditorOnInsert || uid == 0) return;

    // Opening the editor can take a while for a big plug-in. Inserting is complete at this point;
    // let the arrangement / mixer paint the new slot first and create the panel on the next turn.
    juce::String title;
    for (auto& t : project.tracks)
        if (auto* slot = t->findSlot (uid)) { title = slot->desc.name + " - " + t->name; break; }

    windows->showAsync (uid, title);
}
}
