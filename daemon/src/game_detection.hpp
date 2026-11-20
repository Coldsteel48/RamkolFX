// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ramkolfx
{

// Telling a game's playback stream from anything else, from what PipeWire
// says about the stream and its process rather than from the audio
// (docs/content-detect-plan.md, "Game detection"). No PipeWire here:
// DeviceRegistry gathers the hints, this decides.

// What is known about a stream's process.
struct GameStreamHints
{
    std::string MediaRole; // the node's media.role; may be empty
    // The node's application.process.binary; empty if the node has none
    // (native PipeWire streams), in which case the pid's exe is used.
    std::string Binary;
    // The node's application.process.id, else the client's
    // pipewire.sec.pid; 0 if neither is known.
    int64_t Pid = 0;
};

// media.role says so ("Game", any case).
bool IsGameMediaRole(std::string_view Role);

// A Wine process: wine, wine64, wine-preloader, wine64-preloader, or a
// Windows executable (*.exe). Covers Proton, Lutris, Heroic, Bottles.
bool IsWineBinary(std::string_view Binary);

// A process environment (/proc/<pid>/environ: NUL-separated NAME=value)
// with a Steam game id: SteamAppId or SteamGameId, non-empty and not 0.
bool EnvironHasSteamGameId(std::string_view Environ);

// The decision. Reads <ProcRoot>/<pid>/exe and <pid>/environ as needed.
// A client-reported pid may belong to another pid namespace (Flatpak, a
// Steam container), so the environment is only read when the exe's
// basename matches Hints.Binary - a stranger's environment is never read
// in its place. Unreadable files (another user's process, one that has
// exited) count as "no".
bool LooksLikeGame(const GameStreamHints& Hints, const std::string& ProcRoot = "/proc");

} // namespace ramkolfx
