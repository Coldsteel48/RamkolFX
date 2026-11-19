// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// The autostart entry's Exec= quoting (gui/src/desktop_exec.hpp): whatever
// path the entry is written with must read back as that same path, or the
// "Start at login" toggle shows off right after being turned on.

#include "desktop_exec.hpp"
#include "test_harness.hpp"

using namespace ramkolfx::gui;

namespace
{

// The program as IsAutostartEnabled reads it back from an entry written
// the way SetAutostartEnabled writes it.
std::string RoundTrip(const std::string& Program)
{
    return DesktopExecProgram(QuoteDesktopExecArgument(Program) + " --minimized");
}

} // namespace

TEST(PlainPathRoundTrips)
{
    CHECK(RoundTrip("/usr/bin/ramkolfx-gui") == "/usr/bin/ramkolfx-gui");
}

TEST(SpecialCharactersRoundTrip)
{
    for (const std::string Path : {
             "/home/me/My Projects/build/gui/ramkolfx-gui",
             "/opt/back\\slash/ramkolfx-gui",
             "/opt/100%/ramkolfx-gui",
             "/opt/%%f/ramkolfx-gui",
             "/opt/$HOME/ramkolfx-gui",
             "/opt/\"quoted\"/ramkolfx-gui",
             "/opt/`tick`/ramkolfx-gui",
             "/opt/new\nline/tab\there/cr\rhere",
             "/opt/trailing-backslash\\",
             "/opt/\\s-not-a-space/x",
             "/opt/ünïcödé/ramkolfx-gui",
         })
    {
        if (RoundTrip(Path) != Path)
        {
            std::printf("  round trip changed: [%s] -> [%s]\n", Path.c_str(), RoundTrip(Path).c_str());
            CHECK(false);
        }
    }
}

TEST(QuotingLayersAsTheSpecAsks)
{
    // One literal backslash: \\ inside the quotes, then each of those
    // doubled again as a string-value escape.
    CHECK(QuoteDesktopExecArgument("/a\\b") == "\"/a\\\\\\\\b\"");
    CHECK(QuoteDesktopExecArgument("/a%b") == "\"/a%%b\"");
    CHECK(QuoteDesktopExecArgument("/a$b") == "\"/a\\\\$b\"");
    CHECK(QuoteDesktopExecArgument("/a b") == "\"/a b\"");
}

TEST(HandWrittenEntriesParse)
{
    CHECK(DesktopExecProgram("ramkolfx-gui --minimized") == "ramkolfx-gui");
    CHECK(DesktopExecProgram("  /usr/bin/ramkolfx-gui") == "/usr/bin/ramkolfx-gui");
    CHECK(DesktopExecProgram("/usr/bin/ramkolfx-gui\\s--minimized") == "/usr/bin/ramkolfx-gui");
    CHECK(DesktopExecProgram("\"/usr/bin/ramkolfx-gui\"") == "/usr/bin/ramkolfx-gui");
    CHECK(DesktopExecProgram("").empty());
}

RAMKOLFX_TEST_MAIN()
