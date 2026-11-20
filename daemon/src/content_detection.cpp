// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "content_detection.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <numeric>

namespace ramkolfx
{

namespace
{
struct ClassFold
{
    DetectedClass Class;
    std::initializer_list<const char*> Labels; // AudioSet display names
};

// Approximate until phase 0 checks it against real material.
const std::initializer_list<ClassFold> DefaultFold = {
    {DetectedClass::Speech,
     {"Speech", "Narration, monologue", "Conversation", "Male speech, man speaking",
      "Female speech, woman speaking"}},
    {DetectedClass::Movie, {"Explosion", "Gunshot, gunfire", "Vehicle", "Soundtrack music"}},
    {DetectedClass::Music, {"Music"}},
    {DetectedClass::Rock,
     {"Rock music", "Rock and roll", "Punk rock", "Grunge", "Progressive rock", "Psychedelic rock"}},
    {DetectedClass::Metal, {"Heavy metal"}},
    {DetectedClass::Pop, {"Pop music", "Disco"}},
    {DetectedClass::HipHop, {"Hip hop music"}},
    {DetectedClass::RnB, {"Rhythm and blues", "Soul music", "Funk", "Gospel music"}},
    {DetectedClass::Electronic,
     {"Electronic music", "House music", "Techno", "Dubstep", "Drum and bass", "Trance music",
      "Electronic dance music"}},
    {DetectedClass::Jazz, {"Jazz", "Swing music"}},
    {DetectedClass::Classical, {"Classical music", "Opera"}},
    {DetectedClass::Blues, {"Blues"}},
    {DetectedClass::Reggae, {"Reggae", "Ska"}},
    {DetectedClass::Acoustic, {"Folk music", "Country", "Bluegrass", "Acoustic guitar"}},
};

bool IsMusicSide(DetectedClass Class)
{
    return Class == DetectedClass::Music || IsGenreClass(Class);
}
} // namespace

LabelMap LabelMap::Build(const std::vector<std::string>& ModelLabels, std::vector<std::string>* OutMissing)
{
    LabelMap Map;
    for (const ClassFold& Fold : DefaultFold)
    {
        for (const char* Label : Fold.Labels)
        {
            const auto It = std::find(ModelLabels.begin(), ModelLabels.end(), Label);
            if (It == ModelLabels.end())
            {
                if (OutMissing)
                {
                    OutMissing->push_back(Label);
                }
                continue;
            }
            Map.Members[static_cast<size_t>(Fold.Class)].push_back(
                static_cast<size_t>(std::distance(ModelLabels.begin(), It)));
        }
    }
    return Map;
}

ClassScores LabelMap::Fold(std::span<const float> ModelScores) const
{
    ClassScores Out{};
    for (size_t Class = 0; Class < DetectedClassCount; ++Class)
    {
        for (const size_t Index : Members[Class])
        {
            if (Index < ModelScores.size())
            {
                Out[Class] = std::max(Out[Class], ModelScores[Index]);
            }
        }
    }
    return Out;
}

void DetectionSmoother::Reset()
{
    *this = DetectionSmoother(Tuning);
}

DetectedClass DetectionSmoother::Decide() const
{
    if (!bHaveEma)
    {
        return DetectedClass::None;
    }
    const float H = Tuning.Hysteresis;
    const auto Bias = [&](bool bIsCurrent) { return bIsCurrent ? H : 0.0f; };
    const float Speech = ScoreOf(Ema, DetectedClass::Speech);
    const float Music = ScoreOf(Ema, DetectedClass::Music);
    const float Cues = ScoreOf(Ema, DetectedClass::Movie);

    // Anything detected at all? Lowered while holding a class, so a quiet
    // passage doesn't drop it.
    if (std::max(Speech, Music) < Tuning.CoarseThreshold - Bias(CurrentClass != DetectedClass::None))
    {
        return DetectedClass::None;
    }
    if (Cues >= Tuning.MovieThreshold - Bias(CurrentClass == DetectedClass::Movie))
    {
        return DetectedClass::Movie;
    }
    if (Speech + Bias(CurrentClass == DetectedClass::Speech) >= Music + Bias(IsMusicSide(CurrentClass)))
    {
        return DetectedClass::Speech;
    }

    DetectedClass BestGenre = DetectedClass::None;
    float BestScore = 0.0f;
    for (size_t i = static_cast<size_t>(DetectedClass::Rock); i < DetectedClassCount; ++i)
    {
        const DetectedClass Genre = static_cast<DetectedClass>(i);
        const float Score = Ema[i] + Bias(Genre == CurrentClass);
        if (BestGenre == DetectedClass::None || Score > BestScore)
        {
            BestGenre = Genre;
            BestScore = Score;
        }
    }
    // BestScore already carries the current genre's bias, which is the
    // same as lowering its threshold by it.
    return BestScore >= Tuning.GenreThreshold ? BestGenre : DetectedClass::Music;
}

namespace
{
// Whether Candidate challenges Current, or is just a lull: nothing
// confident (quiet passage), or plain Music under a detected genre (a
// genre-ambiguous stretch of the same track).
bool IsRival(DetectedClass Candidate, DetectedClass Current)
{
    if (Candidate == Current || Candidate == DetectedClass::None)
    {
        return false;
    }
    return !(Candidate == DetectedClass::Music && IsGenreClass(Current));
}
} // namespace

void DetectionSmoother::AddTrackBoundary(int64_t NowMs)
{
    bHaveEma = false;
    Candidate = DetectedClass::None;
    CandidateSinceMs = NowMs;
    BoundaryAtMs = NowMs;
}

void DetectionSmoother::AddScores(const ClassScores& Scores, int64_t NowMs)
{
    if (!bHaveEma)
    {
        Ema = Scores;
        bHaveEma = true;
    }
    else
    {
        const double ElapsedMs = static_cast<double>(std::max<int64_t>(0, NowMs - LastSampleMs));
        const float Alpha = static_cast<float>(1.0 - std::exp(-ElapsedMs / static_cast<double>(Tuning.EmaTauMs)));
        for (size_t i = 0; i < Ema.size(); ++i)
        {
            Ema[i] += Alpha * (Scores[i] - Ema[i]);
        }
    }
    LastSampleMs = NowMs;

    const DetectedClass Decided = Decide();
    if (Decided != Candidate)
    {
        Candidate = Decided;
        CandidateSinceMs = NowMs;
    }
    if (!IsRival(Candidate, CurrentClass))
    {
        return;
    }
    const bool bConfirmed = NowMs - CandidateSinceMs >= Tuning.ConfirmMs;
    const bool bDwelled = CurrentClass == DetectedClass::None || NowMs - CurrentSinceMs >= Tuning.MinDwellMs ||
                          InBoundaryWindow(NowMs);
    if (bConfirmed && bDwelled)
    {
        CurrentClass = Candidate;
        CurrentSinceMs = NowMs;
        BoundaryAtMs = -1;
    }
}

float DetectionSmoother::Confidence() const
{
    if (CurrentClass == DetectedClass::None)
    {
        return 0.0f;
    }
    return std::clamp(ScoreOf(Ema, CurrentClass), 0.0f, 1.0f);
}

bool DetectionSmoother::IsSettled(int64_t NowMs) const
{
    return CurrentClass != DetectedClass::None && !IsRival(Candidate, CurrentClass) &&
           NowMs - CurrentSinceMs >= Tuning.MinDwellMs && !InBoundaryWindow(NowMs);
}

void DownmixSevenOneToMono(const float* Interleaved, size_t Frames, float* OutMono)
{
    // Channel order FL FR FC LFE RL RR SL SR (seven_one_channel.hpp).
    constexpr float Centre = std::numbers::sqrt2_v<float> / 2.0f;
    constexpr float Surround = Centre / 2.0f;
    for (size_t i = 0; i < Frames; ++i)
    {
        const float* F = Interleaved + i * 8;
        OutMono[i] = 0.5f * (F[0] + F[1]) + Centre * F[2] + Surround * (F[4] + F[5] + F[6] + F[7]);
    }
}

float Rms(std::span<const float> Samples)
{
    if (Samples.empty())
    {
        return 0.0f;
    }
    double Sum = 0.0;
    for (const float S : Samples)
    {
        Sum += static_cast<double>(S) * S;
    }
    return static_cast<float>(std::sqrt(Sum / static_cast<double>(Samples.size())));
}

SilenceTracker::Result SilenceTracker::AddChunk(float ChunkRms, int64_t DurationMs)
{
    Result Out;
    Out.bSilent = ChunkRms < Tuning.SilenceRms;
    if (Out.bSilent)
    {
        SilentRunMs += DurationMs;
        AudibleRunMs = 0;
    }
    else
    {
        Out.bBoundary = SilentRunMs >= Tuning.BoundarySilenceMs;
        SilentRunMs = 0;
        AudibleRunMs += DurationMs;
    }
    return Out;
}

void SilenceTracker::Reset()
{
    SilentRunMs = 0;
    AudibleRunMs = 0;
}

void SampleHistory::Append(std::span<const float> Samples)
{
    const size_t Cap = Buffer.size();
    if (Cap == 0)
    {
        return;
    }
    if (Samples.size() > Cap)
    {
        Samples = Samples.last(Cap);
    }
    size_t Done = 0;
    while (Done < Samples.size())
    {
        const size_t Run = std::min(Samples.size() - Done, Cap - Next);
        std::copy_n(Samples.data() + Done, Run, Buffer.data() + Next);
        Done += Run;
        Next = (Next + Run) % Cap;
    }
    Filled = std::min(Cap, Filled + Samples.size());
}

bool SampleHistory::CopyLatest(std::span<float> Out) const
{
    if (Out.size() > Filled)
    {
        return false;
    }
    const size_t Cap = Buffer.size();
    const size_t Start = (Next + Cap - Out.size()) % Cap;
    const size_t FirstRun = std::min(Out.size(), Cap - Start);
    std::copy_n(Buffer.data() + Start, FirstRun, Out.data());
    std::copy_n(Buffer.data(), Out.size() - FirstRun, Out.data() + FirstRun);
    return true;
}

Resampler::Resampler(uint32_t InRate, uint32_t OutRate)
{
    const uint32_t Divisor = std::gcd(std::max(InRate, 1u), std::max(OutRate, 1u));
    Up = std::max(OutRate, 1u) / Divisor;
    Down = std::max(InRate, 1u) / Divisor;

    // Low-pass at 90% of the lower Nyquist, relative to the input's.
    const double Cutoff = 0.9 * std::min(1.0, static_cast<double>(Up) / Down);
    constexpr int Taps = 2 * HalfTaps;
    Kernel.resize(static_cast<size_t>(Up) * Taps);
    for (uint32_t Phase = 0; Phase < Up; ++Phase)
    {
        const double Offset = static_cast<double>(Phase) / Up; // output sits this far past input sample `base`
        float* Row = Kernel.data() + static_cast<size_t>(Phase) * Taps;
        double Sum = 0.0;
        for (int j = 0; j < Taps; ++j)
        {
            const double X = (j - HalfTaps + 1) - Offset; // distance from the output position, in input samples
            const double Arg = std::numbers::pi * Cutoff * X;
            const double Sinc = std::abs(Arg) < 1e-9 ? 1.0 : std::sin(Arg) / Arg;
            const double W = X / HalfTaps; // -1..1 across the kernel
            const double Blackman =
                std::abs(W) >= 1.0
                    ? 0.0
                    : 0.42 + 0.5 * std::cos(std::numbers::pi * W) + 0.08 * std::cos(2.0 * std::numbers::pi * W);
            Row[j] = static_cast<float>(Sinc * Blackman);
            Sum += Row[j];
        }
        for (int j = 0; j < Taps; ++j)
        {
            Row[j] = static_cast<float>(Row[j] / Sum); // unity gain at DC
        }
    }
}

size_t Resampler::InputFor(size_t OutSamples) const
{
    return (OutSamples * Down + Up - 1) / Up;
}

void Resampler::Process(std::span<const float> In, std::span<float> Out) const
{
    if (Up == Down)
    {
        const size_t Count = std::min(In.size(), Out.size());
        std::copy_n(In.data(), Count, Out.data());
        std::fill(Out.begin() + static_cast<ptrdiff_t>(Count), Out.end(), 0.0f);
        return;
    }
    constexpr int Taps = 2 * HalfTaps;
    const auto InSize = static_cast<int64_t>(In.size());
    for (size_t n = 0; n < Out.size(); ++n)
    {
        const uint64_t Position = static_cast<uint64_t>(n) * Down;
        const auto Base = static_cast<int64_t>(Position / Up);
        const float* Row = Kernel.data() + static_cast<size_t>(Position % Up) * Taps;
        const int64_t First = Base - HalfTaps + 1;
        float Acc = 0.0f;
        if (First >= 0 && First + Taps <= InSize)
        {
            const float* X = In.data() + First;
            for (int j = 0; j < Taps; ++j)
            {
                Acc += X[j] * Row[j];
            }
        }
        else
        {
            for (int j = 0; j < Taps; ++j)
            {
                const int64_t i = First + j;
                if (i >= 0 && i < InSize)
                {
                    Acc += In[static_cast<size_t>(i)] * Row[j];
                }
            }
        }
        Out[n] = Acc;
    }
}

void InferenceScheduler::Reset()
{
    *this = InferenceScheduler(Tuning);
}

void InferenceScheduler::OnRun(int64_t DurationUs)
{
    DurationUs = std::max<int64_t>(0, DurationUs);
    History[static_cast<size_t>(Runs) % HistorySize] = DurationUs;
    const float Duration = static_cast<float>(DurationUs);
    AvgUs = Runs == 0 ? Duration : AvgUs + Tuning.DurationEmaAlpha * (Duration - AvgUs);
    ++Runs;

    // Judged on the raw duration, not the average: one slow run (a cache
    // miss, a busy moment) mustn't end detection, a steady trend must.
    const double NeededIntervalMs = static_cast<double>(DurationUs) / 1000.0 / Tuning.MaxDutyCycle;
    OverBudgetRuns = NeededIntervalMs > static_cast<double>(Tuning.MaxIntervalMs) ? OverBudgetRuns + 1 : 0;
    bGaveUp = bGaveUp || OverBudgetRuns >= Tuning.GiveUpAfterRuns;
}

int64_t InferenceScheduler::NextIntervalMs(bool bSettled, bool bOnBattery) const
{
    const int64_t Base = bSettled || bOnBattery ? Tuning.SlowIntervalMs : Tuning.FastIntervalMs;
    const auto Needed = static_cast<int64_t>(std::ceil(AvgUs / 1000.0f / Tuning.MaxDutyCycle));
    return std::min(std::max(Base, Needed), Tuning.MaxIntervalMs);
}

float InferenceScheduler::DutyCycle(bool bSettled, bool bOnBattery) const
{
    return AvgUs / 1000.0f / static_cast<float>(NextIntervalMs(bSettled, bOnBattery));
}

int64_t InferenceScheduler::PercentileUs(float Fraction) const
{
    const size_t Count = static_cast<size_t>(std::min<int64_t>(Runs, HistorySize));
    if (Count == 0)
    {
        return 0;
    }
    std::array<int64_t, HistorySize> Sorted = History;
    const size_t Index = std::min(Count - 1, static_cast<size_t>(std::clamp(Fraction, 0.0f, 1.0f) * Count));
    std::nth_element(Sorted.begin(), Sorted.begin() + static_cast<ptrdiff_t>(Index),
                     Sorted.begin() + static_cast<ptrdiff_t>(Count));
    return Sorted[Index];
}

} // namespace ramkolfx
