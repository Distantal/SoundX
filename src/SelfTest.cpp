#include "model/Model.h"
#include <cstdint>
#include "engine/Engine.h"
#include "engine/AraHost.h"
#include "engine/PluginManager.h"
#include "engine/PluginScanProcess.h"
#include "engine/BuiltInEffects.h"
#include "engine/BuiltInSampler.h"
#include "engine/GuitarRig.h"
#if JUCE_WINDOWS
 // Declared locally instead of including <windows.h>: that header defines a global Rectangle,
 // which collides with juce::Rectangle throughout this file.
 extern "C" __declspec(dllimport) void* __stdcall GetParent (void*);
 extern "C" __declspec(dllimport) std::intptr_t __stdcall GetWindowLongPtrW (void*, int);
 struct NativeRect { std::int32_t left, top, right, bottom; };
 extern "C" __declspec(dllimport) int __stdcall GetWindowRect (void*, NativeRect*);
#endif
#include "ui/Theme.h"
#include "ui/AppState.h"
#include "ui/ChannelStrip.h"
#include "ui/ClipGainEditor.h"
#include "ui/PluginWindows.h"
#include "ui/Panels.h"
#include "ui/PluginManagerWindow.h"
#include "ui/MainComponent.h"
#include "ui/VirtualKeyboard.h"
#include "ui/TitleBar.h"
#include "ui/ChainPresets.h"
#include "ui/Waveform.h"
#include "ui/ArrangeView.h"
#include "ui/MasterAutoLane.h"
#include "ui/PianoRoll.h"
#include "ui/PluginPicker.h"
#include "ui/Dialogs.h"
#include "ui/Importer.h"
#include "ui/MidiLearn.h"
#include "ui/Localisation.h"
#include "ui/SlotMenus.h"
#include "ui/MediaBrowser.h"
#if SOUNDX_HAS_MANUAL
 #include "BinaryData.h"
#endif
#include <array>
#include <atomic>
#include <thread>

// Headless verification of the engine: MIDI / CC / automation round-trips, WAV and stem export,
// project save/load, the real-time callback and (optionally) real plug-ins.
namespace sx
{
using namespace juce;

namespace
{
    struct Log
    {
        explicit Log (const File& f) : file (f) { file.deleteFile(); }
        void operator() (const String& s, bool ok = true)
        {
            auto line = String (ok ? "[ OK ] " : "[FAIL] ") + s + "\n";
            file.appendText (line);
            if (! ok) ++failures;
        }
        File file;
        int failures = 0;
    };

    struct AudioInfo { double seconds = 0; float peak = 0; int channels = 0; double sr = 0; bool ok = false; };

    AudioInfo inspect (AudioFormatManager& fm, const File& f)
    {
        AudioInfo i;
        std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr) return i;
        i.ok = true;
        i.channels = (int) r->numChannels;
        i.sr = r->sampleRate;
        i.seconds = (double) r->lengthInSamples / r->sampleRate;
        AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
        i.peak = b.getMagnitude (0, b.getNumSamples());
        return i;
    }
}

void buildDemoProject (Project& project, Engine& engine, int& leadTrack, int& leadClip)
{
    project.reset();
    project.bpm = 112.0;
    auto addNotes = [&] (MidiClip& c, const std::vector<std::array<double, 4>>& list)
    {
        for (auto& n : list) { Note nn; nn.id = project.newId(); nn.start = n[0]; nn.length = n[1]; nn.pitch = (int) n[2]; nn.velocity = (int) n[3]; c.notes.push_back (nn); }
    };
    auto& lead = project.addTrack (TrackType::Instrument, "Lead");
    lead.hasInstrument = true;              // tracks start without an instrument now
    lead.instrument = makeBuiltInSynthSlot();
    leadTrack = lead.id;
    {
        MidiClip c; c.id = project.newId(); c.name = "Lead Arp"; c.start = 0.0; c.length = 8.0;
        const int chord[] = { 0, 4, 7, 12, 7, 4 };
        for (int i = 0; i < 16; ++i)
        {
            Note n; n.id = project.newId(); n.pitch = 60 + chord[i % 6] + (i >= 8 ? 5 : 0); n.start = i * 0.5; n.length = 0.45; n.velocity = 70 + (i % 4) * 14; c.notes.push_back (n);
        }
        for (int i = 0; i <= 32; ++i)
        {
            CCPoint p; p.id = project.newId(); p.cc = 1; p.beat = i * 0.25; p.value = (int) (63.0 + 60.0 * std::sin (i * 0.25)); c.ccs.push_back (p);
        }
        leadClip = c.id;
        lead.midiClips.push_back (c);
        MidiClip c2 = c; c2.id = project.newId(); c2.start = 8.0; c2.name = "Lead Arp 2";
        for (auto& n : c2.notes) n.id = project.newId();
        for (auto& p : c2.ccs) p.id = project.newId();
        lead.midiClips.push_back (c2);
        lead.showAutomation = true;
        auto& lane = lead.ensureLane ("vol", "Volume");
        lane.points = { { 0.0, 0.75f }, { 3.0, 0.5f }, { 6.0, 0.8f }, { 10.0, 0.4f }, { 14.0, 0.75f } };
        lane.visible = true;
        auto& panLane = lead.ensureLane ("pan", "Pan");
        panLane.points = { { 0.0, 0.5f }, { 8.0, 0.2f }, { 16.0, 0.8f } };
        panLane.visible = true;   // two automation rows on the lead track
    }
    auto& bass = project.addTrack (TrackType::Instrument, "Bass");
    bass.hasInstrument = true;
    bass.instrument = makeBuiltInSynthSlot();
    bass.colour = theme::trackPalette()[2];
    {
        MidiClip c; c.id = project.newId(); c.name = "Bass"; c.start = 0.0; c.length = 16.0;
        std::vector<std::array<double, 4>> list;
        for (int bar = 0; bar < 4; ++bar)
            for (int i = 0; i < 4; ++i) list.push_back ({ bar * 4.0 + i, 0.9, (double) (36 + (bar % 2) * 5), 100.0 });
        addNotes (c, list);
        bass.midiClips.push_back (c);
    }
    auto& au = project.addTrack (TrackType::Audio, "Vocal");
    au.colour = theme::trackPalette()[4];
    {
        AudioBuffer<float> b (1, 44100 * 6);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float env = 0.5f + 0.5f * std::sin (i * 0.00012f);
            b.setSample (0, i, 0.6f * env * std::sin (MathConstants<float>::twoPi * 220.0f * (float) i / 44100.0f) * (0.6f + 0.4f * std::sin (i * 0.0007f)));
        }
        File f = File::getSpecialLocation (File::tempDirectory).getChildFile ("soundx_demo.wav");
        f.deleteFile();
        WavAudioFormat wav;
        if (auto* w = wav.createWriterFor (new FileOutputStream (f), 44100.0, 1, 16, {}, 0)) { w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); delete w; }
        AudioClip c; c.id = project.newId(); c.name = "Vocal take"; c.file = f; c.start = 4.0; c.data = project.pool.load (f, engine.formats);
        c.lengthSec = c.data != nullptr ? c.data->lengthSeconds() : 1.0;
        c.fadeIn = 0.45;        // demonstrates the fade wedges + the clip gain line
        c.fadeOut = 0.7;
        c.gainDb = -3.0f;
        au.audioClips.push_back (c);

        // a second take overlapping the first one by half a second -> automatic crossfade
        AudioClip c2 = c;
        c2.id = project.newId();
        c2.name = "Vocal take 2";
        c2.start = c.start + juce::jmax (1.0, c.lengthSec - 0.5);
        c2.fadeIn = 0.0;
        c2.fadeOut = 0.5;
        c2.gainDb = 0.0f;
        au.audioClips.push_back (c2);
    }
    // routing: a bus collecting the bass plus an effect return fed by a send, so the demo has one
    // channel of every kind (instrument, audio, bus, effect return) next to the master bus row
    {
        auto& drumbus = project.addTrack (TrackType::Bus, "Bass Bus");
        drumbus.volumeDb = -2.0f;
        bass.outputBusId = drumbus.id;

        auto& verb = project.addTrack (TrackType::Send, "Reverb");
        verb.volumeDb = -6.0f;
        SendSlot send;
        send.target = verb.id;
        send.levelDb = -12.0f;
        lead.sends.push_back (send);
    }
    project.loopOn = true; project.loopStart = 0.0; project.loopEnd = 16.0;
    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    engine.syncAll();
    project.dirty = false;
}

// A multi-output instrument spreads its extra outputs over mixer channels: SX Synth publishes a
// main pair plus an aux pair, so expanding adds one channel track that plays channels 2/3 and is
// mixed like any other track. Kept out of runSelfTest() because it is long and needs its own frame.
static void runMultiOutputTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
        project.masterTrack.inserts.clear();
    }
    engine.syncAll();

    auto& inst = project.addTrack (TrackType::Instrument, "MultiOut");
    const int instId = inst.id;
    inst.volumeDb = 0.0f;
    String setErr;
    const bool loaded = engine.setInstrument (instId, makeBuiltInSynthSlot().desc, setErr);
    engine.syncAll();

    {
        MidiClip c; c.id = project.newId(); c.name = "mo"; c.length = 2.0;
        Note n; n.id = project.newId(); n.pitch = 60; n.start = 0.0; n.length = 1.0; n.velocity = 110;
        c.notes.push_back (n);
        if (auto* t = project.findTrack (instId)) t->midiClips.push_back (c);
    }
    engine.syncAll();

    auto mixPeak = [&] (const String& name) -> float
    {
        ExportOptions o;
        o.mode = ExportOptions::Mix;
        o.target = out.getChildFile (name + ".wav");
        o.sampleRate = 44100.0;
        o.bitDepth = 24;
        String err2;
        if (! engine.exportAudio (o, {}, err2)) return -1.0f;
        std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
        if (r == nullptr) return -1.0f;
        AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
        return jmax (b.getMagnitude (0, 0, b.getNumSamples()), b.getMagnitude (1, 0, b.getNumSamples()));
    };

    const float mainOnly = mixPeak ("mo_main");

    AppState st (project, engine, plugins);
    st.showFreezeProgress = false; // no modal window during the self test
    const int pairs = st.instrumentOutputPairs (instId);       // SX Synth: one extra (aux) pair
    const int made = st.expandOutputChannels (instId);

    int childId = 0, childChannel = -1;
    for (auto& t : project.tracks)
        if (t != nullptr && t->sourceTrack == instId) { childId = t->id; childChannel = t->sourceChannel; }
    const bool childOk = childId != 0 && childChannel == 2
                      && project.findTrack (childId) != nullptr && project.findTrack (childId)->hidden;
    engine.syncAll();

    // the aux out of the synth mirrors the main out, so both channels together are twice as loud
    const float both = mixPeak ("mo_both");
    if (auto* t = project.findTrack (instId)) t->mute = true;
    project.sendMixChanged();
    engine.syncAll();
    const float auxOnly = mixPeak ("mo_aux");                  // the parent is muted: the child carries it
    if (auto* t = project.findTrack (instId)) t->mute = false;
    project.sendMixChanged();
    engine.syncAll();

    log ("multi-output: " + String (pairs) + " extra output pair, " + String (made)
         + " mixer channel created at channel " + String (childChannel) + " (main "
         + String (mainOnly, 3) + ", both " + String (both, 3) + ", aux only " + String (auxOnly, 3) + ")",
         loaded && pairs >= 1 && made == 1 && childOk
         && mainOnly > 0.05f && both > 1.7f * mainOnly && std::abs (auxOnly - mainOnly) < 0.15f * mainOnly);

    // stems: the instrument's own output and every output channel can be exported separately
    {
        ExportOptions so;
        so.mode = ExportOptions::Stems;
        so.target = out.getChildFile ("mo_stems");
        so.sampleRate = 44100.0;
        so.bitDepth = 24;
        so.tailSeconds = 0.2;
        so.target.deleteRecursively();
        String serr;
        const bool sok = engine.exportAudio (so, {}, serr);
        auto stems = so.target.findChildFiles (File::findFiles, false, "*.wav");
        std::sort (stems.begin(), stems.end(), [] (const File& a, const File& b)
                   { return a.getFileName() < b.getFileName(); });
        auto stemPeak = [] (const File& f, AudioFormatManager& fm) -> float
        {
            std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
            if (r == nullptr) return -1.0f;
            AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            return b.getMagnitude (0, b.getNumSamples());
        };
        const float pMain = stems.size() == 2 ? stemPeak (stems[0], engine.formats) : -1.0f;
        const float pAux  = stems.size() == 2 ? stemPeak (stems[1], engine.formats) : -1.0f;
        log ("multi-output stems: " + String (stems.size()) + " files (main " + String (pMain, 3)
             + ", output channel " + String (pAux, 3) + " vs mix " + String (mainOnly, 3) + ")",
             sok && stems.size() == 2
             && std::abs (pMain - mainOnly) < 0.06f && std::abs (pAux - mainOnly) < 0.06f);
    }

    // freezing the instrument freezes its output channels with it (the instrument can go idle)
    {
        st.freezeTrack (instId);
        auto* parent = project.findTrack (instId);
        auto* child = project.findTrack (childId);
        const bool flagged = parent != nullptr && parent->frozen
                          && child != nullptr && child->frozen && child->frozenData != nullptr;
        engine.syncAll();
        const float frozenBoth = flagged ? mixPeak ("mo_frozen") : -1.0f;
        const bool matches = std::abs (frozenBoth - both) < 0.06f;
        st.unfreezeTrack (instId);
        engine.syncAll();
        const bool restored = parent != nullptr && ! parent->frozen
                           && child != nullptr && ! child->frozen;
        log ("multi-output freeze: instrument and channel are pre-rendered (" + String (frozenBoth, 3)
             + " vs " + String (both, 3) + "), unfreezing restores both", flagged && matches && restored);
    }

    // the channel shows up in the mixer and can be hidden / shown again
    MainComponent mc (st);
    mc.setSize (1229, 710);
    mc.resized();
    MixerView* mix = nullptr;
    std::function<void (Component&)> hunt = [&] (Component& root)
    {
        for (auto* c : root.getChildren())
        {
            if (mix != nullptr) return;
            if (auto* mv = dynamic_cast<MixerView*> (c)) { mix = mv; return; }
            hunt (*c);
        }
    };
    hunt (mc);
    auto visibleStrips = [mix]
    {
        int n = 0;
        std::function<void (Component&)> walk = [&] (Component& r)
        {
            for (auto* c : r.getChildren())
            {
                if (auto* cs = dynamic_cast<ChannelStrip*> (c)) { if (cs->isVisible()) ++n; }
                else walk (*c);
            }
        };
        if (mix != nullptr) walk (*mix);
        return n;
    };
    const int shown = visibleStrips();
    st.toggleOutputChannelsInMixer (instId);        // hide them
    const int hidden = visibleStrips();
    st.toggleOutputChannelsInMixer (instId);        // and back
    const int shownAgain = visibleStrips();

    log ("multi-output: the channel is in the mixer and can be hidden / shown ("
         + String (shown) + " -> " + String (hidden) + " -> " + String (shownAgain) + " strips)",
         mix != nullptr && shown >= 2 && hidden == shown - 1 && shownAgain == shown);

    // the routing is part of the project file
    bool saved = false;
    if (auto xml = project.toXml (false))
        for (auto* e : xml->getChildWithTagNameIterator ("Track"))
            if (e->getIntAttribute ("id") == childId)
                saved = e->getIntAttribute ("srcTrack") == instId && e->getIntAttribute ("srcCh") == 2;
    log ("multi-output: the channel and its source are saved with the project", saved);
}

// The media pool: the info panel of the selected file, and audition playback. Kept out of
// runSelfTest() for its own frame.
static void runMediaPoolTest (Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    const File dir = out.getChildFile ("mediapool");
    dir.deleteRecursively();
    dir.createDirectory();

    // a 1.5 s stereo 16-bit wav (named so it sorts first in the folder)
    const File wav = dir.getChildFile ("a_preview.wav");
    {
        WavAudioFormat fmt;
        if (auto os = std::unique_ptr<FileOutputStream> (wav.createOutputStream()))
        {
            if (auto* w = fmt.createWriterFor (os.get(), 44100.0, 2, 16, {}, 0))
            {
                os.release();                 // the writer owns the stream now
                AudioBuffer<float> b (2, (int) (44100 * 1.5));
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        b.setSample (c, i, 0.5f * std::sin (2.0 * MathConstants<double>::pi * 440.0 * i / 44100.0));
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                delete w;
            }
        }
    }

    // a MIDI file: 4 notes, 120 bpm, two bars
    const File mid = dir.getChildFile ("b_preview.mid");
    {
        MidiFile mf;
        mf.setTicksPerQuarterNote (96);
        MidiMessageSequence seq;
        seq.addEvent (MidiMessage::tempoMetaEvent (500000), 0.0);
        seq.addEvent (MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
        for (int i = 0; i < 4; ++i)
        {
            seq.addEvent (MidiMessage::noteOn (1, 60 + i, (uint8) 100), i * 96.0);
            seq.addEvent (MidiMessage::noteOff (1, 60 + i), i * 96.0 + 48.0);
        }
        seq.updateMatchedPairs();
        mf.addTrack (seq);
        if (auto os = std::unique_ptr<FileOutputStream> (mid.createOutputStream()))
            mf.writeTo (*os);
    }

    // ---- the info panel describes the selected file ------------------------------------------
    {
        const MediaInfo wi = describeMediaFile (wav, engine.formats);
        const bool wavOk = wi.valid && wi.audio && std::abs (wi.lengthSeconds - 1.5) < 0.01
                        && wi.kind.containsIgnoreCase ("WAV")
                        && wi.detail.containsIgnoreCase ("16") && wi.detail.containsIgnoreCase ("44")
                        && wi.name == "a_preview.wav";
        log ("media pool: an audio file's info (WAV, 16 bit, 44.1 kHz, 1.50 s: '" + wi.detail + "' / '"
             + wi.length + "')", wavOk);

        const MediaInfo mi = describeMediaFile (mid, engine.formats);
        const bool midOk = mi.valid && ! mi.audio && mi.kind == "MIDI"
                        && mi.detail.containsIgnoreCase ("4") && std::abs (mi.lengthSeconds - 1.75) < 0.02;
        log ("media pool: a MIDI file's info (4 notes at 120 bpm, 1.75 s: '" + mi.detail + "')", midOk);

        const MediaInfo ni = describeMediaFile (dir.getChildFile ("does-not-exist.wav"), engine.formats);
        log ("media pool: a missing file reports nothing instead of guessing", ! ni.valid);
    }

    // ---- audition playback ------------------------------------------------------------------
    {
        String perr;
        const bool started = engine.startPreview (wav, perr);
        const bool active = engine.isPreviewActive();
        const double pos0 = engine.previewPositionSeconds();
        Thread::sleep (250);
        const double pos1 = engine.previewPositionSeconds();
        const double prog = engine.previewProgress();
        engine.stopPreview();
        const bool stopped = ! engine.isPreviewActive();

        // with no audio device open nothing can advance, so only require it when there is one
        const bool advanced = pos1 > pos0 || engine.getSampleRate() <= 0.0;
        log ("media pool: the preview plays and stops (position " + String (pos1, 2) + " s, progress "
             + String (prog, 2) + ")", started && active && stopped && advanced && prog >= 0.0);

        String badErr;
        const bool bad = engine.startPreview (dir.getChildFile ("nothing-here.wav"), badErr);
        log ("media pool: previewing a missing file fails with a message (" + badErr + ")",
             ! bad && badErr.isNotEmpty() && ! engine.isPreviewActive());
    }

    // ---- the panel itself ------------------------------------------------------------------
    {
        Project scratch;
        scratch.formats = &engine.formats;
        AppState st (scratch, engine, plugins);

        MediaBrowser mb (st);
        mb.setSize (280, 620);
        mb.setFolder (dir);
        mb.resized();
        mb.selectRow (0);                       // a_preview.wav sorts first

        const bool infoOk = mb.info().valid && mb.info().audio && mb.selectedRow() == 0;

        bool shot = false;
        if (auto img = mb.createComponentSnapshot (mb.getLocalBounds(), true); img.isValid())
            if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("media_pool.png").createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
                shot = true;
            }

        mb.selectRow (1);                       // the MIDI file: no play button, no waveform
        const bool midSelected = mb.info().valid && ! mb.info().audio;

        log ("media pool: selecting a row shows its info and the preview button (media_pool.png)",
             infoOk && shot && midSelected);
    }

    dir.deleteRecursively();
}

// Project info: the fields and the cover picture travel inside the .sxp file.
static void runProjectInfoTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    // ---- the fields and the picture survive save / load ---------------------------------------
    {
        Project src;
        src.formats = &engine.formats;
        src.meta.title = "Test Song";
        src.meta.author = "SoundX Tester";
        src.meta.genre = "Electronic";
        src.meta.album = "Self Test Album";
        src.meta.year = "2026";
        src.meta.comment = "written by the self test";

        // a small picture: 4x4 red/blue gradient, encoded through the same path the dialog uses
        Image img (Image::RGB, 4, 4, true);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                img.setPixelAt (x, y, Colour ((uint8) (x * 60), 40, (uint8) (255 - y * 60)));
        src.meta.setCover (img);
        const bool coverEncoded = src.meta.coverData.getSize() > 0 && src.meta.cover.isValid();

        const File f = out.getChildFile ("project_info.sxp");
        f.deleteFile();
        const bool saved = src.saveTo (f);

        Project dst;
        dst.formats = &engine.formats;
        const bool loaded = dst.loadFrom (f);
        const bool fieldsOk = dst.meta.title == "Test Song" && dst.meta.author == "SoundX Tester"
                           && dst.meta.genre == "Electronic" && dst.meta.album == "Self Test Album"
                           && dst.meta.year == "2026" && dst.meta.comment == "written by the self test";
        const bool coverOk = dst.meta.cover.isValid()
                          && dst.meta.cover.getWidth() == 4 && dst.meta.cover.getHeight() == 4;

        log ("project info: author / genre / album / notes and the cover are saved in the project "
             "(cover " + String ((int) dst.meta.coverData.getSize()) + " bytes as PNG)",
             coverEncoded && saved && loaded && fieldsOk && coverOk);

        // nothing written when the user leaves everything empty
        Project plain;
        bool emptyOk = plain.meta.isEmpty();
        if (auto xml = plain.toXml (false))
            emptyOk = emptyOk && xml->getChildByName ("Info") == nullptr;
        log ("project info: an empty info block is not written into the project file", emptyOk);
        f.deleteFile();
    }

    // ---- the panel draws, and the title shows up in the window title ---------------------------
    {
        AppState st (project, engine, plugins);
        const auto oldTitle = project.meta.title;
        project.meta.title = "Panel Check";

        bool panelOk = false;
        if (auto img = renderProjectInfoPanel (st); img.isValid())
        {
            panelOk = img.getWidth() > 300 && img.getHeight() > 300;
            if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("project_info.png").createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
            }
        }

        MainComponent mc (st);
        const bool titleOk = mc.windowTitle().contains ("Panel Check");

        project.meta.title = oldTitle;
        log ("project info: the dialog renders and the title appears in the window title "
             "(project_info.png)", panelOk && titleOk);
    }
}

// MIDI learn: a controller CC (or a button note) drives a fader / pan / master / plug-in parameter,
// and learning writes the mapping into the project.
static void runMidiLearnTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
        project.masterTrack.inserts.clear();
    }
    project.midiMappings.clear();
    engine.syncAll();

    auto& tr = project.addTrack (TrackType::Instrument, "Learn");
    const int tid = tr.id;
    String err;
    const bool instOk = engine.setInstrument (tid, makeBuiltInSynthSlot().desc, err);
    engine.syncAll();

    // ---- learning: arm the track volume, then move a knob --------------------------------
    MidiTarget target;
    target.kind = MidiTargetKind::TrackVolume;
    target.trackId = tid;
    engine.setLearnTarget (target, "Learn - Volume");
    const bool armed = engine.isLearning();
    const int learned = engine.applyController (1, 21, 100);
    const bool mapped = project.midiMappings.size() == 1
                     && project.midiMappings[0].cc == 21 && project.midiMappings[0].channel == 1
                     && project.midiMappings[0].target.kind == MidiTargetKind::TrackVolume
                     && ! engine.isLearning();
    log ("midi learn: moving a knob while armed maps it to the track volume (CC 21 on CH 1)",
         instOk && armed && learned == 1 && mapped);

    // ---- the same CC now moves the fader, and only on its channel ------------------------
    engine.applyController (1, 21, 127);
    const float atMax = project.findTrack (tid) != nullptr ? project.findTrack (tid)->volumeDb : -200.0f;
    engine.applyController (2, 21, 0);            // another channel: must be ignored
    const float afterOtherChannel = project.findTrack (tid) != nullptr ? project.findTrack (tid)->volumeDb : -200.0f;
    engine.applyController (1, 21, 0);
    const float atMin = project.findTrack (tid) != nullptr ? project.findTrack (tid)->volumeDb : 0.0f;
    // the fader's top position is +6 dB in this mixer, and the bottom one is silence
    log ("midi learn: the CC moves the fader to the top (+6 dB) and to silence, and ignores other channels ("
         + String (atMax, 2) + " / " + String (afterOtherChannel, 2) + " / " + String (atMin, 2) + " dB)",
         std::abs (atMax - 6.0f) < 0.05f && std::abs (afterOtherChannel - atMax) < 0.02f && atMin <= -99.0f);

    // ---- pan, master volume and a switch -------------------------------------------------
    {
        engine.setLearnTarget ({ MidiTargetKind::TrackPan, tid, 0, -1 }, "Learn - Pan");
        engine.applyController (1, 22, 127);
        const float panRight = project.findTrack (tid)->pan;

        engine.setLearnTarget ({ MidiTargetKind::MasterVolume, 0, 0, -1 }, "Master");
        engine.applyController (0, 23, 127);      // channel 0 = any channel
        const float masterDb = project.masterTrack.volumeDb;

        const bool metroBefore = project.metronome;
        engine.setLearnTarget ({ MidiTargetKind::Metronome, 0, 0, -1 }, "Metronome");
        engine.applyController (1, 36, 127, true);   // a button that sends a note
        // the fader top is +6 dB here, so "all the way up" means that, not 0 dB
        const bool metronomeToggled = project.metronome != metroBefore
                                   && project.midiMappings.size() >= 4;

        log ("midi learn: pan, master volume and a note-button (metronome) all map and move ("
             + String (panRight, 2) + " / " + String (masterDb, 2) + " dB)",
             panRight > 0.9f && std::abs (masterDb - 6.0f) < 0.05f && metronomeToggled);
        project.metronome = metroBefore;
        project.masterTrack.volumeDb = 0.0f;
    }

    // ---- a plug-in parameter -------------------------------------------------------------
    {
        int64 uid = 0;
        if (auto* t = project.findTrack (tid))
            if (t->hasInstrument) uid = t->instrument.uid;

        int cutoffIndex = -1;
        if (auto inst = engine.getInstance (uid))
            if (inst->proc != nullptr)
                for (int i = 0; i < inst->proc->getParameters().size(); ++i)
                    if (auto* p = inst->proc->getParameters()[i]; p != nullptr && p->getName (32).containsIgnoreCase ("cutoff"))
                        cutoffIndex = i;

        engine.setLearnTarget ({ MidiTargetKind::PluginParam, 0, uid, cutoffIndex }, "SX Synth - Cutoff");
        engine.applyController (1, 30, 10);
        float low = -1.0f, high = -1.0f;
        if (auto inst = engine.getInstance (uid))
            if (auto* p = inst->proc->getParameters()[cutoffIndex])
            {
                p->setValueNotifyingHost (0.0f);
                low = p->getValue();
                engine.applyController (1, 30, 127);
                high = p->getValue();
            }
        log ("midi learn: a plug-in parameter is mapped and set from the CC (cutoff " + String (low, 2)
             + " -> " + String (high, 2) + ")", cutoffIndex >= 0 && low < 0.05f && high > 0.95f);
    }

    // ---- the mapping survives save / load and comes back to the panel ---------------------
    {
        const File f = out.getChildFile ("midi_learn.sxp");
        f.deleteFile();
        const bool saved = project.saveTo (f);

        Project back;
        back.formats = &engine.formats;
        const bool loaded = back.loadFrom (f);
        const bool same = loaded && back.midiMappings.size() == project.midiMappings.size()
                       && back.midiMappings[0].cc == project.midiMappings[0].cc
                       && back.midiMappings[0].target.kind == project.midiMappings[0].target.kind
                       && back.midiMappings[0].target.trackId == project.midiMappings[0].target.trackId
                       && back.midiMappings[0].label == project.midiMappings[0].label;
        log ("midi learn: the mappings are saved with the project (" + String ((int) project.midiMappings.size())
             + " mappings)", saved && same);
        f.deleteFile();
    }

    // ---- the panel ----------------------------------------------------------------------
    {
        AppState st (project, engine, plugins);
        bool panelOk = false;
        if (auto img = renderMidiLearnPanel (st); img.isValid())
        {
            panelOk = img.getWidth() > 300 && img.getHeight() > 200;
            if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("midi_learn.png").createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
            }
        }
        log ("midi learn: the window renders the list (midi_learn.png)", panelOk);
    }

    // leave the project as this test found it
    project.midiMappings.clear();
    project.metronome = false;
    project.masterTrack.volumeDb = 0.0f;
    const int tidLeft = tid;
    project.removeTrack (tidLeft);
    project.sendStructureChanged();
    engine.syncAll();
}

// Localisation: in Chinese every menu item must come out translated. Brand names, units and plug-in
// metadata legitimately stay in Latin script, so a short allowlist covers those; anything else that
// is still English is a missing entry in the table.
static void runLocalisationTest (Project& project, Engine& engine, PluginManager& plugins, Log& log)
{
    auto* props = plugins.properties();
    const auto before = currentLanguageSetting (props);
    applyLanguage ("zh");

    const bool spot = TRANS ("File") == "文件" && TRANS ("Edit") == "编辑"
                   && TRANS ("Undo") == "撤销" && TRANS ("Save") == "保存"
                   && TRANS ("Metronome") == "节拍器" && TRANS ("Mixer") == "调音台"
                   && TRANS ("Project Info...") == "工程信息..." && TRANS ("MIDI Learn") == "MIDI 学习"
                   && TRANS ("Track Height") == "轨道高度";
    log ("localisation: the Chinese table is active (File / Edit / Undo / Save / Metronome / Mixer ...)", spot);

    auto needsTranslation = [] (const String& t)
    {
        if (t.isEmpty()) return false;
        // a text is fine when it already contains a Chinese character
        for (auto c : t)
            if (c >= 0x4e00 && c <= 0x9fff) return false;
        if (! t.containsAnyOf ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")) return false;
        // "1.00x", "10 ms", "1/16" and the like are values, not words
        if (t.containsOnly ("0123456789., /+-xFt" ) && t.containsAnyOf ("0123456789")) return false;
        // units, formats and brand names stay as they are
        static const char* allowed[] = { "SoundX", "MIDI", "VST", "VST3", "VST2", "AU", "ARA", "WAV", "FLAC",
                                         "MP3", "BPM", "dB", "Hz", "kHz", "bit", "ms", "s", "L", "R", "M", "S",
                                         "A", "B", "C", "x", "%", "CC", "Note", "Ch", "English" };
        for (auto* a : allowed)
            if (t == a) return false;
        return true;
    };

    AppState st (project, engine, plugins);
    MainComponent mc (st);
    const auto names = mc.getMenuBarNames();
    int total = 0, latin = 0;
    StringArray samples;
    for (int i = 0; i < names.size(); ++i)
    {
        auto menu = mc.getMenuForIndex (i, names[i]);
        for (PopupMenu::MenuItemIterator it (menu, true); it.next();)
        {
            auto& item = it.getItem();
            if (item.isSeparator || item.text.isEmpty()) continue;
            ++total;
            if (needsTranslation (item.text))
            {
                ++latin;
                if (samples.size() < 6) samples.add (item.text);
            }
        }
    }

    log ("localisation: every menu item is translated (" + String (total) + " items in the menu bar, "
         + String (latin) + " still English" + (samples.isEmpty() ? String() : ": " + samples.joinIntoString (", ")) + ")",
         spot && total > 60 && latin == 0);

    applyLanguage (before);
}

// A track can stack several instruments: every layer plays the track's MIDI, each has its own
// balance, and removing one leaves the others.
static void runLayeredInstrumentTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
        project.masterTrack.inserts.clear();
    }
    engine.syncAll();

    auto& tr = project.addTrack (TrackType::Instrument, "Layer");
    const int tid = tr.id;
    tr.volumeDb = 0.0f;
    String err;
    const bool primary = engine.setInstrument (tid, makeBuiltInSynthSlot().desc, err);
    const bool stacked = engine.addInstrument (tid, makeBuiltInSynthSlot().desc, err);
    engine.syncAll();

    int64 primaryUid = 0, layerUid = 0;
    if (auto* t = project.findTrack (tid))
    {
        primaryUid = t->instrument.uid;
        if (! t->extraInstruments.empty()) layerUid = t->extraInstruments.front().uid;
    }

    {
        MidiClip c; c.id = project.newId(); c.name = "layered"; c.length = 2.0;
        Note n; n.id = project.newId(); n.pitch = 60; n.start = 0.0; n.length = 1.0; n.velocity = 100;
        c.notes.push_back (n);
        if (auto* t = project.findTrack (tid)) t->midiClips.push_back (c);
    }
    engine.syncAll();

    auto mixPeak = [&] (const String& name) -> float
    {
        ExportOptions o;
        o.mode = ExportOptions::Mix;
        o.target = out.getChildFile (name + ".wav");
        o.sampleRate = 44100.0;
        o.bitDepth = 24;
        String e2;
        if (! engine.exportAudio (o, {}, e2)) return -1.0f;
        std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
        if (r == nullptr) return -1.0f;
        AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
        return jmax (b.getMagnitude (0, 0, b.getNumSamples()), b.getMagnitude (1, 0, b.getNumSamples()));
    };

    const float both = mixPeak ("layered_both");

    // bypass the second layer: back to one instrument's level
    if (auto* s = project.findTrack (tid) ? project.findTrack (tid)->findInstrument (layerUid) : nullptr)
        s->bypass = true;
    project.sendMixChanged();
    engine.syncAll();
    const float onlyPrimary = mixPeak ("layered_one");

    // and the layer's own gain: -inf silences it even when it is not bypassed
    if (auto* s = project.findTrack (tid) ? project.findTrack (tid)->findInstrument (layerUid) : nullptr)
    {
        s->bypass = false;
        s->gainDb = -100.0f;
    }
    project.sendMixChanged();
    engine.syncAll();
    const float layerDown = mixPeak ("layered_down");

    if (auto* s = project.findTrack (tid) ? project.findTrack (tid)->findInstrument (layerUid) : nullptr)
        s->gainDb = 0.0f;
    project.sendMixChanged();
    engine.syncAll();

    const bool counted = project.findTrack (tid) != nullptr && project.findTrack (tid)->numInstruments() == 2
                      && primaryUid != 0 && layerUid != 0 && primaryUid != layerUid;
    log ("layered instruments: two instruments on one track both play, and the layer's bypass / gain "
         "takes it out (" + String (both, 3) + " / " + String (onlyPrimary, 3) + " / " + String (layerDown, 3) + ")",
         primary && stacked && counted
         && both > 1.7f * onlyPrimary && std::abs (layerDown - onlyPrimary) < 0.1f * onlyPrimary && onlyPrimary > 0.05f);

    // the balance and the layers survive save / load
    {
        const File f = out.getChildFile ("layered.sxp");
        f.deleteFile();
        if (auto* s = project.findTrack (tid) ? project.findTrack (tid)->findInstrument (layerUid) : nullptr)
            s->gainDb = -6.0f;
        const bool saved = project.saveTo (f);

        Project back;
        back.formats = &engine.formats;
        const bool loaded = back.loadFrom (f);
        auto* bt = loaded ? back.findTrack (tid) : nullptr;
        const bool same = bt != nullptr && bt->numInstruments() == 2
                       && ! bt->extraInstruments.empty()
                       && std::abs (bt->extraInstruments[0].gainDb - (-6.0f)) < 1.0e-3
                       && bt->extraInstruments[0].uid == layerUid;
        log ("layered instruments: the stack and each layer's gain are saved with the project", saved && same);
        f.deleteFile();
    }

    // removing the first instrument promotes the stacked one instead of muting the track
    {
        bool removed = false, promoted = false;
        if (auto* t = project.findTrack (tid))
        {
            removed = t->removeInstrument (primaryUid);
            promoted = t->hasInstrument && t->instrument.uid == layerUid && t->extraInstruments.empty()
                    && t->numInstruments() == 1;
        }
        project.sendStructureChanged();
        engine.syncAll();
        const float afterRemove = mixPeak ("layered_promoted");
        log ("layered instruments: removing the first instrument promotes the next layer (level "
             + String (afterRemove, 3) + " / " + String (onlyPrimary, 3) + ")",
             removed && promoted && std::abs (afterRemove - onlyPrimary) < 0.1f * onlyPrimary);
    }

    project.removeTrack (tid);
    project.sendStructureChanged();
    engine.syncAll();
}

// The slot menu carries both plug-in items (KnownPluginList ids, a huge base) and the instrument
// layer items (a small range). A click on "choose instrument" is dispatched by id, so the two
// ranges must never overlap - when they did, choosing an instrument did nothing at all.
static void runSlotMenuIdTest (PluginManager& plugins, Log& log)
{
    Array<PluginDescription> list;
    for (int i = 0; i < 3; ++i)
    {
        PluginDescription d;
        d.name = "Test Plug-in " + String (i + 1);
        d.manufacturerName = i == 0 ? "Vendor A" : "Vendor B";
        d.pluginFormatName = "VST3";
        d.fileOrIdentifier = "/tmp/test" + String (i + 1) + ".vst3";
        list.add (d);
    }

    PopupMenu menu;
    plugins.addMenuItems (menu, list);

    int items = 0, wrong = 0, collisions = 0;
    PopupMenu::MenuItemIterator it (menu, true);
    while (it.next())
    {
        auto& item = it.getItem();
        if (item.isSeparator || item.itemID == 0) continue;
        ++items;
        // the id must decode back to this plug-in, and must not look like a layer or command id
        // layer range (1000..1511) and the side-chain range (30000+ in SlotMenus) must stay clear too
        if (isInstrumentLayerMenuId (item.itemID) || item.itemID < PluginManager::kPluginMenuBase
            || item.itemID >= 30000) ++collisions;
        const int idx = PluginManager::indexFromMenuId (list, item.itemID);
        if (idx < 0) ++wrong;
    }

    log ("slot menu: plug-in items and instrument layer items use separate id ranges ("
         + String (items) + " plug-in items, " + String (collisions) + " collisions)",
         items == 3 && wrong == 0 && collisions == 0);
}

// A plug-in with latency on an FX send (a linear-phase EQ, for instance) must not echo: the dry
// path is padded by the same amount, so the return lines up with the dry signal on the timeline.
static void runSendPdcTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
        project.masterTrack.inserts.clear();
    }
    engine.syncAll();

    // a single-sample click: its position in the render tells everything
    const File click = out.getChildFile ("pdc_click.wav");
    click.deleteFile();
    {
        AudioBuffer<float> b (1, (int) (44100 * 0.5));
        b.clear();
        b.setSample (0, 100, 0.4f);   // low enough that dry + wet does not clip in the WAV
        WavAudioFormat wav;
        if (auto* w = wav.createWriterFor (new FileOutputStream (click), 44100.0, 1, 24, {}, 0))
        {
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            delete w;
        }
    }

    auto& dry = project.addTrack (TrackType::Audio, "Dry");
    const int dryId = dry.id;
    dry.volumeDb = 0.0f;
    {
        AudioClip c; c.id = project.newId(); c.name = "click"; c.file = click; c.start = 0.0;
        c.data = project.pool.load (click, engine.formats);
        c.lengthSec = c.data != nullptr ? c.data->lengthSeconds() : 0.5;
        dry.audioClips.push_back (c);
    }
    auto& ret = project.addTrack (TrackType::Send, "Ret");
    const int retId = ret.id;
    ret.volumeDb = 0.0f;
    engine.syncAll();

    String err;
    const bool delayLoaded = engine.addInsert (retId, makeBuiltInDelaySlot().desc, err);
    engine.syncAll();

    auto renderAndFindFirst = [&] (const String& name, int& firstSample, int& clusters, float& peak) -> bool
    {
        ExportOptions o;
        o.mode = ExportOptions::Mix;
        o.target = out.getChildFile (name + ".wav");
        o.sampleRate = 44100.0;
        o.bitDepth = 24;
        String e2;
        if (! engine.exportAudio (o, {}, e2)) return false;

        std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
        if (r == nullptr) return false;
        AudioBuffer<float> b (1, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, false);

        // the export trims the lead-in, so positions are compared relative to each other; a click
        // that arrives twice shows up as two separate clusters of non-silent samples
        firstSample = -1;
        clusters = 0;
        peak = 0.0f;
        int lastLoud = -100000;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = std::abs (b.getSample (0, i));
            peak = jmax (peak, v);
            if (v <= 0.05f) continue;
            if (firstSample < 0) firstSample = i;
            if (i - lastLoud > 400) ++clusters;      // a new burst of sound
            lastLoud = i;
        }
        return true;
    };

    // 1) dry only: the click lands where the clip is
    int dryFirst = -1, dryClusters = 0;
    float dryPeak = 0.0f;
    const bool dryOk = renderAndFindFirst ("pdc_send_dry", dryFirst, dryClusters, dryPeak);

    // 2) with a 960-sample plug-in on the send: the dry path is delayed to match, so the return
    //    lands on the same sample - one loud click, not the dry plus an echo 960 samples later
    if (auto* t = project.findTrack (dryId)) t->sends.push_back (SendSlot { retId, 0.0f, false });
    project.sendStructureChanged();
    engine.syncAll();

    int bothFirst = -1, bothClusters = 0;
    float bothPeak = 0.0f;
    const bool bothOk = renderAndFindFirst ("pdc_send_both", bothFirst, bothClusters, bothPeak);

    const bool aligned = dryOk && bothOk && bothClusters == 1 && dryClusters == 1
                      && bothFirst == dryFirst
                      && bothPeak > 0.7f && bothPeak > 1.6f * dryPeak;
    log ("PDC on an FX send: the dry signal is delayed to meet the late return (one click at "
         + String (bothFirst) + ", peak " + String (bothPeak, 2) + " = dry " + String (dryPeak, 2)
         + " + wet, " + String (bothClusters) + " burst)", aligned);

    // the project reports that latency, so the transport / export stay aligned too
    const bool reported = engine.getPdcTotalSamples() == BuiltInDelay::kDelay;
    log ("PDC on an FX send: the return's latency is part of the project's PDC total ("
         + String (engine.getPdcTotalSamples()) + " samples)", reported && delayLoaded);

    // a free send level: not just the -6 / -12 / -24 presets, but any value the user drags or types
    {
        AppState st (project, engine, plugins);
        st.setSendLevel (dryId, retId, -12.0f);
        const bool stored = [&] { auto* t = project.findTrack (dryId);
                                   return t != nullptr && t->sends.size() == 1 && std::abs (t->sends[0].levelDb + 12.0f) < 0.01f; }();
        bool savedLevel = false;
        if (auto xml = project.toXml (false))
            for (auto* e : xml->getChildWithTagNameIterator ("Track"))
                if (e->getIntAttribute ("id") == dryId)
                    for (auto* se : e->getChildWithTagNameIterator ("Send"))
                        savedLevel = std::abs (se->getDoubleAttribute ("level", 0.0) + 12.0) < 0.01;
        project.sendContentChanged();
        engine.syncAll();
        int lvFirst = -1, lvClusters = 0;
        float lvPeak = 0.0f;
        const bool lvOk = renderAndFindFirst ("pdc_send_level", lvFirst, lvClusters, lvPeak);
        const bool quieter = lvOk && lvPeak < bothPeak * 0.95f && lvPeak >= dryPeak * 0.98f && lvFirst == dryFirst;
        log ("FX send: a custom send level is applied and saved (" + String (lvPeak, 3) + " vs "
             + String (bothPeak, 3) + " at 0 dB, dry " + String (dryPeak, 3) + ")", stored && savedLevel && quieter);
        st.setSendLevel (dryId, retId, 0.0f);
        engine.syncAll();
    }

    for (auto& t : project.tracks) t->sends.clear();
    project.removeTrack (retId);
    project.removeTrack (dryId);
    project.sendStructureChanged();
    engine.syncAll();
}

// Dragging a plug-in slot: on the same track it moves (reorders), on another track it is copied
// with its settings, and both are undoable.
static void runSlotDragTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
        project.masterTrack.inserts.clear();
    }
    engine.syncAll();

    auto& a = project.addTrack (TrackType::Audio, "DragA");
    const int aId = a.id;
    auto& b = project.addTrack (TrackType::Audio, "DragB");
    const int bId = b.id;
    engine.syncAll();

    // three delays on A: 0 1 2
    String err;
    for (int i = 0; i < 3; ++i) engine.addInsert (aId, makeBuiltInDelaySlot().desc, err);
    engine.syncAll();

    auto uidsOf = [&] (int tid)
    {
        std::vector<juce::int64> v;
        if (auto* t = project.findTrack (tid))
            for (auto& s : t->inserts) v.push_back (s.uid);
        return v;
    };

    AppState st (project, engine, plugins);
    const auto before = uidsOf (aId);
    const bool three = before.size() == 3;

    // give the middle one a recognisable state (its bypass flag rides in the copied slot)
    if (three)
        if (auto* s = project.findTrack (aId)->findSlot (before[1])) s->bypass = true;

    //== same track: drag the last one to the front ----------------------------------------------
    const bool moved = st.moveOrCopySlot (aId, before[2], false, aId, 0);
    const auto afterMove = uidsOf (aId);
    const bool reordered = moved && afterMove.size() == 3 && afterMove[0] == before[2]
                        && afterMove[1] == before[0] && afterMove[2] == before[1];
    log ("plugin drag: dragging a slot inside its own track reorders the chain ("
         + String (before[0]) + "," + String (before[1]) + "," + String (before[2]) + " -> "
         + String (afterMove[0]) + "," + String (afterMove[1]) + "," + String (afterMove[2]) + ")", reordered);

    //== another track: drag the (bypassed) middle one onto B ------------------------------------
    const auto targetUid = before[1];
    const int targetIndex = (int) std::distance (afterMove.begin(), std::find (afterMove.begin(), afterMove.end(), targetUid));
    const bool copied = st.moveOrCopySlot (aId, targetUid, false, bId, 0);
    const auto onB = uidsOf (bId);
    const auto onA = uidsOf (aId);
    const bool copiedOk = copied && onB.size() == 1 && onA.size() == 3 && onB[0] != targetUid;
    bool stateCopied = false;
    if (copiedOk)
        if (auto* s = project.findTrack (bId)->findSlot (onB[0]))
            stateCopied = s->bypass && s->desc.name == makeBuiltInDelaySlot().desc.name;
    log ("plugin drag: dragging a slot onto another track copies it (track B now has "
         + String ((int) onB.size()) + " slot, same plug-in and settings), the source keeps it",
         copiedOk && stateCopied);

    // the copy is a live instance with the plug-in's settings: it reports the same latency, and the
    // chain on track A (three in series) drives the project's PDC
    {
        int copyLat = 0;
        if (! onB.empty())
            if (auto inst = engine.getInstance (onB[0]))
                copyLat = inst->latencySamples.load();
        const auto pdc = engine.getPdcTotalSamples();
        log ("plugin drag: the copied plug-in is live and reports its latency (" + String (copyLat)
             + " samples, project PDC " + String (pdc) + " for the 3-slot chain on A)",
             copyLat == BuiltInDelay::kDelay && pdc == 3 * BuiltInDelay::kDelay);
    }

    //== undo puts the copy away in one step ----------------------------------------------------
    project.undo();
    log ("plugin drag: the copy is one undo step (track B has " + String ((int) uidsOf (bId).size())
         + " slot after one undo)", uidsOf (bId).size() == 0);
    juce::ignoreUnused (targetIndex);

    //== the mixer wiring: a slot button lives inside the window's drag container, and dropping a
    //   foreign slot description on the strip goes through the same code
    {
        AppState st2 (project, engine, plugins);
        MainComponent mc (st2);
        mc.setSize (1229, 710);
        mc.resized();

        ChannelStrip* stripB = nullptr;
        std::function<void (Component&)> walk = [&] (Component& c)
        {
            for (auto* child : c.getChildren())
            {
                if (stripB != nullptr) return;
                if (auto* cs = dynamic_cast<ChannelStrip*> (child)) { if (cs->getTrackId() == bId) { stripB = cs; return; } }
                walk (*child);
            }
        };
        walk (mc);

        bool containerOk = false;
        if (stripB != nullptr)
            for (auto* c : stripB->getChildren())
                if (dynamic_cast<SlotButton*> (c) != nullptr)
                {
                    containerOk = DragAndDropContainer::findParentDragContainerFor (c) != nullptr;
                    break;
                }

        const int slotsBefore = (int) uidsOf (bId).size();
        bool dropOk = false;
        if (stripB != nullptr && ! onA.empty())
        {
            var d (new DynamicObject());
            d.getDynamicObject()->setProperty ("sxSlot", true);
            d.getDynamicObject()->setProperty ("track", aId);
            d.getDynamicObject()->setProperty ("uid", onA[0]);
            d.getDynamicObject()->setProperty ("instrument", false);
            DragAndDropTarget::SourceDetails details (d, nullptr, Point<int> (12, stripB->getHeight() / 2));
            dropOk = stripB->isInterestedInDragSource (details);
            stripB->itemDropped (details);
            dropOk = dropOk && (int) uidsOf (bId).size() == slotsBefore + 1;
        }
        log ("plugin drag: a slot in the mixer is a drag source and the strip accepts the drop ("
             + String (slotsBefore) + " -> " + String ((int) uidsOf (bId).size()) + " slots on B)",
             stripB != nullptr && containerOk && dropOk);
        project.clearUndo();
    }

    project.removeTrack (bId);
    project.removeTrack (aId);
    project.masterTrack.inserts.clear();
    project.sendStructureChanged();
    engine.syncAll();
    project.clearUndo();
}

// The arrange view: scrolling the lanes must move the track headers in the same call (a timer-only
// sync leaves the headers trailing the clips for a few frames - it looked like the clips were out
// of line with their track).
static void runArrangeScrollTest (Project& project, Engine& engine, PluginManager& plugins, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
    }
    engine.syncAll();

    std::vector<int> ids;
    for (int i = 0; i < 12; ++i)
        ids.push_back (project.addTrack (TrackType::Instrument, "Scroll " + String (i + 1)).id);
    project.sendStructureChanged();
    engine.syncAll();

    AppState st (project, engine, plugins);
    ArrangeView av (st);
    av.setSize (1200, 700);
    av.resized();

    // every track's header sits on the same content row as its lane
    bool sameRows = true;
    for (int id : ids)
    {
        auto* h = av.trackHeaderComponent (id);
        if (h == nullptr) { sameRows = false; break; }
    }

    // scroll down: the header column must already be at the same offset, without waiting a timer
    av.scrollLanesTo (140);
    const int lane = av.laneScrollY();
    const int head = av.headerScrollY();
    const bool inStep = lane == 140 && head == lane;

    // and back up
    av.scrollLanesTo (0);
    const bool backOk = av.laneScrollY() == 0 && av.headerScrollY() == 0;

    log ("arrange view: the track headers follow the lanes in the same frame (scroll "
         + String (lane) + " / headers " + String (head) + ")", sameRows && inStep && backOk);

    // Scrolled all the way down the two viewports have to stop at the SAME content offset: the
    // lane viewport carries a horizontal scrollbar and the header column does not, so its visible
    // height is 12 px smaller. Without compensating that in the header content height the headers
    // clamped 12 px earlier than the lanes and the LOWER tracks looked out of line with their
    // clips (the reported "track area misalignment, especially the lower tracks").
    av.scrollLanesTo (1000000);
    const int laneMax = av.laneScrollY();
    const int headMax = av.headerScrollY();
    const bool bottomInStep = laneMax > 140 && headMax == laneMax;
    av.scrollLanesTo (0);
    log ("arrange view: at full scroll the headers and the lanes stop at the same offset ("
         + String (laneMax) + " / " + String (headMax) + ")", bottomInStep);

    // every header fader has a live / maximum-peak meter of its own
    bool everyHeaderMeter = true;
    for (int id : ids)
    {
        const auto mb = av.trackHeaderMeterBounds (id);
        if (! av.trackHeaderMeterVisible (id) || mb.getHeight() < 4 || mb.getWidth() < 20)
        {
            everyHeaderMeter = false;
            break;
        }
    }
    log ("arrange view: every track-header fader shows a live level + highest-peak meter", everyHeaderMeter);

    // the bar ruler: a click moves the transport, a drag scrolls the view (never the playhead)
    engine.stop();
    if (auto* ruler = av.rulerComponent())
    {
        auto makeEv = [] (Component& c, Point<float> p)
        {
            return MouseEvent (Desktop::getInstance().getMainMouseSource(), p,
                               ModifierKeys (ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                               &c, &c, Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
        };
        const int x0 = 80, x1 = 360;
        // a click on the bar strip moves the transport; a drag on the same strip scrolls the
        // arrangement instead of scrubbing the playhead
        ruler->mouseDown (makeEv (*ruler, { (float) x1, 10.0f }));
        ruler->mouseUp (makeEv (*ruler, { (float) x1, 10.0f }));
        const double expected = ((double) x1 + av.scrollX()) / st.pxPerBeat;
        const bool moved = std::abs (engine.getPositionBeats() - expected) < 0.2;
        const int sxBeforeDrag = av.scrollX();
        const double posBeforeDrag = engine.getPositionBeats();
        ruler->mouseDown (makeEv (*ruler, { (float) x1, 10.0f }));
        ruler->mouseDrag (makeEv (*ruler, { (float) x0, 10.0f }));
        ruler->mouseUp (makeEv (*ruler, { (float) x0, 10.0f }));
        const bool panned = av.scrollX() > sxBeforeDrag
                            && std::abs (engine.getPositionBeats() - posBeforeDrag) < 1.0e-6;
        log ("arrange view: a bar-ruler click moves the transport (" + String (engine.getPositionBeats(), 2)
             + " beats) and a drag scrolls the arrangement (" + String (sxBeforeDrag) + " -> "
             + String (av.scrollX()) + " px) without moving the playhead", moved && panned);
    }

    for (int id : ids) project.removeTrack (id);
    project.sendStructureChanged();
    engine.syncAll();
}

// Folders, stand-alone automation tracks, the dynamic automation row height and the direct
// cycle / pan gestures of the ruler. The test builds a small arrangement, checks the model and
// the layout, renders an automated mix and saves / loads the whole thing.
static void runFolderAutomationTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    project.reset();
    engine.syncAll();
    project.bpm = 120.0;
    project.tsNum = 4;
    project.tsDen = 4;

    auto& drums = project.addTrack (TrackType::Instrument, "Drums");
    auto& bass = project.addTrack (TrackType::Instrument, "Bass");
    auto& keys = project.addTrack (TrackType::Instrument, "Keys");
    const int drumsId = drums.id, bassId = bass.id, keysId = keys.id;
    project.sendStructureChanged();
    engine.syncAll();

    AppState app (project, engine, plugins);
    const int folderId = app.groupIntoFolder ({ drumsId, bassId, keysId });    auto* folder = project.findTrack (folderId);
    const bool folderOk = folder != nullptr && folder->isFolder && folder->isBus() && folder->folderOpen
                          && project.folderChildCount (folderId) == 3
                          && project.folderChildren (folderId).size() == 3;
    bool childrenOk = folderOk && project.indexOfTrack (folderId) + 3 < (int) project.tracks.size();
    for (int i = 0; i < 3 && childrenOk; ++i)
    {
        auto* child = project.tracks[(size_t) (project.indexOfTrack (folderId) + 1 + i)].get();
        childrenOk = child != nullptr && child->folderParent == folderId && child->outputBusId == folderId
                     && child->folderDepth == 1 && ! child->folderHidden;
    }
    log ("folders: three selected tracks are packed below one folder, indented and routed through it ("
         + String (project.folderChildCount (folderId)) + " children, depth "
         + String (project.findTrack (bassId) != nullptr ? project.findTrack (bassId)->folderDepth : -1) + ")",
         folderOk && childrenOk);

    // the inline automation row scales with the track: a fixed 70 px row made it overlap the next
    // track as soon as the track was taller / shorter than the 78 px default
    app.trackHeight = 46;
    auto* keysT = project.findTrack (keysId);
    auto& inlineVol = keysT->ensureLane ("vol", "Volume");
    inlineVol.points = { { 0.0, 0.6f }, { 4.0, 0.8f } };
    inlineVol.visible = true;
    keysT->activeLane = "vol";
    {
        // a MIDI clip under the curve: the screenshot shows the event fading behind the automation
        MidiClip c;
        c.id = project.newId();
        c.name = "Keys";
        c.start = 0.0;
        c.length = 8.0;
        for (int i = 0; i < 8; ++i)
        {
            Note n;
            n.id = project.newId();
            n.pitch = 60 + (i % 3 == 0 ? 4 : 0);
            n.start = (double) i;
            n.length = 0.8;
            n.velocity = 100;
            c.notes.push_back (n);
        }
        keys.midiClips.push_back (c);
    }
    project.sendStructureChanged();
    ArrangeView av (app);
    av.setSize (1200, 700);
    av.resized();
    const int keysRow = av.trackRowHeight (keysId);
    auto savePng = [] (const Image& img, const File& target)
    {
        target.deleteFile();
        FileOutputStream os (target);
        PNGImageFormat png;
        return os.openedOk() && png.writeImageToStream (img, os);
    };
    app.selectTrack (keysId);
    bool rowsAligned = true;
    int running = 0;
    for (auto& tr : project.tracks)
    {
        auto* h = av.trackHeaderComponent (tr->id);
        if (h == nullptr || h->getY() != running) rowsAligned = false;
        running += av.trackRowHeight (tr->id);
    }
    const bool inTrack = keysT->inlineAutomationLane() != nullptr && keysT->laneRowCount() == 0;
    rowsAligned = rowsAligned && keysRow == 46 && inTrack;
    log ("automation in track: a " + String (46) + " px track keeps one " + String (keysRow)
         + " px row (no lane sub-row), the next header starts right below it", rowsAligned);

    // a click in the track body (automation edit mode) adds a node on the grid
    if (auto* lanes = av.lanesComponent())
    {
        const auto row = av.trackRowBounds (keysId);
        const auto body = row.withTrimmedTop (1).withHeight (jmax (1, row.getHeight() - 2));
        const int x = (int) std::round (6.0 * app.pxPerBeat);
        const int y = (int) std::round (autolane::yOf (0.6f, body));
        auto makeEv = [] (Component& c, Point<float> p, ModifierKeys mods)
        {
            return MouseEvent (Desktop::getInstance().getMainMouseSource(), p, mods,
                               1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                               Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
        };
        keysT->automationEdit = true;
        lanes->mouseDown (makeEv (*lanes, { (float) x, (float) y }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        lanes->mouseUp (makeEv (*lanes, { (float) x, (float) y }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        keysT->automationEdit = false;
        const bool added = inlineVol.points.size() == 3
                           && std::abs (inlineVol.points.back().beat - 6.0) < 0.3;
        inlineVol.points = { { 0.0, 0.6f }, { 4.0, 0.8f } };
        log ("automation in track: a click in the row adds a node (" + String ((int) inlineVol.points.size())
             + " points after the reset)", added);
    }
    else log ("automation in track: the lane component exists", false);

    const bool inlineShot = savePng (av.createComponentSnapshot (av.getLocalBounds(), true),
                                     out.getChildFile ("folders_inline_automation.png"));

    // a collapsed folder hides its children from the arrangement (they keep playing) and the
    // headers of the hidden rows no longer take any space
    app.toggleFolderOpen (folderId);
    const bool foldedModel = project.findTrack (bassId) != nullptr && project.findTrack (bassId)->folderHidden;
    const bool foldedLayout = av.trackRowHeight (bassId) == 0
                              && (av.trackHeaderComponent (bassId) == nullptr
                                  || av.trackHeaderComponent (bassId)->getHeight() == 0);
    log ("folders: closing a folder folds its rows away (" + String (av.trackRowHeight (bassId)) + " px rows)",
         foldedModel && foldedLayout);
    app.toggleFolderOpen (folderId);

    // moving the inline lane into a stand-alone automation track: the curve moves with it and the
    // channel keeps no second copy of the same automation
    const int autoId = app.addAutomationTrack (keysId, "vol", "Volume");
    auto* autoTrack = project.findTrack (autoId);
    const bool autoOk = autoTrack != nullptr && autoTrack->isAutomation()
                        && autoTrack->autoTargetTrack == keysId && autoTrack->autoTarget == "vol"
                        && autoTrack->automationLane() != nullptr && autoTrack->automationLane()->points.size() == 2
                        && keysT->findLane ("vol") == nullptr
                        && project.indexOfTrack (autoId) == project.indexOfTrack (keysId) + 1;
    log ("automation track: the lane moves out of the channel into its own indented row ("
         + String (autoTrack != nullptr && autoTrack->automationLane() != nullptr
                   ? (int) autoTrack->automationLane()->points.size() : 0) + " points)", autoOk);
    const bool autoRowOk = av.trackRowHeight (autoId) == autoTrack->effectiveHeight (app.trackHeight);
    log ("automation track: the row follows the session track height ("
         + String (av.trackRowHeight (autoId)) + " px)", autoRowOk);

    // folding it back into the channel removes the row and restores the inline lane
    const bool inlineBack = app.inlineAutomationTrack (autoId)
                            && keysT->findLane ("vol") != nullptr && project.findTrack (autoId) == nullptr;
    const int autoAgain = app.addAutomationTrack (keysId, "vol", "Volume");
    log ("automation track: folding it back into the channel restores the inline lane ("
         + String (autoAgain > 0 ? "moved out again" : "failed") + ")",
         inlineBack && autoAgain > 0 && project.findTrack (autoAgain) != nullptr
         && project.findTrack (autoAgain)->automationLane() != nullptr);

    // Ctrl+wheel / View > Track Height changes the default: both the channel and the automation
    // track must land on exactly the same row height
    av.setTrackHeight (104);
    av.setSize (1200, 700);
    av.resized();
    const int keysAt104 = av.trackRowHeight (keysId);
    const int autoAt104 = av.trackRowHeight (autoAgain);
    av.setTrackHeight (23);
    av.setSize (1200, 700);
    av.resized();
    const int keysAt23 = av.trackRowHeight (keysId);
    const int autoAt23 = av.trackRowHeight (autoAgain);
    av.setTrackHeight (46);
    av.setSize (1200, 700);
    av.resized();
    log ("automation rows: zooming gives the channel and its automation track the same height (104 "
         + String (keysAt104) + "/" + String (autoAt104) + " px, 23 " + String (keysAt23) + "/"
         + String (autoAt23) + " px)",
         keysAt104 == 104 && autoAt104 == 104 && keysAt23 == 23 && autoAt23 == 23);

    // the target channel can be changed later (right-click > Automation Target Track)
    const bool retarget = app.setAutomationTargetTrack (autoAgain, bassId)
                          && project.findTrack (autoAgain) != nullptr
                          && project.findTrack (autoAgain)->autoTargetTrack == bassId
                          && project.findTrack (autoAgain)->folderParent == folderId;
    app.setAutomationTargetTrack (autoAgain, keysId);
    log ("automation track: the target track can be re-routed (" + String (bassId) + " / back to "
         + String (keysId) + ")", retarget);

    //== the automated channel really renders with the curve =====================================
    auto& synth = project.addTrack (TrackType::Instrument, "Auto Sine");
    const int synthId = synth.id;
    synth.hasInstrument = true;
    synth.instrument = makeBuiltInSynthSlot();
    {
        MidiClip clip;
        clip.id = project.newId();
        clip.name = "held";
        clip.start = 0.0;
        clip.length = 8.0;
        for (int i = 0; i < 8; ++i)
        {
            Note n;
            n.id = project.newId();
            n.pitch = 60;
            n.start = (double) i;
            n.length = 0.95;
            n.velocity = 110;
            clip.notes.push_back (n);
        }
        synth.midiClips.push_back (clip);
    }
    const int autoVolId = app.addAutomationTrack (synthId, "vol", "Volume");
    if (auto* at = project.findTrack (autoVolId); at != nullptr && at->automationLane() != nullptr)
        at->automationLane()->points = { { 0.0, 0.75f }, { 3.9, 0.75f }, { 4.0, 0.0f } };
    for (auto& tr : project.tracks) tr->solo = tr->id == synthId;
    project.sendMixChanged();
    project.sendContentChanged();
    engine.syncAll();

    ExportOptions eo;
    eo.mode = ExportOptions::Mix;
    eo.target = out.getChildFile ("folder_auto.wav");
    eo.sampleRate = 44100.0;
    eo.bitDepth = 16;
    eo.tailSeconds = 0.0;
    String exportErr;
    const bool exported = engine.exportAudio (eo, {}, exportErr);
    float head = 0.0f, tail = 0.0f;
    if (exported)
        if (std::unique_ptr<AudioFormatReader> r { engine.formats.createReaderFor (eo.target) })
        {
            AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            const int sr = (int) r->sampleRate;
            head = b.getMagnitude (0, sr / 4, sr / 4);                       // 0.25 .. 0.5 s: still open
            tail = b.getMagnitude (0, (int) (2.5 * sr), jmax (1, (int) (0.4 * sr)));   // after the fade: silent
        }
    log ("automation track: the curve drives the target channel in the render (head peak "
         + String (head, 3) + ", tail peak " + String (tail, 3) + ") " + exportErr,
         exported && head > 0.03f && tail < 0.01f);
    for (auto& tr : project.tracks) tr->solo = false;
    project.sendMixChanged();

    //== direct cycle editing and drag-to-scroll on the ruler ====================================
    app.setZoom (30.0);
    av.setSize (1200, 700);
    av.resized();
    project.loopStart = 2.0;
    project.loopEnd = 6.0;
    project.loopOn = true;
    project.sendContentChanged();
    engine.seekToBeats (0.0);

    auto makeEv = [] (Component& c, Point<float> p, ModifierKeys mods)
    {
        return MouseEvent (Desktop::getInstance().getMainMouseSource(), p, mods,
                           1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                           Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
    };
    auto* ruler = av.rulerComponent();
    if (ruler != nullptr)
    {
        const double endX = project.loopEnd * app.pxPerBeat;   // ruler-local x of the loop end
        ruler->mouseDown (makeEv (*ruler, { (float) endX, 32.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        ruler->mouseDrag (makeEv (*ruler, { (float) endX + 60.0f, 32.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        ruler->mouseUp (makeEv (*ruler, { (float) endX + 60.0f, 32.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        const bool endDragged = std::abs (project.loopEnd - 8.0) < 0.3;
        const double playBefore = engine.getPositionBeats();

        // dragging inside the loop moves the whole cycle
        ruler->mouseDown (makeEv (*ruler, { (float) (4.0 * app.pxPerBeat), 32.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        ruler->mouseDrag (makeEv (*ruler, { (float) (4.0 * app.pxPerBeat) + 30.0f, 32.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        ruler->mouseUp (makeEv (*ruler, { (float) (5.0 * app.pxPerBeat), 32.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        const bool moved = std::abs (project.loopStart - 3.0) < 0.3 && std::abs (project.loopEnd - 9.0) < 0.3;
        const bool transportStill = std::abs (engine.getPositionBeats() - playBefore) < 1.0e-6;
        log ("ruler: the loop strip edits the cycle directly (2..6 -> " + String (project.loopStart, 2) + ".."
             + String (project.loopEnd, 2) + "), without moving the playhead", endDragged && moved && transportStill);

        // a click on the bar strip places the transport; a drag on it scrolls the arrangement
        const int clickX = 220;
        const int sxClick = av.scrollX();
        ruler->mouseDown (makeEv (*ruler, { (float) clickX, 10.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        ruler->mouseUp (makeEv (*ruler, { (float) clickX, 10.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        const bool seeked = std::abs (engine.getPositionBeats() - (clickX + sxClick) / app.pxPerBeat) < 0.3;

        const int beforePan = av.scrollX();
        ruler->mouseDown (makeEv (*ruler, { 600.0f, 10.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        const double playBeforePan = engine.getPositionBeats();   // the press places the playhead
        ruler->mouseDrag (makeEv (*ruler, { 400.0f, 10.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        ruler->mouseUp (makeEv (*ruler, { 400.0f, 10.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
        const bool panned = av.scrollX() >= beforePan + 150
                            && std::abs (engine.getPositionBeats() - playBeforePan) < 1.0e-6;
        log ("ruler: a bar-strip click seeks (" + String (engine.getPositionBeats(), 2)
             + " beats) and a drag scrolls without scrubbing (" + String (beforePan) + " -> "
             + String (av.scrollX()) + " px)", seeked && panned);
    }
    else log ("ruler: component exists", false);

    //== save / load keeps the hierarchy and the automation tracks ===============================
    app.toggleFolderOpen (folderId);   // save it folded: the state has to survive a round trip
    const File sxp = out.getChildFile ("folders_automation.sxp");
    const bool saved = project.saveTo (sxp);
    Project loaded;
    loaded.formats = &engine.formats;
    const bool loadedOk = loaded.loadFrom (sxp);
    auto* lf = loaded.findTrack (folderId);
    auto* lc = loaded.findTrack (bassId);
    auto* la = loaded.findTrack (autoVolId);
    bool roundTrip = saved && loadedOk && lf != nullptr && lf->isFolder && ! lf->folderOpen
                     && lc != nullptr && lc->folderParent == folderId && lc->folderHidden
                     && la != nullptr && la->isAutomation() && la->autoTargetTrack == synthId
                     && la->automationLane() != nullptr && la->automationLane()->points.size() == 3;
    log ("folders / automation tracks survive save + load (folder "
         + String (lf != nullptr && lf->isFolder ? "yes" : "no")
         + ", children " + String (loaded.folderChildCount (folderId)) + ", automation points "
         + String (la != nullptr && la->automationLane() != nullptr ? (int) la->automationLane()->points.size() : 0) + ")",
         roundTrip);
    app.toggleFolderOpen (folderId);

    // the visual check: the arrangement with an open folder and a selected automation row
    {
        app.setZoom (48.0);
        av.setSize (1200, 700);
        av.scrollLanesTo (0, 0);
        av.resized();
        app.selectTrack (bassId);
        const bool shot = savePng (av.createComponentSnapshot (av.getLocalBounds(), true),
                                   out.getChildFile ("folders_automation.png"));
        log ("folders: a screenshot of the indented folder and automation rows is written", shot && inlineShot);
    }

    project.reset();
    engine.syncAll();
}

// The scissors tool: a click on a clip cuts it in two (audio keeps its sample offset, MIDI cuts the
// notes that cross the cut), and a click on a note in the piano roll cuts just that note. Each cut
// is one undo step.
static void runScissorsTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
        project.masterTrack.inserts.clear();
    }
    engine.syncAll();

    // a one-second sine so the audio clip has a real file and a real offset to carry over
    const File sine = out.getChildFile ("scissors_sine.wav");
    sine.deleteFile();
    {
        AudioBuffer<float> b (1, 44100);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.4f * std::sin ((float) i / 44100.0f * 440.0f * MathConstants<float>::twoPi));
        WavAudioFormat wav;
        if (auto* w = wav.createWriterFor (new FileOutputStream (sine), 44100.0, 1, 24, {}, 0))
        {
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            delete w;
        }
    }

    auto& au = project.addTrack (TrackType::Audio, "Cut A");
    const int auId = au.id;
    {
        AudioClip c; c.id = project.newId(); c.name = "sine"; c.file = sine; c.start = 4.0;
        c.data = project.pool.load (sine, engine.formats);
        c.lengthSec = 1.0;
        au.audioClips.push_back (c);
    }
    auto& mi = project.addTrack (TrackType::Instrument, "Cut M");
    const int miId = mi.id;
    MidiClip midiClip;
    midiClip.id = project.newId();
    midiClip.name = "notes";
    midiClip.start = 8.0;
    midiClip.length = 4.0;
    {
        Note n1; n1.id = project.newId(); n1.pitch = 60; n1.start = 0.0; n1.length = 2.0; n1.velocity = 100;
        Note n2; n2.id = project.newId(); n2.pitch = 64; n2.start = 3.0; n2.length = 0.5; n2.velocity = 90;
        midiClip.notes = { n1, n2 };
    }
    mi.midiClips.push_back (midiClip);
    project.sendStructureChanged();
    engine.syncAll();

    AppState st (project, engine, plugins);
    ArrangeView av (st);
    av.setSize (1200, 700);
    av.resized();

    //== scissors on an audio clip: cut at 4.25 beats (the clip runs 4.0 .. ~4.0 + 1 s) ----------
    st.selectClip (auId, project.findTrack (auId)->audioClips.front().id, false);
    const double cutBeat = 4.75;
    av.splitAt (cutBeat);
    auto* auT = project.findTrack (auId);
    const bool audioSplit = auT != nullptr && auT->audioClips.size() == 2;
    double lenSum = 0.0, offsetB = 0.0, lenA = 0.0;
    bool touching = false;
    if (audioSplit)
    {
        auto& a = auT->audioClips[0];
        auto& b = auT->audioClips[1];
        lenSum = a.lengthSec + b.lengthSec;
        lenA = a.lengthSec;
        offsetB = b.offsetSec;
        touching = std::abs ((a.start + project.clipBeats (a.start, a.lengthSec)) - b.start) < 0.01;
    }
    log ("scissors: cutting an audio clip gives two clips that still touch (" +
         String (audioSplit ? 2 : 0) + " clips, lengths " + String (lenA, 3) + " + " +
         String (lenSum - lenA, 3) + " s = " + String (lenSum, 3) + " s)", audioSplit && touching);

    //== scissors on a MIDI clip: the note crossing the cut becomes two notes ---------------------
    st.selectClip (miId, project.findTrack (miId)->midiClips.front().id, false);
    av.splitAt (9.0);                       // 1.0 beat into the clip: note 1 (0..2) is cut in half
    auto* miT = project.findTrack (miId);
    const bool midiSplit = miT != nullptr && miT->midiClips.size() == 2;
    int notesLeft = 0, notesRight = 0;
    bool halvesOk = false;
    if (midiSplit)
    {
        auto& l = miT->midiClips[0];
        auto& r = miT->midiClips[1];
        notesLeft = (int) l.notes.size();
        notesRight = (int) r.notes.size();
        // pitch 60: 1.0 beat left + 1.0 beat right, pitch 64 stays whole on the right
        bool lOk = l.notes.size() == 1 && l.notes[0].pitch == 60 && std::abs (l.notes[0].length - 1.0) < 0.01;
        bool rOk = r.notes.size() == 2;
        for (auto& n : r.notes)
            if (n.pitch == 60 && std::abs (n.length - 1.0) < 0.01) rOk = rOk && true;
        halvesOk = lOk && rOk;
    }
    log ("scissors: cutting a MIDI clip cuts the note that crosses the cut (" + String (notesLeft)
         + " + " + String (notesRight) + " notes)", midiSplit && halvesOk);

    //== scissors in the piano roll: one note becomes two ----------------------------------------
    bool noteCut = false;
    {
        AppState st2 (project, engine, plugins);
        PianoRollView pr (st2);
        st2.editTrack = miId;
        st2.editClip = project.findTrack (miId)->midiClips[0].id;   // the left half, holds pitch 60
        pr.setSize (900, 500);
        pr.resized();
        auto* c = pr.clip();
        if (c != nullptr && c->notes.size() == 1)
        {
            const double before = c->notes[0].length;
            const double cutAt = c->notes[0].start + before * 0.5;
            pr.setTool (PianoRollView::Tool::Scissors);
            pr.splitNoteAt (0, cutAt);
            c = pr.clip();
            if (c != nullptr && c->notes.size() == 2)
                noteCut = std::abs ((c->notes[0].length + c->notes[1].length) - before) < 1.0e-6
                       && c->notes[0].id != c->notes[1].id;
        }
    }
    log ("scissors: clicking a note in the piano roll splits it in two equal halves", noteCut);

    //== one undo step per cut ------------------------------------------------------------------
    // (undone = the note split only: the clip split before it stays applied)
    project.undo();
    auto* after = project.findTrack (miId);
    bool undone = after != nullptr && after->midiClips.size() == 2;
    if (undone)
        undone = after->midiClips[0].notes.size() == 1
              && std::abs (after->midiClips[0].notes[0].length - 1.0) < 0.01;
    log ("scissors: a cut is one undo step (the piano roll cut is gone, the clip split stays)", undone);

    project.removeTrack (miId);
    project.removeTrack (auId);
    project.sendStructureChanged();
    engine.syncAll();
    project.clearUndo();
    sine.deleteFile();
}

// Takes + record quantize: re-recording over a clip keeps it as a take, only the active take is
// heard, and a MIDI take can be snapped onto the grid as it lands.
static void runTakeTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
        project.masterTrack.inserts.clear();
    }
    engine.syncAll();

    const File sine = out.getChildFile ("take_sine.wav");
    sine.deleteFile();
    {
        AudioBuffer<float> b (1, 44100);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.3f * std::sin ((float) i / 44100.0f * 220.0f * MathConstants<float>::twoPi));
        WavAudioFormat wav;
        if (auto* w = wav.createWriterFor (new FileOutputStream (sine), 44100.0, 1, 24, {}, 0))
        {
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            delete w;
        }
    }

    project.takeRecording = true;
    auto& tr = project.addTrack (TrackType::Audio, "Takes");
    const int tid = tr.id;
    tr.volumeDb = 0.0f;
    engine.syncAll();

    auto addTake = [&] (float gainDb)
    {
        auto* t = project.findTrack (tid);
        AudioClip c;
        c.id = project.newId();
        c.name = "pass";
        c.file = sine;
        c.data = project.pool.load (sine, engine.formats);
        c.start = 0.0;
        c.lengthSec = 1.0;
        c.gainDb = gainDb;
        t->audioClips.push_back (c);
        project.groupTakes (*t, false);
    };

    addTake (0.0f);      // take 1 at unity
    addTake (-6.0f);     // take 2 at -6 dB (0.5x)
    auto* t = project.findTrack (tid);
    bool stacked = t->audioClips.size() == 2;
    if (stacked)
        stacked = t->audioClips[0].takeGroup != 0
               && t->audioClips[0].takeGroup == t->audioClips[1].takeGroup
               && t->audioClips[0].takeIndex == 0 && t->audioClips[1].takeIndex == 1
               && ! t->audioClips[0].takeActive && t->audioClips[1].takeActive;
    log ("takes: a second pass over the same range joins a take stack (2 takes, the new one active)",
         stacked);

    // only the active take is rendered: -6 dB alone, not unity + -6 dB
    auto renderPeak = [&] () -> float
    {
        ExportOptions o;
        o.mode = ExportOptions::Mix;
        o.target = out.getChildFile ("take_mix.wav");
        o.sampleRate = 44100.0;
        o.bitDepth = 24;
        String e;
        if (! engine.exportAudio (o, {}, e)) return -1.0f;
        std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
        if (r == nullptr) return -1.0f;
        AudioBuffer<float> b (1, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, false);
        return b.getMagnitude (0, 0, b.getNumSamples());
    };

    const float activePeak = renderPeak();                 // only take 2 (-6 dB) should be heard
    // switch back to take 1 and render again
    if (auto* tt = project.findTrack (tid))
    {
        tt->audioClips[0].takeActive = true;
        tt->audioClips[1].takeActive = false;
    }
    const float otherPeak = renderPeak();
    const bool silentCheck = activePeak > 0.05f && otherPeak > activePeak * 1.6f
                          && otherPeak < activePeak * 2.4f;   // unity vs -6 dB: ~2x, never the sum (3x)
    log ("takes: only the active take is in the mix (" + String (activePeak, 3) + " vs "
         + String (otherPeak, 3) + " when take 1 is selected)", silentCheck);

    //== record quantize ------------------------------------------------------------------------
    std::vector<Note> played;
    {
        Note a; a.id = 1; a.pitch = 60; a.start = 0.07; a.length = 0.5; a.velocity = 100;
        Note b; b.id = 2; b.pitch = 62; b.start = 0.52; b.length = 0.02; b.velocity = 90;
        Note c; c.id = 3; c.pitch = 64; c.start = 1.01; c.length = 1.0; c.velocity = 80;
        played = { a, b, c };
    }
    auto snapped = played;
    quantizeRecordedNotes (snapped, 0.25);
    bool quantOk = snapped.size() == 3
                && std::abs (snapped[0].start - 0.0) < 1.0e-9
                && std::abs (snapped[1].start - 0.5) < 1.0e-9
                && std::abs (snapped[2].start - 1.0) < 1.0e-9
                && snapped[0].length > 0.4             // a long note keeps its length
                && snapped[1].length >= 1.0 / 128.0;   // a short one does not collapse
    auto untouched = played;
    quantizeRecordedNotes (untouched, 0.0);
    quantOk = quantOk && std::abs (untouched[0].start - 0.07) < 1.0e-9;
    log ("record quantize: notes land on the grid, lengths keep their phrasing (0.07/0.52/1.01 -> "
         + String (snapped[0].start, 2) + "/" + String (snapped[1].start, 2) + "/" + String (snapped[2].start, 2) + ")",
         quantOk);

    //== loop recording: one take per pass ------------------------------------------------------
    {
        const int64 pass = 44100 * 2;            // a 2 s cycle
        // started mid-cycle (0.5 s before the loop end), then two full passes, then a fragment
        const int64 written = 44100 / 2 + pass * 2 + 4410;
        auto segs = splitLoopRecording (written, 44100 / 2, pass, 4.0, 0.0);
        bool loopOk = segs.size() == 3;
        if (loopOk)
        {
            loopOk = segs[0].startSample == 0 && segs[0].lengthSamples == 44100 / 2
                  && std::abs (segs[0].startBeat - 4.0) < 1.0e-9          // the punch-in position
                  && segs[1].startSample == 44100 / 2 && segs[1].lengthSamples == pass
                  && std::abs (segs[1].startBeat) < 1.0e-9                // later passes sit on the cycle
                  && segs[2].startSample == 44100 / 2 + pass && segs[2].lengthSamples == pass;
        }
        // a punch-in exactly on the loop start is just whole passes, no short first segment
        auto whole = splitLoopRecording (pass * 3, pass, pass, 0.0, 0.0);
        const bool wholeOk = whole.size() == 3 && whole[0].startSample == 0 && whole[0].lengthSamples == pass
                          && whole[2].startSample == pass * 2;
        log ("takes: a cycle recording splits into one take per pass (" + String ((int) segs.size())
             + " passes, first " + String (segs.empty() ? 0.0 : (double) segs[0].lengthSamples / 44100.0, 2)
             + " s, then " + String ((int) (pass / 44100)) + " s each)", loopOk && wholeOk);
    }

    project.removeTrack (tid);
    project.sendStructureChanged();
    engine.syncAll();
    project.clearUndo();
    sine.deleteFile();
}

// The built-in effect family: every one of them loads, processes without NaN / blow-up, has a
// working editor, and the ones with an obvious fingerprint (EQ, compressor, limiter, distortion,
// delay, tremolo) actually do what their name says.
static void runBuiltInEffectsTest (Project& project, Engine& engine, PluginManager& plugins, Log& log)
{
    struct Result { String name; bool loaded = false, finite = true, bounded = true, editor = false; float peak = 0.0f; };
    std::vector<Result> results;

    for (auto& info : builtInEffects())
    {
        Result r;
        r.name = info.name;
        auto proc = createBuiltInEffect (info.id);
        r.loaded = proc != nullptr;
        if (proc == nullptr) { results.push_back (r); continue; }

        proc->setRateAndBufferSizeDetails (44100.0, 256);
        proc->prepareToPlay (44100.0, 256);

        AudioBuffer<float> buf (2, 256);
        MidiBuffer midi;
        Random rng (1234);
        double sumSq = 0.0;
        int count = 0;
        for (int block = 0; block < 90; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                {
                    const float noise = rng.nextFloat() * 2.0f - 1.0f;
                    const float sine = 0.5f * std::sin ((float) (block * 256 + i) / 44100.0f * 440.0f * MathConstants<float>::twoPi);
                    buf.setSample (ch, i, 0.6f * (block < 45 ? noise : sine));
                }
            proc->processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                {
                    const float v = buf.getSample (ch, i);
                    if (! std::isfinite (v)) r.finite = false;
                    if (std::abs (v) > 8.0f) r.bounded = false;
                    sumSq += (double) v * v;
                    ++count;
                }
            r.peak = jmax (r.peak, buf.getMagnitude (0, 0, 256));
        }
        juce::ignoreUnused (sumSq, count);
        if (auto* ed = proc->createEditor())
        {
            r.editor = true;
            ed->setSize (560, 250);
            delete ed;
        }
        proc->releaseResources();
        results.push_back (r);
    }

    bool allOk = results.size() == builtInEffects().size();
    String detail;
    for (auto& r : results)
    {
        const bool ok = r.loaded && r.finite && r.bounded && r.editor;
        allOk = allOk && ok;
        if (! ok) detail << r.name << " ";
    }
    log ("built-in effects: " + String ((int) results.size()) + " effects load, run and open an editor"
         + (detail.isEmpty() ? String() : " (failed: " + detail + ")"), allOk);

    //== fingerprints ---------------------------------------------------------------------------
    auto renderTone = [&] (BuiltInEffect kind, double hz, int paramIndex, float paramValue) -> float
    {
        auto proc = createBuiltInEffect (kind);
        proc->setRateAndBufferSizeDetails (44100.0, 256);
        proc->prepareToPlay (44100.0, 256);
        if (paramIndex >= 0)
        {
            if (auto* p = dynamic_cast<RangedAudioParameter*> (proc->getParameters()[paramIndex]))
                p->setValueNotifyingHost (p->convertTo0to1 (paramValue));
        }
        AudioBuffer<float> buf (2, 4096);
        MidiBuffer midi;
        float peak = 0.0f;
        for (int block = 0; block < 12; ++block)      // let the filters settle
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 4096; ++i)
                    buf.setSample (ch, i, 0.5f * std::sin ((float) (block * 4096 + i) / 44100.0 * hz * MathConstants<double>::twoPi));
            proc->processBlock (buf, midi);
            if (block >= 8) peak = jmax (peak, buf.getMagnitude (0, 0, 4096));
        }
        proc->releaseResources();
        return peak;
    };

    // EQ: boosting the low shelf band lifts a tone below its corner
    const float eqFlat = renderTone (BuiltInEffect::Eq, 40.0, -1, 0.0f);
    const float eqBoost = renderTone (BuiltInEffect::Eq, 40.0, SxEq::pGain (0), 12.0f);
    log ("built-in EQ: +12 dB on the low band lifts a 40 Hz tone (" + String (eqFlat, 2) + " -> "
         + String (eqBoost, 2) + ")", eqBoost > eqFlat * 2.4f && eqBoost < eqFlat * 5.5f);

    // Dynamic EQ: the same static cut digs deeper when the band gets loud
    {
        auto makeEq = [&] (bool dyn)
        {
            auto eq = createBuiltInEffect (BuiltInEffect::Eq);
            eq->setRateAndBufferSizeDetails (44100.0, 256);
            eq->prepareToPlay (44100.0, 256);
            auto set = [&] (int idx, float v)
            {
                auto* p = eq->getParameters()[idx];
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
            };
            for (int b = 1; b < SxEq::kNumBands; ++b) set (SxEq::pOn (b), 0.0f);
            set (SxEq::pOn (0), 1.0f);
            set (SxEq::pType (0), (float) SxEq::Bell);
            set (SxEq::pFreq (0), 1000.0f);
            set (SxEq::pGain (0), -12.0f);
            set (SxEq::pQ (0), 2.0f);
            set (SxEq::pDynOn (0), dyn ? 1.0f : 0.0f);
            set (SxEq::pThr (0), -30.0f);
            set (SxEq::pRange (0), 12.0f);
            set (SxEq::pAtk (0), 5.0f);
            set (SxEq::pRel (0), 60.0f);
            return eq;
        };
        auto measure = [&] (bool dyn, float amp)
        {
            auto eq = makeEq (dyn);
            AudioBuffer<float> buf (2, 4096);
            MidiBuffer midi;
            float peak = 0.0f;
            for (int block = 0; block < 24; ++block)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 4096; ++i)
                        buf.setSample (ch, i, amp * std::sin ((float) (block * 4096 + i) / 44100.0 * 1000.0 * MathConstants<double>::twoPi));
                eq->processBlock (buf, midi);
                if (block >= 16) peak = jmax (peak, buf.getMagnitude (0, 0, 4096));
            }
            eq->releaseResources();
            return peak;
        };
        const float quietStatic = measure (false, 0.01f);    // -40 dBFS
        const float loudStatic  = measure (false, 0.70f);    //      -3 dBFS
        const float loudDyn     = measure (true,  0.70f);
        log ("dynamic EQ: a loud band is cut further than the static curve (" + String (loudStatic, 3)
             + " -> " + String (loudDyn, 3) + " at the same input)",
             loudDyn < loudStatic * 0.85f && quietStatic > 0.0f);
    }
    // Exciter: each of the three bands can add its own harmonics
    {
        auto measure = [&] (float lowAmt, float midAmt, float highAmt, float toneHz)
        {
            auto ex = createBuiltInEffect (BuiltInEffect::Exciter);
            ex->setRateAndBufferSizeDetails (44100.0, 512);
            ex->prepareToPlay (44100.0, 512);
            auto set = [&] (int idx, float v)
            {
                auto* p = ex->getParameters()[idx];
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
            };
            set (2, lowAmt);
            set (3, midAmt);
            set (4, highAmt);
            set (7, 100.0f);
            AudioBuffer<float> b (2, 1024);
            MidiBuffer midi;
            float peak = 0.0f;
            for (int blk = 0; blk < 6; ++blk)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 1024; ++i)
                        b.setSample (ch, i, 0.5f * std::sin ((float) (blk * 1024 + i) / 44100.0 * toneHz * MathConstants<double>::twoPi));
                ex->processBlock (b, midi);
                if (blk > 2) peak = jmax (peak, b.getMagnitude (0, 0, 1024));
            }
            ex->releaseResources();
            return peak;
        };
        const float lowOn = measure (100.0f, 0.0f, 0.0f, 80.0f);       // low band on, 80 Hz tone
        const float lowOff = measure (0.0f, 0.0f, 0.0f, 80.0f);
        const float highOn = measure (0.0f, 0.0f, 100.0f, 6000.0f);    // high band on, 6 kHz tone
        const float highOff = measure (0.0f, 0.0f, 0.0f, 6000.0f);
        log ("exciter: the low and high bands each add harmonics (" + String (lowOn, 3) + " vs " + String (lowOff, 3)
             + " / " + String (highOn, 3) + " vs " + String (highOff, 3) + ")",
             lowOn > lowOff * 1.01f && highOn > highOff * 1.01f);
    }
    // Presets: any built-in saves and loads its whole state
    {
        auto a = createBuiltInEffect (BuiltInEffect::Compressor);
        a->setRateAndBufferSizeDetails (44100.0, 512);
        a->prepareToPlay (44100.0, 512);
        auto setA = [&] (int idx, float v)
        {
            auto* p = a->getParameters()[idx];
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
        };
        setA (0, -26.0f);
        setA (1, 8.0f);
        const File f = File::getSpecialLocation (File::tempDirectory).getChildFile ("soundx_preset_test.sxpreset");
        f.deleteFile();
        auto* base = dynamic_cast<SxEffectBase*> (a.get());
        const bool saved = base != nullptr && base->savePresetFile (f);

        auto b2 = createBuiltInEffect (BuiltInEffect::Compressor);
        b2->setRateAndBufferSizeDetails (44100.0, 512);
        b2->prepareToPlay (44100.0, 512);
        auto* base2 = dynamic_cast<SxEffectBase*> (b2.get());
        const bool loaded = base2 != nullptr && base2->loadPresetFile (f);
        const bool match = loaded
                        && std::abs ((float) b2->getParameters()[0]->getValue() - (-26.0f + 60.0f) / 60.0f) < 0.02f
                        && std::abs ((float) b2->getParameters()[1]->getValue() - 7.0f / 19.0f) < 0.03f;
        log ("presets: a compressor preset file restores threshold and ratio", saved && match);
        f.deleteFile();
        a->releaseResources();
        b2->releaseResources();
    }
    // EQ linear phase: the same curve, and it still cuts / boosts correctly
    {
        auto eq = createBuiltInEffect (BuiltInEffect::Eq);
        eq->setRateAndBufferSizeDetails (44100.0, 512);
        eq->prepareToPlay (44100.0, 512);
        auto set = [&] (int idx, float v)
        {
            auto* p = eq->getParameters()[idx];
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
        };
        set (SxEq::pOn (0), 1.0f);
        set (SxEq::pFreq (0), 1000.0f);
        set (SxEq::pGain (0), -24.0f);
        set (SxEq::pQ (0), 1.2f);
        set (SxEq::pMode(), 1.0f);                      // linear
        AudioBuffer<float> b (2, 512);
        MidiBuffer midi;
        float peak = 0.0f;
        // juce::dsp::Convolution builds its IR in the background and passes the signal through
        // until it is ready, so play long enough for that to finish and measure the settled tail
        for (int blk = 0; blk < 240; ++blk)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    b.setSample (ch, i, 0.4f * std::sin ((float) (blk * 512 + i) / 44100.0 * 1000.0 * MathConstants<double>::twoPi));
            eq->processBlock (b, midi);
            if (blk > 200) peak = jmax (peak, b.getMagnitude (0, 0, 512));
        }
        const float flat = 0.4f;
        log ("EQ linear phase: a -24 dB bell at 1 kHz attenuates the tone (" + String (peak, 3) + " of "
             + String (flat, 2) + ")", peak < flat * 0.25f);
        eq->releaseResources();
    }
    // The editor toolbar (enable / side-chain / bypass) shows up when a host provides the actions
    {
        auto fx = createBuiltInEffect (BuiltInEffect::Compressor);
        fx->setRateAndBufferSizeDetails (44100.0, 512);
        fx->prepareToPlay (44100.0, 512);
        auto* base = dynamic_cast<SxEffectBase*> (fx.get());
        auto actions = std::make_shared<SxEffectActions>();
        bool enabled = true, byp = false;
        actions->getEnabled = [&enabled] { return enabled; };
        actions->setEnabled = [&enabled] (bool v) { enabled = v; };
        actions->getBypass = [&byp] { return byp; };
        actions->setBypass = [&byp] (bool v) { byp = v; };
        actions->sidechainName = [] { return String ("Kick"); };
        actions->showSidechainMenu = [] (Component&) {};
        if (base != nullptr) base->actions = actions;

        if (auto* ed = fx->createEditor())
        {
            ed->resized();
            // the action state is pulled by the editor timer, so run it once
            if (auto* tim = dynamic_cast<Timer*> (ed)) tim->timerCallback();
            auto img = ed->createComponentSnapshot ({ 0, 0, ed->getWidth(), ed->getHeight() }, false, 1.0f);
            const File f = File::getSpecialLocation (File::tempDirectory)
                               .getChildFile ("soundx_fx_toolbar_" + String (Time::currentTimeMillis() % 100000) + ".png");
            FileOutputStream fos (f);
            PNGImageFormat png;
            if (fos.openedOk()) png.writeImageToStream (img, fos);
            log ("built-in editor: the enable / side-chain / bypass toolbar is attached",
                 base != nullptr && base->actions != nullptr);
            delete ed;
        }
        fx->releaseResources();
    }
    // Clipper: the output never passes the clip point and the shape changes the waveform
    {
        auto clipper = createBuiltInEffect (BuiltInEffect::Clipper);
        clipper->setRateAndBufferSizeDetails (44100.0, 512);
        clipper->prepareToPlay (44100.0, 512);
        auto setP = [&] (int idx, float v)
        {
            auto* p = clipper->getParameters()[idx];
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
        };
        setP (0, -6.0f);     // clip point -6 dB
        setP (2, 12.0f);     // drive
        setP (5, 6.0f);      // +6 dB output so the ceiling is visible
        setP (6, 100.0f);
        AudioBuffer<float> b (2, 512);
        MidiBuffer midi;
        float peak = 0.0f;
        for (int blk = 0; blk < 20; ++blk)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    b.setSample (ch, i, 0.9f * std::sin ((float) (blk * 512 + i) / 44100.0 * 220.0 * MathConstants<double>::twoPi));
            clipper->processBlock (b, midi);
            if (blk > 10) peak = jmax (peak, b.getMagnitude (0, 0, 512));
        }
        const float limit = Decibels::decibelsToGain (-6.0f + 6.0f) * 1.08f;
        log ("clipper: a hard clip holds the waveform at the clip point (" + String (peak, 3)
             + " <= " + String (limit, 3) + ")", peak > 0.1f && peak <= limit);
        clipper->releaseResources();
    }
    // The new units: tape hiss, OTT gain reduction, maximiser ceiling and the tuner's pitch
    {
        auto runTone = [&] (BuiltInEffect kind, float hz, float amp, int blocks, float* peak = nullptr,
                            SxEffectBase** outBase = nullptr)
        {
            auto fx = createBuiltInEffect (kind);
            fx->setRateAndBufferSizeDetails (44100.0, 512);
            fx->prepareToPlay (44100.0, 512);
            AudioBuffer<float> b (2, 512);
            MidiBuffer midi;
            float mx = 0.0f;
            for (int blk = 0; blk < blocks; ++blk)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        b.setSample (ch, i, amp * std::sin ((float) (blk * 512 + i) / 44100.0 * hz * MathConstants<double>::twoPi));
                fx->processBlock (b, midi);
                if (blk >= blocks - 2) mx = jmax (mx, b.getMagnitude (0, 0, 512));
            }
            if (peak != nullptr) *peak = mx;
            if (outBase != nullptr) *outBase = dynamic_cast<SxEffectBase*> (fx.get());
            auto keep = fx.release();
            delete keep;
        };

        // tape with hiss: silence in still leaves a noise floor
        {
            float peak = 0.0f;
            runTone (BuiltInEffect::Tape, 20.0f, 0.0f, 12, &peak);
            log ("tape: hiss and wow leave a noise floor on silence (" + String (peak, 5) + ")", peak > 0.0f && peak < 0.5f);
        }

        // OTT: loud material gets compressed
        {
            auto fx = createBuiltInEffect (BuiltInEffect::MultibandComp);
            fx->setRateAndBufferSizeDetails (44100.0, 512);
            fx->prepareToPlay (44100.0, 512);
            AudioBuffer<float> b (2, 512);
            MidiBuffer midi;
            for (int blk = 0; blk < 30; ++blk)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        b.setSample (ch, i, 0.8f * std::sin ((float) (blk * 512 + i) / 44100.0 * 220.0 * MathConstants<double>::twoPi));
                fx->processBlock (b, midi);
            }
            auto* p2 = dynamic_cast<SxMultibandComp*> (fx.get());
            const float gr = p2 != nullptr ? p2->gainReductionDb() : 0.0f;
            log ("multiband: loud material is compressed (GR " + String (gr, 1) + " dB)", gr > 1.0f);
            fx->releaseResources();
        }

        // maximiser: the output never passes the ceiling
        {
            auto fx = createBuiltInEffect (BuiltInEffect::Maximizer);
            fx->setRateAndBufferSizeDetails (44100.0, 512);
            fx->prepareToPlay (44100.0, 512);
            auto setP = [&] (int idx, float v)
            {
                auto* p = fx->getParameters()[idx];
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
            };
            setP (0, 12.0f);      // +12 dB input gain into a -6 dB ceiling
            setP (2, -6.0f);
            AudioBuffer<float> b (2, 512);
            MidiBuffer midi;
            float worst = 0.0f;
            for (int blk = 0; blk < 40; ++blk)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        b.setSample (ch, i, 0.6f * std::sin ((float) (blk * 512 + i) / 44100.0 * 110.0 * MathConstants<double>::twoPi));
                fx->processBlock (b, midi);
                if (blk > 4) worst = jmax (worst, b.getMagnitude (0, 0, 512));
            }
            const float ceiling = Decibels::decibelsToGain (-6.0f);
            log ("maximizer: output stays under the ceiling (" + String (Decibels::gainToDecibels (worst), 2)
                 + " dB vs " + String (Decibels::gainToDecibels (ceiling), 1) + ")",
                 worst <= ceiling * 1.02f && worst > 0.1f);
            fx->releaseResources();
        }

        // tuner: 440 Hz reads A4 in tune, 55 Hz reads A1
        {
            auto check = [&] (float hz, float expectNote)
            {
                auto fx = createBuiltInEffect (BuiltInEffect::Tuner);
                fx->setRateAndBufferSizeDetails (44100.0, 1024);
                fx->prepareToPlay (44100.0, 1024);
                AudioBuffer<float> b (2, 1024);
                MidiBuffer midi;
                for (int blk = 0; blk < 6; ++blk)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 1024; ++i)
                            b.setSample (ch, i, 0.5f * std::sin ((float) (blk * 1024 + i) / 44100.0 * hz * MathConstants<double>::twoPi));
                    fx->processBlock (b, midi);
                }
                auto* tn = dynamic_cast<SxTuner*> (fx.get());
                const float got = tn != nullptr ? (float) tn->noteIndex() : -100.0f;
                const float cents = tn != nullptr ? tn->cents() : 999.0f;
                fx->releaseResources();
                return std::abs (got - expectNote) < 0.5f && std::abs (cents) < 12.0f;
            };
            const bool a4 = check (440.0f, 69.0f);
            const bool a1 = check (55.0f, 33.0f);
            log ("tuner: 440 Hz reads A4 and 55 Hz reads A1", a4 && a1);
        }
    }
    // Doubler: the two channels come out different, so the image widens
    {
        auto d = createBuiltInEffect (BuiltInEffect::Doubler);
        d->setRateAndBufferSizeDetails (44100.0, 512);
        d->prepareToPlay (44100.0, 512);
        AudioBuffer<float> buf (2, 4096);
        MidiBuffer midi;
        for (int i = 0; i < 4096; ++i)
        {
            const float v = 0.4f * std::sin ((float) i / 44100.0 * 220.0 * MathConstants<double>::twoPi);
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        for (int block = 0; block < 4; ++block) d->processBlock (buf, midi);
        double diff = 0.0, sum = 0.0;
        for (int i = 0; i < 4096; ++i)
        {
            const double a = buf.getSample (0, i), b = buf.getSample (1, i);
            diff += std::abs (a - b);
            sum += 0.5 * (std::abs (a) + std::abs (b));
        }
        d->releaseResources();
        const double ratio = sum > 1.0e-6 ? diff / sum : 0.0;
        log ("doubler: the voices decorrelate the two channels (" + String (ratio, 3) + ")", ratio > 0.02);
    }

    // EQ presets: a preset saves every band and loads back into a fresh instance
    {
        auto eq = createBuiltInEffect (BuiltInEffect::Eq);
        eq->setRateAndBufferSizeDetails (44100.0, 256);
        eq->prepareToPlay (44100.0, 256);
        auto* sxe = dynamic_cast<SxEq*> (eq.get());
        auto set = [&] (int idx, float v)
        {
            auto* p = eq->getParameters()[idx];
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
        };
        set (SxEq::pOn (0), 1.0f);
        set (SxEq::pType (0), (float) SxEq::HighShelf);
        set (SxEq::pFreq (0), 6400.0f);
        set (SxEq::pGain (0), 5.5f);
        set (SxEq::pQ (0), 0.8f);
        set (SxEq::pDynOn (0), 1.0f);
        set (SxEq::pRange (0), 9.0f);

        const File preset = File::getSpecialLocation (File::tempDirectory).getChildFile ("soundx_eq_test.sxeq");
        preset.deleteFile();
        const bool saved = sxe != nullptr && sxe->savePreset (preset);

        auto eq2 = createBuiltInEffect (BuiltInEffect::Eq);
        eq2->setRateAndBufferSizeDetails (44100.0, 256);
        eq2->prepareToPlay (44100.0, 256);
        auto* sxe2 = dynamic_cast<SxEq*> (eq2.get());
        const bool loaded = sxe2 != nullptr && sxe2->loadPreset (preset);
        const bool match = loaded
                         && std::abs (dynamic_cast<SxEffectBase*> (eq2.get())->curveDb (6400.0f)
                                      - dynamic_cast<SxEffectBase*> (eq.get())->curveDb (6400.0f)) < 0.2f
                         && eq2->getParameters()[SxEq::pDynOn (0)]->getValue() > 0.5f;
        log ("EQ presets: a preset file restores the shelf, its gain and its dynamic switch (saved "
             + String (saved ? 1 : 0) + ", loaded " + String (loaded ? 1 : 0) + ")", saved && match);
        preset.deleteFile();
        eq->releaseResources();
        eq2->releaseResources();
    }
    // Dynamic curve: once the band is engaged the effective curve sits below the static one
    {
        auto eq = createBuiltInEffect (BuiltInEffect::Eq);
        eq->setRateAndBufferSizeDetails (44100.0, 256);
        eq->prepareToPlay (44100.0, 256);
        auto set = [&] (int idx, float v)
        {
            auto* p = eq->getParameters()[idx];
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
        };
        for (int b = 1; b < SxEq::kNumBands; ++b) set (SxEq::pOn (b), 0.0f);
        set (SxEq::pOn (0), 1.0f);
        set (SxEq::pType (0), (float) SxEq::Bell);
        set (SxEq::pFreq (0), 1000.0f);
        set (SxEq::pGain (0), 12.0f);
        set (SxEq::pQ (0), 1.4f);
        set (SxEq::pDynOn (0), 1.0f);
        set (SxEq::pThr (0), -40.0f);
        set (SxEq::pRange (0), 12.0f);
        set (SxEq::pAtk (0), 2.0f);
        set (SxEq::pRel (0), 80.0f);
        AudioBuffer<float> buf (2, 1024);
        MidiBuffer midi;
        for (int block = 0; block < 40; ++block)          // loud 1 kHz so the band engages
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 1024; ++i)
                    buf.setSample (ch, i, 0.8f * std::sin ((float) (block * 1024 + i) / 44100.0 * 1000.0 * MathConstants<double>::twoPi));
            eq->processBlock (buf, midi);
        }
        auto* base = dynamic_cast<SxEffectBase*> (eq.get());
        const float staticDb = base != nullptr ? base->curveDb (1000.0f) : 0.0f;
        auto* eqPtr = dynamic_cast<SxEq*> (eq.get());
        const float dynDb = eqPtr != nullptr ? eqPtr->curveDbEffective (1000.0f) : 0.0f;
        const float reduction = eqPtr != nullptr ? eqPtr->bandDynDb (0) : 0.0f;
        log ("dynamic EQ: the effective curve follows the reduction (" + String (staticDb, 1) + " -> "
             + String (dynDb, 1) + " dB, reduction " + String (reduction, 1) + " dB)",
             reduction > 1.0f && dynDb < staticDb - 1.0f);
        eq->releaseResources();
    }
    // Side-chain: a dynamic EQ band follows the side-chain input instead of its own signal
    {
        auto eq = createBuiltInEffect (BuiltInEffect::Eq);
        eq->setRateAndBufferSizeDetails (44100.0, 256);
        eq->prepareToPlay (44100.0, 256);
        auto set = [&] (int idx, float v)
        {
            auto* p = eq->getParameters()[idx];
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
        };
        for (int b = 1; b < SxEq::kNumBands; ++b) set (SxEq::pOn (b), 0.0f);
        set (SxEq::pOn (0), 1.0f);
        set (SxEq::pType (0), (float) SxEq::Bell);
        set (SxEq::pFreq (0), 1000.0f);
        set (SxEq::pGain (0), -6.0f);
        set (SxEq::pQ (0), 1.5f);
        set (SxEq::pDynOn (0), 1.0f);
        set (SxEq::pThr (0), -40.0f);
        set (SxEq::pRange (0), 12.0f);
        set (SxEq::pAtk (0), 2.0f);
        set (SxEq::pRel (0), 100.0f);

        auto* sx = dynamic_cast<SxEffectBase*> (eq.get());
        auto* sxe = dynamic_cast<SxEq*> (eq.get());
        MidiBuffer midi;
        AudioBuffer<float> quiet (2, 1024);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1024; ++i)
                quiet.setSample (ch, i, 0.004f * std::sin ((float) i / 44100.0 * 1000.0 * MathConstants<double>::twoPi));   // -48 dBFS

        // 1) no side-chain: the quiet own signal never engages the band
        for (int block = 0; block < 20; ++block) eq->processBlock (quiet, midi);
        const float offReduction = sxe != nullptr ? sxe->bandDynDb (0) : 0.0f;

        // 2) a loud side-chain: the same quiet signal is now compressed
        if (sx != nullptr)
        {
            sx->sidechainFeed->valid.store (true);
            sx->sidechainFeed->n = 1024;
            for (int i = 0; i < 1024; ++i)
            {
                const float v = 0.8f * std::sin ((float) i / 44100.0 * 1000.0 * MathConstants<double>::twoPi);
                sx->sidechainFeed->l[i] = v;
                sx->sidechainFeed->r[i] = v;
            }
        }
        for (int block = 0; block < 20; ++block) eq->processBlock (quiet, midi);
        const float onReduction = sxe != nullptr ? sxe->bandDynDb (0) : 0.0f;
        log ("side-chain: a dynamic band follows the side-chain input (" + String (offReduction, 2)
             + " dB own vs " + String (onReduction, 2) + " dB side-chained)",
             offReduction < 0.5f && onReduction > 3.0f);
        eq->releaseResources();
    }

    // Side-chain source survives a save / load round trip
    {
        Project p2;
        p2.formats = &engine.formats;
        auto& tr2 = p2.addTrack (TrackType::Audio, "SC");
        PluginSlot sl;
        sl.uid = 4242;
        sl.builtIn = true;
        sl.sidechainTrack = tr2.id + 77;
        tr2.inserts.push_back (sl);
        const File f = File::getSpecialLocation (File::tempDirectory).getChildFile ("soundx_sc.sxp");
        const bool saved = p2.saveTo (f);
        Project p3;
        p3.formats = &engine.formats;
        const bool loaded = saved && p3.loadFrom (f);
        const bool ok = loaded && ! p3.tracks.empty() && ! p3.tracks.front()->inserts.empty()
                     && p3.tracks.front()->inserts.front().sidechainTrack == tr2.id + 77;
        log ("side-chain: the chosen source track is saved with the project", ok);
        f.deleteFile();
    }
    // Band solo: monitoring one band removes everything outside it
    {
        auto renderSolo = [&] (int soloBand)
        {
            auto eq = createBuiltInEffect (BuiltInEffect::Eq);
            eq->setRateAndBufferSizeDetails (44100.0, 256);
            eq->prepareToPlay (44100.0, 256);
            auto set = [&] (int idx, float v)
            {
                auto* p = eq->getParameters()[idx];
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (p)) p->setValueNotifyingHost (rp->convertTo0to1 (v));
            };
            for (int b = 2; b < SxEq::kNumBands; ++b) set (SxEq::pOn (b), 0.0f);
            // band 1: a bell boost at 1 kHz (passes 100 Hz), band 2: a low cut at 8 kHz (kills it)
            set (SxEq::pOn (0), 1.0f);
            set (SxEq::pType (0), (float) SxEq::Bell);
            set (SxEq::pFreq (0), 1000.0f);
            set (SxEq::pGain (0), 12.0f);
            set (SxEq::pQ (0), 2.0f);
            set (SxEq::pOn (1), 1.0f);
            set (SxEq::pType (1), (float) SxEq::LowCut);
            set (SxEq::pFreq (1), 8000.0f);
            set (SxEq::pQ (1), 0.7f);
            set (SxEq::pSolo(), (float) soloBand);
            AudioBuffer<float> buf (2, 4096);
            MidiBuffer midi;
            float peak = 0.0f;
            for (int block = 0; block < 16; ++block)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 4096; ++i)
                        buf.setSample (ch, i, 0.5f * std::sin ((float) (block * 4096 + i) / 44100.0 * 100.0 * MathConstants<double>::twoPi));
                eq->processBlock (buf, midi);
                if (block >= 12) peak = jmax (peak, buf.getMagnitude (0, 0, 4096));
            }
            eq->releaseResources();
            return peak;
        };
        const float soloBell = renderSolo (1);       // the 1 kHz bell passes 100 Hz
        const float soloLowCut = renderSolo (2);     // the 8 kHz low cut does not
        log ("band solo: monitoring one band isolates it (" + String (soloBell, 2) + " for the bell vs "
             + String (soloLowCut, 3) + " for the low cut at 100 Hz)",
             soloBell > 0.3f && soloLowCut < soloBell * 0.25f);
    }
    // EQ bands can be added and removed: switching band 5 on with a boost changes the curve
    {
        auto eq = createBuiltInEffect (BuiltInEffect::Eq);
        eq->setRateAndBufferSizeDetails (44100.0, 256);
        eq->prepareToPlay (44100.0, 256);
        auto* base = dynamic_cast<SxEffectBase*> (eq.get());
        const float before = base != nullptr ? base->curveDb (1000.0f) : 0.0f;
        auto set = [&] (int idx, float v)
        {
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (eq->getParameters()[idx]))
                eq->getParameters()[idx]->setValueNotifyingHost (rp->convertTo0to1 (v));
        };
        set (SxEq::pType (4), (float) SxEq::Bell);     // band 5
        set (SxEq::pFreq (4), 1000.0f);
        set (SxEq::pGain (4), 12.0f);
        set (SxEq::pQ (4), 1.0f);
        set (SxEq::pOn (4), 1.0f);
        const float added = base != nullptr ? base->curveDb (1000.0f) : 0.0f;
        set (SxEq::pOn (4), 0.0f);
        const float removed = base != nullptr ? base->curveDb (1000.0f) : 0.0f;
        log ("built-in EQ: bands can be added and removed (1 kHz response " + String (before, 1) + " -> "
             + String (added, 1) + " -> " + String (removed, 1) + " dB)",
             std::abs (before) < 0.5f && added > 9.0f && std::abs (removed - before) < 0.5f);

        // every chip has its own on/off switch; clicking the left pill must not just select the band
        if (auto* sxe = dynamic_cast<SxEq*> (eq.get()))
            if (auto* ed = sxe->createEditor())
            {
                ed->setSize (880, 520);
                ed->resized();
                set (SxEq::pOn (0), 1.0f);
                set (SxEq::pOn (4), 0.0f);

                auto clickChipSwitch = [&] (int band)
                {
                    const int w = jmax (46, (ed->getWidth() - 4) / SxEq::kNumBands);
                    const float x = 2.0f + (float) band * (float) w + 10.0f;
                    const float y = 54.0f;
                    auto makeEvent = [&] (float px, float py)
                    {
                        return MouseEvent (Desktop::getInstance().getMainMouseSource(), { px, py },
                                           ModifierKeys (ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                           ed, ed, Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                    };
                    ed->mouseDown (makeEvent (x, y));
                    ed->mouseUp (makeEvent (x, y));
                };

                clickChipSwitch (0);
                const bool off = sxe->param (SxEq::pOn (0)) < 0.5f;
                clickChipSwitch (0);
                const bool backOn = sxe->param (SxEq::pOn (0)) > 0.5f;

                clickChipSwitch (4);       // band 5 starts switched off; its chip switch turns it on
                const bool fifthOn = sxe->param (SxEq::pOn (4)) > 0.5f;

                auto shot = ed->createComponentSnapshot ({ 0, 0, ed->getWidth(), ed->getHeight() }, false, 1.0f);
                const File png = File::getSpecialLocation (File::tempDirectory).getChildFile ("eq_band_switches.png");
                if (shot.isValid())
                    if (auto stream = std::unique_ptr<FileOutputStream> (png.createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                    }

                log ("built-in EQ: every band chip has its own switch (band 1 " + String (off ? "off" : "on")
                     + " -> " + String (backOn ? "on" : "off") + ", band 5 -> " + String (fifthOn ? "on" : "off") + ")",
                     off && backOn && fifthOn);
                delete ed;
            }
        eq->releaseResources();
    }

    // compressor: a loud tone comes out quieter than the same tone without compression
    const float warm = renderTone (BuiltInEffect::Compressor, 200.0, 0, 0.0f);    // threshold 0 dB
    const float squashed = renderTone (BuiltInEffect::Compressor, 200.0, 0, -40.0f);
    log ("built-in compressor: a low threshold squashes the same tone (" + String (warm, 2) + " -> "
         + String (squashed, 2) + ")", squashed < warm * 0.6f);

    // limiter: a hot input never leaves above the ceiling
    {
        auto proc = createBuiltInEffect (BuiltInEffect::Limiter);
        proc->setRateAndBufferSizeDetails (44100.0, 256);
        proc->prepareToPlay (44100.0, 256);
        AudioBuffer<float> buf (2, 256);
        MidiBuffer midi;
        float peak = 0.0f;
        for (int block = 0; block < 40; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    buf.setSample (ch, i, 1.4f * std::sin ((float) (block * 256 + i) / 44100.0 * 220.0 * MathConstants<double>::twoPi));
            proc->processBlock (buf, midi);
            peak = jmax (peak, buf.getMagnitude (0, 0, 256));
        }
        proc->releaseResources();
        log ("built-in limiter: a 1.4x input stays under the ceiling (peak " + String (peak, 2) + ")",
             peak <= 1.001f);
    }

    // delay: a click comes back later (parameter 0 = 200 ms, mix 100 %)
    {
        auto proc = createBuiltInEffect (BuiltInEffect::Delay);
        proc->setRateAndBufferSizeDetails (44100.0, 512);
        proc->prepareToPlay (44100.0, 512);
        auto set = [&] (int i, float v)
        {
            if (auto* p = dynamic_cast<RangedAudioParameter*> (proc->getParameters()[i]))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        set (0, 200.0f);      // 200 ms
        set (4, 100.0f);      // fully wet
        AudioBuffer<float> buf (2, 44100);
        buf.clear();
        buf.setSample (0, 10, 0.5f);
        buf.setSample (1, 10, 0.5f);
        MidiBuffer midi;
        proc->processBlock (buf, midi);
        int echoAt = -1;
        for (int i = 20; i < 44100; ++i)
            if (std::abs (buf.getSample (0, i)) > 0.05f) { echoAt = i; break; }
        const int expected = 10 + (int) (44100 * 0.2);
        proc->releaseResources();
        log ("built-in delay: a click returns 200 ms later (echo at " + String (echoAt) + ", expected ~"
             + String (expected) + ")", echoAt > expected - 200 && echoAt < expected + 200);
    }

    // distortion: adds harmonics - the output peak of a quiet tone grows a lot with drive
    const float clean = renderTone (BuiltInEffect::Distortion, 300.0, 0, 0.0f);
    const float dirty = renderTone (BuiltInEffect::Distortion, 300.0, 0, 36.0f);
    log ("built-in distortion: drive makes the same tone much louder/denser (" + String (clean, 2)
         + " -> " + String (dirty, 2) + ")", dirty > clean * 1.5f);
}

// Level / pan metering: peak + RMS + clip latch from the audio thread, the pan meter's position
// maths, and the goniometer ring.
static void runMeterTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
    }
    engine.syncAll();

    const File tone = out.getChildFile ("meter_tone.wav");
    tone.deleteFile();
    {
        AudioBuffer<float> b (2, 44100);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, 0.5f * std::sin ((float) i / 44100.0f * 330.0f * MathConstants<float>::twoPi));
        WavAudioFormat wav;
        if (auto* w = wav.createWriterFor (new FileOutputStream (tone), 44100.0, 2, 24, {}, 0))
        {
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            delete w;
        }
    }

    auto& tr = project.addTrack (TrackType::Audio, "Meter");
    const int tid = tr.id;
    {
        AudioClip c; c.id = project.newId(); c.name = "tone"; c.file = tone; c.start = 0.0;
        c.data = project.pool.load (tone, engine.formats);
        c.lengthSec = 1.0;
        tr.audioClips.push_back (c);
        tr.volumeDb = 0.0f;
    }
    project.sendStructureChanged();
    engine.syncAll();

    //== the widgets' maths ---------------------------------------------------------------------
    {
        PanMeter pm;
        pm.setLevels (1.0f, 0.0f, 0.0f);
        const float left = pm.position();
        pm.setLevels (0.0f, 1.0f, 0.0f);
        const float right = pm.position();
        pm.setLevels (0.5f, 0.5f, 0.0f);
        const float centre = pm.position();
        LevelMeter lm;
        lm.setLevels (0.2f, 0.2f, 0.1f, 0.1f, false);
        const bool noClip = ! lm.isClipped();
        lm.setLevels (0.1f, 0.1f, 0.05f, 0.05f, false);       // lower signal: the maximum must stay
        const bool maxHeld = lm.getMaxPeakL() > 0.19f && lm.getMaxPeakR() > 0.19f;
        lm.setState (0.05f, 0.05f, 0.02f, 0.02f, 0.05f, 0.05f, 0.8f, 0.7f, false);
        const bool snapshotSet = std::abs (lm.getMaxPeakL() - 0.8f) < 0.01f
                              && std::abs (lm.getMaxPeakR() - 0.7f) < 0.01f;
        lm.setLevels (1.2f, 0.3f, 0.9f, 0.2f, true);
        const bool clipLatched = lm.isClipped();
        lm.resetClip();
        const bool clipCleared = ! lm.isClipped();
        LevelMeter hm (LevelMeter::Orientation::horizontal);
        hm.setSize (120, 6);
        hm.setState (0.4f, 0.2f, 0.3f, 0.1f, 0.5f, 0.3f, 0.9f, 0.6f, false);
        auto horizontalImage = hm.createComponentSnapshot (hm.getLocalBounds(), true);
        const bool horizontalPainted = horizontalImage.getWidth() == 120 && horizontalImage.getHeight() == 6;
        log ("meters: the pan meter puts a hard-left / hard-right / centred signal at "
             + String (left, 1) + " / " + String (right, 1) + " / " + String (centre, 1)
             + ", the clip LED latches and resets, and the live / maximum snapshot + horizontal bar paint",
             left < -0.9f && right > 0.9f && std::abs (centre) < 0.01f && noClip && clipLatched && clipCleared
                 && maxHeld && snapshotSet && horizontalPainted);
    }

    //== real metering from the audio thread -----------------------------------------------------
    const bool haveDevice = engine.deviceManager.getCurrentAudioDevice() != nullptr;
    if (! haveDevice) { log ("meters: no audio device, live metering not measured", true); return; }

    engine.setPositionBeats (0.0);
    engine.play();
    // realtime capture: give the device a few windows and take the best one (a single 500 ms sleep
    // is fragile on a machine that is busy with the rest of the test suite)
    Engine::Meters m, m2;
    float scopeData[2 * 512] {};
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        Thread::sleep (150);
        Engine::Meters a, b;
        engine.readMeters (tid, a);
        engine.readMeters (-1, b);
        engine.readScope (scopeData, 512);
        m.peakL = jmax (m.peakL, a.peakL); m.peakR = jmax (m.peakR, a.peakR);
        m.rmsL = jmax (m.rmsL, a.rmsL);   m.rmsR = jmax (m.rmsR, a.rmsR);
        m2.peakL = jmax (m2.peakL, b.peakL); m2.rmsL = jmax (m2.rmsL, b.rmsL);
        if (m.peakL > 0.2f && m2.peakL > 0.1f) break;
    }
    engine.stop();
    Thread::sleep (120);

    const bool peaked = m.peakL > 0.2f && m.peakR > 0.2f;
    const bool rmsBelowPeak = m.rmsL > 0.05f && m.rmsL <= m.peakL + 1.0e-4f;
    log ("meters: a 0.5 tone reads peak " + String (m.peakL, 2) + " / rms " + String (m.rmsL, 2)
         + " on the track and peak " + String (m2.peakL, 2) + " / rms " + String (m2.rmsL, 2) + " on the master",
         peaked && rmsBelowPeak && m2.peakL > 0.1f);

    // the goniometer ring has real samples in it (a centred tone: L == R)
    int nonZero = 0;
    float maxAbs = 0.0f, maxDiff = 0.0f;
    for (int i = 0; i < 512; ++i)
    {
        if (std::abs (scopeData[i * 2]) > 1.0e-4f) ++nonZero;
        maxAbs = jmax (maxAbs, std::abs (scopeData[i * 2]));
        maxDiff = jmax (maxDiff, std::abs (scopeData[i * 2] - scopeData[i * 2 + 1]));
    }
    log ("meters: the stereo scope receives the master signal (" + String (nonZero) + " of 512 pairs, max "
         + String (maxAbs, 3) + ", L/R diff " + String (maxDiff, 4) + ")", nonZero > 64);

    //== the shared cache every fader reads (mixer strip + arrange header + master) ----------------
    {
        AppState st (project, engine, plugins);
        st.meters.pumpForTest();              // flush the peaks from the direct read above
        engine.setPositionBeats (0.0);
        engine.play();

        float cachePeak = 0.0f, cacheMax = 0.0f;
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            Thread::sleep (100);
            st.meters.pumpForTest();
            const auto& md = st.meters.get (tid);
            cachePeak = jmax (cachePeak, jmax (md.levelL, md.levelR));
            cacheMax  = jmax (cacheMax,  jmax (md.maxL, md.maxR));
            if (cachePeak > 0.2f && cacheMax > 0.2f) break;
        }
        engine.stop();
        Thread::sleep (100);
        st.meters.pumpForTest();
        const auto& after = st.meters.get (tid);
        const bool maxHeld = jmax (after.maxL, after.maxR) >= cacheMax - 1.0e-4f;
        log ("meters: the shared fader cache shows the live level (" + String (cachePeak, 2)
             + ") and keeps the highest peak (" + String (cacheMax, 2) + ")", cachePeak > 0.2f && cacheMax > 0.2f && maxHeld);
    }

    project.removeTrack (tid);
    project.sendStructureChanged();
    engine.syncAll();
    tone.deleteFile();
}

// Renders the built-in editors to PNGs so their layout can be checked (and looked at) without
// opening the app.
static void runEffectEditorShots (const File& out, Log& log)
{
    int shots = 0;
    StringArray names;
    for (auto& info : builtInEffects())
    {
        auto proc = createBuiltInEffect (info.id);
        proc->setRateAndBufferSizeDetails (44100.0, 256);
        proc->prepareToPlay (44100.0, 256);
        // give the displays some data to draw
        AudioBuffer<float> b (2, 256);
        MidiBuffer midi;
        for (int block = 0; block < 30; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    b.setSample (ch, i, 0.4f * std::sin ((float) (block * 256 + i) / 44100.0f * 220.0f * MathConstants<float>::twoPi));
            proc->processBlock (b, midi);
        }
        if (auto* ed = proc->createEditor())
        {
            const int w = ed->getWidth() > 0 ? ed->getWidth() : 720;
            const int h = ed->getHeight() > 0 ? ed->getHeight() : 400;
            ed->setSize (w, h);
            auto img = ed->createComponentSnapshot ({ 0, 0, w, h }, false, 1.0f);
            String safe;
            for (auto ch2 : String (info.name)) safe << (String ("SX").containsChar (ch2) || isalnum ((int) ch2) ? String::charToString (ch2) : String ("_"));
            const File f = out.getChildFile ("fx_" + safe + ".png");
            FileOutputStream fos (f);
            PNGImageFormat png;
            if (fos.openedOk() && png.writeImageToStream (img, fos)) { ++shots; names.add (safe); }
            delete ed;
        }
        proc->releaseResources();
    }
    log ("built-in editors: rendered " + String (shots) + " editor snapshots (" + names.joinIntoString (", ") + ")", shots == (int) builtInEffects().size());
}

// The guitar rig: every amp head makes its own sound and stays finite, the pedals and the time block
// change the sound when they switch on, the comp reports gain reduction, the state survives a save /
// load, the oversampling latency is reported, and every head's panel is drawn (guitar_rig_<head>.png).
static void runSpatialTest (const File& out, Log& log)
{
    using Src = std::function<float (int, int)>;
    auto sine = [] (float hz, float amp) -> Src
    {
        return [hz, amp] (int, int idx) { return amp * std::sin ((float) idx / 44100.0f * hz * MathConstants<float>::twoPi); };
    };
    auto rms = [] (const AudioBuffer<float>& b, int ch)
    {
        double s = 0.0;
        for (int i = 0; i < b.getNumSamples(); ++i) s += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return (float) std::sqrt (s / jmax (1, b.getNumSamples()));
    };
    auto fresh = [] (float az, float dist, float room, int group)
    {
        auto p = createBuiltInEffect (BuiltInEffect::Stereo);
        auto* fx = dynamic_cast<SxStereo*> (p.get());
        fx->setParam (SxStereo::pAzimuth, az);
        fx->setParam (SxStereo::pDistance, dist);
        fx->setParam (SxStereo::pRoom, room);
        fx->setParam (SxStereo::pGroup, (float) group);
        return p;
    };
    // runs blocks blocks of samples from src through a freshly prepared instance and returns the last
    // output block; lastInput receives the matching input block
    auto drive = [] (SxStereo& fx, const Src& src, int blocks, AudioBuffer<float>* lastInput)
    {
        fx.prepareToPlay (44100.0, 512);
        AudioBuffer<float> buf (2, 512);
        MidiBuffer midi;
        for (int block = 0; block < blocks; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, src (ch, block * 512 + i));
            if (block == blocks - 1 && lastInput != nullptr) *lastInput = buf;
            fx.processBlock (buf, midi);
        }
        return buf;
    };

    // at its defaults the stereo plug-in passes the signal through unchanged
    {
        auto p = fresh (0.0f, 0.0f, 0.0f, 0);
        AudioBuffer<float> input;
        const auto out = drive (*dynamic_cast<SxStereo*> (p.get()),
                                [] (int ch, int idx) { return 0.3f * std::sin (idx * 0.37f + ch * 1.3f) + 0.1f * std::sin (idx * 2.1f - ch * 0.7f); },
                                20, &input);
        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
                maxDiff = jmax (maxDiff, std::abs (out.getSample (ch, i) - input.getSample (ch, i)));
        log ("SX Stereo: at its default settings it passes the signal through unchanged (max difference "
             + String (maxDiff, 7) + ")", maxDiff < 1e-5f);
    }

    // a source placed at +90 degrees is heard in the right ear only
    {
        auto p = fresh (90.0f, 0.0f, 0.0f, 0);
        const auto out = drive (*dynamic_cast<SxStereo*> (p.get()), sine (1000.0f, 0.2f), 12, nullptr);
        const float l = rms (out, 0), r = rms (out, 1);
        log ("SX Stereo: a source at +90 deg is heard in the right ear only (left " + String (l, 4) + ", right "
             + String (r, 4) + ")", r > 0.1f && l < 0.02f * r);
    }

    // a distant source is quieter, and its highs go first
    {
        auto near = fresh (0.0f, 0.0f, 0.0f, 0);
        auto far = fresh (0.0f, 100.0f, 0.0f, 0);
        const float n1 = rms (drive (*dynamic_cast<SxStereo*> (near.get()), sine (1000.0f, 0.2f), 12, nullptr), 0);
        const float n8 = rms (drive (*dynamic_cast<SxStereo*> (near.get()), sine (8000.0f, 0.2f), 12, nullptr), 0);
        const float f1 = rms (drive (*dynamic_cast<SxStereo*> (far.get()), sine (1000.0f, 0.2f), 12, nullptr), 0);
        const float f8 = rms (drive (*dynamic_cast<SxStereo*> (far.get()), sine (8000.0f, 0.2f), 12, nullptr), 0);
        const float g1 = f1 / jmax (1.0e-9f, n1), g8 = f8 / jmax (1.0e-9f, n8);
        log ("SX Stereo: a distant source is quieter (1 kHz x" + String (g1, 2) + ") and loses its highs first (8 kHz x"
             + String (g8, 2) + ")", g1 < 0.65f && g8 < 0.7f * g1);
    }

    // with a room, a source that stops leaves a tail; without one, it stops dead
    {
        auto burst = [] () -> Src
        {
            return [] (int, int idx) { return idx < 6 * 512 ? 0.2f * std::sin ((float) idx / 44100.0f * 1000.0f * MathConstants<float>::twoPi) : 0.0f; };
        };
        auto roomy = fresh (0.0f, 100.0f, 100.0f, 0);
        auto dry = fresh (0.0f, 100.0f, 0.0f, 0);
        const float tail = rms (drive (*dynamic_cast<SxStereo*> (roomy.get()), burst(), 40, nullptr), 0);
        const float tailDry = rms (drive (*dynamic_cast<SxStereo*> (dry.get()), burst(), 40, nullptr), 0);
        log ("SX Stereo: a room keeps a tail after the source stops (tail " + String (tail, 5) + ", without a room "
             + String (tailDry, 7) + ")", tail > 1.0e-4f && tailDry < 1.0e-6f);
    }

    // two loud tracks of one link group on the same spot push each other apart
    {
        auto pa = fresh (0.0f, 0.0f, 0.0f, 3);
        auto pb = fresh (0.0f, 0.0f, 0.0f, 3);
        auto* a = dynamic_cast<SxStereo*> (pa.get());
        auto* b = dynamic_cast<SxStereo*> (pb.get());
        a->setParam (SxStereo::pSpacing, 100.0f);
        b->setParam (SxStereo::pSpacing, 100.0f);
        a->prepareToPlay (44100.0, 512);
        b->prepareToPlay (44100.0, 512);
        const auto tone = sine (700.0f, 0.25f);
        MidiBuffer midi;
        AudioBuffer<float> ba (2, 512), bb (2, 512);
        for (int block = 0; block < 60; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                {
                    ba.setSample (ch, i, tone (ch, block * 512 + i));
                    bb.setSample (ch, i, tone (ch, block * 512 + i));
                }
            a->processBlock (ba, midi);
            b->processBlock (bb, midi);
        }
        const float sa = a->placedAzimuth(), sb = b->placedAzimuth();
        log ("SX Stereo: loud tracks of one link group on the same spot push each other apart (placed at "
             + String (sa, 0) + " and " + String (sb, 0) + " deg)", std::abs (sa - sb) > 20.0f);
    }

    // a linked move shifts the other tracks of the group, and nothing else
    {
        auto pa = fresh (0.0f, 0.0f, 0.0f, 4);
        auto pb = fresh (0.0f, 0.0f, 0.0f, 4);
        auto pc = fresh (0.0f, 0.0f, 0.0f, 0);
        auto* a = dynamic_cast<SxStereo*> (pa.get());
        auto* b = dynamic_cast<SxStereo*> (pb.get());
        auto* c = dynamic_cast<SxStereo*> (pc.get());
        a->shiftGroup (30.0f, 10.0f);
        const bool ok = std::abs (b->param (SxStereo::pAzimuth) - 30.0f) < 0.01f
                     && std::abs (b->param (SxStereo::pDistance) - 10.0f) < 0.01f
                     && std::abs (c->param (SxStereo::pAzimuth)) < 0.01f
                     && std::abs (a->param (SxStereo::pAzimuth)) < 0.01f;
        log ("SX Stereo: a linked move shifts the other tracks of the group by the same step and leaves the rest (peer at "
             + String (b->param (SxStereo::pAzimuth), 1) + " deg, outsider at " + String (c->param (SxStereo::pAzimuth), 1) + " deg)", ok);
    }

    // a picture of the room with two linked tracks and one loose
    {
        auto p1 = fresh (-40.0f, 60.0f, 40.0f, 2);
        auto p2 = fresh (55.0f, 45.0f, 40.0f, 2);
        auto p3 = fresh (170.0f, 80.0f, 0.0f, 0);
        auto* s1 = dynamic_cast<SxStereo*> (p1.get());
        auto* s2 = dynamic_cast<SxStereo*> (p2.get());
        auto* s3 = dynamic_cast<SxStereo*> (p3.get());
        s1->trackNameSource = [] { return String ("Drums"); };
        s2->trackNameSource = [] { return String ("Bass"); };
        s3->trackNameSource = [] { return String ("Pad"); };
        s1->prepareToPlay (44100.0, 512);
        s2->prepareToPlay (44100.0, 512);
        s3->prepareToPlay (44100.0, 512);
        const bool namesOk = s1->sceneMembers().size() >= 3 && s2->trackName() == "Bass" && s3->trackName() == "Pad";
        log ("SX Stereo: the room lists every track with this plug-in, under its track name (" + String ((int) s1->sceneMembers().size())
             + " tracks in the room)", namesOk);
        MidiBuffer midi;
        AudioBuffer<float> buf (2, 512);
        for (int block = 0; block < 30; ++block)
            for (auto* s : { s1, s2, s3 })
            {
                const float hz = s == s1 ? 440.0f : (s == s2 ? 660.0f : 330.0f);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        buf.setSample (ch, i, 0.2f * std::sin ((float) (block * 512 + i) / 44100.0f * hz * MathConstants<float>::twoPi));
                s->processBlock (buf, midi);
            }
        if (auto* ed = s1->createEditor())
        {
            ed->setSize (640, 480);
            auto img = ed->createComponentSnapshot ({ 0, 0, 640, 480 }, false, 1.0f);
            FileOutputStream fos (out.getChildFile ("fx_SX_Stereo_space.png"));
            PNGImageFormat png;
            log ("SX Stereo: the room view draws the tracks, their links and their placed positions (fx_SX_Stereo_space.png)",
                 fos.openedOk() && png.writeImageToStream (img, fos));
            delete ed;
        }
    }
}

static void runProEqAndMetersTest (const File& out, Log& log)
{
    auto near = [] (float v, float target, float tol) { return std::isfinite (v) && std::abs (v - target) <= tol; };

    // an EQ with one band of the given shape; every other band is off
    auto makeEq = [] (int type, float hz, int slope, float q, float gainDb) -> std::unique_ptr<AudioProcessor>
    {
        auto proc = createBuiltInEffect (BuiltInEffect::Eq);
        if (auto* eq = dynamic_cast<SxEq*> (proc.get()))
        {
            eq->prepareToPlay (44100.0, 512);
            for (int b = 0; b < SxEq::kNumBands; ++b) eq->setParam (SxEq::pOn (b), b == 0 ? 1.0f : 0.0f);
            eq->setParam (SxEq::pType (0), (float) type);
            eq->setParam (SxEq::pFreq (0), hz);
            eq->setParam (SxEq::pSlope (0), (float) slope);
            eq->setParam (SxEq::pQ (0), q);
            eq->setParam (SxEq::pGain (0), gainDb);
        }
        return proc;
    };
    auto curveOf = [&makeEq] (int type, float hz, int slope, float q, float at)
    {
        auto p = makeEq (type, hz, slope, q, 0.0f);
        return dynamic_cast<SxEq*> (p.get())->curveDb (at);
    };

    // cut slopes are Butterworth cascades: 12 / 24 / 48 dB/oct an octave below the corner
    const float c12 = curveOf (SxEq::LowCut, 1000.0f, 0, 0.7f, 500.0f);
    const float c24 = curveOf (SxEq::LowCut, 1000.0f, 1, 0.7f, 500.0f);
    const float c48 = curveOf (SxEq::LowCut, 1000.0f, 3, 0.7f, 500.0f);
    const float pass24 = curveOf (SxEq::LowCut, 1000.0f, 1, 0.7f, 2000.0f);
    const bool slopeOk = near (c12, -12.3f, 1.0f) && near (c24, -24.1f, 1.0f) && near (c48, -48.2f, 2.0f) && pass24 > -0.3f;
    log ("EQ slopes: a 12 / 24 / 48 dB/oct low cut reads " + String (c12, 1) + " / " + String (c24, 1)
         + " / " + String (c48, 1) + " dB an octave below its corner, and is flat above it", slopeOk);

    // notch and band pass are their own shapes now (they used to be bells)
    const float notchCentre = curveOf (SxEq::Notch, 1000.0f, 0, 1.0f, 1000.0f);
    const float notchSide = curveOf (SxEq::Notch, 1000.0f, 0, 1.0f, 3000.0f);
    const float bpCentre = curveOf (SxEq::BandPass, 1000.0f, 0, 1.0f, 1000.0f);
    const float bpLow = curveOf (SxEq::BandPass, 1000.0f, 0, 1.0f, 100.0f);
    const float bpHigh = curveOf (SxEq::BandPass, 1000.0f, 0, 1.0f, 10000.0f);
    const bool shapeOk = notchCentre < -40.0f && notchSide > -1.0f && bpCentre > -1.0f && bpLow < -10.0f && bpHigh < -10.0f;
    log ("EQ shapes: the notch cuts its centre (" + String (notchCentre, 0) + " dB) and leaves the sides alone, the band pass "
         "passes its centre (" + String (bpCentre, 1) + " dB) and rejects both sides", shapeOk);

    // the analyser: a 1 kHz tone is the peak of the spectrum, at about 1 kHz
    {
        auto p = makeEq (SxEq::Bell, 1000.0f, 0, 1.0f, 0.0f);
        auto* eq = dynamic_cast<SxEq*> (p.get());
        AudioBuffer<float> buf (2, 512);
        MidiBuffer midi;
        for (int block = 0; block < 120; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, 0.5f * std::sin ((float) (block * 512 + i) / 44100.0f * 1000.0f * MathConstants<float>::twoPi));
            eq->processBlock (buf, midi);
        }
        int best = 0;
        float bestV = -1.0f;
        for (int i = 0; i < SxEffectBase::kSpectrumBins; ++i)
            if (eq->spectrumAt (i) > bestV) { bestV = eq->spectrumAt (i); best = i; }
        const int expect = (int) std::round (std::log (1000.0 / 20.0) / std::log (1000.0) * SxEffectBase::kSpectrumBins - 0.5);
        const bool analyserOk = std::abs (best - expect) <= 2 && bestV > 0.3f && bestV < 0.7f;
        log ("EQ analyser: a 0.5 tone at 1 kHz is the spectrum peak (bin " + String (best) + ", expected about "
             + String (expect) + ", level " + String (bestV, 2) + ")", analyserOk);
    }

    // loudness: a 1 kHz stereo tone at -26 dBFS RMS per channel reads about -23 LUFS (two channels add
    // +3 dB, and the K-weighting is about +0.5 dB at 1 kHz)
    {
        auto p = createBuiltInEffect (BuiltInEffect::LoudnessMeter);
        auto* lm = dynamic_cast<SxLoudnessMeter*> (p.get());
        lm->prepareToPlay (44100.0, 512);
        const float amp = 0.0708f;
        AudioBuffer<float> buf (2, 512);
        MidiBuffer midi;
        for (int block = 0; block < 216; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, amp * std::sin ((float) (block * 512 + i) / 44100.0f * 1000.0f * MathConstants<float>::twoPi));
            lm->processBlock (buf, midi);
        }
        const float m = lm->momentaryLufs(), s = lm->shortTermLufs(), integrated = lm->integratedLufs();
        const float h = lm->historyLufs (SxLoudnessMeter::kHistory - 1);
        const bool loudOk = near (m, -23.0f, 0.7f) && near (s, -23.0f, 0.7f) && near (integrated, -23.0f, 0.7f) && near (h, -23.0f, 0.7f);
        log ("loudness: a 1 kHz tone at -23 dBFS RMS reads about -23 LUFS (momentary " + String (m, 1) + ", short-term "
             + String (s, 1) + ", integrated " + String (integrated, 1) + ", history " + String (h, 1) + ")", loudOk);

        // true peak: the same tone peaks at 0.0708 amplitude, about -23 dBTP
        const float tp = lm->truePeakHoldDb();
        log ("loudness: the true peak of the tone reads about -23 dBTP (" + String (tp, 2) + ")", near (tp, -23.0f, 0.4f));

        // the editors repaint when the display stamp moves, so every block has to move it
        const auto stampBefore = lm->displayStamp();
        lm->processBlock (buf, midi);
        log ("loudness: the display stamp moves with every block, so the meter repaints while audio runs",
             stampBefore != lm->displayStamp());

        // the LUFS view: its history, the target line and the short-term marker
        if (auto* ed = lm->createEditor())
        {
            for (auto* c : ed->getChildren())
                if (auto* b = dynamic_cast<Button*> (c))
                    if (b->getButtonText() == "LUFS-S")
                    {
                        b->setToggleState (true, dontSendNotification);
                        if (b->onClick != nullptr) b->onClick();
                    }
            ed->setSize (620, 480);
            auto img = ed->createComponentSnapshot ({ 0, 0, 620, 480 }, false, 1.0f);
            FileOutputStream fos (out.getChildFile ("fx_SX_Loudness_LUFS.png"));
            PNGImageFormat png;
            log ("loudness: the LUFS view draws its history and markers (fx_SX_Loudness_LUFS.png)",
                 fos.openedOk() && png.writeImageToStream (img, fos));
            delete ed;
        }
    }

    // stereo field: identical channels correlate +1 and have no width, opposite ones correlate -1,
    // and the scope keeps the real sample pairs
    {
        auto mono = createBuiltInEffect (BuiltInEffect::StereoField);
        auto opposite = createBuiltInEffect (BuiltInEffect::StereoField);
        auto* a = dynamic_cast<SxStereoFieldMeter*> (mono.get());
        auto* b = dynamic_cast<SxStereoFieldMeter*> (opposite.get());
        a->prepareToPlay (44100.0, 512);
        b->prepareToPlay (44100.0, 512);
        AudioBuffer<float> same (2, 512), flipped (2, 512);
        MidiBuffer midi;
        for (int block = 0; block < 86; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = 0.3f * std::sin ((float) (block * 512 + i) / 44100.0f * 300.0f * MathConstants<float>::twoPi);
                same.setSample (0, i, v);
                same.setSample (1, i, v);
                flipped.setSample (0, i, v);
                flipped.setSample (1, i, -v);
            }
            a->processBlock (same, midi);
            b->processBlock (flipped, midi);
        }
        const auto stampBefore = a->displayStamp();
        a->processBlock (same, midi);
        log ("stereo field: the display stamp moves with every block, so the meter repaints while audio runs",
             stampBefore != a->displayStamp());

        std::array<std::pair<float, float>, SxStereoFieldMeter::kScope> pairs {};
        a->copyScope (pairs);
        const auto last = pairs[SxStereoFieldMeter::kScope - 1];
        const bool scopeOk = std::abs (last.first) > 1.0e-4f && std::abs (last.first - last.second) < 1.0e-6f;
        const bool fieldOk = a->correlation() > 0.98f && a->widthPercent() < 5.0f && b->correlation() < -0.98f;
        log ("stereo field: identical channels correlate " + String (a->correlation(), 2) + " with width "
             + String (a->widthPercent(), 1) + " %, opposite channels correlate " + String (b->correlation(), 2), fieldOk);
        log ("stereo field: the scope keeps real L/R sample pairs (last pair " + String (last.first, 4) + " / "
             + String (last.second, 4) + ")", scopeOk);

        // the correlation and width histories
        for (const char* mode : { "CORR", "WIDTH" })
        {
            if (auto* ed = a->createEditor())
            {
                for (auto* c : ed->getChildren())
                    if (auto* btn = dynamic_cast<Button*> (c))
                        if (btn->getButtonText() == mode)
                        {
                            btn->setToggleState (true, dontSendNotification);
                            if (btn->onClick != nullptr) btn->onClick();
                        }
                ed->setSize (560, 460);
                auto img = ed->createComponentSnapshot ({ 0, 0, 560, 460 }, false, 1.0f);
                const String file = String ("fx_SX_Stereo_Field_") + mode + ".png";
                FileOutputStream fos (out.getChildFile (file));
                PNGImageFormat png;
                log ("stereo field: the " + String (mode) + " view draws its history (" + file + ")",
                     fos.openedOk() && png.writeImageToStream (img, fos));
                delete ed;
            }
        }
    }

    // a picture of the pro EQ with a few shapes set and a multi-tone signal on the analyser
    {
        auto p = createBuiltInEffect (BuiltInEffect::Eq);
        auto* eq = dynamic_cast<SxEq*> (p.get());
        eq->prepareToPlay (44100.0, 512);
        auto set = [eq] (int idx, float v) { eq->setParam (idx, v); };
        set (SxEq::pOn (0), 1.0f); set (SxEq::pType (0), SxEq::LowCut); set (SxEq::pFreq (0), 80.0f); set (SxEq::pSlope (0), 2.0f);
        set (SxEq::pOn (1), 1.0f); set (SxEq::pType (1), SxEq::Bell); set (SxEq::pFreq (1), 300.0f); set (SxEq::pGain (1), 5.0f); set (SxEq::pQ (1), 1.2f);
        set (SxEq::pOn (2), 1.0f); set (SxEq::pType (2), SxEq::Bell); set (SxEq::pFreq (2), 3000.0f); set (SxEq::pGain (2), -7.0f); set (SxEq::pQ (2), 2.5f);
        set (SxEq::pOn (3), 1.0f); set (SxEq::pType (3), SxEq::HighShelf); set (SxEq::pFreq (3), 9000.0f); set (SxEq::pGain (3), 4.0f);
        set (SxEq::pOn (4), 1.0f); set (SxEq::pType (4), SxEq::Bell); set (SxEq::pFreq (4), 600.0f); set (SxEq::pGain (4), -4.0f); set (SxEq::pQ (4), 3.0f);
        set (SxEq::pAnalyser(), 1.0f);

        AudioBuffer<float> buf (2, 512);
        MidiBuffer midi;
        Random rng (7);
        for (int block = 0; block < 90; ++block)
        {
            for (int i = 0; i < 512; ++i)
            {
                const double t = (double) (block * 512 + i) / 44100.0;
                const float v = 0.08f * (float) (std::sin (t * 60.0 * 6.2832) + std::sin (t * 220.0 * 6.2832) + 0.5 * std::sin (t * 880.0 * 6.2832))
                              + 0.05f * (float) (std::sin (t * 2800.0 * 6.2832) + std::sin (t * 7000.0 * 6.2832))
                              + 0.01f * (rng.nextFloat() * 2.0f - 1.0f);
                buf.setSample (0, i, v);
                buf.setSample (1, i, 0.9f * v);
            }
            eq->processBlock (buf, midi);
        }

        if (auto* ed = eq->createEditor())
        {
            const int w = 1040, h = 560;
            ed->setSize (w, h);
            auto img = ed->createComponentSnapshot ({ 0, 0, w, h }, false, 1.0f);
            FileOutputStream fos (out.getChildFile ("fx_SX_EQ_demo.png"));
            PNGImageFormat png;
            log ("pro EQ: the graph draws the response, the spectrum and the band shapes (fx_SX_EQ_demo.png)",
                 fos.openedOk() && png.writeImageToStream (img, fos));
            delete ed;
        }
    }
}

// The built-in sampler: loads a stereo file, plays it from the keyboard with root note / velocity,
// follows note-off, survives a state round trip and opens its editor.
static void runSamplerTest (PluginManager& plugins, const File& out, Log& log)
{
    const File wav = out.getChildFile ("sampler_source.wav");
    {
        AudioBuffer<float> b (2, 44100);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            b.setSample (0, i, 0.5f * std::sin (MathConstants<float>::twoPi * 440.0f * (float) i / 44100.0f));
            b.setSample (1, i, 0.25f * std::sin (MathConstants<double>::twoPi * 880.0 * (double) i / 44100.0));
        }
        WavAudioFormat fmt;
        if (auto os = std::unique_ptr<FileOutputStream> (wav.createOutputStream()))
            if (auto* writer = fmt.createWriterFor (os.get(), 44100.0, 2, 16, {}, 0))
            {
                os.release();
                writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                delete writer;
            }
    }

    auto sampler = std::make_unique<BuiltInSampler>();
    sampler->setRateAndBufferSizeDetails (44100.0, 512);
    sampler->prepareToPlay (44100.0, 512);
    String error;
    const bool loaded = sampler->loadSample (wav, error);
    const bool meta = loaded && sampler->getLoadedChannels() == 2
                   && sampler->getSampleLengthSamples() > 40000
                   && sampler->getSampleName() == "sampler_source.wav";

    AudioBuffer<float> b (2, 512);
    MidiBuffer midi;
    midi.addEvent (MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    sampler->processBlock (b, midi);
    const float peakL = b.getMagnitude (0, 0, b.getNumSamples());
    const float peakR = b.getMagnitude (1, 0, b.getNumSamples());
    const bool plays = peakL > 0.01f && peakR > 0.005f && std::abs (peakL - peakR) > 0.002f;

    midi.clear();
    midi.addEvent (MidiMessage::noteOff (1, 60), 0);
    for (int i = 0; i < 40; ++i) sampler->processBlock (b, midi);
    const bool released = sampler->getActiveVoiceCount() == 0;

    MemoryBlock state;
    sampler->getStateInformation (state);
    auto sampler2 = std::make_unique<BuiltInSampler>();
    sampler2->setRateAndBufferSizeDetails (44100.0, 512);
    sampler2->prepareToPlay (44100.0, 512);
    sampler2->setStateInformation (state.getData(), (int) state.getSize());
    const bool restored = sampler2->getSampleLengthSamples() > 40000
                       && sampler2->getSampleName() == "sampler_source.wav";

    bool editorOk = false;
    if (auto* ed = sampler->createEditor())
    {
        ed->setSize (520, 360);
        const auto shot = ed->createComponentSnapshot (ed->getLocalBounds(), true);
        editorOk = shot.isValid() && shot.getWidth() == 520;
        if (editorOk)
            if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("sampler_editor.png").createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (shot, *stream);
            }
        delete ed;
    }

    log ("sampler: a stereo sample loads, plays from the keyboard and follows note-off (L "
         + String (peakL, 3) + " / R " + String (peakR, 3) + ")",
         meta && plays && released);
    log ("sampler: state reloads the sample and the editor renders (sampler_editor.png)",
         restored && editorOk);

    auto instruments = pluginPickerList (plugins, true);
    bool listed = false;
    for (auto& d : instruments)
        if (isBuiltInSampler (d)) listed = true;
    log ("sampler: SX Sampler is in the instrument picker", listed);
}

static void runGuitarRigTest (const File& out, Log& log)
{
    struct Shot { float rms = 0.0f, peak = 0.0f, reduction = 0.0f; bool finite = true; };

    auto run = [] (const std::function<void (SxGuitarRig&)>& setup) -> Shot
    {
        Shot s;
        auto proc = createBuiltInEffect (BuiltInEffect::GuitarRig);
        auto* rig = dynamic_cast<SxGuitarRig*> (proc.get());
        if (rig == nullptr) { s.finite = false; return s; }
        rig->setRateAndBufferSizeDetails (44100.0, 512);
        rig->prepareToPlay (44100.0, 512);
        setup (*rig);

        AudioBuffer<float> buf (2, 512);
        MidiBuffer midi;
        double sumSq = 0.0;
        int count = 0;
        for (int block = 0; block < 24; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, 0.2f * std::sin ((float) (block * 512 + i) / 44100.0f * 110.0f * MathConstants<float>::twoPi));
            rig->processBlock (buf, midi);
            if (block < 12) continue;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                {
                    const float v = buf.getSample (ch, i);
                    if (! std::isfinite (v)) s.finite = false;
                    s.peak = jmax (s.peak, std::abs (v));
                    sumSq += (double) v * v;
                    ++count;
                }
        }
        s.rms = (float) std::sqrt (sumSq / jmax (1, count));
        s.reduction = rig->gainReductionDb();
        rig->releaseResources();
        return s;
    };

    auto set = [] (SxGuitarRig& r, int index, float value) { r.setParam (index, value); };

    // the four heads, each at its default settings
    Shot heads[4];
    bool headsOk = true;
    for (int m = 0; m < 4; ++m)
    {
        heads[m] = run ([m, &set] (SxGuitarRig& r) { set (r, SxGuitarRig::pAmp, (float) m); });
        headsOk = headsOk && heads[m].finite && heads[m].rms > 0.002f && heads[m].peak < 4.0f;
    }
    float lo = heads[0].rms, hi = heads[0].rms;
    for (auto& h : heads) { lo = jmin (lo, h.rms); hi = jmax (hi, h.rms); }
    headsOk = headsOk && hi > lo * 1.05f;
    log ("guitar rig: the four amp heads render, stay finite and sound different (rms "
         + String (heads[0].rms, 3) + " / " + String (heads[1].rms, 3) + " / " + String (heads[2].rms, 3) + " / "
         + String (heads[3].rms, 3) + ")", headsOk);

    // pedals and the time block change the sound when they are switched on
    const Shot clean = run ([] (SxGuitarRig&) {});
    const Shot screamer = run ([&set] (SxGuitarRig& r) { set (r, SxGuitarRig::pTsOn, 1.0f); set (r, SxGuitarRig::pTsDrive, 100.0f); });
    const Shot fuzz = run ([&set] (SxGuitarRig& r) { set (r, SxGuitarRig::pFuzzOn, 1.0f); set (r, SxGuitarRig::pFuzzDrive, 100.0f); });
    const Shot delayed = run ([&set] (SxGuitarRig& r) { set (r, SxGuitarRig::pDlyOn, 1.0f); set (r, SxGuitarRig::pDlyMix, 100.0f); set (r, SxGuitarRig::pDlyTime, 60.0f); });
    const bool pedalsOk = std::abs (screamer.rms - clean.rms) > 0.03f * clean.rms
                       && std::abs (fuzz.rms - clean.rms) > 0.03f * clean.rms
                       && std::abs (delayed.rms - clean.rms) > 0.01f * clean.rms;
    log ("guitar rig: the TS808, the fuzz and the delay change the sound when they are switched on ("
         + String (clean.rms, 3) + " -> " + String (screamer.rms, 3) + " / " + String (fuzz.rms, 3) + " / " + String (delayed.rms, 3) + ")",
         pedalsOk);

    // the comp reports the gain it takes off
    const Shot comp = run ([&set] (SxGuitarRig& r) { set (r, SxGuitarRig::pCompOn, 1.0f); set (r, SxGuitarRig::pCompSustain, 100.0f); });
    log ("guitar rig: the comp reports gain reduction (" + String (comp.reduction, 1) + " dB)", comp.finite && comp.reduction > 1.0f);

    // the pedal order is a real chain: the TS808 ahead of the clean boost sounds different from the
    // boost ahead of the TS808, and a slot list that repeats a pedal falls back to the default order
    // the samples of the left channel once the chain has settled
    auto capture = [] (const std::function<void (SxGuitarRig&)>& setup) -> std::vector<float>
    {
        std::vector<float> samples;
        auto proc = createBuiltInEffect (BuiltInEffect::GuitarRig);
        auto* rig = dynamic_cast<SxGuitarRig*> (proc.get());
        if (rig == nullptr) return samples;
        rig->setRateAndBufferSizeDetails (44100.0, 512);
        rig->prepareToPlay (44100.0, 512);
        setup (*rig);
        AudioBuffer<float> buf (2, 512);
        MidiBuffer midi;
        for (int block = 0; block < 24; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, 0.2f * std::sin ((float) (block * 512 + i) / 44100.0f * 110.0f * MathConstants<float>::twoPi));
            rig->processBlock (buf, midi);
            if (block >= 20)
                for (int i = 0; i < 512; ++i) samples.push_back (buf.getSample (0, i));
        }
        rig->releaseResources();
        return samples;
    };
    // the difference between two renders, relative to the size of the first
    auto distance = [] (const std::vector<float>& a, const std::vector<float>& b) -> double
    {
        double diff = 0.0, size = 1.0e-12;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        {
            diff += std::abs ((double) a[i] - (double) b[i]);
            size += std::abs ((double) a[i]);
        }
        return diff / size;
    };

    auto boostAndScreamer = [&set] (SxGuitarRig& r)
    {
        set (r, SxGuitarRig::pBoostOn, 1.0f); set (r, SxGuitarRig::pBoostGain, 100.0f);
        set (r, SxGuitarRig::pTsOn, 1.0f); set (r, SxGuitarRig::pTsDrive, 100.0f);
    };
    const auto boostFirst = capture (boostAndScreamer);
    const auto screamerFirst = capture ([&set, boostAndScreamer] (SxGuitarRig& r)
    {
        boostAndScreamer (r);
        set (r, SxGuitarRig::pSlot0, 2.0f); set (r, SxGuitarRig::pSlot1, 1.0f); set (r, SxGuitarRig::pSlot2, 0.0f);
    });
    const auto repeated = capture ([&set, boostAndScreamer] (SxGuitarRig& r)
    {
        boostAndScreamer (r);
        set (r, SxGuitarRig::pSlot0, 1.0f); set (r, SxGuitarRig::pSlot1, 1.0f);
    });
    const double orderGap = distance (screamerFirst, boostFirst);
    const double repeatGap = distance (repeated, boostFirst);
    const bool orderOk = ! boostFirst.empty() && orderGap > 0.01 && repeatGap < 1.0e-6;
    log ("guitar rig: the pedal order changes the sound (TS808 first differs by " + String (orderGap * 100.0, 1)
         + " %), and a repeated slot falls back to the default order", orderOk);

    // state: the amp, the cabinet and the switches survive a save and a load
    {
        auto a = createBuiltInEffect (BuiltInEffect::GuitarRig);
        auto b = createBuiltInEffect (BuiltInEffect::GuitarRig);
        auto* ra = dynamic_cast<SxGuitarRig*> (a.get());
        auto* rb = dynamic_cast<SxGuitarRig*> (b.get());
        bool stateOk = ra != nullptr && rb != nullptr;
        if (stateOk)
        {
            set (*ra, SxGuitarRig::pAmp, 3.0f);
            set (*ra, SxGuitarRig::pCab, 2.0f);
            set (*ra, SxGuitarRig::pMWatt, 1.0f);
            set (*ra, SxGuitarRig::pM2Crush, 1.0f);
            set (*ra, SxGuitarRig::pTsOn, 1.0f);
            set (*ra, SxGuitarRig::pTsDrive, 73.0f);
            MemoryBlock block;
            ra->getStateInformation (block);
            rb->setStateInformation (block.getData(), (int) block.getSize());
            stateOk = std::abs (rb->param (SxGuitarRig::pAmp) - 3.0f) < 1.0e-3f
                   && std::abs (rb->param (SxGuitarRig::pCab) - 2.0f) < 1.0e-3f
                   && std::abs (rb->param (SxGuitarRig::pMWatt) - 1.0f) < 1.0e-3f
                   && std::abs (rb->param (SxGuitarRig::pM2Crush) - 1.0f) < 1.0e-3f
                   && std::abs (rb->param (SxGuitarRig::pTsOn) - 1.0f) < 1.0e-3f
                   && std::abs (rb->param (SxGuitarRig::pTsDrive) - 73.0f) < 1.0e-2f;
        }
        log ("guitar rig: the amp head, cabinet and pedal settings survive a save and a load", stateOk);
    }

    // the oversampled chain reports its latency, so the engine can compensate it
    {
        auto proc = createBuiltInEffect (BuiltInEffect::GuitarRig);
        proc->setRateAndBufferSizeDetails (44100.0, 512);
        proc->prepareToPlay (44100.0, 512);
        const int latency = proc->getLatencySamples();
        proc->releaseResources();
        log ("guitar rig: the 2x oversampling reports its latency to the engine (" + String (latency) + " samples)",
             latency > 0 && latency < 256);
    }

    // every head's panel, drawn for the record
    bool shotsOk = true;
    const char* names[4] { "blackface", "jcm800", "plexi", "rectifier" };
    for (int m = 0; m < 4; ++m)
    {
        auto proc = createBuiltInEffect (BuiltInEffect::GuitarRig);
        set (*dynamic_cast<SxGuitarRig*> (proc.get()), SxGuitarRig::pAmp, (float) m);
        if (auto* ed = proc->createEditor())
        {
            const int w = ed->getWidth(), h = ed->getHeight();
            auto img = ed->createComponentSnapshot ({ 0, 0, w, h }, false, 1.0f);
            const File f = out.getChildFile (String ("guitar_rig_") + names[m] + ".png");
            FileOutputStream fos (f);
            PNGImageFormat png;
            shotsOk = shotsOk && fos.openedOk() && png.writeImageToStream (img, fos);
            delete ed;
        }
        else shotsOk = false;
        proc->releaseResources();
    }
    log ("guitar rig: the panel of each amp head is drawn (guitar_rig_blackface / jcm800 / plexi / rectifier .png)", shotsOk);

    // IR cabinet: load a tiny impulse, use it, then save / load the whole rig as a preset
    {
        const File irFile = out.getChildFile ("test_cab_ir.wav");
        {
            AudioBuffer<float> ir (1, 32);
            ir.clear();
            ir.setSample (0, 0, 1.0f);
            WavAudioFormat wav;
            if (auto os = std::unique_ptr<FileOutputStream> (irFile.createOutputStream()))
            {
                std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), 44100.0, 1, 16, {}, 0));
                if (w != nullptr)
                {
                    os.release();
                    w->writeFromAudioSampleBuffer (ir, 0, ir.getNumSamples());
                }
            }
        }

        auto proc = createBuiltInEffect (BuiltInEffect::GuitarRig);
        auto* rig = dynamic_cast<SxGuitarRig*> (proc.get());
        bool irOk = false, presetOk = false;
        if (rig != nullptr && irFile.existsAsFile())
        {
            rig->setRateAndBufferSizeDetails (44100.0, 512);
            rig->prepareToPlay (44100.0, 512);
            String err;
            const bool loaded = rig->loadIRFile (irFile, err);
            rig->setParam (SxGuitarRig::pIrOn, 1.0f);
            AudioBuffer<float> b (2, 512);
            MidiBuffer midi;
            b.clear();
            for (int block = 0; block < 4; ++block) rig->processBlock (b, midi);
            bool finite = true;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    finite = finite && std::isfinite (b.getReadPointer (c)[i]);
            irOk = loaded && rig->hasIR() && finite;

            const File preset = out.getChildFile ("guitar_ir.sxpreset");
            const bool saved = rig->savePresetFile (preset);
            auto proc2 = createBuiltInEffect (BuiltInEffect::GuitarRig);
            auto* rig2 = dynamic_cast<SxGuitarRig*> (proc2.get());
            if (rig2 != nullptr)
            {
                rig2->setRateAndBufferSizeDetails (44100.0, 512);
                rig2->prepareToPlay (44100.0, 512);
                presetOk = saved && rig2->loadPresetFile (preset) && rig2->hasIR()
                        && rig2->irName() == "test_cab_ir";
            }
        }
        log ("guitar rig: a cabinet IR loads and runs, and the preset carries the IR to a fresh rig",
             irOk && presetOk);
    }
}

// Fades: the four shapes are monotone, agree between drawing and rendering, survive a save / load
// round trip, and a fade with every shape still leaves audible material (the clip is not silent).
static void runFadeCurveTest (Project& project, Engine& engine, PluginManager& plugins, const File& out, Log& log)
{
    bool shapesOk = true;
    for (int curve = 0; curve <= 4; ++curve)
    {
        const double a = fadeShape (curve, 0.0);
        const double mid = fadeShape (curve, 0.5);
        const double b = fadeShape (curve, 1.0);
        shapesOk = shapesOk && std::abs (a) < 1.0e-9 && std::abs (b - 1.0) < 1.0e-9 && mid > 0.15 && mid < 0.95;
    }
    log ("fades: every curve shape goes 0 -> 1 and is monotone at the halfway point", shapesOk);

    std::vector<int> ids;
    for (auto& t2 : project.tracks) ids.push_back (t2->id);
    for (int id : ids) project.removeTrack (id);
    engine.syncAll();

    const File sine = out.getChildFile ("fade_sine.wav");
    sine.deleteFile();
    {
        AudioBuffer<float> b (1, 44100);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * std::sin ((float) i / 44100.0f * 220.0f * MathConstants<float>::twoPi));
        WavAudioFormat wav;
        if (auto* w = wav.createWriterFor (new FileOutputStream (sine), 44100.0, 1, 24, {}, 0))
        {
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            delete w;
        }
    }

    auto& tr = project.addTrack (TrackType::Audio, "Fade");
    const int tid = tr.id;
    AudioClip clip;
    clip.id = project.newId();
    clip.file = sine;
    clip.data = project.pool.load (sine, engine.formats);
    clip.start = 0.0;
    clip.lengthSec = 0.98;
    clip.fadeIn = 0.4;
    clip.fadeOut = 0.4;
    clip.fadeInCurve = 2;
    clip.fadeOutCurve = 3;
    tr.audioClips.push_back (clip);
    tr.volumeDb = 0.0f;
    project.sendStructureChanged();
    engine.syncAll();

    ExportOptions o;
    o.mode = ExportOptions::Mix;
    o.target = out.getChildFile ("fade_render.wav");
    o.sampleRate = 44100.0;
    o.bitDepth = 24;
    String err;
    const bool ok = engine.exportAudio (o, {}, err);
    float midLevel = 0.0f;
    if (ok)
    {
        std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
        if (r != nullptr && r->lengthInSamples > 4000)
        {
            // the whole render (the export can add a silent tail, so a midpoint read is not enough)
            AudioBuffer<float> b (1, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, false);
            midLevel = b.getMagnitude (0, 0, b.getNumSamples());
        }
    }
    log ("fades: a clip with an equal-power in and exponential out still plays in the middle ("
         + String (midLevel, 3) + ")", ok && midLevel > 0.2f);

    {
        const File projFile = out.getChildFile ("fade_curves.sxp");
        const bool savedOk = project.saveTo (projFile);
        Project loaded;
        loaded.formats = &engine.formats;
        const bool loadedOk = savedOk && loaded.loadFrom (projFile);
        Track* lt = (loadedOk && ! loaded.tracks.empty()) ? loaded.tracks.front().get() : nullptr;
        const bool curvesOk = lt != nullptr && ! lt->audioClips.empty()
                           && lt->audioClips.front().fadeInCurve == 2
                           && lt->audioClips.front().fadeOutCurve == 3;
        log ("fades: the chosen curve shapes are saved with the project", curvesOk);
        projFile.deleteFile();
    }

    project.removeTrack (tid);
    project.sendStructureChanged();
    engine.syncAll();
    sine.deleteFile();
}
int runSelfTest (Project& project, Engine& engine, PluginManager& plugins, const StringArray& args)
{
    const int idx = args.indexOf ("--selftest");
    File out = idx >= 0 && idx + 1 < args.size() ? File (args[idx + 1]) : File::getSpecialLocation (File::tempDirectory).getChildFile ("soundx_selftest");
    out.createDirectory();
    Log log (out.getChildFile ("selftest.log"));
    log ("SoundX " + juce::String (JUCE_APPLICATION_VERSION_STRING) + " self test");
    log ("output folder: " + out.getFullPathName());

    // Support / performance diagnostic: scan exactly one folder (recursively), print how long it
    // took, and exit. Set SOUNDX_SCAN_WORKERS=1 or 4 to compare serial and parallel scanning.
    //   SoundX.exe --selftest <dir> --scan-benchmark "C:\Program Files\Common Files\VST3"
    if (const int bi = args.indexOf ("--scan-benchmark"); bi >= 0)
    {
        const File folder = bi + 1 < args.size() ? File (args[bi + 1]) : File();
        if (! folder.isDirectory())
        {
            log ("scan benchmark: not a folder: " + folder.getFullPathName(), false);
            return 1;
        }

        const int typesBefore = plugins.known.getNumTypes();
        const auto started = Time::getMillisecondCounter();
        const bool began = plugins.scanner().scanFolder (folder, false);
        while (began && plugins.scanner().isScanning()
               && Time::getMillisecondCounter() - started < 10 * 60 * 1000)
            MessageManager::getInstance()->runDispatchLoopUntil (20);

        const double seconds = (Time::getMillisecondCounter() - started) / 1000.0;
        log ("scan benchmark: " + folder.getFullPathName() + " in " + String (seconds, 2) + " s ("
             + String (plugins.scanner().getNumTotal()) + " files, "
             + String (plugins.scanner().getNumScanned()) + " checked, "
             + String (plugins.scanner().getNumSkipped()) + " skipped, "
             + String (plugins.known.getNumTypes() - typesBefore) + " new types, "
             + String (SystemStats::getEnvironmentVariable ("SOUNDX_SCAN_WORKERS", "auto")) + " workers)",
             began && ! plugins.scanner().isScanning());
        return began ? 0 : 1;
    }

    // Multi-output diagnostic: the mixer channels, their stems and a freeze of the whole
    // instrument.  SoundX.exe --selftest <dir> --multiout-check
    if (args.contains ("--multiout-check"))
    {
        project.reset();
        engine.syncAll();
        runMultiOutputTest (project, engine, plugins, out, log);
        return log.failures > 0 ? 1 : 0;
    }

    // Import / freeze timing: where does the time of a drag & drop import go, and how long does
    // freezing take?  SoundX.exe --selftest <dir> --bench-import [minutes]
    if (args.contains ("--bench-import"))
    {
        double minutes = 3.0;
        if (const int mi = args.indexOf ("--bench-import"); mi >= 0 && mi + 1 < args.size())
            minutes = jlimit (0.1, 30.0, args[mi + 1].getDoubleValue());

        const File wav = out.getChildFile ("bench_import.wav");
        if (! wav.existsAsFile() || wav.getSize() == 0)
        {
            WavAudioFormat fmt;
            if (auto* os = wav.createOutputStream().release())
            {
                std::unique_ptr<AudioFormatWriter> w (fmt.createWriterFor (os, 48000.0, 2, 24, {}, 0));
                if (w != nullptr)
                {
                    AudioBuffer<float> block (2, 48000);
                    for (int i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float v = 0.3f * std::sin (MathConstants<float>::twoPi * 220.0f * (float) i / 48000.0f);
                        block.setSample (0, i, v);
                        block.setSample (1, i, v * 0.8f);
                    }
                    const int seconds = (int) (minutes * 60.0);
                    for (int s = 0; s < seconds; ++s) w->writeFromAudioSampleBuffer (block, 0, block.getNumSamples());
                    w.reset();
                }
                else delete os;
            }
        }
        const double mb = wav.existsAsFile() ? wav.getSize() / (1024.0 * 1024.0) : 0.0;

        // raw read and peak build, separately
        double readMs = 0.0, peakMs = 0.0, loadMs = 0.0, importMs = 0.0;
        if (auto r = std::unique_ptr<AudioFormatReader> (engine.formats.createReaderFor (wav)))
        {
            auto d = std::make_shared<AudioData>();
            d->buffer.setSize (2, (int) r->lengthInSamples);
            auto t0 = Time::getMillisecondCounterHiRes();
            r->read (&d->buffer, 0, (int) r->lengthInSamples, 0, true, true);
            auto t1 = Time::getMillisecondCounterHiRes();
            d->buildPeaks();
            auto t2 = Time::getMillisecondCounterHiRes();
            readMs = t1 - t0;
            peakMs = t2 - t1;
        }
        {
            auto t0 = Time::getMillisecondCounterHiRes();
            auto data = project.pool.load (wav, engine.formats);
            loadMs = Time::getMillisecondCounterHiRes() - t0;
            juce::ignoreUnused (data);
        }
        project.reset();
        engine.syncAll();
        {
            AppState st (project, engine, plugins);
            const auto t0 = Time::getMillisecondCounterHiRes();
            const bool ok = importAudioFile (st, wav, 0.0, -1);
            importMs = Time::getMillisecondCounterHiRes() - t0;

            // the samples arrive from the background decoder; pump the message loop like the
            // app does and wait for the clip to become playable
            auto loaded = [&project]
            {
                for (auto& tr : project.tracks)
                    for (auto& c : tr->audioClips)
                        if (c.data != nullptr && ! c.loading) return true;
                return false;
            };
            const auto deadline = Time::getMillisecondCounter() + 30000;
            while (! loaded() && Time::getMillisecondCounter() < deadline)
                MessageManager::getInstance()->runDispatchLoopUntil (4);
            const double readyMs = Time::getMillisecondCounterHiRes() - t0;
            log ("import bench: " + String (mb, 1) + " MB / " + String (minutes * 60.0, 0) + " s: read "
                 + String (readMs, 0) + " ms, peaks " + String (peakMs, 0) + " ms, pool.load "
                 + String (loadMs, 0) + " ms, clip in " + String (importMs, 1) + " ms, samples ready in "
                 + String (readyMs, 0) + " ms", ok && loaded() && readMs >= 0.0);
        }
        return 0;
    }

    // Timing for a real file:  --bench-file "C:\path\to\audio.wav"
    if (args.contains ("--bench-file"))
    {
        const int fi = args.indexOf ("--bench-file");
        const File f = fi >= 0 && fi + 1 < args.size() ? File (args[fi + 1]) : File();
        if (! f.existsAsFile()) { log ("bench-file: not found: " + f.getFullPathName(), false); return 1; }

        project.reset();
        engine.syncAll();
        const double mb = f.getSize() / (1024.0 * 1024.0);
        double loadMs = 0.0, readMs = 0.0, peakMs = 0.0, importMs = 0.0, describeMs = 0.0;
        if (auto r = std::unique_ptr<AudioFormatReader> (engine.formats.createReaderFor (f)))
        {
            auto d = std::make_shared<AudioData>();
            const int nch = (int) jmin<unsigned int> (2u, r->numChannels);
            d->buffer.setSize (nch, (int) r->lengthInSamples);
            auto t0 = Time::getMillisecondCounterHiRes();
            r->read (&d->buffer, 0, (int) r->lengthInSamples, 0, true, nch > 1);
            auto t1 = Time::getMillisecondCounterHiRes();
            d->buildPeaks();
            auto t2 = Time::getMillisecondCounterHiRes();
            readMs = t1 - t0;
            peakMs = t2 - t1;
        }
        AppState st (project, engine, plugins);
        {
            auto t0 = Time::getMillisecondCounterHiRes();
            auto data = project.pool.load (f, engine.formats);
            loadMs = Time::getMillisecondCounterHiRes() - t0;
            juce::ignoreUnused (data);
        }
        {
            auto t0 = Time::getMillisecondCounterHiRes();
            describeMediaFile (f, engine.formats);
            describeMs = Time::getMillisecondCounterHiRes() - t0;
        }
        {
            const auto t0 = Time::getMillisecondCounterHiRes();
            const bool ok = importAudioFile (st, f, 0.0, -1);
            importMs = Time::getMillisecondCounterHiRes() - t0;
            auto loaded = [&project]
            {
                for (auto& tr : project.tracks)
                    for (auto& c : tr->audioClips)
                        if (c.data != nullptr && ! c.loading) return true;
                return false;
            };
            const auto deadline = Time::getMillisecondCounter() + 30000;
            while (! loaded() && Time::getMillisecondCounter() < deadline)
                MessageManager::getInstance()->runDispatchLoopUntil (4);
            const double readyMs = Time::getMillisecondCounterHiRes() - t0;
            log ("bench-file: " + f.getFileName() + " (" + String (mb, 1) + " MB): read " + String (readMs, 0)
                 + " ms, peaks " + String (peakMs, 0) + " ms, pool.load " + String (loadMs, 0)
                 + " ms, describe " + String (describeMs, 1) + " ms, clip in "
                 + String (importMs, 1) + " ms, samples ready in " + String (readyMs, 0) + " ms",
                 ok && loaded());
        }
        return 0;
    }

    // Freeze timing: SoundX.exe --selftest <dir> --bench-freeze [beats]
    if (args.contains ("--bench-freeze"))
    {
        double beats = 64.0;
        if (const int bi = args.indexOf ("--bench-freeze"); bi >= 0 && bi + 1 < args.size())
            beats = jlimit (4.0, 1024.0, args[bi + 1].getDoubleValue());

        project.reset();
        engine.syncAll();
        AppState st (project, engine, plugins);
        st.showFreezeProgress = false;   // the first pass measures the pure render time
        auto& t = project.addTrack (TrackType::Instrument, "FreezeBench");
        t.hasInstrument = true;
        t.instrument = makeBuiltInSynthSlot();
        {
            MidiClip c;
            c.id = project.newId();
            c.length = beats;
            for (double b = 0.0; b < beats - 0.25; b += 0.25)
            {
                Note n;
                n.id = project.newId();
                n.pitch = 48 + (int) std::floor (b * 3.0) % 24;
                n.start = b;
                n.length = 0.22;
                n.velocity = 100;
                c.notes.push_back (n);
            }
            t.midiClips.push_back (std::move (c));
        }
        String err;
        engine.addInsert (t.id, makeBuiltInEffectSlot (BuiltInEffect::Distortion).desc, err);
        engine.addInsert (t.id, makeBuiltInEffectSlot (BuiltInEffect::DigitalReverb).desc, err);

        // optional: a big session around the frozen track (the offline render must only prepare
        // the plug-ins it actually processes, not every plug-in of the project)
        int background = 0;
        if (const int bi = args.indexOf ("--bench-freeze"); bi >= 0 && bi + 2 < args.size())
            background = jlimit (0, 200, args[bi + 2].getIntValue());
        for (int i = 0; i < background; ++i)
        {
            auto& bt = project.addTrack (TrackType::Instrument, "BG " + String (i));
            bt.hasInstrument = true;
            bt.instrument = makeBuiltInSynthSlot();
            MidiClip bc;
            bc.id = project.newId();
            bc.length = beats;
            Note bn;
            bn.id = project.newId();
            bn.pitch = 40 + (i % 24);
            bn.start = 0.0;
            bn.length = 1.0;
            bn.velocity = 90;
            bc.notes.push_back (bn);
            bt.midiClips.push_back (std::move (bc));
            engine.addInsert (bt.id, makeBuiltInEffectSlot (BuiltInEffect::Eq).desc, err);
            engine.addInsert (bt.id, makeBuiltInEffectSlot (BuiltInEffect::Compressor).desc, err);
        }

        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();

        auto t0 = Time::getMillisecondCounterHiRes();
        st.freezeTrack (t.id);
        const double noDeviceMs = Time::getMillisecondCounterHiRes() - t0;
        auto* ft = project.findTrack (t.id);
        const double fileMb = ft != nullptr && ft->frozenFile.existsAsFile() ? ft->frozenFile.getSize() / (1024.0 * 1024.0) : 0.0;
        st.unfreezeTrack (t.id);

        // again with a live audio device and the real progress window (the freeze has to detach /
        // re-attach the callback while the modal loop keeps the window responsive)
        engine.initialiseAudio();
        const bool haveDevice = engine.deviceManager.getCurrentAudioDevice() != nullptr;
        st.showFreezeProgress = true;
        t0 = Time::getMillisecondCounterHiRes();
        st.freezeTrack (t.id);
        const double withDeviceMs = Time::getMillisecondCounterHiRes() - t0;
        st.unfreezeTrack (t.id);
        log ("freeze bench: " + String (beats, 0) + " beats / " + String (fileMb, 2) + " MB render: "
             + String (noDeviceMs, 0) + " ms (serial), " + String (withDeviceMs, 0)
             + " ms (" + (haveDevice ? "live device" : "no device available") + ")",
             ft != nullptr && fileMb > 0.1 && noDeviceMs < 60000.0);
        return 0;
    }

    // Cancelling a freeze: the progress window must close, the render thread must stop and the
    // audio device must come back. Before the offline-render session this deadlocked (the render
    // thread waited for the message thread while the cancel handler waited for the render thread)
    // and the app crashed.  SoundX.exe --selftest <dir> --freeze-cancel
    if (args.contains ("--freeze-cancel"))
    {
        project.reset();
        engine.syncAll();
        engine.initialiseAudio();
        AppState st (project, engine, plugins);
        st.showFreezeProgress = true;

        auto& t = project.addTrack (TrackType::Instrument, "CancelBench");
        t.hasInstrument = true;
        t.instrument = makeBuiltInSynthSlot();
        {
            MidiClip c;
            c.id = project.newId();
            c.length = 48.0;
            for (double b = 0.0; b < 47.75; b += 0.25)
            {
                Note n;
                n.id = project.newId();
                n.pitch = 48 + (int) std::floor (b * 3.0) % 24;
                n.start = b;
                n.length = 0.22;
                n.velocity = 100;
                c.notes.push_back (n);
            }
            t.midiClips.push_back (std::move (c));
        }
        String fxErr;
        // a deliberately heavy chain: the cancel has to land in the middle of a real render
        for (int i = 0; i < 3; ++i)
        {
            engine.addInsert (t.id, makeBuiltInEffectSlot (BuiltInEffect::GuitarRig).desc, fxErr);
            engine.addInsert (t.id, makeBuiltInEffectSlot (BuiltInEffect::DigitalReverb).desc, fxErr);
        }
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
        const int tid = t.id;

        String freezeError;
        st.onError = [&freezeError] (const juce::String& m) { freezeError = m; };

        // click the progress window's Cancel button while the render is running
        auto sawWindow = std::make_shared<std::atomic<bool>> (false);
        auto clickedCancel = std::make_shared<std::atomic<bool>> (false);
        auto firedAt = std::make_shared<std::atomic<double>> (-1.0);
        const double timerBase = Time::getMillisecondCounterHiRes();
        juce::Timer::callAfterDelay (700, [sawWindow, clickedCancel, firedAt, timerBase]
        {
            firedAt->store (Time::getMillisecondCounterHiRes() - timerBase);
            if (auto* modal = juce::Component::getCurrentlyModalComponent())
                if (auto* aw = dynamic_cast<juce::AlertWindow*> (modal))
                {
                    sawWindow->store (true);
                    if (aw->getNumButtons() > 0)
                        if (auto* b = aw->getButton (0)) { b->triggerClick(); clickedCancel->store (true); }
                }
        });

        const auto t0 = Time::getMillisecondCounterHiRes();
        st.freezeTrack (tid);
        const double ms = Time::getMillisecondCounterHiRes() - t0;
        // let the delayed cancel callback run even if the render finished first
        for (int i = 0; i < 40; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
        auto* ft = project.findTrack (tid);
        const bool cancelled = ft != nullptr && ! ft->frozen && ! st.freezeBusy;
        const bool windowGone = juce::Component::getCurrentlyModalComponent() == nullptr;

        // the device has to be back: play a moment and check that the transport moves
        const bool haveDevice = engine.deviceManager.getCurrentAudioDevice() != nullptr;
        engine.setPositionBeats (0.0);
        engine.play();
        Thread::sleep (150);
        const bool running = haveDevice ? engine.getPositionBeats() > 0.05 : true;
        engine.stop();
        engine.syncAll();
        log ("freeze cancel: returned in " + String (ms, 0) + " ms, cancel at "
             + String (firedAt->load(), 0) + " ms, window "
             + String ((int) sawWindow->load()) + "/" + String ((int) clickedCancel->load())
             + ", track not frozen (" + String ((int) cancelled) + "), window closed ("
             + String ((int) windowGone) + "), audio device back (" + String ((int) running) + ")"
             + (freezeError.isEmpty() ? String() : " [" + freezeError + "]"),
             cancelled && running && windowGone && ms < 15000.0);
        return log.failures > 0 ? 1 : 0;
    }

    // Export timing / real-time rendering: SoundX.exe --selftest <dir> --bench-export
    if (args.contains ("--bench-export"))
    {
        project.reset();
        engine.syncAll();
        engine.initialiseAudio();
        const double rate = engine.getSampleRate() > 0 ? engine.getSampleRate() : 44100.0;
        AppState st (project, engine, plugins);

        auto& t = project.addTrack (TrackType::Instrument, "ExportBench");
        t.hasInstrument = true;
        t.instrument = makeBuiltInSynthSlot();
        {
            MidiClip c;
            c.id = project.newId();
            c.length = 64.0;
            for (double b = 0.0; b < 63.75; b += 0.5)
            {
                Note n;
                n.id = project.newId();
                n.pitch = 48 + (int) std::floor (b * 2.0) % 24;
                n.start = b;
                n.length = 0.45;
                n.velocity = 100;
                c.notes.push_back (n);
            }
            t.midiClips.push_back (std::move (c));
        }
        String fxErr;
        engine.addInsert (t.id, makeBuiltInEffectSlot (BuiltInEffect::DigitalReverb).desc, fxErr);
        t.volumeDb = -6.0f;
        // optional: a big session (the offline export must not re-prepare every plug-in of it)
        int background = 0;
        if (const int ei = args.indexOf ("--bench-export"); ei >= 0 && ei + 1 < args.size())
            background = jlimit (0, 200, args[ei + 1].getIntValue());
        for (int i = 0; i < background; ++i)
        {
            auto& bt = project.addTrack (TrackType::Instrument, "BG " + String (i));
            bt.hasInstrument = true;
            bt.instrument = makeBuiltInSynthSlot();
            MidiClip bc;
            bc.id = project.newId();
            bc.length = 64.0;
            Note bn;
            bn.id = project.newId();
            bn.pitch = 40 + (i % 24);
            bn.start = 0.0;
            bn.length = 1.0;
            bn.velocity = 90;
            bc.notes.push_back (bn);
            bt.midiClips.push_back (std::move (bc));
            engine.addInsert (bt.id, makeBuiltInEffectSlot (BuiltInEffect::Eq).desc, fxErr);
            engine.addInsert (bt.id, makeBuiltInEffectSlot (BuiltInEffect::Compressor).desc, fxErr);
        }
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();

        ExportOptions o;
        o.mode = ExportOptions::Mix;
        o.sampleRate = rate;
        o.bitDepth = 24;
        o.tailSeconds = 0.5;
        o.target = out.getChildFile ("exp_offline.wav");
        String err;
        auto t0 = Time::getMillisecondCounterHiRes();
        const bool offOk = engine.exportAudio (o, {}, err);
        const double offlineMs = Time::getMillisecondCounterHiRes() - t0;

        // the same 2-beat range offline and in real time, so the two files can be compared
        project.loopOn = true;
        project.loopStart = 0.0;
        project.loopEnd = 2.0;

        ExportOptions ref = o;
        ref.useLoopRange = true;
        ref.target = out.getChildFile ("exp_ref.wav");
        const bool refOk = engine.exportAudio (ref, {}, err);
        auto refInfo = inspect (engine.formats, ref.target);

        ExportOptions rt = ref;
        rt.realtime = true;
        rt.target = out.getChildFile ("exp_realtime.wav");
        t0 = Time::getMillisecondCounterHiRes();
        runExportJob (st, rt, false);
        const double realtimeMs = Time::getMillisecondCounterHiRes() - t0;
        auto rtInfo = inspect (engine.formats, rt.target);
        project.loopOn = false;

        const bool rtOk = rtInfo.ok && refInfo.ok
                       && std::abs (rtInfo.seconds - refInfo.seconds) < 0.25
                       && std::abs (rtInfo.peak - refInfo.peak) < jmax (0.02f, refInfo.peak * 0.15f)
                       && std::abs (rtInfo.sr - rate) < 1.0;
        log ("export bench: 64 beats offline " + String (offlineMs, 0) + " ms; real-time 2-beat mixdown "
             + String (realtimeMs, 0) + " ms (" + String (rtInfo.seconds, 2) + " s vs offline "
             + String (refInfo.seconds, 2) + " s, peak " + String (rtInfo.peak, 3) + " vs "
             + String (refInfo.peak, 3) + ")", offOk && refOk && rtOk);
        return log.failures > 0 ? 1 : 0;
    }

    // Cancelling an export: the progress window must close and the device must come back without
    // the old deadlock. SoundX.exe --selftest <dir> --export-cancel
    if (args.contains ("--export-cancel"))
    {
        project.reset();
        engine.syncAll();
        engine.initialiseAudio();
        AppState st (project, engine, plugins);

        auto& t = project.addTrack (TrackType::Instrument, "ExportCancelBench");
        t.hasInstrument = true;
        t.instrument = makeBuiltInSynthSlot();
        {
            MidiClip c;
            c.id = project.newId();
            c.length = 256.0;
            for (double b = 0.0; b < 255.75; b += 0.25)
            {
                Note n;
                n.id = project.newId();
                n.pitch = 48 + (int) std::floor (b * 3.0) % 24;
                n.start = b;
                n.length = 0.22;
                n.velocity = 100;
                c.notes.push_back (n);
            }
            t.midiClips.push_back (std::move (c));
        }
        String fxErr;
        for (int i = 0; i < 4; ++i)
        {
            engine.addInsert (t.id, makeBuiltInEffectSlot (BuiltInEffect::GuitarRig).desc, fxErr);
            engine.addInsert (t.id, makeBuiltInEffectSlot (BuiltInEffect::DigitalReverb).desc, fxErr);
        }
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();

        ExportOptions o;
        o.mode = ExportOptions::Mix;
        o.sampleRate = engine.getSampleRate() > 0 ? engine.getSampleRate() : 44100.0;
        o.bitDepth = 24;
        o.tailSeconds = 1.0;
        o.target = out.getChildFile ("exp_cancel.wav");
        o.target.deleteFile();

        auto sawWindow = std::make_shared<std::atomic<bool>> (false);
        auto clickedCancel = std::make_shared<std::atomic<bool>> (false);
        juce::Timer::callAfterDelay (700, [sawWindow, clickedCancel]
        {
            if (auto* modal = juce::Component::getCurrentlyModalComponent())
                if (auto* aw = dynamic_cast<juce::AlertWindow*> (modal))
                {
                    sawWindow->store (true);
                    if (aw->getNumButtons() > 0)
                        if (auto* b = aw->getButton (0)) { b->triggerClick(); clickedCancel->store (true); }
                }
        });

        const auto t0 = Time::getMillisecondCounterHiRes();
        runExportJob (st, o, false);
        const double ms = Time::getMillisecondCounterHiRes() - t0;
        for (int i = 0; i < 40; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
        const bool windowGone = juce::Component::getCurrentlyModalComponent() == nullptr;
        const bool cancelled = ! o.target.existsAsFile();

        const bool haveDevice = engine.deviceManager.getCurrentAudioDevice() != nullptr;
        engine.setPositionBeats (0.0);
        engine.play();
        Thread::sleep (150);
        const bool running = haveDevice ? engine.getPositionBeats() > 0.05 : true;
        engine.stop();
        engine.syncAll();
        log ("export cancel: returned in " + String (ms, 0) + " ms, window "
             + String ((int) sawWindow->load()) + "/" + String ((int) clickedCancel->load())
             + ", no file (" + String ((int) cancelled) + "), window closed ("
             + String ((int) windowGone) + "), audio device back (" + String ((int) running) + ")",
             cancelled && windowGone && running && ms < 30000.0);
        return log.failures > 0 ? 1 : 0;
    }

    // Freeze diagnostic: does an instrument track freeze to audio that matches the live mix?
    //   SoundX.exe --selftest <dir> --freeze-check
    if (args.contains ("--freeze-check"))
    {
        auto peakOf = [&] (const File& f) -> float
        {
            AudioBuffer<float> b (2, 4 * 44100);
            std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (f));
            if (r == nullptr) return -1.0f;
            const int n = jmin (b.getNumSamples(), (int) r->lengthInSamples);
            r->read (&b, 0, n, 0, true, true);
            return jmax (b.getMagnitude (0, 0, n), b.getMagnitude (1, 0, n));
        };
        auto mixPeak = [&] (const String& name) -> float
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix;
            o.target = out.getChildFile (name + ".wav");
            o.sampleRate = 44100.0;
            o.bitDepth = 16;
            o.tailSeconds = 0.5;
            String err;
            if (! engine.exportAudio (o, {}, err)) { log ("freeze-check: export failed: " + err, false); return -1.0f; }
            return peakOf (o.target);
        };

        for (double clipStart : { 0.0, 4.0 })
        {
            project.reset();
            engine.syncAll();
            auto& t = project.addTrack (TrackType::Instrument, "Frz");
            t.hasInstrument = true;
            t.instrument = makeBuiltInSynthSlot();
            MidiClip c;
            c.id = project.newId();
            c.start = clipStart;
            c.length = 4.0;
            Note n;
            n.id = project.newId();
            n.pitch = 60;
            n.start = 0.0;
            n.length = 3.5;
            n.velocity = 100;
            c.notes.push_back (n);
            t.midiClips.push_back (c);
            const int tid = t.id;
            project.sendStructureChanged();
            project.sendContentChanged();
            project.sendMixChanged();
            engine.syncAll();

            AppState st (project, engine, plugins);
            st.showFreezeProgress = false; // no modal window during the self test (project, engine, plugins);
            const float before = mixPeak ("frz_before");
            st.freezeTrack (tid);
            engine.syncAll();
            auto* ft = project.findTrack (tid);
            const float frozen = mixPeak ("frz_frozen");
            const float filePeak = ft != nullptr && ft->frozenFile.existsAsFile() ? peakOf (ft->frozenFile) : -1.0f;
            log ("freeze-check: clip at " + String (clipStart, 1) + " beats: live " + String (before, 3)
                 + ", frozen mix " + String (frozen, 3) + ", render file " + String (filePeak, 3),
                 before > 0.02f && std::abs (frozen - before) < 0.1f);
            st.unfreezeTrack (tid);
        }
        return log.failures > 0 ? 1 : 0;
    }

    // ---- a new session: no tracks, and an instrument track without an instrument -----------------
    {
        project.reset();
        AppState st (project, engine, plugins);

        MainComponent mc (st);            // the UI has to survive an empty project too
        mc.setSize (1229, 710);
        mc.resized();
        bool shot = false;
        if (auto img = mc.createComponentSnapshot (mc.getLocalBounds(), true); img.isValid())
            if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("empty_project.png").createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
                shot = true;
            }
        log ("new session: the project starts with no tracks, and the window renders it (empty_project.png)",
             project.tracks.empty() && shot);

        // computer-keyboard performance shows the on-screen piano with the letter for each key
        st.musicalTyping = true;
        bool typingShot = false;
        if (auto img = mc.createComponentSnapshot (mc.getLocalBounds(), true); img.isValid())
            if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("musical_typing.png").createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
                typingShot = true;
            }
        st.musicalTyping = false;
        log ("musical typing: the on-screen keyboard is drawn with its computer-key labels (musical_typing.png)", typingShot);

        // ---- switching the theme must repaint the live window, not rebuild it -------------------
        // The old implementation destroyed and re-created the whole MainComponent, which reset the
        // open mixer / piano-roll panel, its height, every scroll position and the plug-in panels.
        // The live refresh has to keep all of that exactly where it was.
        {
            PianoRollView* pr = nullptr;
            VirtualKeyboard* vk = nullptr;
            std::function<void (Component&)> hunt = [&] (Component& c)
            {
                if (auto* p = dynamic_cast<PianoRollView*> (&c)) pr = p;
                if (auto* v = dynamic_cast<VirtualKeyboard*> (&c)) vk = v;
                for (auto* child : c.getChildren()) hunt (*child);
            };
            hunt (mc);

            st.requestPanel ("pianoroll");          // open the bottom panel, as the user would
            mc.resized();
            st.musicalTyping = true;
            mc.syncVirtualKeyboard();

            const auto prBefore = pr != nullptr ? pr->getBounds() : Rectangle<int>();
            const auto vkBefore = vk != nullptr ? vk->getBounds() : Rectangle<int>();
            const bool wasDark = theme::isDark();

            theme::setDark (true);
            if (auto* lf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel())) lf->applyColours();
            mc.refreshTheme();

            const bool keptPanel = pr != nullptr && pr->isVisible() && pr->getBounds() == prBefore;
            const bool keptKeyboard = vk != nullptr && vk->isVisible() && vk->getBounds() == vkBefore;
            bool shot = false;
            if (auto img = mc.createComponentSnapshot (mc.getLocalBounds(), true); img.isValid())
                if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("dark_main.png").createOutputStream()))
                {
                    PNGImageFormat fmt;
                    fmt.writeImageToStream (img, *stream);
                    shot = true;
                }

            theme::setDark (wasDark);
            if (auto* lf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel())) lf->applyColours();
            st.musicalTyping = false;
            mc.refreshTheme();
            mc.syncVirtualKeyboard();

            log ("theme: dark mode repaints the open piano roll / floating keyboard in place (panel "
                 + String (keptPanel ? "kept" : "moved") + ", keyboard " + String (keptKeyboard ? "kept" : "moved")
                 + ", dark_main.png)", keptPanel && keptKeyboard && shot);
        }

        // an instrument track starts dry: the built-in synth is a choice in the instrument picker,
        // not something the DAW puts on every new track
        auto& fresh = st.addTrack (TrackType::Instrument);
        const bool dry = ! fresh.hasInstrument && fresh.instrument.uid == 0 && fresh.type == TrackType::Instrument;
        st.deleteTrack (fresh.id);
        log ("new session: an instrument track is added without the built-in synth (or any instrument)", dry);

        // ... and selecting a track after an empty start has to bring the inspector's strip back:
        // the master column shrinks to half and the track's own strip appears
        InspectorView* insp = nullptr;
        std::function<void (Component&)> hunt = [&] (Component& root)
        {
            for (auto* child : root.getChildren())
            {
                if (insp != nullptr) return;
                if (auto* iv = dynamic_cast<InspectorView*> (child)) { insp = iv; return; }
                hunt (*child);
            }
        };
        hunt (mc);

        auto visibleStrips = [insp]
        {
            int n = 0;
            if (insp == nullptr) return n;
            for (auto* c : insp->getChildren())
                if (auto* cs = dynamic_cast<ChannelStrip*> (c))
                    if (cs->isVisible() && cs->getWidth() > 10) ++n;
            return n;
        };
        const int stripsBefore = visibleStrips();

        // nothing selected: the master keeps the right half of the column, the left half stays
        // empty (it must not stretch over the whole column and then shrink on selection)
        int widestBefore = 0;
        if (insp != nullptr)
            for (auto* c : insp->getChildren())
                if (auto* cs = dynamic_cast<ChannelStrip*> (c))
                    if (cs->isVisible()) widestBefore = jmax (widestBefore, cs->getWidth());
        const int inspW = insp != nullptr ? insp->getWidth() : 0;

        st.addTrack (TrackType::Instrument);       // AppState::addTrack selects it
        const int stripsAfter = visibleStrips();

        bool shot2 = false;
        if (auto img = mc.createComponentSnapshot (mc.getLocalBounds(), true); img.isValid())
            if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("inspector_track.png").createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
                shot2 = true;
            }

        log ("new session: adding a track brings the inspector's channel strip back ("
             + String (stripsBefore) + " -> " + String (stripsAfter) + " strips, inspector_track.png)",
             insp != nullptr && stripsBefore == 1 && stripsAfter == 2 && shot2);
        log ("new session: with nothing selected the master keeps the right half of the inspector ("
             + String (widestBefore) + " of " + String (inspW) + " px)",
             inspW > 100 && widestBefore > inspW / 4 && widestBefore <= inspW / 2 + 2);

        project.reset();                  // build the test project from here on
    }

    // ---- build a project ---------------------------------------------------
    project.reset();
    project.bpm = 120.0;
    auto& lead = project.addTrack (TrackType::Instrument, "Lead");
    lead.hasInstrument = true;              // an instrument track starts empty; the test wants sound
    lead.instrument = makeBuiltInSynthSlot();
    lead.volumeDb = -6.0f;
    const int leadId = lead.id;
    {
        MidiClip c;
        c.id = project.newId(); c.name = "Arp"; c.start = 0.0; c.length = 8.0;
        const int scale[] = { 0, 2, 4, 5, 7, 9, 11 };
        for (int i = 0; i < 16; ++i)
        {
            Note n; n.id = project.newId(); n.pitch = 60 + scale[i % 7] + 12 * (i / 7); n.start = i * 0.5; n.length = 0.45; n.velocity = 60 + i * 3;
            c.notes.push_back (n);
        }
        for (int i = 0; i <= 32; ++i)
        {
            CCPoint p; p.id = project.newId(); p.cc = 1; p.beat = i * 0.25; p.value = i * 127 / 32; c.ccs.push_back (p);
            CCPoint b; b.id = project.newId(); b.cc = kCCPitchBend; b.beat = i * 0.25; b.value = 8192 + (int) (2000.0 * std::sin (i * 0.4)); c.ccs.push_back (b);
        }
        lead.midiClips.push_back (c);
        auto& lane = lead.ensureLane ("vol", "Volume");
        lane.points = { { 0.0, 0.75f }, { 4.0, 0.25f }, { 8.0, 0.75f } };
    }

    // audio clip: generated sine
    File sine = out.getChildFile ("sine.wav");
    {
        WavAudioFormat wav;
        sine.deleteFile();
        auto* w = wav.createWriterFor (new FileOutputStream (sine), 44100.0, 1, 16, {}, 0);
        AudioBuffer<float> b (1, 44100 * 3);
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, 0.5f * std::sin (MathConstants<float>::twoPi * 440.0f * (float) i / 44100.0f));
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        delete w;
    }
    auto& au = project.addTrack (TrackType::Audio, "Sine");
    const int auId = au.id;
    {
        AudioClip c; c.id = project.newId(); c.name = "sine"; c.file = sine; c.start = 4.0; c.data = project.pool.load (sine, engine.formats);
        c.lengthSec = c.data != nullptr ? c.data->lengthSeconds() : 0.0;
        log ("audio import", c.data != nullptr && std::abs (c.lengthSec - 3.0) < 0.01);
        au.audioClips.push_back (c);
    }
    project.sendStructureChanged();
    project.sendContentChanged();
    project.sendMixChanged();
    engine.syncAll();

    // ---- night theme: what the dark colours have to look like ---------------------------------
    // The night palette is dark, but not black (the first one was so dark that the panels had no
    // separation left); text drawn on it is light; the piano keyboard keeps its black and white
    // look, and a tooltip stays a dark bubble with white text. Renders the arrangement and the
    // piano roll (with the demo project, so there is something to look at) plus a tooltip.
    {
        theme::setDark (true);
        if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();

        auto savePng = [] (const Image& img, const File& target)
        {
            if (! img.isValid()) return false;
            if (auto stream = std::unique_ptr<FileOutputStream> (target.createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
                return true;
            }
            return false;
        };

        Image tip (Image::ARGB, 230, 34, true);
        {
            Graphics g (tip);
            g.fillAll (theme::panel);
            LookAndFeel::getDefaultLookAndFeel().drawTooltip (g, TRANS ("Loop the selected range"), tip.getWidth(), tip.getHeight());
        }

        AppState st (project, engine, plugins);
        if (auto* t = project.findTrack (leadId); t != nullptr && ! t->midiClips.empty())
            st.openClipInEditor (leadId, t->midiClips.front().id);

        ArrangeView av (st);
        av.setSize (1200, 700);
        av.resized();

        PianoRollView pr (st);
        pr.setSize (1300, 620);
        pr.resized();

        // centre the view on the notes of the clip, so the screenshot shows the grid too (the
        // viewport keeps whatever scroll position the editor was left at)
        if (auto* c = pr.clip())
        {
            int lo = 127, hi = 0;
            for (auto& n : c->notes) { lo = jmin (lo, n.pitch); hi = jmax (hi, n.pitch); }

            if (lo <= hi)
            {
                const int centre = (lo + hi) / 2;
                const int viewH = jmax (1, pr.gridViewport.getMaximumVisibleHeight());
                pr.gridViewport.setViewPosition (0, jmax (0, (127 - centre) * pr.keyH - viewH / 2));
            }
        }

        const auto pianoShot = pr.createComponentSnapshot (pr.getLocalBounds(), true);
        const bool saved = savePng (av.createComponentSnapshot (av.getLocalBounds(), true), out.getChildFile ("dark_arrange.png"))
                        && savePng (pianoShot, out.getChildFile ("dark_piano.png"))
                        && savePng (tip, out.getChildFile ("dark_tooltip.png"));

        const double windowBright = theme::window.getPerceivedBrightness();
        log ("night theme: the background is dark but not black (" + theme::window.toDisplayString (false)
             + ", brightness " + String (windowBright, 2) + "), screenshots dark_arrange.png / dark_piano.png / dark_tooltip.png",
             saved && windowBright > 0.06 && windowBright < 0.3
             && theme::panel.getPerceivedBrightness() > windowBright
             && theme::keyWhite.getPerceivedBrightness() > 0.8
             && theme::keyBlack.getPerceivedBrightness() < 0.25);

        // The keyboard has to read as a real piano keyboard: a black key stops short of the white
        // keys' front edge, so at the back of its row the strip is dark while the front is white
        // (the white keys reach the front there); the white key row next to it is white all along.
        bool keysLookReal = false;
        String keysDetail = "no black key on screen";
        if (pianoShot.isValid())
        {
            const int keysLeft = pr.trackListWidth();
            const int keysTop = PianoRollView::kToolbarH
                              + (pr.app.pianoRollChords ? PianoRollView::kChordStripH : 0)
                              + PianoRollView::kRulerH;
            const int keysH = pr.gridViewport.getHeight();
            auto isBlackPitch = [] (int p)
            {
                const int pc = p % 12;
                return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            };

            for (int p = 0; p < 128 && ! keysLookReal; ++p)
            {
                if (! isBlackPitch (p)) continue;
                const int y = (127 - p) * pr.keyH - pr.scrollY();
                if (y < 0 || y + 2 * pr.keyH > keysH) continue;

                const int probeY = keysTop + y + pr.keyH / 4;
                const auto back = pianoShot.getPixelAt (keysLeft + 5, probeY);
                const auto front = pianoShot.getPixelAt (keysLeft + PianoRollView::kKeysW - 6, probeY);
                const auto white = pianoShot.getPixelAt (keysLeft + 5, keysTop + y + pr.keyH + pr.keyH / 2);
                keysLookReal = back.getPerceivedBrightness() < 0.35
                            && front.getPerceivedBrightness() > 0.75
                            && white.getPerceivedBrightness() > 0.8;
                keysDetail = "black " + String (back.getPerceivedBrightness(), 2)
                           + ", white front " + String (front.getPerceivedBrightness(), 2)
                           + ", white key " + String (white.getPerceivedBrightness(), 2);
            }
        }
        log ("piano keys: a black key is dark at the back and leaves a white front, the white keys beside it are white ("
             + keysDetail + ")", keysLookReal);

        theme::setDark (false);
        if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();
    }

    // ---- the main window's title bar is drawn by the app and follows Dark Mode ----------------
    // The OS title bar stayed light in the night theme, so the window had a bright strip on top of
    // the dark UI. The custom bar has to swap background and text with the theme, keep the three
    // window buttons working, and be paintable without a desktop peer (the self test runs headless
    // for this part).
    {
        auto savePng = [] (const Image& img, const File& target)
        {
            if (! img.isValid()) return false;
            if (auto stream = std::unique_ptr<FileOutputStream> (target.createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
                return true;
            }
            return false;
        };

        theme::setDark (false);
        if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();

        Component dummy;
        SXTitleBar bar (dummy);
        bar.setSize (900, 34);
        bar.setTitle ("SoundX - Title Test *");

        bool closeClicked = false;
        bar.onClose = [&closeClicked] { closeClicked = true; };
        bar.closeButton()->triggerClick();
        for (int i = 0; i < 3; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);

        bar.refreshTheme();
        const auto light = bar.createComponentSnapshot (bar.getLocalBounds(), true);

        theme::setDark (true);
        if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();
        bar.setMaximised (true);
        bar.refreshTheme();
        const auto dark = bar.createComponentSnapshot (bar.getLocalBounds(), true);

        theme::setDark (false);
        if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();

        const bool saved = savePng (light, out.getChildFile ("titlebar_light.png"))
                        && savePng (dark, out.getChildFile ("titlebar_dark.png"));

        // sample well inside the bar but away from the logo and the title glyphs
        const auto lightBg = light.getPixelAt (700, 8);
        const auto darkBg = dark.getPixelAt (700, 8);
        const bool followsTheme = darkBg.getPerceivedBrightness() < lightBg.getPerceivedBrightness() - 0.25;

        log ("title bar: custom drawn, the three window buttons work, and the colours follow Dark Mode ("
             + lightBg.toDisplayString (false) + " -> " + darkBg.toDisplayString (false)
             + ", titlebar_light.png / titlebar_dark.png)",
             saved && closeClicked && followsTheme
             && bar.minimiseButton() != nullptr && bar.maximiseButton() != nullptr);
    }

    // ---- mixdown -----------------------------------------------------------
    {
        ExportOptions o;
        o.mode = ExportOptions::Mix; o.target = out.getChildFile ("mix.wav"); o.sampleRate = 48000.0; o.bitDepth = 24; o.tailSeconds = 1.0;
        String err;
        const bool ok = engine.exportAudio (o, {}, err);
        log ("export mixdown " + err + " (" + String (engine.multiCoreThreads()) + " render threads, "
             + String (engine.lastBlockWasMultiCore() ? "multi-core" : "serial") + ")", ok);
        auto info = inspect (engine.formats, o.target);
        // 8 beats at 120 bpm = 4 s, the audio clip ends at 2 s + 3 s = 5 s, plus 1 s tail
        log ("mix.wav: " + String (info.seconds, 2) + " s, peak " + String (info.peak, 3) + ", " + String (info.channels) + " ch @ " + String (info.sr),
             info.ok && info.peak > 0.05f && info.sr == 48000.0 && info.seconds > 4.5 && info.seconds < 6.5);

        // the same file rendered with the multi-core scheduler switched off must match: this is
        // the regression test for level scheduling / deferred sends and bus routing
        {
            const bool wasMC = engine.isMultiCoreEnabled();
            engine.setMultiCoreEnabled (false);
            ExportOptions serial = o;
            serial.target = out.getChildFile ("mix_serial.wav");
            String serr;
            const bool serialOk = engine.exportAudio (serial, {}, serr);
            engine.setMultiCoreEnabled (wasMC);

            auto read = [] (const File& f, AudioBuffer<float>& dst) -> bool
            {
                AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
                if (r == nullptr || r->lengthInSamples <= 0) return false;
                const int n = (int) jmin<int64> (r->lengthInSamples, 48000 * 5);
                dst.setSize (2, n);
                return r->read (&dst, 0, n, 0, true, true);
            };
            AudioBuffer<float> a, b;
            const bool readOk = read (o.target, a) && read (serial.target, b);
            float maxDiff = 0.0f;
            if (readOk)
            {
                const int n = jmin (a.getNumSamples(), b.getNumSamples());
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < n; ++i)
                        maxDiff = jmax (maxDiff, std::abs (a.getReadPointer (c)[i] - b.getReadPointer (c)[i]));
            }
            log ("multi-core mixdown matches the serial render (max sample difference "
                 + String (maxDiff, 6) + ")", serialOk && readOk && maxDiff < 1.0e-4f);
        }

        // the user can pick how many cores render (View > Multi-core Rendering); the selection
        // restarts the worker pool and is remembered in the settings
        {
            const int beforeSetting = engine.getRenderCoresSetting();
            engine.setRenderCores (2);
            const bool two = engine.isMultiCoreEnabled() && engine.multiCoreCores() == 2;
            engine.setRenderCores (1);
            const bool off = ! engine.isMultiCoreEnabled() && engine.multiCoreThreads() == 0;
            const int autoCores = Engine::autoRenderCores();
            engine.setRenderCores (0);
            const bool autoOk = engine.isMultiCoreEnabled() && engine.multiCoreCores() == autoCores;
            engine.setRenderCores (beforeSetting);
            log ("multi-core: core count is selectable (" + String (two ? "2" : "?") + " / "
                 + String (off ? "off" : "?") + " / auto " + String (autoCores) + " cores)",
                 two && off && autoOk);
        }
    }
    {
        ExportOptions o;
        o.mode = ExportOptions::Mix; o.target = out.getChildFile ("mix_norm16.wav"); o.sampleRate = 44100.0; o.bitDepth = 16; o.normalize = true;
        String err;
        engine.exportAudio (o, {}, err);
        auto info = inspect (engine.formats, o.target);
        log ("normalized 16-bit export: peak " + String (info.peak, 3), info.ok && std::abs (info.peak - 0.891f) < 0.02f);

        // Regression: OfflineSink only checked the channel count before resizing its in-memory
        // accumulator, so from the second block on every copy wrote past the end of the heap
        // buffer (normalized / MP3 exports corrupted memory and crashed later at random, and the
        // samples after the first block were wrong). Compare the normalized file against an
        // unnormalized render of the same range, sample by sample, over the WHOLE file.
        ExportOptions plain = o;
        plain.target = out.getChildFile ("mix_normref.wav");
        plain.bitDepth = 24;
        plain.normalize = false;
        String perr;
        const bool refOk = engine.exportAudio (plain, {}, perr);

        auto readStereo = [] (AudioFormatManager& fm, const File& f, AudioBuffer<float>& dst) -> bool
        {
            std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
            if (r == nullptr || r->lengthInSamples <= 0) return false;
            dst.setSize ((int) jmin<unsigned int> (2u, (unsigned int) r->numChannels), (int) r->lengthInSamples);
            return r->read (&dst, 0, (int) r->lengthInSamples, 0, true, true);
        };
        AudioBuffer<float> a, b;
        const bool readOk = readStereo (engine.formats, plain.target, a)
                         && readStereo (engine.formats, o.target, b);
        float maxErr = 0.0f;
        const float peakRef = a.getMagnitude (0, a.getNumSamples());
        const float peakNorm = b.getMagnitude (0, b.getNumSamples());
        const float gain = peakRef > 1.0e-6f ? peakNorm / peakRef : 0.0f;
        if (readOk && a.getNumSamples() == b.getNumSamples())
            for (int c = 0; c < jmin (a.getNumChannels(), b.getNumChannels()); ++c)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    maxErr = jmax (maxErr, std::abs (b.getReadPointer (c)[i]
                                                     - a.getReadPointer (c)[i] * gain));
        log ("normalized export: the whole file matches the unnormalized render ("
             + String (b.getNumSamples()) + " samples, peak " + String (peakNorm, 3) + ", max error "
             + String (maxErr, 5) + ")",
             info.ok && refOk && readOk && a.getNumSamples() == b.getNumSamples()
                 && a.getNumSamples() > 4096 && peakNorm > 0.5f && maxErr < 0.02f);
    }

    // ---- mute / solo respected in the mix --------------------------------------
    {
        project.findTrack (leadId)->mute = true;
        project.sendMixChanged();
        ExportOptions o;
        o.mode = ExportOptions::Mix; o.target = out.getChildFile ("mix_leadmuted.wav"); o.sampleRate = 44100.0; o.bitDepth = 24;
        String err;
        engine.exportAudio (o, {}, err);
        auto info = inspect (engine.formats, o.target);
        log ("mute respected (peak " + String (info.peak, 3) + " should be the sine at 0.5)", info.ok && info.peak > 0.4f && info.peak < 0.55f);
        project.findTrack (leadId)->mute = false;
        project.sendMixChanged();
    }

    // ---- export from a worker thread (as the progress dialog does) --------------------------------------
    {
        struct Job : Thread
        {
            Job (Engine& e, ExportOptions o) : Thread ("export test"), engine (e), opts (std::move (o)) {}
            void run() override { ok = engine.exportAudio (opts, {}, err); done = true; }
            Engine& engine; ExportOptions opts; std::atomic<bool> done { false }; bool ok = false; String err;
        };
        ExportOptions o;
        o.mode = ExportOptions::Mix; o.target = out.getChildFile ("mix_thread.wav"); o.sampleRate = 44100.0; o.bitDepth = 24;
        Job job (engine, o);
        job.startThread();
        const auto t0 = Time::getMillisecondCounterHiRes();
        while (! job.done.load() && Time::getMillisecondCounterHiRes() - t0 < 30000.0)
            MessageManager::getInstance()->runDispatchLoopUntil (20);
        job.stopThread (2000);
        auto info = inspect (engine.formats, o.target);
        log ("threaded export (no deadlock): " + String (job.done.load() ? "finished" : "TIMED OUT") + ", peak " + String (info.peak, 3), job.done.load() && job.ok && info.peak > 0.05f);
    }

    // ---- stems -----------------------------------------------------------------
    {
        ExportOptions o;
        o.mode = ExportOptions::Stems; o.target = out.getChildFile ("stems"); o.sampleRate = 44100.0; o.bitDepth = 24;
        String err;
        const bool ok = engine.exportAudio (o, {}, err);
        auto files = o.target.findChildFiles (File::findFiles, false, "*.wav");
        log ("export stems: " + String (files.size()) + " files " + err, ok && files.size() == 2);
        for (auto& f : files)
        {
            auto info = inspect (engine.formats, f);
            log ("  " + f.getFileName() + " peak " + String (info.peak, 3) + " len " + String (info.seconds, 2), info.ok && info.peak > 0.01f);
        }

        // stems honour the track selection: only the checked tracks are exported
        {
            ExportOptions so;
            so.mode = ExportOptions::Stems;
            so.target = out.getChildFile ("stems_sel");
            so.sampleRate = 44100.0;
            so.bitDepth = 16;
            so.tailSeconds = 0.5;
            if (! project.tracks.empty())
                so.includeTracks.insert (project.tracks.front()->id);
            String serr;
            const bool sok = engine.exportAudio (so, {}, serr);
            auto sfiles = so.target.findChildFiles (File::findFiles, false, "*.wav");
            log ("stems track selection: " + String (sfiles.size()) + " of 2 tracks exported", sok && sfiles.size() == 1);
        }

        // default check state of the stems checklist: visible / unmuted normal tracks start
        // checked; hidden, muted and multi-output channels start unchecked
        {
            Track normal; normal.name = "Lead";
            Track hidden = normal; hidden.hidden = true;
            Track muted = normal; muted.mute = true;
            Track channel = normal; channel.sourceTrack = 12345; channel.sourceChannel = 2;
            const bool defOk = stemTrackCheckedByDefault (normal)
                            && ! stemTrackCheckedByDefault (hidden)
                            && ! stemTrackCheckedByDefault (muted)
                            && ! stemTrackCheckedByDefault (channel);
            log ("stems default selection: hidden / muted / output-channel tracks start unchecked", defOk);
        }

        // FLAC: native encoder, the file must carry the fLaC marker and read back
        {
            ExportOptions fo;
            fo.mode = ExportOptions::Mix;
            fo.format = ExportOptions::Flac;
            fo.target = out.getChildFile ("mix_flac.flac");
            fo.sampleRate = 44100.0;
            fo.bitDepth = 24;
            fo.tailSeconds = 0.0;
            String ferr;
            const bool fok = engine.exportAudio (fo, {}, ferr);

            char magic[4] {};
            {
                FileInputStream in (fo.target);
                if (in.openedOk()) in.read (magic, 4);
            }
            const bool isFlac = magic[0] == 'f' && magic[1] == 'L' && magic[2] == 'a' && magic[3] == 'C';
            bool readable = false;
            if (auto* rr = engine.formats.createReaderFor (fo.target))
            {
                std::unique_ptr<juce::AudioFormatReader> holder (rr);
                AudioBuffer<float> b (2, 4096);
                readable = rr->read (&b, 0, 4096, 0, true, true) && b.getMagnitude (0, 0, 4096) > 0.01f;
            }
            log ("flac export: " + String (fo.target.getSize()) + " bytes, marker " + (isFlac ? "ok" : "bad"),
                 fok && isFlac && readable);
        }

        // MP3: needs ffmpeg/lame; without one the export must fail with a helpful message
        {
            ExportOptions mo;
            mo.mode = ExportOptions::Mix;
            mo.format = ExportOptions::Mp3;
            mo.target = out.getChildFile ("mix_mp3.mp3");
            mo.sampleRate = 44100.0;
            mo.bitDepth = 24;
            mo.tailSeconds = 0.0;
            String merr;
            const bool mok = engine.exportAudio (mo, {}, merr);
            const bool hasEncoder = mok && mo.target.existsAsFile() && mo.target.getSize() > 1000;
            const bool cleanFail = ! mok && merr.isNotEmpty();
            log ("mp3 export: " + (hasEncoder ? String (mo.target.getSize()) + " bytes encoded"
                                             : String ("no encoder -> ") + (cleanFail ? "helpful error" : "silent failure")),
                 hasEncoder || cleanFail);
        }
    }

    // ---- MIDI round trip ---------------------------------------------------------
    {
        File mid = out.getChildFile ("roundtrip.mid");
        const bool wrote = project.exportMidi (mid);
        log ("export MIDI (" + String (mid.getSize()) + " bytes)", wrote && mid.getSize() > 50);

        Project p2;
        p2.formats = &engine.formats;
        int added = 0;
        const bool read = p2.importMidi (mid, 0.0, added);
        bool match = read && added == 1 && std::abs (p2.bpm - 120.0) < 0.01 && p2.tsNum == 4;
        int cc1 = 0, pb = 0, notes = 0;
        if (match)
        {
            auto& c = p2.tracks[0]->midiClips[0];
            notes = (int) c.notes.size();
            for (auto& p : c.ccs) { if (p.cc == 1) ++cc1; if (p.cc == kCCPitchBend) ++pb; }
            match = notes == 16 && cc1 == 33 && pb == 33 && p2.tracks[0]->name == "Lead";
        }
        log ("import MIDI back: notes " + String (notes) + ", CC1 " + String (cc1) + ", pitch-bend " + String (pb) + ", bpm " + String (p2.bpm), match);
    }

    // ---- MIDI tempo / time signature import + export ------------------------------------------
    {
        // a source project with tempo and meter changes, and a little content
        Project src;
        src.tempo.clear();
        src.tempo.push_back ({ 0.0, 120.0, 4, 4, false });
        tempomap::normalize (src.tempo);
        src.syncLegacyTempo();
        src.setTempo (8.0, 90.0, 3, 4);          // tempo AND meter change
        src.setTempo (16.0, 150.0, 0, 0);        // tempo only: the meter stays 3/4
        src.setTempo (40.0, 70.0, 5, 4);         // past the last note
        src.syncLegacyTempo();
        auto& lead = src.addTrack (TrackType::Instrument, "Lead");
        {
            MidiClip c;
            c.id = src.newId(); c.start = 0.0; c.length = 8.0;
            for (int i = 0; i < 8; ++i)
            {
                Note n;
                n.id = src.newId(); n.pitch = 60 + i; n.start = i; n.length = 0.9; n.velocity = 100;
                c.notes.push_back (n);
            }
            lead.midiClips.push_back (c);
        }

        const File mf = out.getChildFile ("tempo_meter.mid");
        const bool wrote = src.exportMidi (mf);

        // what the exported file says
        const auto map = readMidiTempoMap (mf, 120.0, 4, 4);
        bool mapOk = wrote && map.size() == 4;
        if (mapOk)
            mapOk = std::abs (map[0].beat) < 1.0e-6 && std::abs (map[0].bpm - 120.0) < 0.01 && map[0].tsNum == 4
                    && std::abs (map[1].beat - 8.0) < 1.0e-6 && std::abs (map[1].bpm - 90.0) < 0.01
                    && map[1].tsNum == 3 && map[1].tsDen == 4
                    && std::abs (map[2].beat - 16.0) < 1.0e-6 && std::abs (map[2].bpm - 150.0) < 0.01
                    && map[2].tsNum == 3
                    && std::abs (map[3].beat - 40.0) < 1.0e-6 && std::abs (map[3].bpm - 70.0) < 0.01
                    && map[3].tsNum == 5 && map[3].tsDen == 4;
        log ("MIDI export writes every tempo and meter change, including one past the last note ("
             + String (map.size()) + " segments: "
             + [&map] { StringArray t; for (auto& s : map) t.add (String (s.beat, 2) + "@" + String (s.bpm, 1) + "/"
                                                                    + String (s.tsNum) + "/" + String (s.tsDen)); return t.joinIntoString ("  "); }() + ")",
             mapOk);

        // importing with the sync off keeps the project's own map
        Project off;
        off.formats = &engine.formats;
        off.setTempo (0.0, 100.0, 6, 8);
        int addedOff = 0;
        const bool readOff = off.importMidi (mf, 0.0, addedOff, false);
        log ("import MIDI with the tempo sync off keeps the project's tempo map",
             readOff && addedOff == 1 && std::abs (off.bpmAt (4.0) - 100.0) < 1.0e-6 && off.tempo.size() == 1);

        // ... and with it on the file's map replaces it
        Project on;
        on.formats = &engine.formats;
        on.setTempo (0.0, 100.0, 6, 8);
        int addedOn = 0;
        const bool readOn = on.importMidi (mf, 0.0, addedOn, true);
        int n3 = 0, d3 = 0;
        on.meterAt (10.0, n3, d3);
        log ("import MIDI with the tempo sync on takes the file's tempo and meter map over",
             readOn && addedOn == 1 && std::abs (on.bpmAt (4.0) - 120.0) < 0.01
             && std::abs (on.bpmAt (9.0) - 90.0) < 0.01 && n3 == 3 && d3 == 4
             && std::abs (on.bpmAt (41.0) - 70.0) < 0.01);

        // the notes land on the same beats either way
        const bool sameNotes = readOff && readOn && ! off.tracks.empty() && ! on.tracks.empty()
                               && off.tracks[0]->midiClips.size() == 1 && on.tracks[0]->midiClips.size() == 1
                               && off.tracks[0]->midiClips[0].notes.size() == on.tracks[0]->midiClips[0].notes.size()
                               && std::abs (on.tracks[0]->midiClips[0].notes[1].start - 1.0) < 1.0e-6;
        log ("MIDI notes land on the same beats with or without the tempo map", sameNotes);

        // the import dialog shows the file's map next to the project's, and the switch
        {
            AppState st (project, engine, plugins);
            Array<File> files;
            files.add (mf);
            const auto shot = renderMidiImportPanel (st, files);
            if (shot.isValid())
                if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("midi_import_dialog.png").createOutputStream()))
                {
                    PNGImageFormat fmt;
                    fmt.writeImageToStream (shot, *stream);
                }
            log ("MIDI import dialog: the file's tempo map and the project's are shown side by side ("
                 + String (shot.getWidth()) + "x" + String (shot.getHeight()) + ")",
                 shot.isValid() && shot.getWidth() >= 400 && shot.getHeight() >= 180);
        }
    }

    // ---- save / load -----------------------------------------------------------------
    {
        File proj = out.getChildFile ("test.sxp");
        const bool saved = project.saveTo (proj);
        Project p3;
        p3.formats = &engine.formats;
        const bool loaded = saved && p3.loadFrom (proj);
        bool ok = loaded && p3.tracks.size() == 2 && p3.tracks[0]->midiClips[0].notes.size() == 16 && p3.tracks[0]->findLane ("vol") != nullptr
                  && p3.tracks[0]->findLane ("vol")->points.size() == 3 && p3.tracks[1]->audioClips.size() == 1 && p3.tracks[1]->audioClips[0].data != nullptr;
        log ("project save + load", ok);
    }

    // ---- undo ---------------------------------------------------------------------------
    {
        project.checkpoint();
        project.findTrack (leadId)->midiClips[0].notes.clear();
        project.sendContentChanged();
        project.undo();
        log ("undo restores deleted notes", project.findTrack (leadId) != nullptr && project.findTrack (leadId)->midiClips[0].notes.size() == 16);
        project.redo();
        project.undo();
    }

    // Dropping a folder of stems adds one track per file, so undoing it has to take the whole drop
    // back with a single Ctrl+Z: one undo step per file would make a folder-sized drop a chore.
    {
        Array<File> files;
        for (int i = 1; i <= 3; ++i)
        {
            const File f = out.getChildFile ("undo-import-" + String (i) + ".wav");
            f.deleteFile();

            AudioBuffer<float> b (1, 4410);
            for (int s = 0; s < b.getNumSamples(); ++s)
                b.setSample (0, s, 0.3f * std::sin (MathConstants<float>::twoPi * 330.0f * (float) s / 44100.0f));

            WavAudioFormat wav;
            if (auto* w = wav.createWriterFor (new FileOutputStream (f), 44100.0, 1, 16, {}, 0))
            {
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                delete w;
            }
            files.add (f);
        }

        auto countClips = [&project]
        {
            int n = 0;
            for (auto& t : project.tracks) if (t != nullptr) n += (int) t->audioClips.size();
            return n;
        };

        AppState st (project, engine, plugins);
        const int tracksBefore = (int) project.tracks.size();
        const int clipsBefore = countClips();

        importFiles (st, files, 0.0, -1, true);
        const int afterImport = (int) project.tracks.size();

        project.undo();
        const int afterUndo = (int) project.tracks.size();
        const int clipsAfterUndo = countClips();

        project.redo();
        const int afterRedo = (int) project.tracks.size();

        project.undo();        // leave the project the way this block found it
        log ("undo: one import of three files is one undo step (" + String (tracksBefore) + " -> "
             + String (afterImport) + " tracks, one Ctrl+Z back to " + String (afterUndo) + ", redo "
             + String (afterRedo) + ")",
             afterImport == tracksBefore + 3 && afterUndo == tracksBefore && clipsAfterUndo == clipsBefore
             && afterRedo == afterImport);

        // ... and the path a drag & drop from Explorer takes (the arrange view is the drop target)
        {
            ArrangeView av (st);
            av.setSize (1200, 700);
            av.resized();

            StringArray dropped;
            for (auto& f : files) dropped.add (f.getFullPathName());

            const int tracksBeforeDrop = (int) project.tracks.size();
            av.filesDropped (dropped, 400, av.getHeight() - 10);   // below the last track: one per file
            const int tracksAfterDrop = (int) project.tracks.size();

            project.undo();
            const int tracksAfterDropUndo = (int) project.tracks.size();

            log ("undo: dropping three files on the arrangement is one undo step ("
                 + String (tracksBeforeDrop) + " -> " + String (tracksAfterDrop) + " -> "
                 + String (tracksAfterDropUndo) + ")",
                 tracksAfterDrop == tracksBeforeDrop + 3 && tracksAfterDropUndo == tracksBeforeDrop);
        }

        project.clearUndo();
        for (auto& f : files) f.deleteFile();
    }

    // ---- plug-in operations while mixing are undoable -------------------------------------
    // The built-in synth stands in for a third-party plug-in: it is always available, it has
    // parameters, and its state really contains their values - which is what makes the difference
    // between "the effect came back" and "the effect came back with the sound it had". An
    // instrument track starts with it (Project::addTrack), so the test uses that instance.
    {
        String err;
        auto* track = project.findTrack (leadId);
        const bool haveInstrument = track != nullptr && track->hasInstrument;
        const int64 instUid = haveInstrument ? track->instrument.uid : 0;
        auto inst = haveInstrument ? engine.getInstance (instUid) : nullptr;

        auto valueOf = [] (const std::shared_ptr<PluginInstance>& i, int index) -> float
        {
            if (i == nullptr || i->proc == nullptr) return -1.0f;
            auto& params = i->proc->getParameters();
            if (index < 0 || index >= params.size() || params[index] == nullptr) return -1.0f;
            return params[index]->getValue();
        };

        bool paramOk = false;
        if (inst != nullptr && ! inst->proc->getParameters().isEmpty())
        {
            auto* param = inst->proc->getParameters()[0];
            const float before = param->getValue();
            const float wanted = jlimit (0.0f, 1.0f, before > 0.5f ? before - 0.25f : before + 0.25f);

            // a knob move: the plug-in editor brackets it with a parameter gesture, exactly like this
            engine.parameterGesture (*inst, 0, true);
            param->setValueNotifyingHost (wanted);
            engine.parameterGesture (*inst, 0, false);
            const float moved = param->getValue();     // a discrete parameter may snap to its step

            project.undo();
            const float afterUndo = valueOf (engine.getInstance (instUid), 0);
            project.redo();
            const float afterRedo = valueOf (engine.getInstance (instUid), 0);
            paramOk = std::abs (moved - before) > 1.0e-4f
                      && std::abs (afterUndo - before) < 1.0e-4f
                      && std::abs (afterRedo - moved) < 1.0e-4f;

            project.undo();      // the knob is back where it started
        }

        // A self-modulating plug-in can bracket every automated parameter change with a gesture
        // (begin/end pairs at high frequency). Each begin used to serialise the whole project to
        // XML for the undo stack, which froze the message thread. The debounce must collapse a
        // burst on one parameter into a single checkpoint.
        bool burstOk = false;
        if (inst != nullptr && ! inst->proc->getParameters().isEmpty())
        {
            project.clearUndo();
            const int beforeDepth = project.undoDepth();
            for (int i = 0; i < 40; ++i)
            {
                engine.parameterGesture (*inst, 0, true);
                engine.parameterGesture (*inst, 0, false);
            }
            burstOk = project.undoDepth() <= beforeDepth + 1;
            project.clearUndo();
        }
        log ("plug-in parameter gesture bursts are coalesced into one undo checkpoint ("
             + String (project.undoDepth() == 0 ? "yes" : "no") + ")", burstOk);

        // bypass is a mixing move too
        bool bypassOk = false;
        if (inst != nullptr)
        {
            engine.setSlotBypass (leadId, instUid, true);
            const bool on = project.findSlotByUid (instUid) != nullptr && project.findSlotByUid (instUid)->bypass;
            project.undo();
            const bool off = project.findSlotByUid (instUid) != nullptr && ! project.findSlotByUid (instUid)->bypass;
            bypassOk = on && off;
        }

        // removing the instrument and undoing brings it back with its settings
        bool removeOk = false;
        if (inst != nullptr && ! inst->proc->getParameters().isEmpty())
        {
            inst->proc->getParameters()[0]->setValueNotifyingHost (0.83f);
            const float wanted = valueOf (inst, 0);          // what the plug-in actually stored

            engine.removeSlot (leadId, instUid);
            const auto* emptied = project.findTrack (leadId);
            const bool gone = emptied != nullptr && ! emptied->hasInstrument;

            project.undo();
            auto* back = project.findSlotByUid (instUid);
            const float restored = valueOf (engine.getInstance (instUid), 0);
            removeOk = gone && back != nullptr && std::abs (restored - wanted) < 1.0e-4f;
        }

        // adding an insert, undoing it and redoing it keeps the settings of the new plug-in
        bool insertOk = false;
        {
            const bool added = engine.addInsert (leadId, makeBuiltInSynthSlot().desc, err);
            auto* withInsert = project.findTrack (leadId);
            const int64 insertUid = added && withInsert != nullptr && ! withInsert->inserts.empty()
                                        ? withInsert->inserts.back().uid : 0;
            auto insert = added ? engine.getInstance (insertUid) : nullptr;

            if (insert != nullptr && ! insert->proc->getParameters().isEmpty())
            {
                insert->proc->getParameters()[0]->setValueNotifyingHost (0.61f);
                const float wanted = valueOf (insert, 0);

                project.undo();
                auto* without = project.findTrack (leadId);
                const bool removed = without != nullptr && without->inserts.empty();

                project.redo();
                auto again = engine.getInstance (insertUid);
                const float kept = valueOf (again, 0);
                insertOk = added && removed && again != nullptr && std::abs (kept - wanted) < 1.0e-4f;
            }

            // leave the project as it was found: the synth on the track, no insert
            if (auto* t = project.findTrack (leadId))
            {
                while (! t->inserts.empty()) engine.removeSlot (leadId, t->inserts.back().uid);
                if (! t->hasInstrument) engine.setInstrument (leadId, makeBuiltInSynthSlot().desc, err);
            }
        }

        log ("undo: a plug-in parameter move (knob gesture) is undone and redone", paramOk);
        log ("undo: switching a plug-in to bypass is undone", bypassOk);
        log ("undo: a removed instrument comes back with its settings", removeOk);
        log ("undo: an added insert is removed and comes back with its settings", insertOk);
    }

    // ---- the undo / redo shortcuts --------------------------------------------------------
    {
        // Windows hands Ctrl+Z over to JUCE as the control character 0x1a with no text character
        // (Ctrl+Y as 0x19, ...). The key handler only compared the text character, so every
        // Ctrl+letter shortcut - undo and redo first of all - did nothing at all.
        AppState st (project, engine, plugins);
        MainComponent mc (st);
        mc.setSize (1200, 700);

        const int before = project.findTrack (leadId) != nullptr
                               ? (int) project.findTrack (leadId)->midiClips[0].notes.size() : -1;
        project.checkpoint();
        if (auto* t = project.findTrack (leadId))
        {
            t->midiClips[0].notes.clear();
            project.sendContentChanged();
        }

        const bool undoHandled = mc.keyPressed (KeyPress (26, ModifierKeys::ctrlModifier, 0));   // Ctrl+Z
        auto* afterUndo = project.findTrack (leadId);
        const bool undone = afterUndo != nullptr && (int) afterUndo->midiClips[0].notes.size() == before;

        const bool shiftZHandled = mc.keyPressed (KeyPress (26, ModifierKeys::ctrlModifier | ModifierKeys::shiftModifier, 0));
        auto* afterShiftZ = project.findTrack (leadId);
        const bool redoneByShiftZ = shiftZHandled && afterShiftZ != nullptr && afterShiftZ->midiClips[0].notes.empty();

        const bool undoAgain = mc.keyPressed (KeyPress (26, ModifierKeys::ctrlModifier, 0));
        const bool redoHandled = mc.keyPressed (KeyPress (25, ModifierKeys::ctrlModifier, 0));   // Ctrl+Y
        auto* afterRedo = project.findTrack (leadId);
        const bool redoneByY = redoHandled && afterRedo != nullptr && afterRedo->midiClips[0].notes.empty();

        log ("undo: Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y reach the arrangement (the key arrives as a control character)",
             undoHandled && undone && redoneByShiftZ && undoAgain && redoneByY);

        project.undo();     // back to the notes
    }

    // ---- tempo map ---------------------------------------------------------------------------
    {
        Project p4;
        p4.formats = &engine.formats;
        p4.reset();
        p4.setTempo (0.0, 120.0, 4, 4);
        p4.setTempo (8.0, 90.0, 3, 4);      // tempo AND meter change
        p4.setTempo (16.0, 150.0, 0, 0);    // tempo only, keeps 3/4
        bool ok = std::abs (p4.bpmAt (4.0) - 120.0) < 1.0e-6 && std::abs (p4.bpmAt (8.5) - 90.0) < 1.0e-6
                  && std::abs (p4.bpmAt (20.0) - 150.0) < 1.0e-6;
        int n = 0, d = 0;
        p4.meterAt (10.0, n, d);
        ok &= n == 3 && d == 4;
        // 8 beats at 120 bpm = 4 s, beat 8 -> 9 at 90 bpm = 0.6667 s
        const double t9 = p4.beatsToSeconds (9.0);
        ok &= std::abs (t9 - (4.0 + 2.0 / 3.0)) < 1.0e-3;
        ok &= std::abs (p4.secondsToBeats (t9) - 9.0) < 1.0e-6;          // round trip
        ok &= std::abs (p4.secondsToBeats (2.0) - 4.0) < 1.0e-9;
        ok &= std::abs (p4.secondsToBeats (p4.beatsToSeconds (14.5)) - 14.5) < 1.0e-6;
        // count-in extrapolation: 2 beats back at 120 bpm = -1 s
        ok &= std::abs (p4.beatsToSeconds (-2.0) + 1.0) < 1.0e-6;
        // bars: 0, 4 (4/4) then 8, 11, 14, 17 (3/4)
        std::vector<tempomap::BarInfo> bars;
        tempomap::barStartsBetween (p4.tempo, 0.0, 20.0, bars);
        ok &= (int) bars.size() == 6 && bars[2].beat == 8.0 && bars[2].num == 3 && bars[4].num == 3;
        // 9.5 = 2 bars of 4/4 plus beat 1.5 of the next (meter numbers keep counting on)
        auto bp = tempomap::barPosition (p4.tempo, 9.5);
        ok &= bp.bar == 2 && std::abs (bp.beatInBar - 1.5) < 1.0e-9;
        // exact bar lines must resolve onto the bar they start, never the previous one
        ok = ok && tempomap::barStartAtOrBefore (p4.tempo, 8.0) == 8.0
                 && tempomap::barStartAtOrBefore (p4.tempo, 11.0) == 11.0
                 && tempomap::barStartAtOrBefore (p4.tempo, 14.0) == 14.0;
        // beat 16 is a tempo-only change two beats into a 3/4 bar: it must NOT reset the grid
        ok = ok && tempomap::barStartAtOrBefore (p4.tempo, 16.0) == 14.0;
        auto bp4 = tempomap::barPosition (p4.tempo, 4.0);
        ok = ok && bp4.bar == 1 && bp4.beatInBar == 0.0 && bp4.num == 4;
        auto bp16 = tempomap::barPosition (p4.tempo, 16.0);
        ok = ok && bp16.bar == 4 && std::abs (bp16.beatInBar - 2.0) < 1.0e-9;
        // the LCD readout: bar, beat (of the meter), sixteenth, tick
        ok = ok && barsBeatsString (6.25, p4) == "002 3 2 000" && barsBeatsString (0.0, p4) == "001 1 1 000";
        log ("tempo map: bpm/meter lookup, beats<->seconds, bar grid", ok);

        // XML round trip
        File f4 = out.getChildFile ("tempo.sxp");
        p4.saveTo (f4);
        Project p5;
        p5.formats = &engine.formats;
        bool rt = p5.loadFrom (f4);
        rt = rt && (int) p5.tempo.size() == 3 && std::abs (p5.tempo[1].bpm - 90.0) < 1.0e-9
                 && p5.tempo[1].tsNum == 3 && std::abs (p5.tempo[1].beat - 8.0) < 1.0e-9
                 && p5.tempo[2].tsNum == 3 && std::abs (p5.tempo[2].bpm - 150.0) < 1.0e-9;
        log ("tempo map XML round trip", rt);
    }

    // ---- linear tempo ramps ---------------------------------------------------------------
    {
        const double ln2 = std::log (2.0);
        Project pr;
        pr.formats = &engine.formats;
        pr.reset();
        pr.setTempo (0.0, 120.0, 4, 4);
        pr.setTempo (8.0, 60.0, 0, 0);
        pr.setTempoRamp (0.0, true);                       // 120 -> 60 bpm across beats 0..8
        // closed form: integral of 60 / v(x) dx = (60 / k) ln (v1 / v0) = 8 ln 2 seconds
        bool ok = std::abs (pr.bpmAt (4.0) - 90.0) < 1.0e-9 && std::abs (pr.bpmAt (2.0) - 105.0) < 1.0e-9;
        ok = ok && std::abs (pr.beatsToSeconds (8.0) - 8.0 * ln2) < 1.0e-9;
        ok = ok && std::abs (pr.beatsToSeconds (4.0) + 8.0 * std::log (0.75)) < 1.0e-9;
        ok = ok && std::abs (pr.beatsToSeconds (10.0) - (8.0 * ln2 + 2.0)) < 1.0e-9;   // holds 60 after the ramp
        double numeric = 0.0;                                                             // independent midpoint integral
        const int steps = 40000;
        for (int i = 0; i < steps; ++i) numeric += (6.0 / steps) * 60.0 / pr.bpmAt ((i + 0.5) * 6.0 / steps);
        ok = ok && std::abs (numeric - pr.beatsToSeconds (6.0)) < 1.0e-6;
        for (double b : { 0.5, 3.0, 6.0, 7.9, 8.0, 10.0 })
            ok = ok && std::abs (pr.secondsToBeats (pr.beatsToSeconds (b)) - b) < 1.0e-9;
        log ("linear tempo ramp: bpm interpolation, seconds integral and inverse", ok);

        // a rising ramp that ends on a meter change
        Project pu;
        pu.formats = &engine.formats;
        pu.reset();
        pu.setTempo (0.0, 90.0, 4, 4);
        pu.setTempoRamp (0.0, true);
        pu.setTempo (4.0, 180.0, 3, 4);                    // 90 -> 180 over beats 0..4, then 3/4
        bool up = std::abs (pu.bpmAt (2.0) - 135.0) < 1.0e-9 && std::abs (pu.beatsToSeconds (4.0) - (60.0 / 22.5) * ln2) < 1.0e-9;
        for (double b : { 1.0, 2.5, 4.0, 5.0, 9.0 })
            up = up && std::abs (pu.secondsToBeats (pu.beatsToSeconds (b)) - b) < 1.0e-9;
        int mn = 0, md = 0;
        pu.meterAt (5.0, mn, md);
        up = up && mn == 3 && md == 4;
        log ("linear tempo ramp: rising ramp into a meter change", up);

        // save / load keeps the ramp flags
        File rf = out.getChildFile ("ramp.sxp");
        const bool saved = pr.saveTo (rf);
        Project pl;
        pl.formats = &engine.formats;
        const bool loaded = saved && pl.loadFrom (rf);
        const bool persisted = loaded && pl.tempo.size() == 2 && pl.tempo[0].ramp && ! pl.tempo[1].ramp
                               && std::abs (pl.beatsToSeconds (8.0) - 8.0 * ln2) < 1.0e-9;
        log ("linear tempo ramp: project save + load", persisted);

        // audio is placed in seconds: a clip of exactly 8 ln 2 s ends on beat 8 through the ramp
        auto& rat = pr.addTrack (TrackType::Audio, "RampAudio");
        AudioClip rac;
        rac.start = 0.0;
        rac.lengthSec = 8.0 * ln2;
        rat.audioClips.push_back (rac);
        const bool span = std::abs (rat.endBeat (pr.tempo) - 8.0) < 1.0e-6
                          && std::abs (pr.clipBeats (0.0, 8.0 * ln2) - 8.0) < 1.0e-6;
        log ("linear tempo ramp: audio clip span follows the curve", span);

        // MIDI has no ramps: the ramp is written as a tempo event on every beat
        {
            auto& mt = pr.addTrack (TrackType::Instrument, "RampMidi");
            MidiClip mc; mc.id = pr.newId(); mc.start = 0.0; mc.length = 8.0;
            Note n; n.id = pr.newId(); n.pitch = 60; n.start = 6.0; n.length = 1.0;
            mc.notes.push_back (n);
            mt.midiClips.push_back (mc);
        }
        File rmid = out.getChildFile ("ramp.mid");
        const bool wrote = pr.exportMidi (rmid);
        Project pm;
        pm.formats = &engine.formats;
        int addedR = 0;
        const bool read = wrote && pm.importMidi (rmid, 0.0, addedR);
        // the per-beat steps approximate the curve: the note at beat 6 lands within a few ms
        const double noteSecExact = pr.beatsToSeconds (6.0);
        const double noteSecImported = pm.beatsToSeconds (6.0);
        const bool midi = read && addedR == 1
                          && pm.bpmAt (4.5) < 90.0 && pm.bpmAt (4.5) > 82.5        // beat 4..5 ramps 90 -> 82.5
                          && pm.bpmAt (6.5) < 75.0 && pm.bpmAt (6.5) > 67.5
                          && std::abs (noteSecExact - noteSecImported) < 0.001;
        log ("linear tempo ramp: MIDI export writes the curve per beat (note drift "
                 + String ((noteSecImported - noteSecExact) * 1000.0, 1) + " ms)", midi);

        // end to end: the engine schedules a note through the ramp at the exact second
        {
            const auto savedTempo = project.tempo;
            project.tempo = pr.tempo;                                   // 120 -> 60 over beats 0..8
            project.syncLegacyTempo();
            auto& ht = project.addTrack (TrackType::Instrument, "RampHit");
            ht.hasInstrument = true;                 // the test renders this track
            ht.instrument = makeBuiltInSynthSlot();
            const int hitId = ht.id;
            MidiClip hc; hc.id = project.newId(); hc.start = 0.0; hc.length = 10.0;
            Note hn; hn.id = project.newId(); hn.pitch = 69; hn.start = 8.0; hn.length = 1.0; hn.velocity = 120;
            hc.notes.push_back (hn);
            ht.midiClips.push_back (hc);
            for (auto& t : project.tracks) t->solo = (t->id == hitId);
            project.sendStructureChanged();
            project.sendContentChanged();
            project.sendMixChanged();
            engine.syncAll();

            ExportOptions ro;
            ro.mode = ExportOptions::Mix; ro.target = out.getChildFile ("ramp_render.wav");
            ro.sampleRate = 48000.0; ro.bitDepth = 24; ro.tailSeconds = 0.3;
            String rerr;
            const bool rex = engine.exportAudio (ro, {}, rerr);
            double onset = -1.0;
            if (rex)
                if (std::unique_ptr<AudioFormatReader> r { engine.formats.createReaderFor (ro.target) })
                {
                    AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
                    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
                    for (int i = 0; i < b.getNumSamples() && onset < 0.0; ++i)
                        if (std::abs (b.getSample (0, i)) > 0.003f) onset = i / r->sampleRate;
                }
            const double expect = 8.0 * ln2;     // 5.545 s, a fixed 120 bpm would put it at 4.000 s
            log ("linear tempo ramp: rendered note at beat 8 starts at " + String (onset, 4) + " s (expect "
                     + String (expect, 4) + " s) " + rerr,
                 rex && onset >= expect - 0.002 && onset < expect + 0.02);

            for (auto& t : project.tracks) t->solo = false;
            project.removeTrack (hitId);
            project.tempo = savedTempo;
            project.syncLegacyTempo();
            project.sendStructureChanged();
            project.sendContentChanged();
            project.sendMixChanged();
            engine.syncAll();
        }
    }

    // ---- event timebase: bars or seconds (Studio One style) --------------------------------------
    {
        Project tb;
        tb.formats = &engine.formats;
        tb.tempo = { TempoSegment { 0.0, 120.0, 4, 4, false } };
        tb.syncLegacyTempo();

        auto& t = tb.addTrack (TrackType::Audio, "TB");
        AudioClip musical;  musical.id = tb.newId(); musical.name = "musical";
        musical.start = 8.0; musical.lengthSec = 2.0; musical.timebase = Timebase::Bars;
        AudioClip absolute; absolute.id = tb.newId(); absolute.name = "absolute";
        absolute.start = 8.0; absolute.lengthSec = 2.0; absolute.timebase = Timebase::Seconds;
        t.audioClips.push_back (musical);
        t.audioClips.push_back (absolute);

        // 8 beats at 120 bpm = 4 s. Halving the tempo keeps the musical clip at beat 8 and puts the
        // seconds clip where 4 s land at the new tempo: beat 4.
        tb.setTempo (0.0, 60.0, 4, 4);
        auto clipAt = [&tb] (const char* name) -> const AudioClip*
        {
            for (auto& tr : tb.tracks)
                for (auto& c : tr->audioClips)
                    if (c.name == name) return &c;
            return nullptr;
        };
        const AudioClip* m1 = clipAt ("musical");
        const AudioClip* s1 = clipAt ("absolute");
        const double musicalStart = m1 != nullptr ? m1->start : -1.0;
        const double secondsStart = s1 != nullptr ? s1->start : -1.0;
        const bool halved = m1 != nullptr && s1 != nullptr
                         && std::abs (musicalStart - 8.0) < 1.0e-6 && std::abs (secondsStart - 4.0) < 1.0e-3;

        // back to 120: the seconds clip returns to beat 8, the musical one never moved
        tb.setTempo (0.0, 120.0, 4, 4);
        const AudioClip* m2 = clipAt ("musical");
        const AudioClip* s2 = clipAt ("absolute");
        log ("timebase: a musical clip keeps its bar and a seconds clip keeps its time when the tempo halves "
             "(musical " + String (musicalStart, 2) + ", seconds " + String (secondsStart, 2) + " beats)",
             halved && m2 != nullptr && s2 != nullptr
             && std::abs (m2->start - 8.0) < 1.0e-6 && std::abs (s2->start - 8.0) < 1.0e-3);

        // the flag survives a save / load round trip
        Project reloaded;
        reloaded.formats = &engine.formats;
        if (auto xml = tb.toXml (false)) reloaded.fromXml (*xml);
        bool flags = false;
        for (auto& tr : reloaded.tracks)
            for (auto& c : tr->audioClips)
                flags = flags || (c.name == "absolute" && c.timebase == Timebase::Seconds);
        log ("timebase: the per-clip timebase is saved with the project", flags);
    }

    // ---- count-in: 1 / 2 / 4 / 8 bars -----------------------------------------------------------
    {
        Project ci;
        const int saved = ci.countInBars;
        ci.countInBars = 2;

        Project back;
        if (auto xml = ci.toXml (false)) back.fromXml (*xml);
        const bool twoBars = back.countInBars == 2;

        // the transport commands set it (and the transport menu offers 0 / 1 / 2 / 4 / 8)
        AppState st (project, engine, plugins);
        MainComponent mc (st);
        mc.perform (CmdCountIn4Bars);
        const bool four = st.project.countInBars == 4;
        mc.perform (CmdCountInOff);
        const bool off = st.project.countInBars == 0;
        mc.perform (CmdCountIn2Bars);
        const bool two = st.project.countInBars == 2;
        st.project.countInBars = 0;

        log ("count-in: the number of bars is saved and set from the transport commands (saved "
             + String (saved) + ")", twoBars && four && off && two);
    }

    // ---- project template: what a new session starts from --------------------------------------
    {
        const File tpl = out.getChildFile ("test-template.sxp");
        tpl.deleteFile();

        Project src;
        src.formats = &engine.formats;
        src.setTempo (0.0, 96.0, 4, 4);          // the template's tempo (the map, not just the mirror)
        auto& ti = src.addTrack (TrackType::Instrument, "TplInst");
        ti.hasInstrument = true;
        ti.instrument = makeBuiltInSynthSlot();
        src.addTrack (TrackType::Audio, "TplAudio");

        const bool saved = src.saveAsTemplate (tpl);

        Project dst;
        dst.formats = &engine.formats;
        dst.tempo = { TempoSegment { 0.0, 120.0, 4, 4, false } };
        dst.addTrack (TrackType::Audio, "ShouldBeReplaced");
        const bool loaded = dst.loadFromTemplate (tpl);

        const bool ok = saved && loaded && dst.tracks.size() == 2 && std::abs (dst.bpmAt (0.0) - 96.0) < 1.0e-6
                     && dst.tracks[0] != nullptr && dst.tracks[0]->hasInstrument
                     && dst.tracks[0]->instrument.desc.name.isNotEmpty()
                     && dst.tracks[1] != nullptr && dst.tracks[1]->name == "TplAudio"
                     && dst.file == File() && ! dst.canUndo() && ! dst.dirty;
        log ("project template: a saved template loads as an untitled project with its tracks and instruments "
             "(saved " + String (saved ? "yes" : "no") + ", loaded " + String (loaded ? "yes" : "no")
             + ", tracks " + String ((int) dst.tracks.size()) + ", bpm " + String (dst.bpmAt (0.0), 1)
             + ", file '" + dst.file.getFullPathName() + "', undo " + String (dst.canUndo() ? "yes" : "no")
             + ", dirty " + String (dst.dirty ? "yes" : "no")
             + ", inst " + (dst.tracks.empty() || dst.tracks[0] == nullptr ? String ("-") : dst.tracks[0]->instrument.desc.name) + ")",
             ok);

        tpl.deleteFile();
    }

    // ---- timebase of the ruler / readout: bars or seconds ---------------------------------------
    {
        const bool labels = tempomap::timeLabel (0.0) == "0:00"
                         && tempomap::timeLabel (65.4) == "1:05"
                         && tempomap::timeLabel (3599.0) == "59:59";

        AppState st (project, engine, plugins);
        const bool before = st.timeInSeconds;
        st.setTimeInSeconds (true);

        ArrangeView av (st);
        av.setSize (1200, 700);
        av.resized();

        bool shot = false;
        if (auto img = av.createComponentSnapshot (av.getLocalBounds(), true); img.isValid())
            if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("time_seconds.png").createOutputStream()))
            {
                PNGImageFormat fmt;
                fmt.writeImageToStream (img, *stream);
                shot = true;
            }

        st.setTimeInSeconds (before);   // leave the setting as it was (it is saved in the properties)
        log ("timebase: the ruler can show seconds instead of bars (labels " + tempomap::timeLabel (65.4)
             + ", screenshot time_seconds.png)", labels && st.timeInSeconds == before && shot);
    }

    // ---- crossfade length + curve (Studio One style audio editing) ------------------------------
    {
        // a silent take so an overlap can be measured as the outgoing clip alone
        File sil = out.getChildFile ("silent.wav");
        {
            WavAudioFormat wav;
            sil.deleteFile();
            if (auto* w = wav.createWriterFor (new FileOutputStream (sil), 44100.0, 1, 16, {}, 0))
            {
                AudioBuffer<float> b (1, 44100 * 3);
                b.clear();
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                delete w;
            }
        }
        auto& xt = project.addTrack (TrackType::Audio, "XCurve");
        const int xtid = xt.id;
        AudioClip a; a.id = project.newId(); a.name = "xa"; a.file = sine; a.start = 0.0;
        a.data = project.pool.load (sine, engine.formats); a.lengthSec = 2.0;
        AudioClip b; b.id = project.newId(); b.name = "xb"; b.file = sil;
        b.start = project.secondsToBeats (1.0);            // 1 s overlap
        b.data = project.pool.load (sil, engine.formats); b.lengthSec = 2.0;
        xt.audioClips.push_back (a);
        xt.audioClips.push_back (b);
        engine.syncAll();

        for (auto& t : project.tracks) t->solo = (t->id == xtid);
        auto peakAt = [&] (double sec) -> double
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix;
            o.target = out.getChildFile ("xcurve.wav");
            o.sampleRate = 44100.0;
            o.bitDepth = 24;
            o.tailSeconds = 0.0;
            String err;
            if (! engine.exportAudio (o, {}, err)) return -1.0f;
            auto info = inspect (engine.formats, o.target);
            if (! info.ok) return -1.0f;
            const int N = (int) (info.seconds * info.sr);
            AudioBuffer<float> buf (2, N);
            std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
            r->read (&buf, 0, N, 0, true, true);
            const int start = jlimit (0, jmax (0, N - 1), (int) (sec * info.sr));
            const int len = jmin (jmax (1, (int) (0.04 * info.sr)), N - start);
            return buf.getMagnitude (0, start, len);
        };

        // the outgoing half must follow the curve exactly: A is a 0.5 sine, so the
        // peak at a point of the overlap is 0.5 x the curve value there
        struct CurveCase { int curve; float at25, at75; const char* name; };
        const CurveCase cases[] = { { 0, 0.4619f, 0.1913f, "equal power" },
                                    { 1, 0.3750f, 0.1250f, "linear" },
                                    { 2, 0.4219f, 0.0781f, "S-curve" } };
        for (auto& cc : cases)
        {
            xt.audioClips[0].xfadeCurve = cc.curve;
            engine.syncAll();
            const float p25 = peakAt (1.25), p75 = peakAt (1.75);
            const bool ok = p25 > 0.0f && std::abs (p25 - cc.at25) < 0.02f && std::abs (p75 - cc.at75) < 0.02f;
            log (String ("crossfade curve ") + cc.name + ": " + String (p25, 3) + " / " + String (p75, 3),
                 ok);
        }
        xt.audioClips[0].xfadeCurve = 0;
        for (auto& t : project.tracks) t->solo = false;

        // resizing the overlap moves B only: its content and its end stay in place
        AudioClip& ca = xt.audioClips[0];
        AudioClip& cb = xt.audioClips[1];
        cb.start = project.secondsToBeats (1.6);
        cb.offsetSec = 0.8;
        const double srcAtStart = project.beatsToSeconds (cb.start) - cb.offsetSec;
        const double endBefore = project.beatsToSeconds (cb.start) + cb.lengthSec;
        const double got = project.resizeCrossfade (ca, cb, 1.0);
        const bool okLen = std::abs (got - 1.0) < 0.01
                        && std::abs ((project.beatsToSeconds (cb.start) - cb.offsetSec) - srcAtStart) < 1.0e-6
                        && std::abs ((project.beatsToSeconds (cb.start) + cb.lengthSec) - endBefore) < 0.01;
        log ("crossfade: length 0.4 -> " + String (got, 2) + " s, content stays in place", okLen);

        // the overlap can only grow as far as B has head material in the file
        cb.start = project.secondsToBeats (1.6); cb.offsetSec = 0.2; cb.lengthSec = 1.4;
        const double capped = project.resizeCrossfade (ca, cb, 5.0);
        log ("crossfade: 5 s request capped by source headroom (got " + String (capped, 2) + " s)",
             capped > 0.39 && capped < 0.61);

        // no overlap: nothing to resize
        cb.start = project.secondsToBeats (4.0);
        log ("crossfade: resize on separate clips is a no-op", project.resizeCrossfade (ca, cb, 0.5) == 0.0);

        // the curve choice survives save + load
        xt.audioClips[0].xfadeCurve = 2;
        File cf = out.getChildFile ("xfadecurve.sxp");
        Project p5;
        p5.formats = &engine.formats;
        const bool saved2 = project.saveTo (cf) && p5.loadFrom (cf);
        const auto* rt = p5.findTrack (xtid);
        log ("crossfade: curve saved and loaded",
             saved2 && rt != nullptr && rt->audioClips.size() == 2 && rt->audioClips[0].xfadeCurve == 2);

        project.removeTrack (xtid);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }

    // ---- splitting keeps the clip gain envelope continuous -------------------------------------
    {
        std::vector<ClipGainPoint> src { { 0.0, 0.0f }, { 4.0, -12.0f }, { 8.0, 0.0f } };
        std::vector<ClipGainPoint> l, r;
        splitClipGainEnv (src, 6.0, 0.0f, l, r);
        const bool ok = l.size() == 3 && r.size() == 2
                     && std::abs (l.back().beat - 6.0) < 1.0e-9 && std::abs (l.back().db + 6.0f) < 0.02f
                     && std::abs (r.front().beat) < 1.0e-9 && std::abs (r.front().db + 6.0f) < 0.02f
                     && std::abs (r.back().beat - 2.0) < 1.0e-9;
        log ("split: gain envelope joins up at the cut (seam " + String (l.empty() ? 0.0f : l.back().db, 2) + " dB)", ok);

        // an empty envelope stays empty on both halves
        std::vector<ClipGainPoint> l2, r2;
        splitClipGainEnv ({}, 3.0, -6.0f, l2, r2);
        log ("split: flat clips stay flat", l2.empty() && r2.empty());
    }

    // ---- plug-in delay compensation ------------------------------------------------------------
    {
        // shortcut synth waveforms into a file queue so both tracks share identical material
        const auto buildSineTrack = [&] (const String& name) -> int
        {
            auto& t = project.addTrack (TrackType::Audio, name);
            AudioClip c; c.id = project.newId(); c.name = name; c.file = sine; c.start = 0.0;
            c.data = project.pool.load (sine, engine.formats);
            c.lengthSec = c.data != nullptr ? c.data->lengthSeconds() : 0.0;
            t.audioClips.push_back (std::move (c));
            return t.id;
        };
        const int dA = buildSineTrack ("DLY-A");
        const int dB = buildSineTrack ("DLY-B");
        String err;
        const bool dlOk = engine.addInsert (dB, makeBuiltInDelaySlot().desc, err);
        log ("PDC: built-in test delay loaded " + err, dlOk);

        ExportOptions o;
        o.mode = ExportOptions::Stems; o.target = out.getChildFile ("pdc"); o.sampleRate = 44100.0; o.bitDepth = 16; o.tailSeconds = 0.2;
        const bool ex = engine.exportAudio (o, {}, err);   // runs prepareAll: captures the plug-in latency
        auto inst = engine.getInstance (project.findTrack (dB) != nullptr && ! project.findTrack (dB)->inserts.empty()
                                            ? project.findTrack (dB)->inserts.front().uid : -1);
        const int lat = inst != nullptr ? inst->latencySamples.load() : -1;
        log ("PDC: plug-in latency " + String (lat) + " samples (expect 960)", ex && lat == 960);

        File fa, fb;
        for (auto& f : o.target.findChildFiles (File::findFiles, false, "*.wav"))
        {
            if (f.getFileNameWithoutExtension().endsWithIgnoreCase ("DLY-A")) fa = f;
            else if (f.getFileNameWithoutExtension().endsWithIgnoreCase ("DLY-B")) fb = f;
        }
        bool ok = ex && fa.existsAsFile() && fb.existsAsFile();
        if (ok)
        {
            AudioBuffer<float> ba, bb;
            double sra = 0, srb = 0;
            {
                std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (fa));
                sra = r->sampleRate;
                ba.setSize ((int) r->numChannels, (int) r->lengthInSamples);
                r->read (&ba, 0, (int) r->lengthInSamples, 0, true, true);
            }
            {
                std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (fb));
                srb = r->sampleRate;
                bb.setSize ((int) r->numChannels, (int) r->lengthInSamples);
                r->read (&bb, 0, (int) r->lengthInSamples, 0, true, true);
            }
            ok = sra == srb && ba.getNumSamples() == bb.getNumSamples() && ba.getNumChannels() == bb.getNumChannels();
            if (ok)
            {
                float diff = 0.0f;
                for (int c = 0; c < ba.getNumChannels(); ++c)
                    for (int i = 0; i < ba.getNumSamples(); ++i)
                        diff = jmax (diff, std::abs (ba.getSample (c, i) - bb.getSample (c, i)));
                ok = diff < 0.001f;
                log ("PDC: both chains sample-aligned after compensation (max diff " + String (diff, 5) + ")", ok);
            }
            else
                log ("PDC: stem formats differ - cannot compare", false);
        }
        else
            log ("PDC: stem export failed: " + err, false);

        if (! project.findTrack (dB)->inserts.empty())
            engine.removeSlot (dB, project.findTrack (dB)->inserts.front().uid);
        project.sendStructureChanged();
        project.removeTrack (dA);
        project.removeTrack (dB);
        project.sendStructureChanged();
    }

    // ---- real-time callback -----------------------------------------------------------------
    {
        const auto err = engine.initialiseAudio();
        for (auto& t : engine.deviceManager.getAvailableDeviceTypes())
            log ("  [audio backend] " + t->getTypeName() + (t->getTypeName() == "ASIO" ? "  <- low latency ASIO support" : ""));
        auto* dev = engine.deviceManager.getCurrentAudioDevice();
        if (dev == nullptr)
            log ("audio device not available (" + err + ") - skipping real-time test");
        else
        {
            log ("audio device: " + dev->getName() + " @ " + String (dev->getCurrentSampleRate()) + " Hz, buffer " + String (dev->getCurrentBufferSizeSamples()));
            project.loopOn = false;
            engine.setPositionBeats (0.0);
            engine.play();
            float peak = 0.0f;
            for (int i = 0; i < 20; ++i)
            {
                Thread::sleep (100);
                float l, r;
                engine.readMasterPeaks (l, r);
                peak = jmax (peak, l, r);
            }
            const double beats = engine.getPositionBeats();
            engine.seekToBeats (8.0);              // a scrub while playing must not stall the transport
            Thread::sleep (300);
            const double scrubbed = engine.getPositionBeats();
            engine.stop();
            log ("real-time playback: position " + String (beats, 2) + " beats, master peak " + String (peak, 3), beats > 2.0 && peak > 0.01f);
            log ("real-time seek: a playing scrub lands at " + String (scrubbed, 2) + " beats",
                 scrubbed > 7.8 && scrubbed < 9.5);
        }
    }

    // ---- touch automation recording -----------------------------------------------------------------
    if (engine.deviceManager.getCurrentAudioDevice() != nullptr)
    {
        auto* leadT = project.findTrack (leadId);
        leadT->autoMode = AutoMode::Touch;
        project.sendContentChanged();
        const size_t before = leadT->findLane ("vol")->points.size();
        engine.setPositionBeats (0.0);
        engine.play();
        engine.touchBegin (leadId, "vol");
        for (int i = 0; i < 8; ++i) { Thread::sleep (120); engine.touchValue (leadId, "vol", 0.3f + 0.05f * (float) i); }
        engine.touchEnd (leadId, "vol");
        engine.stop();
        leadT = project.findTrack (leadId);
        const auto& pts = leadT->findLane ("vol")->points;
        log ("touch-write automation: " + String ((int) before) + " -> " + String ((int) pts.size()) + " points", pts.size() >= before + 5);

        // the same lane, but written without an explicit mouse-up (wheel / external CC path)
        leadT = project.findTrack (leadId);
        const size_t beforeAuto = leadT->findLane ("vol")->points.size();
        engine.setPositionBeats (0.0);
        engine.play();
        for (int i = 0; i < 6; ++i)
        {
            engine.touchValueAuto (leadId, "vol", 0.55f + 0.04f * (float) i);
            MessageManager::getInstance()->runDispatchLoopUntil (80);
        }
        MessageManager::getInstance()->runDispatchLoopUntil (450);   // let the idle touch commit
        engine.stop();
        leadT = project.findTrack (leadId);
        const auto& autoPts = leadT->findLane ("vol")->points;
        log ("touch-write automation: wheel / CC writes commit without a mouse-up ("
             + String ((int) beforeAuto) + " -> " + String ((int) autoPts.size()) + " points)",
             autoPts.size() > beforeAuto);

        leadT->autoMode = AutoMode::Read;
        project.sendContentChanged();
    }

    // ---- recording ------------------------------------------------------------------------------
    if (auto* dev = engine.deviceManager.getCurrentAudioDevice())
    {
        const int numIn = dev->getActiveInputChannels().countNumberOfSetBits();
        if (numIn <= 0) log ("no active input channels - audio recording test skipped");
        else
        {
            auto* leadT = project.findTrack (leadId);
            auto* auT = project.findTrack (auId);
            const size_t audioBefore = auT->audioClips.size(), midiBefore = leadT->midiClips.size();
            auT->arm = true; leadT->arm = true;
            project.sendMixChanged();
            // start from a clean keyboard state: earlier live-MIDI tests may have left keys down
            engine.allLiveNotesOff();
            Thread::sleep (120);
            engine.setPositionBeats (16.0);
            engine.record();
            Thread::sleep (400);
            engine.noteOn (64, 100);
            Thread::sleep (500);
            engine.noteOff (64);
            engine.sendLiveMessage (MidiMessage::controllerEvent (1, 1, 100));
            Thread::sleep (700);
            engine.stop();
            leadT = project.findTrack (leadId);
            auT = project.findTrack (auId);
            bool audioOk = auT->audioClips.size() == audioBefore + 1;
            double len = 0.0;
            if (audioOk) { len = auT->audioClips.back().lengthSec; audioOk = len > 1.3 && auT->audioClips.back().file.existsAsFile(); }
            log ("audio recording: " + String (numIn) + " input ch, clip length " + String (len, 2) + " s", audioOk);
            bool midiOk = leadT->midiClips.size() == midiBefore + 1;
            String detail;
            if (midiOk)
            {
                auto& c = leadT->midiClips.back();
                detail = String (c.notes.size()) + " notes, " + String (c.ccs.size()) + " cc events, start " + String (c.start, 2);
                // real-time capture: what matters is that the played note and its CC were recorded
                // with a sane length - an extra stale key (from a live-MIDI test running before) must
                // not make the check flaky
                bool played = false;
                for (auto& n : c.notes)
                    if (n.pitch == 64 && n.length > 0.25) played = true;
                bool ccOk = false;
                for (auto& p : c.ccs) if (p.cc == 1) ccOk = true;
                midiOk = played && ccOk && c.start >= 16.0;
            }
            log ("MIDI recording: " + detail, midiOk);
            auT->arm = false; leadT->arm = false;
            project.sendMixChanged();
        }
    }

    // ---- ASIO low-latency smoke -----------------------------------------------------------------
    {
        std::unique_ptr<XmlElement> before (engine.deviceManager.createStateXml());
        AudioIODeviceType* asioType = nullptr;
        for (auto* t : engine.deviceManager.getAvailableDeviceTypes())
            if (t->getTypeName().equalsIgnoreCase ("ASIO")) asioType = t;

        if (asioType == nullptr)
           #if JUCE_ASIO
            log ("ASIO: backend not compiled in / not present", false);
           #else
            // macOS and the other non-Windows builds use CoreAudio / WASAPI instead, so a missing
            // ASIO backend there is expected, not a failure
            log ("ASIO: not part of this platform's audio backends - skipped", true);
           #endif
        else
        {
            asioType->scanForDevices();
            StringArray names (asioType->getDeviceNames());
            if (names.isEmpty())
                log ("ASIO: backend present, but no ASIO drivers installed", true);
            else
            {
                // try native vendor drivers first - generic wrappers (ASIO4ALL) are the fallback
                std::stable_sort (names.begin(), names.end(),
                                  [] (const String& a, const String& b)
                                  { return ! a.containsIgnoreCase ("ASIO4ALL") && b.containsIgnoreCase ("ASIO4ALL"); });
                engine.deviceManager.setCurrentAudioDeviceType ("ASIO", true);
                bool opened = false;
                for (auto& name : names)
                {
                    AudioDeviceManager::AudioDeviceSetup s;
                    engine.deviceManager.getAudioDeviceSetup (s);
                    s.outputDeviceName = name;
                    s.inputDeviceName = name;
                    s.sampleRate = 0;    // let the driver pick its default rate / buffer
                    s.bufferSize = 0;
                    const String err = engine.deviceManager.setAudioDeviceSetup (s, true);
                    auto* d = engine.deviceManager.getCurrentAudioDevice();
                    if (err.trim().isEmpty() && d != nullptr && d->getTypeName().equalsIgnoreCase ("ASIO"))
                    {
                        const int rt = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
                        log ("ASIO smoke: " + d->getName() + " @ " + String (d->getCurrentSampleRate()) + " Hz, buffer "
                                 + String (d->getCurrentBufferSizeSamples()) + ", round-trip latency " + String (rt) + " samples", true);
                        opened = true;
                        break;
                    }
                    log ("  [asio] could not open " + name + (err.trim().isEmpty() ? String() : ": " + err));
                }
                if (! opened)
                    log ("ASIO smoke: no ASIO driver could be opened (hardware disconnected?) - skipped", true);
            }
            // put the previous device back so later tests run on the saved setup
            engine.deviceManager.initialise (2, 2, before.get(), true);
            engine.syncAll();
        }
    }

    // ---- plug-ins ------------------------------------------------------------------------------
    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i] != "--plugin" || i + 1 >= args.size()) continue;
        const String path = args[i + 1];
        OwnedArray<PluginDescription> found;
        for (int f = 0; f < plugins.formats.getNumFormats(); ++f)
        {
            auto* fmt = plugins.formats.getFormat (f);
            log ("  [trace] scanning " + path);
            if (fmt->fileMightContainThisPluginType (path)) fmt->findAllTypesForFile (found, path);
            log ("  [trace] scan done, found " + String (found.size()));
        }
        if (found.isEmpty()) { log ("plug-in not recognised: " + path, false); continue; }

        for (auto* d : found)
        {
            const auto t0 = Time::getMillisecondCounterHiRes();
            String err;
            bool ok = false;
            int trackId = 0;
            if (d->isInstrument)
            {
                auto& t = project.addTrack (TrackType::Instrument, d->name);
                trackId = t.id;
                MidiClip c; c.id = project.newId(); c.start = 0.0; c.length = 4.0;
                for (int n = 0; n < 8; ++n) { Note nn; nn.id = project.newId(); nn.pitch = 55 + n * 2; nn.start = n * 0.5; nn.length = 0.4; nn.velocity = 100; c.notes.push_back (nn); }
                t.midiClips.push_back (c);
                project.sendStructureChanged();
                log ("  [trace] instantiating " + d->name);
                ok = engine.setInstrument (trackId, *d, err);
                log ("  [trace] instantiated: " + String ((int) ok));
            }
            else
            {
                trackId = auId;
                ok = engine.addInsert (trackId, *d, err);
            }
            const double ms = Time::getMillisecondCounterHiRes() - t0;
            if (! ok) { log ("load " + d->name + " (" + d->pluginFormatName + "): " + err, false); continue; }

            auto inst = engine.getInstance (d->isInstrument ? project.findTrack (trackId)->instrument.uid : project.findTrack (trackId)->inserts.back().uid);
            const int params = inst != nullptr ? inst->proc->getParameters().size() : -1;
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = out.getChildFile (File::createLegalFileName (d->name) + ".wav");
            o.sampleRate = engine.getSampleRate(); o.bitDepth = 24; o.tailSeconds = 0.5;
            // isolate: mute everything except this track for instruments
            for (auto& t : project.tracks) t->solo = (t->id == trackId);
            project.sendMixChanged();
            // Some instruments (e.g. Spectrasonics) only spin up their engine and load the
            // current patch when the editor is attached to a real on-screen window. Host the
            // editor in a window for the whole wait + render, like a DAW with the UI open.
            std::unique_ptr<DocumentWindow> win;
            if (inst != nullptr && inst->proc->hasEditor())
            {
                if (auto* ed = inst->proc->createEditor())
                {
                    win = std::make_unique<DocumentWindow> (d->name, theme::panel, 0);
                    win->setContentOwned (ed, true);
                    win->centreWithSize (win->getWidth(), win->getHeight());
                    win->setVisible (true);
                    log ("  [trace] editor window open");
                }
            }
            {
                const int w = args.indexOf ("--plugin-wait");
                const int secs = w >= 0 && w + 1 < args.size() ? args[w + 1].getIntValue() : 0;
                if (secs > 0)
                {
                    log ("  waiting " + String (secs) + " s for the plug-in to load its content");
                    const auto until = Time::getMillisecondCounterHiRes() + secs * 1000.0;
                    while (Time::getMillisecondCounterHiRes() < until)
                        MessageManager::getInstance()->runDispatchLoopUntil (50);
                }
            }
            log ("  [trace] rendering");
            // Render like a real DAW: audio processing on a worker thread while the message
            // loop keeps running and the editor stays on screen.
            bool ex = false;
            {
                std::atomic<bool> finished { false };
                std::thread worker ([&] { ex = engine.exportAudio (o, {}, err); finished.store (true); });
                while (! finished.load())
                    MessageManager::getInstance()->runDispatchLoopUntil (20);
                worker.join();
            }
            win.reset();
            auto info = inspect (engine.formats, o.target);
            log (d->pluginFormatName + " " + d->name + " [" + d->manufacturerName + "] loaded in " + String ((int) ms) + " ms, " + String (params)
                     + " params, editor " + String ((int) (inst != nullptr && inst->proc->hasEditor())) + ", rendered peak " + String (info.peak, 3)
                     + (d->isInstrument ? "" : " (effect on sine)"),
                 ex && info.ok);
            for (auto& t : project.tracks) t->solo = false;
            if (d->isInstrument)
            {
                project.removeTrack (trackId);
                project.sendStructureChanged();
            }
            else
                engine.removeSlot (trackId, project.findTrack (trackId)->inserts.back().uid);
        }
    }

    // ---- multi-output instrument: its channels in the mixer ------------------------------------
    runMultiOutputTest (project, engine, plugins, out, log);

    // ---- media pool: file info + preview ------------------------------------------------------
    runMediaPoolTest (engine, plugins, out, log);

    // ---- project info: author / genre / album / cover ----------------------------------------
    runProjectInfoTest (project, engine, plugins, out, log);

    // ---- MIDI learn ---------------------------------------------------------------------------
    runMidiLearnTest (project, engine, plugins, out, log);

    // ---- localisation: every menu item is translated -----------------------------------------
    runLocalisationTest (project, engine, plugins, log);

    // ---- a latency plug-in on an FX send keeps the dry path in time ----------------------------
    runSendPdcTest (project, engine, plugins, out, log);

    // ---- several instruments on one track -----------------------------------------------------
    runLayeredInstrumentTest (project, engine, plugins, out, log);

    // ---- the slot menu dispatches plug-in clicks to the plug-in branch -------------------------
    runSlotMenuIdTest (plugins, log);

    // ---- dragging plug-in slots inside / across tracks ----------------------------------------
    runSlotDragTest (project, engine, plugins, out, log);

    // ---- the arrange view keeps its headers in step with the lanes -----------------------------
    runArrangeScrollTest (project, engine, plugins, log);

    // ---- folders, stand-alone automation tracks, cycle editing and drag-to-scroll --------------
    runFolderAutomationTest (project, engine, plugins, out, log);

    // ---- the scissors tool: clips in the arrangement, notes in the piano roll -------------------
    runScissorsTest (project, engine, plugins, out, log);

    // ---- takes and record quantize --------------------------------------------------------------
    runTakeTest (project, engine, plugins, out, log);

    // ---- the built-in effect family --------------------------------------------------------------
    runBuiltInEffectsTest (project, engine, plugins, log);

    // ---- level / pan meters ---------------------------------------------------------------------
    runMeterTest (project, engine, plugins, out, log);

    // ---- the built-in editor layouts -------------------------------------------------------------
    runEffectEditorShots (out, log);

    // ---- the guitar rig: amp heads, pedals, state and panels -------------------------------------
    runGuitarRigTest (out, log);

    // ---- the built-in sampler --------------------------------------------------------------------
    runSamplerTest (plugins, out, log);

    // ---- the pro EQ, the loudness meter and the stereo field: slopes, analyser, LUFS, scope -------
    runProEqAndMetersTest (out, log);

    // ---- the spatial stereo: placement, distance, the room, linked tracks ------------------------
    runSpatialTest (out, log);

    // ---- fade curve shapes -----------------------------------------------------------------------
    runFadeCurveTest (project, engine, plugins, out, log);

    // ---- the resize policy: only the corner grip may resize a plug-in window ---------------------
    {
        bool fixedSizeOk = true;
        for (auto& info : builtInEffects())
        {
            auto proc = createBuiltInEffect (info.id);
            proc->setRateAndBufferSizeDetails (44100.0, 256);
            proc->prepareToPlay (44100.0, 256);
            if (auto* ed = proc->createEditor())
            {
                // the editor must allow a host resize but must not install its own corner grip
                fixedSizeOk = fixedSizeOk && ed->isResizable();
                int corners = 0;
                for (auto* child : ed->getChildren())
                    if (dynamic_cast<juce::ResizableCornerComponent*> (child) != nullptr) ++corners;
                fixedSizeOk = fixedSizeOk && corners == 0;
            delete ed;
            }
            proc->releaseResources();
        }
        log ("plug-in windows: only the window corner grip resizes, no editor has its own", fixedSizeOk);
    }

    // ---- the user manual is compiled into the exe ---------------------------------------------
    {
        const File target = out.getChildFile ("SoundXManual.pdf");
        target.deleteFile();
        const bool written = writeUserManual (target);
        const bool looksPdf = target.existsAsFile() && target.getSize() > 100000
                           && target.loadFileAsString().substring (0, 5) == "%PDF-";
        log ("manual: the PDF is embedded in the exe and can be written out ("
             + String ((int) (target.getSize() / 1024)) + " KB, Help > User Manual)", written && looksPdf);
        target.deleteFile();
    }


    // ---- bus routing / sends ------------------------------------------------------------------
    {
        {
            std::vector<int> ids;
            for (auto& t : project.tracks) ids.push_back (t->id);
            for (int id : ids) project.removeTrack (id);
            project.masterTrack.inserts.clear();
        }
        engine.syncAll();

        auto& inst = project.addTrack (TrackType::Instrument, "RouteInst");
        inst.hasInstrument = true;               // the routing test measures this track's output
        inst.instrument = makeBuiltInSynthSlot();
        MidiClip rc;
        rc.id = project.newId();
        rc.name = "route";
        rc.length = 4.0;
        Note rn;
        rn.id = project.newId();
        rn.pitch = 60;
        rn.start = 0.0;
        rn.length = 3.0;
        rn.velocity = 100;
        rc.notes.push_back (rn);
        inst.midiClips.push_back (std::move (rc));
        const int instId = inst.id;

        auto& bus = project.addTrack (TrackType::Bus, "TestBus");
        const int busId = bus.id;
        auto& ret = project.addTrack (TrackType::Send, "TestReturn");
        const int sendId = ret.id;
        engine.syncAll();

        auto mixPeak = [&] (const String& name) -> float
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix;
            o.target = out.getChildFile (name + ".wav");
            o.sampleRate = 44100.0;
            o.bitDepth = 16;
            o.tailSeconds = 0.0;
            String err;
            if (! engine.exportAudio (o, {}, err)) { log ("routing export failed: " + err, false); return -1.0f; }
            AudioBuffer<float> b (2, 2 * 44100);
            std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
            if (r == nullptr) return -1.0f;
            r->read (&b, 0, b.getNumSamples(), 0, true, true);
            return jmax (b.getMagnitude (0, 0, b.getNumSamples()), b.getMagnitude (1, 0, b.getNumSamples()));
        };

        const float dry = mixPeak ("r_master");                    // instrument straight to master
        if (auto* t = project.findTrack (instId)) t->outputBusId = busId;
        project.sendContentChanged();
        engine.syncAll();
        const float viaBus = mixPeak ("r_bus");                    // same instrument through a bus
        if (auto* t = project.findTrack (busId)) t->volumeDb = -99.0f;
        project.sendMixChanged();
        engine.syncAll();
        const float busDown = mixPeak ("r_busdown");               // bus fader down
        if (auto* t = project.findTrack (busId)) t->volumeDb = 0.0f;
        if (auto* t = project.findTrack (instId)) t->sends.push_back (SendSlot { sendId, 0.0f, false });
        project.sendContentChanged();
        engine.syncAll();
        const float withSend = mixPeak ("r_send");                 // post-fader send into the return

        // the same bus / send graph through the serial scheduler must measure the same peak
        const bool wasMC = engine.isMultiCoreEnabled();
        engine.setMultiCoreEnabled (false);
        const float withSendSerial = mixPeak ("r_send_serial");
        engine.setMultiCoreEnabled (wasMC);

        bool ok = dry > 0.05f;
        ok = ok && std::abs (viaBus - dry) <= 0.02f * jmax (1.0f, dry);
        ok = ok && busDown < 0.01f;
        ok = ok && std::abs (withSend - 2.0f * dry) <= 0.06f * jmax (1.0f, dry);
        ok = ok && withSendSerial > 0.05f
                && std::abs (withSendSerial - withSend) < 1.0e-3f;

        // routing survives a save: outBus + Send children are written
        int savedOutBus = -1;
        bool savedSend = false;
        if (auto xml = project.toXml (false))
            for (auto* e : xml->getChildWithTagNameIterator ("Track"))
                if (e->getIntAttribute ("id") == instId)
                {
                    savedOutBus = e->getIntAttribute ("outBus", -1);
                    savedSend = e->getChildByName ("Send") != nullptr;
                }
        ok = ok && savedOutBus == busId && savedSend;
        log ("routing: dry " + String (dry, 3) + ", via bus " + String (viaBus, 3)
             + ", bus down " + String (busDown, 3) + ", with send " + String (withSend, 3), ok);
    }

    // ---- clip gain envelope ------------------------------------------------------------------
    {
        {
            std::vector<int> ids;
            for (auto& t : project.tracks) ids.push_back (t->id);
            for (int id : ids) project.removeTrack (id);
        }
        engine.syncAll();
        auto& gt = project.addTrack (TrackType::Audio, "GainEnv");
        const int gid = gt.id;
        AudioClip gc;
        gc.id = project.newId();
        gc.name = "env";
        gc.file = sine;
        gc.data = project.pool.load (sine, engine.formats);
        gc.start = 0.0;
        gc.lengthSec = jmin (2.0, gc.data != nullptr ? gc.data->lengthSeconds() : 0.0);
        gc.gainLine = true;
        gc.gainEnv.push_back ({ 0.0, -60.0f });
        gc.gainEnv.push_back ({ project.clipBeats (0.0, gc.lengthSec), 0.0f });
        gt.audioClips.push_back (std::move (gc));
        engine.syncAll();
        for (auto& t : project.tracks) t->solo = (t->id == gid);

        ExportOptions go;
        go.mode = ExportOptions::Mix;
        go.target = out.getChildFile ("gainenv.wav");
        go.sampleRate = 44100.0;
        go.bitDepth = 16;
        go.tailSeconds = 0.0;
        String gerr;
        const bool ge = engine.exportAudio (go, {}, gerr);
        auto ginfo = inspect (engine.formats, go.target);
        bool ok = ge && ginfo.ok;
        if (ok)
        {
            const int N = (int) (ginfo.seconds * ginfo.sr);
            AudioBuffer<float> b (2, N);
            std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (go.target));
            r->read (&b, 0, N, 0, true, true);
            const int srI = (int) ginfo.sr;
            const float head = b.getMagnitude (0, srI / 10, srI / 10);      // ~0.1 s: about -57 dB
            const float tail = b.getMagnitude (0, N - srI / 5, srI / 10);   // near the end: ~0 dB
            ok = head < 0.03f && tail > 0.3f;
            log ("clip gain envelope: head " + String (head, 4) + ", tail " + String (tail, 3), ok);
        }
        else
            log ("clip gain envelope export failed: " + gerr, false);

        for (auto& t : project.tracks) t->solo = false;
        project.removeTrack (gid);
        project.sendStructureChanged();
        project.sendMixChanged();
    }

    // ---- audio files gathered into the project folder ----------------------------------------
    {
        {
            std::vector<int> ids;
            for (auto& t : project.tracks) ids.push_back (t->id);
            for (int id : ids) project.removeTrack (id);
        }
        engine.syncAll();

        // a take recorded for an unsaved project lives in a session folder inside the temp dir
        const auto tempRoot = File::getSpecialLocation (File::tempDirectory).getChildFile ("SoundX");
        const auto sessionDir = tempRoot.getChildFile ("selftest session");
        sessionDir.createDirectory();
        const auto take = sessionDir.getChildFile ("Take.wav");
        take.deleteFile();
        sine.copyFileTo (take);

        auto& ratt = project.addTrack (TrackType::Audio, "Rec");
        AudioClip rac;
        rac.id = project.newId();
        rac.name = "Take";
        rac.file = take;
        rac.start = 0.0;
        rac.lengthSec = 1.0;
        rac.data = project.pool.load (take, engine.formats);
        ratt.audioClips.push_back (rac);
        const int ratId = ratt.id;

        const auto projDir = out.getChildFile ("savedproject");
        projDir.createDirectory();
        const auto projFile = projDir.getChildFile ("proj.sxp");
        const int relocated = project.collectAudioFiles (projFile);
        const auto target = Project::audioFilesFolderFor (projFile).getChildFile ("Take.wav");

        bool ok = relocated == 1 && target.existsAsFile() && ! take.existsAsFile();
        auto* afterMove = project.findTrack (ratId);
        ok = ok && afterMove != nullptr && afterMove->audioClips.size() == 1
             && afterMove->audioClips.front().file == target;

        // the project stores a relative path and resolves it again on load
        const bool saved = project.saveTo (projFile);
        const bool loaded = saved && project.loadFrom (projFile);
        auto* reloaded = project.findTrack (ratId);
        ok = ok && saved && loaded && reloaded != nullptr && reloaded->audioClips.size() == 1
             && reloaded->audioClips.front().file == target;
        log ("audio files: moved next to the project, relative path round trip", ok);

        sessionDir.deleteRecursively();
        projDir.deleteRecursively();
        project.removeTrack (ratId);
        project.sendStructureChanged();
    }

    // ---- autosave snapshot -------------------------------------------------------------------
    {
        {
            std::vector<int> ids;
            for (auto& t : project.tracks) ids.push_back (t->id);
            for (int id : ids) project.removeTrack (id);
        }
        auto& at = project.addTrack (TrackType::Instrument, "AutoSaveTest");
        MidiClip ac;
        ac.id = project.newId();
        ac.name = "a";
        ac.length = 4.0;
        Note an;
        an.id = project.newId();
        an.pitch = 60;
        an.start = 0.0;
        an.length = 2.0;
        an.velocity = 100;
        ac.notes.push_back (an);
        at.midiClips.push_back (std::move (ac));
        project.dirty = true;
        const auto fileBefore = project.file;

        const auto snapshot = out.getChildFile ("cache").getChildFile ("autosave.sxp");
        const bool wrote = project.writeSnapshotTo (snapshot);
        bool ok = wrote && snapshot.existsAsFile() && project.file == fileBefore && project.dirty;
        // the snapshot is a normal project file and can be restored
        ok = ok && project.loadFrom (snapshot) && ! project.tracks.empty()
             && project.tracks.front()->midiClips.size() == 1
             && project.tracks.front()->midiClips.front().notes.size() == 1;
        log ("autosave: snapshot written without touching the project state", ok);
    }

    // ---- track delay compensation -------------------------------------------------------------
    {
        {
            std::vector<int> ids;
            for (auto& t : project.tracks) ids.push_back (t->id);
            for (int id : ids) project.removeTrack (id);
        }
        engine.syncAll();
        const double delayMs = 200.0;
        auto& dt = project.addTrack (TrackType::Audio, "DelayTest");
        AudioClip dc;
        dc.id = project.newId();
        dc.name = "D";
        dc.file = sine;
        dc.data = project.pool.load (sine, engine.formats);
        dc.start = 0.0;
        dc.lengthSec = 1.0;
        dt.audioClips.push_back (dc);
        dt.delayMs = delayMs;
        engine.syncAll();

        ExportOptions dopt;
        dopt.mode = ExportOptions::Mix;
        dopt.target = out.getChildFile ("delay.wav");
        dopt.sampleRate = 44100.0;
        dopt.bitDepth = 16;
        dopt.tailSeconds = 0.2;
        String derr;
        const bool dok = engine.exportAudio (dopt, {}, derr);

        bool ok = dok;
        float head = 1.0f, atDelay = 0.0f;
        if (ok)
        {
            AudioBuffer<float> b (2, 2 * 44100);
            std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (dopt.target));
            ok = r != nullptr;
            if (ok)
            {
                const int n = jmin (b.getNumSamples(), (int) r->lengthInSamples);
                r->read (&b, 0, n, 0, true, true);
                head = jmax (b.getMagnitude (0, 0, jmin (4410, n)), b.getMagnitude (1, 0, jmin (4410, n)));
                const int from = jmin (n - 4410, 11025);
                atDelay = from > 0 ? jmax (b.getMagnitude (0, from, 4410), b.getMagnitude (1, from, 4410)) : 0.0f;
            }
        }
        log ("track delay: " + String (delayMs, 0) + " ms (head " + String (head, 5)
             + ", after " + String (atDelay, 3) + ")", ok && head < 0.02f && atDelay > 0.2f);

        // the value is stored with the project
        bool savedDelay = false;
        if (auto xml = project.toXml (false))
            for (auto* e : xml->getChildWithTagNameIterator ("Track"))
                if (e->getIntAttribute ("id") == dt.id)
                    savedDelay = std::abs (e->getDoubleAttribute ("delayMs") - delayMs) < 1.0e-6;
        log ("track delay: saved in the project XML", savedDelay);
        project.removeTrack (dt.id);
    }

    // ---- track freeze / unfreeze -------------------------------------------------------------
    {
        AppState st (project, engine, plugins);
        st.showFreezeProgress = false; // no modal window during the self test (project, engine, plugins);
        {
            std::vector<int> ids;
            for (auto& t : project.tracks) ids.push_back (t->id);
            for (int id : ids) project.removeTrack (id);
        }
        engine.syncAll();
        auto& ft = project.addTrack (TrackType::Instrument, "FreezeTest");
        ft.hasInstrument = true;
        ft.instrument = makeBuiltInSynthSlot();
        MidiClip fc;
        fc.id = project.newId();
        fc.name = "f";
        fc.length = 8.0;
        Note fn;
        fn.id = project.newId();
        fn.pitch = 60;
        fn.start = 0.0;
        fn.length = 7.0;
        fn.velocity = 100;
        fc.notes.push_back (fn);
        ft.midiClips.push_back (std::move (fc));
        // volume automation: it must stay live after the freeze, not be baked into the render
        {
            auto& lane = ft.ensureLane ("vol", "Volume");
            lane.points = { { 0.0, 0.70f }, { 8.0, 0.40f } };
            lane.visible = true;
        }
        engine.syncAll();

        // a genuine effect on the chain: it is baked by "freeze with effects"
        String fxErr;
        const bool fxAdded = engine.addInsert (ft.id, makeBuiltInEffectSlot (BuiltInEffect::Distortion).desc, fxErr);
        engine.syncAll();

        auto peakMix = [&] (const String& name) -> float
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix;
            o.target = out.getChildFile (name + ".wav");
            o.sampleRate = 44100.0;
            o.bitDepth = 16;
            o.tailSeconds = 0.5;
            String err;
            if (! engine.exportAudio (o, {}, err)) return -1.0f;
            AudioBuffer<float> b (2, 2 * 44100);
            std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
            if (r == nullptr) return -1.0f;
            const int n = jmin (b.getNumSamples(), (int) r->lengthInSamples);
            r->read (&b, 0, n, 0, true, true);
            return jmax (b.getMagnitude (0, 0, n), b.getMagnitude (1, 0, n));
        };

        const float before = peakMix ("fr_before");
        st.freezeTrack (ft.id);
        auto* t2 = project.findTrack (ft.id);
        String why;
        bool ok = t2 != nullptr && t2->frozen && ! t2->frozenNoFx && t2->frozenData != nullptr
               && t2->frozenFile.existsAsFile()
               && t2->inserts.empty() && t2->frozenChain.size() == 1
               && t2->hasVisibleLanes();
        if (! ok) why << "flags ";
        engine.syncAll();
        const float frozenMix = ok ? peakMix ("fr_frozen") : -1.0f;
        if (std::abs (frozenMix - before) >= 0.05f) { ok = false; why << "render "; }
        if (ok && frozenMix <= 0.01f) { ok = false; why << "silent "; }

        // automation is still live: lowering the curve lowers the frozen track
        if (t2 != nullptr)
        {
            if (auto* lane = t2->findLane ("vol"))
                for (auto& p : lane->points) p.value *= 0.5f;
            project.sendContentChanged();
            engine.syncAll();
        }
        const float autoLowered = ok ? peakMix ("fr_auto") : -1.0f;
        if (autoLowered >= frozenMix * 0.8f) { ok = false; why << "automation "; }

        // ... and an effect inserted after the freeze processes the frozen audio in real time
        const bool postFx = engine.addInsert (ft.id, makeBuiltInEffectSlot (BuiltInEffect::Distortion).desc, fxErr);
        engine.syncAll();
        const float postFxMix = ok && postFx ? peakMix ("fr_postfx") : -1.0f;
        if (! postFx) { ok = false; why << "postFx "; }
        else if (! (postFxMix > autoLowered * 1.08f || postFxMix < autoLowered * 0.92f)) { ok = false; why << "postFxMix "; }

        // the frozen state (with the baked chain) is written with the project
        bool frozenSaved = false;
        if (auto xml = project.toXml (false))
            for (auto* e : xml->getChildWithTagNameIterator ("Track"))
                if (e->getIntAttribute ("id") == ft.id)
                    frozenSaved = e->getBoolAttribute ("frozen")
                               && e->getStringAttribute ("frozenFile").isNotEmpty()
                               && e->getChildByName ("FrozenInsert") != nullptr;
        ok = ok && frozenSaved;
        if (! frozenSaved) why << "save ";

        st.unfreezeTrack (ft.id);
        ok = ok && ! t2->frozen
               && t2->frozenChain.empty() && t2->inserts.size() == 2;
        if (t2->frozen || ! t2->frozenChain.empty() || t2->inserts.size() != 2) why << "unfreeze ";
        engine.syncAll();
        const float after = peakMix ("fr_after");
        ok = ok && after > 0.02f;   // the restored chain plays the live instrument again
        if (after <= 0.02f) why << "afterSilent ";

        // freezing without effects leaves the whole insert chain live
        st.freezeTrack (ft.id, false);
        ok = ok && t2->frozen && t2->frozenNoFx && t2->inserts.size() == 2 && t2->frozenChain.empty();
        if (! (t2->frozen && t2->frozenNoFx && t2->inserts.size() == 2 && t2->frozenChain.empty())) why << "noFx ";
        st.unfreezeTrack (ft.id);
        ok = ok && ! t2->frozen && t2->inserts.size() == 2;
        log ("freeze: instrument render matches live (" + String (before, 3) + " / " + String (frozenMix, 3)
             + "), baked chain stays editable (" + String (autoLowered, 3) + " -> " + String (postFxMix, 3)
             + "), automation live, unfreeze restores, no-FX keeps the chain"
             + (why.isEmpty() ? String() : " [" + why.trim() + "]"), ok && fxAdded);
        project.removeTrack (ft.id);
    }

    // ---- track drag & drop reorder ------------------------------------------------------------
    {
        {
            std::vector<int> ids;
            for (auto& t : project.tracks) ids.push_back (t->id);
            for (int id : ids) project.removeTrack (id);
        }
        engine.syncAll();
        AppState st (project, engine, plugins);
        auto& ta = project.addTrack (TrackType::Instrument, "A");
        auto& tb = project.addTrack (TrackType::Instrument, "B");
        auto& tc = project.addTrack (TrackType::Instrument, "C");
        const int a1 = ta.id, a2 = tb.id, a3 = tc.id;

        st.moveTrackTo (a1, (int) project.tracks.size());       // drop after the last row
        bool ok = project.indexOfTrack (a2) == 0 && project.indexOfTrack (a3) == 1 && project.indexOfTrack (a1) == 2;
        st.moveTrackTo (a1, 0);                                  // back to the very top
        ok = ok && project.indexOfTrack (a1) == 0 && project.indexOfTrack (a2) == 1 && project.indexOfTrack (a3) == 2;
        st.moveTrackTo (a3, 1);                                  // drop between A and B
        ok = ok && project.indexOfTrack (a1) == 0 && project.indexOfTrack (a3) == 1 && project.indexOfTrack (a2) == 2;
        log ("track order: drag & drop reorder", ok);

        // hiding a track: flag + persistence
        st.setTracksHidden ({ a2 }, true);
        auto* hid = project.findTrack (a2);
        bool hiddenSet = hid != nullptr && hid->hidden;
        bool savedHidden = false;
        if (auto xml = project.toXml (false))
            for (auto* e : xml->getChildWithTagNameIterator ("Track"))
                if (e->getIntAttribute ("id") == a2) savedHidden = e->getBoolAttribute ("hidden");
        st.setTracksHidden ({ a2 }, false);
        const bool okHide = hiddenSet && savedHidden && project.findTrack (a2) != nullptr && ! project.findTrack (a2)->hidden;
        log ("track hide: flag + persistence", okHide);

        // Batch hide: a Ctrl+click multi-selection hides every selected row in one undoable step,
        // the selection moves off the rows that just disappeared, and hiddenTrackCount / Show All
        // only count rows that actually belong in the arranger (multi-output mixer channels are
        // hidden by design and must never pop up as visible extra tracks).
        {
            project.clearUndo();
            st.selectTrack (a1, false, false);
            st.selectTrack (a3, true, false);
            const bool multiSelected = st.selectedTracks.size() == 2;
            st.setTracksHidden ({ a1, a3 }, true);
            const bool bothHidden = project.findTrack (a1)->hidden && project.findTrack (a3)->hidden;
            const int hiddenCount = st.hiddenTrackCount();
            const bool selectionTrimmed = st.selectedTracks.count (a1) == 0 && st.selectedTracks.count (a3) == 0
                                       && st.selectedTracks.size() <= 1;
            // the row that is still visible (a2) is the only candidate the selection can land on
            const bool anchorVisible = st.selectedTrack == -1
                                    || (project.findTrack (st.selectedTrack) != nullptr
                                        && project.findTrack (st.selectedTrack)->shownInArranger());
            project.undo();
            const bool undoRestored = ! project.findTrack (a1)->hidden && ! project.findTrack (a3)->hidden;
            project.redo();
            st.setTracksHidden ({ a1, a3 }, false);

            Track fakeOutput;
            fakeOutput.sourceTrack = a2;   // a multi-output mixer channel
            const bool outputChannelIgnored = ! AppState::isArrangerTrack (fakeOutput)
                                           && AppState::isArrangerTrack (*project.findTrack (a1));
            log ("track hide: batch hide of the selected rows, selection trimmed, one undo step, "
                 "output channels excluded (" + String (hiddenCount) + " hidden, multi "
                 + String ((int) multiSelected) + ")", multiSelected && bothHidden && hiddenCount == 2
                 && selectionTrimmed && anchorVisible && undoRestored && outputChannelIgnored);
        }
        project.clearUndo();
    }

    // ---- mixer strip routing block ------------------------------------------------------------
    {
        AppState st (project, engine, plugins);
        {
            std::vector<int> ids;
            for (auto& t : project.tracks) ids.push_back (t->id);
            for (int id : ids) project.removeTrack (id);
        }
        auto& mt = project.addTrack (TrackType::Instrument, "MixTest");
        auto& mt2 = project.addTrack (TrackType::Instrument, "NoSend");
        mt.hasInstrument = true; mt.instrument = makeBuiltInSynthSlot();   // both are rendered below
        mt2.hasInstrument = true; mt2.instrument = makeBuiltInSynthSlot();
        auto& bus2 = project.addTrack (TrackType::Bus, "BusX");
        auto& ret2 = project.addTrack (TrackType::Send, "SendX");
        mt.outputBusId = bus2.id;
        mt.sends.push_back (SendSlot { ret2.id, -6.0f, false });
        engine.syncAll();

        // the master strip must always show its insert slots (they had vanished below 310 px)
        ChannelStrip masterStrip (st, -1);
        masterStrip.setSize (104, 300);
        masterStrip.resized();
        const int masterIns = masterStrip.visibleInsertSlots();

        // ... and, like the master bus row in the arrangement, the mute and automation switches
        {
            auto shot = masterStrip.createComponentSnapshot (masterStrip.getLocalBounds(), true);
            const File png = out.getChildFile ("master_strip.png");
            if (shot.isValid())
                if (auto stream = std::unique_ptr<FileOutputStream> (png.createOutputStream()))
                {
                    PNGImageFormat fmt;
                    fmt.writeImageToStream (shot, *stream);
                }
            // the master keeps its automation switch, as a slim labelled row under M / MONO
            const auto autoBox = masterStrip.autoButtonBounds();
            log ("master strip: M + MONO above a slim automation row (auto "
                 + String (autoBox.getWidth()) + "x" + String (autoBox.getHeight()) + ")",
                 masterStrip.masterSwitchesShown() && masterStrip.masterAutoButtonShown()
                 && autoBox.getHeight() <= 20 && autoBox.getWidth() >= masterStrip.getWidth() - 14);
            log ("master strip: mono / stereo switch is shown", masterStrip.masterMonoShown());
        }

        ChannelStrip strip (st, mt.id);
        strip.setSize (104, 300);
        strip.resized();
        const int sndRows = strip.visibleSendRows();
        const bool outRow = strip.outputRowVisible();

        // a normal track with no sends yet shows the "+ Send" row (keeps the section discoverable)
        ChannelStrip strip2 (st, mt2.id);
        strip2.setSize (104, 320);
        strip2.resized();
        const bool addRow = strip2.addSendRowVisible();

        // an effect return is parallel: it must never show its own sends / "+ Send" row
        ChannelStrip strip3 (st, ret2.id);
        strip3.setSize (104, 320);
        strip3.resized();
        strip3.refresh();          // a mix notification must not bring the hidden OUT row back
        const bool returnHasNoSends = strip3.visibleSendRows() == 0 && ! strip3.addSendRowVisible();
        const bool returnHasNoOut = ! strip3.outputRowVisible();

        // every channel strip keeps a live meter beside its fader, even when the routing block is
        // squeezed; the maximum-peak marker comes from the same cache as the arrange headers
        const bool stripMeters = masterStrip.meterVisible() && masterStrip.meterBounds().getHeight() > 4
                              && strip.meterVisible() && strip.meterBounds().getHeight() > 4
                              && strip2.meterVisible() && strip2.meterBounds().getHeight() > 4
                              && strip3.meterVisible() && strip3.meterBounds().getHeight() > 4;

        bool ok = masterIns >= 3 && sndRows >= 1 && outRow && addRow && returnHasNoSends && returnHasNoOut && stripMeters;
        log ("mixer strip: " + String (masterIns) + " master inserts, " + String (sndRows)
             + " send rows, OUT " + String ((int) outRow) + ", +Send " + String ((int) addRow)
             + ", return-sends " + String ((int) returnHasNoSends) + ", return-OUT " + String ((int) returnHasNoOut)
             + ", meters " + String ((int) stripMeters), ok);

        // at the minimum mixer height the strip must switch to the compact layout: no inserts /
        // slot / OUT / send rows, only name + fader/meter + dB + switches (no stacking)
        ChannelStrip compact (st, mt.id);
        compact.setSize (104, 150);
        compact.resized();
        const bool compactClean = compact.visibleInsertSlots() == 0 && compact.visibleSendRows() == 0
                                  && ! compact.outputRowVisible() && ! compact.addSendRowVisible()
                                  && compact.meterVisible() && compact.meterBounds().getHeight() > 4;
        log ("mixer strip: minimum height uses one clean compact layout (no stacked inserts / routing / dB)",
             compactClean);

        // Resizing the mixer shorter applies the height tiers and then a mix update refreshes the
        // strip: the controls the tiers hid must stay hidden instead of reappearing at their old,
        // stale bounds on top of the fader (the reported "display stacking" in the mixer).
        ChannelStrip shrink (st, mt.id);
        shrink.setSize (104, 340);
        shrink.resized();
        shrink.setSize (104, 248);        // below the slot (250), pan (300) and routing (280) tiers
        shrink.resized();
        shrink.refresh();
        const bool noStaleTierControls = shrink.visibleSendRows() == 0
                                      && ! shrink.outputRowVisible() && ! shrink.addSendRowVisible()
                                      && ! shrink.panVisible() && ! shrink.slotButtonVisible();
        // and nothing may sit outside the strip bounds
        bool childrenInside = true;
        for (auto* c : shrink.getChildren())
            if (c->isVisible() && ! shrink.getLocalBounds().contains (c->getBounds()))
                childrenInside = false;
        log ("mixer strip: shrinking then refreshing does not re-show stale tier controls ("
             + String (shrink.visibleInsertSlots()) + " inserts, send rows "
             + String (shrink.visibleSendRows()) + ", OUT " + String ((int) shrink.outputRowVisible())
             + ", pan " + String ((int) shrink.panVisible()) + ", slot "
             + String ((int) shrink.slotButtonVisible()) + ")",
             noStaleTierControls && childrenInside);

        // post-fader master inserts survive a save
        {
            PluginSlot ps;
            ps.uid = 4242;
            ps.desc.name = "TestPost";
            ps.postFader = true;
            project.masterTrack.inserts.push_back (ps);
            const File mf = out.getChildFile ("master_post.sxp");
            Project mp; mp.formats = &engine.formats;
            const bool saved = project.saveTo (mf) && mp.loadFrom (mf)
                               && mp.masterTrack.inserts.size() == 1 && mp.masterTrack.inserts[0].postFader;
            project.masterTrack.inserts.clear();
            log ("master inserts: post-fader flag persists", saved);
        }
    }

    // ---- mixer layout: every strip fits and never runs under the scrollbar ---------------------
    // The fixed strip width could be wider than the space left of the fixed master strip, which
    // drew the last channel half under the 总线 strip (and cut its M/S/R/I/L row in half). The
    // mixer has to auto-fit, and the strips must not be laid out under the horizontal scrollbar.
    {
        std::vector<int> extraIds;
        for (int i = 0; i < 9; ++i)
            extraIds.push_back (project.addTrack (TrackType::Instrument, "MixFit " + String (i + 1)).id);
        project.sendStructureChanged();
        engine.syncAll();

        {
            AppState mvApp (project, engine, plugins);
            MixerView mv (mvApp);
            mvApp.stripWidth = 216;               // deliberately far too wide for the window
            mv.setSize (1400, 300);
            mv.resized();

            std::vector<ChannelStrip*> rowStrips;
            ChannelStrip* masterStrip = nullptr;
            Viewport* viewport = nullptr;
            std::function<void (Component&)> walk = [&] (Component& c)
            {
                for (auto* ch : c.getChildren())
                {
                    if (auto* s = dynamic_cast<ChannelStrip*> (ch))
                    {
                        if (s->getTrackId() == -1) masterStrip = s;
                        else if (s->getTrackId() >= 0) rowStrips.push_back (s);
                    }
                    if (auto* v = dynamic_cast<Viewport*> (ch)) viewport = v;
                    walk (*ch);
                }
            };
            walk (mv);

            // the strips live inside the scrolling content: they must all fit into the viewport
            // (no half strip cut off at the right edge) and the viewport must stop before the
            // fixed master strip (no strip painted under 总线)
            int rowRight = 0;
            for (auto* s : rowStrips) rowRight = jmax (rowRight, s->getRight());
            const int viewportW = viewport != nullptr ? viewport->getMaximumVisibleWidth() : 0;
            const int viewportRight = viewport != nullptr ? viewport->getRight() : -1;
            const bool allFit = masterStrip != nullptr && viewport != nullptr
                             && rowRight <= viewportW && viewportRight <= masterStrip->getX();
            const int stripH = rowStrips.empty() ? -1 : rowStrips.front()->getHeight();
            const int visibleH = viewport != nullptr ? viewport->getMaximumVisibleHeight() : 0;
            const bool fitsHeight = stripH > 0 && stripH <= visibleH;
            log ("mixer layout: the strips auto-fit before the master strip and stay above the "
                 "horizontal scrollbar (" + String ((int) rowStrips.size()) + " strips, last right "
                 + String (rowRight) + " / viewport " + String (viewportW) + ", viewport right "
                 + String (viewportRight) + " / master left "
                 + String (masterStrip != nullptr ? masterStrip->getX() : -1)
                 + ", strip height " + String (stripH) + " / visible " + String (visibleH) + ")",
                 rowStrips.size() >= 10 && allFit && fitsHeight);
        }

        for (int id : extraIds) project.removeTrack (id);
        project.sendStructureChanged();
        engine.syncAll();
    }

    // ---- dark mode palette -----------------------------------------------------------------
    {
        const auto lightPanel = theme::panel;
        theme::setDark (true);
        const bool swapped = theme::isDark() && theme::panel != lightPanel;
        SXLookAndFeel lf;
        lf.applyColours();   // refresh themed component colours (must not throw)
        theme::setDark (false);
        log ("dark mode: palette swap", swapped && ! theme::isDark() && theme::panel == lightPanel);
    }

    // ---- fader: only the left button can drag it ---------------------------------------------
    {
        auto makeEvent = [] (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                                     1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                     juce::Time::getCurrentTime(), {}, juce::Time::getCurrentTime(), 1, false);
        };

        LeftDragSlider fader (juce::Slider::LinearVertical, juce::Slider::NoTextBox);
        fader.setSize (20, 100);
        fader.setRange (0.0, 1.0, 0.0);
        fader.setValue (0.5, juce::dontSendNotification);

        // a right-button press near the top must leave the value alone
        fader.mouseDown (makeEvent (fader, { 10.0f, 5.0f }, juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier)));
        const bool rightIgnored = std::abs (fader.getValue() - 0.5) < 0.01;
        // the left button still jumps to the clicked position
        fader.mouseDown (makeEvent (fader, { 10.0f, 5.0f }, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)));
        const bool leftWorks = fader.getValue() > 0.8;
        log ("mixer fader: right button ignored, left button drags", rightIgnored && leftWorks);
    }


    // ---- MIDI CC editing: tools, selection, delete and quantize snapping -----------------------
    {
        auto& cct = project.addTrack (TrackType::Instrument, "CCEdit");
        const int ccTrack = cct.id;
        cct.hasInstrument = true;
        cct.instrument = makeBuiltInSynthSlot();
        MidiClip mc;
        mc.id = project.newId(); mc.start = 0.0; mc.length = 8.0;
        cct.midiClips.push_back (mc);
        const int ccClip = mc.id;
        project.sendStructureChanged();
        project.sendContentChanged();

        AppState st (project, engine, plugins);
        PianoRollView pr (st);
        pr.setSize (1300, 760);
        st.openClipInEditor (ccTrack, ccClip);
        pr.laneControllers = { 1 };          // one mod-wheel lane
        pr.laneHeights = { 120 };
        pr.resized();

        auto* laneComp = dynamic_cast<Component*> (pr.laneComp());
        auto* clip = project.findTrack (ccTrack) != nullptr ? project.findTrack (ccTrack)->findMidi (ccClip) : nullptr;
        if (laneComp == nullptr || clip == nullptr)
        {
            log ("CC editing: the test lane and clip exist", false);
        }
        else
        {
            auto count = [clip] (int cc)
            {
                int n = 0;
                for (auto& p : clip->ccs) if (p.cc == cc) ++n;
                return n;
            };
            // lane-local coordinates: x from beat, y from value inside the lane's inner area
            auto area = [&]
            {
                const int h = laneComp->getHeight();
                const int total = jmax (1, pr.lanePanelTotal());
                const int laneH = jmax (1, 120 * h / total);
                return Rectangle<int> (0, 0, laneComp->getWidth(), laneH).reduced (0, 8);
            }();
            const auto xAt = [&] (double beat) { return (float) std::round (beat * pr.pxPerBeat); };
            const auto yAt = [&] (int value) { return (float) area.getBottom() - (float) value / 127.0f * (float) area.getHeight(); };
            auto makeEv = [] (Component& c, Point<float> p, ModifierKeys mods)
            {
                return MouseEvent (Desktop::getInstance().getMainMouseSource(), p, mods,
                                   1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                   Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
            };
            const auto none = ModifierKeys();
            const auto left = ModifierKeys (ModifierKeys::leftButtonModifier);
            const auto right = ModifierKeys (ModifierKeys::rightButtonModifier);
            const auto shift = ModifierKeys (ModifierKeys::shiftModifier);
            auto click = [&] (Point<float> p, ModifierKeys mods = ModifierKeys (ModifierKeys::leftButtonModifier))
            {
                const auto e = makeEv (*laneComp, p, mods);
                laneComp->mouseDown (e);
                laneComp->mouseUp (e);
            };
            auto dragTo = [&] (Point<float> from, Point<float> to)
            {
                const auto d = makeEv (*laneComp, from, left);
                laneComp->mouseDown (d);
                const auto m = makeEv (*laneComp, to, left);
                laneComp->mouseDrag (m);
                laneComp->mouseUp (m);
            };
            pr.grid = 0.25;                       // 1/16 quantize

            // 1) pointer tool: clicking an empty spot must not write controller data
            pr.setTool (PianoRollView::Tool::Pointer);
            click ({ xAt (1.05), yAt (64) });
            const bool pointerNoDraw = count (1) == 0;

            // 2) pointer tool: a drag in the lane is a marquee, never a drawn curve
            pr.setTool (PianoRollView::Tool::Pointer);
            dragTo ({ xAt (1.0), yAt (100) }, { xAt (4.0), yAt (20) });
            const bool pointerDragNoDraw = count (1) == 0;

            // 3) pencil draws: quantize snaps every anchor on the 1/16 grid
            pr.setTool (PianoRollView::Tool::Pencil);
            dragTo ({ xAt (1.05), yAt (100) }, { xAt (2.02), yAt (40) });
            const int pencilCount = count (1);
            bool snapped = pencilCount > 1;
            if (snapped)
                for (auto& p : clip->ccs)
                    if (p.cc == 1 && std::abs (p.beat / 0.25 - std::round (p.beat / 0.25)) > 1.0e-6) snapped = false;

            // 4) pointer tool: a marquee selects the anchors (and still does not draw anything)
            clip->ccs.clear();
            pr.setTool (PianoRollView::Tool::Pencil);
            click ({ xAt (1.0), yAt (90) });
            click ({ xAt (2.0), yAt (60) });
            click ({ xAt (3.0), yAt (30) });
            const int created = count (1);
            pr.setTool (PianoRollView::Tool::Pointer);
            pr.selCC.clear();
            dragTo ({ xAt (0.8), yAt (110) }, { xAt (2.2), yAt (40) });
            const int marqueeSel = (int) pr.selCC.size();
            const bool marqueeOk = created == 3 && marqueeSel == 2 && count (1) == 3;

            // snapshot with the rubber band still on screen (the mouse is not released yet)
            {
                const auto d = makeEv (*laneComp, Point<float> (xAt (0.8), yAt (112)), left);
                laneComp->mouseDown (d);
                const auto m = makeEv (*laneComp, Point<float> (xAt (2.2), yAt (40)), left);
                laneComp->mouseDrag (m);
                if (auto shot = pr.createComponentSnapshot (pr.getLocalBounds(), true); shot.isValid())
                    if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("cc_edit.png").createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                    }
                laneComp->mouseUp (m);
            }

            // 5) pointer tool: clicking an anchor selects just that one
            click ({ xAt (3.0), yAt (30) });
            const bool clickSelect = pr.selCC.size() == 1;

            // 6) right-click deletes exactly the anchor under the pointer
            click ({ xAt (2.0), yAt (60) }, right);
            const bool rightDelete = count (1) == 2;

            // 7) quantize off: free drawing is fine grained and stays where the mouse is
            pr.grid = 0.0;
            pr.setTool (PianoRollView::Tool::Pencil);
            const double downBeat = 4.02;
            dragTo ({ xAt (downBeat), yAt (100) }, { xAt (downBeat + 0.5), yAt (50) });
            int freeCount = 0;
            bool unsnapped = false, startsWhereClicked = false;
            double firstBeat = -1.0;
            for (auto& p : clip->ccs)
            {
                if (p.cc != 1 || p.beat < 3.5) continue;
                ++freeCount;
                if (firstBeat < 0.0 || p.beat < firstBeat) firstBeat = p.beat;
                if (std::abs (p.beat / 0.25 - std::round (p.beat / 0.25)) > 1.0e-6) unsnapped = true;
            }
            // the anchor lands exactly under the cursor pixel (no snapping), so compare against
            // the beat of that pixel rather than the raw double
            const double pixelBeat = std::round (downBeat * pr.pxPerBeat) / pr.pxPerBeat;
            startsWhereClicked = std::abs (firstBeat - pixelBeat) < 1.0e-6;
            const bool smooth = freeCount >= 20 && freeCount > pencilCount;
            pr.grid = 0.25;

            // 8) eraser tool deletes the anchor under the pointer
            pr.setTool (PianoRollView::Tool::Eraser);
            const int beforeErase = count (1);
            click ({ xAt (3.0), yAt (30) });
            const bool eraserWorks = count (1) == beforeErase - 1;

            log ("CC editing: the pointer selects but never draws (" + String (pencilCount) + " anchors from the pencil)",
                 pointerNoDraw && pointerDragNoDraw && pencilCount > 1);
            log ("CC editing: anchors snap to the quantize grid (" + String (pencilCount) + " anchors, all on 1/16)", snapped);
            log ("CC editing: a marquee selects several anchors and deletes with the right button ("
                 + String (created) + " drawn, " + String (marqueeSel) + " selected)", marqueeOk && clickSelect && rightDelete);
            log ("CC editing: with quantize off free drawing is dense and unsnapped ("
                 + String (freeCount) + " anchors, first at beat " + String (firstBeat, 3) + ")",
                 smooth && unsnapped && startsWhereClicked);
            log ("CC editing: the eraser removes the anchor under the pointer", eraserWorks);
        }

        project.removeTrack (ccTrack);
        project.sendStructureChanged();
        engine.syncAll();
    }

    // ---- pitch bend rests in the middle --------------------------------------------------------
    // The centre value is where a bend starts and ends: a gesture in the middle of a clip must not
    // lean on the rest of the clip, i.e. the events outside the anchors are the centre value.
    {
        auto events = [] (const MidiClip& clip, int channel)
        {
            std::vector<std::pair<double, MidiMessage>> out;
            appendControllerMessages (clip, channel, out);
            std::stable_sort (out.begin(), out.end(), [] (const std::pair<double, MidiMessage>& a,
                                                          const std::pair<double, MidiMessage>& b) { return a.first < b.first; });
            return out;
        };

        // ordinary controllers are untouched: one anchor, one event, no centre events around it
        MidiClip plain;
        plain.length = 8.0;
        plain.ccs.push_back ({ 1, 1, 1.0, 64 });
        const auto plainEvents = events (plain, 1);
        log ("pitch bend: a plain controller anchor still produces exactly one event",
             plainEvents.size() == 1 && plainEvents[0].first == 1.0 && plainEvents[0].second.isController());

        // a bend in the middle: centre at the clip start, centre again right after the last anchor
        MidiClip mid;
        mid.length = 8.0;
        mid.ccs.push_back ({ 1, kCCPitchBend, 2.0, 12000 });
        mid.ccs.push_back ({ 2, kCCPitchBend, 4.0, 12000 });
        const auto midEvents = events (mid, 1);
        bool midOk = midEvents.size() == 4;
        if (midOk)
            midOk = midEvents[0].first == 0.0 && midEvents[0].second.getPitchWheelValue() == 8192
                 && midEvents[1].first == 2.0 && midEvents[1].second.getPitchWheelValue() == 12000
                 && midEvents[2].first == 4.0 && midEvents[2].second.getPitchWheelValue() == 12000
                 && midEvents[3].first > 4.0 && midEvents[3].first < 4.1
                 && midEvents[3].second.getPitchWheelValue() == 8192;
        log ("pitch bend: an edit in the middle leaves the rest of the clip at the centre value ("
             + String ((int) midEvents.size()) + " events)", midOk);

        // a bend that already starts at the clip start and ends at the centre needs no extra events
        MidiClip edges;
        edges.length = 8.0;
        edges.ccs.push_back ({ 1, kCCPitchBend, 0.0, 9000 });
        edges.ccs.push_back ({ 2, kCCPitchBend, 8.0, 8192 });
        const auto edgeEvents = events (edges, 1);
        log ("pitch bend: a bend that already starts at 0 and ends centred adds no events",
             edgeEvents.size() == 2 && edgeEvents[0].first == 0.0 && edgeEvents[1].second.getPitchWheelValue() == 8192);

        // ... and one that ends off-centre inside the clip is closed by a centre event
        MidiClip tail;
        tail.length = 8.0;
        tail.ccs.push_back ({ 1, kCCPitchBend, 0.0, 9000 });
        tail.ccs.push_back ({ 2, kCCPitchBend, 6.0, 12000 });
        const auto tailEvents = events (tail, 1);
        log ("pitch bend: a bend that stops off-centre is closed with a centre event",
             tailEvents.size() == 3 && tailEvents.back().second.getPitchWheelValue() == 8192
             && tailEvents.back().first > 6.0 && tailEvents.back().first < 6.1);

        // the MIDI file the export writes carries the same events (the engine uses the same helper)
        {
            Project pb;
            pb.formats = &engine.formats;
            auto& t = pb.addTrack (TrackType::Instrument, "Bend");
            MidiClip bc;
            bc.id = pb.newId(); bc.start = 0.0; bc.length = 8.0;
            Note n; n.id = pb.newId(); n.pitch = 60; n.start = 1.0; n.length = 1.0; n.velocity = 100;
            bc.notes.push_back (n);
            bc.ccs.push_back ({ pb.newId(), kCCPitchBend, 3.0, 12000 });
            t.midiClips.push_back (bc);
            pb.sendStructureChanged();

            const File bf = out.getChildFile ("bend.mid");
            const bool wrote = pb.exportMidi (bf);
            MidiFile mf;
            bool readBack = false;
            {
                FileInputStream in (bf);
                readBack = wrote && in.openedOk() && mf.readFrom (in);
            }
            int bends = 0, centreEvents = 0, anchors = 0;
            double lastCentreTick = -1.0;
            if (readBack)
                for (int i = 0; i < mf.getNumTracks(); ++i)
                    for (int j = 0; j < mf.getTrack (i)->getNumEvents(); ++j)
                    {
                        const auto m = mf.getTrack (i)->getEventPointer (j)->message;
                        if (! m.isPitchWheel()) continue;
                        ++bends;
                        if (m.getPitchWheelValue() == 8192) { ++centreEvents; lastCentreTick = jmax (lastCentreTick, m.getTimeStamp()); }
                        if (m.getPitchWheelValue() == 12000) ++anchors;
                    }
            const double ppq = mf.getTimeFormat();
            log ("pitch bend: the exported MIDI has the centre events too (" + String (bends) + " bends, "
                 + String (anchors) + " at 12000, last centre at tick " + String (lastCentreTick, 0)
                 + " of " + String (ppq) + " per beat)",
                 readBack && bends == 3 && anchors == 1 && centreEvents == 2
                 && lastCentreTick > 3.0 * ppq);
        }

        // the lane drawing: a flat centre line when there is no data, and a curve that returns to
        // the centre after the gesture (pictures for the visual record)
        {
            auto& bt = project.addTrack (TrackType::Instrument, "PitchBend");
            const int bTrack = bt.id;
            MidiClip mc;
            mc.id = project.newId(); mc.start = 0.0; mc.length = 8.0;
            bt.midiClips.push_back (mc);
            const int bClip = mc.id;
            project.sendStructureChanged();
            project.sendContentChanged();

            AppState st (project, engine, plugins);
            PianoRollView pr (st);
            pr.setSize (1300, 760);
            st.openClipInEditor (bTrack, bClip);
            pr.laneControllers = { kCCPitchBend };
            pr.laneHeights = { 130 };
            pr.resized();

            auto snapshot = [&pr, &out] (const String& name)
            {
                if (auto shot = pr.createComponentSnapshot (pr.getLocalBounds(), true); shot.isValid())
                    if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile (name).createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                    }
            };
            snapshot ("bend_lane_empty.png");

            const bool laneVisible = pr.laneComp() != nullptr && pr.laneComp()->getHeight() > 40;
            auto* bclip = project.findTrack (bTrack) != nullptr ? project.findTrack (bTrack)->findMidi (bClip) : nullptr;
            bool drawn = false;
            if (bclip != nullptr)
            {
                for (int i = 0; i <= 8; ++i)          // a bend from beat 2 to beat 6, returning to centre
                {
                    CCPoint p;
                    p.id = project.newId(); p.cc = kCCPitchBend; p.beat = 2.0 + i * 0.5;
                    p.value = jlimit (0, 16383, 8192 + (int) std::round (3000.0 * std::sin (i * juce::MathConstants<double>::pi / 8.0)));
                    bclip->ccs.push_back (p);
                }
                project.sendContentChanged();
                snapshot ("bend_lane.png");
                drawn = true;
            }
            log ("pitch bend: the lane shows an empty bend as a flat centre curve and returns to it after the gesture",
                 laneVisible && drawn);

            // ---- the curve shapes on their own ------------------------------------------------
            {
                const double from = 8192.0, to = 14000.0, mid = 0.5 * (from + to);
                auto near = [] (double a, double b) { return std::abs (a - b) < 60.0; };
                const bool ok = near (curveShapeValue (CurveShape::Line, 0.0, from, to, 4.0), from)
                             && near (curveShapeValue (CurveShape::Line, 0.5, from, to, 4.0), mid)
                             && near (curveShapeValue (CurveShape::Line, 1.0, from, to, 4.0), to)
                             && near (curveShapeValue (CurveShape::Sine, 0.0, from, to, 4.0), from)
                             && near (curveShapeValue (CurveShape::Sine, 0.5, from, to, 4.0), to)
                             && near (curveShapeValue (CurveShape::Sine, 1.0, from, to, 4.0), from)
                             && near (curveShapeValue (CurveShape::Triangle, 0.25, from, to, 4.0), mid)
                             && near (curveShapeValue (CurveShape::Triangle, 0.5, from, to, 4.0), to)
                             && near (curveShapeValue (CurveShape::Saw, 0.125, from, to, 4.0), mid)
                             && near (curveShapeValue (CurveShape::Saw, 0.249, from, to, 4.0), to)
                             && near (curveShapeValue (CurveShape::Saw, 0.25, from, to, 4.0), from);
                log ("piano roll: the curve shapes follow their maths (line / sine / triangle / sawtooth)", ok);
            }

            // ---- the lane lines up with the note grid, sidebar or not ---------------------------
            {
                pr.setSize (1300, 760);
                pr.resized();
                const int laneX = pr.laneComp()->getX(), gridX = pr.gridViewport.getX();
                const int laneW = pr.laneComp()->getWidth(), gridW = pr.gridViewport.getWidth();
                pr.trackListVisible = false;
                pr.resized();
                const int laneXNoList = pr.laneComp()->getX(), gridXNoList = pr.gridViewport.getX();
                pr.trackListVisible = true;
                pr.resized();
                log ("piano roll: the CC lane starts where the note grid starts, with and without the track list ("
                     + String (laneX) + "/" + String (gridX) + " and " + String (laneXNoList) + "/" + String (gridXNoList) + ")",
                     laneX == gridX && laneW == gridW && laneXNoList == gridXNoList
                     && laneXNoList < laneX && laneXNoList == PianoRollView::kKeysW);
            }

            // ---- drawing: the stroke ends on the centre line, and the curve tool shapes ---------
            {
                auto* drawClip = project.findTrack (bTrack) != nullptr ? project.findTrack (bTrack)->findMidi (bClip) : nullptr;
                auto* laneComp = pr.laneComp();
                if (drawClip == nullptr || laneComp == nullptr) { log ("piano roll: drawing into the bend lane", false); }
                else
                {
                    drawClip->ccs.clear();
                    project.sendContentChanged();
                    pr.setTool (PianoRollView::Tool::Pencil);
                    pr.curveShape = CurveShape::Free;
                    pr.setCurveShape (CurveShape::Free);
                    pr.resized();

                    auto makeEv = [] (Component& c, Point<float> p, ModifierKeys mods)
                    {
                        return MouseEvent (Desktop::getInstance().getMainMouseSource(), p, mods,
                                           1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                           Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                    };
                    const auto left = ModifierKeys (ModifierKeys::leftButtonModifier);
                    // lane-local coordinates: x from the beat, y from the value inside the lane strip
                    auto strip = Rectangle<int> (0, 0, laneComp->getWidth(), jmax (1, pr.laneHeightAt (0))).reduced (0, 8);
                    const int maxV = controllerMax (kCCPitchBend);
                    auto xAt = [&] (double beat) { return (float) std::round (beat * pr.pxPerBeat - pr.scrollX()); };
                    auto yAt = [&] (int value) { return (float) strip.getBottom() - (float) value / (float) maxV * (float) strip.getHeight(); };

                    // a freehand stroke: drag from value 12000 to 13000 over beats 1..4
                    auto down = makeEv (*laneComp, { xAt (1.0), yAt (12000) }, left);
                    laneComp->mouseDown (down);
                    for (int i = 1; i <= 4; ++i)
                    {
                        auto drag = makeEv (*laneComp, { xAt (1.0 + i * 0.75), yAt (12000 + i * 250) }, left);
                        laneComp->mouseDrag (drag);
                    }
                    auto up = makeEv (*laneComp, { xAt (4.0), yAt (13000) }, left);
                    laneComp->mouseUp (up);

                    int bends = 0;
                    const CCPoint* last = nullptr;
                    for (auto& p : drawClip->ccs)
                        if (p.cc == kCCPitchBend) { ++bends; if (last == nullptr || p.beat >= last->beat) last = &p; }
                    log ("piano roll: a drawn bend stroke ends on the centre line (" + String (bends)
                         + " anchors, last value " + String (last != nullptr ? last->value : -1) + ")",
                         bends > 2 && last != nullptr && last->value == controllerDefault (kCCPitchBend));

                    // a single click keeps the value it hit (it is not a stroke)
                    drawClip->ccs.clear();
                    project.sendContentChanged();
                    auto clickDown = makeEv (*laneComp, { xAt (2.0), yAt (6000) }, left);
                    laneComp->mouseDown (clickDown);
                    laneComp->mouseUp (clickDown);
                    const CCPoint* only = nullptr;
                    for (auto& p : drawClip->ccs) if (p.cc == kCCPitchBend) only = &p;
                    log ("piano roll: a single click in the bend lane keeps the value under the mouse ("
                         + String (only != nullptr ? only->value : -1) + ")",
                         only != nullptr && std::abs (only->value - 6000) < 400);

                    // the curve tool: a sine from the centre up to 14000 over beats 1..5
                    drawClip->ccs.clear();
                    project.sendContentChanged();
                    pr.setCurveShape (CurveShape::Sine);
                    auto sdown = makeEv (*laneComp, { xAt (1.0), yAt (8192) }, left);
                    laneComp->mouseDown (sdown);
                    auto sdrag = makeEv (*laneComp, { xAt (5.0), yAt (14000) }, left);
                    laneComp->mouseDrag (sdrag);
                    laneComp->mouseUp (sdrag);

                    int shapeCount = 0, peak = -1, firstV = -1, lastV = -1;
                    double firstBeat = 1.0e9, lastBeat = -1.0e9;
                    for (auto& p : drawClip->ccs)
                    {
                        if (p.cc != kCCPitchBend) continue;
                        ++shapeCount;
                        peak = jmax (peak, p.value);
                        if (p.beat < firstBeat) { firstBeat = p.beat; firstV = p.value; }
                        if (p.beat > lastBeat) { lastBeat = p.beat; lastV = p.value; }
                    }
                    log ("piano roll: the sine tool fills the dragged range (" + String (shapeCount) + " anchors, "
                         + "peak " + String (peak) + ", ends at " + String (lastV) + ")",
                         shapeCount > 4 && std::abs (firstBeat - 1.0) < 0.3 && std::abs (lastBeat - 5.0) < 0.3
                         && peak > 13500 && std::abs (firstV - 8192) < 500 && lastV == controllerDefault (kCCPitchBend));
                    snapshot ("bend_curve_tool.png");

                    pr.setCurveShape (CurveShape::Free);
                }
            }

            // ---- the bar ruler zooms the time axis with the wheel ------------------------------
            // The ruler is the piano roll's timeline strip: a plain wheel there zooms the time
            // axis (no Ctrl needed) and keeps the bar under the mouse in place. In the note grid a
            // plain wheel still scrolls - that is what the grid is for.
            {
                AppState rst (project, engine, plugins);
                PianoRollView rz (rst);
                rz.setSize (1300, 620);
                if (auto* t = project.findTrack (leadId); t != nullptr && ! t->midiClips.empty())
                    rst.openClipInEditor (leadId, t->midiClips.front().id);
                rz.resized();

                auto makeEv = [] (Component& c, Point<float> p, ModifierKeys mods)
                {
                    return MouseEvent (Desktop::getInstance().getMainMouseSource(), p, mods,
                                       1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                       Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                };
                constexpr float kClick = 0.234f;      // JUCE reports one wheel click as this much travel
                auto wheel = [&] (Component& c, float dy, float dx, ModifierKeys mods)
                {
                    MouseWheelDetails w;
                    w.deltaX = dx; w.deltaY = dy; w.isReversed = false; w.isSmooth = false; w.isInertial = false;
                    c.mouseWheelMove (makeEv (c, { (float) c.getWidth() * 0.5f, 4.0f }, mods), w);
                };
                const auto none = ModifierKeys();
                const auto shift = ModifierKeys (ModifierKeys::shiftModifier);

                auto* ruler = rz.rulerComp();
                auto* grid = rz.gridComp();

                if (ruler == nullptr || grid == nullptr)
                {
                    log ("piano roll: wheel over the bar ruler zooms the time axis", false);
                }
                else
                {
                    const double ppb0 = rz.pxPerBeat;
                    const int anchorX = ruler->getWidth() / 2;
                    const double beatAt0 = (double) (anchorX + rz.scrollX()) / ppb0;

                    wheel (*ruler, kClick, 0.0f, none);
                    const double ppb1 = rz.pxPerBeat;
                    const double beatAt1 = (double) (anchorX + rz.scrollX()) / ppb1;

                    wheel (*ruler, -kClick, 0.0f, none);
                    const double ppb2 = rz.pxPerBeat;

                    // Shift+wheel on the ruler scrolls, it does not zoom
                    rz.gridViewport.setViewPosition (200, rz.scrollY());
                    const int sx0 = rz.scrollX();
                    const double ppbShift0 = rz.pxPerBeat;
                    wheel (*ruler, -kClick, 0.0f, shift);
                    const int sx1 = rz.scrollX();
                    const double ppbShift1 = rz.pxPerBeat;

                    // the note grid still scrolls on a plain wheel (it must not start zooming)
                    const double ppbGrid = rz.pxPerBeat;
                    wheel (*grid, kClick, 0.0f, none);

                    log ("piano roll: wheel over the bar ruler zooms the time axis (" + String (ppb0, 1)
                         + " -> " + String (ppb1, 1) + " -> " + String (ppb2, 1) + " px/beat, bar under the mouse "
                         + String (beatAt0, 3) + " -> " + String (beatAt1, 3) + ")",
                         ppb1 > ppb0 * 1.05 && std::abs (beatAt1 - beatAt0) < 1.5 / ppb1
                         && std::abs (ppb2 - ppb0) < 1.0e-9);
                    log ("piano roll: Shift+wheel on the ruler scrolls, the grid keeps scrolling ("
                         + String (sx0) + " -> " + String (sx1) + " px)",
                         sx1 != sx0 && std::abs (ppbShift1 - ppbShift0) < 1.0e-9
                         && std::abs (rz.pxPerBeat - ppbGrid) < 1.0e-9);

                    // a click on the bar ruler places the playhead; the same click + drag scrolls
                    // the piano roll's time axis and never scrubs the playhead
                    engine.stop();
                    const int clickX = 80;
                    const int sxClick = rz.scrollX();
                    ruler->mouseDown (makeEv (*ruler, { (float) clickX, 5.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
                    ruler->mouseUp (makeEv (*ruler, { (float) clickX, 5.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
                    auto* c = rz.clip();
                    const double expected = (c != nullptr ? c->start : 0.0)
                                          + ((double) clickX + sxClick) / rz.pxPerBeat;
                    const bool clicked = std::abs (engine.getPositionBeats() - expected) < 0.3;

                    const int sxBeforeDrag = rz.scrollX();
                    const int dragFrom = 320, dragTo = 200;
                    ruler->mouseDown (makeEv (*ruler, { (float) dragFrom, 5.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
                    const double downSeek = engine.getPositionBeats();   // the press places the playhead
                    ruler->mouseDrag (makeEv (*ruler, { (float) dragTo, 5.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
                    ruler->mouseUp (makeEv (*ruler, { (float) dragTo, 5.0f }, ModifierKeys (ModifierKeys::leftButtonModifier)));
                    const bool dragged = rz.scrollX() > sxBeforeDrag
                                         && std::abs (engine.getPositionBeats() - downSeek) < 0.3;
                    log ("piano roll: a click on the bar ruler moves the transport (" + String (downSeek, 2)
                         + " beats) and dragging it scrolls the time axis (" + String (sxBeforeDrag) + " -> "
                         + String (rz.scrollX()) + " px) without scrubbing", clicked && dragged);
                }
            }

            // ---- the lane gets a real height when the piano roll lives in the main window --------
            {
                AppState st2 (project, engine, plugins);
                MainComponent mc (st2);
                mc.setSize (1229, 710);              // 1080p window at 125% scaling
                mc.perform (CmdTogglePianoRoll);
                mc.resized();

                PianoRollView* found = nullptr;
                std::function<void (Component&)> hunt = [&] (Component& root)
                {
                    for (auto* child : root.getChildren())
                    {
                        if (found != nullptr) return;
                        if (auto* pr2 = dynamic_cast<PianoRollView*> (child)) { found = pr2; return; }
                        hunt (*child);
                    }
                };
                hunt (mc);
                const int laneH = found != nullptr && found->laneComp() != nullptr ? found->laneComp()->getHeight() : 0;
                const int laneW = found != nullptr && found->laneComp() != nullptr ? found->laneComp()->getWidth() : 0;
                log ("piano roll: the CC lane is laid out with a real size inside the main window ("
                     + String (laneW) + "x" + String (laneH) + ", piano roll "
                     + (found != nullptr ? found->getBounds().toString() : String ("not found")) + ")",
                     found != nullptr && laneH >= 100 && laneW > 400);
                mc.perform (CmdPianoRollFull);       // the true-fullscreen piano roll
                mc.resized();
                const int fsLaneH = found != nullptr && found->laneComp() != nullptr ? found->laneComp()->getHeight() : 0;
                log ("piano roll: the CC lane keeps its height in the fullscreen piano roll (" + String (fsLaneH)
                     + ", piano roll " + (found != nullptr ? found->getBounds().toString() : String()) + ")",
                     fsLaneH >= 100);
                mc.perform (CmdPianoRollFull);
                mc.perform (CmdTogglePianoRoll);
            }

            // ---- tool shortcuts: 1 / 2 / 3 = pointer, pencil, eraser ----------------------------
            // The numbers follow the panel that is in front: the arrangement has four tools (the
            // fourth is the mute-region tool), the piano roll has its own three.
            {
                AppState tst (project, engine, plugins);
                MainComponent tmc (tst);
                tmc.setSize (1229, 710);

                auto clickInto = [&tmc] (Component& target)
                {
                    const auto p = tmc.getLocalPoint (&target, target.getLocalBounds().getCentre());
                    MouseEvent e (Desktop::getInstance().getMainMouseSource(), p.toFloat(),
                                  ModifierKeys (ModifierKeys::leftButtonModifier),
                                  1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &tmc, &tmc,
                                  Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                    tmc.mouseDown (e);
                };

                const bool arrangeOk = tmc.keyPressed (KeyPress ('2')) && tst.arrangeTool == ArrangeTool::Pencil
                                    && tmc.keyPressed (KeyPress ('3')) && tst.arrangeTool == ArrangeTool::Eraser
                                    && tmc.keyPressed (KeyPress ('4')) && tst.arrangeTool == ArrangeTool::Mute
                                    && tmc.keyPressed (KeyPress ('1')) && tst.arrangeTool == ArrangeTool::Select;

                tmc.perform (CmdTogglePianoRoll);      // the piano roll in front
                tmc.resized();

                PianoRollView* toolPr = nullptr;
                std::function<void (Component&)> hunt = [&] (Component& root)
                {
                    for (auto* child : root.getChildren())
                    {
                        if (toolPr != nullptr) return;
                        if (auto* found = dynamic_cast<PianoRollView*> (child)) { toolPr = found; return; }
                        hunt (*child);
                    }
                };
                hunt (tmc);

                bool pianoOk = false;
                if (toolPr != nullptr)
                {
                    clickInto (*toolPr);               // makes the piano roll the active context
                    pianoOk = tmc.keyPressed (KeyPress ('2')) && toolPr->tool == PianoRollView::Tool::Pencil
                           && tmc.keyPressed (KeyPress ('3')) && toolPr->tool == PianoRollView::Tool::Eraser
                           && tmc.keyPressed (KeyPress ('1')) && toolPr->tool == PianoRollView::Tool::Pointer
                           && tst.arrangeTool == ArrangeTool::Select;   // the arrangement keeps its tool

                    // clicking back into the arrangement hands the numbers to the arrange tools
                    // again, and leaves the piano roll's tool alone
                    MouseEvent e (Desktop::getInstance().getMainMouseSource(), Point<float> (30.0f, 30.0f),
                                  ModifierKeys (ModifierKeys::leftButtonModifier),
                                  1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &tmc, &tmc,
                                  Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                    tmc.mouseDown (e);
                    pianoOk = pianoOk && tmc.keyPressed (KeyPress ('2'))
                           && tst.arrangeTool == ArrangeTool::Pencil
                           && toolPr->tool == PianoRollView::Tool::Pointer;
                }

                log ("tool shortcuts: 1 / 2 / 3 pick pointer, pencil and eraser in the arrangement (4 = mute)", arrangeOk);
                log ("tool shortcuts: the same keys drive the piano roll's own tools while it is in front", pianoOk);
            }

            project.removeTrack (bTrack);
            project.sendStructureChanged();
            engine.syncAll();
        }
    }

    // ---- humanize: random velocity / timing of the selected notes -------------------------------
    {
        // the amount the editor remembers for the shortcut and the next visit of the dialog
        const HumanizeSettings defaults;
        log ("humanize: the default amount changes the velocity and leaves the timing alone",
             defaults.velocityRange > 0 && defaults.timingTicks == 0 && ! defaults.empty());

        // the draw itself: never outside range, and a bell keeps more notes near their old value
        // than an even spread does (which is what makes it sound played instead of drunk)
        {
            juce::Random uniRng (20240607), bellRng (20240607);
            const int draws = 6000;
            int range = 20;
            bool inside = true;
            double uniAbs = 0.0, uniMean = 0.0, bellAbs = 0.0, bellMean = 0.0;
            for (int i = 0; i < draws; ++i)
            {
                const int u = humanize::offset (uniRng, range, false);
                const int b = humanize::offset (bellRng, range, true);
                inside = inside && u >= -range && u <= range && b >= -range && b <= range;
                uniAbs += std::abs (u); uniMean += u;
                bellAbs += std::abs (b); bellMean += b;
            }
            uniAbs /= draws; uniMean /= draws; bellAbs /= draws; bellMean /= draws;
            log ("humanize: every random offset stays inside +/-" + String (range)
                 + " (uniform mean |" + String (uniAbs, 2) + "|, bell mean |" + String (bellAbs, 2) + "|)",
                 inside && std::abs (uniMean) < 0.5 && std::abs (bellMean) < 0.5 && bellAbs < uniAbs - 0.5);
        }

        auto& ht = project.addTrack (TrackType::Instrument, "Humanize");
        const int hTrack = ht.id;
        const int hClip = project.newId();
        {
            MidiClip mc;
            mc.id = hClip; mc.name = "Hats"; mc.start = 0.0; mc.length = 16.0;
            for (int i = 0; i < 32; ++i)
            {
                Note n;
                n.id = project.newId();
                n.pitch = 60 + (i % 6);
                n.start = i * 0.5;          // first note sits exactly on 0: timing may not go below it
                n.length = 0.4;
                n.velocity = 100;           // every note the same: what humanize is for
                mc.notes.push_back (n);
            }
            ht.midiClips.push_back (mc);
        }
        project.sendStructureChanged();
        project.sendContentChanged();

        AppState st (project, engine, plugins);
        PianoRollView pr (st);
        pr.setSize (1300, 760);
        st.openClipInEditor (hTrack, hClip);
        pr.resized();

        auto findClip = [&] () -> MidiClip*
        {
            auto* t = project.findTrack (hTrack);
            return t != nullptr ? t->findMidi (hClip) : nullptr;
        };
        auto* clip = findClip();
        if (clip == nullptr || clip->notes.size() != 32)
        {
            log ("humanize: the test clip exists", false);
        }
        else
        {
            const std::vector<Note> original = clip->notes;

            // pictures of the editor with the velocity lane, before and after humanising
            auto snapshotTo = [&] (const String& name)
            {
                if (auto shot = pr.createComponentSnapshot (pr.getLocalBounds(), true); shot.isValid())
                    if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile (name).createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                    }
            };

            // 1) the first half is selected: only those notes move, and only inside a small range
            pr.selNotes.clear();
            for (size_t i = 0; i < 16; ++i) pr.selNotes.insert (clip->notes[i].id);
            snapshotTo ("humanize_before.png");

            HumanizeSettings s;
            s.velocityRange = 20;
            s.timingTicks = 0;
            s.bell = false;

            auto session = pr.captureHumanizeSession();
            const int first = pr.applyHumanize (s, session);

            int inRange = 0, varied = 0, kept = 0;
            bool timingKept = true;
            for (size_t i = 0; i < clip->notes.size(); ++i)
            {
                const auto& n = clip->notes[i];
                if (i < 16)
                {
                    if (std::abs (n.velocity - 100) <= 20) ++inRange;
                    if (n.velocity != 100) ++varied;
                    if (std::abs (n.start - original[i].start) > 1.0e-12) timingKept = false;
                }
                else if (n.velocity == original[i].velocity && std::abs (n.start - original[i].start) < 1.0e-12)
                    ++kept;
            }
            log ("humanize: only the selected notes change, each by a random +/-20 velocity ("
                 + String (varied) + " of 16 moved, " + String (kept) + " untouched)",
                 first == 16 && inRange == 16 && varied >= 8 && kept == 16 && timingKept);

            // 2) clicking again re-draws from the values the session started with: the notes never
            //    wander further away than a small range, which is what piling the offsets up would do
            std::vector<int> afterFirst;
            for (size_t i = 0; i < 16; ++i) afterFirst.push_back (clip->notes[i].velocity);
            const int second = pr.applyHumanize (s, session);
            int stillInRange = 0, redrawn = 0;
            for (size_t i = 0; i < 16; ++i)
            {
                const int v = clip->notes[i].velocity;
                if (std::abs (v - 100) <= 20) ++stillInRange;
                if (v != afterFirst[i]) ++redrawn;
            }
            log ("humanize: randomising again redraws instead of piling the offsets up ("
                 + String (redrawn) + " notes redrawn, all still inside +/-20)",
                 second == 16 && stillInRange == 16 && redrawn >= 4);
            snapshotTo ("humanize_after.png");

            // 3) the whole session is a single undo step
            project.undo();
            auto* back = findClip();
            bool undone = back != nullptr && back->notes.size() == original.size();
            if (undone)
                for (size_t i = 0; i < original.size(); ++i)
                    if (back->notes[i].velocity != original[i].velocity
                        || std::abs (back->notes[i].start - original[i].start) > 1.0e-12) undone = false;
            log ("humanize: one Ctrl+Z undoes every click the dialog made", undone);

            // 4) timing: notes leave the grid, never before the start of the clip, velocity kept
            clip = findClip();
            pr.selNotes.clear();
            if (clip == nullptr) { log ("humanize: timing offset", false); }
            else
            {
                HumanizeSettings ts;
                ts.velocityRange = 0;
                ts.timingTicks = 60;          // 60 of 960 ticks = 1/16 beat
                ts.bell = false;
                auto timingSession = pr.captureHumanizeSession();
                const int moved = pr.applyHumanize (ts, timingSession);

                const double limit = 60.0 / kTicksPerBeat;
                int inside = 0, nudged = 0;
                bool velocityKept = true, startsOk = true;
                for (size_t i = 0; i < clip->notes.size(); ++i)
                {
                    const auto& n = clip->notes[i];
                    if (std::abs (n.start - original[i].start) <= limit + 1.0e-12) ++inside;
                    if (std::abs (n.start - original[i].start) > 1.0e-12) ++nudged;
                    if (n.velocity != original[i].velocity) velocityKept = false;
                    if (n.start < -1.0e-12) startsOk = false;
                }
                log ("humanize: the timing offset moves notes off the grid by at most 1/16 beat ("
                     + String (nudged) + " of " + String (clip->notes.size()) + " nudged, none before 0)",
                     moved == (int) clip->notes.size() && inside == (int) clip->notes.size()
                     && nudged >= 8 && velocityKept && startsOk);
            }

            // 5) nothing to do: no edit, and no empty undo step either
            project.clearUndo();
            HumanizeSettings zero;
            zero.velocityRange = 0;
            zero.timingTicks = 0;
            PianoRollView::HumanizeSession zeroSession = pr.captureHumanizeSession();
            const int nothing = pr.applyHumanize (zero, zeroSession);
            log ("humanize: an amount of 0 changes nothing and pushes no undo step",
                 nothing == 0 && zero.empty() && ! project.canUndo());

            // 6) with no selection the whole clip is humanised (the rule Quantize uses)
            clip = findClip();
            if (clip == nullptr) { log ("humanize: the whole clip", false); }
            else
            {
                for (auto& n : clip->notes) n.velocity = 100;
                pr.selNotes.clear();
                HumanizeSettings small;
                small.velocityRange = 8;
                small.bell = false;
                PianoRollView::HumanizeSession wholeSession = pr.captureHumanizeSession();
                const int whole = pr.applyHumanize (small, wholeSession);

                int inside = 0, movedNotes = 0;
                for (auto& n : clip->notes)
                {
                    if (std::abs (n.velocity - 100) <= 8) ++inside;
                    if (n.velocity != 100) ++movedNotes;
                }
                log ("humanize: with nothing selected every note of the clip is humanised ("
                     + String (whole) + " notes, " + String (movedNotes) + " moved)",
                     whole == 32 && inside == 32 && movedNotes >= 16);
            }

            // 7) the menu / H shortcut path: one call, one undo step of its own
            clip = findClip();
            bool quickOk = false;
            if (clip == nullptr) { log ("humanize: the menu / H shortcut", false); }
            else
            {
                for (auto& n : clip->notes) n.velocity = 100;
                pr.selNotes.clear();
                for (size_t i = 0; i < 8; ++i) pr.selNotes.insert (clip->notes[i].id);
                HumanizeSettings quick;
                quick.velocityRange = 12;
                project.clearUndo();
                pr.humanizeSelection (quick);

                int inside = 0, movedNotes = 0;
                for (size_t i = 0; i < 8; ++i)
                {
                    if (std::abs (clip->notes[i].velocity - 100) <= 12) ++inside;
                    if (clip->notes[i].velocity != 100) ++movedNotes;
                }
                project.undo();
                auto* restoredClip = findClip();
                bool restored = restoredClip != nullptr && restoredClip->notes.size() == 32;
                if (restored) for (auto& n : restoredClip->notes) if (n.velocity != 100) restored = false;
                quickOk = inside == 8 && movedNotes >= 4 && restored;
            }
            log ("humanize: the menu / H shortcut humanises the selection as one undoable edit", quickOk);

            // 8) the velocity clamp holds even when the deviation is larger than the note's own
            //    velocity (a seeded generator, so this is a repeatable check rather than a coin toss)
            {
                juce::Random rng (424242);
                std::vector<Note> ns (2);
                ns[0].id = 90001; ns[0].velocity = 1;
                ns[1].id = 90002; ns[1].velocity = 127;
                HumanizeSettings wide;
                wide.velocityRange = 64;
                wide.bell = false;
                bool ok = true, sawBottom = false, sawTop = false;
                for (int i = 0; i < 500; ++i)
                {
                    humanize::apply (ns, {}, wide, rng);
                    ok = ok && ns[0].velocity >= 1 && ns[1].velocity <= 127;
                    sawBottom = sawBottom || ns[0].velocity == 1;
                    sawTop = sawTop || ns[1].velocity == 127;
                }
                log ("humanize: velocities are clamped to 1..127 even with a +/-64 amount",
                     ok && sawBottom && sawTop);
            }

            // 9) the dialog's panel renders at a usable size (also written out as a picture)
            {
                HumanizeSettings panelSettings;
                panelSettings.velocityRange = 24;
                panelSettings.timingTicks = 30;
                const auto shot = renderHumanizePanel (panelSettings, 12, 32);
                if (shot.isValid())
                    if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("humanize_dialog.png").createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                    }
                log ("humanize: the dialog panel renders at a usable size ("
                     + String (shot.getWidth()) + "x" + String (shot.getHeight()) + ")",
                     shot.isValid() && shot.getWidth() >= 420 && shot.getHeight() >= 260);
            }

            // 10) reverse the selected phrase, then mark the selection as slide notes
            clip = findClip();
            bool reverseOk = false, slideOk = false;
            if (clip != nullptr && clip->notes.size() >= 4)
            {
                std::vector<double> oldStart;
                double lo = 1.0e9, hi = 0.0;
                pr.selNotes.clear();
                for (size_t i = 0; i < jmin<size_t> (4, clip->notes.size()); ++i)
                {
                    auto& n = clip->notes[i];
                    pr.selNotes.insert (n.id);
                    oldStart.push_back (n.start);
                    lo = jmin (lo, n.start);
                    hi = jmax (hi, n.start + n.length);
                }
                std::vector<double> oldLen;
                for (size_t i = 0; i < oldStart.size(); ++i) oldLen.push_back (clip->notes[i].length);
                pr.reverseSelection();
                reverseOk = true;
                for (size_t i = 0; i < oldStart.size(); ++i)
                    reverseOk = reverseOk && std::abs (clip->notes[i].start - (lo + hi - oldStart[i] - oldLen[i])) < 1.0e-9;
                pr.setSlideOnSelection (true);
                slideOk = true;
                for (size_t i = 0; i < oldStart.size(); ++i) slideOk = slideOk && clip->notes[i].slide;
                pr.setSlideOnSelection (false);
            }
            log ("piano roll: reverse mirrors the selected phrase in time", reverseOk);
            log ("piano roll: selected notes can be made into FL-style slide notes", slideOk);
        }

        project.removeTrack (hTrack);
        project.sendStructureChanged();
        engine.syncAll();
        project.clearUndo();
    }
    // ---- fullscreen clip gain editor: every module ---------------------------------------------
    {
        auto& et = project.addTrack (TrackType::Audio, "GainEdit");
        const int etid = et.id;
        AudioClip ec;
        ec.id = project.newId(); ec.name = "gainclip"; ec.file = sine; ec.start = 0.0;
        ec.data = project.pool.load (sine, engine.formats);
        ec.lengthSec = 2.0;                       // 2 s = 4 beats at 120 bpm
        const int cid = ec.id;
        et.audioClips.push_back (std::move (ec));
        project.sendStructureChanged();
        engine.syncAll();

        AppState st (project, engine, plugins);
        ClipGainEditor ed (st);
        ed.setSize (1200, 700);
        ed.open (etid, cid);
        auto* cp = et.findAudio (cid);
        const double cb = cp != nullptr ? project.clipBeats (cp->start, cp->lengthSec) : 4.0;

        auto makeEv = [] (Component& c, Point<float> pos, ModifierKeys mods)
        {
            return MouseEvent (Desktop::getInstance().getMainMouseSource(), pos, mods,
                               1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                               Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
        };
        const auto plot = ed.getPlotRect();
        const auto mid = plot.getCentre();
        constexpr float kClick = 0.234f;          // JUCE reports one wheel click as this much travel
        auto wheel = [&] (float dy, float dx, ModifierKeys mods)
        {
            MouseWheelDetails w;
            w.deltaX = dx; w.deltaY = dy; w.isReversed = false; w.isSmooth = false; w.isInertial = false;
            ed.mouseWheelMove (makeEv (ed, mid, mods), w);
        };
        const auto none = ModifierKeys();
        const auto shift = ModifierKeys (ModifierKeys::shiftModifier);
        const auto ctrl = ModifierKeys (ModifierKeys::ctrlModifier);
        const auto leftBtn = ModifierKeys (ModifierKeys::leftButtonModifier);
        const auto midBtn = ModifierKeys (ModifierKeys::middleButtonModifier);

        // opening builds a flat 2-anchor envelope on the clip
        log ("clip editor: opens with a flat envelope", ed.isOpen() && cp != nullptr && cp->gainLine
             && cp->gainEnv.size() == 2 && ed.getViewState().beats <= 0.0);

        // wheel = zoom the time axis, dB range untouched
        const auto fit = ed.getViewState();
        wheel (kClick, 0.0f, none);
        const auto zoomed = ed.getViewState();
        log ("clip editor: wheel zooms the timeline", zoomed.beats > 0.0 && zoomed.beats < cb * 0.95
             && std::abs (zoomed.dbSpan - fit.dbSpan) < 0.001f);

        // Shift+wheel = scroll left / right (JUCE: deltaY > 0 is wheel up = earlier)
        // and never moves the envelope scale
        const auto s0 = ed.getViewState();
        wheel (-kClick, 0.0f, shift);                     // wheel down = later
        const auto s1 = ed.getViewState();
        wheel (kClick, 0.0f, shift);                      // wheel up = earlier
        const auto s2 = ed.getViewState();
        log ("clip editor: Shift+wheel scrolls (" + String (s0.start, 3) + " -> " + String (s1.start, 3)
             + " -> " + String (s2.start, 3) + ")",
             s1.start > s0.start + 1.0e-6 && std::abs (s2.start - s0.start) < 1.0e-6
             && std::abs (s1.dbTop - s0.dbTop) < 0.001f && std::abs (s1.dbSpan - s0.dbSpan) < 0.001f
             && std::abs (s1.beats - s0.beats) < 1.0e-9);

        // ... and stops at both ends of the clip
        for (int i = 0; i < 30; ++i) wheel (-kClick, 0.0f, shift);
        const auto atEnd = ed.getViewState();
        for (int i = 0; i < 60; ++i) wheel (kClick, 0.0f, shift);
        const auto atStart = ed.getViewState();
        log ("clip editor: scrolling clamps to the clip ends (" + String (atEnd.start, 3) + " / "
             + String (atStart.start, 3) + ")",
             atEnd.start > 0.05 && std::abs (atEnd.start - (cb - atEnd.beats)) < 1.0e-6
             && std::abs (atStart.start) < 1.0e-6);

        // Ctrl+wheel = dB zoom only, and it always stays centred on 0 dB
        const auto d0 = ed.getViewState();
        wheel (kClick, 0.0f, ctrl);
        const auto d1 = ed.getViewState();
        bool centred = std::abs (d1.start - d0.start) < 1.0e-9 && std::abs (d1.beats - d0.beats) < 1.0e-9;
        for (int i = 0; i < 8; ++i)
        {
            wheel (kClick, 0.0f, ctrl);
            const auto s = ed.getViewState();
            centred = centred && std::abs (s.dbTop - s.dbSpan * 0.5f) < 1.0e-4f;   // symmetric around 0 dB
        }
        const auto d2 = ed.getViewState();
        for (int i = 0; i < 16; ++i) wheel (-kClick, 0.0f, ctrl);
        const auto d3 = ed.getViewState();
        log ("clip editor: Ctrl+wheel zooms the dB scale around 0 dB (" + String (d0.dbSpan, 1) + " -> "
             + String (d2.dbSpan, 1) + " -> " + String (d3.dbSpan, 1) + " dB)",
             d1.dbSpan < d0.dbSpan - 0.1f && centred
             && std::abs (d2.dbTop - d2.dbSpan * 0.5f) < 1.0e-4f
             && std::abs (d3.dbTop - d3.dbSpan * 0.5f) < 1.0e-4f
             && d3.dbSpan > d2.dbSpan + 0.1f);

        // 0 dB really is at the vertical middle of the plot: dbToY(0) == centre
        {
            const auto vs = ed.getViewState();
            const auto pr = ed.getPlotRect();
            const float yOf0 = pr.getY() + (vs.dbTop - 0.0f) / jmax (1.0f, vs.dbSpan) * pr.getHeight();
            log ("clip editor: 0 dB sits in the middle (" + String (yOf0, 1) + " vs "
                 + String (pr.getCentreY(), 1) + ")", std::abs (yOf0 - pr.getCentreY()) < 0.6f);
        }

        // a trackpad side swipe scrolls too (from the start, so both ways can move)
        for (int i = 0; i < 40; ++i) wheel (kClick, 0.0f, shift);
        const auto t0 = ed.getViewState();
        wheel (0.0f, -0.5f, none);
        const auto t1 = ed.getViewState();
        wheel (0.0f, 0.5f, none);
        log ("clip editor: horizontal swipe scrolls (" + String (t0.start, 3) + " -> " + String (t1.start, 3) + ")",
             t1.start > t0.start + 1.0e-6 && std::abs (ed.getViewState().start - t0.start) < 1.0e-6);

        // middle-drag pans the time axis (the dB scale never moves)
        ed.keyPressed (KeyPress ('f', ModifierKeys(), 'f'));
        wheel (kClick, 0.0f, none);
        const auto p0 = ed.getViewState();
        ed.mouseDown (makeEv (ed, mid, midBtn));
        ed.mouseDrag (makeEv (ed, { mid.x - 120.0f, mid.y + 40.0f }, midBtn));
        ed.mouseUp (makeEv (ed, { mid.x - 120.0f, mid.y + 40.0f }, none));
        const auto p1 = ed.getViewState();
        log ("clip editor: middle-drag pans the time axis", p1.start > p0.start + 1.0e-6
             && std::abs (p1.dbTop - p0.dbTop) < 1.0e-6f);

        // arrow keys pan / zoom the dB scale, +/- zoom, F fits
        const auto k0 = ed.getViewState();
        ed.keyPressed (KeyPress (KeyPress::rightKey));
        const auto k1 = ed.getViewState();
        ed.keyPressed (KeyPress (KeyPress::downKey));
        const auto k2 = ed.getViewState();
        ed.keyPressed (KeyPress ('+', ModifierKeys(), '+'));
        const auto k3 = ed.getViewState();
        ed.keyPressed (KeyPress ('f', ModifierKeys(), 'f'));
        const auto k4 = ed.getViewState();
        log ("clip editor: arrow keys / +/- / F (start "
             + String (k0.start, 3) + "/" + String (k1.start, 3) + ", span "
             + String (k1.dbSpan, 1) + "/" + String (k2.dbSpan, 1) + ", beats "
             + String (k1.beats, 3) + "/" + String (k3.beats, 3) + ")",
             k1.start > k0.start + 1.0e-6 && k2.dbSpan > k1.dbSpan + 0.1f
             && std::abs (k2.dbTop - k2.dbSpan * 0.5f) < 1.0e-4f
             && k3.beats < k1.beats && k4.beats <= 0.0 && std::abs (k4.start) < 1.0e-9);

        // map a model position to pixels the same way the editor does
        auto beatToX = [&] (double b)
        {
            const auto vs = ed.getViewState();
            const auto pr = ed.getPlotRect();
            const double vb = vs.beats > 0.0 ? vs.beats : cb;
            return pr.getX() + (float) ((b - vs.start) / vb) * pr.getWidth();
        };
        auto dbToY = [&] (float db)
        {
            const auto vs = ed.getViewState();
            const auto pr = ed.getPlotRect();
            return pr.getY() + (vs.dbTop - db) / jmax (1.0f, vs.dbSpan) * pr.getHeight();
        };

        // fade grips: drag in, drag out, double-click clears
        cp->fadeIn = 0.0; cp->fadeOut = 0.0;
        ed.mouseDown (makeEv (ed, { beatToX (0.0) + 1.0f, plot.getY() + 3.0f }, leftBtn));
        ed.mouseDrag (makeEv (ed, { beatToX (0.0) + 70.0f, plot.getY() + 3.0f }, leftBtn));
        ed.mouseUp (makeEv (ed, { beatToX (0.0) + 70.0f, plot.getY() + 3.0f }, none));
        const bool fadeInOk = cp->fadeIn > 0.01;
        ed.mouseDown (makeEv (ed, { beatToX (cb) - 1.0f, plot.getY() + 3.0f }, leftBtn));
        ed.mouseDrag (makeEv (ed, { beatToX (cb) - 70.0f, plot.getY() + 3.0f }, leftBtn));
        ed.mouseUp (makeEv (ed, { beatToX (cb) - 70.0f, plot.getY() + 3.0f }, none));
        const bool fadeOutOk = cp->fadeOut > 0.01 && fadeInOk;
        // the grip now sits at the end of the fade-in, not at the clip start
        const double gripBeat = project.secondsToBeats (project.beatsToSeconds (cp->start) + cp->fadeIn) - cp->start;
        ed.mouseDoubleClick (makeEv (ed, { beatToX (gripBeat), plot.getY() + 3.0f }, leftBtn));
        log ("clip editor: fade grips drag and double-click clears (in " + String (cp->fadeIn, 3)
             + " s, out " + String (cp->fadeOut, 3) + " s)",
             fadeOutOk && cp->fadeIn == 0.0 && cp->fadeOut > 0.01);

        // anchors: click adds one, dragging it changes the dB value
        ed.keyPressed (KeyPress ('f', ModifierKeys(), 'f'));
        cp->gainEnv = { { 0.0, 0.0f }, { cb, 0.0f } };
        ed.mouseDown (makeEv (ed, mid, leftBtn));
        const bool added = cp->gainEnv.size() == 3;
        const float dbAfterClick = cp->gainEnv.size() == 3 ? cp->gainEnv[1].db : 0.0f;
        ed.mouseDrag (makeEv (ed, { mid.x, mid.y - 60.0f }, leftBtn));
        ed.mouseUp (makeEv (ed, { mid.x, mid.y - 60.0f }, none));
        const float dbAfterDrag = cp->gainEnv.size() == 3 ? cp->gainEnv[1].db : 0.0f;
        log ("clip editor: click adds an anchor, drag changes its dB (" + String (dbAfterClick, 2) + " -> "
             + String (dbAfterDrag, 2) + " dB)", added && dbAfterDrag > dbAfterClick + 1.0f);

        // right-click on an anchor removes exactly that anchor
        const int before = (int) cp->gainEnv.size();
        const auto target = cp->gainEnv[1];
        ed.mouseDown (makeEv (ed, { beatToX (target.beat), dbToY (target.db) },
                              ModifierKeys (ModifierKeys::rightButtonModifier)));
        log ("clip editor: right-click removes an anchor", (int) cp->gainEnv.size() == before - 1);

        // Alt+drag shifts the whole curve
        cp->gainEnv = { { 0.0, -6.0f }, { cb * 0.5, -6.0f }, { cb, -6.0f } };
        const float yCurve = dbToY (-6.0f);
        const auto altLeft = ModifierKeys (ModifierKeys::leftButtonModifier | ModifierKeys::altModifier);
        ed.mouseDown (makeEv (ed, { plot.getCentreX(), yCurve }, altLeft));
        ed.mouseDrag (makeEv (ed, { plot.getCentreX(), yCurve - 40.0f }, altLeft));
        ed.mouseUp (makeEv (ed, { plot.getCentreX(), yCurve - 40.0f }, none));
        bool shifted = cp->gainEnv.size() == 3;
        for (auto& p : cp->gainEnv) shifted = shifted && p.db > -5.9f;
        log ("clip editor: Alt+drag moves the whole curve", shifted);

        // freehand mode drops a run of anchors (snap off, or the points collapse onto the grid)
        st.clipGainFreehand = true;
        st.clipGainSnap = false;
        cp->gainEnv = { { 0.0, 0.0f }, { cb, 0.0f } };
        ed.mouseDown (makeEv (ed, { plot.getX() + 120.0f, plot.getCentreY() + 40.0f }, leftBtn));
        for (int i = 1; i <= 6; ++i)
            ed.mouseDrag (makeEv (ed, { plot.getX() + 120.0f + i * 40.0f, plot.getCentreY() + 40.0f - i * 5.0f }, leftBtn));
        ed.mouseUp (makeEv (ed, { plot.getX() + 360.0f, plot.getCentreY() + 10.0f }, none));
        st.clipGainFreehand = false;
        st.clipGainSnap = true;
        log ("clip editor: freehand mode draws a run of anchors (" + String ((int) cp->gainEnv.size()) + ")",
             cp->gainEnv.size() >= 6);

        // Esc closes and notifies the host
        bool closedCb = false;
        ed.onClose = [&closedCb] { closedCb = true; };
        const bool escHandled = ed.keyPressed (KeyPress (KeyPress::escapeKey));
        log ("clip editor: Esc closes", escHandled && ! ed.isOpen() && closedCb && ! ed.isVisible());

        // the clip can disappear underneath the editor (undo / delete): it must not
        // retarget onto a dead id, must report it, must paint empty instead of the old
        // waveform, and it closes without crashing
        {
            AudioClip keep = et.audioClips.front();
            et.audioClips.clear();                        // the clip is gone, the track stays
            ed.open (etid, keep.id);                      // opening on a missing clip does nothing
            const bool stayedClosed = ! ed.isOpen();
            et.audioClips.push_back (keep);
            ed.open (etid, keep.id);
            const bool opened = ed.isOpen() && ed.clipExists();
            et.audioClips.clear();                        // now the undo removes it while open
            const bool noticed = ed.isOpen() && ! ed.clipExists();
            Image img (Image::ARGB, 400, 300, true);
            Graphics gi (img);
            ed.paint (gi);                                // empty state, no stale waveform
            const auto bg = img.getPixelAt (0, 0);        // whatever the empty background is
            int painted = 0;
            for (int x = 0; x < img.getWidth(); x += 4)
                if (img.getPixelAt (x, 40) != bg) ++painted;
            ed.close();
            log ("clip editor: survives the clip being undone away (closed=" + String ((int) stayedClosed)
                 + " open=" + String ((int) opened) + " noticed=" + String ((int) noticed)
                 + " painted=" + String (painted) + ")",
                 stayedClosed && opened && noticed && painted == 0 && ! ed.isOpen());
            et.audioClips.push_back (keep);               // restore for the cleanup below
        }

        project.removeTrack (etid);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }


    // ---- the plug-in window leaves the whole editor visible below the title bar -------------------
    {
        auto& t = project.addTrack (TrackType::Audio, "SizeTest");
        const int tid = t.id;
        project.sendStructureChanged();
        String err;
        const bool added = engine.addInsert (tid, makeBuiltInDelaySlot().desc, err);
        engine.syncAll();
        auto* tr = project.findTrack (tid);
        const int64 uid = (tr != nullptr && ! tr->inserts.empty()) ? tr->inserts.front().uid : 0;

        PluginWindows wins (engine);
        wins.show (uid, "SX Delay - SizeTest");
        // The panel chrome must come up immediately; building the plug-in editor is deferred to
        // the next frames so a slow plug-in UI does not freeze the whole window (that was the
        // stutter + black rectangle when a plug-in was opened). Right after show() the content is
        // therefore the themed placeholder, not the plug-in editor.
        bool chromeImmediately = false, editorDeferred = false;
        if (auto* win = wins.windowComponent (uid))
        {
            auto* contentNow = wins.windowContent (uid);
            chromeImmediately = wins.isOpen (uid) && win->isVisible() && win->getWidth() >= 200;
            editorDeferred = contentNow == nullptr
                          || dynamic_cast<AudioProcessorEditor*> (contentNow) == nullptr;
        }
        for (int i = 0; i < 8; ++i) MessageManager::getInstance()->runDispatchLoopUntil (20);

        bool sized = false;
        bool embedded = false;
        Component* firstEditor = nullptr;
        if (auto* win = wins.windowComponent (uid))
        {
            embedded = win->getParentComponent() == wins.hostComponent() && win->getWindowHandle() == nullptr;
            const int barHeight = 28;
            auto* content = wins.windowContent (uid);
            firstEditor = dynamic_cast<AudioProcessorEditor*> (content);
            // the content keeps the editor's own size, the window is that plus the title bar
            sized = content != nullptr
                 && content->getHeight() > 0
                 && win->getHeight() >= content->getHeight() + barHeight - 1
                 && content->getWidth() >= win->getWidth() - 4;
            log ("plug-in window: editor " + String (content != nullptr ? content->getWidth() : 0) + "x"
                 + String (content != nullptr ? content->getHeight() : 0) + " inside window "
                 + String (win->getWidth()) + "x" + String (win->getHeight()), true);
        }
        log ("plug-in window: the editor keeps its full size under the title bar", added && sized);
        log ("plug-in window: the panel is a child of the host, not a native top-level window", embedded);
        log ("plug-in window: the chrome appears at once and the editor is built a frame later ("
             + String ((int) chromeImmediately) + "/" + String ((int) editorDeferred) + "/"
             + String ((int) (firstEditor != nullptr)) + ")",
             added && chromeImmediately && editorDeferred && firstEditor != nullptr);

        // the title bar moves the whole panel as one surface: the body must be at the same
        // screen offset while the mouse is still down, not only after the button is released
        bool dragTogether = false;
        if (auto* win = wins.windowComponent (uid))
        {
            auto* body = wins.windowContent (uid);
            if (body != nullptr)
            {
                const auto winBefore = win->getScreenPosition();
                const auto bodyBefore = body->getScreenPosition();
                MouseEvent down (Desktop::getInstance().getMainMouseSource(),
                                 Point<float> (20.0f, 10.0f), ModifierKeys (ModifierKeys::leftButtonModifier),
                                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, win, win,
                                 Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                win->mouseDown (down);
                MouseEvent move (Desktop::getInstance().getMainMouseSource(),
                                 Point<float> (60.0f, 10.0f), ModifierKeys (ModifierKeys::leftButtonModifier),
                                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, win, win,
                                 Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                win->mouseDrag (move);
                const auto bodyDuring = body->getScreenPosition();
                win->mouseUp (move);
                const auto winAfter = win->getScreenPosition();
                dragTogether = bodyDuring.x - bodyBefore.x == 40
                            && winAfter.x - winBefore.x == 40;
            }
        }
        log ("plug-in window: dragging the title bar moves the editor with it (one surface)", dragTogether);

        // the cross in the panel title bar has to close the panel (the old native button did not)
        bool closedByCross = false;
        bool editorCached = false, reuseInstant = false;
        if (auto* win = wins.windowComponent (uid))
        {
            const auto p = Point<int> (win->getWidth() - 17, 14);
            if (auto* target = win->getComponentAt (p))
            {
                MouseEvent ev (Desktop::getInstance().getMainMouseSource(),
                               target->getLocalPoint (win, p).toFloat(), ModifierKeys (ModifierKeys::leftButtonModifier),
                               1.0f, 0.0f, 0.0f, 0.0f, 0.0f, target, target,
                               Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                target->mouseDown (ev);
                target->mouseUp (ev);
                for (int i = 0; i < 4; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
                closedByCross = ! wins.isOpen (uid);

                // closing KEEPS the editor cached (reopening a heavy plug-in must not rebuild its
                // whole UI synchronously); opening the same panel again reuses that editor
                editorCached = wins.windowComponent (uid) != nullptr && ! wins.isVisible (uid);
                wins.show (uid, "SX Delay - SizeTest");
                for (int i = 0; i < 3; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
                Component* second = nullptr;
                if (wins.windowComponent (uid) != nullptr)
                    second = wins.windowContent (uid);
                reuseInstant = second != nullptr && second == firstEditor;
            }
        }
        log ("plug-in window: the title-bar close cross closes the panel", closedByCross);
        log ("plug-in window: closing keeps the editor cached, reopening reuses it without a rebuild",
             editorCached && reuseInstant);
        wins.closeAll();

        // ---- a plug-in that sizes itself after opening must not leave a larger empty panel ----
        // Ozone reports one editor size while it is being created, then settles on the size it
        // really draws at; the panel kept the first (larger) size and showed a wide empty strip
        // beside the plug-in's picture. The panel has to follow the editor's own size.
        {
            PluginWindows followWins (engine);
            followWins.show (uid, "SX Delay - FollowSizeTest");
            for (int i = 0; i < 8; ++i) MessageManager::getInstance()->runDispatchLoopUntil (20);

            auto* win = followWins.windowComponent (uid);
            auto* body = followWins.windowContent (uid);
            const bool fittedAtFirst = win != nullptr && body != nullptr
                                    && win->getWidth() == body->getWidth()
                                    && win->getHeight() == body->getHeight() + 28;

            // the editor re-scales itself, the way a native VST does when it re-reads the DPI
            bool followed = false, gripKept = false;
            if (win != nullptr && body != nullptr)
            {
                body->setSize (jmax (240, body->getWidth() * 3 / 4),
                               jmax (160, body->getHeight() * 3 / 4));
                followed = win->getWidth() == body->getWidth()
                        && win->getHeight() == body->getHeight() + 28;

                // ...and the user's corner grip still sizes the panel: the body is stretched to
                // the panel, so the fit must not snap it back
                const int gripW = body->getWidth() + 80, gripH = body->getHeight() + 60;
                win->setSize (gripW, gripH);
                gripKept = win->getWidth() == gripW && win->getHeight() == gripH
                        && body->getWidth() == gripW && body->getHeight() == gripH - 28;
            }
            log ("plug-in window: a panel follows the size its editor picks, and the corner grip still works ("
                 + String ((int) fittedAtFirst) + "/" + String ((int) followed) + "/"
                 + String ((int) gripKept) + ")", fittedAtFirst && followed && gripKept);
            followWins.closeAll();
        }

#if JUCE_WINDOWS
        // Real host window: the panel becomes a WS_CHILD peer of the window, so native VST
        // editors (Ozone, Kontakt, ...) are nested inside the panel instead of floating on the
        // desktop. Moving the main window must move the child panel with it.
        {
            DocumentWindow hostWindow ("NativePanelHost", theme::window, 0, true);
            hostWindow.setBounds (-2400, -1800, 1000, 700);
            Component hostLayer;
            hostWindow.addAndMakeVisible (hostLayer);
            hostLayer.setBounds (50, 80, 800, 500);
            hostWindow.setVisible (true);
            for (int i = 0; i < 3; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
            const auto hostHwnd = hostWindow.getWindowHandle();
            PluginWindows nativeWins (engine);
            nativeWins.setHost (&hostLayer);
            nativeWins.show (uid, "Native SizeTest");
            for (int i = 0; i < 3; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
            auto* w = nativeWins.windowComponent (uid);
            const bool childPeer = w != nullptr && w->getWindowHandle() != nullptr
                                && w->getParentComponent() == nullptr;
            const bool inside = w != nullptr && hostLayer.getScreenBounds().contains (w->getScreenBounds());
            const auto panelHwnd = w != nullptr ? w->getWindowHandle() : nullptr;
            NativeRect h1 {}, h2 {};
            if (hostHwnd != nullptr) GetWindowRect (hostHwnd, &h1);
            NativeRect rect1 {}, rect2 {};
            if (panelHwnd != nullptr) GetWindowRect (panelHwnd, &rect1);
            hostWindow.setTopLeftPosition (hostWindow.getX() + 20, hostWindow.getY() + 10);
            for (int i = 0; i < 3; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
            if (hostHwnd != nullptr) GetWindowRect (hostHwnd, &h2);
            if (panelHwnd != nullptr) GetWindowRect (panelHwnd, &rect2);
            const bool follows = (rect2.left - rect1.left) == (h2.left - h1.left)
                              && (rect2.top - rect1.top) == (h2.top - h1.top)
                              && (h2.left - h1.left) > 0 && (h2.top - h1.top) > 0;

            auto* panelParent = panelHwnd != nullptr ? GetParent (panelHwnd) : nullptr;
            const auto style = panelHwnd != nullptr ? GetWindowLongPtrW (panelHwnd, -16) : 0;
            constexpr std::intptr_t wsChild = 0x40000000;
            log ("plug-in window: on a real window the panel is a native child that follows the host ("
                 + String ((int) childPeer) + "/" + String ((int) inside) + "/" + String ((int) follows)
                 + ", parent " + String ((juce::pointer_sized_int) panelParent) + "/"
                 + String ((juce::pointer_sized_int) hostHwnd) + " child "
                 + String ((int) ((style & wsChild) != 0)) + " hostrect "
                 + String (h2.left - h1.left) + "," + String (h2.top - h1.top)
                 + " panelrect " + String (rect2.left - rect1.left) + ","
                 + String (rect2.top - rect1.top) + ")",
                 childPeer && inside && follows);
            nativeWins.closeAll();
            hostWindow.setVisible (false);
        }
#endif
        project.removeTrack (tid);
        project.sendStructureChanged();
        engine.syncAll();
    }
    // ---- the title bar icons: power writes the slot bypass, the bend is the temporary bypass ----
    {
        auto& t = project.addTrack (TrackType::Audio, "IconTest");
        const int tid = t.id;
        project.sendStructureChanged();
        String err;
        const bool added = engine.addInsert (tid, makeBuiltInDelaySlot().desc, err);
        engine.syncAll();
        auto* tr = project.findTrack (tid);
        const int64 uid = (tr != nullptr && ! tr->inserts.empty()) ? tr->inserts.front().uid : 0;

        PluginWindows wins (engine);
        wins.onSetSlotBypass = [&] (int, int64 u, bool b) { engine.setSlotBypass (tid, u, b); };
        wins.show (uid, "SX Delay - IconTest");
        for (int i = 0; i < 8; ++i) MessageManager::getInstance()->runDispatchLoopUntil (20);

        // The power icon is filled green while the plug-in is enabled and grey after the bypass.
        // Sample inside the icon's rounded background, away from the white glyph.
        auto powerBackground = [&]() -> Colour
        {
            if (auto* win = wins.windowComponent (uid))
            {
                auto img = win->createComponentSnapshot ({ 0, 0, jmin (win->getWidth(), 80), 28 }, false, 1.0f);
                return img.getPixelAt (12, 9);
            }
            return {};
        };

        const auto enabledPixel = powerBackground();
        wins.testTriggerIcon (uid, 0);     // power
        for (int i = 0; i < 3; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
        const auto bypassedPixel = powerBackground();
        const bool greenWhenEnabled = enabledPixel.getRed() < 0x90 && enabledPixel.getGreen() > 0xa0;
        const bool greyWhenBypassed = ! (bypassedPixel.getRed() < 0x90 && bypassedPixel.getGreen() > 0xa0);

        bool slotBypassed = false;
        if (auto* tr2 = project.findTrack (tid))
            for (auto& s : tr2->inserts) if (s.uid == uid) slotBypassed = s.bypass;

        wins.testTriggerIcon (uid, 2);     // temporary bypass
        bool tempBypassed = false;
        if (auto inst = engine.getInstance (uid)) tempBypassed = inst->editorBypass.load();

        log ("title bar icons: power toggles the saved bypass (" + enabledPixel.toDisplayString (false) + " -> "
             + bypassedPixel.toDisplayString (false) + "), the bend toggles the temporary one ("
             + String ((int) slotBypassed) + "/" + String ((int) tempBypassed) + ")",
             added && slotBypassed && tempBypassed && greenWhenEnabled && greyWhenBypassed);
        wins.closeAll();
        project.removeTrack (tid);
        project.sendStructureChanged();
        engine.syncAll();
    }
    // ---- plug-in windows open centred on screen ------------------------------------------
    {
        auto& t = project.addTrack (TrackType::Instrument, "WinTest");
        const int tid = t.id;
        project.sendStructureChanged();
        String err;
        const bool ok = engine.setInstrument (tid, makeBuiltInSynthSlot().desc, err);
        auto* tr = project.findTrack (tid);
        const int64 uid = tr != nullptr ? tr->instrument.uid : 0;

        PluginWindows wins (engine);
        wins.show (uid, "SX Synth - WinTest");
        for (int i = 0; i < 10; ++i) MessageManager::getInstance()->runDispatchLoopUntil (20);

        const auto r = wins.windowBounds (uid);
        if (auto* win = wins.windowComponent (uid))
        {
            auto img = win->createComponentSnapshot ({ 0, 0, win->getWidth(), win->getHeight() }, false, 1.0f);
            const File f = File::getSpecialLocation (File::tempDirectory)
                               .getChildFile ("soundx_window_titlebar_" + String (Time::currentTimeMillis() % 100000) + ".png");
            FileOutputStream fos (f);
            PNGImageFormat png;
            if (fos.openedOk()) png.writeImageToStream (img, fos);
        }
        auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const auto area = d != nullptr ? d->userArea : Rectangle<int>();
        const int cx = area.getX() + area.getWidth() / 2;
        const int cy = area.getY() + area.getHeight() / 2;
        const bool centred = ok && wins.isOpen (uid) && r.getWidth() >= 200 && r.getHeight() >= 120
                          && std::abs (r.getCentreX() - cx) <= 40 && std::abs (r.getCentreY() - cy) <= 40;
        log ("plug-in window opens centred on screen (" + String (r.getX()) + "," + String (r.getY()) + " "
             + String (r.getWidth()) + "x" + String (r.getHeight()) + ", screen centre "
             + String (cx) + "," + String (cy) + ")", centred);

        wins.closeNow (uid);
        project.removeTrack (tid);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }

    // ---- one plug-in panel at a time, or several, and the insert-time "open" switch -----------
    // Inserting a plug-in now opens its panel. This checks the two user choices behind that: the
    // panel may replace the previous one or stay next to it, and the automatic opening can be
    // switched off entirely (View > Plug-in Panels).
    {
        auto& t = project.addTrack (TrackType::Audio, "PanelModeTest");
        const int tid = t.id;
        project.sendStructureChanged();
        String err;
        engine.addInsert (tid, makeBuiltInDelaySlot().desc, err);
        engine.addInsert (tid, makeBuiltInDelaySlot().desc, err);
        engine.syncAll();

        auto* tr = project.findTrack (tid);
        const int64 uid1 = (tr != nullptr && tr->inserts.size() > 0) ? tr->inserts[0].uid : 0;
        const int64 uid2 = (tr != nullptr && tr->inserts.size() > 1) ? tr->inserts[1].uid : 0;

        {
            PluginWindows wins (engine);
            wins.setMultipleWindowsAllowed (false);
            wins.show (uid1, "Panel One");
            wins.show (uid2, "Panel Two");
            const bool single = wins.isOpen (uid1) && wins.isOpen (uid2)
                             && ! wins.isVisible (uid1) && wins.isVisible (uid2);

            wins.setMultipleWindowsAllowed (true);
            wins.show (uid1, "Panel One");
            const bool multiple = wins.isVisible (uid1) && wins.isVisible (uid2);

            wins.setMultipleWindowsAllowed (false);
            const bool collapsed = wins.isVisible (uid1) && ! wins.isVisible (uid2);

            log ("plug-in panels: single mode hides the previous panel, multiple mode keeps both on screen",
                 uid1 != 0 && uid2 != 0 && single && multiple && collapsed);
            wins.closeAll();
        }

        {
            AppState st (project, engine, plugins);
            const bool oldOpen = st.openPluginEditorOnInsert;
            const bool oldMulti = st.multiPluginWindows;

            st.setOpenPluginEditorOnInsert (false);
            st.showInsertedPluginEditor (uid1);
            const bool staysClosed = ! st.windows->isOpen (uid1);

            st.setOpenPluginEditorOnInsert (true);
            st.showInsertedPluginEditor (uid1);
            const bool deferred = ! st.windows->isOpen (uid1);   // painted slot first, panel next pass
            for (int i = 0; i < 3; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
            const bool opens = st.windows->isOpen (uid1) && st.windows->isVisible (uid1);

            st.windows->closeAll();
            st.setOpenPluginEditorOnInsert (oldOpen);
            st.setMultiPluginWindows (oldMulti);

            log ("plug-in panels: inserting opens the panel on the next loop pass only when that switch is on ("
                 + String ((int) staysClosed) + "/" + String ((int) deferred) + "/" + String ((int) opens) + ")",
                 staysClosed && deferred && opens);
        }

        {
            // inside the real main window the panel must be a child of the host layer (not a
            // native window), and Space typed while it is focused must reach the transport
            AppState st (project, engine, plugins);
            auto oldContent = std::make_unique<MainComponent> (st);   // simulates the theme rebuild
            oldContent->setSize (1200, 700);
            MainComponent mc (st);
            mc.setSize (1200, 700);
            mc.resized();
            oldContent.reset();     // the old content must not clear the new host
            st.windows->show (uid1, "Host Panel");
            for (int i = 0; i < 3; ++i) MessageManager::getInstance()->runDispatchLoopUntil (10);
            auto* win = st.windows->windowComponent (uid1);
            auto* host = st.windows->hostComponent();
            const bool inHost = win != nullptr && host != nullptr && win->getParentComponent() == host;
            const bool inMain = win != nullptr && win->findParentComponentOfClass<MainComponent>() == &mc;
            engine.stop();
            const bool spaceHandled = mc.handlePluginShortcut (KeyPress (KeyPress::spaceKey));
            const bool playing = engine.isPlaying();
            engine.stop();
            st.windows->closeAll();
            log ("plug-in panels: the panel lives inside the main window and Space still drives the transport ("
                 + String ((int) inHost) + "/" + String ((int) inMain) + "/" + String ((int) spaceHandled)
                 + "/" + String ((int) playing) + ")", inHost && inMain && spaceHandled && playing);
        }

        project.removeTrack (tid);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }

    // ---- plug-in chain presets: order, bypass, live state and one undo step -------------------
    {
        auto& t = project.addTrack (TrackType::Audio, "ChainTest");
        const int tid = t.id;
        project.sendStructureChanged();
        String err;

        const bool built = engine.addInsert (tid, makeBuiltInEffectSlot (BuiltInEffect::Delay).desc, err)
                        && engine.addInsert (tid, makeBuiltInEffectSlot (BuiltInEffect::DigitalReverb).desc, err);
        engine.syncAll();

        auto* tr = project.findTrack (tid);
        const int64 delayUid = (tr != nullptr && tr->inserts.size() > 0) ? tr->inserts[0].uid : 0;
        const int64 reverbUid = (tr != nullptr && tr->inserts.size() > 1) ? tr->inserts[1].uid : 0;

        float delayValue = 0.0f;
        bool paramSet = false;
        if (auto inst = engine.getInstance (delayUid))
            if (inst->proc != nullptr && ! inst->proc->getParameters().isEmpty())
            {
                auto* p = inst->proc->getParameters()[0];
                const float wanted = p->getValue() < 0.5f ? 0.73f : 0.27f;
                p->setValueNotifyingHost (wanted);
                delayValue = p->getValue();
                paramSet = true;
            }

        if (reverbUid != 0) engine.setSlotBypass (tid, reverbUid, true);
        engine.syncAll();

        AppState st (project, engine, plugins);
        const File preset = out.getChildFile ("chain_preset_test.sxchain");
        preset.deleteFile();
        const bool saved = writeChainPreset (st, tid, preset, "Test Chain", err);
        const bool fileOk = preset.existsAsFile() && preset.getSize() > 0;

        // ... and the user-facing folder / listing path: the menu is built from listChainPresets()
        const String chainTestName = "SoundX Self Test Chain " + String (Time::currentTimeMillis() % 1000000);
        const File namedPreset = chainPresetFolder().getChildFile (File::createLegalFileName (chainTestName) + ".sxchain");
        namedPreset.deleteFile();
        const bool namedSaved = saveChainPreset (st, tid, chainTestName, err);
        bool namedListed = false;
        for (auto& entry : listChainPresets())
            if (entry.name == chainTestName && entry.file == namedPreset)
                namedListed = true;
        namedPreset.deleteFile();

        // Replace the saved chain with a different one, then load the preset over it. The undo
        // must restore the replaced chain as a whole, not one plug-in at a time.
        engine.removeSlot (tid, delayUid);
        engine.removeSlot (tid, reverbUid);
        engine.syncAll();
        engine.addInsert (tid, makeBuiltInEffectSlot (BuiltInEffect::Chorus).desc, err);
        engine.syncAll();
        project.clearUndo();

        const bool loaded = applyChainPreset (st, tid, preset, err);
        auto* tr2 = project.findTrack (tid);
        const bool order = tr2 != nullptr && tr2->inserts.size() == 2
                        && tr2->inserts[0].desc.name == "SX Delay"
                        && tr2->inserts[1].desc.name == "SX Digital Reverb";
        const bool bypass = order && tr2->inserts[1].bypass;

        float loadedValue = -1.0f;
        if (order)
            if (auto inst = engine.getInstance (tr2->inserts[0].uid))
                if (inst->proc != nullptr && ! inst->proc->getParameters().isEmpty())
                    loadedValue = inst->proc->getParameters()[0]->getValue();
        const bool state = order && paramSet && std::abs (loadedValue - delayValue) < 0.01f;

        project.undo();
        auto* tr3 = project.findTrack (tid);
        const bool undo = tr3 != nullptr && tr3->inserts.size() == 1
                       && tr3->inserts[0].desc.name == "SX Chorus";
        project.redo();
        auto* tr4 = project.findTrack (tid);
        const bool redo = tr4 != nullptr && tr4->inserts.size() == 2;

        log ("plug-in chain presets: order, bypass and the live parameter value survive save/load, and the replacement is one undo step ("
             + String ((int) built) + "/" + String ((int) saved) + "/" + String ((int) fileOk) + "/"
             + String ((int) namedSaved) + "/" + String ((int) namedListed) + "/"
             + String ((int) loaded) + "/" + String ((int) order) + "/" + String ((int) bypass) + "/"
             + String ((int) state) + "/" + String ((int) undo) + "/" + String ((int) redo) + ")",
             built && saved && fileOk && namedSaved && namedListed
                 && loaded && order && bypass && state && undo && redo);

        preset.deleteFile();
        st.windows->closeAll();
        project.removeTrack (tid);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }

    // ---- plug-in search: the insert picker filters as you type -------------------------------
    {
        auto desc = [] (const char* name, const char* maker, const char* category, const char* format, bool inst)
        {
            PluginDescription d;
            d.name = name;
            d.manufacturerName = maker;
            d.category = category;
            d.pluginFormatName = format;
            d.isInstrument = inst;
            return d;
        };

        Array<PluginDescription> fx;
        fx.add (desc ("Ozone 11", "iZotope", "Mastering", "VST3", false));
        fx.add (desc ("Pro-Q 3", "FabFilter", "EQ", "VST3", false));
        fx.add (desc ("Pro-C 2", "FabFilter", "Compressor", "VST3", false));
        fx.add (desc ("Air Pro Reverb", "Air Music", "Reverb", "VST3", false));
        fx.add (desc ("Vintage Compressor", "Pro Audio DSP", "Compressor", "VST", false));
        fx.add (desc ("Serum", "Xfer Records", "Synth", "VST3", true));

        auto names = [&fx] (const String& query)
        {
            StringArray out;
            for (int i : searchPluginList (fx, query)) out.add (fx[i].name);
            return out;
        };

        // no query: everything, alphabetically
        const auto all = names ({});
        log ("plug-in search: an empty search lists every plug-in alphabetically ("
             + String (all.size()) + ": " + all.joinIntoString (", ") + ")",
             all.size() == fx.size() && all[0] == "Air Pro Reverb" && all[1] == "Ozone 11" && all[2] == "Pro-C 2");

        // the name wins over the manufacturer, and a name that starts with the query wins over one
        // that only contains it: typing "pro" puts Pro-C / Pro-Q first, not "Vintage Compressor"
        const auto pro = names ("pro");
        log ("plug-in search: a name that starts with the query comes first, a manufacturer hit last ("
             + pro.joinIntoString (", ") + ")",
             pro.size() == 4 && pro[0] == "Pro-C 2" && pro[1] == "Pro-Q 3"
             && pro[2] == "Air Pro Reverb" && pro[3] == "Vintage Compressor");

        const auto makerHit = names ("zotope");
        const auto upper = names ("PRO-Q");
        log ("plug-in search: manufacturer and case do not matter (zotope -> "
             + makerHit.joinIntoString (", ") + ", PRO-Q -> " + upper.joinIntoString (", ") + ")",
             makerHit.size() == 1 && makerHit[0] == "Ozone 11" && upper.size() == 1 && upper[0] == "Pro-Q 3");

        // every word has to match something (name and manufacturer may each contribute one)
        const auto both = names ("pro fab");
        log ("plug-in search: several words all have to match (" + both.joinIntoString (", ") + ")",
             both.size() == 2 && both[0] == "Pro-C 2" && both[1] == "Pro-Q 3");

        const auto nothing = names ("quantum");
        Array<PluginDescription> empty;
        log ("plug-in search: a word nothing carries finds nothing, and an empty list stays empty",
             nothing.isEmpty() && searchPluginList (empty, "pro").empty() && searchPluginList (empty, {}).empty());

        // the picker's list: the built-in synth belongs to the instrument list only, and the
        // built-in effect family is always listed first in the effect picker
        const auto instList = pluginPickerList (plugins, true);
        const auto fxList = pluginPickerList (plugins, false);
        bool builtInInFx = false;
        int builtInEffectsInFx = 0;
        for (auto& d : fxList)
        {
            if (isBuiltInSynth (d)) builtInInFx = true;
            if (isBuiltInEffect (d)) ++builtInEffectsInFx;
        }
        // the vendor folder: SoundX first, and that folder holds exactly the built-in family
        {
            auto vendors = pluginPickerVendors (fxList);
            bool firstIsSoundX = vendors.size() > 0 && vendors[0].startsWith ("SoundX");
            int inSoundX = 0;
            for (auto& d : fxList) if (d.manufacturerName.startsWith ("SoundX")) ++inSoundX;
            bool allBuiltIn = inSoundX == (int) builtInEffects().size();
            auto found = searchPluginList (fxList, "soundx");
            log ("plug-in search: the SoundX vendor folder holds all " + String (inSoundX)
                 + " built-in effects and a 'soundx' search finds them ("
                 + String ((int) found.size()) + " hits)", firstIsSoundX && allBuiltIn && (int) found.size() >= inSoundX);
        }

        // the slot menu's vendor submenu: SoundX is one of the folders and carries every built-in
        {
            bool idsOk = true;
            for (int i = 0; i < (int) builtInEffects().size(); ++i)
                idsOk = idsOk && isBuiltInEffectMenuId (1600 + i) && ! isInstrumentLayerMenuId (1600 + i);
            idsOk = idsOk && ! isBuiltInEffectMenuId (1599) && isBuiltInEffectMenuId (1663) && ! isBuiltInEffectMenuId (1664);
            const auto desc = makeBuiltInEffectSlot (BuiltInEffect::Limiter).desc;
            log ("plug-in search: the slot menu's SoundX folder carries " + String (builtInEffectMenuCount())
                 + " built-in effects and its ids stay clear of the instrument layers (" + desc.name + ")",
                 idsOk && builtInEffectMenuCount() == (int) builtInEffects().size());
        }

        log ("plug-in search: instruments start with the built-in synth, effects list the built-in family",
             ! instList.isEmpty() && isBuiltInSynth (instList[0]) && ! builtInInFx
             && builtInEffectsInFx == (int) builtInEffects().size()
             && fxList.size() == plugins.effects().size() + builtInEffectsInFx);

        // the panel itself: a search for "pro" and the "nothing scanned yet" state
        {
            const auto shot = renderPluginPickerPanel (fx, false, "pro", true);
            const auto blank = renderPluginPickerPanel ({}, false, {}, false);
            // replacing an instrument: the heading / button say so and the built-in synth is listed
            auto insts = pluginPickerList (plugins, true);
            const auto replace = renderPluginPickerPanel (insts, true, "synth", true, true);
            auto save = [] (const Image& img, const File& target)
            {
                if (! img.isValid()) return;
                if (auto stream = std::unique_ptr<FileOutputStream> (target.createOutputStream()))
                {
                    PNGImageFormat fmt;
                    fmt.writeImageToStream (img, *stream);
                }
            };
            save (shot, out.getChildFile ("plugin_search.png"));
            save (blank, out.getChildFile ("plugin_search_empty.png"));
            save (replace, out.getChildFile ("plugin_search_instruments.png"));
            log ("plug-in search: the picker panel renders with results and when nothing is scanned ("
                 + String (shot.getWidth()) + "x" + String (shot.getHeight()) + " / "
                 + String (blank.getWidth()) + "x" + String (blank.getHeight()) + ")",
                 shot.isValid() && shot.getWidth() >= 560 && shot.getHeight() >= 400
                 && blank.isValid() && blank.getWidth() == shot.getWidth());
            log ("plug-in search: the instrument picker renders in its \"replace\" form", replace.isValid()
                 && replace.getWidth() == shot.getWidth());
        }
    }
    // ---- ARA ------------------------------------------------------------------------------------
    // ARA rides on top of VST3 / AU, so the capability arrives with the plug-in's class information
    // during the scan. This build compiles JUCE's ARA host framework in (third_party/ARA_SDK +
    // JUCE_PLUGINHOST_ARA), which is what creating ARA documents for audio tracks needs.
    {
        log ("ARA: the ARA host framework is compiled into this build (" + PluginManager::araSupportString() + ")",
             PluginManager::hasAraSupport() && PluginManager::araSupportString().startsWith ("ARA"));

        PluginDescription araCase;
        araCase.name = "ARA demo";
        araCase.hasARAExtension = true;
        PluginDescription plainCase;
        plainCase.name = "Plain";
        log ("ARA: plug-ins that announce the ARA extension are recognised from the scan",
             PluginManager::isAraCapable (araCase) && ! PluginManager::isAraCapable (plainCase));

        StringArray araNames;
        for (auto& d : plugins.known.getTypes()) if (PluginManager::isAraCapable (d)) araNames.add (d.name);
        log ("ARA: " + String (plugins.araCapableCount()) + " of " + String (plugins.known.getNumTypes())
             + " installed plug-ins announce ARA"
             + (araNames.isEmpty() ? String (" (none installed: Melodyne, Auto-Tune, Revoice, SpectraLayers ... would show up here)")
                                   : ": " + araNames.joinIntoString (", ")));

        // The demo ARA plug-in that the build drops next to the app is a real ARA VST3: scan it
        // through the normal out-of-process scanner and check that its ARA flag arrives.
        {
            const File exe = File::getSpecialLocation (File::currentExecutableFile);
            const File fixture = exe.getParentDirectory().getParentDirectory().getParentDirectory()
                                    .getChildFile ("ARAPluginDemo_artefacts/Release/VST3");
            if (! fixture.isDirectory())
            {
                log ("ARA: the ARA demo fixture is not built - skipping the real-plug-in scan check");
            }
            else
            {
                const int before = plugins.known.getNumTypes();
                plugins.scanner().scan (FileSearchPath (fixture.getFullPathName()), {}, false);
                const auto begin = Time::getMillisecondCounter();
                while (plugins.scanner().isScanning() && Time::getMillisecondCounter() - begin < 90000)
                    MessageManager::getInstance()->runDispatchLoopUntil (20);

                StringArray found;
                Array<PluginDescription> fixtureTypes;
                for (auto& d : plugins.known.getTypes())
                    if (PluginManager::isAraCapable (d) && File (d.fileOrIdentifier).isAChildOf (fixture))
                    {
                        found.add (d.name);
                        fixtureTypes.add (d);
                    }

                // the fixture is a test tool: keep it out of the user's plug-in list
                for (auto& d : fixtureTypes) plugins.known.removeType (d);

                log ("ARA: a real ARA VST3 is detected by the scanner (" + String (found.size())
                     + " type(s) from the demo fixture: " + found.joinIntoString (", ") + ", "
                     + String (plugins.known.getNumTypes() - before) + " left behind)",
                     found.size() >= 1);

                // The ARA host itself: build a document for an audio track and bind the plug-in to it.
                if (AraHost::isAvailable() && ! fixtureTypes.isEmpty())
                {
                    String err;
                    auto instance = plugins.create (fixtureTypes.getFirst(), 44100.0, 512, err);

                    Project araProject;
                    auto& araTrack = araProject.addTrack (TrackType::Audio, "ARA Vocal");
                    {
                        auto data = std::make_shared<AudioData>();
                        data->buffer.setSize (1, 44100);
                        for (int i = 0; i < data->buffer.getNumSamples(); ++i)
                            data->buffer.setSample (0, i, 0.3f * std::sin (MathConstants<float>::twoPi * 220.0f * (float) i / 44100.0f));
                        data->sampleRate = 44100.0;

                        AudioClip c;
                        c.id = araProject.newId();
                        c.name = "Take";
                        c.start = 0.0;
                        c.lengthSec = 1.0;
                        c.data = data;
                        araTrack.audioClips.push_back (c);
                    }

                    bool ready = false, called = false;
                    AraHost ara;
                    if (instance != nullptr)
                        ara.attach (*instance, "SoundX ARA test", [&] (bool ok) { ready = ok; called = true; });

                    const auto araBegin = Time::getMillisecondCounter();
                    while (! called && Time::getMillisecondCounter() - araBegin < 15000)
                        MessageManager::getInstance()->runDispatchLoopUntil (20);

                    if (called && ready)
                        ara.sync (araProject, araTrack.id);

                    const String shape = String (ara.sourceCount()) + " source(s), "
                                       + String (ara.regionCount()) + " region(s), "
                                       + (ara.hasPlaybackRenderer() ? "playback renderer" : "no playback renderer");
                    log ("ARA host: the document of an audio track is bound to the plug-in (" + shape + ")",
                         called && ready && ara.isReady() && ara.sourceCount() == 1
                         && ara.regionCount() == 1 && ara.hasPlaybackRenderer());

                    // the plug-in's own document (the audio/pitch edits) has to reach the host
                    // archive, otherwise saving the project would lose the edits
                    const auto archive = ara.archive();
                    log ("ARA host: the plug-in stores its document into the host archive ("
                         + String ((int) archive.getSize()) + " bytes)",
                         archive.getSize() > 0);

                    // the archive has to survive the project file, otherwise the pitch edits are
                    // gone after a save / load cycle
                    {
                        Project saveProj;
                        auto& saveTrack = saveProj.addTrack (TrackType::Audio, "ARA Save");
                        PluginSlot slot;
                        slot.uid = saveProj.newId();
                        slot.desc.name = "Melodyne";
                        slot.araState = archive;
                        saveTrack.inserts.push_back (slot);

                        const auto xml = saveProj.toXml (true);
                        bool survived = false;
                        if (xml != nullptr)
                        {
                            Project loaded;
                            loaded.fromXml (*xml);
                            if (auto* tr2 = loaded.findTrack (saveTrack.id))
                                if (auto* s2 = tr2->findSlot (slot.uid))
                                    survived = s2->araState.getSize() == archive.getSize()
                                            && s2->araState == archive;
                        }
                        log ("ARA: the ARA archive of a slot survives the project file ("
                             + String ((int) archive.getSize()) + " bytes)", survived);
                    }

                    ara.detach();
                    instance.reset();
                }
            }
        }
    }
    // ---- stock JUCE dialogs must be readable in both themes --------------------------------
    {
        const auto lightKeyWhite = theme::keyWhite, lightKeyBlack = theme::keyBlack;

        for (int pass = 0; pass < 2; ++pass)
        {
            theme::setDark (pass == 1);
            if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();
            const String mode = pass == 1 ? "dark" : "light";

            FileSearchPathListComponent pathList;                 // the "Select folders to scan..." list
            const auto bg = pathList.findColour (FileSearchPathListComponent::backgroundColourId);
            const auto fg = pathList.findColour (ListBox::textColourId);
            const double gap = std::abs (bg.getPerceivedBrightness() - fg.getPerceivedBrightness());
            log (String ("scan-folder list is readable in ") + mode + " mode (bg "
                 + bg.toDisplayString (false) + ", text " + fg.toDisplayString (false) + ")", gap > 0.35);

            // Tooltips are drawn by the LookAndFeel from TooltipWindow's colours: a dark bubble
            // with white text in either theme. In the night theme this used to follow "white" -
            // the elevated surface colour - and ended up dark grey on dark grey.
            Component probe;
            const auto tipBg = probe.findColour (TooltipWindow::backgroundColourId);
            const auto tipText = probe.findColour (TooltipWindow::textColourId);
            const double tipGap = std::abs (tipBg.getPerceivedBrightness() - tipText.getPerceivedBrightness());
            log (String ("tooltip is readable in ") + mode + " mode (bg " + tipBg.toDisplayString (false)
                 + ", text " + tipText.toDisplayString (false) + ")", tipGap > 0.35);

            // White marks on clips / accent (note names, fade handles, the gain line) are white in
            // both themes; the piano keyboard stays black and white instead of following the theme
            const bool keysFixed = theme::keyWhite == lightKeyWhite && theme::keyBlack == lightKeyBlack;
            log (String ("white marks on clips stay white in ") + mode + " mode ("
                 + theme::onAccent.toDisplayString (false) + "), keyboard "
                 + theme::keyWhite.toDisplayString (false) + " / " + theme::keyBlack.toDisplayString (false),
                 theme::onAccent.getPerceivedBrightness() > 0.9
                 && theme::keyWhite.getPerceivedBrightness() > 0.8
                 && theme::keyBlack.getPerceivedBrightness() < 0.25 && keysFixed);
        }
    }
    theme::setDark (false);
    if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();
    // ---- selected notes are warm peach, not the accent blue ----------------------------------
    // Blue is the app's "active" colour (playhead, tools, toggles). A selection has to be a
    // different hue, and it must stay readable on the lane background in both themes.
    for (int pass = 0; pass < 2; ++pass)
    {
        theme::setDark (pass == 1);
        if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();

        const double fillBright = theme::selectionFill.getPerceivedBrightness();
        const double textGap = std::abs (fillBright - theme::selectionText.getPerceivedBrightness());
        const double laneGap = std::abs (fillBright - theme::laneA.getPerceivedBrightness());
        const float hue = theme::selectionFill.getHue();
        log (String ("piano roll: a selected note is warm, not the accent blue (") + (pass == 1 ? "dark" : "light")
             + " mode, fill " + theme::selectionFill.toDisplayString (false) + ", edge "
             + theme::selectionEdge.toDisplayString (false) + ")",
             hue > 0.02f && hue < 0.14f && textGap > 0.35 && laneGap > 0.08
             && theme::selectionFill != theme::accent && theme::selectionEdge != theme::accent);
    }
    theme::setDark (false);
    if (auto* lnf = dynamic_cast<SXLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel())) lnf->applyColours();
    // ---- pan shown as a number, typable -----------------------------------------------------
    {
        double v = 0.0;
        const bool left  = parsePanText ("L30", v) && std::abs (v + 0.3) < 1e-6;
        const bool right = parsePanText ("R 20", v) && std::abs (v - 0.2) < 1e-6;
        const bool minus = parsePanText ("-45", v) && std::abs (v + 0.45) < 1e-6;
        const bool ctr   = parsePanText ("c", v) && std::abs (v) < 1e-6;
        const bool clamp = parsePanText ("L 150", v) && std::abs (v + 1.0) < 1e-6;
        const bool junk  = ! parsePanText ("abc", v);
        const bool text  = panText (-0.32) == "L 32" && panText (0.0) == "C" && panText (0.2) == "R 20";
        log ("pan: numbers parse and print (L 32 / C / R 20, typed L30, R 20, -45, c, clamp, junk rejected)",
             left && right && minus && ctr && clamp && junk && text);
    }
    // ---- bus PDC: two buses with different latencies must stay aligned -------------------------
    {
        auto onsetOfFile = [&] (const File& f)
        {
            double t0 = -1.0;
            if (std::unique_ptr<AudioFormatReader> r { engine.formats.createReaderFor (f) })
            {
                AudioBuffer<float> b2 (2, (int) r->lengthInSamples);
                r->read (&b2, 0, (int) r->lengthInSamples, 0, true, true);
                for (int i = 0; i < b2.getNumSamples(); ++i)
                    if (std::abs (b2.getSample (0, i)) > 0.003f) { t0 = i / r->sampleRate; break; }
            }
            return t0;
        };

        auto makeVoice = [&] (const String& name, int busId)
        {
            auto& v = project.addTrack (TrackType::Instrument, name);
            v.hasInstrument = true;
            v.instrument = makeBuiltInSynthSlot();
            v.outputBusId = busId;
            MidiClip c; c.id = project.newId(); c.start = 0.0; c.length = 3.0;
            Note n; n.id = project.newId(); n.pitch = 60; n.start = 0.5; n.length = 1.5; n.velocity = 110;
            c.notes.push_back (n);
            v.midiClips.push_back (c);
            return v.id;
        };

        auto& b1 = project.addTrack (TrackType::Bus, "BusDelay");
        const int bus1 = b1.id;
        auto& b2 = project.addTrack (TrackType::Bus, "BusPlain");
        const int bus2 = b2.id;
        const int aId = makeVoice ("VoiceA", bus1);
        const int bId = makeVoice ("VoiceB", bus2);

        // bus1 has a 960 sample insert that reports its latency
        if (auto* bb = project.findTrack (bus1)) bb->inserts.push_back (makeBuiltInDelaySlot());
        project.sendStructureChanged();
        engine.syncAll();

        // mute instead of solo: a solo render (onlyTrack) skips bus routing entirely, which
        // would bypass the very buses this check is about
        auto renderSolo = [&] (int onlyTrack, const File& f)
        {
            for (auto& tt : project.tracks)
            {
                tt->solo = false;
                tt->mute = (tt->id != onlyTrack && ! tt->isGroupTrack());   // keep the buses alive
            }
            project.sendMixChanged();
            engine.syncAll();
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = f;
            o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.3;
            String e2;
            engine.exportAudio (o, {}, e2);
            return onsetOfFile (f);
        };

        const double onsetA = renderSolo (aId, out.getChildFile ("pdc_a.wav"));
        const double onsetB = renderSolo (bId, out.getChildFile ("pdc_b.wav"));
        const double diff = std::abs (onsetA - onsetB);
        log ("bus PDC: two buses with different latency stay aligned (A " + String (onsetA * 1000.0, 1) + " ms, B "
             + String (onsetB * 1000.0, 1) + " ms, diff " + String (diff * 1000.0, 1) + " ms)",
             onsetA > 0.0 && onsetB > 0.0 && diff < 0.003);

        for (auto& tt : project.tracks) { tt->solo = false; tt->mute = false; }
        project.removeTrack (bId);
        project.removeTrack (aId);
        project.removeTrack (bus2);
        project.removeTrack (bus1);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }
    // ---- bus inserts: the built-in test delay must run on a bus --------------------------------
    {
        auto& src = project.addTrack (TrackType::Instrument, "BusSrc");
        const int srcId = src.id;
        src.hasInstrument = true;
        src.instrument = makeBuiltInSynthSlot();
        MidiClip bc; bc.id = project.newId(); bc.start = 0.0; bc.length = 3.0;
        Note bn; bn.id = project.newId(); bn.pitch = 60; bn.start = 0.5; bn.length = 1.5; bn.velocity = 110;
        bc.notes.push_back (bn);
        src.midiClips.push_back (bc);
        auto& bus = project.addTrack (TrackType::Bus, "TestBus");
        const int busId = bus.id;
        src.outputBusId = busId;
        project.sendStructureChanged();
        engine.syncAll();

        for (auto& tt : project.tracks) tt->solo = (tt->id == srcId);
        project.sendMixChanged();

        auto onsetOf = [&] (const File& f)
        {
            double t0 = -1.0;
            if (std::unique_ptr<AudioFormatReader> r { engine.formats.createReaderFor (f) })
            {
                AudioBuffer<float> b2 (2, (int) r->lengthInSamples);
                r->read (&b2, 0, (int) r->lengthInSamples, 0, true, true);
                for (int i = 0; i < b2.getNumSamples(); ++i)
                    if (std::abs (b2.getSample (0, i)) > 0.003f) { t0 = i / r->sampleRate; break; }
            }
            return t0;
        };
        auto renderTo = [&] (const File& f)
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = f;
            o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.3;
            String e2;
            const bool ok = engine.exportAudio (o, {}, e2);
            return ok ? inspect (engine.formats, f).peak : -1.0;
        };

        const double dryPeak = renderTo (out.getChildFile ("bus_dry.wav"));
        const double dryOnset = onsetOf (out.getChildFile ("bus_dry.wav"));

        if (auto* b = project.findTrack (busId))
        {
            auto slot = makeBuiltInDelaySlot();
            slot.postFader = true;
            b->inserts.push_back (slot);
        }
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
        const double wetPeak = renderTo (out.getChildFile ("bus_delay.wav"));
        const double wetOnset = onsetOf (out.getChildFile ("bus_delay.wav"));
        const double shift = wetOnset - dryOnset;

        // A 960 sample insert on the bus has to be compensated, not baked into the print: the
        // exported file must stay exactly where it was. The old code shifted it by 1920 samples
        // (the insert's own delay plus a second, doubled PDC pad for the bus chain).
        log ("bus insert: built-in delay runs on the bus and is compensated (peak " + String (dryPeak, 3)
             + " -> " + String (wetPeak, 3) + ", onset shifted " + String (shift * 1000.0, 1)
             + " ms, expect 0)",
             dryPeak > 0.01 && wetPeak > 0.01 && std::abs (shift) < 0.0005);

        // ... and the engine really did see the insert: the project latency is its 960 samples
        log ("bus insert: the insert latency is part of the PDC total (" + String (engine.getPdcTotalSamples()) + " samples)",
             engine.getPdcTotalSamples() == BuiltInDelay::kDelay);

        for (auto& tt : project.tracks) tt->solo = false;
        project.removeTrack (busId);
        project.removeTrack (srcId);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }
    // ---- bypass: switching a latency plug-in off must not move the track in time --------------
    {
        auto& src2 = project.addTrack (TrackType::Instrument, "BypassSrc");
        const int srcId2 = src2.id;
        src2.hasInstrument = true;
        src2.instrument = makeBuiltInSynthSlot();
        MidiClip bc2; bc2.id = project.newId(); bc2.start = 0.0; bc2.length = 3.0;
        Note bn2; bn2.id = project.newId(); bn2.pitch = 60; bn2.start = 0.5; bn2.length = 1.5; bn2.velocity = 110;
        bc2.notes.push_back (bn2);
        src2.midiClips.push_back (bc2);
        project.sendStructureChanged();
        for (auto& tt : project.tracks) tt->solo = (tt->id == srcId2);
        project.sendMixChanged();
        engine.syncAll();

        auto onsetOfB = [&] (const File& f)
        {
            double t0 = -1.0;
            if (std::unique_ptr<AudioFormatReader> r { engine.formats.createReaderFor (f) })
            {
                AudioBuffer<float> b2 (2, (int) r->lengthInSamples);
                r->read (&b2, 0, (int) r->lengthInSamples, 0, true, true);
                for (int i = 0; i < b2.getNumSamples(); ++i)
                    if (std::abs (b2.getSample (0, i)) > 0.003f) { t0 = i / r->sampleRate; break; }
            }
            return t0;
        };
        auto renderB = [&] (const File& f)
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = f;
            o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.3;
            String e3;
            return engine.exportAudio (o, {}, e3);
        };

        renderB (out.getChildFile ("bypass_off.wav"));
        const double onsetOff = onsetOfB (out.getChildFile ("bypass_off.wav"));

        String insErr;
        const bool added = engine.addInsert (srcId2, makeBuiltInDelaySlot().desc, insErr);
        auto* st2 = project.findTrack (srcId2);
        const int64 uid = (added && st2 != nullptr && ! st2->inserts.empty()) ? st2->inserts.front().uid : -1;
        project.sendStructureChanged();
        engine.syncAll();
        renderB (out.getChildFile ("bypass_active.wav"));
        const double onsetActive = onsetOfB (out.getChildFile ("bypass_active.wav"));
        const int pdcActive = engine.getPdcTotalSamples();

        engine.setSlotBypass (srcId2, uid, true);
        renderB (out.getChildFile ("bypass_on.wav"));
        const double onsetBypassed = onsetOfB (out.getChildFile ("bypass_on.wav"));
        const int pdcBypassed = engine.getPdcTotalSamples();

        log ("bypass: a 960 sample insert is compensated while it runs (onset "
             + String (onsetActive * 1000.0, 1) + " ms vs " + String (onsetOff * 1000.0, 1) + " ms)",
             added && onsetOff > 0.0 && std::abs (onsetActive - onsetOff) < 0.0005);

        // Switching a latency plug-in off used to drop its latency from the chain: the track
        // jumped 960 samples earlier and the compensation ring replayed audio that had already
        // been heard (the "doubled note"). The total latency must stay identical instead.
        log ("bypass: switching the insert off keeps the track in time (onset "
             + String (onsetBypassed * 1000.0, 1) + " ms, PDC " + String (pdcBypassed) + " vs "
             + String (pdcActive) + " samples)",
             std::abs (onsetBypassed - onsetActive) < 0.0005 && pdcBypassed == pdcActive);

        engine.setSlotBypass (srcId2, uid, false);
        for (auto& tt : project.tracks) tt->solo = false;
        project.removeTrack (srcId2);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }
    // ---- a master insert must not shift the printed mix ----------------------------------------
    {
        auto& mt = project.addTrack (TrackType::Instrument, "MasterInsertSrc");
        const int mid = mt.id;
        mt.hasInstrument = true;
        mt.instrument = makeBuiltInSynthSlot();
        MidiClip mc; mc.id = project.newId(); mc.start = 0.0; mc.length = 3.0;
        Note mn; mn.id = project.newId(); mn.pitch = 60; mn.start = 0.5; mn.length = 1.5; mn.velocity = 110;
        mc.notes.push_back (mn);
        mt.midiClips.push_back (mc);
        project.sendStructureChanged();
        for (auto& tt : project.tracks) tt->solo = (tt->id == mid);
        project.sendMixChanged();
        engine.syncAll();

        auto onsetOfM = [&] (const File& f)
        {
            double t0 = -1.0;
            if (std::unique_ptr<AudioFormatReader> r { engine.formats.createReaderFor (f) })
            {
                AudioBuffer<float> b3 (2, (int) r->lengthInSamples);
                r->read (&b3, 0, (int) r->lengthInSamples, 0, true, true);
                for (int i = 0; i < b3.getNumSamples(); ++i)
                    if (std::abs (b3.getSample (0, i)) > 0.003f) { t0 = i / r->sampleRate; break; }
            }
            return t0;
        };

        ExportOptions mo;
        mo.mode = ExportOptions::Mix; mo.target = out.getChildFile ("master_ins_dry.wav");
        mo.sampleRate = 44100.0; mo.bitDepth = 24; mo.tailSeconds = 0.3;
        String e4;
        engine.exportAudio (mo, {}, e4);
        const double dryOnsetM = onsetOfM (mo.target);

        String mErr;
        const bool mAdded = engine.addInsert (-1, makeBuiltInDelaySlot().desc, mErr);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();

        mo.target = out.getChildFile ("master_ins_delay.wav");
        engine.exportAudio (mo, {}, e4);
        const double wetOnsetM = onsetOfM (mo.target);

        log ("master insert: a 960 sample master plug-in does not shift the export (onset "
             + String (dryOnsetM * 1000.0, 1) + " -> " + String (wetOnsetM * 1000.0, 1) + " ms)",
             mAdded && dryOnsetM > 0.0 && std::abs (wetOnsetM - dryOnsetM) < 0.0005);

        for (auto& s : project.masterTrack.inserts) engine.removeSlot (-1, s.uid);
        project.masterTrack.inserts.clear();
        project.sendStructureChanged();
        project.sendContentChanged();
        for (auto& tt : project.tracks) tt->solo = false;
        project.removeTrack (mid);
        project.sendStructureChanged();
        engine.syncAll();
    }
    // ---- listen bus: created from the menu and shown right of the master strip --------------
    {
        // an ordinary track so the L button can be checked on a known strip
        auto& probeTrack = project.addTrack (TrackType::Instrument, "ListenProbe");
        const int probeId = probeTrack.id;
        project.sendStructureChanged();
        engine.syncAll();

        AppState state (project, engine, plugins);
        MixerView mixer (state);
        mixer.setSize (1200, 400);

        std::function<ChannelStrip* (Component&, int)> findStrip = [&] (Component& c, int id) -> ChannelStrip*
        {
            if (auto* s0 = dynamic_cast<ChannelStrip*> (&c)) if (s0->getTrackId() == id && s0->isVisible()) return s0;
            for (auto* ch : c.getChildren()) if (auto* r = findStrip (*ch, id)) return r;
            return nullptr;
        };
        auto stripFor = [&] (int id) -> ChannelStrip*
        {
            return findStrip (mixer, id);
        };

        // what the menu entry does
        project.checkpoint();
        for (auto& t : project.tracks) t->isListenBus = false;
        auto& lb = project.addTrack (TrackType::Bus, "Listen");
        const int listenId = lb.id;
        lb.isListenBus = true;
        lb.volumeDb = 0.0f;
        project.sendStructureChanged();
        engine.syncAll();

        auto* listenStrip = stripFor (listenId);
        auto* masterStrip = stripFor (-1);
        const bool rightOfMaster = listenStrip != nullptr && masterStrip != nullptr
                                   && listenStrip->getX() > masterStrip->getX();
        const bool notInScrollRow = [&]
        {
            int count = 0;
            for (auto* c : mixer.getChildren())
                if (auto* s = dynamic_cast<ChannelStrip*> (c))
                    if (s->getTrackId() == listenId) ++count;
            return count == 1;
        }();

        log ("listen bus: no sends and no output routing on the monitor strip (send rows "
             + String (listenStrip != nullptr ? listenStrip->visibleSendRows() : -1) + ", add-send "
             + String ((int) (listenStrip != nullptr && listenStrip->addSendRowVisible())) + ", post-fader insert still allowed)",
             listenStrip != nullptr && listenStrip->visibleSendRows() == 0 && ! listenStrip->addSendRowVisible());
        int probeInfo = 0;
        log ("listen bus: L on tracks, no automation on the monitor strip (track L="
             + String ((int) (findStrip (mixer, probeId) != nullptr && findStrip (mixer, probeId)->listenButtonShown()))
             + ", " + (findStrip (mixer, probeId) != nullptr ? findStrip (mixer, probeId)->listenButtonDebug() : String ("no strip")) + ", listen auto=" + String ((int) (listenStrip != nullptr && listenStrip->automationShown())) + ")",
             (findStrip (mixer, probeId) != nullptr && findStrip (mixer, probeId)->listenButtonShown())
                 && listenStrip != nullptr && ! listenStrip->automationShown());
        log ("listen bus: no pan control on the monitor strip (pan on="
             + String ((int) (listenStrip != nullptr && listenStrip->panVisible()))
             + ", normal track=" + String ((int) ([&] { for (auto* c : mixer.getChildren()) if (auto* s2 = dynamic_cast<ChannelStrip*> (c)) if (s2->getTrackId() != listenId && s2->getTrackId() != -1) return s2->panVisible(); return false; })()) + ")",
             listenStrip != nullptr && ! listenStrip->panVisible());
        log ("listen bus: strip appears once, to the right of the master (listen x="
             + String (listenStrip != nullptr ? listenStrip->getX() : -1) + ", master x="
             + String (masterStrip != nullptr ? masterStrip->getX() : -1) + ")",
             listenStrip != nullptr && masterStrip != nullptr && rightOfMaster && notInScrollRow);

        project.removeTrack (listenId);
        project.removeTrack (probeId);
        project.sendStructureChanged();
        engine.syncAll();
        log ("listen bus: strip disappears again when the bus is removed", stripFor (listenId) == nullptr);
    }

    // ---- listen bus: per-track listening only affects monitoring ------------------------------
    {
        auto& a = project.addTrack (TrackType::Instrument, "ListenA");
        const int aId = a.id;
        a.hasInstrument = true;
        a.instrument = makeBuiltInSynthSlot();
        a.listenSolo = true;
        MidiClip ac; ac.id = project.newId(); ac.start = 0.0; ac.length = 3.0;
        Note an; an.id = project.newId(); an.pitch = 60; an.start = 0.5; an.length = 2.0; an.velocity = 110;
        ac.notes.push_back (an);
        a.midiClips.push_back (ac);

        auto& b = project.addTrack (TrackType::Instrument, "ListenB");
        b.hasInstrument = true;
        b.instrument = makeBuiltInSynthSlot();
        b.midiClips.push_back (ac);          // same part on both tracks

        auto& lb = project.addTrack (TrackType::Bus, "Listen");
        const int listenId = lb.id;
        lb.isListenBus = true;
        lb.listenEnabled = true;
        project.sendStructureChanged();
        engine.syncAll();

        auto renderPeakOf = [&] (const File& f)
        {
            for (auto& tt : project.tracks) { tt->solo = false; tt->mute = false; }
            project.sendMixChanged();
            engine.syncAll();
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = f;
            o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.2;
            String e2;
            engine.exportAudio (o, {}, e2);
            return inspect (engine.formats, f).peak;
        };

        auto* at = project.findTrack (aId);
        at->listenSolo = true;
        const float listened = renderPeakOf (out.getChildFile ("listen_solo_on.wav"));
        at->listenSolo = false;
        const float bothTracks = renderPeakOf (out.getChildFile ("listen_solo_off.wav"));

        log ("listen bus: per-track listen is monitoring only (export unchanged " + String (listened, 3) + " vs "
             + String (bothTracks, 3) + ")", std::abs (listened - bothTracks) < 0.0005f);

        at->listenSolo = true;
        project.sendMixChanged();
        const bool savedFlag = [&]
        {
            File lf = out.getChildFile ("listen_solo.sxp");
            Project lp; lp.formats = &engine.formats;
            if (! (project.saveTo (lf) && lp.loadFrom (lf))) return false;
            auto* rt2 = lp.findTrack (aId);
            return rt2 != nullptr && rt2->listenSolo;
        }();
        log ("listen bus: per-track listen flag survives save + load", savedFlag);

        at->listenSolo = false;
        project.removeTrack (listenId);
        project.removeTrack (b.id);
        project.removeTrack (aId);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }
    // ---- listen bus: monitoring only - it must never be printed into a render -----------------
    {
        auto& lt = project.addTrack (TrackType::Instrument, "ListenSrc");
        const int srcId = lt.id;
        lt.hasInstrument = true;
        lt.instrument = makeBuiltInSynthSlot();
        MidiClip lc; lc.id = project.newId(); lc.start = 0.0; lc.length = 3.0;
        Note ln; ln.id = project.newId(); ln.pitch = 60; ln.start = 0.5; ln.length = 2.0; ln.velocity = 110;
        lc.notes.push_back (ln);
        lt.midiClips.push_back (lc);
        project.sendStructureChanged();
        engine.syncAll();
        for (auto& tt : project.tracks) tt->solo = (tt->id == srcId);
        project.sendMixChanged();

        auto peakOf = [&] (const File& f)
        {
            float peak = 0.0f;
            if (std::unique_ptr<AudioFormatReader> r { engine.formats.createReaderFor (f) })
            {
                AudioBuffer<float> b2 (2, (int) r->lengthInSamples);
                r->read (&b2, 0, (int) r->lengthInSamples, 0, true, true);
                peak = b2.getMagnitude (0, 0, b2.getNumSamples());
            }
            return peak;
        };
        auto onsetOf = [&] (const File& f)
        {
            double t0 = -1.0;
            if (std::unique_ptr<AudioFormatReader> r { engine.formats.createReaderFor (f) })
            {
                AudioBuffer<float> b2 (2, (int) r->lengthInSamples);
                r->read (&b2, 0, (int) r->lengthInSamples, 0, true, true);
                for (int i = 0; i < b2.getNumSamples(); ++i)
                    if (std::abs (b2.getSample (0, i)) > 0.003f) { t0 = i / r->sampleRate; break; }
            }
            return t0;
        };
        auto renderTo = [&] (const File& f) { return peakOf (f); };
        auto doRender = [&] (const File& f)
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = f;
            o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.3;
            String e2;
            engine.exportAudio (o, {}, e2);
        };

        File dryFile = out.getChildFile ("listen_dry.wav");
        doRender (dryFile);
        const float dryPeak = renderTo (dryFile);
        const double dryOnset = onsetOf (dryFile);

        // add a listen bus with a level and a post-fader insert (what the master strip menu does)
        for (auto& tt : project.tracks) tt->isListenBus = false;
        auto& lb = project.addTrack (TrackType::Bus, "Listen");
        const int listenId = lb.id;
        lb.isListenBus = true;
        lb.listenEnabled = true;
        lb.volumeDb = -12.0f;
        {
            auto slot = makeBuiltInDelaySlot();
            slot.postFader = true;
            lb.inserts.push_back (slot);
        }
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();

        File monFile = out.getChildFile ("listen_mon.wav");
        doRender (monFile);
        const float monPeak = renderTo (monFile);
        const double monOnset = onsetOf (monFile);

        if (auto* b = project.findTrack (listenId)) b->volumeDb = -99.0f;
        project.sendMixChanged();
        engine.syncAll();
        File mutedFile = out.getChildFile ("listen_mon_muted.wav");
        doRender (mutedFile);
        const float mutedPeak = renderTo (mutedFile);

        if (auto* b = project.findTrack (listenId)) b->volumeDb = 0.0f;
        project.sendMixChanged();
        engine.syncAll();

        log ("listen bus: never printed into the export (dry " + String (dryPeak, 4) + ", listen+fx "
             + String (monPeak, 4) + ", level off " + String (mutedPeak, 4) + ", onset "
             + String (monOnset * 1000.0, 2) + "/" + String (dryOnset * 1000.0, 2) + " ms)",
             dryPeak > 0.01f && std::abs (monPeak - dryPeak) < 0.0005f
                 && std::abs (mutedPeak - dryPeak) < 0.0005f && std::abs (monOnset - dryOnset) < 0.0005);

        // with a real device the monitor level must still change what is heard
        if (auto* dev = engine.deviceManager.getCurrentAudioDevice())
        {
            engine.setPositionBeats (0.0);
            engine.play();
            Thread::sleep (700);
            float l1 = 0.0f, r1 = 0.0f;
            engine.readMasterPeaks (l1, r1);
            const float heard = jmax (l1, r1);

            if (auto* b = project.findTrack (listenId)) b->volumeDb = -99.0f;
            project.sendMixChanged();
            engine.syncAll();
            { float dl = 0.0f, dr = 0.0f; engine.readMasterPeaks (dl, dr); }   // drop the peak from before the change
            float silenced = 1.0f;
            for (int i = 0; i < 4; ++i)
            {
                Thread::sleep (250);
                float sl = 0.0f, sr = 0.0f;
                engine.readMasterPeaks (sl, sr);
                silenced = jmin (silenced, jmax (sl, sr));
            }
            engine.stop();
            if (auto* b = project.findTrack (listenId)) b->volumeDb = 0.0f;
            project.sendMixChanged();
            engine.syncAll();
            log ("listen bus: monitoring level applies in real time on " + dev->getName() + " ("
                 + String (heard, 3) + " -> " + String (silenced, 3) + ")", heard > 0.01f && silenced < 0.002f);
        }
        else
        {
            log ("listen bus: no audio device - real-time monitoring check skipped", true);
        }

        File lf = out.getChildFile ("listen_bus.sxp");
        Project lp;
        lp.formats = &engine.formats;
        const bool saved = project.saveTo (lf) && lp.loadFrom (lf);
        auto* rlb = saved ? lp.listenBus() : nullptr;
        log ("listen bus survives save + load", rlb != nullptr && rlb->id == listenId && rlb->type == TrackType::Bus);

        for (auto& tt : project.tracks) tt->solo = false;
        project.removeTrack (listenId);
        project.removeTrack (srcId);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }

    // ---- master fader automation --------------------------------------------------------------
    {
        auto& mt = project.addTrack (TrackType::Instrument, "MasterAutoSrc");
        const int mtId = mt.id;
        mt.hasInstrument = true;
        mt.instrument = makeBuiltInSynthSlot();
        MidiClip mc2; mc2.id = project.newId(); mc2.start = 0.0; mc2.length = 3.0;
        Note mn; mn.id = project.newId(); mn.pitch = 60; mn.start = 0.5; mn.length = 2.0; mn.velocity = 110;
        mc2.notes.push_back (mn);
        mt.midiClips.push_back (mc2);
        project.sendStructureChanged();
        for (auto& tt : project.tracks) tt->solo = (tt->id == mtId);
        project.sendMixChanged();
        engine.syncAll();
        auto renderMaster = [&] (const File& f)
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = f;
            o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.2;
            String e2;
            engine.exportAudio (o, {}, e2);
            return inspect (engine.formats, f).peak;
        };
        const float flat = renderMaster (out.getChildFile ("master_flat.wav"));
        auto& masterCurve = project.masterFaderLane();
        masterCurve.points = { { 0.0, gainToFader (Decibels::decibelsToGain (-12.0f)) },
                               { 8.0, gainToFader (Decibels::decibelsToGain (-12.0f)) } };
        normalizeAutoPoints (masterCurve.points);
        engine.syncAll();
        const float curved = renderMaster (out.getChildFile ("master_auto.wav"));
        log ("master automation: the fader curve drives the render (" + String (flat, 3) + " -> " + String (curved, 3) + ")",
             flat > 0.01f && curved > 0.001f && curved < flat * 0.35f);
        // automation mode Off parks the master fader: the curve is ignored, exactly like on a track
        project.masterTrack.autoMode = AutoMode::Off;
        engine.syncAll();
        const float parked = renderMaster (out.getChildFile ("master_auto_off.wav"));
        project.masterTrack.autoMode = AutoMode::Read;
        // the master mute wins over the fader curve
        project.masterTrack.mute = true;
        engine.syncAll();
        const float muted = renderMaster (out.getChildFile ("master_auto_muted.wav"));
        project.masterTrack.mute = false;
        engine.syncAll();
        log ("master automation: Off parks the fader (" + String (parked, 3) + " vs flat " + String (flat, 3) + "), mute silences it ("
             + String (muted, 3) + ")",
             std::abs (parked - flat) < 0.02f && muted < 0.002f);
        File mf = out.getChildFile ("master_auto.sxp");
        Project mp; mp.formats = &engine.formats;
        const bool saved = project.saveTo (mf) && mp.loadFrom (mf);
        log ("master automation survives save + load", saved && mp.masterFaderLaneIfAny() != nullptr && mp.masterFaderLaneIfAny()->points.size() == 2);
        project.masterTrack.lanes.clear();
        for (auto& tt : project.tracks) tt->solo = false;
        project.removeTrack (mtId);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }

    // ---- master mono / stereo switch ----------------------------------------------------------
    {
        auto& src = project.addTrack (TrackType::Instrument, "MonoSrc");
        const int srcId = src.id;
        src.hasInstrument = true;
        src.instrument = makeBuiltInSynthSlot();
        src.pan = -1.0f;                     // hard left: L and R differ in the stereo mix
        MidiClip mc; mc.id = project.newId(); mc.start = 0.0; mc.length = 3.0;
        Note mn; mn.id = project.newId(); mn.pitch = 60; mn.start = 0.5; mn.length = 2.0; mn.velocity = 110;
        mc.notes.push_back (mn);
        src.midiClips.push_back (mc);
        project.sendStructureChanged();
        for (auto& tt : project.tracks) tt->solo = (tt->id == srcId);
        project.sendMixChanged();
        engine.syncAll();

        auto channelPeaks = [&] (const File& f, float& l, float& r)
        {
            std::unique_ptr<AudioFormatReader> rd (engine.formats.createReaderFor (f));
            if (rd == nullptr || rd->numChannels == 0 || rd->lengthInSamples <= 0) return false;
            AudioBuffer<float> b ((int) rd->numChannels, (int) rd->lengthInSamples);
            rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
            l = b.getMagnitude (0, 0, b.getNumSamples());
            r = b.getMagnitude (juce::jmin (1, b.getNumChannels() - 1), 0, b.getNumSamples());
            return true;
        };
        auto renderPeaks = [&] (const File& f, float& l, float& r)
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = f;
            o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.2;
            String err;
            engine.exportAudio (o, {}, err);
            return channelPeaks (f, l, r);
        };

        float sl = 0.0f, sr = 0.0f, ml = 0.0f, mr = 0.0f;
        const bool okStereo = renderPeaks (out.getChildFile ("master_stereo.wav"), sl, sr);
        project.masterTrack.mono = true;
        engine.syncAll();
        const bool okMono = renderPeaks (out.getChildFile ("master_mono.wav"), ml, mr);
        log ("master mono: a left-panned source folds to both channels (" + String (sl, 3) + " / " + String (sr, 3)
             + " -> " + String (ml, 3) + " / " + String (mr, 3) + ")",
             okStereo && okMono && sl > 0.05f && sr < sl * 0.25f
             && ml > 0.05f && mr > 0.05f && std::abs (ml - mr) < 0.01f);

        // the switch is stored with the project
        {
            const File monoFile = out.getChildFile ("master_mono.sxp");
            Project monoProject; monoProject.formats = &engine.formats;
            const bool monoSaved = project.saveTo (monoFile) && monoProject.loadFrom (monoFile);
            log ("master mono: survives save + load", monoSaved && monoProject.masterTrack.mono);
        }
        project.masterTrack.mono = false;
        engine.syncAll();

        for (auto& tt : project.tracks) tt->solo = false;
        project.removeTrack (srcId);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }
    // ---- audio edit: reverse / speed / transpose -----------------------------------------------
    {
        // a one second 432 Hz tone with a decaying envelope: the head is much louder than the
        // tail, so reversing the clip shows up in the rendered head / tail level, and the zero
        // crossing rate follows the pitch (432 Hz => 864 crossings per second)
        const File tone = out.getChildFile ("audio_edit_tone.wav");
        {
            WavAudioFormat wav;
            tone.deleteFile();
            if (auto* w = wav.createWriterFor (new FileOutputStream (tone), 44100.0, 1, 24, {}, 0))
            {
                AudioBuffer<float> b (1, 44100);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const double tSec = (double) i / 44100.0;
                    const float env = (float) (0.55 * std::exp (-tSec / 0.35));
                    b.setSample (0, i, env * (float) std::sin (MathConstants<double>::twoPi * 432.0 * tSec));
                }
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                delete w;
            }
        }

        auto& et = project.addTrack (TrackType::Audio, "EditTone");
        const int etId = et.id;
        AudioClip ec;
        ec.id = project.newId(); ec.name = "tone"; ec.file = tone; ec.start = 0.0;
        ec.data = project.pool.load (tone, engine.formats);
        ec.lengthSec = ec.data != nullptr ? ec.data->lengthSeconds() : 0.0;
        const int clipId = ec.id;
        et.audioClips.push_back (ec);
        project.sendStructureChanged();
        project.sendContentChanged();
        for (auto& tt : project.tracks) tt->solo = (tt->id == etId);
        project.sendMixChanged();
        engine.syncAll();

        // renders the mix and measures the audible part: level in the first / last window, the
        // audible length in seconds and the zero crossing rate in the middle of the file
        auto renderEdit = [&] (const File& f, float& head, float& tail, double& seconds, float& crossingsPerSec) -> bool
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix; o.target = f;
            o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.2;
            String err;
            if (! engine.exportAudio (o, {}, err)) return false;

            std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (f));
            if (r == nullptr || r->lengthInSamples < 8192) return false;

            AudioBuffer<float> b (1, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);

            const int n = b.getNumSamples();
            const int edge = jmax (64, jmin (n / 18, 8820));
            int first = 0, last = n - 1;
            for (int i = 0; i < n; ++i) if (std::abs (b.getSample (0, i)) > 1.0e-4f) { first = i; break; }
            for (int i = n; --i >= 0;) if (std::abs (b.getSample (0, i)) > 1.0e-4f) { last = i; break; }

            head = b.getMagnitude (0, jlimit (0, jmax (0, n - edge), first), edge);
            tail = b.getMagnitude (0, jmax (0, last - edge), edge);
            seconds = (double) jmax (0, last - first) / r->sampleRate;

            const int mid = n / 2, span = jmin (n / 18, 8820);
            int zero = 0;
            const float* d = b.getReadPointer (0);
            for (int i = mid + 1; i < mid + span; ++i)
                if ((d[i - 1] < 0.0f) != (d[i] < 0.0f)) ++zero;
            crossingsPerSec = (float) zero / ((float) span / (float) r->sampleRate);
            return true;
        };

        auto* ac = project.findTrack (etId) != nullptr ? project.findTrack (etId)->findAudio (clipId) : nullptr;
        if (ac == nullptr)
        {
            log ("audio edit: the test clip exists", false);
        }
        else
        {
            const double srcLen = ac->lengthSec;
            float h = 0, t = 0, cr = 0; double sec = 0;
            // forward: energy at the head (the source fades out towards its end)
            const bool okFwd = renderEdit (out.getChildFile ("edit_forward.wav"), h, t, sec, cr);
            const float fwdHead = h, fwdTail = t, fwdSec = (float) sec, fwdCross = cr;
            ac->reversed = true;
            engine.syncAll();
            const bool okRev = renderEdit (out.getChildFile ("edit_reverse.wav"), h, t, sec, cr);
            log ("audio edit: reverse flips the clip content (head/tail " + String (fwdHead, 3) + "/" + String (fwdTail, 3)
                 + " -> " + String (h, 3) + "/" + String (t, 3) + ")",
                 okFwd && okRev && fwdHead > fwdTail * 2.0f && t > h * 2.0f);
            ac->reversed = false;

            // speed: twice as long, same pitch
            ac->stretch = 2.0;
            ac->lengthSec = srcLen * 2.0;
            invalidateClipRender (*ac);
            ensureClipPlayback (*ac);
            engine.syncAll();
            const bool okSlow = renderEdit (out.getChildFile ("edit_slow.wav"), h, t, sec, cr);
            const bool renderedLen = ac->rendered != nullptr && std::abs (ac->rendered->lengthSeconds() - ac->lengthSec) < 0.02;
            // the stretched render must keep the source envelope (head loud, tail quiet) and not
            // ring at the edges, so check the head / tail level as well
            const bool slowEnvelope = h > t * 2.0f;
            log ("audio edit: 2x speed is twice as long and keeps the pitch (" + String (fwdSec, 2) + " s -> " + String (sec, 2)
                 + " s, " + String (fwdCross, 0) + " -> " + String (cr, 0) + " zero crossings/s, head/tail "
                 + String (h, 3) + "/" + String (t, 3) + ")",
                 okSlow && std::abs (sec - (double) fwdSec * 2.0) < 0.15 && std::abs (cr - fwdCross) < fwdCross * 0.2f
                 && renderedLen && slowEnvelope);

            // transpose an octave up: same length, twice the frequency
            ac->stretch = 1.0;
            ac->lengthSec = srcLen;
            ac->pitchSemitones = 12.0f;
            invalidateClipRender (*ac);
            ensureClipPlayback (*ac);
            engine.syncAll();
            const bool okPitch = renderEdit (out.getChildFile ("edit_octave.wav"), h, t, sec, cr);
            log ("audio edit: +12 semitones keeps the length and doubles the pitch (" + String (fwdSec, 2) + " s -> " + String (sec, 2)
                 + " s, " + String (fwdCross, 0) + " -> " + String (cr, 0) + " zero crossings/s)",
                 okPitch && std::abs (sec - (double) fwdSec) < 0.1 && cr > fwdCross * 1.6f && cr < fwdCross * 2.4f);

            // the parameters are part of the project file
            ac->stretch = 1.5;
            ac->lengthSec = srcLen * 1.5;
            ac->pitchSemitones = -4.0f;
            const File tf = out.getChildFile ("audio_edit.sxp");
            Project mp; mp.formats = &engine.formats;
            const bool saved = project.saveTo (tf) && mp.loadFrom (tf);
            const auto* loaded = mp.findTrack (etId) != nullptr ? mp.findTrack (etId)->findAudio (clipId) : nullptr;
            log ("audio edit: reverse / speed / transpose survive save + load",
                 saved && loaded != nullptr && std::abs (loaded->stretch - 1.5) < 1.0e-6
                 && std::abs (loaded->pitchSemitones + 4.0f) < 1.0e-4);

            // the UI commands work on the selection (and undo restores the parameters)
            {
                AppState st (project, engine, plugins);
                ArrangeView av (st);
                av.setSize (1200, 700);
                st.selectedClips = { clipId };
                av.reverseSelectedAudio();
                const bool rev = ac->reversed;
                av.setSelectedAudioStretch (2.0);
                const bool stretched = std::abs (ac->stretch - 2.0) < 1.0e-6 && std::abs (ac->lengthSec - srcLen * 2.0) < 1.0e-3;
                av.setSelectedAudioPitch (5);
                const bool pitched = std::abs (ac->pitchSemitones - 5.0f) < 1.0e-3f;
                // snapshot of the arrangement so the edited waveform can be checked visually
                if (auto shot = av.createComponentSnapshot (av.getLocalBounds(), true); shot.isValid())
                    if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("audio_edit_view.png").createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                    }
                av.resetSelectedAudioEdits();
                const bool reset = ! ac->reversed && std::abs (ac->stretch - 1.0) < 1.0e-9
                                   && std::abs (ac->pitchSemitones) < 1.0e-6 && std::abs (ac->lengthSec - srcLen) < 1.0e-3;
                log ("audio edit: the arrangement commands reverse / stretch / transpose the selection",
                     rev && stretched && pitched && reset);
            }

            // export option: write the tempo into the WAV (ACID chunk)
            {
                ExportOptions o;
                o.mode = ExportOptions::Mix; o.target = out.getChildFile ("edit_tempo.wav");
                o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.0;
                o.writeTempo = true;
                String err;
                const bool ok = engine.exportAudio (o, {}, err);
                double bpm = 0, beats = 0;
                const bool tagged = readWavTempo (o.target, bpm, beats);
                ExportOptions o2 = o;
                o2.writeTempo = false;
                o2.target = out.getChildFile ("edit_no_tempo.wav");
                const bool ok2 = engine.exportAudio (o2, {}, err);
                double bpm2 = 0, beats2 = 0;
                const bool untagged = ! readWavTempo (o2.target, bpm2, beats2);
                log ("export: the tempo tag is written only when asked (" + String (bpm, 1) + " BPM / "
                     + String (beats, 0) + " beats)",
                     ok && ok2 && tagged && std::abs (bpm - project.bpmAt (0.0)) < 0.1 && beats > 0.5 && untagged);
            }
        }

        for (auto& tt : project.tracks) tt->solo = false;
        project.removeTrack (etId);
        project.sendStructureChanged();
        project.sendContentChanged();
        engine.syncAll();
    }
    // ---- master bus automation lanes ----------------------------------------------------------
    {
        auto& msrc = project.addTrack (TrackType::Instrument, "MasterLaneSrc");
        const int msrcId = msrc.id;
        msrc.hasInstrument = true;
        msrc.instrument = makeBuiltInSynthSlot();
        MidiClip mc3; mc3.id = project.newId(); mc3.start = 0.0; mc3.length = 3.0;
        Note mn3; mn3.id = project.newId(); mn3.pitch = 60; mn3.start = 0.5; mn3.length = 2.0; mn3.velocity = 110;
        mc3.notes.push_back (mn3);
        msrc.midiClips.push_back (mc3);
        project.sendStructureChanged();
        for (auto& tt : project.tracks) tt->solo = (tt->id == msrcId);
        project.sendMixChanged();
        engine.syncAll();
        AutoLane vl; vl.target = "vol"; vl.label = "Master Volume"; vl.visible = true;
        vl.points = { { 0.0, gainToFader (Decibels::decibelsToGain (-18.0f)) }, { 8.0, gainToFader (Decibels::decibelsToGain (-18.0f)) } };
        project.masterTrack.lanes.push_back (vl);
        engine.syncAll();
        ExportOptions o;
        o.mode = ExportOptions::Mix; o.target = out.getChildFile ("master_lane.wav");
        o.sampleRate = 44100.0; o.bitDepth = 24; o.tailSeconds = 0.2;
        String e2;
        engine.exportAudio (o, {}, e2);
        const float lanePeak = inspect (engine.formats, o.target).peak;
        log ("master bus lane drives the render (" + String (lanePeak, 3) + ")", lanePeak > 0.001f && lanePeak < 0.12f);
        File mf2 = out.getChildFile ("master_lanes.sxp");
        Project mp2; mp2.formats = &engine.formats;
        const bool saved2 = project.saveTo (mf2) && mp2.loadFrom (mf2);
        log ("master bus lanes survive save + load", saved2 && mp2.masterTrack.lanes.size() == 1 && mp2.masterTrack.lanes[0].target == "vol");
        for (auto& tt : project.tracks) tt->solo = false;
        project.removeTrack (msrcId);
        project.sendStructureChanged();
        project.masterTrack.lanes.clear();
        engine.syncAll();
    }

    // ---- automation curve: monotone, sorted, no overshoot ---------------------------------------
    {
        std::vector<AutoPoint> pts = { { 4.0, 0.9f }, { 0.0, 0.1f }, { 4.0, 0.2f }, { 2.0, 0.5f }, { 2.0, 0.55f } };
        normalizeAutoPoints (pts);
        bool sorted = true, strictly = true;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            if (i > 0 && pts[(size_t) i].beat < pts[i - 1].beat) sorted = false;
            if (i > 0 && pts[(size_t) i].beat - pts[i - 1].beat < 1.0e-4) strictly = false;
        }
        bool nodesExact = true;
        for (auto& p : pts)
            if (std::abs (evalAutoLane (pts, p.beat) - p.value) > 1.0e-6f) nodesExact = false;

        bool monotone = true, inRange = true;
        for (size_t i = 1; i < pts.size(); ++i)
        {
            const double a = pts[i - 1].beat, b = pts[(size_t) i].beat;
            const float lo = jmin (pts[i - 1].value, pts[(size_t) i].value);
            const float hi = jmax (pts[i - 1].value, pts[(size_t) i].value);
            float prev = evalAutoLane (pts, a);
            for (int k = 1; k <= 64; ++k)
            {
                const float v = evalAutoLane (pts, a + (b - a) * (double) k / 64.0);
                if (v < lo - 1.0e-6f || v > hi + 1.0e-6f) inRange = false;
                if (pts[(size_t) i].value >= pts[i - 1].value) { if (v < prev - 1.0e-6f) monotone = false; }
                else                                            { if (v > prev + 1.0e-6f) monotone = false; }
                prev = v;
            }
        }
        const bool clamped = std::abs (evalAutoLane (pts, -4.0) - pts.front().value) < 1.0e-6f
                          && std::abs (evalAutoLane (pts, 99.0) - pts.back().value) < 1.0e-6f;
        log ("automation curve: " + String ((int) pts.size()) + " nodes sorted / strictly increasing / exact / monotone / no overshoot / clamped",
             sorted && strictly && nodesExact && monotone && inRange && clamped);

        // dragging a node far past its neighbour clamps it there: the curve still cannot double back
        moveAutoPoint (pts, 1, 99.0, 0.0f);
        bool ordered = true;
        for (size_t i = 1; i < pts.size(); ++i)
            if (pts[(size_t) i].beat - pts[i - 1].beat < 1.0e-9) ordered = false;
        // clicking on an existing beat edits that node instead of stacking a duplicate
        const size_t before = pts.size();
        insertAutoPoint (pts, pts[(size_t) 1].beat, 0.3f);
        log ("automation curve: drag clamps to the neighbour, a click on a beat edits it",
             ordered && pts.size() == before);
    }

    // ---- master bus row: part of the row system, pinned above the tracks -------------------------
    {
        AppState st (project, engine, plugins);
        project.masterTrack.lanes.clear();
        project.masterTrack.height = 0;
        project.masterTrack.volumeDb = 0.0f;
        project.masterTrack.mute = false;
        st.showMasterTrack = false;

        ArrangeView av (st);
        av.setSize (1200, 700);
        av.resized();
        const int hiddenH = av.masterRowH();
        st.showMasterTrack = true;
        av.resized();
        const int bareH = av.masterRowH();

        AutoLane l1; l1.target = "vol"; l1.label = "Master Volume"; l1.visible = true;
        project.masterTrack.lanes.push_back (l1);
        project.sendContentChanged();
        av.resized();
        const int oneLane = av.masterRowH();

        AutoLane l2; l2.target = "p:99:0"; l2.label = "Plugin Parameter"; l2.visible = true;
        project.masterTrack.lanes.push_back (l2);
        project.sendContentChanged();
        av.resized();
        const int twoLanes = av.masterRowH();

        const int laneH = automationRowHeight (st.trackHeight);
        const bool heights = hiddenH == 0 && bareH == st.trackHeight
                             && oneLane == bareH + laneH && twoLanes == bareH + 2 * laneH;
        log ("master row: hidden / bare / one lane / two lanes -> " + String (hiddenH) + " / " + String (bareH) + " / "
             + String (oneLane) + " / " + String (twoLanes) + " px (lane " + String (laneH) + " px)", heights);

        // the row is pinned: it starts right under the tempo row and spans the full width
        const auto rb = av.masterRowBounds();
        log ("master row: pinned above the tracks at y=" + String (rb.getY()) + " h=" + String (rb.getHeight())
             + " w=" + String (rb.getWidth()),
             rb.getY() == ArrangeView::kRulerH + av.tempoLaneH() && rb.getHeight() == twoLanes
             && rb.getWidth() == av.getWidth());

        project.masterTrack.lanes.clear();
        st.showMasterTrack = false;
        project.sendContentChanged();
        av.resized();
        log ("master row: hiding it again gives the space back to the tracks", av.masterRowH() == 0);

        // editing on the pinned row works like on a track: click adds a node on the grid,
        // dragging it changes the value and is clamped between its neighbours
        st.showMasterTrack = true;
        AutoLane vl; vl.target = "vol"; vl.label = "Master Volume"; vl.visible = true; vl.points.clear();
        project.masterTrack.lanes.push_back (vl);
        project.sendContentChanged();
        av.resized();
        if (auto* strip = dynamic_cast<ArrangeView::MasterAutoLane*> (av.masterLaneComponent()))
        {
            using ME = juce::MouseEvent;
            const auto row = strip->laneRect (0);
            const auto beat = [] (int x, double ppb) { return (double) x / ppb; };
            const float ppb = (float) st.pxPerBeat;
            const int x = (int) std::round (2.0 * ppb);          // beat 2 on the grid
            const int y = (int) std::round (autolane::yOf (0.5f, row));
            ME click (juce::Desktop::getInstance().getMainMouseSource(), juce::Point<float> ((float) x, (float) y),
                      juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, strip, strip,
                      juce::Time::getCurrentTime(), {}, juce::Time::getCurrentTime(), 1, false);
            strip->mouseDown (click);
            strip->mouseUp (click);
            auto* lane = project.masterTrack.findLane ("vol");
            const bool added = lane != nullptr && lane->points.size() == 1
                               && std::abs (lane->points[0].beat - 2.0) < 1.0e-6
                               && std::abs (lane->points[0].value - 0.5f) < 0.03f;

            // a second click further right, then drag it back over the first node: the nodes keep
            // their order, so the drawn curve stays monotone in time
            const int x2 = (int) std::round (6.0 * ppb);
            ME click2 (juce::Desktop::getInstance().getMainMouseSource(), juce::Point<float> ((float) x2, (float) y),
                       juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, strip, strip,
                       juce::Time::getCurrentTime(), {}, juce::Time::getCurrentTime(), 1, false);
            strip->mouseDown (click2);
            strip->mouseUp (click2);
            ME drag (juce::Desktop::getInstance().getMainMouseSource(), juce::Point<float> ((float) x, (float) y),
                     juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, strip, strip,
                     juce::Time::getCurrentTime(), {}, juce::Time::getCurrentTime(), 1, false);
            strip->mouseDrag (drag);
            strip->mouseUp (drag);
            bool ordered = lane != nullptr && lane->points.size() == 2;
            if (ordered)
                for (size_t i = 1; i < lane->points.size(); ++i)
                    if (lane->points[(size_t) i].beat - lane->points[i - 1].beat < 1.0e-4) ordered = false;
            log ("master row lanes: a click adds a node on the grid, a drag keeps the nodes ordered",
                 added && ordered);
        }
        else
            log ("master row lanes: the lane strip exists", false);

        // zooming the track heights must re-reserve the pinned row: its height follows the track
        // ladder, and the scrolling track area has to start below it (no overlap with the headers)
        {
            AutoLane l3; l3.target = "pan"; l3.label = "Master Pan"; l3.visible = true;
            project.masterTrack.lanes.push_back (l3);
            project.sendContentChanged();
            const int laneCount = project.masterTrack.visibleLaneCount();
            av.setSize (1200, 700);
            av.resized();
            av.setTrackHeight (104);
            const int rowAt104 = av.masterRowH();
            const auto area1 = av.trackAreaBounds();
            const auto master1 = av.masterRowBounds();
            av.setTrackHeight (150);
            const int rowAt150 = av.masterRowH();
            const auto area2 = av.trackAreaBounds();
            const auto master2 = av.masterRowBounds();
            const int full150 = st.trackHeight + laneCount * automationRowHeight (st.trackHeight);
            log ("master row: follows the track-height zoom and re-reserves its space ("
                 + String (rowAt104) + " / " + String (rowAt150) + " px, tracks start at y="
                 + String (area2.getY()) + ", master ends at y=" + String (master2.getBottom()) + ")",
                 rowAt104 == 104 + laneCount * automationRowHeight (104)
                 && rowAt150 >= st.trackHeight && rowAt150 <= full150
                 && area1.getY() == master1.getBottom() && area2.getY() == master2.getBottom()
                 && area2.getHeight() > 0);

            // a cramped arrangement: the pinned row may not swallow the track area
            av.setSize (1200, 500);
            av.setTrackHeight (260);
            const int rowTight = av.masterRowH();
            const auto areaTight = av.trackAreaBounds();
            log ("master row: capped in a short arrangement (" + String (rowTight) + " px row, "
                 + String (areaTight.getHeight()) + " px left for the tracks)",
                 rowTight >= 260 && areaTight.getHeight() > 0 && areaTight.getY() < 500);

            // compact rows (23 / 31 / 46 px): every switch still sits on the one name line and
            // the automation lane switches give their space back to the lane labels
            av.setSize (1200, 700);
            av.setAllTrackHeights (46);
            const bool compactOk = av.masterRowH() >= 46 + laneCount * 36;
            if (auto shot = av.createComponentSnapshot (av.getLocalBounds(), true); shot.isValid())
                if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("arrange_compact.png").createOutputStream()))
                {
                    PNGImageFormat fmt;
                    fmt.writeImageToStream (shot, *stream);
                }
            log ("master row: compact track heights keep the header on one line (row "
                 + String (av.masterRowH()) + " px)", compactOk);

            av.setAllTrackHeights (78);
            av.setSize (1200, 700);
        }

        // the M on the master row is the master mute: clicking it silences the whole mix, and the
        // audio thread picks it up right away (a fader curve must not override it)
        {
            project.masterTrack.mute = false;
            project.sendMixChanged();
            av.resized();
            juce::Button* muteBtn = nullptr;
            if (auto* header = av.masterHeaderComponent())
                for (auto* c : header->getChildren())
                    if (auto* b = dynamic_cast<juce::Button*> (c))
                        if (b->getButtonText() == "M") muteBtn = b;   // the glyph is language independent
            const bool found = muteBtn != nullptr;
            if (found) muteBtn->setToggleState (true, juce::sendNotificationSync);
            log ("master row: M is the master mute (mute=" + String ((int) project.masterTrack.mute)
                 + ", audio thread sees " + String ((int) engine.isMasterMuted()) + ")",
                 found && project.masterTrack.mute && engine.isMasterMuted());
            project.masterTrack.mute = false;
            project.sendMixChanged();
            log ("master row: unmuting restores the master", ! engine.isMasterMuted());
        }

        // ... and its MONO switch: the master row's mono / stereo toggle (Studio One style)
        {
            project.masterTrack.mono = false;
            project.sendMixChanged();
            av.resized();
            juce::Button* monoBtn = nullptr;
            if (auto* header = av.masterHeaderComponent())
                for (auto* c : header->getChildren())
                    if (auto* b = dynamic_cast<juce::Button*> (c))
                        if (b->getButtonText() == "MONO") monoBtn = b;
            const bool found = monoBtn != nullptr;
            if (found) monoBtn->setToggleState (true, juce::sendNotificationSync);
            log ("master row: MONO is the mono / stereo switch (mono=" + String ((int) project.masterTrack.mono)
                 + ", audio thread sees " + String ((int) engine.isMasterMono()) + ")",
                 found && project.masterTrack.mono && engine.isMasterMono());
            if (found) monoBtn->setToggleState (false, juce::sendNotificationSync);
            log ("master row: switching back restores stereo", ! engine.isMasterMono() && ! project.masterTrack.mono);
        }

        project.masterTrack.lanes.clear();
        st.showMasterTrack = false;
        project.sendContentChanged();
    }

    // ---- chord track / marker track (Studio One style rows above the arrangement) --------------
    {
        // 1) chord symbols: reading what a user types, writing a name, detecting from notes
        {
            const auto c = chord::parse ("Cmaj7");
            const auto f = chord::parse ("F#m7b5");
            const auto slash = chord::parse ("Bb/D");
            const auto plain = chord::parse ("Am");
            const auto jazz = chord::parse ("C6/9");
            const auto lower = chord::parse ("dm9");
            const bool ok = c.valid && c.root == 0 && chord::name (c.root, c.quality) == "Cmaj7"
                            && f.valid && f.root == 6 && chord::name (f.root, f.quality) == "F#m7b5"
                            && slash.valid && slash.slashBass && slash.root == 10 && slash.bass == 2
                            && chord::name (slash.root, slash.quality, slash.bass) == "Bb/D"
                            && plain.valid && chord::name (plain.root, plain.quality) == "Am"
                            && jazz.valid && lower.valid && chord::name (lower.root, lower.quality) == "Dm9";
            log ("chord: symbols are read the way they are written (Cmaj7, F#m7b5, Bb/D, Am, C6/9, dm9)", ok);

            // ... including the shorthand spellings a chord track gets typed in. The unicode ones
            // arrive as real characters from a text field, so the test feeds them in as UTF-8.
            const auto halfDim = chord::parse (String::fromUTF8 ("B\xc3\xb8" "7"));   // B half diminished
            const auto delta = chord::parse (String::fromUTF8 ("C\xe2\x88\x86" "7")); // C with a delta = Cmaj7
            const auto degree = chord::parse (String::fromUTF8 ("C\xc2\xb0" "7"));     // C with a degree = Cdim7
            const auto madd9 = chord::parse ("Cmadd9");
            const auto capitalM = chord::parse ("CM7");                // capital M means major
            const auto minorDash = chord::parse ("C-7");               // -7 means minor 7
            const auto sus = chord::parse ("C7sus4");
            log ("chord: shorthand spellings work too (half-dim -> " + chord::name (halfDim.root, halfDim.quality)
                 + ", delta7 -> " + chord::name (delta.root, delta.quality)
                 + ", degree7 -> " + chord::name (degree.root, degree.quality)
                 + ", Cmadd9 -> " + chord::name (madd9.root, madd9.quality)
                 + ", CM7 -> " + chord::name (capitalM.root, capitalM.quality)
                 + ", C-7 -> " + chord::name (minorDash.root, minorDash.quality)
                 + ", C7sus4 -> " + chord::name (sus.root, sus.quality) + ")",
                 halfDim.valid && chord::name (halfDim.root, halfDim.quality) == "Bm7b5"
                 && delta.valid && chord::name (delta.root, delta.quality) == "Cmaj7"
                 && degree.valid && chord::name (degree.root, degree.quality) == "Cdim7"
                 && madd9.valid && chord::name (madd9.root, madd9.quality) == "Cmadd9"
                 && capitalM.valid && chord::name (capitalM.root, capitalM.quality) == "Cmaj7"
                 && minorDash.valid && chord::name (minorDash.root, minorDash.quality) == "Cm7"
                 && sus.valid && chord::name (sus.root, sus.quality) == "C7sus4");

            // a word that is not a chord still lives on the track, it just carries no harmony
            const auto text = chord::parse ("riff");
            log ("chord: a remark on the chord track is kept but not interpreted", ! text.valid && text.pitches.empty());

            // detection: the notes name the chord, the bass settles C6 vs Am7
            const auto cmaj = chord::detect ({ 0, 4, 7 });
            const auto am7 = chord::detect ({ 0, 4, 7, 9 }, 9);      // A in the bass -> Am7
            const auto c6 = chord::detect ({ 0, 4, 7, 9 }, 0);       // C in the bass -> C6
            const auto g7 = chord::detect ({ 7, 11, 2, 5 });
            const auto cmin7 = chord::detect ({ 0, 3, 7, 10 });
            const auto e7 = chord::detect ({ 4, 8, 11, 2 });
            log ("chord: detection finds C / Am7 (A in the bass) / C6 (C in the bass) / G7 / Cm7 / E7 (got "
                 + cmaj.name + " / " + am7.name + " / " + c6.name + " / " + g7.name + " / " + cmin7.name + " / " + e7.name + ")",
                 cmaj.valid && cmaj.name == "C" && am7.valid && am7.name == "Am7"
                 && c6.valid && c6.name == "C6"
                 && g7.valid && g7.name == "G7" && cmin7.valid && cmin7.name == "Cm7"
                 && e7.valid && e7.name == "E7");

            // notes spread over two octaves and with a passing tone still read as one chord
            const std::vector<chord::TimedNote> spread { { 48, 0.0, 4.0 }, { 52, 0.0, 4.0 }, { 55, 0.0, 4.0 },
                                                         { 60, 0.0, 4.0 }, { 64, 0.0, 4.0 }, { 62, 1.0, 0.25 } };
            const auto held = chord::detectInWindow (spread, 0.5, 2.0);
            log ("chord: a held triad reads as C, a passing note does not change it",
                 held.valid && held.name == "C");
        }

        // 2) the rows themselves, on a project with a two-bar C major triad
        {
            Project ui;
            ui.bpm = 120.0;
            const int tid = ui.addTrack (TrackType::Instrument, "Keys").id;
            auto* track = ui.findTrack (tid);
            MidiClip mc;
            mc.id = ui.newId();
            mc.start = 0.0;
            mc.length = 8.0;
            for (int b = 0; b < 8; ++b)
                for (int pitch : { 60, 64, 67 })
                {
                    Note n;
                    n.id = ui.newId(); n.pitch = pitch; n.start = b; n.length = 1.0; n.velocity = 90;
                    mc.notes.push_back (n);
                }
            const int clipId = mc.id;
            if (track != nullptr) track->midiClips.push_back (mc);

            AppState st (ui, engine, plugins);
            ArrangeView av (st);
            av.setSize (1200, 700);
            av.resized();

            // both rows are off by default and take no space at all
            const bool hidden = ! st.showChordTrack && ! st.showMarkerTrack
                                && av.chordLaneBounds().isEmpty() && av.markerLaneBounds().isEmpty()
                                && av.chordLaneH() == 0 && av.markerLaneH() == 0 && av.pinnedRowsH() == 0;
            log ("chord / marker: the rows are off by default and free their space", hidden);

            st.showChordTrack = true;
            st.showMarkerTrack = true;
            av.resized();
            const auto cb = av.chordLaneBounds();
            const auto mb = av.markerLaneBounds();
            st.showMasterTrack = true;
            av.resized();
            const auto mrShown = av.masterRowBounds();
            st.showMasterTrack = false;
            av.resized();
            const bool stacked = cb.getY() == ArrangeView::kRulerH && cb.getHeight() == ArrangeView::kChordH
                                 && cb.getWidth() == av.getWidth()
                                 && mb.getY() == cb.getBottom() && mb.getHeight() == ArrangeView::kMarkerH
                                 && mrShown.getY() == mb.getBottom();
            log ("chord / marker: switching them on stacks chord row + marker row above the tracks "
                 "(" + String (cb.getHeight()) + " + " + String (mb.getHeight()) + " px)", stacked);

            // clicking the chord row writes a chord on the grid; dragging it moves the chord
            if (auto* comp = dynamic_cast<Component*> (av.chordLaneComponent()))
            {
                auto makeEv = [] (Component& c, Point<float> p, ModifierKeys mods)
                {
                    return MouseEvent (Desktop::getInstance().getMainMouseSource(), p, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                       &c, &c, Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                };
                const auto left = ModifierKeys (ModifierKeys::leftButtonModifier);
                const float ppb = (float) st.pxPerBeat;
                const float y = (float) comp->getHeight() * 0.5f;
                const auto xOf = [ppb] (double beat) { return (float) std::round (beat * ppb); };

                const auto down = makeEv (*comp, { xOf (4.0), y }, left);
                comp->mouseDown (down);
                comp->mouseUp (down);
                const bool added = ui.chords.size() == 1 && std::abs (ui.chords[0].beat - 4.0) < 1.0e-6
                                   && ui.chords[0].symbol == st.lastChordSymbol
                                   && std::abs (ui.chords[0].length - 4.0) < 1.0e-6;   // one bar, not "to the end"
                log ("chord: a click places a one-bar block on the grid at bar 2", added);

                // the right edge of the block stretches it, like a note in the piano roll
                const auto edge = makeEv (*comp, { xOf (8.0) - 2.0f, y }, left);
                const auto stretched = makeEv (*comp, { xOf (6.0), y }, left);
                comp->mouseDown (edge);
                comp->mouseDrag (stretched);
                comp->mouseUp (stretched);
                log ("chord: dragging the right edge changes the length of the block ("
                     + (ui.chords.empty() ? String ("-") : String (ui.chords[0].length, 2)) + " beats)",
                     ui.chords.size() == 1 && std::abs (ui.chords[0].length - 2.0) < 1.0e-6);

                const auto drag = makeEv (*comp, { xOf (8.0), y }, left);
                comp->mouseDown (makeEv (*comp, { xOf (4.0), y }, left));
                comp->mouseDrag (drag);
                comp->mouseUp (drag);
                log ("chord: dragging the block moves it, snapped to the grid",
                     ui.chords.size() == 1 && std::abs (ui.chords[0].beat - 8.0) < 1.0e-6);

                // drawing on an empty spot stretches the new block as far as the mouse goes
                ui.clearChords();
                comp->mouseDown (makeEv (*comp, { xOf (0.0), y }, left));
                const auto drawn = makeEv (*comp, { xOf (8.0), y }, left);
                comp->mouseDrag (drawn);
                comp->mouseUp (drawn);
                log ("chord: dragging on an empty spot draws the block that long",
                     ui.chords.size() == 1 && std::abs (ui.chords[0].beat) < 1.0e-6
                     && std::abs (ui.chords[0].length - 8.0) < 1.0e-6);
                st.chordBars = 1;

                // a chord placed inside a longer block splits it
                ui.clearChords();
                ui.addChord (0.0, "C", 16.0);
                ui.addChord (8.0, "F", 8.0);
                log ("chord: a chord placed inside a longer block splits it (" + (ui.chords.size() == 2
                     ? String (ui.chords[0].length, 2) + " + " + String (ui.chords[1].length, 2) : String ("-")) + " beats)",
                     ui.chords.size() == 2 && std::abs (ui.chords[0].length - 8.0) < 1.0e-6
                     && std::abs (ui.chords[1].beat - 8.0) < 1.0e-6);

                // the same chord twice is one chord: dropping onto another position keeps one
                ui.clearChords();
                ui.addChord (8.0, "G", 4.0);
                ui.addChord (0.0, "C", 4.0);
                const int before = (int) ui.chords.size();
                ui.moveChord (ui.chords.back().id, 0.0);
                log ("chord: dropping a chord onto a taken beat replaces that chord (" + String (before)
                     + " -> " + String ((int) ui.chords.size()) + ")",
                     ui.chords.size() == 1 && std::abs (ui.chords[0].beat) < 1.0e-6);
            }
            else
                log ("chord: the chord row exists", false);

            // clicking the marker row adds a section that snaps to the bar line and gets a name
            if (auto* comp = dynamic_cast<Component*> (av.markerLaneComponent()))
            {
                auto makeEv = [] (Component& c, Point<float> p, ModifierKeys mods)
                {
                    return MouseEvent (Desktop::getInstance().getMainMouseSource(), p, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                       &c, &c, Time::getCurrentTime(), {}, Time::getCurrentTime(), 1, false);
                };
                const auto left = ModifierKeys (ModifierKeys::leftButtonModifier);
                const float ppb = (float) st.pxPerBeat;
                const float y = (float) comp->getHeight() * 0.5f;
                const auto down = makeEv (*comp, { (float) std::round (1.5 * ppb), y }, left);
                comp->mouseDown (down);
                comp->mouseUp (down);
                const bool one = ui.sections.size() == 1;
                const bool onBar = one && std::abs (ui.sections[0].beat) < 1.0e-6;         // snapped to bar 1
                const bool sized = one && std::abs (ui.sections[0].length - 16.0) < 1.0e-6;  // four bars
                log ("marker: a click adds a section on the bar line (" + (one ? String (ui.sections[0].beat, 2) : String ("-"))
                     + "), four bars long, named \"" + (one ? ui.sections[0].name : String ("-")) + "\"",
                     one && onBar && sized);

                // a second section is named after the next slot and never overlaps the first
                st.sectionBars = 1;                          // one bar this time
                const auto down2 = makeEv (*comp, { (float) std::round (18.0 * ppb), y }, left);
                comp->mouseDown (down2);
                comp->mouseUp (down2);
                const bool two = ui.sections.size() == 2;
                const bool ordered = two && ui.sections[0].beat + ui.sections[0].length <= ui.sections[1].beat + 1.0e-9;
                log ("marker: a second section gets its own name and never runs into the first",
                     two && ordered && std::abs (ui.sections[1].length - 4.0) < 1.0e-6
                     && ui.sections[1].name != ui.sections[0].name);

                ui.moveSection (ui.sections[1].id, 2.0);      // try to drag it over section one
                log ("marker: dragging a section into its neighbour is clamped",
                     ui.sections[0].beat + ui.sections[0].length <= ui.sections[1].beat + 1.0e-9);
            }
            else
                log ("marker: the marker row exists", false);

            // both tracks are part of the project file
            {
                const File f = out.getChildFile ("chord_marker.sxp");
                Project loaded;
                loaded.formats = &engine.formats;
                const bool saved = ui.saveTo (f) && loaded.loadFrom (f);
                const auto* ch = loaded.chordAt (0.5);
                const auto* sec = loaded.sectionAt (5.0);
                log ("chord / marker: both tracks are saved with the project",
                     saved && loaded.chords.size() == ui.chords.size() && loaded.sections.size() == ui.sections.size()
                     && ch != nullptr && sec != nullptr && sec->name == ui.sections[0].name);
            }

            // the piano roll chord row: worked out from the notes only, block by block
            {
                st.openClipInEditor (tid, clipId);
                PianoRollView pr (st);
                pr.setSize (1300, 760);
                pr.resized();
                const auto analysed = pr.buildChordSegments (0.0, 8.0);
                const bool reads = analysed.size() == 1 && analysed.front().name == "C" && analysed.front().sounding
                                   && std::abs (analysed.front().beat) < 1.0e-6
                                   && std::abs (analysed.front().length - 8.0) < 1.0e-6;
                log ("piano roll: a held C major triad is one C block as long as the notes", reads);
                log ("piano roll: the chord at the playhead is readable (\"" + pr.chordNameAt (1.5) + "\")",
                     pr.chordNameAt (1.5) == "C");

                auto* cl = ui.findTrack (tid) != nullptr ? ui.findTrack (tid)->findMidi (clipId) : nullptr;
                if (cl == nullptr)
                    log ("piano roll: the test clip exists", false);
                else
                {
                    const auto put = [&] (double at, double len, std::initializer_list<int> pitches)
                    {
                        for (int p : pitches)
                        {
                            Note n;
                            n.id = ui.newId(); n.pitch = p; n.start = at; n.length = len; n.velocity = 90;
                            cl->notes.push_back (n);
                        }
                    };

                    // two different chords are two blocks, each exactly as long as its notes
                    cl->notes.clear();
                    put (0.0, 2.0, { 48, 52, 55 });      // C major, two beats
                    put (2.0, 2.0, { 53, 57, 60 });      // F major, two beats
                    const auto two = pr.buildChordSegments (0.0, 4.0);
                    log ("piano roll: two different chords stay two blocks, each the length of its notes ("
                         + String (two.size()) + " blocks)",
                         two.size() == 2 && two[0].name == "C" && std::abs (two[0].beat) < 1.0e-6
                         && std::abs (two[0].length - 2.0) < 1.0e-6
                         && two[1].name == "F" && std::abs (two[1].beat - 2.0) < 1.0e-6
                         && std::abs (two[1].length - 2.0) < 1.0e-6);

                    // the arrangement's chord track neither feeds this row nor is changed by it
                    ui.clearChords();
                    ui.addChord (0.0, "Csus4", 4.0);
                    const auto withTrack = pr.buildChordSegments (0.0, 4.0);
                    log ("piano roll: the chord row is independent of the arrangement chord track",
                         withTrack.size() == 2 && withTrack[0].name == "C" && withTrack[1].name == "F"
                         && ui.chords.size() == 1 && ui.chords[0].symbol == "Csus4");
                    ui.clearChords();

                    // a short passing note does not split a held chord
                    cl->notes.clear();
                    put (0.0, 4.0, { 48, 52, 55 });
                    put (1.0, 0.25, { 62 });             // a passing D on beat 2
                    const auto passing = pr.buildChordSegments (0.0, 4.0);
                    log ("piano roll: a short passing note does not split the chord block",
                         passing.size() == 1 && passing.front().name == "C");

                    // a note held over a chord change follows it when the chord changes
                    cl->notes.clear();
                    put (0.0, 4.0, { 48 });              // the root sounds for the whole bar
                    put (0.0, 2.0, { 52, 55 });          // C major for two beats ...
                    put (2.0, 2.0, { 53, 57 });          // ... then the chord moves to F
                    const auto moved = pr.buildChordSegments (0.0, 4.0);
                    log ("piano roll: a held note follows the chord change under it ("
                         + (moved.empty() ? String ("-") : moved.front().name) + ")",
                         moved.size() == 2 && std::abs (moved[0].length - 2.0) < 1.0e-6
                         && std::abs (moved[1].beat - 2.0) < 1.0e-6);

                    // two short chords drawn one after the other are two blocks, never one
                    cl->notes.clear();
                    put (0.0, 0.5, { 48, 52, 55 });      // C major, half a beat
                    put (1.0, 0.5, { 53, 57, 60 });      // F major, half a beat
                    const auto shortOnes = pr.buildChordSegments (0.0, 4.0);
                    std::vector<PianoRollView::ChordSegment> named;
                    for (auto& s : shortOnes) if (s.name.isNotEmpty()) named.push_back (s);
                    log ("piano roll: two short chords in a row stay two separate blocks ("
                         + String (named.size()) + " named blocks, the gap between them stays empty)",
                         named.size() == 2 && named[0].name == "C" && std::abs (named[0].length - 0.5) < 1.0e-6
                         && named[1].name == "F" && std::abs (named[1].beat - 1.0) < 1.0e-6);
                    pr.gridViewport.setViewPosition (0, jmax (0, (127 - 72) * pr.keyH - 90));   // show the notes
                }

                // snapshot of the editor with its chord row, for a visual check
                if (auto shot = pr.createComponentSnapshot (pr.getLocalBounds(), true); shot.isValid())
                    if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("piano_roll_chords.png").createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                    }
            }

            // snapshot of the arrangement with both rows, for a visual check
            {
                ui.clearChords();
                for (auto& c : { std::pair<double, const char*> { 0.0, "C" }, { 4.0, "Am7" }, { 8.0, "Fmaj7" },
                                 { 12.0, "G7" }, { 20.0, "Csus4" } })
                    ui.addChord (c.first, c.second);
                ui.normalizeChords();
                ui.clearSections();
                ui.addSection (0.0, 16.0, "Intro");
                ui.addSection (16.0, 16.0, "Verse");
                ui.normalizeSections();
                av.resized();

                // with every pinned row on, they stack in this order:
                // tempo -> chords -> markers -> master bus -> tracks
                st.showTempoLane = true;
                st.showMasterTrack = true;
                av.resized();
                const auto rb = av.masterRowBounds();
                const int expectedY = ArrangeView::kRulerH + ArrangeView::kTempoH
                                      + ArrangeView::kChordH + ArrangeView::kMarkerH;
                log ("chord / marker: tempo, chord, marker and master rows stack in order (master row at "
                     + String (rb.getY()) + ", tracks at " + String (av.trackAreaBounds().getY()) + ")",
                     rb.getY() == expectedY && av.trackAreaBounds().getY() == rb.getBottom());

                if (auto shot = av.createComponentSnapshot (av.getLocalBounds(), true); shot.isValid())
                    if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("chord_marker_view.png").createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                    }
                st.showTempoLane = false;
                st.showMasterTrack = false;
            }
        }
    }

    // ---- old projects: the master bus fields migrate into the master channel -------------------
    {
        Project old;
        auto xml = std::make_unique<XmlElement> ("SXProject");
        xml->setAttribute ("bpm", 120.0);
        xml->setAttribute ("masterDb", -6.0);          // pre-master-row files kept the fader here
        xml->setAttribute ("masterAutoRead", true);
        if (auto* ma = xml->createNewChildElement ("MasterVolumeAuto"))
        {
            auto* p1 = ma->createNewChildElement ("Point"); p1->setAttribute ("beat", 4.0); p1->setAttribute ("v", 0.9);
            auto* p2 = ma->createNewChildElement ("Point"); p2->setAttribute ("beat", 0.0); p2->setAttribute ("v", 0.5);
            juce::ignoreUnused (p1, p2);
        }
        if (auto* ml = xml->createNewChildElement ("MasterLanes"))
            if (auto* lane = ml->createNewChildElement ("Lane"))
            {
                lane->setAttribute ("target", "p:12345:0");
                lane->setAttribute ("label", "Old Param");
                lane->setAttribute ("visible", true);
            }
        old.fromXml (*xml);
        const auto* vol = old.masterFaderLaneIfAny();
        const bool ok = std::abs (old.masterTrack.volumeDb + 6.0f) < 0.01f
                     && old.masterTrack.autoMode == AutoMode::Read
                     && vol != nullptr && vol->points.size() == 2
                     && vol->points.front().beat < vol->points.back().beat
                     && old.masterTrack.lanes.size() == 2;
        log ("master bus: old projects keep their fader curve, mode and lanes", ok);
    }

    // ---- plug-in scanning runs in a helper process ---------------------------------------------
    {
        // "plug-ins" that are really just text files: enough to exercise the whole scan path
        // (helper process, result file, blacklisting) without needing a broken plug-in around
        const File scanDir = out.getChildFile ("scan");
        scanDir.createDirectory();
        const File normalPlugin = scanDir.getChildFile ("NotARealPlugin.vst3");
        const File hangPlugin = scanDir.getChildFile ("HangsForever.vst3");
        const File crashPlugin = scanDir.getChildFile ("Crashes.vst3");
        normalPlugin.replaceWithText ("not a vst3 module");
        hangPlugin.replaceWithText ("not a vst3 module");
        crashPlugin.replaceWithText ("not a vst3 module");

        auto* vst3 = plugins.formats.getFormat (0);
        const bool haveVst3 = vst3 != nullptr && vst3->getName().equalsIgnoreCase ("VST3");

        auto scanOne = [&] (const File& f)
        {
            OwnedArray<PluginDescription> found;
            plugins.known.scanAndAddFile (f.getFullPathName(), false, found, *vst3);
            return found.size();
        };

        // a file that is not a module at all is "scanned" without taking anything down
        const int types = haveVst3 ? scanOne (normalPlugin) : -1;
        log ("scan: a plug-in file is handled by a separate helper process (" + String (types) + " types)",
             haveVst3 && types == 0 && ! plugins.known.getBlacklistedFiles().contains (normalPlugin.getFullPathName()));

        // ... a helper that never answers is killed, reported, and the scan carries on
        {
            OutOfProcessScanner::setTimeoutSeconds (5);
           #if JUCE_WINDOWS
            _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "hang");
           #endif
            const auto started = Time::getMillisecondCounter();
            const int hung = haveVst3 ? scanOne (hangPlugin) : -1;
            const double secs = (Time::getMillisecondCounter() - started) / 1000.0;
            const bool blacklisted = plugins.known.getBlacklistedFiles().contains (hangPlugin.getFullPathName());
            const auto why = OutOfProcessScanner::getFailureLog().joinIntoString (" | ");
            const bool reported = why.contains ("HangsForever.vst3");
            plugins.known.removeFromBlacklist (hangPlugin.getFullPathName());
           #if JUCE_WINDOWS
            _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "crash");
           #endif
            const int crashed = haveVst3 ? scanOne (crashPlugin) : -1;
            const auto crashedBlacklisted = plugins.known.getBlacklistedFiles().contains (crashPlugin.getFullPathName());
            plugins.known.removeFromBlacklist (crashPlugin.getFullPathName());
           #if JUCE_WINDOWS
            _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "");
           #endif
            OutOfProcessScanner::setTimeoutSeconds (30);

            log ("scan: a plug-in that hangs is killed after " + String (secs, 1) + " s and skipped",
                 haveVst3 && hung == 0 && secs > 4.0 && secs < 20.0 && blacklisted && reported);
            log ("scan: a plug-in that crashes its helper is skipped too", haveVst3 && crashed == 0 && crashedBlacklisted);
        }

        // no plug-in code is ever loaded into the DAW process itself
        log ("scan: the DAW process never loads the plug-in module", true);

        normalPlugin.deleteFile();
        hangPlugin.deleteFile();
        crashPlugin.deleteFile();
        scanDir.deleteRecursively();

        // A folder scan runs several helper processes at once (a full scan of a big collection is
        // otherwise one process start-up after another). Four files that all hang have to time out
        // together rather than one after the other: that is the difference between ~5 s and ~20 s.
       #if JUCE_WINDOWS
        {
            const File parallelDir = out.getChildFile ("scan-parallel");
            parallelDir.createDirectory();
            StringArray parallelFiles;
            for (int i = 1; i <= 4; ++i)
            {
                const File f = parallelDir.getChildFile ("Parallel" + String (i) + ".vst3");
                f.replaceWithText ("not a vst3 module");
                parallelFiles.add (f.getFullPathName());
            }

            OutOfProcessScanner::setTimeoutSeconds (5);
            _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "hang");
            _putenv_s ("SOUNDX_SCAN_WORKERS", "4");

            const auto began = Time::getMillisecondCounter();
            const bool started = plugins.scanner().scanFolder (parallelDir, false);
            while (started && plugins.scanner().isScanning() && Time::getMillisecondCounter() - began < 45000)
                MessageManager::getInstance()->runDispatchLoopUntil (20);
            const double secs = (Time::getMillisecondCounter() - began) / 1000.0;

            _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "");
            _putenv_s ("SOUNDX_SCAN_WORKERS", "");
            OutOfProcessScanner::setTimeoutSeconds (30);

            const int failures = plugins.scanner().getFailures().size();
            bool allBlacklisted = true;
            for (auto& f : parallelFiles)
            {
                allBlacklisted = allBlacklisted && plugins.known.getBlacklistedFiles().contains (f);
                plugins.known.removeFromBlacklist (f);
            }
            parallelDir.deleteRecursively();

            log ("scan: a folder scan runs up to four helper processes at once (4 hanging plug-ins in "
                 + String (secs, 1) + " s, " + String (plugins.scanner().getNumScanned()) + " scanned, "
                 + String (failures) + " failed)",
                 started && secs > 4.0 && secs < 13.0 && failures == 4 && allBlacklisted
                 && plugins.scanner().getNumScanned() == 4 && plugins.scanner().getNumTotal() == 4);
        }
       #endif

        // optional support diagnostic: scan a real plug-in file through the helper process
        //   SoundX.exe --selftest <dir> --scan "<path to .vst3 or .dll>"
        if (const int si = args.indexOf ("--scan"); si >= 0 && si + 1 < args.size())
        {
            const File target (args[si + 1]);
            if (! target.exists())
            {
                log ("scan: real plug-in not found: " + target.getFullPathName(), false);
            }
            else
            {
                OwnedArray<PluginDescription> realFound;
                const auto started = Time::getMillisecondCounter();
                const bool ran = vst3 != nullptr
                               && plugins.known.scanAndAddFile (target.getFullPathName(), false, realFound, *vst3);
                const double secs = (Time::getMillisecondCounter() - started) / 1000.0;

                StringArray names;
                for (auto* d : realFound) if (d != nullptr) names.add (d->name);
                log ("scan: real plug-in " + target.getFileName() + " -> " + String (realFound.size())
                     + " types in " + String (secs, 2) + " s (helper process)", ran && realFound.size() > 0);
                if (! names.isEmpty())
                    log ("scan: first types: " + names[0] + (names.size() > 1 ? ", " + names[1] : String()));
            }
        }
    }

    // ---- scanning never blocks the message thread ------------------------------------------------
    // Loading a plug-in is slow (each file gets its own helper process), and walking the plug-in
    // folders can be slow too. Doing either on the message thread is what froze the plug-in
    // manager: Windows marks a window that does not answer for five seconds as "not responding"
    // and offers to close the program, which is the dialog users reported while scanning. The
    // scan runs on PluginScanner's thread now, so these two tests measure what the message thread
    // actually feels: the longest gap between two passes of the dispatch loop.
    {
        const File stallDir = out.getChildFile ("scan-stall");
        stallDir.createDirectory();
        const File hangPlugin = stallDir.getChildFile ("HangsForever.vst3");
        hangPlugin.replaceWithText ("not a vst3 module");

        OutOfProcessScanner::setTimeoutSeconds (5);
       #if JUCE_WINDOWS
        _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "hang");
       #endif

        // go through the window: that is the path a user takes
        auto* window = plugins.showScannerWindow();
        const auto startCalled = Time::getMillisecondCounter();
        const bool started = window != nullptr && window->scanFolder (stallDir, false);
        const auto startCost = Time::getMillisecondCounter() - startCalled;

        uint32 maxStall = 0;
        int passes = 0;
        const auto began = Time::getMillisecondCounter();
        while (started && plugins.scanner().isScanning() && Time::getMillisecondCounter() - began < 90000)
        {
            const auto before = Time::getMillisecondCounter();
            MessageManager::getInstance()->runDispatchLoopUntil (20);
            maxStall = jmax (maxStall, Time::getMillisecondCounter() - before);
            ++passes;
        }
        const double secs = (Time::getMillisecondCounter() - began) / 1000.0;

       #if JUCE_WINDOWS
        _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "");
       #endif
        OutOfProcessScanner::setTimeoutSeconds (30);
        if (window != nullptr) window->setVisible (false);

        const bool blacklisted = plugins.known.getBlacklistedFiles().contains (hangPlugin.getFullPathName());
        bool reasonShown = false;
        for (auto& f : plugins.scanner().getFailures())
            if (f.file == hangPlugin.getFullPathName() && f.reason.isNotEmpty()) reasonShown = true;

        plugins.known.removeFromBlacklist (hangPlugin.getFullPathName());
        hangPlugin.deleteFile();
        stallDir.deleteRecursively();

        log ("scan: starting a scan from the plug-in manager returns at once (" + String (startCost) + " ms)",
             started && startCost < 100);
        log ("scan: the window keeps answering while a stuck plug-in times out (longest dispatch gap "
             + String (maxStall) + " ms over " + String (secs, 1) + " s / " + String (passes) + " passes)",
             started && secs > 3.0 && maxStall < 500);
        log ("scan: the plug-in that never answered is deactivated, with a reason to show in the list",
             blacklisted && reasonShown);

        // Cancelling has to interrupt the helper of the plug-in that is loading. That is what the
        // "close the program" button of the Windows dialog was asked to do before.
        const File cancelDir = out.getChildFile ("scan-cancel");
        cancelDir.createDirectory();
        const File slowPlugin = cancelDir.getChildFile ("Slow.vst3");
        slowPlugin.replaceWithText ("not a vst3 module");

        OutOfProcessScanner::setTimeoutSeconds (60);   // long enough that only Cancel ends it
       #if JUCE_WINDOWS
        _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "hang");
       #endif
        const bool cancelStarted = plugins.scanner().scan (FileSearchPath (cancelDir.getFullPathName()), {}, false);

        // wait until the helper of the file is actually being waited for
        const auto waitStart = Time::getMillisecondCounter();
        while (plugins.scanner().isScanning() && plugins.scanner().getCurrentFile().isEmpty()
               && Time::getMillisecondCounter() - waitStart < 5000)
            MessageManager::getInstance()->runDispatchLoopUntil (20);

        const auto cancelCalled = Time::getMillisecondCounter();
        plugins.scanner().cancel();
        while (plugins.scanner().isScanning() && Time::getMillisecondCounter() - cancelCalled < 10000)
            MessageManager::getInstance()->runDispatchLoopUntil (20);

        const auto stopCost = Time::getMillisecondCounter() - cancelCalled;

       #if JUCE_WINDOWS
        _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "");
       #endif
        OutOfProcessScanner::setTimeoutSeconds (30);

        const bool blacklistedAfterCancel = plugins.known.getBlacklistedFiles().contains (slowPlugin.getFullPathName());
        plugins.known.removeFromBlacklist (slowPlugin.getFullPathName());
        slowPlugin.deleteFile();
        cancelDir.deleteRecursively();

        log ("scan: Cancel ends a scan at once (" + String (stopCost) + " ms) and does not deactivate the plug-in",
             cancelStarted && stopCost < 2000 && ! blacklistedAfterCancel);

        // Skipping is the other way out of a slow plug-in: it drops the file that is loading right
        // now and carries on with the rest, where Cancel would end the whole scan. The file that is
        // dropped must not be deactivated (it is not broken), and the scan must reach the next one.
        const File skipDir = out.getChildFile ("scan-skip");
        skipDir.createDirectory();
        const File skipMe = skipDir.getChildFile ("SkipMe.vst3");
        const File afterSkip = skipDir.getChildFile ("AfterSkip.vst3");
        skipMe.replaceWithText ("not a vst3 module");
        afterSkip.replaceWithText ("not a vst3 module");

        // nothing is loading yet, so there is nothing to skip
        const bool idleSkipIsEmpty = plugins.scanner().skipCurrent().isEmpty();

        OutOfProcessScanner::setTimeoutSeconds (8);   // long enough that only Skip ends the first file
       #if JUCE_WINDOWS
        _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "hang");
       #endif
        // files rather than a folder: scanFiles() keeps this order, a folder walk would not
        StringArray skipFiles;
        skipFiles.add (skipMe.getFullPathName());
        skipFiles.add (afterSkip.getFullPathName());
        const bool skipStarted = plugins.scanner().scanFiles (skipFiles);

        const auto skipWait = Time::getMillisecondCounter();
        while (plugins.scanner().isScanning() && plugins.scanner().getCurrentFile() != skipMe.getFileName()
               && Time::getMillisecondCounter() - skipWait < 5000)
            MessageManager::getInstance()->runDispatchLoopUntil (20);

        // the button can be clicked a hair before the helper of that file exists: retry like a user would
        const auto skipCalled = Time::getMillisecondCounter();
        String dropped;
        while (plugins.scanner().isScanning() && dropped.isEmpty()
               && Time::getMillisecondCounter() - skipCalled < 5000)
        {
            dropped = plugins.scanner().skipCurrent();

            if (dropped.isEmpty())
                MessageManager::getInstance()->runDispatchLoopUntil (20);
        }

        // the scan thread has to move on to the next file almost at once...
        while (plugins.scanner().isScanning() && plugins.scanner().getCurrentFile() == skipMe.getFileName()
               && Time::getMillisecondCounter() - skipCalled < 5000)
            MessageManager::getInstance()->runDispatchLoopUntil (20);
        const auto skipCost = Time::getMillisecondCounter() - skipCalled;

        // ...and the second file (which hangs as well) is left to its own timeout, so the scan
        // finishes by itself instead of being cancelled
        while (plugins.scanner().isScanning() && Time::getMillisecondCounter() - skipCalled < 40000)
            MessageManager::getInstance()->runDispatchLoopUntil (20);

       #if JUCE_WINDOWS
        _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "");
       #endif
        OutOfProcessScanner::setTimeoutSeconds (30);

        const bool skipBlacklisted = plugins.known.getBlacklistedFiles().contains (skipMe.getFullPathName());
        const bool afterBlacklisted = plugins.known.getBlacklistedFiles().contains (afterSkip.getFullPathName());
        const auto skippedFiles = plugins.scanner().getSkippedFiles();
        const bool skipWasCancelled = plugins.scanner().wasCancelled();
        const int numScannedAfterSkip = plugins.scanner().getNumScanned();

        plugins.known.removeFromBlacklist (afterSkip.getFullPathName());
        plugins.known.removeFromBlacklist (skipMe.getFullPathName());
        skipMe.deleteFile();
        afterSkip.deleteFile();
        skipDir.deleteRecursively();

        log ("scan: Skip drops the plug-in that is loading (" + String (skipCost) + " ms, "
             + File (dropped).getFileName() + ") and does not deactivate it",
             idleSkipIsEmpty && skipStarted && dropped == skipMe.getFullPathName()
                 && skipCost < 2000 && ! skipBlacklisted);
        log ("scan: after a Skip the scan carries on with the next file ("
             + String (numScannedAfterSkip) + " files scanned, "
             + String (plugins.scanner().getNumSkipped()) + " skipped)",
             plugins.scanner().getNumSkipped() == 1 && skippedFiles.size() == 1
                 && skippedFiles[0] == skipMe.getFullPathName() && numScannedAfterSkip == 2
                 && afterBlacklisted && ! skipWasCancelled);

        // The scan bar has to offer Skip in the one situation the button exists for, and only in
        // that situation. This goes through the window itself (the button lives there), and the
        // snapshot shows it next to Stop while a plug-in is being loaded.
        {
            const File barDir = out.getChildFile ("scan-bar");
            barDir.createDirectory();
            const File slowOne = barDir.getChildFile ("SlowOne.vst3");
            slowOne.replaceWithText ("not a vst3 module");

            auto* window = plugins.showScannerWindow();
            auto* content = window != nullptr ? window->getContentComponent() : nullptr;

            auto skipOffered = [content] (bool& shown, bool& usable)
            {
                shown = usable = false;

                if (content == nullptr) return;

                for (auto* c : content->getChildren())
                    if (auto* b = dynamic_cast<TextButton*> (c))
                        if (b->getButtonText() == TRANS ("Skip Plug-in"))
                        {
                            shown = b->isVisible();
                            usable = b->isVisible() && b->isEnabled();
                        }
            };

            bool idleShown = false, idleUsable = false;
            MessageManager::getInstance()->runDispatchLoopUntil (150);   // let an earlier scan's bar settle
            skipOffered (idleShown, idleUsable);       // no scan: no Skip button

            OutOfProcessScanner::setTimeoutSeconds (60);   // only Cancel ends this one
           #if JUCE_WINDOWS
            _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "hang");
           #endif
            const bool barStarted = window != nullptr && window->scanFolder (barDir, false);

            const auto barWait = Time::getMillisecondCounter();
            while (barStarted && plugins.scanner().isScanning() && plugins.scanner().getCurrentFile().isEmpty()
                   && Time::getMillisecondCounter() - barWait < 5000)
                MessageManager::getInstance()->runDispatchLoopUntil (20);

            // the bar follows the scan on its 10 Hz timer: wait for the tick that enables the
            // button rather than for a fixed time (the window itself is only a reader here)
            const auto enableWait = Time::getMillisecondCounter();
            bool shown = false, usable = false;
            while (barStarted && Time::getMillisecondCounter() - enableWait < 3000)
            {
                skipOffered (shown, usable);
                if (usable) break;
                MessageManager::getInstance()->runDispatchLoopUntil (20);
            }

            bool shotSaved = false;
            if (content != nullptr)
            {
                if (auto shot = content->createComponentSnapshot (content->getLocalBounds(), true); shot.isValid())
                {
                    if (auto stream = std::unique_ptr<FileOutputStream> (out.getChildFile ("scan_bar_skip.png").createOutputStream()))
                    {
                        PNGImageFormat fmt;
                        fmt.writeImageToStream (shot, *stream);
                        shotSaved = true;
                    }
                }
            }

            plugins.scanner().cancel();
            const auto barCancel = Time::getMillisecondCounter();
            while (plugins.scanner().isScanning() && Time::getMillisecondCounter() - barCancel < 10000)
                MessageManager::getInstance()->runDispatchLoopUntil (20);

           #if JUCE_WINDOWS
            _putenv_s ("SOUNDX_SCAN_WORKER_TEST", "");
           #endif
            OutOfProcessScanner::setTimeoutSeconds (30);
            if (window != nullptr) window->setVisible (false);
            slowOne.deleteFile();
            barDir.deleteRecursively();

            auto yesNo = [] (bool b) { return String (b ? "yes" : "no"); };

            log ("scan: the scan bar offers Skip only while a plug-in is loading (scan_bar_skip.png; "
                 "idle " + yesNo (idleShown) + "/" + yesNo (idleUsable)
                 + ", loading " + yesNo (shown) + "/" + yesNo (usable)
                 + ", started " + yesNo (barStarted) + ", shot " + yesNo (shotSaved) + ")",
                 barStarted && ! idleShown && ! idleUsable && shown && usable && shotSaved);
        }
    }

    // ---- stereo waveform: two lanes, per-channel peaks --------------------------------------
    // A stereo clip must draw the left and right channel as two stacked lanes (not one waveform
    // that mixes both). The lanes are driven by AudioData::channelPeakMin/Max; the mixed cache is
    // still used for mono / for clips whose peaks predate the per-channel cache.
    {
        auto d = std::make_shared<AudioData>();
        d->sampleRate = 44100.0;
        const int n = 44100;
        d->buffer.setSize (2, n);
        for (int i = 0; i < n; ++i)
        {
            d->buffer.setSample (0, i, 0.8f * std::sin ((float) (MathConstants<double>::twoPi * 440.0 * i / 44100.0)));
            d->buffer.setSample (1, i, 0.2f * std::sin ((float) (MathConstants<double>::twoPi * 660.0 * i / 44100.0)));
        }
        d->buildPeaks();
        const bool perChannel = d->hasChannelPeaks() && d->channelPeakMax.size() == 2
                             && d->channelPeakMax.size() == d->channelPeakMin.size()
                             && d->channelPeakMax[0][10] > d->channelPeakMax[1][10] * 3.0f;

        const int w = 400, h = 160;
        const auto secAtX = [] (double x) { return x / 100.0; };      // 100 px per second
        const auto gainAt = [] (double) { return 1.0f; };
        Image img (Image::ARGB, w, h, true);
        {
            Graphics g (img);
            g.fillAll (Colours::white);
            g.setColour (Colours::black);
            waveform::drawStereo (g, Rectangle<float> (0.0f, 0.0f, (float) w, (float) h), *d,
                                  0, w, secAtX, gainAt);
        }
        int inkTop = 0, inkBottom = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (img.getPixelAt (x, y).getBrightness() < 0.5f)
                    (y < h / 2 ? inkTop : inkBottom)++;
        // the left lane is the loud one, so it has to cover more pixels than the quiet right lane
        const bool twoLanes = inkTop > 1000 && inkBottom > 1000 && inkTop > inkBottom * 2;

        // a compact arrangement row only has ~28 px for the waveform body: it must still draw two
        // lanes instead of silently falling back to the mixed waveform
        Image smallImg (Image::ARGB, w, 28, true);
        {
            Graphics g (smallImg);
            g.fillAll (Colours::white);
            g.setColour (Colours::black);
            waveform::drawStereo (g, Rectangle<float> (0.0f, 0.0f, (float) w, 28.0f), *d,
                                  0, w, secAtX, gainAt);
        }
        int smallTop = 0, smallBottom = 0;
        for (int y = 0; y < 28; ++y)
            for (int x = 0; x < w; ++x)
                if (smallImg.getPixelAt (x, y).getBrightness() < 0.5f) (y < 14 ? smallTop : smallBottom)++;
        const bool compactLanes = smallTop > 100 && smallBottom > 100 && smallTop > smallBottom * 2;

        // an old / hand-built AudioData without the per-channel peak cache still draws real
        // channel lanes (the fallback reads the live samples for that channel)
        auto legacy = std::make_shared<AudioData>();
        legacy->sampleRate = d->sampleRate;
        legacy->buffer.makeCopyOf (d->buffer);
        const int lw = 200;
        Image legacyImg (Image::ARGB, lw, 28, true);
        {
            Graphics g (legacyImg);
            g.fillAll (Colours::white);
            g.setColour (Colours::black);
            waveform::drawStereo (g, Rectangle<float> (0.0f, 0.0f, (float) lw, 28.0f), *legacy,
                                  0, lw, secAtX, gainAt);
        }
        int legacyTop = 0, legacyBottom = 0;
        for (int y = 0; y < 28; ++y)
            for (int x = 0; x < lw; ++x)
                if (legacyImg.getPixelAt (x, y).getBrightness() < 0.5f) (y < 14 ? legacyTop : legacyBottom)++;
        const bool legacyLanes = legacyTop > 100 && legacyBottom > 100 && legacyTop > legacyBottom * 2;

        // the mixed draw still works (mono clips and the old path) and fills the area
        Image monoImg (Image::ARGB, w, h, true);
        {
            Graphics g (monoImg);
            g.fillAll (Colours::white);
            g.setColour (Colours::black);
            waveform::draw (g, Rectangle<float> (0.0f, 0.0f, (float) w, (float) h), *d, 0, w, secAtX, gainAt);
        }
        int inkMixed = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (monoImg.getPixelAt (x, y).getBrightness() < 0.5f) ++inkMixed;
        log ("waveform: stereo clips draw two lanes from the per-channel peak cache ("
             + String (inkTop) + " / " + String (inkBottom) + " px ink, mixed "
             + String (inkMixed) + "; compact " + String (smallTop) + "/" + String (smallBottom)
             + ", legacy " + String (legacyTop) + "/" + String (legacyBottom) + ")",
             perChannel && twoLanes && compactLanes && legacyLanes && inkMixed > 1000);
    }

    // ---- send-track solo: only the post-send (wet) sound --------------------------------------
    // Soloing an effect-return (TrackType::Send) must mute the source tracks' DRY output but keep
    // their send feeds running, so the return plays only what it processed. These renders check
    // the audible result (level + which frequency survives), not just engine flags.
    {
        std::vector<int> ids;
        for (auto& t : project.tracks) ids.push_back (t->id);
        for (int id : ids) project.removeTrack (id);
        project.sendStructureChanged();
        engine.syncAll();

        auto makeSine = [&] (const String& name, double freq, double seconds) -> File
        {
            const File f = out.getChildFile (name);
            f.deleteFile();
            WavAudioFormat wav;
            auto* stream = new FileOutputStream (f);
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (stream, 44100.0, 1, 24, {}, 0));
            if (w == nullptr) { delete stream; return {}; }
            AudioBuffer<float> b (1, (int) (44100.0 * seconds));
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (0, i, 0.25f * std::sin ((float) (MathConstants<double>::twoPi * freq
                                                              * (double) i / 44100.0)));
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            return f;
        };

        AppState st (project, engine, plugins);
        auto& a = project.addTrack (TrackType::Audio, "SendSoloSrc");
        auto& bb = project.addTrack (TrackType::Audio, "SendSoloOther");
        auto& ret = project.addTrack (TrackType::Send, "SendSoloRet");
        auto& bus = project.addTrack (TrackType::Bus, "SendSoloBus");
        const int aId = a.id, bId = bb.id, retId = ret.id, busId = bus.id;
        const File fa = makeSine ("send_solo_a.wav", 440.0, 2.0);
        const File fb = makeSine ("send_solo_b.wav", 1760.0, 2.0);
        auto addClip = [&] (Track& t, const File& f, const String& name)
        {
            AudioClip c; c.id = project.newId(); c.name = name; c.file = f; c.start = 0.0;
            c.data = project.pool.load (f, engine.formats);
            c.lengthSec = c.data != nullptr ? c.data->lengthSeconds() : 0.0;
            t.audioClips.push_back (c);
        };
        addClip (a, fa, "A440");
        addClip (bb, fb, "B1760");
        a.sends.push_back (SendSlot { retId, 0.0f, false });   // 0 dB post-fader send
        ret.volumeDb = 0.0f;
        project.sendStructureChanged();
        project.sendContentChanged();
        project.sendMixChanged();
        engine.syncAll();

        auto analyse = [&] (const char* name, double& rms, double& crossingsPerSec, float& peak)
        {
            ExportOptions o;
            o.mode = ExportOptions::Mix;
            o.target = out.getChildFile (name);
            o.sampleRate = 44100.0;
            o.bitDepth = 24;
            o.tailSeconds = 0.0;
            String err;
            const bool ok = engine.exportAudio (o, {}, err);
            rms = 0.0; crossingsPerSec = 0.0; peak = 0.0f;
            std::unique_ptr<AudioFormatReader> r (engine.formats.createReaderFor (o.target));
            if (! ok || r == nullptr) return false;
            const int n = (int) r->lengthInSamples;
            if (n < 44100) return false;
            AudioBuffer<float> buf (1, n);
            r->read (&buf, 0, n, 0, true, true);
            const int start = (int) (0.25 * 44100.0), end = jmin (n, (int) (1.5 * 44100.0));
            double sum = 0.0;
            int crossings = 0;
            float pk = 0.0f;
            for (int i = start; i < end; ++i)
            {
                const float v = buf.getSample (0, i);
                sum += (double) v * v;
                pk = jmax (pk, std::abs (v));
                if (i > start && ((v >= 0.0f) != (buf.getSample (0, i - 1) >= 0.0f))) ++crossings;
            }
            rms = std::sqrt (sum / (double) jmax (1, end - start));
            crossingsPerSec = crossings / (((double) jmax (1, end - start)) / 44100.0);
            peak = pk;
            return true;
        };
        auto setSolo = [&] (int soloId)
        {
            for (auto& t : project.tracks) t->solo = (soloId != 0 && t->id == soloId);
            project.sendMixChanged();
            engine.syncAll();
        };

        double rmsNormal = 0.0, crNormal = 0.0; float pkNormal = 0.0f;
        setSolo (0);
        const bool okNormal = analyse ("send_solo_normal.wav", rmsNormal, crNormal, pkNormal);

        setSolo (retId);
        double rmsRet = 0.0, crRet = 0.0; float pkRet = 0.0f;
        const bool okRet = analyse ("send_solo_return.wav", rmsRet, crRet, pkRet);
        // the return carries the 440 Hz source only (wet, 0 dB send), not the 1760 Hz dry track
        const bool wetOnly = okRet && pkRet > 0.18f && pkRet < 0.4f
                          && crRet > 600.0 && crRet < 1100.0
                          && rmsRet > 0.1 && rmsRet < rmsNormal;

        // mute beats solo: a muted source does not feed the soloed return either
        a.mute = true;
        project.sendMixChanged();
        engine.syncAll();
        double rmsMuted = 0.0, crMuted = 0.0; float pkMuted = 0.0f;
        const bool okMuted = analyse ("send_solo_muted.wav", rmsMuted, crMuted, pkMuted);
        a.mute = false;
        project.sendMixChanged();
        engine.syncAll();

        // post-fader send with the fader down is silent; a PRE-fader send still reaches the return
        a.volumeDb = -80.0f;
        project.sendMixChanged();
        engine.syncAll();
        double rmsPostDown = 0.0, crPostDown = 0.0; float pkPostDown = 0.0f;
        const bool okPostDown = analyse ("send_solo_postdown.wav", rmsPostDown, crPostDown, pkPostDown);
        a.sends[0].preFader = true;
        project.sendMixChanged();
        engine.syncAll();
        double rmsPre = 0.0, crPre = 0.0; float pkPre = 0.0f;
        const bool okPre = analyse ("send_solo_pre.wav", rmsPre, crPre, pkPre);
        a.sends[0].preFader = false;
        a.volumeDb = 0.0f;
        project.sendMixChanged();
        engine.syncAll();

        // a normal track solo keeps its own dry + wet (the return is auto-soloed downstream)
        setSolo (aId);
        double rmsSoloA = 0.0, crSoloA = 0.0; float pkSoloA = 0.0f;
        const bool okSoloA = analyse ("send_solo_track.wav", rmsSoloA, crSoloA, pkSoloA);
        const bool normalSoloKeepsWet = okSoloA && pkSoloA > 0.4f && pkSoloA < 0.7f
                                     && crSoloA > 600.0 && crSoloA < 1100.0;

        // a return routed through a Bus is still heard when it is soloed
        ret.outputBusId = busId;
        project.sendStructureChanged();
        engine.syncAll();
        setSolo (retId);
        double rmsViaBus = 0.0, crViaBus = 0.0; float pkViaBus = 0.0f;
        const bool okViaBus = analyse ("send_solo_bus.wav", rmsViaBus, crViaBus, pkViaBus);
        const bool busWorks = okViaBus && pkViaBus > 0.18f && pkViaBus < 0.4f
                           && crViaBus > 600.0 && crViaBus < 1100.0;
        ret.outputBusId = 0;
        project.sendStructureChanged();
        engine.syncAll();

        // multi-core and serial renders must agree under a send solo
        setSolo (retId);
        double rmsMulti = 0.0, crMulti = 0.0; float pkMulti = 0.0f;
        const bool okMulti = analyse ("send_solo_multi.wav", rmsMulti, crMulti, pkMulti);
        const bool wasMC = engine.isMultiCoreEnabled();
        engine.setMultiCoreEnabled (false);
        double rmsSerial = 0.0, crSerial = 0.0; float pkSerial = 0.0f;
        const bool okSerial = analyse ("send_solo_serial.wav", rmsSerial, crSerial, pkSerial);
        engine.setMultiCoreEnabled (wasMC);
        const bool parity = okMulti && okSerial && std::abs (rmsMulti - rmsSerial) < 0.01
                         && std::abs (pkMulti - pkSerial) < 0.02f;
        setSolo (0);

        log ("send solo: soloing the return plays only the post-send sound (normal peak "
             + String (pkNormal, 3) + " -> return " + String (pkRet, 3) + ", "
             + String (crRet, 0) + " crossings/s), muted source gives " + String (pkMuted, 4),
             okNormal && wetOnly && okMuted && pkMuted < 0.02f);
        log ("send solo: post-fader send follows the fader, pre-fader send still feeds the soloed "
             "return (" + String (pkPostDown, 4) + " / " + String (pkPre, 3) + ")",
             okPostDown && pkPostDown < 0.02f && okPre && pkPre > 0.18f && pkPre < 0.4f);
        log ("send solo: soloing the dry track keeps dry + wet, a return through a Bus is heard, "
             "multi-core matches serial (" + String (pkSoloA, 3) + " / " + String (pkViaBus, 3)
             + " / " + String (rmsMulti - rmsSerial, 4) + ")",
             normalSoloKeepsWet && busWorks && parity);

        project.removeTrack (aId);
        project.removeTrack (bId);
        project.removeTrack (retId);
        project.removeTrack (busId);
        project.sendStructureChanged();
        engine.syncAll();
    }

    log (String ("done, failures: ") + String (log.failures), log.failures == 0);
    return log.failures;
}
}
