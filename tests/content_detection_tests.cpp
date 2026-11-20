// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// The pure side of Auto-detect (docs/content-detect-plan.md): folding
// model labels into classes, smoothing them into one stable class, and
// pacing inference inside the performance budget.

#include <cmath>
#include <initializer_list>
#include <numbers>
#include <utility>
#include <vector>

#include "content_detection.hpp"
#include "test_harness.hpp"

using namespace ramkolfx;

namespace
{
ClassScores Scores(std::initializer_list<std::pair<DetectedClass, float>> Values)
{
    ClassScores Out{};
    for (const auto& [Class, Score] : Values)
    {
        ScoreOf(Out, Class) = Score;
    }
    return Out;
}

const ClassScores Jazz = Scores({{DetectedClass::Music, 0.9f}, {DetectedClass::Jazz, 0.8f}});
const ClassScores Rock = Scores({{DetectedClass::Music, 0.9f}, {DetectedClass::Rock, 0.8f}});
const ClassScores Speech = Scores({{DetectedClass::Speech, 0.9f}});
const ClassScores Nothing{}; // a quiet passage: nothing confident

// Feeds S every StepMs from FromMs up to and including ToMs.
void Feed(DetectionSmoother& Smoother, const ClassScores& S, int64_t FromMs, int64_t ToMs, int64_t StepMs = 2000)
{
    for (int64_t T = FromMs; T <= ToMs; T += StepMs)
    {
        Smoother.AddScores(S, T);
    }
}
} // namespace

// --- LabelMap -----------------------------------------------------------------

TEST(LabelMapFoldsByNameTakingTheMax)
{
    const std::vector<std::string> Labels{"Speech", "Music", "Jazz", "Swing music", "Banjo"};
    std::vector<std::string> Missing;
    const LabelMap Map = LabelMap::Build(Labels, &Missing);
    CHECK(Map.Covers(DetectedClass::Speech) && Map.Covers(DetectedClass::Jazz));
    CHECK(!Map.Covers(DetectedClass::Rock));
    CHECK(!Missing.empty());

    const std::vector<float> ModelScores{0.1f, 0.7f, 0.3f, 0.6f, 0.99f};
    const ClassScores Folded = Map.Fold(ModelScores);
    CHECK(ScoreOf(Folded, DetectedClass::Speech) == 0.1f);
    CHECK(ScoreOf(Folded, DetectedClass::Music) == 0.7f);
    CHECK(ScoreOf(Folded, DetectedClass::Jazz) == 0.6f); // max of Jazz, Swing music
    CHECK(ScoreOf(Folded, DetectedClass::Rock) == 0.0f);

    // Short output: missing indices count as 0, never read past the end.
    const std::vector<float> Short{0.5f};
    CHECK(ScoreOf(Map.Fold(Short), DetectedClass::Jazz) == 0.0f);
}

// --- DetectionSmoother --------------------------------------------------------

TEST(NothingDetectedUntilConfirmed)
{
    DetectionSmoother Smoother;
    CHECK(Smoother.Current() == DetectedClass::None);
    Smoother.AddScores(Jazz, 0);
    Smoother.AddScores(Jazz, 2000);
    CHECK(Smoother.Current() == DetectedClass::None); // 2s < ConfirmMs
    Smoother.AddScores(Jazz, 4000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
    CHECK(Smoother.Confidence() > 0.7f);
}

TEST(CoarseClassesAndGenreFloor)
{
    DetectionSmoother Talk;
    Feed(Talk, Speech, 0, 4000);
    CHECK(Talk.Current() == DetectedClass::Speech);

    // Music with no genre signal at all: plain Music.
    DetectionSmoother Unsure;
    Feed(Unsure, Scores({{DetectedClass::Music, 0.9f}, {DetectedClass::Pop, 0.005f}}), 0, 4000);
    CHECK(Unsure.Current() == DetectedClass::Music);

    // Below the coarse threshold: nothing.
    DetectionSmoother Weak;
    Feed(Weak, Scores({{DetectedClass::Music, 0.3f}, {DetectedClass::Jazz, 0.9f}}), 0, 10000);
    CHECK(Weak.Current() == DetectedClass::None);
}

TEST(FilmCuesMakeItAMovie)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Scores({{DetectedClass::Speech, 0.8f}, {DetectedClass::Movie, 0.5f}}), 0, 4000);
    CHECK(Smoother.Current() == DetectedClass::Movie);
}

TEST(QuietPassageHoldsTheClass)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 4000);
    Feed(Smoother, Nothing, 6000, 120000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
}

TEST(GenreAmbiguousStretchKeepsTheGenre)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 4000);
    Feed(Smoother, Scores({{DetectedClass::Music, 0.9f}}), 6000, 120000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
}

TEST(GaplessChangeWaitsForTheDwellTime)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 4000); // Jazz since 4000
    Feed(Smoother, Rock, 6000, 12000);
    CHECK(Smoother.Current() == DetectedClass::Jazz); // Rock confirmed, but < MinDwellMs since 4000
    Feed(Smoother, Rock, 14000, 14000);
    CHECK(Smoother.Current() == DetectedClass::Rock);
}

TEST(GaplessGenreChangeOnASettledTrackTakesAboutFifteenSeconds)
{
    // No gap to restart the genre average: Rock has to outweigh a
    // minute of Jazz in it (GenreTauMs) by GenreHysteresis, then confirm.
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 60000);
    Feed(Smoother, Rock, 62000, 70000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
    Feed(Smoother, Rock, 72000, 76000);
    CHECK(Smoother.Current() == DetectedClass::Rock);
}

TEST(GaplessSwitchBetweenCoarseClassesTakesSeconds)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Speech, 0, 60000);
    Feed(Smoother, Jazz, 62000, 66000);
    CHECK(Smoother.Current() == DetectedClass::Speech);
    Feed(Smoother, Jazz, 68000, 70000); // ~8 s after the change
    CHECK(Smoother.Current() == DetectedClass::Jazz);
}

// Real genre scores (EfficientAT on 3 s windows) are low: a clear genre
// often scores 0.15-0.3, with the others near zero.
TEST(DefaultTimingMapsToTheDefaultTuning)
{
    const DetectionTuning Detection = DetectionTuningFor({});
    const DetectionTuning Defaults;
    CHECK(Detection.EmaTauMs == Defaults.EmaTauMs && Detection.ConfirmMs == Defaults.ConfirmMs);
    CHECK(Detection.MinDwellMs == Defaults.MinDwellMs && Detection.BoundaryWindowMs == Defaults.BoundaryWindowMs);
    const SchedulerTuning Cadence = SchedulerTuningFor({});
    const SchedulerTuning DefaultCadence;
    CHECK(Cadence.FastIntervalMs == DefaultCadence.FastIntervalMs);
    CHECK(Cadence.SlowIntervalMs == DefaultCadence.SlowIntervalMs);
    CHECK(Cadence.MaxIntervalMs == DefaultCadence.MaxIntervalMs);
    CHECK(Cadence.MaxDutyCycle == DefaultCadence.MaxDutyCycle);
}

TEST(FastTimingSwitchesWithinSeconds)
{
    // Check every 1 s, switch after 1 s, keep at least 0: a gapless
    // change from speech to music lands ~2 s in instead of ~8.
    DetectionSmoother Smoother(DetectionTuningFor({1000, 1000, 0, 1.0f}));
    Feed(Smoother, Speech, 0, 60000, 1000);
    Feed(Smoother, Jazz, 61000, 62000, 1000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
}

TEST(TuningCanChangeMidSpell)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Speech, 0, 60000);
    Smoother.SetTuning(DetectionTuningFor({1000, 1000, 0, 1.0f}));
    CHECK(Smoother.Current() == DetectedClass::Speech); // kept
    Feed(Smoother, Jazz, 61000, 62000, 1000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
}

TEST(LowButClearGenreIsDetected)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Scores({{DetectedClass::Music, 0.7f}, {DetectedClass::HipHop, 0.2f}, {DetectedClass::Pop, 0.06f}}),
         0, 4000);
    CHECK(Smoother.Current() == DetectedClass::HipHop);
    CHECK(Smoother.Confidence() > 0.15f);
}

TEST(MusicAlwaysGetsTheBestGenre)
{
    // Real genre scores are tiny and close; the best one still wins.
    DetectionSmoother Smoother;
    Feed(Smoother,
         Scores({{DetectedClass::Music, 0.7f}, {DetectedClass::RnB, 0.06f}, {DetectedClass::Pop, 0.05f},
                 {DetectedClass::Electronic, 0.04f}}),
         0, 4000);
    CHECK(Smoother.Current() == DetectedClass::RnB);
}

TEST(ARivalGenreMustClearlyBeatTheCurrentOne)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Scores({{DetectedClass::Music, 0.7f}, {DetectedClass::RnB, 0.06f}, {DetectedClass::Pop, 0.03f}}), 0,
         30000);
    CHECK(Smoother.Current() == DetectedClass::RnB);
    // Pop edges ahead, by less than the hysteresis factor: no change.
    Feed(Smoother, Scores({{DetectedClass::Music, 0.7f}, {DetectedClass::RnB, 0.06f}, {DetectedClass::Pop, 0.07f}}),
         32000, 120000);
    CHECK(Smoother.Current() == DetectedClass::RnB);
    // Pop pulls clearly ahead: it takes over.
    Feed(Smoother, Scores({{DetectedClass::Music, 0.7f}, {DetectedClass::RnB, 0.04f}, {DetectedClass::Pop, 0.08f}}),
         122000, 160000);
    CHECK(Smoother.Current() == DetectedClass::Pop);
}

TEST(NoisyGenreWindowsAverageOut)
{
    // Window by window the lead flips; over the track Electronic is clear.
    const ClassScores A = Scores({{DetectedClass::Music, 0.7f}, {DetectedClass::Electronic, 0.31f}, {DetectedClass::Pop, 0.05f}});
    const ClassScores B = Scores({{DetectedClass::Music, 0.7f}, {DetectedClass::Electronic, 0.09f}, {DetectedClass::Pop, 0.11f}});
    DetectionSmoother Smoother;
    for (int64_t T = 0; T <= 20000; T += 4000)
    {
        Smoother.AddScores(A, T);
        Smoother.AddScores(B, T + 2000);
    }
    CHECK(Smoother.Current() == DetectedClass::Electronic);
}

TEST(TrackBoundaryRestartsTheGenreAverage)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 120000);
    Smoother.AddTrackBoundary(121000);
    Feed(Smoother, Rock, 122000, 126000); // a minute of Jazz doesn't hold it back
    CHECK(Smoother.Current() == DetectedClass::Rock);
}

TEST(TrackBoundarySkipsTheDwell)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 4000);     // Jazz since 4000
    Smoother.AddTrackBoundary(8000);   // the next song starts
    CHECK(!Smoother.IsSettled(8000));
    Feed(Smoother, Rock, 9000, 11000); // first window judged 1 s in
    CHECK(Smoother.Current() == DetectedClass::Jazz); // still confirming
    Feed(Smoother, Rock, 13000, 13000);
    CHECK(Smoother.Current() == DetectedClass::Rock); // before the dwell would allow
}

TEST(BoundaryIntoTheSameClassKeepsIt)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 30000);
    Smoother.AddTrackBoundary(32000);
    Feed(Smoother, Jazz, 33000, 33000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
    Feed(Smoother, Jazz, 35000, 60000);
    CHECK(Smoother.IsSettled(60000)); // waiver window over
}

TEST(ShortBurstOfAnotherClassIsIgnored)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 40000);
    Feed(Smoother, Speech, 42000, 44000); // a DJ talking for a moment
    Feed(Smoother, Jazz, 46000, 80000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
}

TEST(HysteresisFavorsTheCurrentGenre)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 30000);
    // Rock edges ahead by less than the hysteresis margin: no change.
    Feed(Smoother, Scores({{DetectedClass::Music, 0.9f}, {DetectedClass::Jazz, 0.6f}, {DetectedClass::Rock, 0.65f}}),
         32000, 120000);
    CHECK(Smoother.Current() == DetectedClass::Jazz);
}

TEST(SettledAfterDwellWithNoRival)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 4000);
    CHECK(!Smoother.IsSettled(4000));
    Feed(Smoother, Jazz, 6000, 14000);
    CHECK(Smoother.IsSettled(14000));
    // A rival appears: back to fast cadence.
    Feed(Smoother, Speech, 32000, 40000);
    CHECK(!Smoother.IsSettled(40000));
}

TEST(ResetForgetsEverything)
{
    DetectionSmoother Smoother;
    Feed(Smoother, Jazz, 0, 4000);
    Smoother.Reset();
    CHECK(Smoother.Current() == DetectedClass::None);
    CHECK(Smoother.Confidence() == 0.0f);
    Smoother.AddScores(Speech, 100000);
    CHECK(Smoother.Current() == DetectedClass::None); // needs confirming again
}

// --- Input audio --------------------------------------------------------------

TEST(DownmixKeepsStereoLevelAndFoldsTheRest)
{
    // FL FR FC LFE RL RR SL SR
    const float Frames[2 * 8] = {0.4f, 0.2f, 0, 0, 0, 0, 0, 0, //
                                 0, 0, 1.0f, 5.0f, 0.5f, 0.5f, 0, 0};
    float Mono[2] = {};
    DownmixSevenOneToMono(Frames, 2, Mono);
    CHECK(std::abs(Mono[0] - 0.3f) < 1e-6f);
    // Centre at -3 dB, LFE dropped, rears at -3 dB then halved.
    CHECK(std::abs(Mono[1] - (0.70710678f + 0.35355339f)) < 1e-5f);
}

TEST(RmsOfASquareWave)
{
    const std::vector<float> Square{0.5f, -0.5f, 0.5f, -0.5f};
    CHECK(std::abs(Rms(Square) - 0.5f) < 1e-6f);
    CHECK(Rms({}) == 0.0f);
}

TEST(SilenceTrackerSpotsGapsBetweenTracks)
{
    SilenceTracker Tracker;
    CHECK(!Tracker.AddChunk(0.1f, 100).bSilent);
    CHECK(Tracker.AudibleMs() == 100);

    // A 0.5 s pause inside a song: not a boundary.
    for (int i = 0; i < 5; ++i)
    {
        CHECK(Tracker.AddChunk(0.0f, 100).bSilent);
    }
    CHECK(Tracker.AudibleMs() == 0);
    CHECK(!Tracker.AddChunk(0.1f, 100).bBoundary);

    // A 1.5 s gap: the sound after it starts a new track - once.
    for (int i = 0; i < 15; ++i)
    {
        CHECK(!Tracker.AddChunk(0.0005f, 100).bBoundary);
    }
    CHECK(Tracker.AddChunk(0.1f, 100).bBoundary);
    CHECK(!Tracker.AddChunk(0.1f, 100).bBoundary);
    CHECK(Tracker.AudibleMs() == 200);
}

TEST(SampleHistoryKeepsTheLatestInOrder)
{
    SampleHistory History(4);
    std::vector<float> Out(3);
    CHECK(!History.CopyLatest(Out));
    History.Append(std::vector<float>{1, 2, 3});
    History.Append(std::vector<float>{4, 5});
    CHECK(History.Size() == 4);
    CHECK(History.CopyLatest(Out));
    CHECK(Out == (std::vector<float>{3, 4, 5}));

    History.Append(std::vector<float>{6, 7, 8, 9, 10, 11}); // more than fits
    CHECK(History.CopyLatest(Out));
    CHECK(Out == (std::vector<float>{9, 10, 11}));
    History.Clear();
    CHECK(History.Size() == 0 && !History.CopyLatest(Out));
}

namespace
{
std::vector<float> Sine(float Hz, uint32_t Rate, size_t Count, float Amplitude = 0.5f)
{
    std::vector<float> Out(Count);
    for (size_t i = 0; i < Count; ++i)
    {
        Out[i] = Amplitude * std::sin(2.0f * std::numbers::pi_v<float> * Hz * static_cast<float>(i) /
                                      static_cast<float>(Rate));
    }
    return Out;
}

// RMS away from the zero-padded edges.
float InnerRms(const std::vector<float>& Samples)
{
    const size_t Margin = Samples.size() / 8;
    return Rms(std::span<const float>(Samples).subspan(Margin, Samples.size() - 2 * Margin));
}
} // namespace

TEST(ResamplerPassesTheBandAndStopsAliases)
{
    for (const uint32_t Rate : {32000u, 16000u})
    {
        const Resampler R(48000, Rate);
        const size_t OutCount = Rate / 2; // 0.5 s
        CHECK(R.InputFor(OutCount) == 24000);

        std::vector<float> Out(OutCount);
        R.Process(Sine(1000.0f, 48000, 24000), Out);
        const float Expected = InnerRms(Sine(1000.0f, Rate, OutCount));
        CHECK(std::abs(InnerRms(Out) - Expected) < 0.01f * Expected);
        // Still 1 kHz: matches a sine generated at the output rate.
        const std::vector<float> Reference = Sine(1000.0f, Rate, OutCount);
        float MaxError = 0.0f;
        for (size_t i = OutCount / 8; i < OutCount - OutCount / 8; ++i)
        {
            MaxError = std::max(MaxError, std::abs(Out[i] - Reference[i]));
        }
        CHECK(MaxError < 0.01f);

        // Above the output's Nyquist: filtered out, not aliased down.
        const float Above = static_cast<float>(Rate) * 0.5f + 3000.0f;
        R.Process(Sine(Above, 48000, 24000), Out);
        CHECK(InnerRms(Out) < 0.01f * InnerRms(Sine(Above, 48000, 24000)));
    }

    // Same rate: a straight copy.
    const Resampler Same(48000, 48000);
    const std::vector<float> In{0.1f, -0.2f, 0.3f};
    std::vector<float> Out(3);
    Same.Process(In, Out);
    CHECK(std::abs(Out[1] + 0.2f) < 1e-3f);
}

// --- InferenceScheduler -------------------------------------------------------

TEST(CadenceIsFastThenSlow)
{
    InferenceScheduler Scheduler;
    Scheduler.OnRun(4000); // 4 ms: 1% budget needs only 400 ms
    CHECK(Scheduler.NextIntervalMs(false, false) == 2000);
    CHECK(Scheduler.NextIntervalMs(true, false) == 6000);
    CHECK(Scheduler.NextIntervalMs(false, true) == 6000); // battery
    CHECK(Scheduler.DutyCycle(false, false) < 0.01f);
}

TEST(GovernorStretchesTheIntervalToHoldTheDutyCap)
{
    InferenceScheduler Scheduler;
    for (int i = 0; i < 10; ++i)
    {
        Scheduler.OnRun(50000); // 50 ms -> 5 s needed at 1%
    }
    CHECK(Scheduler.NextIntervalMs(false, false) == 5000);
    CHECK(Scheduler.NextIntervalMs(true, false) == 6000);
    CHECK(Scheduler.DutyCycle(false, false) <= 0.0101f);
    CHECK(!Scheduler.GaveUp());
}

TEST(GovernorGivesUpOnlyOnASteadyTrend)
{
    InferenceScheduler Scheduler;
    Scheduler.OnRun(400000); // 400 ms -> 40 s needed: over the 15 s max
    Scheduler.OnRun(400000);
    Scheduler.OnRun(5000); // one fast run resets the streak
    Scheduler.OnRun(400000);
    Scheduler.OnRun(400000);
    CHECK(!Scheduler.GaveUp());
    CHECK(Scheduler.NextIntervalMs(false, false) == 15000); // capped while trying
    Scheduler.OnRun(400000);
    CHECK(Scheduler.GaveUp());

    Scheduler.Reset();
    CHECK(!Scheduler.GaveUp() && Scheduler.RunCount() == 0);
}

TEST(NewCadenceAndBudgetApplyAndRejudgeAGiveUp)
{
    InferenceScheduler Scheduler;
    for (int i = 0; i < 3; ++i)
    {
        Scheduler.OnRun(200000); // 200 ms at 1% needs 20 s: over the 15 s max
    }
    CHECK(Scheduler.GaveUp());

    Scheduler.SetTuning(SchedulerTuningFor({500, 4000, 10000, 5.0f})); // 200 ms at 5%: 4 s
    CHECK(!Scheduler.GaveUp() && Scheduler.RunCount() == 3);
    Scheduler.OnRun(200000);
    CHECK(!Scheduler.GaveUp());
    CHECK(Scheduler.NextIntervalMs(false, false) == 4000);

    InferenceScheduler Fast(SchedulerTuningFor({500, 4000, 10000, 5.0f}));
    Fast.OnRun(4000); // 4 ms: well inside 5%
    CHECK(Fast.NextIntervalMs(false, false) == 500);
    CHECK(Fast.NextIntervalMs(true, false) == 1500);
}

TEST(PercentilesOverRecentRuns)
{
    InferenceScheduler Scheduler;
    CHECK(Scheduler.PercentileUs(0.5f) == 0);
    for (int i = 1; i <= 100; ++i)
    {
        Scheduler.OnRun(i * 1000);
    }
    // Only the last 64 runs (37..100 ms) count.
    CHECK(Scheduler.PercentileUs(0.0f) == 37000);
    CHECK(Scheduler.PercentileUs(0.5f) == 69000);
    CHECK(Scheduler.PercentileUs(1.0f) == 100000);
}

RAMKOLFX_TEST_MAIN()
