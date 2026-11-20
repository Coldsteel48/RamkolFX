// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// Telling a game's stream from anything else, from metadata and the
// process (docs/content-detect-plan.md, "Game detection"). /proc is faked
// in a temporary directory.

#include "game_detection.hpp"
#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ramkolfx;
namespace fs = std::filesystem;

namespace
{
using namespace std::string_literals;

// A fake /proc: each process is <root>/<pid>/exe (a symlink, which needn't
// resolve) and <root>/<pid>/environ.
class FakeProc
{
public:
    FakeProc()
    {
        std::string Template = (fs::temp_directory_path() / "ramkolfx-proc-XXXXXX").string();
        Root = mkdtemp(Template.data());
    }
    ~FakeProc()
    {
        std::error_code Ignored;
        fs::remove_all(Root, Ignored);
    }

    void Add(int Pid, const std::string& Exe, const std::string& Environ)
    {
        const fs::path Dir = fs::path(Root) / std::to_string(Pid);
        fs::create_directories(Dir);
        fs::create_symlink(Exe, Dir / "exe");
        std::ofstream(Dir / "environ", std::ios::binary) << Environ;
    }

    std::string Root;
};
} // namespace

TEST(MediaRoleGame)
{
    CHECK(IsGameMediaRole("Game"));
    CHECK(IsGameMediaRole("game"));
    CHECK(!IsGameMediaRole("Music"));
    CHECK(!IsGameMediaRole(""));
    CHECK(LooksLikeGame({"GAME", "", 0}, "/nonexistent"));
}

TEST(WineBinaries)
{
    for (const char* Wine : {"wine", "wine64", "wine-preloader", "wine64-preloader", "Game.exe", "HALO.EXE",
                             "/opt/proton/files/bin/wine64"})
    {
        CHECK(IsWineBinary(Wine));
    }
    for (const char* Other : {"firefox", "winecfg-helper", ".exe", "exe", "steam", ""})
    {
        CHECK(!IsWineBinary(Other));
    }
    CHECK(LooksLikeGame({"", "wine64-preloader", 0}, "/nonexistent"));
}

TEST(SteamGameIdInTheEnvironment)
{
    CHECK(EnvironHasSteamGameId("HOME=/home/me\0SteamAppId=570\0"s));
    CHECK(EnvironHasSteamGameId("SteamGameId=12345"s)); // no trailing NUL
    CHECK(!EnvironHasSteamGameId("SteamAppId=0\0SteamGameId=\0"s));
    CHECK(!EnvironHasSteamGameId("XSteamAppId=570\0PATH=/bin\0"s));
    CHECK(!EnvironHasSteamGameId(""));
}

TEST(SteamGameFoundThroughTheProcess)
{
    FakeProc Proc;
    Proc.Add(100, "/home/me/.steam/steamapps/common/Game/game_x64", "SteamAppId=570\0"s);
    Proc.Add(200, "/usr/bin/firefox", "HOME=/home/me\0"s);

    // Named by the node (pipewire-pulse streams)...
    CHECK(LooksLikeGame({"", "game_x64", 100}, Proc.Root));
    // ...or only a pid (native PipeWire streams): the exe names it.
    CHECK(LooksLikeGame({"", "", 100}, Proc.Root));
    CHECK(!LooksLikeGame({"", "firefox", 200}, Proc.Root));
}

TEST(PidFromAnotherNamespaceIsNotTrusted)
{
    // The stream says "game_x64" with pid 200, but our pid 200 is Firefox:
    // a pid from a container's namespace. Its environment isn't read.
    FakeProc Proc;
    Proc.Add(200, "/usr/bin/firefox", "SteamAppId=570\0"s);
    CHECK(!LooksLikeGame({"", "game_x64", 200}, Proc.Root));
}

TEST(NativeWineStreamFoundThroughTheExe)
{
    FakeProc Proc;
    Proc.Add(300, "/usr/bin/wine64-preloader", "PATH=/bin\0"s);
    CHECK(LooksLikeGame({"", "", 300}, Proc.Root));
}

TEST(UnreadableProcessIsNotAGame)
{
    FakeProc Proc;
    CHECK(!LooksLikeGame({"", "game_x64", 4242}, Proc.Root)); // exited, or another user's
    CHECK(!LooksLikeGame({"Music", "firefox", 0}, Proc.Root));
}

RAMKOLFX_TEST_MAIN()
