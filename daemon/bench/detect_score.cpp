// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// ramkolfx-detect-score: scores Auto-detect on a labelled test set
// (docs/content-detect-plan.md, phase 7). Not installed.
//
//   ramkolfx-detect-score [--model PATH] [--provider cpu|cuda|...]
//                         [--out FILE.tsv] [--baseline FILE.tsv]
//                         [--seconds N] [--verbose] [PATH...]
//
// PATHs are clips or directories (default: the test set, see
// DefaultTestSet). A clip's expected class is the name of the folder it
// sits in: testset/Jazz/so-what.flac is Jazz. Any format ffmpeg reads.
//
// Each clip is replayed the way the daemon hears it: decoded to 48 kHz,
// downmixed to mono, cut into 100 ms chunks for the silence tracker, and
// classified at the worker's cadence (fast until the class settles, then
// slow) by the same LabelMap and DetectionSmoother. The cadence is the
// nominal one: the CPU governor is left out so results don't depend on
// how busy the machine is. A clip's verdict is the class held longest.
//
// Game clips are reported but not scored: games are recognised from
// stream metadata, so their row only shows what the audio alone would
// pick if the metadata missed one.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "content_detection.hpp"
#include "detect/content_detector.hpp"
#include "detect/onnx_classifier.hpp"

using namespace ramkolfx;
namespace fs = std::filesystem;

namespace
{
constexpr uint32_t Rate = 48000;      // what the tap delivers
constexpr int64_t ChunkMs = 100;      // ContentDetectorTuning::ChunkMs
constexpr int64_t PollMs = 500;       // ContentDetectorTuning::PollIntervalMs
constexpr size_t ChunkSamples = Rate * ChunkMs / 1000;

// $RAMKOLFX_TESTSET, else ~/.local/share/ramkolfx/testset.
std::string DefaultTestSet()
{
    if (const char* Env = std::getenv("RAMKOLFX_TESTSET"); Env && *Env)
    {
        return Env;
    }
    const char* Data = std::getenv("XDG_DATA_HOME");
    if (Data && *Data)
    {
        return std::string(Data) + "/ramkolfx/testset";
    }
    const char* Home = std::getenv("HOME");
    return std::string(Home ? Home : ".") + "/.local/share/ramkolfx/testset";
}

std::string Lower(std::string S)
{
    std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return std::tolower(C); });
    return S;
}

// A folder name as a class: its label ("Hip-Hop", "R&B") or its key
// ("hiphop", "rnb"), in any case.
std::optional<DetectedClass> ClassFromFolder(const std::string& Name)
{
    const std::string Want = Lower(Name);
    for (size_t c = 1; c < DetectedClassCount; ++c)
    {
        const auto Class = static_cast<DetectedClass>(c);
        if (Want == Lower(DetectedClassLabel(Class)) || Want == DetectedClassKey(Class))
        {
            return Class;
        }
    }
    return std::nullopt;
}

bool IsAudioFile(const fs::path& Path)
{
    static const char* const Extensions[] = {".flac", ".wav", ".mp3", ".ogg", ".opus", ".m4a",
                                             ".aac",  ".webm", ".mka", ".mkv", ".mp4", ".wma"};
    const std::string Ext = Lower(Path.extension().string());
    return std::any_of(std::begin(Extensions), std::end(Extensions), [&](const char* E) { return Ext == E; });
}

// Decodes Path with ffmpeg to 48 kHz stereo and averages it to mono:
// (L + R) / 2, as DownmixSevenOneToMono does for stereo content (ffmpeg's
// surround-to-stereo downmix also drops LFE). MaxSeconds > 0 stops early.
bool DecodeMono48k(const std::string& Path, double MaxSeconds, std::vector<float>& Out, std::string& Error)
{
    int Pipe[2];
    if (pipe(Pipe) != 0)
    {
        Error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    std::vector<std::string> Args = {"ffmpeg", "-nostdin", "-v", "error", "-i", Path};
    if (MaxSeconds > 0)
    {
        Args.insert(Args.end(), {"-t", std::to_string(MaxSeconds)});
    }
    Args.insert(Args.end(), {"-vn", "-ac", "2", "-ar", std::to_string(Rate), "-f", "f32le", "-"});
    const pid_t Child = fork();
    if (Child < 0)
    {
        Error = std::string("fork: ") + std::strerror(errno);
        return false;
    }
    if (Child == 0)
    {
        dup2(Pipe[1], STDOUT_FILENO);
        close(Pipe[0]);
        close(Pipe[1]);
        std::vector<char*> Argv;
        for (std::string& A : Args)
        {
            Argv.push_back(A.data());
        }
        Argv.push_back(nullptr);
        execvp("ffmpeg", Argv.data());
        std::fprintf(stderr, "cannot run ffmpeg: %s\n", std::strerror(errno));
        _exit(127);
    }
    close(Pipe[1]);
    Out.clear();
    float Frame[2 * 4096];
    size_t Pending = 0; // bytes of a partial read carried over
    char* Bytes = reinterpret_cast<char*>(Frame);
    for (;;)
    {
        const ssize_t N = read(Pipe[0], Bytes + Pending, sizeof(Frame) - Pending);
        if (N < 0 && errno == EINTR)
        {
            continue;
        }
        if (N <= 0)
        {
            break;
        }
        Pending += static_cast<size_t>(N);
        const size_t Frames = Pending / (2 * sizeof(float));
        for (size_t i = 0; i < Frames; ++i)
        {
            Out.push_back(0.5f * (Frame[2 * i] + Frame[2 * i + 1]));
        }
        const size_t Used = Frames * 2 * sizeof(float);
        std::memmove(Bytes, Bytes + Used, Pending - Used);
        Pending -= Used;
    }
    close(Pipe[0]);
    int Status = 0;
    waitpid(Child, &Status, 0);
    if (!WIFEXITED(Status) || WEXITSTATUS(Status) != 0)
    {
        Error = WIFEXITED(Status) && WEXITSTATUS(Status) == 127 ? "ffmpeg not found" : "ffmpeg failed to decode it";
        return false;
    }
    return true;
}

struct ClipResult
{
    std::string Path;
    DetectedClass Expected = DetectedClass::None;
    DetectedClass Verdict = DetectedClass::None; // held longest; None if nothing was ever detected
    int64_t DurationMs = 0;
    int64_t FirstClassMs = -1;                   // first class adopted; -1 never
    int Switches = 0;                            // class changes after the first
    std::array<int64_t, DetectedClassCount> HeldMs{};
    ClassScores Evidence{};                      // the smoother's, at the end
    int Runs = 0;

    double RightShare() const
    {
        return DurationMs > 0
                   ? static_cast<double>(HeldMs[static_cast<size_t>(Expected)]) / static_cast<double>(DurationMs)
                   : 0.0;
    }
};

// Replays Audio through the worker's chunking, cadence and smoother.
bool ScoreClip(ContentClassifier& Classifier, const LabelMap& Map, const std::vector<float>& Audio,
               ClipResult& Result, bool bVerbose, std::string& Error)
{
    const AutoDetectTiming Timing;
    const SchedulerTuning Cadence = SchedulerTuningFor(Timing);
    DetectionSmoother Smoother(DetectionTuningFor(Timing));
    SilenceTracker Silence;

    const Resampler ToModel(Rate, Classifier.SampleRate());
    std::vector<float> Window(ToModel.InputFor(Classifier.InputSamples()));
    std::vector<float> ModelInput(Classifier.InputSamples());
    std::vector<float> Scores;
    SampleHistory History(Window.size());
    const auto WindowMs = static_cast<int64_t>(Classifier.InputSamples() * 1000 / Classifier.SampleRate());
    const int64_t NeedAudibleMs = std::min(ContentDetectorTuning{}.MinAudibleMs, WindowMs);

    int64_t NextRunAtMs = 0;
    DetectedClass Previous = DetectedClass::None;
    const size_t Chunks = Audio.size() / ChunkSamples;
    for (size_t i = 0; i < Chunks; ++i)
    {
        const std::span<const float> Chunk(Audio.data() + i * ChunkSamples, ChunkSamples);
        const auto StartMs = static_cast<int64_t>(i) * ChunkMs;
        const int64_t EndMs = StartMs + ChunkMs;
        const SilenceTracker::Result R = Silence.AddChunk(Rms(Chunk), ChunkMs);
        History.Append(Chunk);
        if (R.bBoundary)
        {
            Smoother.AddTrackBoundary(StartMs);
        }
        if (EndMs % PollMs == 0 && EndMs >= NextRunAtMs && Silence.AudibleMs() >= NeedAudibleMs &&
            History.CopyLatest(Window))
        {
            ToModel.Process(Window, ModelInput);
            if (!Classifier.Classify(ModelInput, Scores, Error))
            {
                return false;
            }
            const ClassScores Folded = Map.Fold(Scores);
            Smoother.AddScores(Folded, EndMs);
            ++Result.Runs;
            const bool bSettled = Smoother.IsSettled(EndMs);
            NextRunAtMs = EndMs + (bSettled ? Cadence.SlowIntervalMs : Cadence.FastIntervalMs);
            if (bVerbose)
            {
                size_t Best = 1;
                for (size_t c = 1; c < DetectedClassCount; ++c)
                {
                    Best = Folded[c] > Folded[Best] ? c : Best;
                }
                std::printf("    %6.1f s  speech %.2f music %.2f film %.2f  top %-10s %.2f  -> %s%s\n",
                            static_cast<double>(EndMs) / 1000.0,
                            static_cast<double>(ScoreOf(Folded, DetectedClass::Speech)),
                            static_cast<double>(ScoreOf(Folded, DetectedClass::Music)),
                            static_cast<double>(ScoreOf(Folded, DetectedClass::Movie)),
                            DetectedClassLabel(static_cast<DetectedClass>(Best)), static_cast<double>(Folded[Best]),
                            DetectedClassLabel(Smoother.Current()), bSettled ? " (settled)" : "");
            }
        }
        const DetectedClass Current = Smoother.Current();
        if (Current != Previous)
        {
            if (Previous == DetectedClass::None)
            {
                Result.FirstClassMs = EndMs;
            }
            else
            {
                ++Result.Switches;
            }
            Previous = Current;
        }
        Result.HeldMs[static_cast<size_t>(Current)] += ChunkMs;
    }
    Result.DurationMs = static_cast<int64_t>(Chunks) * ChunkMs;
    Result.Evidence = Smoother.Smoothed();
    int64_t Longest = 0;
    for (size_t c = 1; c < DetectedClassCount; ++c)
    {
        if (Result.HeldMs[c] > Longest)
        {
            Longest = Result.HeldMs[c];
            Result.Verdict = static_cast<DetectedClass>(c);
        }
    }
    return true;
}

// --- Results file ------------------------------------------------------------
// One line per clip: path, expected, verdict, right %, first class ms,
// switches, then the final evidence per class (header names them).

void WriteResults(const std::string& Path, const std::vector<ClipResult>& Results)
{
    std::ofstream Out(Path);
    Out << "path\texpected\tverdict\tright_pct\tfirst_class_ms\tswitches";
    for (size_t c = 1; c < DetectedClassCount; ++c)
    {
        Out << "\tev_" << DetectedClassKey(static_cast<DetectedClass>(c));
    }
    Out << "\n";
    for (const ClipResult& R : Results)
    {
        char Pct[16];
        std::snprintf(Pct, sizeof(Pct), "%.1f", 100.0 * R.RightShare());
        Out << R.Path << "\t" << DetectedClassLabel(R.Expected) << "\t" << DetectedClassLabel(R.Verdict) << "\t" << Pct
            << "\t" << R.FirstClassMs << "\t" << R.Switches;
        for (size_t c = 1; c < DetectedClassCount; ++c)
        {
            char Ev[16];
            std::snprintf(Ev, sizeof(Ev), "%.4f", static_cast<double>(R.Evidence[c]));
            Out << "\t" << Ev;
        }
        Out << "\n";
    }
}

// path -> verdict label, from an earlier --out file.
std::map<std::string, std::string> ReadVerdicts(const std::string& Path)
{
    std::map<std::string, std::string> Verdicts;
    std::ifstream In(Path);
    std::string Line;
    std::getline(In, Line); // header
    while (std::getline(In, Line))
    {
        std::istringstream Fields(Line);
        std::string ClipPath, Expected, Verdict;
        if (std::getline(Fields, ClipPath, '\t') && std::getline(Fields, Expected, '\t') &&
            std::getline(Fields, Verdict, '\t'))
        {
            Verdicts[ClipPath] = Verdict;
        }
    }
    return Verdicts;
}

// --- Report ------------------------------------------------------------------

bool Scored(const ClipResult& R)
{
    return R.Expected != DetectedClass::Game;
}

// Short column heading for the confusion table.
std::string Abbrev(DetectedClass Class)
{
    switch (Class)
    {
    case DetectedClass::None: return "-";
    case DetectedClass::HipHop: return "HipH";
    case DetectedClass::RnB: return "R&B";
    case DetectedClass::Electronic: return "Elec";
    case DetectedClass::Classical: return "Clas";
    case DetectedClass::Acoustic: return "Acou";
    default: return std::string(DetectedClassLabel(Class)).substr(0, 4);
    }
}

void Report(const std::vector<ClipResult>& Results)
{
    struct Row
    {
        int Clips = 0;
        int Right = 0;
        double RightShare = 0.0;
        int64_t FirstSum = 0;
        int FirstCount = 0;
        int Switches = 0;
    };
    std::array<Row, DetectedClassCount> Rows{};
    std::array<std::array<int, DetectedClassCount>, DetectedClassCount> Confusion{};
    std::array<bool, DetectedClassCount> Column{};
    for (const ClipResult& R : Results)
    {
        Row& Rw = Rows[static_cast<size_t>(R.Expected)];
        ++Rw.Clips;
        Rw.Right += R.Verdict == R.Expected ? 1 : 0;
        Rw.RightShare += R.RightShare();
        Rw.Switches += R.Switches;
        if (R.FirstClassMs >= 0)
        {
            Rw.FirstSum += R.FirstClassMs;
            ++Rw.FirstCount;
        }
        ++Confusion[static_cast<size_t>(R.Expected)][static_cast<size_t>(R.Verdict)];
        Column[static_cast<size_t>(R.Expected)] = true;
        Column[static_cast<size_t>(R.Verdict)] = true;
    }

    std::printf("\nPer class (verdict: the class held longest; time right: share of the clip spent on the right "
                "class)\n");
    std::printf("  %-11s %5s %6s %9s %11s %12s %9s\n", "expected", "clips", "right", "accuracy", "time right",
                "first class", "switches");
    int AllClips = 0;
    int AllRight = 0;
    double MacroSum = 0.0;
    int MacroCount = 0;
    for (size_t c = 1; c < DetectedClassCount; ++c)
    {
        const Row& Rw = Rows[c];
        if (Rw.Clips == 0)
        {
            continue;
        }
        const auto Class = static_cast<DetectedClass>(c);
        const double Accuracy = 100.0 * Rw.Right / Rw.Clips;
        std::printf("  %-11s %5d %6d %8.0f%% %10.0f%% %10.1f s %9.1f%s\n", DetectedClassLabel(Class), Rw.Clips,
                    Rw.Right, Accuracy, 100.0 * Rw.RightShare / Rw.Clips,
                    Rw.FirstCount ? static_cast<double>(Rw.FirstSum) / Rw.FirstCount / 1000.0 : 0.0,
                    static_cast<double>(Rw.Switches) / Rw.Clips, Class == DetectedClass::Game ? "  (not scored)" : "");
        if (Class != DetectedClass::Game)
        {
            AllClips += Rw.Clips;
            AllRight += Rw.Right;
            MacroSum += Accuracy;
            ++MacroCount;
        }
    }
    if (AllClips > 0)
    {
        std::printf("  %-11s %5d %6d %8.0f%%   (mean of the per-class accuracies: %.0f%%)\n", "all", AllClips,
                    AllRight, 100.0 * AllRight / AllClips, MacroSum / MacroCount);
    }

    std::printf("\nConfusion (rows: expected, columns: verdict; - = nothing detected)\n  %-11s", "");
    std::vector<size_t> Cols;
    for (size_t c = 1; c < DetectedClassCount; ++c)
    {
        if (Column[c])
        {
            Cols.push_back(c);
        }
    }
    if (Column[0])
    {
        Cols.push_back(0);
    }
    for (size_t c : Cols)
    {
        std::printf(" %5s", Abbrev(static_cast<DetectedClass>(c)).c_str());
    }
    std::printf("\n");
    for (size_t e = 1; e < DetectedClassCount; ++e)
    {
        if (Rows[e].Clips == 0)
        {
            continue;
        }
        std::printf("  %-11s", DetectedClassLabel(static_cast<DetectedClass>(e)));
        for (size_t c : Cols)
        {
            const int N = Confusion[e][c];
            if (N == 0)
            {
                std::printf(" %5s", ".");
            }
            else
            {
                std::printf(" %4d%s", N, c == e ? "*" : " ");
            }
        }
        std::printf("\n");
    }
}

void CompareWithBaseline(const std::vector<ClipResult>& Results, const std::string& BaselinePath)
{
    const std::map<std::string, std::string> Old = ReadVerdicts(BaselinePath);
    if (Old.empty())
    {
        std::printf("\nBaseline %s: no results read\n", BaselinePath.c_str());
        return;
    }
    int Fixed = 0;
    int Broken = 0;
    int OldRight = 0;
    int NewRight = 0;
    int Common = 0;
    std::vector<std::string> Lines;
    for (const ClipResult& R : Results)
    {
        const auto It = Old.find(R.Path);
        if (It == Old.end() || !Scored(R))
        {
            continue;
        }
        ++Common;
        const std::string Expected = DetectedClassLabel(R.Expected);
        const std::string Now = DetectedClassLabel(R.Verdict);
        const bool bWasRight = It->second == Expected;
        const bool bIsRight = Now == Expected;
        OldRight += bWasRight ? 1 : 0;
        NewRight += bIsRight ? 1 : 0;
        if (It->second != Now)
        {
            Fixed += !bWasRight && bIsRight ? 1 : 0;
            Broken += bWasRight && !bIsRight ? 1 : 0;
            Lines.push_back("  " + std::string(bIsRight ? "fixed " : bWasRight ? "BROKE " : "moved ") + R.Path +
                            ": " + It->second + " -> " + Now + " (expected " + Expected + ")");
        }
    }
    std::printf("\nAgainst %s (%d clips in both): %d right before, %d now; %d fixed, %d broken\n",
                BaselinePath.c_str(), Common, OldRight, NewRight, Fixed, Broken);
    for (const std::string& L : Lines)
    {
        std::printf("%s\n", L.c_str());
    }
}

void Usage()
{
    std::printf("usage: ramkolfx-detect-score [--model PATH] [--provider cpu|cuda|openvino-gpu|openvino-npu]\n"
                "                             [--out FILE.tsv] [--baseline FILE.tsv] [--seconds N]\n"
                "                             [--verbose] [PATH...]\n"
                "PATHs are clips or folders (default: %s);\n"
                "a clip's expected class is the name of its folder (Speech, Movie, Music, Rock, Hip-Hop, ...).\n",
                DefaultTestSet().c_str());
}
} // namespace

int main(int Argc, char** Argv)
{
    std::string Model = DefaultClassifierModelPaths().Model;
    std::string Provider = "cpu";
    std::string OutPath;
    std::string BaselinePath;
    double MaxSeconds = 0;
    bool bVerbose = false;
    std::vector<std::string> Inputs;
    for (int i = 1; i < Argc; ++i)
    {
        const std::string Arg = Argv[i];
        const bool bHasValue = i + 1 < Argc;
        if (Arg == "--model" && bHasValue)
        {
            Model = Argv[++i];
        }
        else if (Arg == "--provider" && bHasValue)
        {
            Provider = Argv[++i];
        }
        else if (Arg == "--out" && bHasValue)
        {
            OutPath = Argv[++i];
        }
        else if (Arg == "--baseline" && bHasValue)
        {
            BaselinePath = Argv[++i];
        }
        else if (Arg == "--seconds" && bHasValue)
        {
            MaxSeconds = std::max(0.0, std::atof(Argv[++i]));
        }
        else if (Arg == "--verbose" || Arg == "-v")
        {
            bVerbose = true;
        }
        else if (Arg == "--help" || Arg == "-h")
        {
            Usage();
            return 0;
        }
        else if (!Arg.empty() && Arg[0] == '-')
        {
            Usage();
            return 2;
        }
        else
        {
            Inputs.push_back(Arg);
        }
    }
    if (Inputs.empty())
    {
        Inputs.push_back(DefaultTestSet());
    }

    // Clips, sorted so runs line up.
    std::vector<std::pair<std::string, DetectedClass>> Clips;
    int Unlabelled = 0;
    auto AddClip = [&](const fs::path& Path) {
        const auto Class = ClassFromFolder(Path.parent_path().filename().string());
        if (!Class)
        {
            std::fprintf(stderr, "skipped %s: folder '%s' is not a class name\n", Path.c_str(),
                         Path.parent_path().filename().c_str());
            ++Unlabelled;
            return;
        }
        Clips.emplace_back(Path.string(), *Class);
    };
    for (const std::string& Input : Inputs)
    {
        std::error_code Ec;
        if (fs::is_directory(Input, Ec))
        {
            for (const auto& Entry : fs::recursive_directory_iterator(Input, Ec))
            {
                if (Entry.is_regular_file() && IsAudioFile(Entry.path()))
                {
                    AddClip(Entry.path());
                }
            }
        }
        else if (fs::is_regular_file(Input, Ec))
        {
            AddClip(Input);
        }
        else
        {
            std::fprintf(stderr, "no such file or folder: %s\n", Input.c_str());
            return 1;
        }
    }
    std::sort(Clips.begin(), Clips.end());
    if (Clips.empty())
    {
        std::fprintf(stderr, "no clips found%s\n", Unlabelled ? " in class-named folders" : "");
        return 1;
    }

    ClassifierLoadResult Loaded = LoadOnnxClassifier(Model, Provider);
    if (!Loaded.Classifier)
    {
        std::fprintf(stderr, "cannot load %s: %s\n", Model.c_str(), Loaded.Message.c_str());
        return 1;
    }
    ContentClassifier& Classifier = *Loaded.Classifier;
    std::vector<std::string> Missing;
    const LabelMap Map = LabelMap::Build(Classifier.Labels(), &Missing);
    std::printf("Model %s (%s), %zu clips\n", Model.c_str(), Loaded.Message.c_str(), Clips.size());
    if (!Missing.empty())
    {
        std::printf("  %zu LabelMap labels not in the model\n", Missing.size());
    }

    std::vector<ClipResult> Results;
    std::vector<float> Audio;
    for (const auto& [Path, Expected] : Clips)
    {
        std::string Error;
        if (!DecodeMono48k(Path, MaxSeconds, Audio, Error))
        {
            std::fprintf(stderr, "skipped %s: %s\n", Path.c_str(), Error.c_str());
            continue;
        }
        ClipResult R;
        R.Path = Path;
        R.Expected = Expected;
        if (bVerbose)
        {
            std::printf("  %s\n", Path.c_str());
        }
        if (!ScoreClip(Classifier, Map, Audio, R, bVerbose, Error))
        {
            std::fprintf(stderr, "inference failed on %s: %s\n", Path.c_str(), Error.c_str());
            return 1;
        }
        const char* Mark = !Scored(R) ? " " : R.Verdict == R.Expected ? "+" : "x";
        std::printf("%s %-10s -> %-10s %3.0f%% right, first at %4.1f s, %d switches  %s\n", Mark,
                    DetectedClassLabel(R.Expected), DetectedClassLabel(R.Verdict), 100.0 * R.RightShare(),
                    R.FirstClassMs >= 0 ? static_cast<double>(R.FirstClassMs) / 1000.0 : 0.0, R.Switches,
                    fs::path(Path).filename().c_str());
        std::fflush(stdout);
        Results.push_back(std::move(R));
    }
    if (Results.empty())
    {
        return 1;
    }

    Report(Results);
    if (!BaselinePath.empty())
    {
        CompareWithBaseline(Results, BaselinePath);
    }
    if (!OutPath.empty())
    {
        WriteResults(OutPath, Results);
        std::printf("\nResults written to %s\n", OutPath.c_str());
    }
    return 0;
}
