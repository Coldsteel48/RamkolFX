// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "key_value_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <unistd.h>

namespace ramkolfx
{

std::optional<KeyValueFile> KeyValueFile::Load(const std::string& Path)
{
    std::ifstream File(Path);
    if (!File.is_open())
    {
        return std::nullopt;
    }

    KeyValueFile Result;
    std::string Line;
    while (std::getline(File, Line))
    {
        const size_t Eq = Line.find('=');
        if (Eq == std::string::npos)
        {
            continue;
        }
        Result.Fields.emplace(Line.substr(0, Eq), Line.substr(Eq + 1));
    }
    return Result;
}

bool KeyValueFile::Has(const std::string& Key) const
{
    return Fields.find(Key) != Fields.end();
}

std::string KeyValueFile::GetString(const std::string& Key, const std::string& Default) const
{
    const auto It = Fields.find(Key);
    return It != Fields.end() ? It->second : Default;
}

long KeyValueFile::GetLong(const std::string& Key, long Default) const
{
    const auto It = Fields.find(Key);
    if (It == Fields.end())
    {
        return Default;
    }
    try
    {
        return std::stol(It->second);
    }
    catch (...)
    {
        return Default;
    }
}

float KeyValueFile::GetFloat(const std::string& Key, float Default) const
{
    const auto It = Fields.find(Key);
    if (It == Fields.end())
    {
        return Default;
    }
    try
    {
        return std::stof(It->second);
    }
    catch (...)
    {
        return Default;
    }
}

bool KeyValueFile::GetBool(const std::string& Key, bool Default) const
{
    const auto It = Fields.find(Key);
    return It != Fields.end() ? It->second == "1" : Default;
}

size_t KeyValueFile::GetCount(const std::string& Key) const
{
    const long Count = GetLong(Key, 0);
    return Count <= 0 ? 0 : std::min(static_cast<size_t>(Count), Fields.size());
}

namespace
{
// write() until done, retrying on EINTR and short writes.
bool WriteAll(int Fd, const std::string& Contents)
{
    size_t Written = 0;
    while (Written < Contents.size())
    {
        const ssize_t Result = write(Fd, Contents.data() + Written, Contents.size() - Written);
        if (Result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        Written += static_cast<size_t>(Result);
    }
    return true;
}
} // namespace

bool WriteFileAtomically(const std::string& Path, const std::string& Contents)
{
    const std::string TempPath = Path + ".tmp";
    const int Fd = open(TempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (Fd < 0)
    {
        fprintf(stderr, "[ramkolfxd] failed to write %s: %s\n", TempPath.c_str(), strerror(errno));
        return false;
    }
    const bool bWritten = WriteAll(Fd, Contents) && fsync(Fd) == 0;
    const int WriteErrno = errno;
    if (close(Fd) != 0 || !bWritten)
    {
        fprintf(stderr, "[ramkolfxd] failed to write %s: %s\n", TempPath.c_str(),
                strerror(bWritten ? errno : WriteErrno));
        unlink(TempPath.c_str());
        return false;
    }

    if (std::rename(TempPath.c_str(), Path.c_str()) != 0)
    {
        fprintf(stderr, "[ramkolfxd] failed to save %s: %s\n", Path.c_str(), strerror(errno));
        unlink(TempPath.c_str());
        return false;
    }

    // Persist the rename itself. Failure here is harmless (the new file
    // is already in place and its data on disk), so it isn't reported.
    const size_t Slash = Path.find_last_of('/');
    const std::string Directory = Slash == std::string::npos ? "." : (Slash == 0 ? "/" : Path.substr(0, Slash));
    if (const int DirFd = open(Directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); DirFd >= 0)
    {
        fsync(DirFd);
        close(DirFd);
    }
    return true;
}

} // namespace ramkolfx
