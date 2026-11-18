// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "audio_engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>

#include <sys/inotify.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include "control/control_server.hpp"
#include "device_registry.hpp"
#include "dsp/ambisonics_stage.hpp"
#include "dsp/bass_enhancer_stage.hpp"
#include "dsp/binaural_stage.hpp"
#include "dsp/dsp_stage.hpp"
#include "dsp/hrtf_deck.hpp"
#include "dsp/hw_eq_stage.hpp"
#include "dsp/passthrough_stage.hpp"
#include "dsp/seven_one_channel.hpp"
#include "hardware_output.hpp"
#include "single_instance_lock.hpp"
#include "virtual_sink.hpp"
#include "xdg_paths.hpp"

namespace ramkolfx
{

namespace
{
// Scratch/mix buffer sizing: generous relative to a typical PipeWire
// quantum (usually 1024 frames, occasionally up to a few thousand under
// high-latency settings). Blocks larger than this are clamped rather than
// risking an allocation on the realtime thread. Kept equal to
// DspStage::MaxProcessFrames so every stage's own scratch buffers (e.g.
// BinauralStage's virtual speaker signals) are sized consistently with
// what AudioEngine will ever actually pass to Process().
constexpr uint32 MaxScratchFrames = MaxProcessFrames;

// SevenOneChannel and SpeakerToFrameIndex come from
// dsp/seven_one_channel.hpp - shared with every DSP stage that reads this
// same interleaved 7.1 layout.

// Test noise amplitude: loud enough to clearly identify a speaker, quiet
// enough not to be unpleasant or risk clipping once decoded/summed.
constexpr float TestNoiseGain = 0.25f;

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    Teardown();
}

std::string AudioEngine::ResolveInitialOutputDevice(const std::optional<PersistedSettings>& LoadedSettings) const
{
    if (LoadedSettings && !LoadedSettings->OutputDeviceName.empty())
    {
        if (Devices->HasDevice(LoadedSettings->OutputDeviceName))
        {
            return LoadedSettings->OutputDeviceName;
        }
        fprintf(stderr,
                "[ramkolfxd] saved output device '%s' not found, falling back to auto-selection\n",
                LoadedSettings->OutputDeviceName.c_str());
    }

    if (const std::optional<std::string> Picked = Devices->PickAnyDevice())
    {
        return *Picked;
    }

    // Nothing enumerated yet (e.g. the registry sync raced a slow hardware
    // probe) - fall back to whatever was the system's real default sink
    // before this daemon claimed the virtual sink for itself. Target by
    // name even though it isn't confirmed present in Devices: PipeWire
    // resolves target.object once a matching node appears, same as a
    // persisted device name would. GetPreviousDefaultSinkName() already
    // refuses to hand back our own virtual sink (see its doc comment), so
    // this can't reintroduce the loopback AUTOCONNECT-to-"default" would.
    if (const std::optional<std::string> PreviousDefault = Devices->GetPreviousDefaultSinkName())
    {
        return *PreviousDefault;
    }

    // Last resort: no real hardware sink is known at all, and no usable
    // previous default was captured either, so there is no device name to
    // fall back to. Normally unreachable.
    fprintf(stderr, "[ramkolfxd] no output devices found\n");
    return {};
}

void AudioEngine::ResolveHrtfSofaPath(std::string& OutPath, HrtfSourceKind& OutKind) const
{
    if (const char* Override = std::getenv("RAMKOLFX_HRTF_SOFA"))
    {
        OutPath = Override;
        OutKind = HrtfSourceKind::SofaFile;
        return;
    }

    // RuntimeHrtfCatalog is already filtered down to entries that exist on
    // disk and ordered with imported (user) files ahead of the bundled set
    // (see RebuildHrtfCatalog), so its first entry is always a safe default
    // that prefers whatever the user has imported over the bundled one.
    // Locked like every other RuntimeHrtfCatalog access - see
    // HrtfCatalogMutex's doc comment.
    std::lock_guard<std::mutex> Lock(HrtfCatalogMutex);
    if (!RuntimeHrtfCatalog.empty())
    {
        OutPath = RuntimeHrtfCatalog.front().Path;
        OutKind = RuntimeHrtfCatalog.front().Kind;
    }
}

std::vector<HrtfCatalogEntry> AudioEngine::BuildHrtfCatalog()
{
    std::vector<HrtfCatalogEntry> Result;
    Result.reserve(HrtfCatalogCount);
    for (size_t i = 0; i < HrtfCatalogCount; ++i)
    {
        const HrtfCatalogEntry& Entry = HrtfCatalog[i];
        if (Entry.Kind == HrtfSourceKind::SyntheticSphericalHead || std::filesystem::exists(Entry.Path))
        {
            Result.push_back(Entry);
        }
        else
        {
            fprintf(stderr, "[ramkolfxd] HRTF catalog entry '%s' skipped, file not found: %s\n",
                    Entry.DisplayName.c_str(), Entry.Path.c_str());
        }
    }
    return Result;
}

std::vector<std::string> AudioEngine::ResolveUserHrtfDirectories()
{
    std::vector<std::string> Directories = {XdgConfigHome() + "/ramkolfx/hrtf"};

    // RAMKOLFX_HRTF_DIR adds a second directory rather than replacing the
    // default one, so files dropped into the normal XDG location are never
    // silently stopped from showing up just because this is set. Compared
    // via weakly_canonical rather than a raw string match so a trailing
    // slash, relative path, or symlink pointing at the same physical
    // directory as the default doesn't add it a second time (neither
    // directory need exist yet at this point - weakly_canonical handles
    // that, unlike std::filesystem::equivalent).
    if (const char* Override = std::getenv("RAMKOLFX_HRTF_DIR"); Override && *Override)
    {
        std::error_code Ignored;
        const bool bSameAsDefault = std::filesystem::weakly_canonical(Override, Ignored) ==
                                     std::filesystem::weakly_canonical(Directories[0], Ignored);
        if (!bSameAsDefault)
        {
            Directories.emplace_back(Override);
        }
    }

    std::error_code Ignored;
    for (const std::string& Directory : Directories)
    {
        std::filesystem::create_directories(Directory, Ignored);
    }
    return Directories;
}

std::vector<HrtfCatalogEntry> AudioEngine::ScanUserHrtfDirectories(const std::vector<std::string>& DirectoryPaths)
{
    std::vector<HrtfCatalogEntry> Entries;
    for (const std::string& DirectoryPath : DirectoryPaths)
    {
        std::error_code Ignored;
        for (const std::filesystem::directory_entry& DirEntry :
             std::filesystem::directory_iterator(DirectoryPath, Ignored))
        {
            if (!DirEntry.is_regular_file(Ignored))
            {
                continue;
            }
            std::string Extension = DirEntry.path().extension().string();
            std::transform(Extension.begin(), Extension.end(), Extension.begin(),
                            [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
            if (Extension != ".sofa")
            {
                continue;
            }
            HrtfCatalogEntry Entry;
            Entry.DisplayName = "(user) " + DirEntry.path().stem().string();
            Entry.Kind = HrtfSourceKind::SofaFile;
            Entry.Path = DirEntry.path().string();
            Entries.push_back(std::move(Entry));
        }
    }
    std::sort(Entries.begin(), Entries.end(),
              [](const HrtfCatalogEntry& A, const HrtfCatalogEntry& B) { return A.Path < B.Path; });
    return Entries;
}

void AudioEngine::RebuildHrtfCatalog()
{
    // Imported (user) files first, then the bundled set, so an imported
    // file is preferred whenever something picks the catalog's first entry
    // as a default - see ResolveHrtfSofaPath.
    std::vector<HrtfCatalogEntry> NewCatalog = ScanUserHrtfDirectories(UserHrtfDirectories);
    std::vector<HrtfCatalogEntry> BundledEntries = BuildHrtfCatalog();

    // HrtfIndex/ActiveHrtfIndex are a single byte on the wire (see
    // protocol.hpp) - drop excess user entries (truncating NewCatalog
    // before appending the bundled set below) rather than silently
    // wrapping or corrupting an index elsewhere. The bundled/curated set
    // is never truncated by this.
    constexpr size_t MaxCatalogEntries = 255;
    if (NewCatalog.size() + BundledEntries.size() > MaxCatalogEntries)
    {
        const size_t MaxUserEntries =
            BundledEntries.size() < MaxCatalogEntries ? MaxCatalogEntries - BundledEntries.size() : 0;
        fprintf(stderr,
                "[ramkolfxd] HRTF catalog has %zu entries after scanning %zu user director%s, "
                "truncating user entries to %zu (the wire protocol's HrtfIndex is a single byte)\n",
                NewCatalog.size() + BundledEntries.size(), UserHrtfDirectories.size(),
                UserHrtfDirectories.size() == 1 ? "y" : "ies", MaxUserEntries);
        NewCatalog.resize(MaxUserEntries);
    }

    NewCatalog.insert(NewCatalog.end(), std::make_move_iterator(BundledEntries.begin()),
                       std::make_move_iterator(BundledEntries.end()));

    std::lock_guard<std::mutex> Lock(HrtfCatalogMutex);
    const uint8 OldIndex = ActiveHrtfIndex.load(std::memory_order_relaxed);
    if (OldIndex < RuntimeHrtfCatalog.size())
    {
        const std::string OldDisplayName = RuntimeHrtfCatalog[OldIndex].DisplayName;
        for (size_t i = 0; i < NewCatalog.size(); ++i)
        {
            if (NewCatalog[i].DisplayName == OldDisplayName)
            {
                ActiveHrtfIndex.store(static_cast<uint8>(i), std::memory_order_relaxed);
                break;
            }
        }
    }
    RuntimeHrtfCatalog = std::move(NewCatalog);
}

void AudioEngine::OnHrtfDirectoryChanged(void* Data, int Fd, uint32 Mask)
{
    (void)Mask;

    // Drain every pending event before rebuilding - inotify can coalesce
    // many filesystem changes (e.g. `cp *.sofa dir/`) into one readiness
    // notification, and reading only one event per call would leave the
    // rest queued and immediately re-fire. The event contents aren't
    // needed: any change at all just triggers a full rescan.
    char EventBuffer[4096];
    while (read(Fd, EventBuffer, sizeof(EventBuffer)) > 0)
    {
    }
    static_cast<AudioEngine*>(Data)->RebuildHrtfCatalog();
}

int AudioEngine::Run()
{
    const std::string SocketPath = DefaultControlSocketPath();
    const std::string LockPath = SocketPath.substr(0, SocketPath.find_last_of('/')) + "/daemon.lock";
    InstanceLock = std::make_unique<SingleInstanceLock>(LockPath);
    if (!InstanceLock->TryAcquire())
    {
        fprintf(stderr, "[ramkolfxd] another instance is already running (pid %s), exiting\n",
                InstanceLock->HolderPid().empty() ? "unknown" : InstanceLock->HolderPid().c_str());
        return 1;
    }

    pw_init(nullptr, nullptr);
    bPipeWireInitialized = true;

    MainLoop = pw_main_loop_new(nullptr);
    if (!MainLoop)
    {
        fprintf(stderr, "[ramkolfxd] failed to create PipeWire main loop\n");
        Teardown();
        return 1;
    }
    Loop = pw_main_loop_get_loop(MainLoop);

    // Signal handlers must be registered before any other thread is
    // spawned (e.g. ControlServer's accept thread below): POSIX signal
    // masks are per-thread, so a thread created beforehand inherits SIGINT/
    // SIGTERM unblocked and the kernel can kill the whole process via
    // default disposition, bypassing this handler entirely.
    auto OnSignal = [](void* Data, int SignalNumber)
    {
        fprintf(stderr, "[ramkolfxd] received signal %d, shutting down\n", SignalNumber);
        static_cast<AudioEngine*>(Data)->Stop();
    };
    pw_loop_add_signal(Loop, SIGINT, OnSignal, this);
    pw_loop_add_signal(Loop, SIGTERM, OnSignal, this);

    DspScratch.resize(MaxScratchFrames * HardwareOutput::Channels);
    InputScratch.resize(MaxScratchFrames * VirtualSink::Channels);
    OffStage = std::make_unique<PassthroughStage>();
    BasicStage = std::make_unique<AmbisonicsStage>(SharedLayout);

    // Restores whatever the user last chose via the control protocol (mode,
    // speaker layout, near-field toggle) - see settings_store.hpp. Applied
    // to SharedLayout before AdvancedStage is constructed below, since
    // BinauralStage reads speaker azimuth/distance directly from it at
    // construction time rather than picking them up later.
    const std::optional<PersistedSettings> LoadedSettings = Settings.Load();
    if (LoadedSettings)
    {
        Mode.store(LoadedSettings->Mode, std::memory_order_relaxed);
        bNearFieldEnabled.store(LoadedSettings->bNearFieldEnabled, std::memory_order_relaxed);
        for (uint8 i = 0; i < SpeakerCount; ++i)
        {
            const auto Channel = static_cast<SpeakerLayout::SpeakerChannel>(i);
            SharedLayout.SetSpeakerAzimuth(Channel, LoadedSettings->SpeakerAzimuthDegrees[i]);
            SharedLayout.SetSpeakerDistance(Channel, LoadedSettings->SpeakerDistanceMeters[i]);
            SharedLayout.SetSpeakerMuted(Channel, LoadedSettings->SpeakerMuted[i]);
        }
        fprintf(stderr, "[ramkolfxd] restored settings from previous session\n");
    }

    {
        std::lock_guard<std::mutex> Lock(HwEqMutex);
        HwEqBandState = LoadedSettings ? LoadedSettings->HwEqBands : DefaultHwEqBands();
    }
    HwEq = std::make_unique<HwEqStage>(HwEqBandState, static_cast<float>(HardwareOutput::SampleRate));

    {
        std::lock_guard<std::mutex> Lock(BassEnhancerMutex);
        BassEnhancerSettingsState = LoadedSettings ? LoadedSettings->BassEnhancer : BassEnhancerSettings{};
    }
    BassEnhancer = std::make_unique<BassEnhancerStage>(BassEnhancerSettingsState,
                                                        static_cast<float>(HardwareOutput::SampleRate));

    {
        std::lock_guard<std::mutex> Lock(PresetMutex);
        PresetCatalog = EqPresetCatalog(PresetFile.Load());
        if (LoadedSettings && PresetCatalog.Find(LoadedSettings->ActivePresetName))
        {
            ActivePresetName = LoadedSettings->ActivePresetName;
            bPresetModified = LoadedSettings->bPresetModified;
        }
        // else: no preset was active, or it was deleted from presets.conf
        // since - the curve itself was restored above either way, it just
        // no longer has a name.
    }

    // Directories the daemon watches for user-supplied SOFA files - unlike
    // the bundled catalog, this isn't rebuilt on a timer: the inotify
    // watches set up below rebuild it the moment a file is added or
    // removed in any of them, so a running daemon never needs restarting
    // to pick up new files (see ScanUserHrtfDirectories' doc comment).
    UserHrtfDirectories = ResolveUserHrtfDirectories();
    RebuildHrtfCatalog();

    const int HrtfDirWatchFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (HrtfDirWatchFd < 0)
    {
        fprintf(stderr, "[ramkolfxd] inotify_init1 failed: %s (user HRTF directories won't auto-refresh)\n",
                strerror(errno));
    }
    else
    {
        // One inotify fd, one watch descriptor per directory - the shared
        // callback below doesn't care which one fired, so a change in
        // either directory triggers the same full rescan.
        bool bAnyWatchSucceeded = false;
        for (const std::string& Directory : UserHrtfDirectories)
        {
            if (inotify_add_watch(HrtfDirWatchFd, Directory.c_str(),
                                   IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE) < 0)
            {
                fprintf(stderr, "[ramkolfxd] failed to watch HRTF directory %s: %s (won't auto-refresh)\n",
                        Directory.c_str(), strerror(errno));
            }
            else
            {
                bAnyWatchSucceeded = true;
            }
        }

        if (bAnyWatchSucceeded)
        {
            // close=true: PipeWire owns closing this fd when the loop (and
            // therefore this source) is torn down in Teardown(), so no
            // matching cleanup is needed there - same as the pw_loop_add_signal
            // sources registered above needing no explicit removal.
            pw_loop_add_io(Loop, HrtfDirWatchFd, SPA_IO_IN, true, &AudioEngine::OnHrtfDirectoryChanged, this);
        }
        else
        {
            close(HrtfDirWatchFd);
        }
    }

    // ResolveHrtfSofaPath() (RAMKOLFX_HRTF_SOFA, else the catalog's first
    // entry - imported files before the bundled set) picks the initial
    // HRTF source unless a persisted choice overrides it below.
    // RAMKOLFX_HRTF_SOFA is a lower-level dev/testing override (see its
    // doc comment) so it always wins over a persisted choice when set.
    std::string InitialSofaPath;
    HrtfSourceKind InitialHrtfKind = HrtfSourceKind::SofaFile;
    ResolveHrtfSofaPath(InitialSofaPath, InitialHrtfKind);

    bool bAppliedPersistedHrtfChoice = false;
    if (!std::getenv("RAMKOLFX_HRTF_SOFA") && LoadedSettings && !LoadedSettings->ActiveHrtfDisplayName.empty())
    {
        for (size_t i = 0; i < RuntimeHrtfCatalog.size(); ++i)
        {
            if (LoadedSettings->ActiveHrtfDisplayName == RuntimeHrtfCatalog[i].DisplayName)
            {
                InitialSofaPath = RuntimeHrtfCatalog[i].Path;
                InitialHrtfKind = RuntimeHrtfCatalog[i].Kind;
                ActiveHrtfIndex.store(static_cast<uint8>(i), std::memory_order_relaxed);
                bAppliedPersistedHrtfChoice = true;
                break;
            }
        }
    }

    // If it happens to match a catalog entry, report that entry as active
    // so the GUI's dropdown starts in sync; otherwise ActiveHrtfIndex just
    // stays at its 0 default, since an env-var override sits outside the
    // catalog entirely (see ResolveHrtfSofaPath's comment).
    if (!bAppliedPersistedHrtfChoice)
    {
        for (size_t i = 0; i < RuntimeHrtfCatalog.size(); ++i)
        {
            if (RuntimeHrtfCatalog[i].Kind == HrtfSourceKind::SofaFile &&
                InitialSofaPath == RuntimeHrtfCatalog[i].Path)
            {
                ActiveHrtfIndex.store(static_cast<uint8>(i), std::memory_order_relaxed);
                break;
            }
        }
    }

    AdvancedStage = std::make_unique<HrtfDeck>(SharedLayout, InitialHrtfKind, InitialSofaPath,
                                                static_cast<float>(HardwareOutput::SampleRate),
                                                bNearFieldEnabled.load(std::memory_order_relaxed));

    Sink = std::make_unique<VirtualSink>(Loop);
    Sink->SetAudioCallback(
        [this](const float* Interleaved, uint32 Frames)
        {
            HandleVirtualSinkAudio(Interleaved, Frames);
        });
    if (!Sink->Start())
    {
        Teardown();
        return 1;
    }

    // Started before Output below (and given a moment to enumerate what's
    // already in the PipeWire graph via WaitForInitialSync) so
    // ResolveInitialOutputDevice can validate a persisted device choice, or
    // auto-pick a live one, instead of blindly trusting a name that might
    // no longer exist.
    Devices = std::make_unique<DeviceRegistry>(Loop);
    if (!Devices->Start())
    {
        Teardown();
        return 1;
    }
    Devices->WaitForInitialSync();

    const std::string InitialOutputDevice = ResolveInitialOutputDevice(LoadedSettings);
    if (!LoadedSettings || LoadedSettings->OutputDeviceName != InitialOutputDevice)
    {
        // First run (nothing persisted yet) or the persisted device is
        // gone - remember whatever we resolved to instead so the next
        // start (and the GUI's picker) reflect it rather than re-running
        // this fallback every time.
        PersistedSettings ToSave = LoadedSettings.value_or(PersistedSettings{});
        ToSave.OutputDeviceName = InitialOutputDevice;
        Settings.Save(ToSave);
    }

    Output = std::make_unique<HardwareOutput>(Loop, InitialOutputDevice);
    Output->SetFillCallback(
        [this](float* Interleaved, uint32 Frames)
        {
            return HandleHardwareOutputRequest(Interleaved, Frames);
        });
    if (!Output->Start())
    {
        Teardown();
        return 1;
    }

    // EQ preset auto-switching (docs/eq-presets-plan.md): stream state
    // arrives as PipeWire events, but the grace period needs a clock, so a
    // short periodic tick is simpler than reacting to every event. The
    // same tick flushes settings saves (see bSettingsDirty). Created
    // before the control server starts, so client threads only ever see
    // its final value.
    AutoSwitchTimer = pw_loop_add_timer(Loop, &AudioEngine::OnAutoSwitchTimer, this);
    if (AutoSwitchTimer)
    {
        timespec Interval{0, AutoSwitchTickMs * 1000000L};
        pw_loop_update_timer(Loop, AutoSwitchTimer, &Interval, &Interval, false);
    }
    else
    {
        fprintf(stderr, "[ramkolfxd] failed to create auto-switch timer: no EQ auto-switching, "
                        "settings saved after each command instead\n");
    }

    Server = std::make_unique<ControlServer>(SocketPath);
    Server->SetCommandHandler(
        [this](const Command& InCommand)
        {
            return HandleControlCommand(InCommand);
        });
    if (!Server->Start())
    {
        Teardown();
        return 1;
    }

    fprintf(stderr, "[ramkolfxd] running (pass-through mode). Ctrl+C to stop.\n");
    pw_main_loop_run(MainLoop);

    Teardown();
    return 0;
}

void AudioEngine::Stop()
{
    bStopping.store(true); // before the loop stops - see Teardown
    if (MainLoop)
    {
        pw_main_loop_quit(MainLoop);
    }
}

void AudioEngine::Teardown()
{
    // Server first: once Stop() returns, every client thread is joined, so
    // nothing below can race a command - they read AutoSwitchTimer (see
    // HandleControlCommand) and use everything reset further down.
    //
    // Joining can't hang on a command waiting for the (now stopped) main
    // loop - SetOutputDevice's blocking pw_loop_invoke: PipeWire runs any
    // invoke still queued when the loop is left (loop_leave flushes the
    // queues), and one issued after that runs right on the calling thread,
    // since the loop has no owner any more. bStopping keeps new ones from
    // starting at all. Deliberately not "keep iterating the loop until
    // joined": re-entering the loop races exactly that ownerless path for
    // the loop's lock - confirmed live to deadlock (PipeWire 1.6).
    bStopping.store(true);
    if (Server)
    {
        Server->Stop();
        Server.reset();
    }
    if (AutoSwitchTimer && Loop)
    {
        pw_loop_destroy_source(Loop, AutoSwitchTimer);
        AutoSwitchTimer = nullptr;
    }
    // Anything changed since the last timer tick - no command can mark it
    // dirty any more now that the server is stopped. Needs Output (and the
    // stages it reads state from), so before those are reset below.
    if (Output)
    {
        FlushSettingsIfDirty();
    }
    if (Devices)
    {
        Devices->RestorePreviousDefaultSink();
    }
    Devices.reset();
    Output.reset();
    Sink.reset();
    OffStage.reset();
    BasicStage.reset();
    AdvancedStage.reset();
    HwEq.reset();
    BassEnhancer.reset();

    if (MainLoop)
    {
        pw_main_loop_destroy(MainLoop);
        MainLoop = nullptr;
        Loop = nullptr;
    }

    if (bPipeWireInitialized)
    {
        pw_deinit();
        bPipeWireInitialized = false;
    }
}

DspStage& AudioEngine::ActiveStage()
{
    switch (Mode.load(std::memory_order_relaxed))
    {
    case SpatialMode::Basic:
        return *BasicStage;
    case SpatialMode::Advanced:
        return *AdvancedStage;
    case SpatialMode::Off:
    default:
        return *OffStage;
    }
}

void AudioEngine::HandleVirtualSinkAudio(const float* Interleaved, uint32 Frames)
{
    if (Frames > MaxScratchFrames)
    {
        Frames = MaxScratchFrames; // drop overflow rather than overrun the scratch buffer
    }

    if (bTestNoiseEnabled.load(std::memory_order_relaxed))
    {
        FillTestNoise(InputScratch.data(), Frames);
    }
    else
    {
        std::memcpy(InputScratch.data(), Interleaved, sizeof(float) * Frames * VirtualSink::Channels);
    }
    ApplySpeakerGains(InputScratch.data(), Frames);

    ActiveStage().Process(InputScratch.data(), VirtualSink::Channels, DspScratch.data(),
                           HardwareOutput::Channels, Frames);
    HwEq->Process(DspScratch.data(), HardwareOutput::Channels, DspScratch.data(), HardwareOutput::Channels,
                  Frames);
    BassEnhancer->Process(DspScratch.data(), HardwareOutput::Channels, DspScratch.data(),
                           HardwareOutput::Channels, Frames);
    StereoMixBuffer.Push(DspScratch.data(), Frames * HardwareOutput::Channels);
}

void AudioEngine::ApplySpeakerGains(float* Interleaved, uint32 Frames)
{
    // Combines mute (gain 0) and near-field loudness falloff (gain 1 when
    // near-field is off) into one per-speaker gain array and one pass over
    // the interleaved buffer, rather than each effect snapshotting its own
    // state and running its own separate frame loop back to back.
    const bool bNearField = bNearFieldEnabled.load(std::memory_order_relaxed);

    float Gains[SpeakerCount];
    bool bAnyGainNotOne = false;
    for (uint32 Speaker = 0; Speaker < SpeakerCount; ++Speaker)
    {
        const auto Channel = static_cast<SpeakerLayout::SpeakerChannel>(Speaker);
        if (SharedLayout.IsSpeakerMuted(Channel))
        {
            Gains[Speaker] = 0.0f;
        }
        else if (bNearField)
        {
            const float Distance = SharedLayout.GetSpeakerDistance(Channel);
            Gains[Speaker] = ReferenceSpeakerDistanceMeters / std::max(Distance, MinSpeakerDistanceMeters);
        }
        else
        {
            Gains[Speaker] = 1.0f;
        }
        bAnyGainNotOne |= (Gains[Speaker] != 1.0f);
    }
    if (!bAnyGainNotOne)
    {
        return;
    }

    for (uint32 FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        float* Frame = Interleaved + FrameIndex * VirtualSink::Channels;
        for (uint32 Speaker = 0; Speaker < SpeakerCount; ++Speaker)
        {
            Frame[SpeakerToFrameIndex[Speaker]] *= Gains[Speaker];
        }
    }
}

void AudioEngine::FillTestNoise(float* Interleaved, uint32 Frames)
{
    for (uint32 FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        float* Frame = Interleaved + FrameIndex * VirtualSink::Channels;
        for (uint32 Channel = 0; Channel < VirtualSink::Channels; ++Channel)
        {
            if (Channel == LFE)
            {
                Frame[Channel] = 0.0f;
                continue;
            }
            // xorshift32: cheap, allocation-free, good enough decorrelation
            // between channels for identifying a speaker by ear.
            NoiseState ^= NoiseState << 13;
            NoiseState ^= NoiseState >> 17;
            NoiseState ^= NoiseState << 5;
            const float Uniform01 = static_cast<float>(NoiseState >> 8) * (1.0f / 16777216.0f); // 24 bits -> [0,1)
            Frame[Channel] = (Uniform01 * 2.0f - 1.0f) * TestNoiseGain;
        }
    }
}

uint32 AudioEngine::HandleHardwareOutputRequest(float* Interleaved, uint32 Frames)
{
    const size_t Popped = StereoMixBuffer.Pop(Interleaved, Frames * HardwareOutput::Channels);
    return static_cast<uint32>(Popped / HardwareOutput::Channels);
}

std::vector<uint8> AudioEngine::HandleControlCommand(const Command& InCommand)
{
    // Reclaims whatever HrtfDeck's last completed crossfade faded out of.
    // Not realtime-safe (may deallocate) - fine here, this runs on a
    // per-client control thread, never the audio callback. Piggybacked on
    // every command rather than needing a dedicated timer, since the GUI
    // already polls GetStatus at a steady ~4Hz.
    AdvancedStage->CollectGarbage();
    HwEq->CollectGarbage();
    BassEnhancer->CollectGarbage();

    if (InCommand.CommandOpcode == Opcode::GetDevices)
    {
        return EncodeDeviceListResponse(Devices->GetDevices());
    }

    if (InCommand.CommandOpcode == Opcode::GetHwEqState)
    {
        std::lock_guard<std::mutex> Lock(HwEqMutex);
        return EncodeHwEqStateResponse(HwEqBandState);
    }

    if (InCommand.CommandOpcode == Opcode::GetBassEnhancerState)
    {
        std::lock_guard<std::mutex> Lock(BassEnhancerMutex);
        return EncodeBassEnhancerStateResponse(BassEnhancerSettingsState);
    }

    if (InCommand.CommandOpcode == Opcode::GetHrtfCatalog)
    {
        std::vector<std::string> DisplayNames;
        {
            std::lock_guard<std::mutex> Lock(HrtfCatalogMutex);
            DisplayNames.reserve(RuntimeHrtfCatalog.size());
            for (const HrtfCatalogEntry& Entry : RuntimeHrtfCatalog)
            {
                DisplayNames.emplace_back(Entry.DisplayName);
            }
        }
        return EncodeHrtfCatalogResponse(DisplayNames);
    }

    if (InCommand.CommandOpcode == Opcode::GetEqPresetCatalog)
    {
        std::lock_guard<std::mutex> Lock(PresetMutex);
        return EncodeEqPresetCatalogResponse(PresetCatalog.Infos());
    }

    if (InCommand.CommandOpcode == Opcode::GetContentStreams)
    {
        std::vector<ContentStreamInfo> Streams = Devices->GetContentStreams();
        std::lock_guard<std::mutex> Lock(PresetMutex);
        for (ContentStreamInfo& Stream : Streams)
        {
            Stream.BoundPresetName = PresetCatalog.AppBinding(Stream.AppName);
        }
        return EncodeContentStreamListResponse(Streams);
    }

    // Set whenever this command changes state PersistedSettings tracks, so
    // it gets saved to disk below. Left false for read-only commands
    // (GetStatus and the two early-returned Get* above) and for
    // SetTestNoise, which is deliberately not persisted - see
    // PersistedSettings' doc comment.
    bool bPersistedStateChanged = false;

    // Set by commands that can change which binding applies - output
    // device, bindings themselves, or a deleted preset taking its bindings
    // with it - so the auto-switch rule re-runs right away below rather
    // than on the next timer tick, and this response already reflects it.
    bool bReevaluateAutoSwitch = false;

    if (InCommand.CommandOpcode == Opcode::SetSpatialMode)
    {
        Mode.store(InCommand.ModeValue, std::memory_order_relaxed);
        bPersistedStateChanged = true;
        fprintf(stderr, "[ramkolfxd] spatial mode set to %d\n", static_cast<int>(InCommand.ModeValue));
    }
    else if (InCommand.CommandOpcode == Opcode::SetSpeakerAzimuth)
    {
        const auto Channel = static_cast<SpeakerLayout::SpeakerChannel>(InCommand.SpeakerIndex);
        SharedLayout.SetSpeakerAzimuth(Channel, InCommand.AzimuthDegrees);
        bPersistedStateChanged = true;
        // Keeps the near-field path's per-voice filter in sync with live
        // repositioning regardless of whether that mode is currently on,
        // so it's never stale by the time someone switches it on - see
        // BinauralStage::RebuildVoiceForSpeaker's doc comment.
        AdvancedStage->RebuildVoiceForSpeaker(Channel, InCommand.AzimuthDegrees,
                                               SharedLayout.GetSpeakerDistance(Channel));
    }
    else if (InCommand.CommandOpcode == Opcode::SetSpeakerDistance)
    {
        const auto Channel = static_cast<SpeakerLayout::SpeakerChannel>(InCommand.SpeakerIndex);
        SharedLayout.SetSpeakerDistance(Channel, InCommand.DistanceMeters);
        bPersistedStateChanged = true;
        AdvancedStage->RebuildVoiceForSpeaker(Channel, SharedLayout.GetSpeakerAzimuth(Channel),
                                               InCommand.DistanceMeters);
    }
    else if (InCommand.CommandOpcode == Opcode::SetNearFieldEnabled)
    {
        bNearFieldEnabled.store(InCommand.bNearFieldEnabled, std::memory_order_relaxed);
        bPersistedStateChanged = true;
        AdvancedStage->SetNearFieldEnabled(InCommand.bNearFieldEnabled);
    }
    else if (InCommand.CommandOpcode == Opcode::ResetSpeakerPositions)
    {
        SharedLayout.ResetSpeakerPositions();
        bPersistedStateChanged = true;
        for (uint8 i = 0; i < SpeakerCount; ++i)
        {
            const auto Channel = static_cast<SpeakerLayout::SpeakerChannel>(i);
            AdvancedStage->RebuildVoiceForSpeaker(Channel, SharedLayout.GetSpeakerAzimuth(Channel),
                                                   SharedLayout.GetSpeakerDistance(Channel));
        }
    }
    else if (InCommand.CommandOpcode == Opcode::SetOutputDevice)
    {
        // Device names end up in the key=value config files, so refuse
        // ones with control characters (see IsValidTargetName) instead of
        // letting a newline inject keys. Logged rather than an
        // ErrorResponse, which would make the GUI drop its connection.
        if (!IsValidTargetName(InCommand.OutputDeviceName))
        {
            fprintf(stderr, "[ramkolfxd] SetOutputDevice: invalid device name refused\n");
        }
        else if (bStopping.load())
        {
            // Reconnecting needs the main loop, which is stopping - see
            // Teardown for why this matters.
            fprintf(stderr, "[ramkolfxd] SetOutputDevice: ignored, shutting down\n");
        }
        else
        {
            bPersistedStateChanged = true;
            bReevaluateAutoSwitch = true;
            if (!Output->SetTargetNode(InCommand.OutputDeviceName))
            {
                fprintf(stderr, "[ramkolfxd] failed to switch output device to %s\n",
                        InCommand.OutputDeviceName.c_str());
            }
        }
    }
    else if (InCommand.CommandOpcode == Opcode::SetSpeakerMute)
    {
        SharedLayout.SetSpeakerMuted(static_cast<SpeakerLayout::SpeakerChannel>(InCommand.SpeakerIndex),
                                      InCommand.bMuted);
        bPersistedStateChanged = true;
    }
    else if (InCommand.CommandOpcode == Opcode::SetTestNoise)
    {
        bTestNoiseEnabled.store(InCommand.bTestNoiseEnabled, std::memory_order_relaxed);
    }
    else if (InCommand.CommandOpcode == Opcode::SetHrtfFile)
    {
        // DecodeCommand only checked the payload shape, not that the index
        // is actually in range for this daemon's catalog - bounds-check
        // here before indexing. Copy the selected entry out while holding
        // the lock, then release it before calling SwitchTo() below: SOFA
        // parsing there can take a noticeable moment (see HrtfDeck's own
        // doc comment), and there's no reason to block a concurrent
        // OnHrtfDirectoryChanged rebuild for that long.
        bool bIndexValid = false;
        HrtfCatalogEntry SelectedEntry;
        size_t CatalogSize = 0;
        {
            std::lock_guard<std::mutex> Lock(HrtfCatalogMutex);
            CatalogSize = RuntimeHrtfCatalog.size();
            bIndexValid = InCommand.HrtfIndex < CatalogSize;
            if (bIndexValid)
            {
                SelectedEntry = RuntimeHrtfCatalog[InCommand.HrtfIndex];
            }
        }
        if (bIndexValid)
        {
            AdvancedStage->SwitchTo(SelectedEntry.Kind, SelectedEntry.Path);
            ActiveHrtfIndex.store(InCommand.HrtfIndex, std::memory_order_relaxed);
            bPersistedStateChanged = true;
            fprintf(stderr, "[ramkolfxd] HRTF source switched to '%s'\n", SelectedEntry.DisplayName.c_str());
        }
        else
        {
            fprintf(stderr, "[ramkolfxd] SetHrtfFile: index %u out of range (catalog has %zu entries)\n",
                    InCommand.HrtfIndex, CatalogSize);
        }
    }
    else if (InCommand.CommandOpcode == Opcode::SetHwEqBand)
    {
        // Held for the whole update: ApplyEqCurve (preset apply and
        // auto-switch, always under PresetMutex) replaces every band, so
        // an edit interleaved with it could publish a torn curve to the
        // DSP, or flag a freshly applied preset as modified by an edit it
        // overwrote. Serialized, the later of the two simply wins.
        std::lock_guard<std::mutex> PresetLock(PresetMutex);
        std::array<EqBand, MaxEqBands> Bands;
        {
            std::lock_guard<std::mutex> Lock(HwEqMutex);
            HwEqBandState[InCommand.BandIndex] = InCommand.Band;
            Bands = HwEqBandState;
        }
        HwEq->SetBands(Bands);
        bPresetModified = !ActivePresetName.empty();
        bPersistedStateChanged = true;
    }
    else if (InCommand.CommandOpcode == Opcode::SetBassEnhancer)
    {
        std::lock_guard<std::mutex> PresetLock(PresetMutex); // see SetHwEqBand
        {
            std::lock_guard<std::mutex> Lock(BassEnhancerMutex);
            BassEnhancerSettingsState = InCommand.BassEnhancer;
        }
        BassEnhancer->SetSettings(InCommand.BassEnhancer);
        bPresetModified = !ActivePresetName.empty();
        bPersistedStateChanged = true;
    }
    else if (InCommand.CommandOpcode == Opcode::SetHwEqPreset ||
             InCommand.CommandOpcode == Opcode::SaveHwEqPreset ||
             InCommand.CommandOpcode == Opcode::DeleteEqPreset)
    {
        bPersistedStateChanged = HandlePresetCommand(InCommand);
        bReevaluateAutoSwitch = InCommand.CommandOpcode == Opcode::DeleteEqPreset;
    }
    else if (InCommand.CommandOpcode == Opcode::BindDevicePreset ||
             InCommand.CommandOpcode == Opcode::BindAppPreset)
    {
        HandleBindCommand(InCommand);
        bReevaluateAutoSwitch = true;
    }

    if (bReevaluateAutoSwitch)
    {
        // One save below covers both this command's own changes and
        // whatever auto-switch just applied.
        bPersistedStateChanged |= EvaluateAutoSwitch();
    }

    if (bPersistedStateChanged)
    {
        // Saved by the next timer tick rather than here: slider and dial
        // drags send a command per frame, and a durable (fsync'd) write per
        // command would stall this client's reply on a slow disk.
        bSettingsDirty.store(true);
        if (!AutoSwitchTimer)
        {
            FlushSettingsIfDirty(); // no tick to do it (see Run())
        }
    }
    return EncodeStatusResponse(BuildStatus());
}

Status AudioEngine::BuildStatus()
{
    Status OutStatus;
    OutStatus.Mode = Mode.load(std::memory_order_relaxed);
    OutStatus.OutputDeviceName = Output->GetTargetNodeName();
    OutStatus.bTestNoiseEnabled = bTestNoiseEnabled.load(std::memory_order_relaxed);
    OutStatus.ActiveHrtfIndex = ActiveHrtfIndex.load(std::memory_order_relaxed);
    OutStatus.bNearFieldEnabled = bNearFieldEnabled.load(std::memory_order_relaxed);
    OutStatus.EqRevision = EqRevision.load(std::memory_order_relaxed);
    OutStatus.PresetCatalogRevision = PresetCatalogRevision.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> Lock(PresetMutex);
        OutStatus.ActivePresetName = ActivePresetName;
        OutStatus.bPresetModified = bPresetModified;
        OutStatus.DevicePresetName = PresetCatalog.DeviceBinding(OutStatus.OutputDeviceName);
        OutStatus.PresetSource = AutoSwitch.Source();
        OutStatus.PresetSourceApp = AutoSwitch.SourceApp();
    }
    for (uint8 i = 0; i < SpeakerCount; ++i)
    {
        OutStatus.SpeakerAzimuthDegrees[i] =
            SharedLayout.GetSpeakerAzimuth(static_cast<SpeakerLayout::SpeakerChannel>(i));
        OutStatus.SpeakerMuted[i] =
            SharedLayout.IsSpeakerMuted(static_cast<SpeakerLayout::SpeakerChannel>(i));
        OutStatus.SpeakerDistanceMeters[i] =
            SharedLayout.GetSpeakerDistance(static_cast<SpeakerLayout::SpeakerChannel>(i));
    }
    return OutStatus;
}

void AudioEngine::PersistCurrentSettings()
{
    // Taken before the snapshot is built, not just around the write: a
    // thread that saves later then always snapshots later, so a slow
    // earlier save can't land on top of a newer one with stale state. No
    // other lock is held by any caller, and everything BuildStatus and
    // the snapshot take nests inside this one.
    std::lock_guard<std::mutex> SaveLock(SettingsSaveMutex);
    const Status CurrentStatus = BuildStatus();
    PersistedSettings ToSave;
    ToSave.Mode = CurrentStatus.Mode;
    ToSave.SpeakerAzimuthDegrees = CurrentStatus.SpeakerAzimuthDegrees;
    ToSave.SpeakerDistanceMeters = CurrentStatus.SpeakerDistanceMeters;
    ToSave.SpeakerMuted = CurrentStatus.SpeakerMuted;
    ToSave.bNearFieldEnabled = CurrentStatus.bNearFieldEnabled;
    ToSave.OutputDeviceName = CurrentStatus.OutputDeviceName;
    ToSave.ActivePresetName = CurrentStatus.ActivePresetName;
    ToSave.bPresetModified = CurrentStatus.bPresetModified;
    {
        EqState Curve = CaptureEqCurve();
        ToSave.HwEqBands = Curve.Bands;
        ToSave.BassEnhancer = Curve.Bass;
    }
    {
        std::lock_guard<std::mutex> Lock(HrtfCatalogMutex);
        if (CurrentStatus.ActiveHrtfIndex < RuntimeHrtfCatalog.size())
        {
            ToSave.ActiveHrtfDisplayName = RuntimeHrtfCatalog[CurrentStatus.ActiveHrtfIndex].DisplayName;
        }
    }
    Settings.Save(ToSave);
}

EqState AudioEngine::CaptureEqCurve()
{
    EqState Curve;
    {
        std::lock_guard<std::mutex> Lock(HwEqMutex);
        Curve.Bands = HwEqBandState;
    }
    {
        std::lock_guard<std::mutex> Lock(BassEnhancerMutex);
        Curve.Bass = BassEnhancerSettingsState;
    }
    return Curve;
}

EqState AudioEngine::CaptureEqState()
{
    EqState Live = CaptureEqCurve();
    Live.PresetName = ActivePresetName;
    Live.bModified = bPresetModified;
    return Live;
}

void AudioEngine::SetActivePreset(const EqState& Live)
{
    ActivePresetName = Live.PresetName;
    bPresetModified = Live.bModified;
}

void AudioEngine::ApplyEqCurve(const std::array<EqBand, MaxEqBands>& Bands, const BassEnhancerSettings& Bass)
{
    {
        std::lock_guard<std::mutex> Lock(HwEqMutex);
        HwEqBandState = Bands;
    }
    HwEq->SetBands(Bands);
    {
        std::lock_guard<std::mutex> Lock(BassEnhancerMutex);
        BassEnhancerSettingsState = Bass;
    }
    BassEnhancer->SetSettings(Bass);
    EqRevision.fetch_add(1, std::memory_order_relaxed);
}

namespace
{
const char* PresetResultText(EqPresetCatalog::Result Result)
{
    switch (Result)
    {
    case EqPresetCatalog::Result::Ok:
        return "ok";
    case EqPresetCatalog::Result::InvalidName:
        return "invalid name";
    case EqPresetCatalog::Result::BuiltInReadOnly:
        return "built-in presets are read-only";
    case EqPresetCatalog::Result::NotFound:
        return "not found";
    }
    return "?";
}
} // namespace

bool AudioEngine::HandlePresetCommand(const Command& InCommand)
{
    // Lock order: AutoSwitchMutex, then PresetMutex (see audio_engine.hpp).
    std::lock_guard<std::mutex> AutoLock(AutoSwitchMutex);

    if (InCommand.CommandOpcode == Opcode::SetHwEqPreset)
    {
        std::lock_guard<std::mutex> Lock(PresetMutex);
        const EqPreset* Selected = PresetCatalog.Find(InCommand.PresetName);
        if (!Selected)
        {
            fprintf(stderr, "[ramkolfxd] SetHwEqPreset: no preset named '%s'\n", InCommand.PresetName.c_str());
            return false;
        }
        ApplyEqCurve(Selected->Bands, Selected->Bass);
        ActivePresetName = Selected->Name;
        bPresetModified = false;
        AutoSwitch.OnPresetChosenByHand(CaptureEqState());
        fprintf(stderr, "[ramkolfxd] EQ preset '%s' applied\n", Selected->Name.c_str());
        return true;
    }

    std::lock_guard<std::mutex> Lock(PresetMutex);
    // Restored if presets.conf can't be written, so the catalog never
    // claims a preset (or binding) that wouldn't survive a restart.
    const EqPresetCatalog Backup = PresetCatalog;
    const bool bSave = InCommand.CommandOpcode == Opcode::SaveHwEqPreset;
    EqPresetCatalog::Result Result;
    std::string StoredName;
    if (bSave)
    {
        const EqState Curve = CaptureEqCurve();
        Result = PresetCatalog.Save(InCommand.PresetName, Curve.Bands, Curve.Bass, &StoredName);
    }
    else // DeleteEqPreset
    {
        Result = PresetCatalog.Remove(InCommand.PresetName);
    }

    const char* Verb = bSave ? "save" : "delete";
    if (Result != EqPresetCatalog::Result::Ok)
    {
        fprintf(stderr, "[ramkolfxd] EQ preset %s '%s' refused: %s\n", Verb, InCommand.PresetName.c_str(),
                PresetResultText(Result));
        return false;
    }
    if (!PresetFile.Save(PresetCatalog.ToFileContents()))
    {
        PresetCatalog = Backup;
        fprintf(stderr, "[ramkolfxd] EQ preset %s '%s' failed: could not write presets.conf\n", Verb,
                InCommand.PresetName.c_str());
        return false;
    }

    PresetCatalogRevision.fetch_add(1, std::memory_order_relaxed);
    if (bSave)
    {
        ActivePresetName = StoredName;
        bPresetModified = false;
        AutoSwitch.OnPresetChosenByHand(CaptureEqState());
    }
    else if (ActivePresetName == InCommand.PresetName)
    {
        // The curve stays as it is - it just no longer has a name.
        ActivePresetName.clear();
        bPresetModified = false;
        AutoSwitch.TakeManualControl(CaptureEqState());
    }
    fprintf(stderr, "[ramkolfxd] EQ preset %s '%s'\n", Verb,
            StoredName.empty() ? InCommand.PresetName.c_str() : StoredName.c_str());
    return true;
}

AutoSwitchTarget AudioEngine::ResolveCurrentTarget()
{
    return ResolveAutoSwitchTarget(AppActivity, PresetCatalog.AppBindings(), Output->GetTargetNodeName(),
                                   PresetCatalog.DeviceBindings());
}

void AudioEngine::HandleBindCommand(const Command& InCommand)
{
    const bool bDevice = InCommand.CommandOpcode == Opcode::BindDevicePreset;
    const std::string& Target = bDevice ? InCommand.OutputDeviceName : InCommand.ContentAppName;

    std::lock_guard<std::mutex> AutoLock(AutoSwitchMutex);
    std::lock_guard<std::mutex> Lock(PresetMutex);
    const EqPresetCatalog Backup = PresetCatalog; // see HandlePresetCommand
    const EqPresetCatalog::Result Result = bDevice ? PresetCatalog.BindDevice(Target, InCommand.PresetName)
                                                   : PresetCatalog.BindApp(Target, InCommand.PresetName);
    if (Result != EqPresetCatalog::Result::Ok)
    {
        fprintf(stderr, "[ramkolfxd] bind %s '%s' -> '%s' refused: %s\n", bDevice ? "device" : "app",
                Target.c_str(), InCommand.PresetName.c_str(), PresetResultText(Result));
        return;
    }
    if (!PresetFile.Save(PresetCatalog.ToFileContents()))
    {
        PresetCatalog = Backup;
        fprintf(stderr, "[ramkolfxd] bind %s '%s' failed: could not write presets.conf\n",
                bDevice ? "device" : "app", Target.c_str());
        return;
    }

    if (InCommand.PresetName.empty())
    {
        AutoSwitch.OnBindingRemoved(bDevice ? EqPresetSource::Device : EqPresetSource::App, Target,
                                    ResolveCurrentTarget(), CaptureEqState());
    }

    fprintf(stderr, "[ramkolfxd] %s '%s' %s%s\n", bDevice ? "device" : "app", Target.c_str(),
            InCommand.PresetName.empty() ? "unbound" : "bound to preset ", InCommand.PresetName.c_str());
}

bool AudioEngine::EvaluateAutoSwitch()
{
    if (!Devices || !Output)
    {
        return false;
    }

    // Everything the rule depends on - the stream list, the clock, the
    // output device - is sampled under AutoSwitchMutex, not before it: the
    // timer and control threads both run this, and an input sampled
    // outside the lock could be applied after a newer evaluation's result.
    std::lock_guard<std::mutex> AutoLock(AutoSwitchMutex);

    std::vector<std::string> PlayingApps;
    for (const ContentStreamInfo& Stream : Devices->GetContentStreams())
    {
        if (Stream.bPlaying)
        {
            PlayingApps.push_back(Stream.AppName);
        }
    }
    const int64_t NowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    AppActivity.Update(PlayingApps, NowMs);

    std::lock_guard<std::mutex> Lock(PresetMutex);
    const AutoSwitchTarget Target = ResolveCurrentTarget();
    if (Target == AutoSwitch.LastTarget())
    {
        return false; // the common case on every tick - skip capturing the curve
    }

    EqState Live = CaptureEqState();
    bool bCurveChanged = false;
    AutoSwitch.Evaluate(Target, PresetCatalog, Live, bCurveChanged);
    if (bCurveChanged)
    {
        ApplyEqCurve(Live.Bands, Live.Bass);
    }
    SetActivePreset(Live);

    if (Target.Source == EqPresetSource::Manual)
    {
        fprintf(stderr, "[ramkolfxd] EQ auto-switch: no binding applies, back to manual\n");
    }
    else
    {
        fprintf(stderr, "[ramkolfxd] EQ auto-switch: %s '%s' -> preset '%s'\n",
                Target.Source == EqPresetSource::App ? "app" : "device", Target.Key.c_str(),
                Target.PresetName.c_str());
    }
    return true;
}

void AudioEngine::OnAutoSwitchTimer(void* Data, uint64 Expirations)
{
    (void)Expirations;
    AudioEngine* Self = static_cast<AudioEngine*>(Data);
    if (Self->EvaluateAutoSwitch())
    {
        Self->bSettingsDirty.store(true);
    }
    Self->FlushSettingsIfDirty();
}

void AudioEngine::FlushSettingsIfDirty()
{
    // Cleared before the snapshot is taken, so a change landing while the
    // save runs re-marks it dirty and goes out on the next tick.
    if (bSettingsDirty.exchange(false))
    {
        PersistCurrentSettings(); // no locks held here, see its comment
    }
}

} // namespace ramkolfx
