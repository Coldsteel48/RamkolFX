// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "detect/content_classifier.hpp"

namespace ramkolfx
{

// ContentClassifier on ONNX Runtime, which is dlopened rather than linked
// (docs/content-detect-plan.md): builds need no ORT, and the daemon runs
// without it, with Auto-detect unavailable. RAMKOLFX_ORT_LIB overrides
// the library path; the build can fix one with -DRAMKOLFX_ORT_LIBRARY.
//
// The model is self-describing, so the exported model can change without
// a code change. It takes one float input - a mono waveform, shape [N] or
// [1, N] - and its first output is one score per label. Custom metadata
// (ONNX metadata_props) says the rest:
//   ramkolfx.sample_rate    input rate in Hz (required)
//   ramkolfx.labels         label names, '\n'-separated, in output order (required)
//   ramkolfx.input_samples  N, if the input shape leaves it dynamic
//   ramkolfx.output         "probabilities" (default) or "logits" (sigmoid applied here)

struct ClassifierModelPaths
{
    std::string Model;    // fp32, for the CPU and GPUs
    std::string NpuModel; // static-shape int8; optional (NPUs then get Model)
};

// <model dir>/audio_classifier.onnx and audio_classifier.int8.onnx. The
// directory is RAMKOLFX_MODEL_DIR from the environment if set, else the
// build's configured one.
ClassifierModelPaths DefaultClassifierModelPaths();

// The backend ContentDetector runs on. Probe only looks for files. Load
// picks the execution provider for the device policy (Auto: an NPU, else
// the CPU; GPU only when asked), keeps an accelerator only if it agrees
// with the CPU and beats it on a test signal, and otherwise runs on the
// CPU. Sessions run single-threaded on the caller, without spin-waiting
// or a memory arena.
ClassifierBackend MakeOnnxClassifierBackend(ClassifierModelPaths Paths);

// Loads one model on one specific execution provider, skipping device
// policy and validation - for tools and tests. Provider: "cpu", "cuda",
// "openvino-gpu", "openvino-npu".
ClassifierLoadResult LoadOnnxClassifier(const std::string& ModelPath, const std::string& Provider);

// ONNX Runtime libraries (libonnxruntime.so, or .so.<version> - Debian
// ships only libonnxruntime.so.1.21) in Dirs, as full paths, and in the
// dynamic linker's cache (the contents of /etc/ld.so.cache), as names;
// newest version first. For the loader and its tests.
std::vector<std::string> FindOnnxRuntimeLibraries(const std::vector<std::string>& Dirs, std::string_view LdSoCache);

// The execution providers the loaded runtime offers (e.g.
// "CPUExecutionProvider"), for tools; empty if it doesn't load.
std::vector<std::string> OnnxAvailableProviders();

} // namespace ramkolfx
