// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// The Auto-detect worker (docs/content-detect-plan.md) against a fake
// classifier: lazy loading, silence skipping, track boundaries, the
// duty-cycle give-up, idle unload and accelerator fallback. Timings are
// shrunk to milliseconds; waits are generous so a busy machine doesn't
// fail them.

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "detect/content_detector.hpp"
#include "test_harness.hpp"

using namespace ramkolfx;

namespace
{
struct FakeState
{
    std::atomic<int> Loads{0};
    std::atomic<int> CpuLoads{0};
    std::atomic<int> Runs{0};
    std::atomic<int64_t> SpinUs{0};       // CPU time each run burns
    std::atomic<bool> bAcceleratorFails{false};
    AutoDetectAvailability LoadVerdict = AutoDetectAvailability::Ok;
};

class FakeClassifier final : public ContentClassifier
{
public:
    FakeClassifier(FakeState& InState, bool bInCpu) : State(InState), bCpu(bInCpu)
    {
    }
    uint32_t SampleRate() const override
    {
        return 16000;
    }
    size_t InputSamples() const override
    {
        return 8000; // 0.5 s
    }
    const std::vector<std::string>& Labels() const override
    {
        return LabelNames;
    }
    bool Classify(std::span<const float> Waveform, std::vector<float>& OutScores, std::string& OutError) override
    {
        if (Waveform.size() != InputSamples())
        {
            OutError = "length";
            return false;
        }
        if (!bCpu && State.bAcceleratorFails)
        {
            OutError = "device lost";
            return false;
        }
        const int64_t Until = ThreadCpuTimeUs() + State.SpinUs;
        while (ThreadCpuTimeUs() < Until)
        {
        }
        ++State.Runs;
        OutScores = {0.1f, 0.9f, 0.8f};
        return true;
    }
    std::string DeviceLabel() const override
    {
        return bCpu ? "CPU" : "NPU (fake)";
    }
    bool IsCpu() const override
    {
        return bCpu;
    }

private:
    FakeState& State;
    bool bCpu;
    std::vector<std::string> LabelNames{"Speech", "Music", "Jazz"};
};

ClassifierBackend FakeBackend(FakeState& State, bool bAccelerator = false)
{
    ClassifierBackend Backend;
    Backend.Probe = [] { return AutoDetectAvailability::Ok; };
    Backend.Load = [&State, bAccelerator](InferenceDevice Device) {
        ClassifierLoadResult Result;
        ++State.Loads;
        if (State.LoadVerdict != AutoDetectAvailability::Ok)
        {
            Result.Availability = State.LoadVerdict;
            Result.Message = "fake failure";
            return Result;
        }
        const bool bCpu = !bAccelerator || Device == InferenceDevice::Cpu;
        State.CpuLoads += bCpu ? 1 : 0;
        Result.Classifier = std::make_unique<FakeClassifier>(State, bCpu);
        Result.Message = "fake";
        return Result;
    };
    return Backend;
}

ContentDetectorTuning FastTuning()
{
    ContentDetectorTuning Tuning;
    Tuning.PollIntervalMs = 10;
    Tuning.IdleUnloadMs = 150;
    Tuning.bLowPriority = false;
    Tuning.bWatchBattery = false;
    Tuning.Scheduler.FastIntervalMs = 30;
    Tuning.Scheduler.SlowIntervalMs = 60;
    Tuning.Scheduler.MaxIntervalMs = 200;
    return Tuning;
}

bool WaitFor(const std::function<bool()>& Condition, int TimeoutMs = 5000)
{
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
    while (std::chrono::steady_clock::now() < Deadline)
    {
        if (Condition())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return Condition();
}

// Ms of 7.1 input: a 440 Hz tone on the fronts, or silence.
void TapAudio(ContentDetector& Detector, int Ms, bool bSilent = false)
{
    constexpr uint32 Block = 480;
    std::vector<float> Frames(Block * 8);
    static uint64_t Phase = 0;
    for (int Done = 0; Done < Ms * 48; Done += Block)
    {
        for (uint32 i = 0; i < Block; ++i)
        {
            const float S = bSilent ? 0.0f : 0.3f * std::sin(2.0f * 3.14159265f * 440.0f * (Phase + i) / 48000.0f);
            Frames[i * 8 + 0] = S;
            Frames[i * 8 + 1] = S;
        }
        Phase += Block;
        Detector.Tap(Frames.data(), Block);
    }
}

bool Loaded(const ContentDetector& Detector)
{
    return Detector.Stats().bLoaded;
}

int CountScores(const std::vector<DetectionEvent>& Events)
{
    int Count = 0;
    for (const DetectionEvent& E : Events)
    {
        Count += E.bTrackBoundary ? 0 : 1;
    }
    return Count;
}
} // namespace

TEST(NothingHappensUntilActivated)
{
    FakeState State;
    ContentDetector Detector(FakeBackend(State), FastTuning());
    TapAudio(Detector, 1000); // tap off: dropped on the floor
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(State.Loads == 0);
    CHECK(Detector.TakeEvents().empty());
    CHECK(Detector.Availability() == AutoDetectAvailability::Ok);
    CHECK(!Loaded(Detector));
}

TEST(ActiveDetectorClassifiesSound)
{
    FakeState State;
    ContentDetector Detector(FakeBackend(State), FastTuning());
    Detector.SetActive(true, 1);
    REQUIRE(WaitFor([&] { return Loaded(Detector); }));
    CHECK(Detector.Stats().DeviceLabel == "CPU");

    TapAudio(Detector, 1000);
    std::vector<DetectionEvent> Events;
    REQUIRE(WaitFor([&] {
        auto New = Detector.TakeEvents();
        Events.insert(Events.end(), New.begin(), New.end());
        return CountScores(Events) > 0;
    }));
    const DetectionEvent& First = Events.back();
    CHECK(!First.bTrackBoundary);
    CHECK(ScoreOf(First.Scores, DetectedClass::Jazz) == 0.8f);
    CHECK(ScoreOf(First.Scores, DetectedClass::Music) == 0.9f);
    CHECK(Detector.Stats().Runs >= 1);
    CHECK(State.Loads == 1);
}

TEST(SilenceIsSkippedAndAGapIsATrackBoundary)
{
    FakeState State;
    ContentDetector Detector(FakeBackend(State), FastTuning());
    Detector.SetActive(true, 1);
    REQUIRE(WaitFor([&] { return Loaded(Detector); }));

    TapAudio(Detector, 1000, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(State.Runs == 0); // nothing to classify

    // Not enough sound for a whole window yet either.
    TapAudio(Detector, 300);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(State.Runs == 0);
    Detector.TakeEvents();

    // A 2 s gap, then the next song.
    TapAudio(Detector, 2000, true);
    TapAudio(Detector, 1000);
    std::vector<DetectionEvent> Events;
    REQUIRE(WaitFor([&] {
        auto New = Detector.TakeEvents();
        Events.insert(Events.end(), New.begin(), New.end());
        return CountScores(Events) > 0;
    }));
    REQUIRE(!Events.empty());
    CHECK(Events.front().bTrackBoundary);
}

TEST(LoadFailureMakesItUnavailableUntilTheDeviceChanges)
{
    FakeState State;
    State.LoadVerdict = AutoDetectAvailability::NoRuntime;
    ContentDetector Detector(FakeBackend(State), FastTuning());
    Detector.SetActive(true, 1);
    REQUIRE(WaitFor([&] { return Detector.Availability() == AutoDetectAvailability::NoRuntime; }));

    // Re-probing doesn't override what a real load found.
    Detector.RefreshAvailability();
    CHECK(Detector.Availability() == AutoDetectAvailability::NoRuntime);
    CHECK(State.Loads == 1);

    State.LoadVerdict = AutoDetectAvailability::Ok;
    Detector.SetDevice(InferenceDevice::Cpu);
    CHECK(Detector.Availability() == AutoDetectAvailability::Ok);
    REQUIRE(WaitFor([&] { return Loaded(Detector); }));
    CHECK(State.Loads == 2);
}

TEST(TooSlowGivesUp)
{
    FakeState State;
    State.SpinUs = 4000; // 4 ms at 1% needs 400 ms between runs; the max is 200
    ContentDetector Detector(FakeBackend(State), FastTuning());
    Detector.SetActive(true, 1);
    REQUIRE(WaitFor([&] { return Loaded(Detector); }));
    TapAudio(Detector, 2000);
    REQUIRE(WaitFor([&] { return Detector.Availability() == AutoDetectAvailability::TooSlow; }));
    CHECK(State.Runs == 3);
    REQUIRE(WaitFor([&] { return !Loaded(Detector); }));
}

TEST(IdleUnloadAndReload)
{
    FakeState State;
    ContentDetector Detector(FakeBackend(State), FastTuning());
    Detector.SetActive(true, 1);
    REQUIRE(WaitFor([&] { return Loaded(Detector); }));
    Detector.SetActive(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(Loaded(Detector)); // not straight away: bindings flicker
    REQUIRE(WaitFor([&] { return !Loaded(Detector); }));

    Detector.SetActive(true, 2);
    REQUIRE(WaitFor([&] { return Loaded(Detector); }));
    CHECK(State.Loads == 2);
}

TEST(FailingAcceleratorFallsBackToTheCpu)
{
    FakeState State;
    State.bAcceleratorFails = true;
    ContentDetector Detector(FakeBackend(State, true), FastTuning());
    Detector.SetActive(true, 1);
    REQUIRE(WaitFor([&] { return Loaded(Detector); }));
    CHECK(Detector.Stats().DeviceLabel == "NPU (fake)");
    TapAudio(Detector, 1000);
    REQUIRE(WaitFor([&] { return Detector.Stats().DeviceLabel == "CPU"; }));
    CHECK(Detector.Availability() == AutoDetectAvailability::Ok);
    CHECK(State.CpuLoads == 1);
}

RAMKOLFX_TEST_MAIN()
