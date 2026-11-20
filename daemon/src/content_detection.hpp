// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ramkolfx/protocol.hpp"

namespace ramkolfx
{

// Pure, clock-injected logic behind Auto-detect bindings
// (docs/content-detect-plan.md): turning a classifier's AudioSet label
// scores into one stable DetectedClass, and pacing inference so it stays
// inside the performance budget. No ONNX Runtime, no threads, no IO - the
// inference worker drives these and the tests drive them directly.

// One score per DetectedClass, indexed by its value ([None] unused). For
// Movie it's the strength of film cues (effects, soundtrack), not a
// probability of "this is a film" on its own; see DetectionTuning.
using ClassScores = std::array<float, DetectedClassCount>;

inline float& ScoreOf(ClassScores& Scores, DetectedClass Class)
{
    return Scores[static_cast<size_t>(Class)];
}
inline float ScoreOf(const ClassScores& Scores, DetectedClass Class)
{
    return Scores[static_cast<size_t>(Class)];
}

// Folds a model's per-label scores (AudioSet's 527 labels, in the model's
// output order) into ClassScores: each class scores the max of its
// member labels. Built from the model's label names rather than fixed
// indices, so a different AudioSet model (or label order) needs no code
// change.
class LabelMap
{
public:
    // The default fold (approximate until phase 0 locks it), matched by
    // AudioSet display name. Names the model doesn't have are skipped and
    // listed in OutMissing, if given.
    static LabelMap Build(const std::vector<std::string>& ModelLabels, std::vector<std::string>* OutMissing = nullptr);

    // ModelScores must have as many entries as the label list Build saw;
    // indices beyond its end are treated as 0.
    ClassScores Fold(std::span<const float> ModelScores) const;

    // Whether any of the model's labels feed Class.
    bool Covers(DetectedClass Class) const
    {
        return !Members[static_cast<size_t>(Class)].empty();
    }

private:
    std::array<std::vector<size_t>, DetectedClassCount> Members;
};

// Thresholds and timings for DetectionSmoother. Defaults are placeholders
// until phase 0 tunes them on real material.
struct DetectionTuning
{
    // Time constant of the per-class score EMA.
    int64_t EmaTauMs = 3000;
    // Speech or Music must reach this for any class to be detected.
    float CoarseThreshold = 0.5f;
    // Film-cue strength that turns Speech/Music into Movie.
    float MovieThreshold = 0.25f;
    // Genre evidence is a track-long average rather than the short EMA:
    // genre is a property of the track, and one window's genre scores
    // are noisy and low (AudioSet's genre labels are sparse; on real
    // music they mostly read 0.02-0.1). Weighted mean with this time
    // constant, restarted at track boundaries.
    int64_t GenreTauMs = 10000;
    // Music always gets a genre: the one with the best average, as long
    // as any genre registers at all (above this floor).
    float GenreFloor = 0.01f;
    // The current class keeps its place until a rival leads by this much
    // (and its thresholds are lowered by it), so scores hovering near a
    // boundary don't flip the class.
    float Hysteresis = 0.1f;
    // The same for the current genre, relative since genre scores are
    // small: a rival genre must beat it by this factor.
    float GenreHysteresis = 1.25f;
    // A new candidate must stay the candidate this long before it's
    // adopted - so the first class lands ~4 s into a spell.
    int64_t ConfirmMs = 4000;
    // Minimum time between two class changes, so a genre-ambiguous song
    // doesn't flip the EQ back and forth. Waived after a track boundary.
    int64_t MinDwellMs = 10000;
    // How long after a track boundary (see SilenceTracker) the dwell
    // waiver lasts (a new track that turns out to be the same class
    // leaves the waiver unused).
    int64_t BoundaryWindowMs = 15000;
};

// Smooths classifier output into one stable DetectedClass. Feed it every
// inference result, and AddTrackBoundary() when the input comes back
// after a gap between tracks. Silence itself needs no call: no inference
// runs on it, and the current class holds. Reset() for every new Auto
// binding spell.
class DetectionSmoother
{
public:
    explicit DetectionSmoother(DetectionTuning InTuning = {}) : Tuning(InTuning)
    {
    }

    void Reset();
    void AddScores(const ClassScores& Scores, int64_t NowMs);
    // A new track started at NowMs: its class is judged on its own
    // scores, not averaged with the last one's, and may replace the
    // current class without waiting out MinDwellMs. The current class
    // stays until the new one confirms.
    void AddTrackBoundary(int64_t NowMs);

    DetectedClass Current() const
    {
        return CurrentClass;
    }
    // Smoothed score backing Current() (0 when None).
    float Confidence() const;
    // Current class confirmed and unchallenged for MinDwellMs, and no
    // track boundary just passed - inference can slow down.
    bool IsSettled(int64_t NowMs) const;
    // The evidence Decide() works on: the short EMA for Speech, Music and
    // Movie, the track-long genre average for genres.
    const ClassScores& Smoothed() const
    {
        return Evidence;
    }

    // The class the smoothed scores point at right now, before confirm
    // and dwell: Current() biased by hysteresis. Exposed for tests.
    DetectedClass Decide() const;

private:
    DetectionTuning Tuning;
    ClassScores Ema{};
    ClassScores GenreSum{}; // decayed sums; mean = GenreSum / GenreWeight
    float GenreWeight = 0.0f;
    ClassScores Evidence{};
    bool bHaveEma = false;
    int64_t LastSampleMs = 0;

    DetectedClass CurrentClass = DetectedClass::None;
    int64_t CurrentSinceMs = 0;
    DetectedClass Candidate = DetectedClass::None;
    int64_t CandidateSinceMs = 0;

    int64_t BoundaryAtMs = -1; // -1: no dwell waiver pending

    bool InBoundaryWindow(int64_t NowMs) const
    {
        return BoundaryAtMs >= 0 && NowMs - BoundaryAtMs <= Tuning.BoundaryWindowMs;
    }
};

// --- Input audio -------------------------------------------------------------

// The classifier's view of the 7.1 virtual sink input: a standard
// downmix to stereo (centre and surrounds at -3 dB, LFE dropped - it
// duplicates the mains' low end), averaged to mono. Stereo content
// comes out as (L + R) / 2. Real-time safe.
void DownmixSevenOneToMono(const float* Interleaved, size_t Frames, float* OutMono);

// Root mean square of Samples (0 when empty).
float Rms(std::span<const float> Samples);

// Splits the input into silence and sound, chunk by chunk, and spots
// track boundaries: sound returning after a gap long enough to be the
// gap between songs (or a paused video), rather than a pause inside one.
// Durations come from sample counts, so it's independent of when the
// worker happens to wake.
struct SilenceTuning
{
    // Chunks quieter than this (about -60 dBFS) are silent.
    float SilenceRms = 0.001f;
    // Silence at least this long, followed by sound, is a track boundary.
    int64_t BoundarySilenceMs = 1500;
};

class SilenceTracker
{
public:
    explicit SilenceTracker(SilenceTuning InTuning = {}) : Tuning(InTuning)
    {
    }

    struct Result
    {
        bool bSilent = false;
        bool bBoundary = false; // this chunk is sound ending a long enough silence
    };

    Result AddChunk(float ChunkRms, int64_t DurationMs);
    void Reset();

    // Sound since the last silent chunk: an inference window must lie
    // entirely inside it, so a half-silent window (a song's first second)
    // isn't judged.
    int64_t AudibleMs() const
    {
        return AudibleRunMs;
    }

private:
    SilenceTuning Tuning;
    int64_t SilentRunMs = 0;
    int64_t AudibleRunMs = 0;
};

// The last Capacity samples of a mono stream.
class SampleHistory
{
public:
    explicit SampleHistory(size_t InCapacity) : Buffer(InCapacity)
    {
    }

    void Append(std::span<const float> Samples);
    void Clear()
    {
        Filled = 0;
        Next = 0;
    }
    size_t Size() const
    {
        return Filled;
    }
    size_t Capacity() const
    {
        return Buffer.size();
    }
    // Copies the most recent Out.size() samples, oldest first. Returns
    // false (Out untouched) if fewer are held.
    bool CopyLatest(std::span<float> Out) const;

private:
    std::vector<float> Buffer;
    size_t Filled = 0;
    size_t Next = 0; // write position
};

// Windowed-sinc rational resampler (48 kHz -> the model's rate), for
// whole windows at a time: Process() treats samples before the start and
// past the end of its input as 0. A gain-normalised 32-tap kernel per
// phase - plenty for a classifier, which only looks at a mel spectrogram.
// Equal rates copy straight through.
class Resampler
{
public:
    Resampler(uint32_t InRate, uint32_t OutRate);

    // Input samples spanning OutSamples output samples.
    size_t InputFor(size_t OutSamples) const;
    // Fills Out (Out.size() samples) from In.
    void Process(std::span<const float> In, std::span<float> Out) const;

private:
    static constexpr int HalfTaps = 16;
    uint32_t Up = 1;   // L
    uint32_t Down = 1; // M
    std::vector<float> Kernel; // [phase][2 * HalfTaps]
};

// Pacing for the inference worker: adaptive cadence plus the duty-cycle
// governor that holds inference to MaxDutyCycle of one core.
struct SchedulerTuning
{
    int64_t FastIntervalMs = 2000; // finding the class
    int64_t SlowIntervalMs = 6000; // class settled, or on battery
    int64_t MaxIntervalMs = 15000; // the governor won't stretch past this
    float MaxDutyCycle = 0.01f;    // inference time / interval
    int GiveUpAfterRuns = 3;       // consecutive runs too slow even at MaxIntervalMs
    float DurationEmaAlpha = 0.3f;
};

class InferenceScheduler
{
public:
    explicit InferenceScheduler(SchedulerTuning InTuning = {}) : Tuning(InTuning)
    {
    }

    // Forget timings - for a new session (another execution provider or
    // model), whose speed the old numbers say nothing about.
    void Reset();

    // A run finished, taking DurationUs.
    void OnRun(int64_t DurationUs);

    // Milliseconds from the start of the last run to the next one.
    int64_t NextIntervalMs(bool bSettled, bool bOnBattery) const;

    // Inference can't stay within MaxDutyCycle even at MaxIntervalMs on
    // this machine: detection should stop and report it.
    bool GaveUp() const
    {
        return bGaveUp;
    }

    // Perf counters for the GUI/logs (0 before the first run).
    int64_t PercentileUs(float Fraction) const;
    // Expected share of one core at the given cadence.
    float DutyCycle(bool bSettled, bool bOnBattery) const;
    int64_t RunCount() const
    {
        return Runs;
    }

private:
    static constexpr size_t HistorySize = 64;

    SchedulerTuning Tuning;
    float AvgUs = 0.0f;
    int64_t Runs = 0;
    int OverBudgetRuns = 0;
    bool bGaveUp = false;
    std::array<int64_t, HistorySize> History{};
};

} // namespace ramkolfx
