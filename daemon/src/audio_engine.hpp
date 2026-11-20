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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ramkolfx/protocol.hpp"
#include "ramkolfx/ring_buffer.hpp"
#include "ramkolfx/types.hpp"
#include "dsp/speaker_layout.hpp"
#include "hrtf_catalog.hpp"
#include "detect/content_detector.hpp"
#include "preset_auto_switch.hpp"
#include "preset_store.hpp"
#include "settings_store.hpp"

struct pw_main_loop;
struct pw_loop;
struct spa_source;

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

    // Saves the current state (BuildStatus plus the HRTF display name it
    // maps to via RuntimeHrtfCatalog). Called via FlushSettingsIfDirty()
    // once persisted state has changed - see bSettingsDirty, and
    // PersistedSettings for what's included.
    // Must be called with no other engine mutex held: it takes
    // SettingsSaveMutex first and builds the snapshot inside it.
    void PersistCurrentSettings();

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

    // Raises each 7.1 channel's ChannelPeaks entry to the largest absolute
    // sample in this block. Realtime-safe: only relaxed atomic CAS on 8
    // floats per block, after one plain pass over the buffer.
    void MeterChannelPeaks(const float* Interleaved, uint32 Frames);

    // Reads every ChannelPeaks entry and resets it to 0 (peak-hold since
    // the previous call) - control thread, for GetChannelLevels.
    ChannelLevels TakeChannelLevels();

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
    // saved to whenever persisted state changes - see bSettingsDirty and
    // PersistCurrentSettings().
    SettingsStore Settings;

    SpeakerLayout SharedLayout;
    std::atomic<SpatialMode> Mode{SpatialMode::Off};

    // Spatial mode remembered per output device (see
    // PersistedSettings::DeviceSpatialModes). DeviceModeMutex also covers
    // the Mode store that goes with each update, so a concurrent
    // SetSpatialMode / SetOutputDevice pair can't record one device's
    // mode under the other.
    std::mutex DeviceModeMutex;
    std::map<std::string, SpatialMode> DeviceSpatialModes;

    // Applies Device's remembered spatial mode, or - for a device never
    // seen before - remembers the live mode for it. Returns true if
    // DeviceSpatialModes changed (see ApplyRememberedSpatialMode). Locks
    // DeviceModeMutex.
    bool ApplyDeviceSpatialMode(const std::string& Device);
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

    // Named EQ presets (see docs/eq-presets-plan.md). PresetFile persists
    // only the user presets; PresetCatalog adds the built-ins in front.
    // ActivePresetName/bPresetModified are what Status reports. Lock order
    // when nesting: PresetMutex, then HwEqMutex, then BassEnhancerMutex.
    PresetStore PresetFile;
    std::mutex PresetMutex;
    EqPresetCatalog PresetCatalog;
    std::string ActivePresetName;
    bool bPresetModified = false;

    // See Status::EqRevision. Bumped whenever the curve changes by a route
    // other than a client's own SetHwEqBand/SetBassEnhancer.
    std::atomic<uint32> EqRevision{0};

    // See Status::PresetCatalogRevision. Bumped on every successful preset
    // save/delete.
    std::atomic<uint32> PresetCatalogRevision{0};

    // See Status::DetectedTableRevision. Bumped on BindDetectedClass.
    std::atomic<uint32> DetectedTableRevision{0};

    // Replaces the live HW EQ curve and bass enhancer settings (state +
    // DSP stages) in one go and bumps EqRevision. Caller must not hold
    // HwEqMutex or BassEnhancerMutex.
    void ApplyEqCurve(const std::array<EqBand, MaxEqBands>& Bands, const BassEnhancerSettings& Bass);

    // The live curve (Bands/Bass only, no preset name). Takes HwEqMutex
    // and BassEnhancerMutex, so callers must hold neither.
    EqState CaptureEqCurve();

    // The live curve plus the active preset name/modified flag. Caller
    // holds PresetMutex.
    EqState CaptureEqState();

    // Writes the name/modified half of Live back to the Status-visible
    // fields. Caller holds PresetMutex.
    void SetActivePreset(const EqState& Live);

    // EQ preset auto-switching (docs/eq-presets-plan.md). Lock order when
    // nesting: AutoSwitchMutex, then PresetMutex, then HwEqMutex, then
    // BassEnhancerMutex. AppActivity is guarded by AutoSwitchMutex alone;
    // AutoSwitch is mutated only with both AutoSwitchMutex and PresetMutex
    // held, so BuildStatus can read its Source()/SourceApp() under
    // PresetMutex alone.
    std::mutex AutoSwitchMutex;
    AppActivityTracker AppActivity;
    // Who won and who's waiting in the last resolution, for Status.
    // Guarded by AutoSwitchMutex alone, like AppActivity.
    BindingContest LastContest;
    AutoSwitchController AutoSwitch;
    // Detection state for the Auto-detect binding in effect (if any), fed
    // by Detector's events. Guarded by AutoSwitchMutex alone.
    AutoDetectSession AutoDetect;
    // The timing AutoDetect and Detector were last given (persisted as
    // auto_detect.*). Guarded by AutoSwitchMutex.
    AutoDetectTiming DetectTiming;
    // Who wins when a game and another bound app play at once (persisted
    // as auto_switch.priority). Guarded by AutoSwitchMutex.
    PlaybackPriority Priority = PlaybackPriority::GameFirst;
    // The hold phase last logged (LogHoldPhaseChange). Guarded by
    // AutoSwitchMutex.
    AutoDetectSession::EHold LoggedHoldPhase = AutoDetectSession::EHold::None;

    static constexpr long AutoSwitchTickMs = 250;
    spa_source* AutoSwitchTimer = nullptr;

    // Auto-detect's inference worker (docs/content-detect-plan.md), fed by
    // a tap on the raw 7.1 input in HandleVirtualSinkAudio - before the
    // EQ, so changing the EQ can't change what it hears. Created in Run()
    // before the streams start, never replaced. Inactive (and threadless)
    // until an Auto-detect binding is in effect.
    std::unique_ptr<ContentDetector> Detector;
    // The device policy last set (persisted as auto_detect.device).
    std::atomic<InferenceDevice> AutoDetectDevicePolicy{InferenceDevice::Auto};
    // Main loop only: counts AutoSwitchTimer ticks between re-probes of an
    // unavailable runtime/model (see ContentDetector::RefreshAvailability).
    int TicksSinceAvailabilityProbe = 0;

    // Serializes PersistCurrentSettings(), called via FlushSettingsIfDirty
    // from the main loop (timer, Teardown) and - only if the timer couldn't
    // be created - from control threads. Outermost lock: it's held while the snapshot is built, so
    // a later save never carries older state.
    std::mutex SettingsSaveMutex;

    // Set when persisted state changes (control commands, auto-switch);
    // FlushSettingsIfDirty() turns it into at most one settings.conf write
    // per AutoSwitchTimer tick, so drags that send a command per frame
    // don't each pay for a durable write. Worst case on a crash: the last
    // ~250ms of changes.
    std::atomic<bool> bSettingsDirty{false};
    void FlushSettingsIfDirty();

    // Set once shutdown begins (Stop(), Teardown): commands that would wait
    // on the main loop (SetOutputDevice) are refused from then on - see
    // Teardown.
    std::atomic<bool> bStopping{false};

    // Resolves which binding (if any) applies now - from the live stream
    // list, the output device and the bindings - and, only when that
    // differs from the last target, applies it via AutoSwitch: snapshotting
    // the unbound state on the way in, restoring it on the way back out. Runs on the
    // AutoSwitchTimer tick (main loop) and right after commands that can
    // change the outcome (control threads). Takes AutoSwitchMutex itself,
    // so call it without any of the preset/EQ mutexes held. Returns
    // whether the target changed (and so persisted state may have), which
    // the caller turns into bSettingsDirty.
    bool EvaluateAutoSwitch();
    static void OnAutoSwitchTimer(void* Data, uint64 Expirations);

    // What the rule currently resolves to - for an Auto-detect binding,
    // the preset its detected class maps to (or the pending target) - and
    // switches the detection worker on or off to match. Caller holds
    // AutoSwitchMutex and PresetMutex.
    AutoSwitchTarget ResolveCurrentTarget(int64_t NowMs);

    // Clamps Timing and hands it to AutoDetect and Detector. Takes
    // AutoSwitchMutex. Returns whether it changed (and needs saving).
    bool ApplyAutoDetectTiming(const AutoDetectTiming& Timing);

    // The user changed the EQ by hand: pause detection for the Auto-detect
    // binding in effect, if any (see AutoDetectSession::Hold). Takes
    // AutoSwitchMutex, so call it without the preset/EQ mutexes held.
    void HoldAutoDetect();
    // The same, with AutoSwitchMutex already held.
    void HoldAutoDetectLocked();
    // Logs where a hold on Auto-detect went since the last call: most
    // phase changes leave the target (and so its log line) as it was.
    // AutoSwitchMutex held.
    void LogHoldPhaseChange();

    // BindDetectedClass. Returns whether presets.conf changed.
    bool HandleDetectedClassCommand(const Command& InCommand);
    std::vector<uint8> BuildDetectedClassesResponse();

    // Handles BindDevicePreset/BindAppPreset (empty preset = unbind) and
    // BindDeviceAutoDetect/BindAppAutoDetect.
    // Refusals are logged, like HandlePresetCommand's.
    void HandleBindCommand(const Command& InCommand);

    // Current state as reported to clients and persisted.
    Status BuildStatus();

    // Handles SetHwEqPreset/SaveHwEqPreset/DeleteEqPreset. Returns whether
    // persisted settings changed. Refusals (unknown name, built-in
    // read-only, invalid name) are logged and leave state untouched, same
    // as SetHrtfFile's out-of-range handling - an ErrorResponse would
    // make the GUI drop its connection.
    bool HandlePresetCommand(const Command& InCommand);

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

    // Peak absolute sample per 7.1 channel (VirtualSink frame order, LFE
    // included - SpeakerCount + 1 == VirtualSink::Channels, checked in
    // audio_engine.cpp) of InputScratch as fed to the DSP stage, held until the
    // next TakeChannelLevels(). The audio thread only ever raises a value
    // and the control thread only ever resets it to 0, so a CAS-max on
    // one side and an exchange on the other never lose more than the one
    // block that raced the reset.
    std::array<std::atomic<float>, SpeakerCount + 1> ChannelPeaks{};
};

} // namespace ramkolfx
