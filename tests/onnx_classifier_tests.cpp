// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// The ONNX Runtime classifier, against the real runtime. Skipped (exit 77)
// when ORT isn't installed - builds and CI don't need it; point
// RAMKOLFX_ORT_LIB at a libonnxruntime.so to run it anyway.
//
// - Plumbing, on a stand-in model built right here: a tiny ONNX graph
//   (scores = mean(|waveform|) * weights) encoded by hand, so the test
//   needs no Python and no model download.
// - Parity, on the real model if it's installed: every
//   <model dir>/golden/<class>[_anything].wav must come out as <class>
//   (e.g. jazz_take5.wav -> Jazz) after smoothing, as the daemon would.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "content_detection.hpp"
#include "detect/onnx_classifier.hpp"
#include "detect/wav_file.hpp"
#include "test_harness.hpp"

using namespace ramkolfx;
using ramkolfx::test::TempPath;

namespace
{
// Just enough protobuf to write an ONNX ModelProto.
class Proto
{
public:
    Proto& Varint(int Field, uint64_t Value)
    {
        Key(Field, 0);
        PutVarint(Value);
        return *this;
    }
    Proto& Bytes(int Field, std::string_view Value)
    {
        Key(Field, 2);
        PutVarint(Value.size());
        Buffer.append(Value);
        return *this;
    }
    Proto& Message(int Field, const Proto& Value)
    {
        return Bytes(Field, Value.Buffer);
    }
    const std::string& Data() const
    {
        return Buffer;
    }

private:
    void Key(int Field, int WireType)
    {
        PutVarint(static_cast<uint64_t>(Field) << 3 | static_cast<uint64_t>(WireType));
    }
    void PutVarint(uint64_t Value)
    {
        do
        {
            const auto Byte = static_cast<char>((Value & 0x7f) | (Value > 0x7f ? 0x80 : 0));
            Buffer.push_back(Byte);
            Value >>= 7;
        } while (Value);
    }
    std::string Buffer;
};

constexpr int OnnxFloat = 1;

Proto TensorType(std::initializer_list<int64_t> Dims)
{
    Proto Shape;
    for (const int64_t D : Dims)
    {
        Proto Dim;
        if (D >= 0)
        {
            Dim.Varint(1, static_cast<uint64_t>(D)); // dim_value
        }
        else
        {
            Dim.Bytes(2, "n"); // dim_param: dynamic
        }
        Shape.Message(1, Dim);
    }
    Proto Tensor;
    Tensor.Varint(1, OnnxFloat).Message(2, Shape);
    Proto Type;
    Type.Message(1, Tensor);
    return Type;
}

Proto ValueInfo(const char* Name, std::initializer_list<int64_t> Dims)
{
    Proto Info;
    Info.Bytes(1, Name).Message(2, TensorType(Dims));
    return Info;
}

Proto Node(const char* Op, std::initializer_list<const char*> Inputs, const char* Output)
{
    Proto N;
    for (const char* In : Inputs)
    {
        N.Bytes(1, In);
    }
    N.Bytes(2, Output).Bytes(4, Op);
    return N;
}

struct StandIn
{
    int64_t Length = 16000; // -1: dynamic, given by metadata instead
    bool bLogits = false;
    bool bWithLabels = true;
};

// scores[1, 3] = mean(|waveform[1, N]|) * {0, 2, 4}; labels Speech, Music, Jazz.
std::string BuildStandInModel(const StandIn& Options)
{
    Proto Graph;
    Graph.Message(1, Node("Abs", {"waveform"}, "abs"));
    Proto Reduce = Node("ReduceMean", {"abs"}, "mean");
    Proto Axes;
    Axes.Bytes(1, "axes").Varint(8, 1).Varint(20, 7); // ints = [1], type INTS
    Reduce.Message(5, Axes);
    Graph.Message(1, Reduce);
    Graph.Message(1, Node("Mul", {"mean", "weights"}, "scores"));
    Graph.Bytes(2, "stand_in");

    const float Weights[3] = {0.0f, 2.0f, 4.0f};
    Proto Init;
    Init.Varint(1, 1).Varint(1, 3).Varint(2, OnnxFloat).Bytes(8, "weights");
    Init.Bytes(9, std::string_view(reinterpret_cast<const char*>(Weights), sizeof(Weights)));
    Graph.Message(5, Init);
    Graph.Message(11, ValueInfo("waveform", {1, Options.Length}));
    Graph.Message(12, ValueInfo("scores", {1, 3}));

    Proto Opset;
    Opset.Bytes(1, "").Varint(2, 13);
    Proto Model;
    Model.Varint(1, 7).Bytes(2, "ramkolfx-test").Message(7, Graph).Message(8, Opset);
    const auto Meta = [&](const char* Key, const std::string& Value) {
        Proto Entry;
        Entry.Bytes(1, Key).Bytes(2, Value);
        Model.Message(14, Entry);
    };
    Meta("ramkolfx.sample_rate", "16000");
    if (Options.bWithLabels)
    {
        Meta("ramkolfx.labels", "Speech\nMusic\nJazz");
    }
    if (Options.Length < 0)
    {
        Meta("ramkolfx.input_samples", "16000");
    }
    if (Options.bLogits)
    {
        Meta("ramkolfx.output", "logits");
    }
    return Model.Data();
}

void WriteFile(const std::string& Path, const std::string& Contents)
{
    std::ofstream(Path, std::ios::binary) << Contents;
}

std::vector<float> Tone(float Amplitude, size_t Count)
{
    std::vector<float> Out(Count);
    for (size_t i = 0; i < Count; ++i)
    {
        Out[i] = Amplitude * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * static_cast<float>(i) / 16000.0f);
    }
    return Out;
}

bool Near(float A, float B)
{
    return std::abs(A - B) < 1e-3f;
}
} // namespace

TEST(StandInModelRunsOnTheCpu)
{
    TempPath Model;
    WriteFile(Model.Get(), BuildStandInModel({}));
    ClassifierLoadResult Loaded = LoadOnnxClassifier(Model.Get(), "cpu");
    if (!Loaded.Classifier)
    {
        std::printf("  load failed: %s\n", Loaded.Message.c_str());
    }
    REQUIRE(Loaded.Classifier);
    ContentClassifier& Classifier = *Loaded.Classifier;
    CHECK(Classifier.SampleRate() == 16000);
    CHECK(Classifier.InputSamples() == 16000);
    CHECK(Classifier.Labels() == (std::vector<std::string>{"Speech", "Music", "Jazz"}));
    CHECK(Classifier.IsCpu() && Classifier.DeviceLabel() == "CPU");

    std::vector<float> Scores;
    std::string Error;
    REQUIRE(Classifier.Classify(Tone(0.5f, 16000), Scores, Error));
    const float MeanAbs = 0.5f * 2.0f / std::numbers::pi_v<float>;
    REQUIRE(Scores.size() == 3);
    CHECK(Near(Scores[0], 0.0f) && Near(Scores[1], 2.0f * MeanAbs) && Near(Scores[2], 4.0f * MeanAbs));

    CHECK(!Classifier.Classify(Tone(0.5f, 100), Scores, Error)); // wrong length
}

TEST(LogitsAndDynamicLength)
{
    StandIn Options;
    Options.Length = -1;
    Options.bLogits = true;
    TempPath Model;
    WriteFile(Model.Get(), BuildStandInModel(Options));
    ClassifierLoadResult Loaded = LoadOnnxClassifier(Model.Get(), "cpu");
    REQUIRE(Loaded.Classifier);
    CHECK(Loaded.Classifier->InputSamples() == 16000);
    std::vector<float> Scores;
    std::string Error;
    REQUIRE(Loaded.Classifier->Classify(Tone(0.0f, 16000), Scores, Error));
    CHECK(Near(Scores[1], 0.5f)); // sigmoid(0)
}

TEST(BadModelsAreRefused)
{
    StandIn Options;
    Options.bWithLabels = false;
    TempPath Model;
    WriteFile(Model.Get(), BuildStandInModel(Options));
    ClassifierLoadResult NoLabels = LoadOnnxClassifier(Model.Get(), "cpu");
    CHECK(!NoLabels.Classifier && NoLabels.Availability == AutoDetectAvailability::Failed);

    WriteFile(Model.Get(), "not a model");
    CHECK(!LoadOnnxClassifier(Model.Get(), "cpu").Classifier);
    CHECK(LoadOnnxClassifier("/nonexistent.onnx", "cpu").Availability == AutoDetectAvailability::NoModel);
}

TEST(BackendFallsBackToTheCpuWithoutAnNpu)
{
    // The stand-in lacks the Speech/Music labels ContentDetector insists
    // on, but the backend itself doesn't care.
    const std::string Dir = std::filesystem::temp_directory_path() / "ramkolfx_test_models";
    std::filesystem::create_directories(Dir);
    WriteFile(Dir + "/audio_classifier.onnx", BuildStandInModel({}));
    const ClassifierBackend Backend = MakeOnnxClassifierBackend({Dir + "/audio_classifier.onnx", ""});
    CHECK(Backend.Probe() == AutoDetectAvailability::Ok);
    ClassifierLoadResult Loaded = Backend.Load(InferenceDevice::Auto);
    REQUIRE(Loaded.Classifier);
    std::printf("  %s\n", Loaded.Message.c_str());

    const ClassifierBackend Missing = MakeOnnxClassifierBackend({Dir + "/nope.onnx", ""});
    CHECK(Missing.Probe() == AutoDetectAvailability::NoModel);
    CHECK(Missing.Load(InferenceDevice::Cpu).Availability == AutoDetectAvailability::NoModel);
    std::filesystem::remove_all(Dir);
}

TEST(GoldenClipsComeOutAsTheirClass)
{
    const ClassifierModelPaths Paths = DefaultClassifierModelPaths();
    const std::filesystem::path GoldenDir = std::filesystem::path(Paths.Model).parent_path() / "golden";
    std::error_code Ignored;
    if (!std::filesystem::exists(Paths.Model) || !std::filesystem::is_directory(GoldenDir, Ignored))
    {
        std::printf("  (no model or golden clips installed - skipped)\n");
        return;
    }
    ClassifierLoadResult Loaded = LoadOnnxClassifier(Paths.Model, "cpu");
    REQUIRE(Loaded.Classifier);
    ContentClassifier& Classifier = *Loaded.Classifier;
    const LabelMap Map = LabelMap::Build(Classifier.Labels());

    for (const auto& Entry : std::filesystem::directory_iterator(GoldenDir))
    {
        if (Entry.path().extension() != ".wav")
        {
            continue;
        }
        const std::string Stem = Entry.path().stem().string();
        const auto Expected = DetectedClassFromKey(Stem.substr(0, Stem.find('_')));
        MonoWav Wav;
        std::string Error;
        if (!Expected || !ReadWavMono(Entry.path().string(), Wav, Error))
        {
            std::printf("  %s: skipped (%s)\n", Stem.c_str(), Expected ? Error.c_str() : "unknown class");
            continue;
        }
        const Resampler ToModel(Wav.SampleRate, Classifier.SampleRate());
        std::vector<float> Audio(Wav.Samples.size() * Classifier.SampleRate() / Wav.SampleRate);
        ToModel.Process(Wav.Samples, Audio);

        // A window every 2 s, as the worker's fast cadence would.
        DetectionSmoother Smoother;
        std::vector<float> Scores;
        const size_t Step = 2 * Classifier.SampleRate();
        for (size_t Start = 0; Start + Classifier.InputSamples() <= Audio.size(); Start += Step)
        {
            const std::span<const float> Window(Audio.data() + Start, Classifier.InputSamples());
            REQUIRE(Classifier.Classify(Window, Scores, Error));
            Smoother.AddScores(Map.Fold(Scores), static_cast<int64_t>(Start * 1000 / Classifier.SampleRate()));
        }
        std::printf("  %s -> %s (%.2f)\n", Stem.c_str(), DetectedClassLabel(Smoother.Current()),
                    static_cast<double>(Smoother.Confidence()));
        CHECK(Smoother.Current() == *Expected);
    }
}

int main()
{
    if (OnnxAvailableProviders().empty())
    {
        std::printf("ONNX Runtime not found - skipped (set RAMKOLFX_ORT_LIB to run)\n");
        return 77;
    }
    return ::ramkolfx::test::RunAll();
}
