// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>

#include "ramkolfx/protocol.hpp"
#include "dsp/graphic_eq_filter.hpp"

namespace ramkolfx
{

// Snapshot of daemon state worth remembering across restarts: spatial
// mode, per-speaker layout (azimuth/distance/mute), the near-field
// toggle, the pinned output device, the active HRTF source, and the
// single global HW EQ curve (see HwEqStage/GraphicEqFilter - not yet a
// keyed multi-preset system, just today's live curve, same as everything
// else here). Deliberately excludes test-noise: that's a one-off
// calibration aid, not something anyone wants blasting on every daemon
// start.
struct PersistedSettings
{
    SpatialMode Mode = SpatialMode::Off;
    std::array<float, SpeakerCount> SpeakerAzimuthDegrees{};
    std::array<float, SpeakerCount> SpeakerDistanceMeters{};
    std::array<bool, SpeakerCount> SpeakerMuted{};
    bool bNearFieldEnabled = false;
    std::string OutputDeviceName;

    // Matched against RuntimeHrtfCatalog entries by display name at load
    // time (see AudioEngine::Run), not by index - the catalog's contents
    // and order can shift between builds (e.g. a SADIE subject file going
    // missing), and a stale index would silently resolve to the wrong
    // entry instead of just failing to match.
    std::string ActiveHrtfDisplayName;

    std::array<EqBand, MaxEqBands> HwEqBands = DefaultHwEqBands();

    // Headphone bass enhancer (see dsp/bass_enhancer_filter.hpp). Defaults
    // to disabled, so upgrading an existing install doesn't suddenly
    // change what playback sounds like.
    BassEnhancerSettings BassEnhancer;

    // Which named EQ preset the curve above came from, and whether it has
    // been tweaked since (see Status::ActivePresetName). The curve itself
    // is always saved in full above, so a preset deleted from
    // presets.conf between runs only loses its name here, never the sound.
    std::string ActivePresetName;
    bool bPresetModified = false;

    // Whether ActivePresetName named a built-in when saved, so a later
    // build that ships a built-in with a user preset's name doesn't pass
    // the user's curve off as it (see EqPresetCatalog::ResolveStoredName).
    // nullopt: read from a file written before this was recorded.
    std::optional<bool> bActivePresetBuiltIn;

    // Spatial mode remembered per output device (keyed by PipeWire node
    // name), so e.g. headphones can stay on Advanced while speakers stay
    // Off. Mode above is still the live mode; this map is what switching
    // devices restores from. A device missing here keeps whatever mode
    // is live when it's first selected.
    std::map<std::string, SpatialMode> DeviceSpatialModes;

    // Where Auto-detect's classifier runs (SetAutoDetectDevice).
    InferenceDevice AutoDetectDevice = InferenceDevice::Auto;
    // How quickly Auto-detect reacts (SetAutoDetectTiming), clamped.
    AutoDetectTiming DetectTiming;
    // Who wins when a game and another bound app play at once
    // (SetPlaybackPriority).
    PlaybackPriority Priority = PlaybackPriority::GameFirst;
};

// The per-device spatial mode rule, on switching to Device: a remembered
// device sets InOutMode to its mode; a device never seen before remembers
// the live InOutMode instead. Returns true if Modes changed. Invalid device
// names (empty, control characters - see IsValidTargetName) are ignored.
bool ApplyRememberedSpatialMode(std::map<std::string, SpatialMode>& Modes, const std::string& Device,
                                SpatialMode& InOutMode);

// Loads/saves PersistedSettings as a small text file under
// $XDG_CONFIG_HOME/ramkolfx (falling back to ~/.config/ramkolfx), so user
// choices made through the control protocol survive a daemon restart.
// Not realtime-safe - only ever called from AudioEngine::Run() (startup
// load) and HandleControlCommand() (save after a mutating command, on a
// control-client worker thread), never the audio thread.
class SettingsStore
{
public:
    SettingsStore();

    // Reads/writes Path instead of the XDG default (used by tests).
    explicit SettingsStore(std::string Path);

    // Returns nullopt if no settings file exists yet, or it couldn't be
    // opened - callers should fall back to built-in defaults in that
    // case. Fields missing from an otherwise-readable file (e.g. an older
    // version's file, or partial corruption) fall back individually to
    // PersistedSettings' own defaults rather than failing the whole load.
    std::optional<PersistedSettings> Load() const;

    // Writes InSettings to disk, replacing whatever was there. Writes to
    // a temp file and renames over the target so a crash or power loss
    // mid-write can't corrupt the settings file. Best-effort: logs to
    // stderr and returns on failure rather than throwing.
    void Save(const PersistedSettings& InSettings) const;

private:
    std::string SettingsPath;
};

} // namespace ramkolfx
