#pragma once

#include <vector>

namespace sx
{
enum Cmd
{
    CmdNone = 0,
    CmdNewProject = 1, CmdOpenProject, CmdSaveProject, CmdSaveProjectAs,
    CmdNewFromTemplate, CmdSaveAsTemplate, CmdDeleteTemplate, CmdProjectInfo, CmdMidiLearn,
    CmdImportMidi, CmdImportAudio, CmdExportMidi, CmdExportMix, CmdExportStems, CmdQuit,
    CmdSetRecordingFolder,
    CmdUndo, CmdRedo, CmdCut, CmdCopy, CmdPaste, CmdDelete, CmdSelectAll, CmdDuplicate, CmdSplit,
    CmdQuantize, CmdHumanize, CmdHumanizeDialog, CmdReverseNotes, CmdSetLoopToSelection,
    CmdNewInstrumentTrack, CmdNewAudioTrack, CmdDeleteTrack, CmdDuplicateTrack, CmdAddTracks,
    CmdNewBusTrack, CmdNewSendTrack, CmdGroupIntoBus, CmdGroupIntoFolder, CmdUnpackFolder,
    CmdAddAutomationTrack, CmdToggleMasterPost, CmdAddListenBus, CmdRemoveListenBus, CmdToggleMasterTrack,
    CmdHideTracks, CmdShowAllTracks,
    CmdToggleMultiCore,
    CmdMergeClips,
    CmdPlayStop, CmdStop, CmdRecord, CmdRewind, CmdToggleLoop, CmdToggleMetronome, CmdToggleCountIn,
    CmdCountInOff, CmdCountIn1Bar, CmdCountIn2Bars, CmdCountIn4Bars, CmdCountIn8Bars,
    // MIDI record quantize: Off / 1/4 / 1/8 / 1/8T / 1/16 / 1/16T / 1/32
    CmdRecQuantOff, CmdRecQuant4, CmdRecQuant8, CmdRecQuant8T, CmdRecQuant16, CmdRecQuant16T, CmdRecQuant32,
    CmdToggleTakeRecording,
    CmdTimebaseBars, CmdTimebaseSeconds,
    CmdToggleInspector, CmdToggleMixer, CmdTogglePianoRoll, CmdToggleKeyboardTyping, CmdToggleMediaPool,
    CmdZoomIn, CmdZoomOut, CmdZoomFit, CmdTrackTaller, CmdTrackShorter, CmdToggleFollow, CmdToggleTempoTrack,
    CmdTrackHeightXS, CmdTrackHeightS, CmdTrackHeightM, CmdTrackHeightL, CmdTrackHeightXL,
    CmdPianoRollFull,
    CmdToggleDarkMode,
    CmdToggleAppFullscreen,
    CmdTogglePluginEditorOnInsert, CmdToggleMultiPluginWindows,
    CmdAudioSettings, CmdPluginManager, CmdAbout, CmdUserManual, CmdLanguageAuto, CmdLanguageEnglish, CmdLanguageChinese,
    CmdSplitAtCursor, CmdAddCrossfade, CmdFadeInToPlayhead, CmdFadeOutFromPlayhead, CmdClearFades,
    CmdToggleChordTrack, CmdToggleMarkerTrack, CmdTogglePianoRollChords,
    CmdChordEnter, CmdMarkerEnter,
    CmdToggleIdleDeviceRelease,

    // multi-core render core count (Automatic / Off / a fixed number of cores)
    CmdRenderCoresAuto = 760, CmdRenderCoresOff, CmdRenderCores2 = 770,

    // audio editing (reverse / speed / transpose on the selected audio clips)
    CmdReverseAudio = 700, CmdAudioStretchPrompt, CmdAudioPitchPrompt, CmdAudioResetEdits,
    CmdAudioStretchPreset = 720,   // + index into audioStretchPresets()
    CmdAudioPitchPreset = 740      // + index into audioPitchPresets()
};

// speed factors offered in the menus (1.0 = original speed, 2.0 = half speed / twice as long)
inline const std::vector<double>& audioStretchPresets()
{
    static const std::vector<double> v { 0.5, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 2.0 };
    return v;
}

inline const char* const* audioStretchPresetNames()
{
    static const char* const n[] { "0.50x", "0.75x", "0.90x", "1.00x", "1.10x", "1.25x", "1.50x", "2.00x" };
    return n;
}

// transposes offered in the menus, in semitones
inline const std::vector<int>& audioPitchPresets()
{
    static const std::vector<int> v { -12, -5, -2, -1, 0, 1, 2, 5, 12 };
    return v;
}

inline const char* const* audioPitchPresetNames()
{
    static const char* const n[] { "-12", "-5", "-2", "-1", "0", "+1", "+2", "+5", "+12" };
    return n;
}
}
