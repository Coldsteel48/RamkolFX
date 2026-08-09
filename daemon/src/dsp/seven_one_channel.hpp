// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include "ramkolfx/protocol.hpp"
#include "ramkolfx/types.hpp"

namespace ramkolfx
{

// Raw 7.1 input channel order, matching the layout VirtualSink captures.
// Single source of truth for every DSP stage (and AudioEngine) that reads
// or writes this interleaved layout - previously redefined independently
// in each of those files, which relied on the copies staying in sync by
// convention rather than the compiler.
enum SevenOneChannel : uint32
{
    FL = 0,
    FR = 1,
    FC = 2,
    LFE = 3,
    RL = 4,
    RR = 5,
    SL = 6,
    SR = 7,
    SevenOneChannelCount = 8,
};

// Maps SpeakerLayout::SpeakerChannel index (0..6, non-LFE order FL,FR,FC,
// RL,RR,SL,SR) to its frame offset within the 8-channel interleaved 7.1
// layout above.
constexpr uint32 SpeakerToFrameIndex[SpeakerCount] = {FL, FR, FC, RL, RR, SL, SR};

} // namespace ramkolfx
