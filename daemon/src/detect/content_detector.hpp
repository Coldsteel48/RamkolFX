// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "content_detection.hpp"
#include "detect/content_classifier.hpp"
#include "ramkolfx/ring_buffer.hpp"
#include "ramkolfx/types.hpp"

namespace ramkolfx
{

struct ContentDetectorTuning
{
    uint32_t InputRate = 48000;
    // While active: how often the tapped audio is drained and checked for
    // silence, and how long a chunk the silence check judges.
    int64_t PollIntervalMs = 500;
    int64_t ChunkMs = 100;
    // Unload the classifier after this long without an Auto binding in
    // effect, or with detection paused.
    int64_t IdleUnloadMs = 5 * 60 * 1000;
    // RT -> worker buffer, in seconds of mono audio.
    int64_t TapSeconds = 4;
    // SCHED_IDLE + nice 19 for the worker. Off in tests.
    bool bLowPriority = true;
    // Use the slow cadence on battery (/sys/class/power_supply).
    bool bWatchBattery = true;
    SchedulerTuning Scheduler;
    SilenceTuning Silence;
};

// Something the worker found, for AutoDetectSession: an inference result
// or a track boundary. AtMs is steady-clock milliseconds: when the run
// finished, or when the boundary's sound arrived at the tap.
struct DetectionEvent
{
    int64_t AtMs = 0;
    bool bTrackBoundary = false;
    ClassScores Scores{}; // when !bTrackBoundary
};

struct DetectorStats
{
    AutoDetectAvailability Availability = AutoDetectAvailability::Ok;
    bool bLoaded = false;
    std::string DeviceLabel; // "" until loaded
    int64_t P50Us = 0;       // inference CPU time per run, recent runs
    int64_t P95Us = 0;
    float DutyCycle = 0.0f;  // at the current cadence
    int64_t Runs = 0;
    int64_t RssAddedKb = 0;  // by the last load
    int64_t TapDroppedSamples = 0; // the worker fell behind (starved)
};

// The Auto-detect inference worker (docs/content-detect-plan.md). The RT
// thread taps the 7.1 input into it; a low-priority thread drains that,
// skips silence, spots track boundaries, and runs the classifier at the
// cadence InferenceScheduler allows. The control side switches it on and
// off and collects DetectionEvents.
//
// Costs nothing while no Auto binding is in effect: no thread until first
// activated, then one blocked on a condition variable (no timer); the
// classifier is loaded on first activation and unloaded after
// IdleUnloadMs inactive; the RT tap is one atomic load.
class ContentDetector
{
public:
    explicit ContentDetector(ClassifierBackend Backend, ContentDetectorTuning Tuning = {});
    ~ContentDetector();

    ContentDetector(const ContentDetector&) = delete;
    ContentDetector& operator=(const ContentDetector&) = delete;

    // RT thread: one block of 8-channel 7.1 input, before any processing.
    // Real-time safe; a single atomic load when inactive.
    void Tap(const float* Interleaved, uint32 Frames) noexcept;

    // An Auto binding is in effect (held or not). A different SpellId
    // (another binding, or the same one's new playing spell) starts over:
    // stale audio dropped, scheduling fresh.
    void SetActive(bool bActive, uint64_t SpellId = 0);

    // The detected class has settled: inference slows down.
    void SetSettled(bool bSettled)
    {
        bSettledFlag.store(bSettled, std::memory_order_relaxed);
    }

    // Detection is paused (the user set the EQ by hand): keep tracking
    // silence and reporting track boundaries, but run no inference - and
    // unload the classifier after IdleUnloadMs of it. Set it before
    // SetActive, which may wake the worker for a new spell.
    void SetPaused(bool bPaused)
    {
        bPausedFlag.store(bPaused, std::memory_order_relaxed);
    }

    // Changes the device policy. The classifier is rebuilt on next use,
    // and a Failed/TooSlow verdict is forgotten (the new device may do).
    void SetDevice(InferenceDevice Device);

    // Re-probes the runtime and model files if a probe is what the
    // current availability rests on (no load tried yet, or none since
    // they went missing) - so installing ONNX Runtime while the daemon
    // runs enables Auto-detect. Load-time verdicts (Failed, TooSlow) stay
    // until SetDevice().
    void RefreshAvailability();

    // Whether Auto bindings should resolve.
    AutoDetectAvailability Availability() const;

    // Events since the last call, oldest first.
    std::vector<DetectionEvent> TakeEvents();

    DetectorStats Stats() const;

private:
    void Run();
    void StartSpell();
    // Drain + silence + maybe inference. Called with Lock held; releases
    // it while working.
    void Step(std::unique_lock<std::mutex>& Lock);
    bool LoadClassifier(std::unique_lock<std::mutex>& Lock, InferenceDevice Device);
    // bKeepTap: the worker goes on draining the tap (paused, tracking
    // silence), so it stays on through the teardown - no audio lost.
    void Unload(std::unique_lock<std::mutex>& Lock, bool bKeepTap = false);
    void PushEvent(const DetectionEvent& Event); // Mutex held

    const ClassifierBackend Backend;
    const ContentDetectorTuning Tuning;

    // RT side. TapRing is created on first activation (before the tap is
    // first enabled) so an unused detector holds no buffer.
    std::atomic<bool> bTapEnabled{false};
    std::unique_ptr<RingBuffer<float>> TapRing;
    std::atomic<int64_t> TapDropped{0};
    std::atomic<bool> bSettledFlag{false};
    std::atomic<bool> bPausedFlag{false};

    // Guarded by Mutex.
    mutable std::mutex Mutex;
    std::condition_variable Wake;
    std::thread Worker;
    bool bStop = false;
    bool bActive = false;
    uint64_t ActiveSpell = 0;
    uint64_t SpellGeneration = 0; // bumped when a fresh spell must start
    int64_t InactiveSinceMs = 0;
    InferenceDevice Device = InferenceDevice::Auto;
    bool bDeviceChanged = false;
    AutoDetectAvailability CurrentAvailability = AutoDetectAvailability::Ok;
    bool bVerdictFromLoad = false; // CurrentAvailability came from a load, not a probe
    std::vector<DetectionEvent> Events;
    InferenceScheduler Scheduler;
    std::string DeviceLabel;
    int64_t RssAddedKb = 0;

    // Worker-only.
    std::unique_ptr<ContentClassifier> Classifier;
    LabelMap Labels;
    std::optional<Resampler> ToModelRate;
    SampleHistory History{0};
    SilenceTracker Silence;
    std::vector<float> Window;     // at the input rate
    std::vector<float> ModelInput; // at the model's rate
    std::vector<float> RawScores;
    std::vector<float> Drained;
    std::vector<float> Chunk;
    size_t ChunkFill = 0;
    uint64_t SeenGeneration = 0;
    int64_t NextRunAtMs = 0;
    int64_t PausedSinceMs = -1; // -1 = not paused
    std::atomic<bool> bOnBattery{false};
    int64_t BatteryCheckedAtMs = -1;
    bool bGovernorStretching = false;
    // RAMKOLFX_DETECT_DUMP=<dir>: log each run's top labels and save the
    // window the model saw - for tuning and debugging, off by default.
    std::string DumpDir;
    uint64_t DumpRuns = 0;
};

// True when a system battery (not a mouse's or headset's) is discharging.
bool IsOnBattery();

} // namespace ramkolfx
