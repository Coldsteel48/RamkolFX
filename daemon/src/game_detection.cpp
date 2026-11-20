// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "game_detection.hpp"

#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>

#include <unistd.h>

namespace ramkolfx
{
namespace
{
bool EqualsIgnoreCase(std::string_view A, std::string_view B)
{
    if (A.size() != B.size())
    {
        return false;
    }
    for (size_t i = 0; i < A.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(A[i])) != std::tolower(static_cast<unsigned char>(B[i])))
        {
            return false;
        }
    }
    return true;
}

std::string_view Basename(std::string_view Path)
{
    const size_t Slash = Path.rfind('/');
    return Slash == std::string_view::npos ? Path : Path.substr(Slash + 1);
}

// Basename of the process's executable, or nullopt if it can't be read.
std::optional<std::string> ExeBasename(const std::string& ProcRoot, int64_t Pid)
{
    const std::string Link = ProcRoot + "/" + std::to_string(Pid) + "/exe";
    std::array<char, 4096> Target{};
    const ssize_t Length = readlink(Link.c_str(), Target.data(), Target.size() - 1);
    if (Length <= 0)
    {
        return std::nullopt;
    }
    return std::string(Basename(std::string_view(Target.data(), static_cast<size_t>(Length))));
}

std::optional<std::string> ReadEnviron(const std::string& ProcRoot, int64_t Pid)
{
    std::ifstream File(ProcRoot + "/" + std::to_string(Pid) + "/environ", std::ios::binary);
    if (!File)
    {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
}
} // namespace

bool IsGameMediaRole(std::string_view Role)
{
    return EqualsIgnoreCase(Role, "Game");
}

bool IsWineBinary(std::string_view Binary)
{
    const std::string_view Name = Basename(Binary);
    for (std::string_view Wine : {"wine", "wine64", "wine-preloader", "wine64-preloader"})
    {
        if (Name == Wine)
        {
            return true;
        }
    }
    constexpr std::string_view Exe = ".exe";
    return Name.size() > Exe.size() && EqualsIgnoreCase(Name.substr(Name.size() - Exe.size()), Exe);
}

bool EnvironHasSteamGameId(std::string_view Environ)
{
    while (!Environ.empty())
    {
        const size_t End = Environ.find('\0');
        const std::string_view Entry = Environ.substr(0, End);
        for (std::string_view Key : {"SteamAppId=", "SteamGameId="})
        {
            if (Entry.starts_with(Key))
            {
                const std::string_view Value = Entry.substr(Key.size());
                if (!Value.empty() && Value != "0")
                {
                    return true;
                }
            }
        }
        if (End == std::string_view::npos)
        {
            break;
        }
        Environ.remove_prefix(End + 1);
    }
    return false;
}

bool LooksLikeGame(const GameStreamHints& Hints, const std::string& ProcRoot)
{
    if (IsGameMediaRole(Hints.MediaRole) || IsWineBinary(Hints.Binary))
    {
        return true;
    }
    if (Hints.Pid <= 0)
    {
        return false;
    }
    const std::optional<std::string> Exe = ExeBasename(ProcRoot, Hints.Pid);
    if (!Exe || (!Hints.Binary.empty() && *Exe != Hints.Binary))
    {
        return false; // gone, not ours to read, or not the process the stream named
    }
    if (Hints.Binary.empty() && IsWineBinary(*Exe))
    {
        return true;
    }
    const std::optional<std::string> Environ = ReadEnviron(ProcRoot, Hints.Pid);
    return Environ && EnvironHasSteamGameId(*Environ);
}

} // namespace ramkolfx
