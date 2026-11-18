// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include <cstdio>
#include <cstring>

#include "audio_engine.hpp"
#include "ramkolfx/version.hpp"

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-V") == 0)
        {
            std::printf("ramkolfxd %s\n", ramkolfx::Version);
            return 0;
        }
    }

    ramkolfx::AudioEngine Engine;
    return Engine.Run();
}
