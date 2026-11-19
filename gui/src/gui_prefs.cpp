// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "gui_prefs.hpp"

#include <optional>
#include <sstream>
#include <string>

#include "ramkolfx/atomic_file.hpp"
#include "ramkolfx/key_value_file.hpp"
#include "ramkolfx/xdg_paths.hpp"

namespace ramkolfx::gui
{

namespace
{

// $XDG_CONFIG_HOME/ramkolfx, falling back to ~/.config/ramkolfx - same
// directory the daemon's SettingsStore uses, but a distinct filename so
// this file's writes never race with or clobber the daemon's own.
std::string GuiPrefsPath()
{
    return RamkolfxConfigDir() + "/gui_prefs.conf";
}

} // namespace

GuiPreferences LoadGuiPreferences()
{
    GuiPreferences Result;
    const std::optional<KeyValueFile> File = KeyValueFile::Load(GuiPrefsPath());
    if (!File)
    {
        return Result;
    }
    Result.bMirrorModeEnabled = File->GetBool("mirror_left_right", Result.bMirrorModeEnabled);
    Result.bAdvancedEqMode = File->GetBool("advanced_eq_mode", Result.bAdvancedEqMode);
    Result.bAdvancedBassMode = File->GetBool("advanced_bass_mode", Result.bAdvancedBassMode);
    Result.ActiveTabIndex = File->GetLong("active_tab_index", Result.ActiveTabIndex) == 1 ? 1 : 0;
    Result.bMinimizeToTrayOnClose = File->GetBool("minimize_to_tray_on_close", Result.bMinimizeToTrayOnClose);
    Result.bMinimizeToTrayOnMinimize =
        File->GetBool("minimize_to_tray_on_minimize", Result.bMinimizeToTrayOnMinimize);
    return Result;
}

void SaveGuiPreferences(const GuiPreferences& InPreferences)
{
    const std::string Path = GuiPrefsPath();

    std::ostringstream Out;
    Out << "mirror_left_right=" << (InPreferences.bMirrorModeEnabled ? 1 : 0) << "\n";
    Out << "advanced_eq_mode=" << (InPreferences.bAdvancedEqMode ? 1 : 0) << "\n";
    Out << "advanced_bass_mode=" << (InPreferences.bAdvancedBassMode ? 1 : 0) << "\n";
    Out << "active_tab_index=" << InPreferences.ActiveTabIndex << "\n";
    Out << "minimize_to_tray_on_close=" << (InPreferences.bMinimizeToTrayOnClose ? 1 : 0) << "\n";
    Out << "minimize_to_tray_on_minimize=" << (InPreferences.bMinimizeToTrayOnMinimize ? 1 : 0) << "\n";
    // Not durable: this is saved from the render loop on every tab switch
    // and toggle, where an fsync could stall a frame for a long time - and
    // losing the last UI tweak to a power cut is harmless.
    WriteFileAtomically(Path, Out.str(), {.LogPrefix = "[ramkolfx-gui]", .bDurable = false});
}

} // namespace ramkolfx::gui
