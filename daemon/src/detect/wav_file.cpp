// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "detect/wav_file.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

namespace ramkolfx
{

namespace
{
uint32_t U32(const unsigned char* P)
{
    return P[0] | (P[1] << 8) | (P[2] << 16) | (static_cast<uint32_t>(P[3]) << 24);
}
uint16_t U16(const unsigned char* P)
{
    return static_cast<uint16_t>(P[0] | (P[1] << 8));
}
} // namespace

bool ReadWavMono(const std::string& Path, MonoWav& Out, std::string& OutError)
{
    std::ifstream File(Path, std::ios::binary);
    if (!File)
    {
        OutError = "can't open " + Path;
        return false;
    }
    const std::vector<unsigned char> Data((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
    if (Data.size() < 12 || std::memcmp(Data.data(), "RIFF", 4) != 0 || std::memcmp(Data.data() + 8, "WAVE", 4) != 0)
    {
        OutError = Path + ": not a WAV file";
        return false;
    }

    uint16_t Format = 0;
    uint16_t Channels = 0;
    uint16_t Bits = 0;
    const unsigned char* Pcm = nullptr;
    size_t PcmBytes = 0;
    for (size_t Pos = 12; Pos + 8 <= Data.size();)
    {
        const unsigned char* Chunk = Data.data() + Pos;
        const size_t Size = U32(Chunk + 4);
        const size_t Available = std::min(Size, Data.size() - Pos - 8);
        if (std::memcmp(Chunk, "fmt ", 4) == 0 && Available >= 16)
        {
            Format = U16(Chunk + 8);
            Channels = U16(Chunk + 10);
            Out.SampleRate = U32(Chunk + 12);
            Bits = U16(Chunk + 22);
            if (Format == 0xFFFE && Available >= 26)
            {
                Format = U16(Chunk + 32); // SubFormat GUID's first two bytes
            }
        }
        else if (std::memcmp(Chunk, "data", 4) == 0)
        {
            Pcm = Chunk + 8;
            PcmBytes = Available;
        }
        Pos += 8 + Size + (Size & 1);
    }

    const bool bFloat = Format == 3 && Bits == 32;
    const bool bInt = Format == 1 && (Bits == 16 || Bits == 24 || Bits == 32);
    if (!Pcm || Channels == 0 || Out.SampleRate == 0 || (!bFloat && !bInt))
    {
        OutError = Path + ": unsupported WAV (need PCM 16/24/32-bit or float32)";
        return false;
    }

    const size_t BytesPerSample = Bits / 8;
    const size_t Frames = PcmBytes / (BytesPerSample * Channels);
    Out.Samples.assign(Frames, 0.0f);
    for (size_t f = 0; f < Frames; ++f)
    {
        float Sum = 0.0f;
        for (size_t c = 0; c < Channels; ++c)
        {
            const unsigned char* S = Pcm + (f * Channels + c) * BytesPerSample;
            float V = 0.0f;
            if (bFloat)
            {
                std::memcpy(&V, S, 4);
            }
            else if (Bits == 16)
            {
                V = static_cast<float>(static_cast<int16_t>(U16(S))) / 32768.0f;
            }
            else if (Bits == 24)
            {
                const int32_t I = static_cast<int32_t>((S[0] << 8) | (S[1] << 16) | (static_cast<uint32_t>(S[2]) << 24)) >> 8;
                V = static_cast<float>(I) / 8388608.0f;
            }
            else
            {
                V = static_cast<float>(static_cast<int32_t>(U32(S))) / 2147483648.0f;
            }
            Sum += V;
        }
        Out.Samples[f] = Sum / static_cast<float>(Channels);
    }
    return true;
}

} // namespace ramkolfx
