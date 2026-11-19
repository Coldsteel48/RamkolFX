// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <cstdlib>
#include <string>

#include <sys/stat.h>

namespace ramkolfx
{

// $XDG_CONFIG_HOME if set, else ~/.config, else /tmp as a last resort
// (matching DefaultControlSocketPath's own /tmp fallback for
// XDG_RUNTIME_DIR) - per the XDG base directory spec. Callers append their
// own subdirectory (e.g. "/ramkolfx", "/ramkolfx/hrtf") on top.
inline std::string XdgConfigHome()
{
    if (const char* XdgConfigHome = std::getenv("XDG_CONFIG_HOME"); XdgConfigHome && *XdgConfigHome)
    {
        return XdgConfigHome;
    }
    if (const char* Home = std::getenv("HOME"); Home && *Home)
    {
        return std::string(Home) + "/.config";
    }
    return "/tmp";
}

// Creates Path and any missing parents (like `mkdir -p`), each owner-only
// as the XDG base directory spec asks for directories it has to create.
// Existing ones are left as they are. Failures aren't reported here: the
// caller's write into the directory fails and logs instead.
inline void CreateDirectories(const std::string& Path)
{
    for (size_t Slash = Path.find('/', 1); Slash != std::string::npos; Slash = Path.find('/', Slash + 1))
    {
        mkdir(Path.substr(0, Slash).c_str(), 0700);
    }
    mkdir(Path.c_str(), 0700);
}

// $XDG_CONFIG_HOME/ramkolfx - where the daemon (settings.conf,
// presets.conf) and the GUI (gui_prefs.conf) keep their files, each under
// its own filename so neither's writes clobber the other's. Just the path:
// it may not exist (a fresh account may not even have ~/.config yet, or it
// was deleted while running) - WriteFileAtomically creates it, parents
// included, whenever a write finds it missing, so every save recovers
// without any directory syscalls on the common path.
inline std::string RamkolfxConfigDir()
{
    return XdgConfigHome() + "/ramkolfx";
}

} // namespace ramkolfx
