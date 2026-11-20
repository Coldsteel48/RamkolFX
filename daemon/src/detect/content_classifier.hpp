// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ramkolfx/protocol.hpp"

namespace ramkolfx
{

// The audio classifier behind Auto-detect (docs/content-detect-plan.md),
// swappable like DspStage: the ONNX Runtime one in the daemon, fakes in
// the tests. It takes a mono waveform and returns one probability per
// label (AudioSet's display names), which LabelMap folds into classes.
// Used from one thread at a time.
class ContentClassifier
{
public:
    virtual ~ContentClassifier() = default;

    // Classify() takes exactly InputSamples() samples at SampleRate().
    virtual uint32_t SampleRate() const = 0;
    virtual size_t InputSamples() const = 0;

    // One name per score, in output order.
    virtual const std::vector<std::string>& Labels() const = 0;

    // Fills OutScores with one probability per label. On failure returns
    // false and says why in OutError.
    virtual bool Classify(std::span<const float> Waveform, std::vector<float>& OutScores, std::string& OutError) = 0;

    // What it runs on, for the GUI and logs: "CPU", "NPU (OpenVINO)".
    virtual std::string DeviceLabel() const = 0;
    // Runs on the CPU (as opposed to an accelerator, which is replaced by
    // the CPU if it fails at runtime).
    virtual bool IsCpu() const = 0;
};

struct ClassifierLoadResult
{
    std::unique_ptr<ContentClassifier> Classifier; // null when unavailable
    AutoDetectAvailability Availability = AutoDetectAvailability::Ok;
    // Why it's unavailable, or which device was picked and why - for the log.
    std::string Message;
};

// What ContentDetector needs from a classifier implementation.
struct ClassifierBackend
{
    // Cheap check for the GUI's enabled/disabled state: do the runtime
    // and model files exist? Must not load anything.
    std::function<AutoDetectAvailability()> Probe;
    // Builds a classifier for the device policy. Slow (model load,
    // accelerator validation); called on the detection worker only.
    std::function<ClassifierLoadResult(InferenceDevice Device)> Load;
};

// CPU time used by the calling thread - what inference costs the
// machine, unlike wall time, which also counts time spent preempted.
inline int64_t ThreadCpuTimeUs()
{
    timespec Ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &Ts);
    return static_cast<int64_t>(Ts.tv_sec) * 1000000 + Ts.tv_nsec / 1000;
}

} // namespace ramkolfx
