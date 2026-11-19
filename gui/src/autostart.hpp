// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <string>

namespace ramkolfx::gui
{

// "Start at login" via an XDG autostart entry
// ($XDG_CONFIG_HOME/autostart/ramkolfx.desktop), the mechanism every
// freedesktop session (KDE, GNOME, Xfce, COSMIC, ...) honors. The entry
// file itself is the source of truth - deliberately not mirrored in
// gui_prefs.conf - so toggling it from the desktop's own autostart
// settings, or deleting the file by hand, is reflected here too.
//
// The entry launches the GUI with --minimized (see main.cpp): it starts
// hidden in the tray when a tray option is on, else minimized.

// True when the entry exists, isn't disabled in place, and its program
// still exists - so the toggle never claims an entry that would start
// nothing at login.
bool IsAutostartEnabled();

// Writes or removes the entry. Returns false (and logs to stderr) if the
// file couldn't be written/removed. Not fsynced: the entry is as cheap to
// recreate as gui_prefs.conf, and this runs on the render thread.
bool SetAutostartEnabled(bool bEnabled);

// Where the entry lives ($XDG_CONFIG_HOME/autostart/ramkolfx.desktop), for
// pointing the user at it.
std::string AutostartEntryPath();

} // namespace ramkolfx::gui
