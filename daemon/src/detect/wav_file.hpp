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
#include <vector>

namespace ramkolfx
{

// A WAV file averaged down to mono - for the detection bench and the
// classifier parity test, not the daemon.
struct MonoWav
{
    uint32_t SampleRate = 0;
    std::vector<float> Samples;
};

// PCM 16/24/32-bit or 32-bit float, any channel count (plain or
// WAVE_FORMAT_EXTENSIBLE). False with OutError on anything else.
bool ReadWavMono(const std::string& Path, MonoWav& Out, std::string& OutError);

} // namespace ramkolfx
