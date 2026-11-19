// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace ramkolfx
{

// Parsed contents of a simple `key=value`-per-line config file - the
// format the daemon's SettingsStore and PresetStore and the GUI's
// gui_prefs.conf all persist in. Deliberately not JSON: nothing here has a
// JSON dependency, and these files are small flat sets of scalar fields.
// Every getter falls back to the caller's default when a key is missing
// or unparseable, so an older version's file (or a partially corrupt one)
// still loads field by field instead of failing as a whole.
class KeyValueFile
{
public:
    // Returns nullopt if Path doesn't exist or couldn't be opened. Lines
    // without '=' are ignored; the key is everything before the first '=',
    // the value everything after it (so values may themselves contain '=').
    static std::optional<KeyValueFile> Load(const std::string& Path);

    bool Has(const std::string& Key) const;
    std::string GetString(const std::string& Key, const std::string& Default) const;
    long GetLong(const std::string& Key, long Default) const;
    float GetFloat(const std::string& Key, float Default) const;
    bool GetBool(const std::string& Key, bool Default) const;

    // A `<prefix>.count` field that sizes a run of indexed entries: clamped
    // to [0, number of lines in the file], since every counted entry takes
    // at least one line - so a corrupt huge count can't spin a load loop.
    size_t GetCount(const std::string& Key) const;

private:
    std::unordered_map<std::string, std::string> Fields;
};

} // namespace ramkolfx
