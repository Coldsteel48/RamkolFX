// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ramkolfx/protocol.hpp"
#include "ramkolfx/ring_buffer.hpp"
#include "ramkolfx/types.hpp"
#include "dsp/speaker_layout.hpp"
#include "hrtf_catalog.hpp"
#include "settings_store.hpp"

struct pw_main_loop;
struct pw_loop;

namespace ramkolfx
{

class VirtualSink;
class HardwareOutput;
class DspStage;
class PassthroughStage;
class AmbisonicsStage;
class HrtfDeck;
class HwEqStage;
class BassEnhancerStage;
class ControlServer;
class DeviceRegistry;
class SingleInstanceLock;

// Owns the whole daemon pipeline: PipeWire main loop, the virtual sink
// (capture), the DSP stages, the hardware output (playback), and the
// control socket. Bridges the capture and playback streams' process
// callbacks through a ring buffer since their block sizes aren't
// guaranteed to match.
//
// One instance of every spatial mode's stage (Off/Basic/Advanced) is kept
// alive at once; HandleVirtualSinkAudio() just dispatches to whichever
// Mode currently selects. All three encode from the same SharedLayout, so
// repositioning a virtual speaker affects whichever mode is active and
// GetStatus always reports one consistent layout regardless of mode.
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Sets everything up and runs the PipeWire main loop until Stop() is
    // called (e.g. from a signal handler) or a fatal error occurs. Returns
    // a process exit code.
    int Run();

    void Stop();

private:
    // Idempotent teardown, safe to call after a partial/failed setup and
    // again from the destructor. Order matters: streams and the control
    // server must be torn down before the main loop, which must be
    // destroyed before pw_deinit().
    void Teardown();

    void HandleVirtualSinkAudio(const float* Interleaved, uint32 Frames);
    uint32 HandleHardwareOutputRequest(float* Interleaved, uint32 Frames);
    std::vector<uint8> HandleControlCommand(const Command& InCommand);

    // Saves CurrentStatus (as returned to the control client) plus the
    // HRTF display name it maps to via RuntimeHrtfCatalog. Called from
    // HandleControlCommand() after any command that changes persisted
    // state - see PersistedSettings for what's included.
    void PersistCurrentSettings(const Status& CurrentStatus);

    // Scales each non-LFE channel in-place by a per-speaker gain: 0.0 for
    // any currently-muted speaker, otherwise an inverse-distance loudness
    // falloff (1.0 at ReferenceSpeakerDistanceMeters, louder when closer,
    // quieter when farther) when near-field mode is on, otherwise 1.0 -
    // one combined pass rather than mute and near-field each snapshotting
    // their own state and looping the buffer separately. Snapshots once per
    // block, same pattern as SpeakerLayout::SnapshotDirections - mute/
    // distance state only changes from control commands, never at audio
    // rate. Applies in every spatial mode (unlike the near-field ILD
    // filter, which is Advanced-only inside BinauralStage) since it's just
    // a gain, no per-ear meaning required - see
    // docs/near-field-distance-plan.md.
    void ApplySpeakerGains(float* Interleaved, uint32 Frames);

    // Fills Interleaved with decorrelated white noise on every non-LFE
    // channel (LFE stays silent), for speaker-calibration purposes:
    // combined with ApplySpeakerGains' mute handling, lets the user isolate
    // one physical speaker at a time regardless of what's actually playing.
    void FillTestNoise(float* Interleaved, uint32 Frames);

    // Returns whichever concrete stage Mode currently selects.
    DspStage& ActiveStage();

    // Picks the output device Run() should start HardwareOutput on: a
    // persisted choice from LoadedSettings if it's non-empty and still
    // present in Devices (populated via a prior WaitForInitialSync() call),
    // otherwise an arbitrary currently-available real sink (first run, or
    // the saved device having disappeared - e.g. unplugged/renamed since
    // last run), otherwise an empty string if Devices has discovered no
    // real sinks at all yet. Doesn't persist the result itself - Run()
    // does that if it differs from what was loaded.
    std::string ResolveInitialOutputDevice(const std::optional<PersistedSettings>& LoadedSettings) const;

    // Resolves the HRTF source BinauralStage should load initially: the
    // RAMKOLFX_HRTF_SOFA environment variable if set (a lower-level
    // override for a SOFA file outside the catalog entirely), else the
    // first entry of RuntimeHrtfCatalog - which RebuildHrtfCatalog orders
    // with imported (user) files ahead of the bundled set, so an imported
    // file wins over the bundled default when both are present. Must run
    // after RebuildHrtfCatalog() has populated RuntimeHrtfCatalog. The
    // GetHrtfCatalog/SetHrtfFile control opcodes are the normal way to
    // switch afterward.
    void ResolveHrtfSofaPath(std::string& OutPath, HrtfSourceKind& OutKind) const;

    // Filters the CMake-generated HrtfCatalog (hrtf_catalog.hpp) down to
    // entries that are actually usable right now: SyntheticSphericalHead
    // always is, SofaFile entries need their file present on disk (a
    // bundled SADIE subject could be missing if data/hrtf/sadie/ wasn't
    // populated). Runs once at startup.
    static std::vector<HrtfCatalogEntry> BuildHrtfCatalog();

    // Resolves the directories the daemon watches for user-supplied SOFA
    // files: always XdgConfigHome()/ramkolfx/hrtf, plus RAMKOLFX_HRTF_DIR
    // if it's set to something different - an extra directory rather than
    // a replacement, so setting it never hides files already sitting in
    // the default location. Created if they don't exist yet. Unlike the
    // bundled catalog, files placed here are never vetted for license or
    // validity - see
    // data/hrtf/README.md.
    static std::vector<std::string> ResolveUserHrtfDirectories();

    // Scans each of DirectoryPaths for *.sofa files (case-insensitive
    // extension), returning every match in one list sorted alphabetically
    // by path. Only checks that each file exists and has the right
    // extension, same as BuildHrtfCatalog's own bundled-file check - actual
    // SOFA parsing happens lazily in HrtfLoader::Open when an entry is
    // selected, and fails gracefully on a corrupt file.
    static std::vector<HrtfCatalogEntry> ScanUserHrtfDirectories(const std::vector<std::string>& DirectoryPaths);

    // Rebuilds RuntimeHrtfCatalog from a fresh
    // ScanUserHrtfDirectories(UserHrtfDirectories) followed by
    // BuildHrtfCatalog() - imported files first, then the bundled set (see
    // ResolveHrtfSofaPath) - under HrtfCatalogMutex. Called once at startup
    // and again from OnHrtfDirectoryChanged
    // whenever a watched directory's contents change. If the currently
    // active entry shifted index (or disappeared) since the last build,
    // re-locates it by display name so ActiveHrtfIndex doesn't silently
    // end up pointing at a different entry than what's actually loaded -
    // same reasoning as PersistedSettings::ActiveHrtfDisplayName.
    void RebuildHrtfCatalog();

    // inotify readiness callback shared by every watch registered on
    // UserHrtfDirectories (all on the same fd - see Run()), hooked into the
    // PipeWire main loop via pw_loop_add_io in Run() (the same facility
    // Run() already uses for SIGINT/SIGTERM) rather than polling on a
    // timer, so new/removed files are picked up immediately. Doesn't need
    // to know which directory fired: any change at all triggers a full
    // rescan of all of them. Runs on the PipeWire main loop thread - see
    // HrtfCatalogMutex's doc comment for why that matters.
    static void OnHrtfDirectoryChanged(void* Data, int Fd, uint32 Mask);

    bool bPipeWireInitialized = false;
    pw_main_loop* MainLoop = nullptr;
    pw_loop* Loop = nullptr;

    // Acquired first, before anything PipeWire-related, so a second
    // instance refuses to start rather than creating a duplicate virtual
    // sink. See single_instance_lock.hpp.
    std::unique_ptr<SingleInstanceLock> InstanceLock;

    std::unique_ptr<VirtualSink> Sink;
    std::unique_ptr<HardwareOutput> Output;
    std::unique_ptr<ControlServer> Server;
    std::unique_ptr<DeviceRegistry> Devices;

    // Loaded once at startup (Run()) to seed initial state below, and
    // saved to after any control command that changes persisted state -
    // see PersistCurrentSettings().
    SettingsStore Settings;

    SpeakerLayout SharedLayout;
    std::atomic<SpatialMode> Mode{SpatialMode::Off};
    std::atomic<bool> bTestNoiseEnabled{false};
    std::atomic<bool> bNearFieldEnabled{false};

    // State for FillTestNoise()'s xorshift32 PRNG; advanced once per
    // generated sample. Only ever touched from the realtime audio thread.
    uint32 NoiseState = 0x9E3779B9u;

    std::unique_ptr<PassthroughStage> OffStage;
    std::unique_ptr<AmbisonicsStage> BasicStage;
    std::unique_ptr<HrtfDeck> AdvancedStage;

    // Applied unconditionally after ActiveStage(), on the final downmixed
    // stereo signal - not one of the three switchable modes above. See
    // protocol.hpp's comment near EqFilterType for why this is a single
    // global curve rather than per-speaker-channel.
    std::unique_ptr<HwEqStage> HwEq;

    // Control-thread-side source of truth for "current 10 bands" (HwEq
    // itself only holds derived filter coefficients, not the EqBand values
    // they came from) - mirrors how SharedLayout is the source of truth
    // for speaker state. Guarded by HwEqMutex rather than SharedLayout's
    // own per-field atomics since ControlServer can run HandleControlCommand
    // concurrently from more than one client connection (same reasoning as
    // HrtfCatalogMutex).
    std::mutex HwEqMutex;
    std::array<EqBand, MaxEqBands> HwEqBandState;

    // Headphone bass enhancer - runs after HwEq on the same final
    // downmixed stereo signal (see HandleVirtualSinkAudio). Same
    // control-thread-source-of-truth pattern as HwEqBandState/HwEqMutex.
    std::unique_ptr<BassEnhancerStage> BassEnhancer;
    std::mutex BassEnhancerMutex;
    BassEnhancerSettings BassEnhancerSettingsState;

    // Whatever's currently in UserHrtfDirectories, followed by the bundled
    // catalog (BuildHrtfCatalog()) - see RebuildHrtfCatalog; index into
    // this is what the GetHrtfCatalog/SetHrtfFile/Status wire values mean.
    // No longer fixed for the process
    // lifetime - RebuildHrtfCatalog() replaces it whenever a watched
    // directory changes, from the PipeWire main loop thread
    // (OnHrtfDirectoryChanged), while HandleControlCommand reads/writes it
    // from per-connection control threads (ControlServer::HandleClient) -
    // HrtfCatalogMutex guards every access from either side. Neither side
    // is the realtime audio callback, so a plain mutex (rather than
    // HrtfDeck's lock-free Slot/Publish pattern) is fine here.
    // mutable: locked from ResolveHrtfSofaPath(), which is const.
    mutable std::mutex HrtfCatalogMutex;
    std::vector<HrtfCatalogEntry> RuntimeHrtfCatalog;
    std::atomic<uint8> ActiveHrtfIndex{0};

    // Resolved once in Run() by ResolveUserHrtfDirectories(); re-scanned by
    // RebuildHrtfCatalog() on every OnHrtfDirectoryChanged callback.
    std::vector<std::string> UserHrtfDirectories;

    // Holds processed (post-DSP) interleaved stereo samples awaiting
    // playback. Sized generously relative to a typical PipeWire quantum.
    RingBuffer<float> StereoMixBuffer{48000 * 2}; // ~1s of stereo audio at 48kHz

    // Scratch buffer for the DSP stage's stereo output, reused per virtual
    // sink callback to avoid allocating on the realtime thread.
    std::vector<float> DspScratch;

    // Holds the 7.1 signal actually fed to the active DSP stage: either a
    // copy of the captured input or synthesized test noise, with muted
    // speakers zeroed - see FillTestNoise()/ApplySpeakerMute(). Always
    // copied into rather than mutating the captured buffer in place, since
    // that buffer belongs to VirtualSink.
    std::vector<float> InputScratch;
};

} // namespace ramkolfx
