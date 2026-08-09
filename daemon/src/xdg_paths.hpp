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

} // namespace ramkolfx
