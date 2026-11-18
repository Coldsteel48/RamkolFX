// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "settings_store.hpp"

#include <sstream>
#include <utility>

#include <sys/stat.h>

#include "key_value_file.hpp"
#include "preset_store.hpp"
#include "xdg_paths.hpp"

namespace ramkolfx
{

namespace
{

// $XDG_CONFIG_HOME/ramkolfx, falling back to ~/.config/ramkolfx per the
// XDG base directory spec - unlike the control socket (XDG_RUNTIME_DIR,
// typically tmpfs), settings need to survive a reboot.
std::string DefaultSettingsPath()
{
    std::string ConfigDir = XdgConfigHome() + "/ramkolfx";
    mkdir(ConfigDir.c_str(), 0700);
    return ConfigDir + "/settings.conf";
}

std::string SpeakerKey(size_t Index, const char* Field)
{
    return "speaker." + std::to_string(Index) + "." + Field;
}

bool IsValidSpatialMode(long Value)
{
    return Value >= static_cast<long>(SpatialMode::Off) && Value <= static_cast<long>(SpatialMode::Advanced);
}

} // namespace

SettingsStore::SettingsStore() : SettingsPath(DefaultSettingsPath())
{
}

SettingsStore::SettingsStore(std::string Path) : SettingsPath(std::move(Path))
{
}

std::optional<PersistedSettings> SettingsStore::Load() const
{
    const std::optional<KeyValueFile> File = KeyValueFile::Load(SettingsPath);
    if (!File)
    {
        return std::nullopt;
    }

    PersistedSettings Result;
    if (const long ModeValue = File->GetLong("mode", static_cast<long>(Result.Mode)); IsValidSpatialMode(ModeValue))
    {
        Result.Mode = static_cast<SpatialMode>(ModeValue);
    }
    Result.bNearFieldEnabled = File->GetBool("near_field", Result.bNearFieldEnabled);
    Result.OutputDeviceName = File->GetString("output_device", Result.OutputDeviceName);
    Result.ActiveHrtfDisplayName = File->GetString("hrtf_display_name", Result.ActiveHrtfDisplayName);
    for (size_t i = 0; i < SpeakerCount; ++i)
    {
        Result.SpeakerAzimuthDegrees[i] = File->GetFloat(SpeakerKey(i, "azimuth"), Result.SpeakerAzimuthDegrees[i]);
        Result.SpeakerDistanceMeters[i] =
            File->GetFloat(SpeakerKey(i, "distance"), Result.SpeakerDistanceMeters[i]);
        Result.SpeakerMuted[i] = File->GetBool(SpeakerKey(i, "muted"), Result.SpeakerMuted[i]);
    }
    ReadEqCurve(*File, "", Result.HwEqBands, Result.BassEnhancer);
    Result.ActivePresetName = File->GetString("eq_preset.active", Result.ActivePresetName);
    Result.bPresetModified = File->GetBool("eq_preset.modified", Result.bPresetModified);
    return Result;
}

void SettingsStore::Save(const PersistedSettings& InSettings) const
{
    std::ostringstream Out;
    Out << "mode=" << static_cast<int>(InSettings.Mode) << "\n";
    Out << "near_field=" << (InSettings.bNearFieldEnabled ? 1 : 0) << "\n";
    Out << "output_device=" << InSettings.OutputDeviceName << "\n";
    Out << "hrtf_display_name=" << InSettings.ActiveHrtfDisplayName << "\n";
    for (size_t i = 0; i < SpeakerCount; ++i)
    {
        Out << SpeakerKey(i, "azimuth") << "=" << InSettings.SpeakerAzimuthDegrees[i] << "\n";
        Out << SpeakerKey(i, "distance") << "=" << InSettings.SpeakerDistanceMeters[i] << "\n";
        Out << SpeakerKey(i, "muted") << "=" << (InSettings.SpeakerMuted[i] ? 1 : 0) << "\n";
    }
    WriteEqCurve(Out, "", InSettings.HwEqBands, InSettings.BassEnhancer);
    Out << "eq_preset.active=" << InSettings.ActivePresetName << "\n";
    Out << "eq_preset.modified=" << (InSettings.bPresetModified ? 1 : 0) << "\n";
    WriteFileAtomically(SettingsPath, Out.str());
}

} // namespace ramkolfx
