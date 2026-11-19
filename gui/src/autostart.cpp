// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "autostart.hpp"

#include "desktop_exec.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ramkolfx/atomic_file.hpp"
#include "ramkolfx/xdg_paths.hpp"

namespace ramkolfx::gui
{

namespace
{

constexpr const char* ExecutableName = "ramkolfx-gui";

std::string AutostartDir()
{
    return XdgConfigHome() + "/autostart";
}

std::string RealPath(const std::string& Path)
{
    char Buffer[PATH_MAX];
    return realpath(Path.c_str(), Buffer) ? std::string(Buffer) : std::string();
}

std::string Trim(const std::string& Text)
{
    constexpr const char* Whitespace = " \t\r";
    const size_t First = Text.find_first_not_of(Whitespace);
    if (First == std::string::npos)
    {
        return std::string();
    }
    return Text.substr(First, Text.find_last_not_of(Whitespace) - First + 1);
}

// Whether Path is a file this process may execute - access(X_OK) alone
// also passes for a searchable directory, which launches nothing.
bool IsExecutableFile(const std::string& Path)
{
    struct stat Info;
    return stat(Path.c_str(), &Info) == 0 && S_ISREG(Info.st_mode) && access(Path.c_str(), X_OK) == 0;
}

// The first executable named Name along $PATH - what a launcher would
// run for a bare program name - or "" if there's none.
std::string FindOnPath(const std::string& Name)
{
    const char* PathEnv = std::getenv("PATH");
    if (!PathEnv)
    {
        return std::string();
    }
    const std::string Path = PathEnv;
    size_t Start = 0;
    while (true)
    {
        const size_t End = Path.find(':', Start);
        const std::string Dir = Path.substr(Start, End == std::string::npos ? std::string::npos : End - Start);
        if (!Dir.empty())
        {
            const std::string Candidate = Dir + "/" + Name;
            if (IsExecutableFile(Candidate))
            {
                return Candidate;
            }
        }
        if (End == std::string::npos)
        {
            return std::string();
        }
        Start = End + 1;
    }
}

// Whether a launcher could actually start Program: an absolute path must
// be executable, a bare name must resolve along $PATH. Entries this GUI
// writes are always absolute (see ExecCommand); a bare name can only come
// from a hand-edited entry, and this process's $PATH is the best guess at
// the login session's there is.
bool IsLaunchable(const std::string& Program)
{
    if (Program.empty())
    {
        return false;
    }
    if (Program.find('/') != std::string::npos)
    {
        return IsExecutableFile(Program);
    }
    return !FindOnPath(Program).empty();
}

// What the entry's Exec= should launch, always as an absolute path quoted
// per the Desktop Entry spec: the session that runs autostart entries at
// login needn't have this process's $PATH (say, ~/.local/bin added only by
// a shell rc), so a bare name could silently start nothing. When $PATH
// resolves to this very binary - the normal installed case - that's the
// path as found on $PATH, not its symlink target, so it keeps working
// across upgrades even where the real path is versioned (e.g. a Nix
// profile's bin/ linking into the store). Otherwise (a dev build run from
// the build tree, `nix run`) this binary's own real path - which can later
// disappear, see IsAutostartEnabled.
std::string ExecCommand()
{
    const std::string Self = RealPath("/proc/self/exe");
    const std::string OnPath = FindOnPath(ExecutableName);
    if (!OnPath.empty() && OnPath[0] == '/' && (Self.empty() || RealPath(OnPath) == Self))
    {
        return QuoteDesktopExecArgument(OnPath);
    }
    if (Self.empty())
    {
        return ExecutableName; // no telling where this binary is - best effort
    }
    return QuoteDesktopExecArgument(Self);
}

} // namespace

std::string AutostartEntryPath()
{
    return AutostartDir() + "/ramkolfx.desktop";
}

bool IsAutostartEnabled()
{
    std::ifstream File(AutostartEntryPath());
    if (!File.is_open())
    {
        return false;
    }
    // Desktop settings panels may disable an entry in place rather than
    // deleting it. Per the spec, whitespace around '=' is insignificant,
    // and only keys in the [Desktop Entry] group count.
    bool bInDesktopEntry = false;
    std::string Exec;
    std::string Line;
    while (std::getline(File, Line))
    {
        Line = Trim(Line);
        if (Line.empty() || Line[0] == '#')
        {
            continue;
        }
        if (Line[0] == '[')
        {
            bInDesktopEntry = Line == "[Desktop Entry]";
            continue;
        }
        const size_t Eq = Line.find('=');
        if (!bInDesktopEntry || Eq == std::string::npos)
        {
            continue;
        }
        const std::string Key = Trim(Line.substr(0, Eq));
        const std::string Value = Trim(Line.substr(Eq + 1));
        if ((Key == "Hidden" && (Value == "true" || Value == "1")) ||
            (Key == "X-GNOME-Autostart-enabled" && (Value == "false" || Value == "0")))
        {
            return false;
        }
        if (Key == "Exec")
        {
            Exec = Value;
        }
    }
    // An entry whose program is gone (a dev build or `nix run` store path
    // that was since deleted or garbage-collected) silently starts nothing
    // at login - report it as off, so turning it back on rewrites it with
    // this binary's current location.
    return IsLaunchable(DesktopExecProgram(Exec));
}

bool SetAutostartEnabled(bool bEnabled)
{
    const std::string Path = AutostartEntryPath();

    if (!bEnabled)
    {
        if (std::remove(Path.c_str()) != 0 && errno != ENOENT)
        {
            fprintf(stderr, "[ramkolfx-gui] failed to remove %s: %s\n", Path.c_str(), std::strerror(errno));
            return false;
        }
        return true;
    }

    const std::string Contents = "[Desktop Entry]\n"
                                 "Type=Application\n"
                                 "Name=RamkolFX\n"
                                 "Comment=Start RamkolFX in the background at login\n"
                                 "Exec=" +
                                 ExecCommand() +
                                 " --minimized\n"
                                 "Terminal=false\n"
                                 "X-GNOME-Autostart-enabled=true\n";
    // No fsync: called from the render thread (the Settings toggle), where
    // one can stall a frame for tens to hundreds of ms on a slow disk - and
    // a lost entry is just re-enabled, same as gui_prefs.conf.
    return WriteFileAtomically(Path, Contents, {.LogPrefix = "[ramkolfx-gui]", .bDurable = false});
}

} // namespace ramkolfx::gui
