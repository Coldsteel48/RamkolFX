// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// ramkolfx-detect-bench: measures Auto-detect against the performance
// budget in docs/content-detect-plan.md, on this machine. Not installed;
// run it on the reference old and new machines before calling a
// detection change done (CI timing is too noisy to gate on).
//
//   ramkolfx-detect-bench [--model PATH] [--device auto|cpu|gpu|npu]
//                         [--provider cpu|cuda|openvino-gpu|openvino-npu]
//                         [--wav FILE] [--runs N]
//
// --device goes through the daemon's selection and validation; --provider
// forces one execution provider. --wav classifies a file window by window
// (and prints what the smoother makes of it) instead of a test signal.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "content_detection.hpp"
#include "detect/content_detector.hpp"
#include "detect/onnx_classifier.hpp"
#include "detect/wav_file.hpp"

using namespace ramkolfx;

namespace
{
// Budget (docs/content-detect-plan.md, "Performance budget").
constexpr double TapBudgetPercent = 1.0;      // of the block's duration
constexpr double DutyCapPercent = 1.0;       // of one core, enforced
constexpr double DutyTargetPercent = 0.5;    // average, on the old machine
constexpr double RssBudgetMb = 60.0;
constexpr uint32_t Rate = 48000;

int64_t NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
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

const char* Verdict(bool bOk)
{
    return bOk ? "ok" : "OVER BUDGET";
}

template <typename T>
T Percentile(std::vector<T> Values, double Fraction)
{
    if (Values.empty())
    {
        return T{};
    }
    const auto Index = static_cast<size_t>(Fraction * static_cast<double>(Values.size() - 1));
    std::nth_element(Values.begin(), Values.begin() + static_cast<ptrdiff_t>(Index), Values.end());
    return Values[Index];
}

// A classifier that's instantly ready, so the tap can be measured without
// a model.
class NullClassifier final : public ContentClassifier
{
public:
    uint32_t SampleRate() const override
    {
        return 16000;
    }
    size_t InputSamples() const override
    {
        return 16000;
    }
    const std::vector<std::string>& Labels() const override
    {
        return Names;
    }
    bool Classify(std::span<const float>, std::vector<float>& Out, std::string&) override
    {
        Out.assign(Names.size(), 0.0f);
        return true;
    }
    std::string DeviceLabel() const override
    {
        return "none";
    }
    bool IsCpu() const override
    {
        return true;
    }

private:
    std::vector<std::string> Names{"Speech", "Music"};
};

void BenchTap()
{
    std::printf("RT tap (budget: < %.0f%% of the block's duration)\n", TapBudgetPercent);
    ClassifierBackend Backend;
    Backend.Load = [](InferenceDevice) {
        ClassifierLoadResult Result;
        Result.Classifier = std::make_unique<NullClassifier>();
        Result.Message = "null classifier (tap benchmark)";
        return Result;
    };
    ContentDetectorTuning Tuning;
    Tuning.PollIntervalMs = 20; // drain fast enough that the ring never fills
    Tuning.bWatchBattery = false;

    for (const uint32 Frames : {64u, 256u, 1024u})
    {
        std::vector<float> Block(static_cast<size_t>(Frames) * 8);
        for (size_t i = 0; i < Block.size(); ++i)
        {
            Block[i] = 0.25f * std::sin(static_cast<float>(i) * 0.01f);
        }
        const double BlockNs = 1e9 * Frames / Rate;

        for (const bool bOn : {false, true})
        {
            ContentDetector Detector(Backend, Tuning);
            if (bOn)
            {
                Detector.SetActive(true, 1);
                while (!Detector.Stats().bLoaded)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            // In bursts of ~1 s of audio, letting the worker drain between
            // them as it would in real time.
            std::vector<double> PerBlockNs;
            const uint32 BlocksPerBurst = Rate / Frames;
            for (int Burst = 0; Burst < 5; ++Burst)
            {
                for (uint32 b = 0; b < BlocksPerBurst; ++b)
                {
                    const int64_t Start = NowNs();
                    Detector.Tap(Block.data(), Frames);
                    PerBlockNs.push_back(static_cast<double>(NowNs() - Start));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
            const double P50 = Percentile(PerBlockNs, 0.5);
            const double P99 = Percentile(PerBlockNs, 0.99);
            const double Percent = 100.0 * P99 / BlockNs;
            std::printf("  %4u frames, tap %-3s: p50 %8.0f ns, p99 %8.0f ns = %.4f%% of the block  %s\n", Frames,
                        bOn ? "on" : "off", P50, P99, Percent, Verdict(Percent < TapBudgetPercent));
            if (bOn && Detector.Stats().TapDroppedSamples > 0)
            {
                std::printf("  (the worker fell behind: %lld samples dropped)\n",
                            static_cast<long long>(Detector.Stats().TapDroppedSamples));
            }
        }
    }
}

// What the worker does per run: resample a 48 kHz window, classify, fold.
struct RunResult
{
    int64_t WallUs = 0;
    int64_t CpuUs = 0;
};

int BenchInference(const std::string& ModelPath, const std::string& Device, const std::string& ProviderKey,
                   const std::string& WavPath, int Runs)
{
    std::printf("\nInference (budget: <= %.0f%% of a core, enforced; target %.1f%% average; <= %.0f MB RSS)\n",
                DutyCapPercent, DutyTargetPercent, RssBudgetMb);
    const std::vector<std::string> Providers = OnnxAvailableProviders();
    if (Providers.empty())
    {
        std::printf("  ONNX Runtime not found - set RAMKOLFX_ORT_LIB or install it\n");
        return 1;
    }
    std::printf("  providers in this runtime:");
    for (const std::string& P : Providers)
    {
        std::printf(" %s", P.c_str());
    }
    std::printf("\n");

    const int64_t RssBefore = ResidentKb();
    const int64_t LoadStart = NowUs();
    ClassifierLoadResult Loaded;
    if (!ProviderKey.empty())
    {
        Loaded = LoadOnnxClassifier(ModelPath, ProviderKey);
    }
    else
    {
        ClassifierModelPaths Paths = DefaultClassifierModelPaths();
        Paths.Model = ModelPath;
        const auto Policy = InferenceDeviceFromKey(Device);
        if (!Policy)
        {
            std::printf("  unknown --device '%s'\n", Device.c_str());
            return 2;
        }
        Loaded = MakeOnnxClassifierBackend(Paths).Load(*Policy);
    }
    const double LoadMs = static_cast<double>(NowUs() - LoadStart) / 1000.0;
    if (!Loaded.Classifier)
    {
        std::printf("  load failed: %s\n", Loaded.Message.c_str());
        return 1;
    }
    ContentClassifier& Classifier = *Loaded.Classifier;
    const double RssMb = static_cast<double>(ResidentKb() - RssBefore) / 1024.0;
    std::printf("  %s; loaded in %.0f ms; %u Hz, %zu samples (%.2f s) per window, %zu labels\n",
                Loaded.Message.c_str(), LoadMs, Classifier.SampleRate(), Classifier.InputSamples(),
                static_cast<double>(Classifier.InputSamples()) / Classifier.SampleRate(), Classifier.Labels().size());
    std::vector<std::string> Missing;
    const LabelMap Map = LabelMap::Build(Classifier.Labels(), &Missing);
    if (!Missing.empty())
    {
        std::printf("  %zu LabelMap labels not in the model:", Missing.size());
        for (const std::string& M : Missing)
        {
            std::printf(" '%s'", M.c_str());
        }
        std::printf("\n");
    }

    // Input at 48 kHz, as the tap delivers it.
    std::vector<float> Audio;
    if (!WavPath.empty())
    {
        MonoWav Wav;
        std::string Error;
        if (!ReadWavMono(WavPath, Wav, Error))
        {
            std::printf("  %s\n", Error.c_str());
            return 1;
        }
        const Resampler To48k(Wav.SampleRate, Rate);
        Audio.resize(Wav.Samples.size() * Rate / Wav.SampleRate);
        To48k.Process(Wav.Samples, Audio);
    }
    const Resampler ToModel(Rate, Classifier.SampleRate());
    std::vector<float> Window(ToModel.InputFor(Classifier.InputSamples()));
    std::vector<float> ModelInput(Classifier.InputSamples());
    if (Audio.size() < Window.size())
    {
        uint32_t Seed = 1;
        Audio.resize(Window.size() * 4);
        for (size_t i = 0; i < Audio.size(); ++i)
        {
            Seed = Seed * 1664525u + 1013904223u;
            Audio[i] = 0.05f * (static_cast<float>(Seed >> 8) / 16777216.0f - 0.5f) +
                       0.3f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / Rate);
        }
    }

    // Window positions: every 2 s through a WAV (the fast cadence), or
    // round-robin through the test signal.
    const size_t StepSamples = WavPath.empty() ? Window.size() / 2 : 2 * Rate;
    const size_t Positions = (Audio.size() - Window.size()) / StepSamples + 1;
    if (!WavPath.empty())
    {
        Runs = static_cast<int>(Positions);
    }

    DetectionSmoother Smoother;
    std::vector<int64_t> Cpu;
    std::vector<int64_t> Wall;
    std::vector<float> Scores;
    std::string Error;
    for (int Run = -2; Run < Runs; ++Run) // two warm-up runs
    {
        const size_t Start = (static_cast<size_t>(std::max(Run, 0)) % Positions) * StepSamples;
        std::copy_n(Audio.data() + Start, Window.size(), Window.data());
        const int64_t WallStart = NowUs();
        const int64_t CpuStart = ThreadCpuTimeUs();
        ToModel.Process(Window, ModelInput);
        if (!Classifier.Classify(ModelInput, Scores, Error))
        {
            std::printf("  run failed: %s\n", Error.c_str());
            return 1;
        }
        const ClassScores Folded = Map.Fold(Scores);
        if (Run < 0)
        {
            continue;
        }
        Cpu.push_back(ThreadCpuTimeUs() - CpuStart);
        Wall.push_back(NowUs() - WallStart);

        if (!WavPath.empty())
        {
            const auto AtMs = static_cast<int64_t>(Start * 1000 / Rate);
            Smoother.AddScores(Folded, AtMs);
            size_t Best = 1;
            for (size_t c = 1; c < DetectedClassCount; ++c)
            {
                Best = Folded[c] > Folded[Best] ? c : Best;
            }
            std::printf("  %6.1f s  speech %.2f music %.2f film %.2f  top %-10s (%.2f)  -> %s\n",
                        static_cast<double>(AtMs) / 1000.0,
                        static_cast<double>(ScoreOf(Folded, DetectedClass::Speech)),
                        static_cast<double>(ScoreOf(Folded, DetectedClass::Music)),
                        static_cast<double>(ScoreOf(Folded, DetectedClass::Movie)),
                        DetectedClassLabel(static_cast<DetectedClass>(Best)), static_cast<double>(Folded[Best]),
                        DetectedClassLabel(Smoother.Current()));
        }
    }

    const double P50Cpu = static_cast<double>(Percentile(Cpu, 0.5)) / 1000.0;
    const double P95Cpu = static_cast<double>(Percentile(Cpu, 0.95)) / 1000.0;
    const double P50Wall = static_cast<double>(Percentile(Wall, 0.5)) / 1000.0;
    const SchedulerTuning Cadence;
    const double FastDuty = 100.0 * P50Cpu / static_cast<double>(Cadence.FastIntervalMs);
    const double SlowDuty = 100.0 * P50Cpu / static_cast<double>(Cadence.SlowIntervalMs);
    std::printf("  %zu runs: CPU p50 %.2f ms, p95 %.2f ms; wall p50 %.2f ms\n", Cpu.size(), P50Cpu, P95Cpu, P50Wall);
    std::printf("  duty cycle: %.3f%% at the fast cadence (%lld s), %.3f%% settled (%lld s)  %s\n", FastDuty,
                static_cast<long long>(Cadence.FastIntervalMs / 1000), SlowDuty,
                static_cast<long long>(Cadence.SlowIntervalMs / 1000),
                FastDuty <= DutyCapPercent
                    ? (SlowDuty <= DutyTargetPercent ? "ok" : "ok under the cap, over the average target")
                    : "OVER THE CAP - the governor would stretch the interval");
    std::printf("  RSS added by the loaded session: %.1f MB  %s\n", RssMb, Verdict(RssMb <= RssBudgetMb));
    return 0;
}

void Usage()
{
    std::printf("usage: ramkolfx-detect-bench [--model PATH] [--device auto|cpu|gpu|npu]\n"
                "                             [--provider cpu|cuda|openvino-gpu|openvino-npu]\n"
                "                             [--wav FILE] [--runs N] [--tap-only]\n");
}
} // namespace

int main(int Argc, char** Argv)
{
    std::string Model = DefaultClassifierModelPaths().Model;
    std::string Device = "auto";
    std::string Provider;
    std::string Wav;
    int Runs = 50;
    bool bTapOnly = false;
    for (int i = 1; i < Argc; ++i)
    {
        const std::string Arg = Argv[i];
        const bool bHasValue = i + 1 < Argc;
        if (Arg == "--model" && bHasValue)
        {
            Model = Argv[++i];
        }
        else if (Arg == "--device" && bHasValue)
        {
            Device = Argv[++i];
        }
        else if (Arg == "--provider" && bHasValue)
        {
            Provider = Argv[++i];
        }
        else if (Arg == "--wav" && bHasValue)
        {
            Wav = Argv[++i];
        }
        else if (Arg == "--runs" && bHasValue)
        {
            Runs = std::max(1, std::atoi(Argv[++i]));
        }
        else if (Arg == "--tap-only")
        {
            bTapOnly = true;
        }
        else
        {
            Usage();
            return Arg == "--help" || Arg == "-h" ? 0 : 2;
        }
    }

    BenchTap();
    return bTapOnly ? 0 : BenchInference(Model, Device, Provider, Wav, Runs);
}
