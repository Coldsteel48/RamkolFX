// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "ramkolfx/atomic_file.hpp"

#include "ramkolfx/xdg_paths.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace ramkolfx
{

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

// The directory Path's file is in: "." for a bare name, "/" for a file
// directly under the root.
std::string ParentDirectory(const std::string& Path)
{
    const size_t Slash = Path.find_last_of('/');
    if (Slash == std::string::npos)
    {
        return ".";
    }
    return Slash == 0 ? "/" : Path.substr(0, Slash);
}
} // namespace

bool WriteFileAtomically(const std::string& Path, const std::string& Contents, const AtomicWriteOptions& Options)
{
    const std::string TempPath = Path + ".tmp";
    const auto OpenTemp = [&TempPath]() { return open(TempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); };
    int Fd = OpenTemp();
    if (Fd < 0 && errno == ENOENT)
    {
        // The directory is missing - never created yet, or deleted while
        // running. Only looked into on this failure, so a normal save
        // costs no extra syscalls.
        CreateDirectories(ParentDirectory(Path));
        Fd = OpenTemp();
    }
    if (Fd < 0)
    {
        fprintf(stderr, "%s failed to write %s: %s\n", Options.LogPrefix, TempPath.c_str(), strerror(errno));
        return false;
    }
    const bool bWritten = WriteAll(Fd, Contents) && (!Options.bDurable || fsync(Fd) == 0);
    const int WriteErrno = errno;
    if (close(Fd) != 0 || !bWritten)
    {
        fprintf(stderr, "%s failed to write %s: %s\n", Options.LogPrefix, TempPath.c_str(),
                strerror(bWritten ? errno : WriteErrno));
        unlink(TempPath.c_str());
        return false;
    }

    if (std::rename(TempPath.c_str(), Path.c_str()) != 0)
    {
        fprintf(stderr, "%s failed to save %s: %s\n", Options.LogPrefix, Path.c_str(), strerror(errno));
        unlink(TempPath.c_str());
        return false;
    }

    if (!Options.bDurable)
    {
        return true;
    }

    // Persist the rename itself. Failure here is harmless (the new file
    // is already in place and its data on disk), so it isn't reported.
    if (const int DirFd = open(ParentDirectory(Path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); DirFd >= 0)
    {
        fsync(DirFd);
        close(DirFd);
    }
    return true;
}

} // namespace ramkolfx
