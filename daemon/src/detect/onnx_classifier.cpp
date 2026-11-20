// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "detect/onnx_classifier.hpp"

#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>

#include "onnxruntime_c_api.h"

#ifndef RAMKOLFX_MODEL_DIR
#define RAMKOLFX_MODEL_DIR "/usr/share/ramkolfx/models"
#endif

namespace ramkolfx
{

namespace
{

// The ORT API version this file is written against. The vendored header
// is 1.21's, but everything called here exists since 1.17 (OpenVINO_V2
// being the newest), and requesting 17 lets any runtime from 1.17 on load
// us - newer runtimes serve older API versions unchanged.
constexpr uint32_t RequestedOrtApiVersion = 17;

bool FileExists(const std::string& Path)
{
    struct stat St{};
    return !Path.empty() && stat(Path.c_str(), &St) == 0 && S_ISREG(St.st_mode);
}

// --- The runtime: dlopened once, kept for the process' lifetime -------------
//
// Sessions are released when idle (that's the memory that matters); the
// library itself stays mapped once loaded. Its code pages are file-backed
// and cost nothing the kernel can't reclaim, while dlclose()ing a C++
// runtime with static state (protobuf, abseil, ORT's own registries) is a
// known source of crashes at exit.

struct OrtRuntime
{
    const OrtApi* Api = nullptr;
    OrtEnv* Env = nullptr;
};

std::mutex RuntimeMutex;
OrtRuntime Runtime;      // valid once Api is set
std::string RuntimeError; // last load failure

const char* LibraryOverride()
{
    const char* Path = std::getenv("RAMKOLFX_ORT_LIB");
    return Path && *Path ? Path : nullptr;
}

// Takes ownership of S; true if it's null (success).
bool Ok(OrtStatus* S, std::string& OutError, const char* What)
{
    if (!S)
    {
        return true;
    }
    OutError = std::string(What) + ": " + Runtime.Api->GetErrorMessage(S);
    Runtime.Api->ReleaseStatus(S);
    return false;
}

const OrtRuntime* GetRuntime(std::string& OutError)
{
    std::lock_guard Lock(RuntimeMutex);
    if (Runtime.Api)
    {
        return &Runtime;
    }

    std::vector<std::string> Candidates;
    if (const char* Override = LibraryOverride())
    {
        Candidates.emplace_back(Override);
    }
    else
    {
        Candidates = {"libonnxruntime.so.1", "libonnxruntime.so"};
    }
    void* Handle = nullptr;
    std::string Errors;
    for (const std::string& Name : Candidates)
    {
        Handle = dlopen(Name.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (Handle)
        {
            break;
        }
        const char* Err = dlerror();
        Errors += (Errors.empty() ? "" : "; ") + std::string(Err ? Err : Name);
    }
    if (!Handle)
    {
        OutError = "ONNX Runtime not found (" + Errors + ")";
        return nullptr;
    }

    using GetApiBaseFn = const OrtApiBase* (*)();
    const auto GetApiBase = reinterpret_cast<GetApiBaseFn>(dlsym(Handle, "OrtGetApiBase"));
    const OrtApiBase* Base = GetApiBase ? GetApiBase() : nullptr;
    const OrtApi* Api = Base ? Base->GetApi(RequestedOrtApiVersion) : nullptr;
    if (!Api)
    {
        OutError = std::string("ONNX Runtime ") + (Base ? Base->GetVersionString() : "(unknown version)") +
                   " is too old: 1.17 or newer is needed";
        dlclose(Handle);
        return nullptr;
    }

    Runtime.Api = Api;
    OrtEnv* Env = nullptr;
    if (!Ok(Api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ramkolfx", &Env), OutError, "CreateEnv"))
    {
        Runtime.Api = nullptr;
        dlclose(Handle);
        return nullptr;
    }
    Runtime.Env = Env;
    fprintf(stderr, "[ramkolfxd] auto-detect: loaded ONNX Runtime %s\n", Base->GetVersionString());
    return &Runtime; // Handle deliberately never closed, see above
}

// Is libonnxruntime findable without loading it? Checks the override,
// LD_LIBRARY_PATH, the dynamic linker's cache and the usual directories.
bool RuntimeLooksInstalled()
{
    {
        std::lock_guard Lock(RuntimeMutex);
        if (Runtime.Api)
        {
            return true;
        }
    }
    if (const char* Override = LibraryOverride())
    {
        return FileExists(Override);
    }
    std::vector<std::string> Dirs;
    if (const char* LdPath = std::getenv("LD_LIBRARY_PATH"))
    {
        std::stringstream Stream(LdPath);
        std::string Dir;
        while (std::getline(Stream, Dir, ':'))
        {
            if (!Dir.empty())
            {
                Dirs.push_back(Dir);
            }
        }
    }
    for (const char* Dir : {"/usr/lib", "/usr/lib64", "/usr/local/lib", "/usr/local/lib64", "/lib", "/lib64",
                            "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib/riscv64-linux-gnu"})
    {
        Dirs.emplace_back(Dir);
    }
    for (const std::string& Dir : Dirs)
    {
        if (FileExists(Dir + "/libonnxruntime.so.1") || FileExists(Dir + "/libonnxruntime.so"))
        {
            return true;
        }
    }
    // ld.so.cache lists every library ldconfig knows, by name.
    std::ifstream Cache("/etc/ld.so.cache", std::ios::binary);
    const std::string Contents((std::istreambuf_iterator<char>(Cache)), std::istreambuf_iterator<char>());
    return Contents.find("libonnxruntime.so") != std::string::npos;
}

// --- RAII for ORT objects -----------------------------------------------------

#define RAMKOLFX_ORT_DELETER(Type)                                                                                     \
    struct Type##Deleter                                                                                               \
    {                                                                                                                  \
        void operator()(Ort##Type* P) const                                                                            \
        {                                                                                                              \
            if (P)                                                                                                     \
            {                                                                                                          \
                Runtime.Api->Release##Type(P);                                                                         \
            }                                                                                                          \
        }                                                                                                              \
    };                                                                                                                 \
    using Unique##Type = std::unique_ptr<Ort##Type, Type##Deleter>;

RAMKOLFX_ORT_DELETER(Session)
RAMKOLFX_ORT_DELETER(SessionOptions)
RAMKOLFX_ORT_DELETER(MemoryInfo)
RAMKOLFX_ORT_DELETER(Value)
RAMKOLFX_ORT_DELETER(TypeInfo)
RAMKOLFX_ORT_DELETER(ModelMetadata)
RAMKOLFX_ORT_DELETER(TensorTypeAndShapeInfo)
#undef RAMKOLFX_ORT_DELETER

struct CudaOptionsDeleter
{
    void operator()(OrtCUDAProviderOptionsV2* P) const
    {
        if (P)
        {
            Runtime.Api->ReleaseCUDAProviderOptions(P);
        }
    }
};
using UniqueCudaOptions = std::unique_ptr<OrtCUDAProviderOptionsV2, CudaOptionsDeleter>;

// For calls that only fail on a bug (freeing what ORT itself allocated).
void IgnoreStatus(OrtStatus* S)
{
    if (S)
    {
        Runtime.Api->ReleaseStatus(S);
    }
}

// A string ORT allocated with the default allocator.
std::string TakeOrtString(OrtAllocator* Allocator, char* Value)
{
    std::string Out = Value ? Value : "";
    if (Value)
    {
        IgnoreStatus(Runtime.Api->AllocatorFree(Allocator, Value));
    }
    return Out;
}

// --- Execution providers ----------------------------------------------------

enum class Provider
{
    Cpu,
    Cuda,
    OpenVinoGpu,
    OpenVinoNpu,
};

struct ProviderInfo
{
    Provider Id;
    const char* Key;     // LoadOnnxClassifier's name for it
    const char* OrtName; // as GetAvailableProviders lists it
    const char* Label;   // for the GUI
};

constexpr ProviderInfo Providers[] = {
    {Provider::Cpu, "cpu", "CPUExecutionProvider", "CPU"},
    {Provider::Cuda, "cuda", "CUDAExecutionProvider", "GPU (CUDA)"},
    {Provider::OpenVinoGpu, "openvino-gpu", "OpenVINOExecutionProvider", "GPU (OpenVINO)"},
    {Provider::OpenVinoNpu, "openvino-npu", "OpenVINOExecutionProvider", "NPU (OpenVINO)"},
};

const ProviderInfo& InfoOf(Provider Id)
{
    for (const ProviderInfo& Info : Providers)
    {
        if (Info.Id == Id)
        {
            return Info;
        }
    }
    return Providers[0];
}

std::set<std::string> AvailableProviders()
{
    std::set<std::string> Out;
    char** Names = nullptr;
    int Count = 0;
    std::string Error;
    if (Ok(Runtime.Api->GetAvailableProviders(&Names, &Count), Error, "GetAvailableProviders"))
    {
        for (int i = 0; i < Count; ++i)
        {
            Out.insert(Names[i]);
        }
        IgnoreStatus(Runtime.Api->ReleaseAvailableProviders(Names, Count));
    }
    return Out;
}

bool AppendProvider(OrtSessionOptions* Options, Provider Id, std::string& OutError)
{
    const OrtApi& Api = *Runtime.Api;
    switch (Id)
    {
    case Provider::Cpu:
        return true;
    case Provider::Cuda:
    {
        OrtCUDAProviderOptionsV2* Raw = nullptr;
        if (!Ok(Api.CreateCUDAProviderOptions(&Raw), OutError, "CUDA options"))
        {
            return false;
        }
        const UniqueCudaOptions CudaOptions(Raw);
        return Ok(Api.SessionOptionsAppendExecutionProvider_CUDA_V2(Options, CudaOptions.get()), OutError, "CUDA");
    }
    case Provider::OpenVinoGpu:
    case Provider::OpenVinoNpu:
    {
        const char* Keys[] = {"device_type"};
        const char* Values[] = {Id == Provider::OpenVinoNpu ? "NPU" : "GPU"};
        return Ok(Api.SessionOptionsAppendExecutionProvider_OpenVINO_V2(Options, Keys, Values, 1), OutError,
                  "OpenVINO");
    }
    }
    return false;
}

// --- The classifier -----------------------------------------------------------

class OnnxClassifier final : public ContentClassifier
{
public:
    static std::unique_ptr<OnnxClassifier> Load(const std::string& Path, Provider Id, std::string& OutError);

    uint32_t SampleRate() const override
    {
        return Rate;
    }
    size_t InputSamples() const override
    {
        return Samples;
    }
    const std::vector<std::string>& Labels() const override
    {
        return LabelNames;
    }
    std::string DeviceLabel() const override
    {
        return InfoOf(Id).Label;
    }
    bool IsCpu() const override
    {
        return Id == Provider::Cpu;
    }

    bool Classify(std::span<const float> Waveform, std::vector<float>& OutScores, std::string& OutError) override;

private:
    Provider Id = Provider::Cpu;
    UniqueSession Session;
    UniqueMemoryInfo MemoryInfo;
    std::string InputName;
    std::string OutputName;
    std::vector<int64_t> InputShape;
    uint32_t Rate = 0;
    size_t Samples = 0;
    std::vector<std::string> LabelNames;
    bool bLogits = false;
};

std::unique_ptr<OnnxClassifier> OnnxClassifier::Load(const std::string& Path, Provider Id, std::string& OutError)
{
    const OrtApi& Api = *Runtime.Api;
    auto Out = std::make_unique<OnnxClassifier>();
    Out->Id = Id;

    // Cheap to keep around: everything on the calling thread (the
    // low-priority worker), no spin-waiting between runs (ORT's default
    // burns idle CPU), no arena that grows and never shrinks.
    OrtSessionOptions* RawOptions = nullptr;
    if (!Ok(Api.CreateSessionOptions(&RawOptions), OutError, "session options"))
    {
        return nullptr;
    }
    const UniqueSessionOptions Options(RawOptions);
    if (!Ok(Api.SetIntraOpNumThreads(Options.get(), 1), OutError, "intra-op threads") ||
        !Ok(Api.SetInterOpNumThreads(Options.get(), 1), OutError, "inter-op threads") ||
        !Ok(Api.SetSessionExecutionMode(Options.get(), ORT_SEQUENTIAL), OutError, "execution mode") ||
        !Ok(Api.SetSessionGraphOptimizationLevel(Options.get(), ORT_ENABLE_ALL), OutError, "optimization") ||
        !Ok(Api.DisableCpuMemArena(Options.get()), OutError, "memory arena") ||
        !Ok(Api.AddSessionConfigEntry(Options.get(), "session.intra_op.allow_spinning", "0"), OutError, "spinning") ||
        !Ok(Api.AddSessionConfigEntry(Options.get(), "session.inter_op.allow_spinning", "0"), OutError, "spinning") ||
        !AppendProvider(Options.get(), Id, OutError))
    {
        return nullptr;
    }

    OrtSession* RawSession = nullptr;
    if (!Ok(Api.CreateSession(Runtime.Env, Path.c_str(), Options.get(), &RawSession), OutError, Path.c_str()))
    {
        return nullptr;
    }
    Out->Session.reset(RawSession);

    OrtAllocator* Allocator = nullptr;
    if (!Ok(Api.GetAllocatorWithDefaultOptions(&Allocator), OutError, "allocator"))
    {
        return nullptr;
    }

    // Metadata.
    OrtModelMetadata* RawMeta = nullptr;
    if (!Ok(Api.SessionGetModelMetadata(Out->Session.get(), &RawMeta), OutError, "metadata"))
    {
        return nullptr;
    }
    const UniqueModelMetadata Meta(RawMeta);
    const auto Lookup = [&](const char* Key) {
        char* Value = nullptr;
        std::string Ignored;
        Ok(Api.ModelMetadataLookupCustomMetadataMap(Meta.get(), Allocator, Key, &Value), Ignored, Key);
        return TakeOrtString(Allocator, Value);
    };
    Out->Rate = static_cast<uint32_t>(std::strtoul(Lookup("ramkolfx.sample_rate").c_str(), nullptr, 10));
    {
        std::stringstream Stream(Lookup("ramkolfx.labels"));
        std::string Label;
        while (std::getline(Stream, Label))
        {
            Out->LabelNames.push_back(Label);
        }
    }
    Out->bLogits = Lookup("ramkolfx.output") == "logits";
    if (Out->Rate == 0 || Out->LabelNames.empty())
    {
        OutError = Path + ": missing ramkolfx.sample_rate / ramkolfx.labels metadata";
        return nullptr;
    }

    // One float waveform in, scores out.
    size_t InputCount = 0;
    size_t OutputCount = 0;
    if (!Ok(Api.SessionGetInputCount(Out->Session.get(), &InputCount), OutError, "inputs") ||
        !Ok(Api.SessionGetOutputCount(Out->Session.get(), &OutputCount), OutError, "outputs"))
    {
        return nullptr;
    }
    if (InputCount != 1 || OutputCount < 1)
    {
        OutError = Path + ": expected one input and at least one output";
        return nullptr;
    }
    char* Name = nullptr;
    if (!Ok(Api.SessionGetInputName(Out->Session.get(), 0, Allocator, &Name), OutError, "input name"))
    {
        return nullptr;
    }
    Out->InputName = TakeOrtString(Allocator, Name);
    if (!Ok(Api.SessionGetOutputName(Out->Session.get(), 0, Allocator, &Name), OutError, "output name"))
    {
        return nullptr;
    }
    Out->OutputName = TakeOrtString(Allocator, Name);

    OrtTypeInfo* RawType = nullptr;
    if (!Ok(Api.SessionGetInputTypeInfo(Out->Session.get(), 0, &RawType), OutError, "input type"))
    {
        return nullptr;
    }
    const UniqueTypeInfo Type(RawType);
    const OrtTensorTypeAndShapeInfo* Tensor = nullptr;
    ONNXTensorElementDataType Element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    size_t Rank = 0;
    if (!Ok(Api.CastTypeInfoToTensorInfo(Type.get(), &Tensor), OutError, "input type") || !Tensor ||
        !Ok(Api.GetTensorElementType(Tensor, &Element), OutError, "input type") ||
        !Ok(Api.GetDimensionsCount(Tensor, &Rank), OutError, "input shape"))
    {
        OutError = OutError.empty() ? Path + ": input isn't a tensor" : OutError;
        return nullptr;
    }
    if (Element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || Rank < 1 || Rank > 2)
    {
        OutError = Path + ": input must be a float waveform, shape [N] or [1, N]";
        return nullptr;
    }
    Out->InputShape.resize(Rank);
    if (!Ok(Api.GetDimensions(Tensor, Out->InputShape.data(), Rank), OutError, "input shape"))
    {
        return nullptr;
    }
    if (Rank == 2)
    {
        Out->InputShape[0] = 1; // batch of one, fixed or dynamic
    }
    int64_t& Length = Out->InputShape.back();
    if (Length <= 0)
    {
        Length = std::strtoll(Lookup("ramkolfx.input_samples").c_str(), nullptr, 10);
    }
    if (Length <= 0)
    {
        OutError = Path + ": input length is dynamic and ramkolfx.input_samples isn't set";
        return nullptr;
    }
    Out->Samples = static_cast<size_t>(Length);

    OrtMemoryInfo* RawMemory = nullptr;
    if (!Ok(Api.CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &RawMemory), OutError, "memory info"))
    {
        return nullptr;
    }
    Out->MemoryInfo.reset(RawMemory);
    return Out;
}

bool OnnxClassifier::Classify(std::span<const float> Waveform, std::vector<float>& OutScores, std::string& OutError)
{
    const OrtApi& Api = *Runtime.Api;
    if (Waveform.size() != Samples)
    {
        OutError = "wrong input length";
        return false;
    }
    OrtValue* RawInput = nullptr;
    // ORT doesn't write to inputs; the C API just has no const overload.
    if (!Ok(Api.CreateTensorWithDataAsOrtValue(MemoryInfo.get(), const_cast<float*>(Waveform.data()),
                                               Waveform.size_bytes(), InputShape.data(), InputShape.size(),
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &RawInput),
            OutError, "input tensor"))
    {
        return false;
    }
    const UniqueValue Input(RawInput);
    const char* InputNames[] = {InputName.c_str()};
    const char* OutputNames[] = {OutputName.c_str()};
    const OrtValue* Inputs[] = {Input.get()};
    OrtValue* RawOutput = nullptr;
    if (!Ok(Api.Run(Session.get(), nullptr, InputNames, Inputs, 1, OutputNames, 1, &RawOutput), OutError, "run"))
    {
        return false;
    }
    const UniqueValue Output(RawOutput);

    OrtTensorTypeAndShapeInfo* RawShape = nullptr;
    size_t Count = 0;
    if (!Ok(Api.GetTensorTypeAndShape(Output.get(), &RawShape), OutError, "output shape"))
    {
        return false;
    }
    const UniqueTensorTypeAndShapeInfo Shape(RawShape);
    ONNXTensorElementDataType Element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    if (!Ok(Api.GetTensorShapeElementCount(Shape.get(), &Count), OutError, "output shape") ||
        !Ok(Api.GetTensorElementType(Shape.get(), &Element), OutError, "output type"))
    {
        return false;
    }
    if (Element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || Count != LabelNames.size())
    {
        OutError = "output has " + std::to_string(Count) + " values for " + std::to_string(LabelNames.size()) +
                   " labels";
        return false;
    }
    float* Data = nullptr;
    if (!Ok(Api.GetTensorMutableData(Output.get(), reinterpret_cast<void**>(&Data)), OutError, "output data"))
    {
        return false;
    }
    OutScores.assign(Data, Data + Count);
    if (bLogits)
    {
        for (float& S : OutScores)
        {
            S = 1.0f / (1.0f + std::exp(-S));
        }
    }
    return true;
}

// --- Device policy and validation -------------------------------------------

struct RunCost
{
    int64_t WallUs = 0;
    int64_t CpuUs = 0;
};

// A fixed test signal (a chord over noise) and the median cost of
// classifying it, after warm-up. False if a run fails.
bool Measure(ContentClassifier& Classifier, std::vector<float>& OutScores, RunCost& OutCost, std::string& OutError)
{
    std::vector<float> Wave(Classifier.InputSamples());
    uint32_t Seed = 12345;
    for (size_t i = 0; i < Wave.size(); ++i)
    {
        Seed = Seed * 1664525u + 1013904223u;
        const float Noise = static_cast<float>(Seed >> 8) / static_cast<float>(1u << 24) - 0.5f;
        const float T = static_cast<float>(i) / static_cast<float>(Classifier.SampleRate());
        Wave[i] = 0.1f * Noise + 0.2f * std::sin(2.0f * 3.14159265f * 220.0f * T) +
                  0.15f * std::sin(2.0f * 3.14159265f * 277.2f * T) + 0.1f * std::sin(2.0f * 3.14159265f * 329.6f * T);
    }
    constexpr int WarmUp = 2;
    constexpr int Timed = 5;
    std::vector<int64_t> Wall;
    std::vector<int64_t> Cpu;
    for (int i = 0; i < WarmUp + Timed; ++i)
    {
        const auto WallStart = std::chrono::steady_clock::now();
        const int64_t CpuStart = ThreadCpuTimeUs();
        if (!Classifier.Classify(Wave, OutScores, OutError))
        {
            return false;
        }
        if (i >= WarmUp)
        {
            Cpu.push_back(ThreadCpuTimeUs() - CpuStart);
            Wall.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                 WallStart)
                               .count());
        }
    }
    std::nth_element(Wall.begin(), Wall.begin() + Timed / 2, Wall.end());
    std::nth_element(Cpu.begin(), Cpu.begin() + Timed / 2, Cpu.end());
    OutCost = {Wall[Timed / 2], Cpu[Timed / 2]};
    return true;
}

// An accelerator is kept only if it agrees with the CPU (an int8 NPU
// model can drift) and is both faster and cheaper on CPU time.
bool Validate(ContentClassifier& Accelerator, ContentClassifier& Cpu, std::string& OutWhy)
{
    if (Accelerator.SampleRate() != Cpu.SampleRate() || Accelerator.InputSamples() != Cpu.InputSamples() ||
        Accelerator.Labels() != Cpu.Labels())
    {
        OutWhy = "its model has a different input or labels than the CPU one";
        return false;
    }
    std::vector<float> CpuScores;
    std::vector<float> AccScores;
    RunCost CpuCost;
    RunCost AccCost;
    std::string Error;
    if (!Measure(Cpu, CpuScores, CpuCost, Error) || !Measure(Accelerator, AccScores, AccCost, Error))
    {
        OutWhy = "a test run failed: " + Error;
        return false;
    }
    float MaxDiff = 0.0f;
    for (size_t i = 0; i < CpuScores.size(); ++i)
    {
        MaxDiff = std::max(MaxDiff, std::abs(CpuScores[i] - AccScores[i]));
    }
    char Numbers[160];
    snprintf(Numbers, sizeof(Numbers), "max score difference %.3f; %.2f ms (%.2f ms CPU) vs CPU %.2f ms", MaxDiff,
             static_cast<double>(AccCost.WallUs) / 1000.0, static_cast<double>(AccCost.CpuUs) / 1000.0,
             static_cast<double>(CpuCost.WallUs) / 1000.0);
    OutWhy = Numbers;
    if (MaxDiff > 0.1f)
    {
        OutWhy = "its results differ from the CPU's (" + OutWhy + ")";
        return false;
    }
    if (AccCost.WallUs >= CpuCost.WallUs || AccCost.CpuUs >= CpuCost.CpuUs)
    {
        OutWhy = "it isn't faster than the CPU (" + OutWhy + ")";
        return false;
    }
    return true;
}

ClassifierLoadResult LoadForPolicy(const ClassifierModelPaths& Paths, InferenceDevice Device)
{
    ClassifierLoadResult Result;
    if (!GetRuntime(Result.Message))
    {
        Result.Availability = AutoDetectAvailability::NoRuntime;
        return Result;
    }
    if (!FileExists(Paths.Model))
    {
        Result.Availability = AutoDetectAvailability::NoModel;
        Result.Message = "model not found: " + Paths.Model;
        return Result;
    }

    std::string Error;
    std::unique_ptr<ContentClassifier> Cpu = OnnxClassifier::Load(Paths.Model, Provider::Cpu, Error);
    if (!Cpu)
    {
        Result.Availability = AutoDetectAvailability::Failed;
        Result.Message = Error;
        return Result;
    }

    std::vector<Provider> Candidates;
    switch (Device)
    {
    case InferenceDevice::Auto:
    case InferenceDevice::Npu:
        Candidates = {Provider::OpenVinoNpu};
        break;
    case InferenceDevice::Gpu:
        Candidates = {Provider::Cuda, Provider::OpenVinoGpu};
        break;
    default:
        break;
    }

    std::string Notes;
    const std::set<std::string> Available = Candidates.empty() ? std::set<std::string>{} : AvailableProviders();
    for (const Provider Id : Candidates)
    {
        const ProviderInfo& Info = InfoOf(Id);
        if (!Available.contains(Info.OrtName))
        {
            Notes += std::string("; ") + Info.Label + ": not in this ONNX Runtime build";
            continue;
        }
        const std::string& Model =
            Id == Provider::OpenVinoNpu && FileExists(Paths.NpuModel) ? Paths.NpuModel : Paths.Model;
        std::unique_ptr<ContentClassifier> Accelerator = OnnxClassifier::Load(Model, Id, Error);
        if (!Accelerator)
        {
            Notes += std::string("; ") + Info.Label + ": " + Error;
            continue;
        }
        std::string Why;
        if (Validate(*Accelerator, *Cpu, Why))
        {
            Result.Message = std::string("running on ") + Info.Label + " (" + Why + ")";
            Result.Classifier = std::move(Accelerator);
            return Result;
        }
        Notes += std::string("; ") + Info.Label + ": " + Why;
    }

    Result.Message = "running on CPU" + (Notes.empty() ? std::string() : " (" + Notes.substr(2) + ")");
    Result.Classifier = std::move(Cpu);
    return Result;
}

} // namespace

ClassifierModelPaths DefaultClassifierModelPaths()
{
    const char* Override = std::getenv("RAMKOLFX_MODEL_DIR");
    const std::string Dir = Override && *Override ? Override : RAMKOLFX_MODEL_DIR;
    return {Dir + "/audio_classifier.onnx", Dir + "/audio_classifier.int8.onnx"};
}

ClassifierBackend MakeOnnxClassifierBackend(ClassifierModelPaths Paths)
{
    ClassifierBackend Backend;
    Backend.Probe = [Paths] {
        if (!RuntimeLooksInstalled())
        {
            return AutoDetectAvailability::NoRuntime;
        }
        return FileExists(Paths.Model) ? AutoDetectAvailability::Ok : AutoDetectAvailability::NoModel;
    };
    Backend.Load = [Paths](InferenceDevice Device) { return LoadForPolicy(Paths, Device); };
    return Backend;
}

ClassifierLoadResult LoadOnnxClassifier(const std::string& ModelPath, const std::string& ProviderKey)
{
    ClassifierLoadResult Result;
    if (!GetRuntime(Result.Message))
    {
        Result.Availability = AutoDetectAvailability::NoRuntime;
        return Result;
    }
    const ProviderInfo* Info = nullptr;
    for (const ProviderInfo& Candidate : Providers)
    {
        if (ProviderKey == Candidate.Key)
        {
            Info = &Candidate;
        }
    }
    if (!Info)
    {
        Result.Availability = AutoDetectAvailability::Failed;
        Result.Message = "unknown provider '" + ProviderKey + "'";
        return Result;
    }
    if (!FileExists(ModelPath))
    {
        Result.Availability = AutoDetectAvailability::NoModel;
        Result.Message = "model not found: " + ModelPath;
        return Result;
    }
    Result.Classifier = OnnxClassifier::Load(ModelPath, Info->Id, Result.Message);
    if (!Result.Classifier)
    {
        Result.Availability = AutoDetectAvailability::Failed;
    }
    else
    {
        Result.Message = std::string("running on ") + Info->Label;
    }
    return Result;
}

std::vector<std::string> OnnxAvailableProviders()
{
    std::string Error;
    if (!GetRuntime(Error))
    {
        return {};
    }
    const std::set<std::string> Available = AvailableProviders();
    return {Available.begin(), Available.end()};
}

} // namespace ramkolfx
