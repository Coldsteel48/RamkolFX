// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// Dev helper: writes the installed app icon (packaging/common/ramkolfx.svg)
// from the same logo definition the GUI draws with - see logo.hpp - or,
// with --check, verifies the checked-in copy still matches it (the
// logo_svg_up_to_date test). Not installed.

#include "logo.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

int main(int Argc, char** Argv)
{
    const bool bCheck = Argc == 3 && std::strcmp(Argv[1], "--check") == 0;
    if (Argc != 2 && !bCheck)
    {
        std::fprintf(stderr, "usage: %s OUTPUT.svg\n       %s --check FILE.svg\n", Argv[0], Argv[0]);
        return 2;
    }
    const char* Path = Argv[Argc - 1];
    const std::string Svg = ramkolfx::gui::logo::SvgIcon();

    if (bCheck)
    {
        std::ifstream File(Path, std::ios::binary);
        const std::string OnDisk((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
        if (!File.is_open() || OnDisk != Svg)
        {
            std::fprintf(stderr,
                         "%s is out of date with logo.hpp - regenerate it with\n"
                         "  cmake --build <build dir> --target ramkolfx-update-logo-svg\n",
                         Path);
            return 1;
        }
        return 0;
    }

    std::FILE* File = std::fopen(Path, "wb");
    if (!File)
    {
        std::perror(Path);
        return 1;
    }
    const bool bOk = std::fwrite(Svg.data(), 1, Svg.size(), File) == Svg.size();
    return (std::fclose(File) == 0 && bOk) ? 0 : 1;
}
