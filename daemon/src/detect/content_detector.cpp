// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "detect/content_detector.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>

namespace ramkolfx
{

namespace
{
constexpr uint32 TapChannels = 8;
constexpr size_t MaxEvents = 64; // if nobody collects them, keep the newest

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t ResidentKb()
{
    std::ifstream Statm("/proc/self/statm");
    long Size = 0;
    long Resident = 0;
    Statm >> Size >> Resident;
    return static_cast<int64_t>(Resident) * sysconf(_SC_PAGESIZE) / 1024;
}

std::string ReadLine(const std::filesystem::path& Path)
{
    std::ifstream File(Path);
    std::string Line;
    std::getline(File, Line);
    return Line;
}

// Debug aid (RAMKOLFX_DETECT_DUMP): the window as a 32-bit float WAV,
// and the top labels on one log line.
void DumpRun(const std::string& Dir, uint64_t Run, std::span<const float> Samples, uint32_t Rate,
             const std::vector<float>& Scores, const std::vector<std::string>& Names, const ClassScores& Folded)
{
    const std::string Path = Dir + "/window-" + std::to_string(Run % 20) + ".wav";
    std::ofstream File(Path, std::ios::binary | std::ios::trunc);
    const auto Put32 = [&](uint32_t V) { File.write(reinterpret_cast<const char*>(&V), 4); };
    const auto Put16 = [&](uint16_t V) { File.write(reinterpret_cast<const char*>(&V), 2); };
    const auto Bytes = static_cast<uint32_t>(Samples.size() * sizeof(float));
    File.write("RIFF", 4);
    Put32(36 + Bytes);
    File.write("WAVEfmt ", 8);
    Put32(16);
    Put16(3); // IEEE float
    Put16(1);
    Put32(Rate);
    Put32(Rate * 4);
    Put16(4);
    Put16(32);
    File.write("data", 4);
    Put32(Bytes);
    File.write(reinterpret_cast<const char*>(Samples.data()), Bytes);

    std::vector<size_t> Order(Scores.size());
    for (size_t i = 0; i < Order.size(); ++i)
    {
        Order[i] = i;
    }
    const size_t Top = std::min<size_t>(8, Order.size());
    std::partial_sort(Order.begin(), Order.begin() + static_cast<std::ptrdiff_t>(Top), Order.end(),
                      [&](size_t A, size_t B) { return Scores[A] > Scores[B]; });
    std::string Line;
    for (size_t i = 0; i < Top; ++i)
    {
        char Item[96];
        std::snprintf(Item, sizeof(Item), "%s%s %.2f", i ? ", " : "",
                      Order[i] < Names.size() ? Names[Order[i]].c_str() : "?", static_cast<double>(Scores[Order[i]]));
        Line += Item;
    }
    std::string Classes;
    for (size_t i = 1; i < DetectedClassCount; ++i)
    {
        if (Folded[i] >= 0.05f)
        {
            char Item[64];
            std::snprintf(Item, sizeof(Item), " %s=%.2f", DetectedClassLabel(static_cast<DetectedClass>(i)),
                          static_cast<double>(Folded[i]));
            Classes += Item;
        }
    }
    fprintf(stderr, "[ramkolfxd] auto-detect run %llu (%s): %s |%s\n", static_cast<unsigned long long>(Run),
            Path.c_str(), Line.c_str(), Classes.c_str());
}

void LowerOwnPriority()
{
    // Only CPU nobody else wants: under load detection stalls and the
    // last class holds, which is the right outcome.
    sched_param Param{};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &Param);
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
}
} // namespace

bool IsOnBattery()
{
    std::error_code Error;
    for (const auto& Entry : std::filesystem::directory_iterator("/sys/class/power_supply", Error))
    {
        if (ReadLine(Entry.path() / "type") == "Battery" && ReadLine(Entry.path() / "scope") != "Device" &&
            ReadLine(Entry.path() / "status") == "Discharging")
        {
            return true;
        }
    }
    return false;
}

ContentDetector::ContentDetector(ClassifierBackend InBackend, ContentDetectorTuning InTuning)
    : Backend(std::move(InBackend)), Tuning(InTuning), Scheduler(InTuning.Scheduler), Silence(InTuning.Silence)
{
    if (const char* Dump = std::getenv("RAMKOLFX_DETECT_DUMP"); Dump && *Dump)
    {
        DumpDir = Dump;
    }
    CurrentAvailability = Backend.Probe ? Backend.Probe() : AutoDetectAvailability::Ok;
    Chunk.resize(static_cast<size_t>(Tuning.InputRate * Tuning.ChunkMs / 1000));
    Drained.resize(Chunk.size());
}

ContentDetector::~ContentDetector()
{
    {
        std::lock_guard Lock(Mutex);
        bStop = true;
    }
    Wake.notify_all();
    if (Worker.joinable())
    {
        Worker.join();
    }
}

void ContentDetector::Tap(const float* Interleaved, uint32 Frames) noexcept
{
    if (!bTapEnabled.load(std::memory_order_acquire))
    {
        return;
    }
    constexpr uint32 Block = 256;
    float Mono[Block];
    while (Frames > 0)
    {
        const uint32 Run = std::min(Frames, Block);
        DownmixSevenOneToMono(Interleaved, Run, Mono);
        const size_t Pushed = TapRing->Push(Mono, Run);
        if (Pushed < Run)
        {
            TapDropped.fetch_add(static_cast<int64_t>(Run - Pushed), std::memory_order_relaxed);
        }
        Interleaved += static_cast<size_t>(Run) * TapChannels;
        Frames -= Run;
    }
}

void ContentDetector::SetActive(bool bNowActive, uint64_t SpellId)
{
    bool bChanged = false;
    {
        std::lock_guard Lock(Mutex);
        if (bNowActive && (!bActive || SpellId != ActiveSpell))
        {
            ++SpellGeneration;
            bChanged = true;
        }
        if (!bNowActive && bActive)
        {
            InactiveSinceMs = NowMs();
            bChanged = true;
        }
        bActive = bNowActive;
        ActiveSpell = SpellId;
        if (bActive && !Worker.joinable())
        {
            TapRing = std::make_unique<RingBuffer<float>>(
                static_cast<size_t>(Tuning.TapSeconds) * Tuning.InputRate);
            Worker = std::thread(&ContentDetector::Run, this);
        }
    }
    if (bChanged)
    {
        Wake.notify_all(); // only on a change: the engine calls this every tick
    }
}

void ContentDetector::SetDevice(InferenceDevice NewDevice)
{
    {
        std::lock_guard Lock(Mutex);
        bDeviceChanged = bDeviceChanged || NewDevice != Device;
        Device = NewDevice;
        if (bVerdictFromLoad)
        {
            bVerdictFromLoad = false;
            bDeviceChanged = true; // retry even on the same device
            CurrentAvailability = Backend.Probe ? Backend.Probe() : AutoDetectAvailability::Ok;
        }
    }
    Wake.notify_all();
}

void ContentDetector::SetCadence(const SchedulerTuning& Cadence)
{
    {
        std::lock_guard Lock(Mutex);
        Scheduler.SetTuning(Cadence);
        bCadenceChanged = true;
        if (bVerdictFromLoad && CurrentAvailability == AutoDetectAvailability::TooSlow)
        {
            // Judged against the old budget: try again under the new one.
            bVerdictFromLoad = false;
            CurrentAvailability = Backend.Probe ? Backend.Probe() : AutoDetectAvailability::Ok;
        }
    }
    Wake.notify_all();
}

void ContentDetector::RefreshAvailability()
{
    std::lock_guard Lock(Mutex);
    if (bVerdictFromLoad || !Backend.Probe || !DeviceLabel.empty())
    {
        return; // a load's verdict, or loaded and working
    }
    const AutoDetectAvailability Probed = Backend.Probe();
    if (Probed != CurrentAvailability)
    {
        CurrentAvailability = Probed;
        Wake.notify_all();
    }
}

AutoDetectAvailability ContentDetector::Availability() const
{
    std::lock_guard Lock(Mutex);
    return CurrentAvailability;
}

std::vector<DetectionEvent> ContentDetector::TakeEvents()
{
    std::lock_guard Lock(Mutex);
    std::vector<DetectionEvent> Out;
    Out.swap(Events);
    return Out;
}

DetectorStats ContentDetector::Stats() const
{
    std::lock_guard Lock(Mutex);
    DetectorStats Out;
    Out.Availability = CurrentAvailability;
    Out.bLoaded = !DeviceLabel.empty();
    Out.DeviceLabel = DeviceLabel;
    Out.P50Us = Scheduler.PercentileUs(0.5f);
    Out.P95Us = Scheduler.PercentileUs(0.95f);
    Out.Runs = Scheduler.RunCount();
    Out.DutyCycle = Out.Runs > 0 ? Scheduler.DutyCycle(bSettledFlag.load(std::memory_order_relaxed),
                                                       bOnBattery.load(std::memory_order_relaxed))
                                 : 0.0f;
    Out.RssAddedKb = RssAddedKb;
    Out.TapDroppedSamples = TapDropped.load(std::memory_order_relaxed);
    return Out;
}

void ContentDetector::PushEvent(const DetectionEvent& Event)
{
    if (Events.size() >= MaxEvents)
    {
        Events.erase(Events.begin());
    }
    Events.push_back(Event);
}

void ContentDetector::Run()
{
    if (Tuning.bLowPriority)
    {
        LowerOwnPriority();
    }
    pthread_setname_np(pthread_self(), "ramkolfx-detect");

    std::unique_lock Lock(Mutex);
    while (!bStop)
    {
        if (bDeviceChanged)
        {
            bDeviceChanged = false;
            if (Classifier)
            {
                Unload(Lock);
            }
            continue;
        }
        if (!bActive || CurrentAvailability != AutoDetectAvailability::Ok)
        {
            bTapEnabled.store(false, std::memory_order_relaxed);
            PausedSinceMs = -1; // a pause before this says nothing about the next spell
            if (Classifier && bActive)
            {
                Unload(Lock); // gave up while active
                continue;
            }
            if (Classifier)
            {
                const int64_t UnloadAtMs = InactiveSinceMs + Tuning.IdleUnloadMs;
                if (NowMs() >= UnloadAtMs)
                {
                    Unload(Lock);
                    continue;
                }
                Wake.wait_for(Lock, std::chrono::milliseconds(UnloadAtMs - NowMs()));
            }
            else
            {
                Wake.wait(Lock); // no timer: nothing runs until something changes
            }
            continue;
        }
        // Paused (the user set the EQ by hand), only silence is tracked,
        // which needs no classifier: it goes after IdleUnloadMs, like
        // when inactive, and comes back once detection resumes.
        const bool bPaused = bPausedFlag.load(std::memory_order_relaxed);
        if (!bPaused)
        {
            if (PausedSinceMs >= 0)
            {
                NextRunAtMs = 0; // resumed for a re-check or to learn: run as soon as there's a window
            }
            PausedSinceMs = -1;
        }
        else if (PausedSinceMs < 0)
        {
            PausedSinceMs = NowMs();
        }
        if (Classifier && bPaused && NowMs() - PausedSinceMs >= Tuning.IdleUnloadMs)
        {
            Unload(Lock, true); // still listening for the gap
            continue;
        }
        if (!Classifier && !bPaused && !LoadClassifier(Lock, Device))
        {
            continue;
        }
        // Also after any other Unload (device change, failure) while
        // paused, which skips the reload that would otherwise do this:
        // deaf, the hold could never see a gap.
        if (SeenGeneration != SpellGeneration || !bTapEnabled.load(std::memory_order_relaxed))
        {
            StartSpell();
        }
        Step(Lock);
        if (!bStop && bActive && !bDeviceChanged && (Classifier || bPaused))
        {
            Wake.wait_for(Lock, std::chrono::milliseconds(Tuning.PollIntervalMs));
        }
    }
    bTapEnabled.store(false, std::memory_order_relaxed);
    Lock.unlock();
    Classifier.reset();
}

bool ContentDetector::LoadClassifier(std::unique_lock<std::mutex>& Lock, InferenceDevice LoadDevice)
{
    const int64_t RssBefore = ResidentKb();
    Lock.unlock();
    ClassifierLoadResult Result = Backend.Load(LoadDevice);
    std::vector<std::string> Missing;
    LabelMap NewLabels;
    if (Result.Classifier)
    {
        NewLabels = LabelMap::Build(Result.Classifier->Labels(), &Missing);
        if (!NewLabels.Covers(DetectedClass::Speech) || !NewLabels.Covers(DetectedClass::Music))
        {
            Result.Classifier.reset();
            Result.Availability = AutoDetectAvailability::Failed;
            Result.Message = "the model has no Speech/Music labels";
        }
    }
    Lock.lock();

    if (!Result.Classifier)
    {
        CurrentAvailability = Result.Availability == AutoDetectAvailability::Ok ? AutoDetectAvailability::Failed
                                                                                : Result.Availability;
        bVerdictFromLoad = true;
        fprintf(stderr, "[ramkolfxd] auto-detect unavailable: %s\n", Result.Message.c_str());
        return false;
    }
    fprintf(stderr, "[ramkolfxd] auto-detect: %s\n", Result.Message.c_str());
    if (!Missing.empty())
    {
        fprintf(stderr, "[ramkolfxd] auto-detect: %zu expected labels not in the model (first: '%s')\n",
                Missing.size(), Missing.front().c_str());
    }

    Classifier = std::move(Result.Classifier);
    Labels = std::move(NewLabels);
    ToModelRate.emplace(Tuning.InputRate, Classifier->SampleRate());
    ModelInput.resize(Classifier->InputSamples());
    Window.resize(ToModelRate->InputFor(ModelInput.size()));
    History = SampleHistory(Window.size());
    Scheduler.Reset(); // a new session's speed: the old timings say nothing about it
    bGovernorStretching = false;
    DeviceLabel = Classifier->DeviceLabel();
    RssAddedKb = std::max<int64_t>(0, ResidentKb() - RssBefore);
    SeenGeneration = SpellGeneration - 1; // start the spell afresh
    return true;
}

void ContentDetector::Unload(std::unique_lock<std::mutex>& Lock, bool bKeepTap)
{
    if (!bKeepTap)
    {
        bTapEnabled.store(false, std::memory_order_relaxed);
    }
    std::unique_ptr<ContentClassifier> Old = std::move(Classifier);
    DeviceLabel.clear();
    Lock.unlock();
    Old.reset();
    fprintf(stderr, "[ramkolfxd] auto-detect: classifier unloaded\n");
    Lock.lock();
}

void ContentDetector::StartSpell()
{
    SeenGeneration = SpellGeneration;
    PausedSinceMs = -1;
    History.Clear();
    Silence.Reset();
    ChunkFill = 0;
    NextRunAtMs = 0;
    // Whatever the tap still holds belongs to before this spell.
    while (TapRing->Pop(Drained.data(), Drained.size()) > 0)
    {
    }
    bTapEnabled.store(true, std::memory_order_release);
}

void ContentDetector::Step(std::unique_lock<std::mutex>& Lock)
{
    const bool bSettled = bSettledFlag.load(std::memory_order_relaxed);
    const bool bPaused = bPausedFlag.load(std::memory_order_relaxed);
    if (bCadenceChanged)
    {
        bCadenceChanged = false;
        NextRunAtMs = 0; // the next run was scheduled at the old cadence
    }
    Lock.unlock();

    // Paused, no inference runs, so the battery (which paces it) can wait.
    const int64_t Now = NowMs();
    if (Tuning.bWatchBattery && !bPaused && (BatteryCheckedAtMs < 0 || Now - BatteryCheckedAtMs >= 60000))
    {
        bOnBattery.store(IsOnBattery(), std::memory_order_relaxed);
        BatteryCheckedAtMs = Now;
    }
    const bool bBattery = bOnBattery.load(std::memory_order_relaxed);

    // Drain the tap in fixed chunks: silence, track boundaries, history.
    // A boundary is noted as how many samples were drained before its
    // chunk began.
    std::vector<int64_t> BoundaryAt;
    int64_t Popped = 0;
    size_t Count = 0;
    while ((Count = TapRing->Pop(Drained.data(), Drained.size())) > 0)
    {
        size_t Done = 0;
        while (Done < Count)
        {
            const size_t Take = std::min(Count - Done, Chunk.size() - ChunkFill);
            std::copy_n(Drained.data() + Done, Take, Chunk.data() + ChunkFill);
            ChunkFill += Take;
            Done += Take;
            if (ChunkFill == Chunk.size())
            {
                const SilenceTracker::Result R = Silence.AddChunk(Rms(Chunk), Tuning.ChunkMs);
                if (Classifier) // kept while paused, so a re-check can start on what played since
                {
                    History.Append(Chunk);
                }
                ChunkFill = 0;
                if (R.bBoundary)
                {
                    BoundaryAt.push_back(Popped + static_cast<int64_t>(Done) - static_cast<int64_t>(Chunk.size()));
                }
            }
        }
        Popped += static_cast<int64_t>(Count);
    }
    // The tap is drained up to now (read after the drain, so no sample
    // counted is newer than it), so a boundary's place in what was drained
    // says when it was heard - which can be most of a poll interval before
    // the worker got to it. AutoDetectSession tells a gap from before a
    // hold from one after it by this. Popped includes the samples already
    // in the next, partial chunk.
    if (!BoundaryAt.empty())
    {
        const int64_t DrainedAt = NowMs();
        for (int64_t& At : BoundaryAt)
        {
            At = DrainedAt - (Popped - At) * 1000 / static_cast<int64_t>(Tuning.InputRate);
        }
    }

    // Inference, if it's time and the whole window is sound.
    bool bRan = false;
    bool bFailed = false;
    int64_t CpuUs = 0;
    ClassScores Folded{};
    std::string Error;
    if (!bPaused && Classifier && Now >= NextRunAtMs &&
        Silence.AudibleMs() >= static_cast<int64_t>(ModelInput.size() * 1000 / Classifier->SampleRate()) &&
        History.CopyLatest(Window))
    {
        const int64_t CpuStart = ThreadCpuTimeUs();
        ToModelRate->Process(Window, ModelInput);
        if (Classifier->Classify(ModelInput, RawScores, Error))
        {
            Folded = Labels.Fold(RawScores);
            CpuUs = ThreadCpuTimeUs() - CpuStart;
            bRan = true;
            if (!DumpDir.empty())
            {
                DumpRun(DumpDir, DumpRuns++, ModelInput, Classifier->SampleRate(), RawScores, Classifier->Labels(),
                        Folded);
            }
        }
        else
        {
            bFailed = true;
        }
    }

    Lock.lock();
    // A new spell began while unlocked (SetActive): what was drained and
    // classified belongs to the old one, and AutoDetectSession has already
    // started afresh - don't hand it the old spell's audio.
    const bool bStaleSpell = SeenGeneration != SpellGeneration;
    if (!bStaleSpell)
    {
        for (int64_t At : BoundaryAt)
        {
            PushEvent({At, true, {}});
        }
    }
    if (bFailed)
    {
        fprintf(stderr, "[ramkolfxd] auto-detect: inference on %s failed: %s\n", DeviceLabel.c_str(),
                Error.c_str());
        const bool bWasCpu = Classifier->IsCpu();
        Unload(Lock);
        if (!bWasCpu)
        {
            LoadClassifier(Lock, InferenceDevice::Cpu); // device lost, driver reset: carry on on the CPU
        }
        else
        {
            CurrentAvailability = AutoDetectAvailability::Failed;
            bVerdictFromLoad = true;
        }
        return;
    }
    if (!bRan)
    {
        return;
    }

    Scheduler.OnRun(CpuUs);
    if (Scheduler.GaveUp())
    {
        fprintf(stderr,
                "[ramkolfxd] auto-detect: stopped - inference takes %.1f ms on %s, too slow to stay under %.0f%% "
                "of a core\n",
                static_cast<double>(Scheduler.PercentileUs(0.5f)) / 1000.0, DeviceLabel.c_str(),
                static_cast<double>(Scheduler.GetTuning().MaxDutyCycle) * 100.0);
        CurrentAvailability = AutoDetectAvailability::TooSlow;
        bVerdictFromLoad = true;
        Unload(Lock);
        return;
    }
    if (!bStaleSpell)
    {
        PushEvent({Now, false, Folded});
    }

    const int64_t Interval = Scheduler.NextIntervalMs(bSettled, bBattery);
    NextRunAtMs = Now + Interval;
    const SchedulerTuning& Cadence = Scheduler.GetTuning();
    const int64_t Base = bSettled || bBattery ? Cadence.SlowIntervalMs : Cadence.FastIntervalMs;
    const bool bStretching = Interval > Base;
    if (bStretching != bGovernorStretching)
    {
        bGovernorStretching = bStretching;
        if (bStretching)
        {
            fprintf(stderr, "[ramkolfxd] auto-detect: %.1f ms per run, slowing to one run every %.1f s\n",
                    static_cast<double>(CpuUs) / 1000.0, static_cast<double>(Interval) / 1000.0);
        }
        else
        {
            fprintf(stderr, "[ramkolfxd] auto-detect: back to the normal cadence\n");
        }
    }
}

} // namespace ramkolfx
