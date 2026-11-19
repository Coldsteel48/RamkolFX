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
#include <ostream>
#include <string>
#include <vector>

#include "ramkolfx/protocol.hpp"

namespace ramkolfx
{

class KeyValueFile;

// A named snapshot of everything the "Equalizer & Bass" tab controls: the
// full HW EQ curve plus the bass enhancer (see docs/eq-presets-plan.md).
struct EqPreset
{
    std::string Name;
    std::array<EqBand, MaxEqBands> Bands;
    BassEnhancerSettings Bass;
    EqPresetCategory Category = EqPresetCategory::User;

    bool IsBuiltIn() const
    {
        return IsBuiltInPresetCategory(Category);
    }
};

// Writes/reads an EQ curve + bass enhancer as `<Prefix>hweq.<i>.<field>`
// and `<Prefix>bass_enhancer.<field>` keys. Shared by SettingsStore (empty
// prefix - the live curve, same keys it has always used) and PresetStore
// (`preset.<n>.` prefix). ReadEqCurve leaves any field whose key is
// missing at whatever OutBands/OutBass already held.
void WriteEqCurve(std::ostream& Out, const std::string& Prefix, const std::array<EqBand, MaxEqBands>& Bands,
                  const BassEnhancerSettings& Bass);
void ReadEqCurve(const KeyValueFile& File, const std::string& Prefix, std::array<EqBand, MaxEqBands>& OutBands,
                 BassEnhancerSettings& OutBass);

// The read-only presets shipped with the daemon, in catalog order (grouped
// by category: General, Music, Games). Built once and shared.
const std::vector<EqPreset>& BuiltInEqPresets();

// The built-in called Name, or nullptr. Built-in names are fixed for a
// given build and no user preset can take one, so whether a name is a
// built-in needs no catalog (or lock) to answer.
const EqPreset* FindBuiltInEqPreset(const std::string& Name);

// Target (output device node.name, or app name) -> bound preset name.
using PresetBindingMap = std::map<std::string, std::string>;

// Everything persisted in presets.conf.
struct PresetFileContents
{
    std::vector<EqPreset> UserPresets;
    PresetBindingMap DeviceBindings;
    PresetBindingMap AppBindings;
};

// In-memory preset list: built-ins first (fixed order), then user presets
// (sorted by name), plus the device/app bindings that point at them. Pure
// logic, no IO or locking - AudioEngine guards it with its own mutex and
// persists ToFileContents() through PresetStore. Invariant: every binding
// names a preset that exists (Remove drops bindings to the removed
// preset; the constructor drops dangling ones).
//
// The constructor also owns migrating to a build that ships new
// built-ins: a user preset whose name a built-in now has (saved before
// that built-in existed) is kept as "<name> (custom)" ("(custom 2)"...
// if taken), and its bindings follow it. Any preset name stored outside
// presets.conf goes through ResolveStoredName, which applies the same
// renames - so no caller handles them itself.
class EqPresetCatalog
{
public:
    enum class Result
    {
        Ok,
        InvalidName,
        BuiltInReadOnly,
        NotFound,
    };

    explicit EqPresetCatalog(PresetFileContents Contents = {});

    const EqPreset* Find(const std::string& Name) const;

    // Resolves a preset name stored outside presets.conf (settings.conf's
    // active preset) to the preset it meant. bStoredAsBuiltIn is whether
    // it named a built-in when stored; nullopt means the file predates
    // recording that (see the .cpp for how that's decided). Returns the
    // name to use now - the renamed one if the constructor renamed that
    // user preset - or empty if it no longer resolves, including when the
    // name now belongs to a built-in but meant a user preset (whose curve
    // is not the built-in's).
    std::string ResolveStoredName(const std::string& Name, std::optional<bool> bStoredAsBuiltIn) const;

    // True if the constructor renamed any user preset, i.e. presets.conf
    // should be saved from ToFileContents() to make the renames stick.
    bool RenamedUserPresets() const
    {
        return !Renamed.empty();
    }

    // Creates or overwrites the user preset called Name (after
    // NormalizePresetName). Refuses built-in names. On Ok, OutStoredName
    // (if given) receives the normalized name actually stored.
    Result Save(const std::string& Name, const std::array<EqBand, MaxEqBands>& Bands,
                const BassEnhancerSettings& Bass, std::string* OutStoredName = nullptr);

    // Removes the user preset called Name, and every binding to it.
    // Refuses built-ins.
    Result Remove(const std::string& Name);

    // Binds Target to PresetName, or unbinds it if PresetName is empty.
    // NotFound if PresetName is non-empty and no such preset exists.
    Result BindDevice(const std::string& DeviceName, const std::string& PresetName);
    Result BindApp(const std::string& AppName, const std::string& PresetName);

    const PresetBindingMap& DeviceBindings() const
    {
        return Devices;
    }
    const PresetBindingMap& AppBindings() const
    {
        return Apps;
    }

    // Binding for Target, or empty if none.
    std::string DeviceBinding(const std::string& DeviceName) const;
    std::string AppBinding(const std::string& AppName) const;

    std::vector<EqPresetInfo> Infos() const;
    PresetFileContents ToFileContents() const;

private:
    Result Bind(PresetBindingMap& Map, const std::string& Target, const std::string& PresetName);

    std::vector<EqPreset> User; // kept sorted by Name
    PresetBindingMap Devices;
    PresetBindingMap Apps;
    std::map<std::string, std::string> Renamed; // by the constructor: old name -> new name
};

// Loads/saves the user (non-built-in) presets and bindings as a key=value file at
// $XDG_CONFIG_HOME/ramkolfx/presets.conf, same format and atomic-write
// behavior as SettingsStore. Not realtime-safe - control threads only.
class PresetStore
{
public:
    PresetStore();
    explicit PresetStore(std::string InPath);

    // Missing file -> empty contents. Presets with an invalid or duplicate
    // name are skipped (logged), not fatal. Loaded as stored otherwise:
    // EqPresetCatalog renames ones a built-in now shadows and drops
    // dangling bindings.
    PresetFileContents Load() const;

    bool Save(const PresetFileContents& Contents) const;

private:
    std::string Path;
};

} // namespace ramkolfx
