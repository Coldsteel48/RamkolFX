// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// End-to-end scenarios against a real ramkolfxd on a live PipeWire session:
// EQ preset auto-switching (with real pw-play streams), per-device spatial
// modes and settings persistence. Not part of ctest - it needs PipeWire
// and starts/stops the daemon. Driven by tests/e2e/run.sh, which gives each
// scenario a fresh daemon and config directory; see that script.
//
// Usage: ramkolfx_e2e <scenario> [args...]   (exit 0 = pass)

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <csignal>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ramkolfx/protocol.hpp"

using namespace ramkolfx;

namespace
{

// The app name the test stream plays under - what the scenarios bind.
constexpr const char* TestApp = "rfxtest_play";

void StopPlaying();

[[noreturn]] void Fail(int Line, const char* Expression)
{
    std::printf("FAIL line %d: %s\n", Line, Expression);
    StopPlaying();
    std::exit(1);
}

#define E2E_CHECK(Condition)                                                                                         \
    do                                                                                                               \
    {                                                                                                                \
        if (!(Condition))                                                                                            \
        {                                                                                                            \
            Fail(__LINE__, #Condition);                                                                              \
        }                                                                                                            \
    } while (0)

void SleepMs(int Ms)
{
    usleep(static_cast<useconds_t>(Ms) * 1000);
}

// --- daemon connection -----------------------------------------------------

int SocketFd = -1;

// Connects SocketFd to the daemon's control socket; false if it isn't up.
bool Connect()
{
    SocketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un Addr{};
    Addr.sun_family = AF_UNIX;
    std::strncpy(Addr.sun_path, DefaultControlSocketPath().c_str(), sizeof(Addr.sun_path) - 1);
    return connect(SocketFd, reinterpret_cast<sockaddr*>(&Addr), sizeof(Addr)) == 0;
}

void ReadExactly(uint8* Out, size_t Size)
{
    size_t Got = 0;
    while (Got < Size)
    {
        const ssize_t Count = read(SocketFd, Out + Got, Size - Got);
        E2E_CHECK(Count > 0);
        Got += static_cast<size_t>(Count);
    }
}

std::pair<Opcode, std::vector<uint8>> Request(const std::vector<uint8>& Message)
{
    E2E_CHECK(write(SocketFd, Message.data(), Message.size()) == static_cast<ssize_t>(Message.size()));
    uint8 HeaderBytes[HeaderSize];
    ReadExactly(HeaderBytes, HeaderSize);
    const MessageHeader Header = *TryReadHeader(HeaderBytes, HeaderSize);
    std::vector<uint8> Payload(Header.PayloadLength);
    ReadExactly(Payload.data(), Payload.size());
    return {Header.MessageOpcode, std::move(Payload)};
}

Status Send(const std::vector<uint8>& Message)
{
    auto [Op, Payload] = Request(Message);
    E2E_CHECK(Op == Opcode::StatusResponse);
    const auto Decoded = DecodeStatusResponse(Payload.data(), static_cast<uint16>(Payload.size()));
    E2E_CHECK(Decoded);
    return *Decoded;
}

Status GetStatus()
{
    return Send(EncodeGetStatusRequest());
}

std::vector<ContentStreamInfo> GetStreams()
{
    auto [Op, Payload] = Request(EncodeGetContentStreamsRequest());
    E2E_CHECK(Op == Opcode::ContentStreamListResponse);
    return *DecodeContentStreamListResponse(Payload.data(), static_cast<uint16>(Payload.size()));
}

std::array<EqBand, MaxEqBands> GetBands()
{
    auto [Op, Payload] = Request(EncodeGetHwEqStateRequest());
    E2E_CHECK(Op == Opcode::HwEqStateResponse);
    return *DecodeHwEqStateResponse(Payload.data(), static_cast<uint16>(Payload.size()));
}

void Show(const char* Step, const Status& S)
{
    static const char* Sources[] = {"manual", "device", "app"};
    std::printf("  %-36s preset='%s'%s source=%s%s%s mode=%d\n", Step, S.ActivePresetName.c_str(),
                S.bPresetModified ? " (modified)" : "", Sources[static_cast<int>(S.PresetSource)],
                S.PresetSourceApp.empty() ? "" : ":", S.PresetSourceApp.c_str(), static_cast<int>(S.Mode));
}

// --- test stream -----------------------------------------------------------

std::string WavPath;
pid_t PlayerPid = -1;

// Starts pw-play on the silent WAV as TestApp. It plays into the default
// sink, which the daemon has made its own virtual sink.
void StartPlaying()
{
    E2E_CHECK(PlayerPid < 0);
    std::fflush(stdout); // else the child's freopen flushes a copy of the buffered output
    PlayerPid = fork();
    E2E_CHECK(PlayerPid >= 0);
    if (PlayerPid == 0)
    {
        const std::string Props = std::string("{ application.name = \"") + TestApp + "\" }";
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("pw-play", "pw-play", "-P", Props.c_str(), WavPath.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
}

void StopPlaying()
{
    if (PlayerPid > 0)
    {
        kill(PlayerPid, SIGTERM);
        waitpid(PlayerPid, nullptr, 0);
        PlayerPid = -1;
    }
}

// Long enough for any scenario; silence, so nothing is audible.
void WriteSilentWav(const std::string& Path)
{
    constexpr uint32_t Rate = 48000, Channels = 2, Seconds = 60, BytesPerSample = 2;
    constexpr uint32_t DataSize = Rate * Channels * Seconds * BytesPerSample;
    std::ofstream File(Path, std::ios::binary);
    const auto U32 = [&](uint32_t V) { File.write(reinterpret_cast<const char*>(&V), 4); };
    const auto U16 = [&](uint16_t V) { File.write(reinterpret_cast<const char*>(&V), 2); };
    File.write("RIFF", 4);
    U32(36 + DataSize);
    File.write("WAVEfmt ", 8);
    U32(16);
    U16(1); // PCM
    U16(Channels);
    U32(Rate);
    U32(Rate * Channels * BytesPerSample);
    U16(Channels * BytesPerSample);
    U16(16);
    File.write("data", 4);
    U32(DataSize);
    const std::vector<char> Zeros(DataSize, 0);
    File.write(Zeros.data(), Zeros.size());
}

// Auto-switching ticks every 250ms and an app counts as playing for 2s
// after it stops (AutoSwitchGraceMs), so these leave margin around both.
constexpr int StreamSettleMs = 1500;
constexpr int GraceExpiredMs = 3200;

// --- scenarios -------------------------------------------------------------

// App binding with snapshot restore, device binding under an app,
// explicit device unbind, deleting a bound preset. One output device.
void ScenarioPresets()
{
    const std::string Device = GetStatus().OutputDeviceName;
    E2E_CHECK(!Device.empty());

    std::puts("app binding restores the previous sound:");
    Show("manual Flat", Send(EncodeSetHwEqPresetRequest("Flat")));
    Send(EncodeBindAppPresetRequest(TestApp, "Bass Boost"));
    StartPlaying();
    SleepMs(StreamSettleMs);
    bool bStreamSeen = false;
    for (const ContentStreamInfo& Stream : GetStreams())
    {
        if (Stream.AppName == TestApp)
        {
            bStreamSeen = Stream.bPlaying && Stream.BoundPresetName == "Bass Boost";
        }
    }
    E2E_CHECK(bStreamSeen);
    Status S = GetStatus();
    Show("app playing", S);
    E2E_CHECK(S.ActivePresetName == "Bass Boost" && S.PresetSource == EqPresetSource::App &&
              S.PresetSourceApp == TestApp);
    StopPlaying();
    SleepMs(1000);
    S = GetStatus();
    Show("stopped 1s (grace period)", S);
    E2E_CHECK(S.PresetSource == EqPresetSource::App);
    SleepMs(GraceExpiredMs - 1000);
    S = GetStatus();
    Show("stopped, grace over", S);
    E2E_CHECK(S.ActivePresetName == "Flat" && S.PresetSource == EqPresetSource::Manual);

    std::puts("device binding, app over it:");
    S = Send(EncodeBindDevicePresetRequest(Device, "Vocal"));
    Show("device bound to Vocal", S);
    E2E_CHECK(S.ActivePresetName == "Vocal" && S.PresetSource == EqPresetSource::Device &&
              S.DevicePresetName == "Vocal");
    StartPlaying();
    SleepMs(StreamSettleMs);
    S = GetStatus();
    Show("app playing", S);
    E2E_CHECK(S.ActivePresetName == "Bass Boost" && S.PresetSource == EqPresetSource::App);
    StopPlaying();
    SleepMs(GraceExpiredMs);
    S = GetStatus();
    Show("app stopped", S);
    E2E_CHECK(S.ActivePresetName == "Vocal" && S.PresetSource == EqPresetSource::Device);

    std::puts("explicit device unbind keeps the sound:");
    auto Bands = GetBands();
    Bands[2].GainDb = 9.0f;
    Send(EncodeSetHwEqBandRequest(2, Bands[2]));
    S = Send(EncodeBindDevicePresetRequest(Device, ""));
    Show("device unbound", S);
    E2E_CHECK(S.ActivePresetName == "Vocal" && S.bPresetModified && S.PresetSource == EqPresetSource::Manual &&
              S.DevicePresetName.empty());
    E2E_CHECK(GetBands()[2].GainDb == 9.0f);

    std::puts("deleting a bound preset drops the binding:");
    Send(EncodeSaveHwEqPresetRequest("E2E Temp"));
    Send(EncodeBindDevicePresetRequest(Device, "E2E Temp"));
    S = Send(EncodeDeleteEqPresetRequest("E2E Temp"));
    Show("bound preset deleted", S);
    E2E_CHECK(S.DevicePresetName.empty() && S.ActivePresetName.empty());
}

// Unbinding the app binding in effect keeps its sound, also once the
// output moves to an unbound device. Invalid names are refused without
// dropping the connection. Two output devices, starting on A.
void ScenarioUnbind(const std::string& DeviceA, const std::string& DeviceB)
{
    Status S = GetStatus();
    E2E_CHECK(S.OutputDeviceName == DeviceA);
    Send(EncodeSetHwEqPresetRequest("Flat"));
    S = Send(EncodeSetHwEqBandRequest(5, EqBand{EqFilterType::Peaking, 1000.0f, 7.0f, 1.0f}));
    Show("A: Flat + tweak", S);
    E2E_CHECK(S.bPresetModified);
    Send(EncodeBindDevicePresetRequest(DeviceB, "Bass Boost"));
    Send(EncodeBindAppPresetRequest(TestApp, "Vocal"));
    S = Send(EncodeSetOutputDeviceRequest(DeviceB));
    Show("-> B (device bound)", S);
    E2E_CHECK(S.ActivePresetName == "Bass Boost" && S.PresetSource == EqPresetSource::Device);
    StartPlaying();
    SleepMs(StreamSettleMs);
    S = GetStatus();
    Show("app plays on B", S);
    E2E_CHECK(S.ActivePresetName == "Vocal" && S.PresetSource == EqPresetSource::App);
    S = Send(EncodeBindAppPresetRequest(TestApp, ""));
    Show("app unbound (keeps the sound)", S);
    E2E_CHECK(S.ActivePresetName == "Vocal" && S.PresetSource == EqPresetSource::Manual);
    SleepMs(600);
    E2E_CHECK(GetStatus().ActivePresetName == "Vocal"); // not re-applied on the next tick
    StopPlaying();
    SleepMs(GraceExpiredMs);
    S = Send(EncodeSetOutputDeviceRequest(DeviceA));
    Show("-> A (unbound)", S);
    E2E_CHECK(S.ActivePresetName == "Vocal" && S.PresetSource == EqPresetSource::Manual);

    Send(EncodeBindAppPresetRequest("bad\nname", "Flat"));
    S = Send(EncodeSetOutputDeviceRequest("bad\nname"));
    E2E_CHECK(S.OutputDeviceName == DeviceA);
}

// Reverting or saving over the playing app's own bound preset keeps the
// binding in charge; picking a different preset makes a new baseline.
// Also checks PresetCatalogRevision. One output device.
void ScenarioBoundPreset()
{
    const uint32 Revision = GetStatus().PresetCatalogRevision;
    const EqBand TweakUp{EqFilterType::Peaking, 1000.0f, 7.0f, 1.0f};
    const EqBand TweakDown{EqFilterType::Peaking, 1000.0f, -5.0f, 1.0f};

    Status S = Send(EncodeSaveHwEqPresetRequest("E2E Mine"));
    E2E_CHECK(S.PresetCatalogRevision == Revision + 1);
    S = Send(EncodeSaveHwEqPresetRequest("Vocal")); // built-in: refused, no bump
    E2E_CHECK(S.PresetCatalogRevision == Revision + 1);
    Send(EncodeSetHwEqPresetRequest("Flat"));
    S = Send(EncodeSetHwEqBandRequest(5, TweakUp));
    Show("baseline: Flat + tweak", S);

    std::puts("re-picking the bound preset (revert):");
    Send(EncodeBindAppPresetRequest(TestApp, "Vocal"));
    StartPlaying();
    SleepMs(StreamSettleMs);
    E2E_CHECK(GetStatus().ActivePresetName == "Vocal");
    E2E_CHECK(Send(EncodeSetHwEqBandRequest(5, TweakDown)).bPresetModified);
    S = Send(EncodeSetHwEqPresetRequest("Vocal"));
    Show("re-pick Vocal", S);
    E2E_CHECK(!S.bPresetModified && S.PresetSource == EqPresetSource::App);
    StopPlaying();
    SleepMs(GraceExpiredMs);
    S = GetStatus();
    Show("app stopped", S);
    E2E_CHECK(S.ActivePresetName == "Flat" && S.bPresetModified && S.PresetSource == EqPresetSource::Manual);

    std::puts("saving over the bound preset:");
    Send(EncodeBindAppPresetRequest(TestApp, "E2E Mine"));
    StartPlaying();
    SleepMs(StreamSettleMs);
    E2E_CHECK(GetStatus().ActivePresetName == "E2E Mine");
    Send(EncodeSetHwEqBandRequest(5, TweakDown));
    S = Send(EncodeSaveHwEqPresetRequest("E2E Mine"));
    Show("save over E2E Mine", S);
    E2E_CHECK(!S.bPresetModified && S.PresetSource == EqPresetSource::App);
    E2E_CHECK(S.PresetCatalogRevision == Revision + 2);
    StopPlaying();
    SleepMs(GraceExpiredMs);
    S = GetStatus();
    Show("app stopped", S);
    E2E_CHECK(S.ActivePresetName == "Flat" && S.bPresetModified);

    std::puts("picking a different preset during the app:");
    StartPlaying();
    SleepMs(StreamSettleMs);
    S = Send(EncodeSetHwEqPresetRequest("Bass Boost"));
    Show("pick Bass Boost", S);
    E2E_CHECK(S.PresetSource == EqPresetSource::Manual);
    StopPlaying();
    SleepMs(GraceExpiredMs);
    S = GetStatus();
    Show("app stopped", S);
    E2E_CHECK(S.ActivePresetName == "Bass Boost");

    S = Send(EncodeDeleteEqPresetRequest("E2E Mine"));
    E2E_CHECK(S.PresetCatalogRevision == Revision + 3);
}

// Spatial mode per output device. "spatial" sets it up (starting on A
// with mode Off); "spatial-restart" checks it after a daemon restart.
void ScenarioSpatial(const std::string& DeviceA, const std::string& DeviceB)
{
    Status S = GetStatus();
    E2E_CHECK(S.OutputDeviceName == DeviceA);
    S = Send(EncodeSetSpatialModeRequest(SpatialMode::Advanced));
    Show("A: Advanced", S);
    S = Send(EncodeSetOutputDeviceRequest(DeviceB));
    Show("-> B (new device inherits)", S);
    E2E_CHECK(S.Mode == SpatialMode::Advanced);
    S = Send(EncodeSetSpatialModeRequest(SpatialMode::Off));
    Show("B: Off", S);
    S = Send(EncodeSetOutputDeviceRequest(DeviceA));
    Show("-> A", S);
    E2E_CHECK(S.Mode == SpatialMode::Advanced);
    S = Send(EncodeSetOutputDeviceRequest(DeviceB));
    Show("-> B", S);
    E2E_CHECK(S.Mode == SpatialMode::Off);
}

void ScenarioSpatialRestart(const std::string& DeviceA, const std::string& DeviceB)
{
    Status S = GetStatus();
    Show("after restart", S);
    E2E_CHECK(S.OutputDeviceName == DeviceB && S.Mode == SpatialMode::Off);
    S = Send(EncodeSetOutputDeviceRequest(DeviceA));
    Show("-> A", S);
    E2E_CHECK(S.Mode == SpatialMode::Advanced);
}

// A slider-drag-sized burst of band edits ending at FinalGain; run.sh then
// checks when settings.conf picks it up (batched saves, flush on exit).
void ScenarioBurst(float FinalGain)
{
    for (int i = 0; i < 60; ++i)
    {
        const float Gain = i == 59 ? FinalGain : static_cast<float>(i % 12) - 6.0f;
        Send(EncodeSetHwEqBandRequest(5, EqBand{EqFilterType::Peaking, 1000.0f, Gain, 1.0f}));
    }
}

} // namespace

int main(int Argc, char** Argv)
{
    const std::string Scenario = Argc > 1 ? Argv[1] : "";
    const auto Arg = [&](int Index) { return Index < Argc ? std::string(Argv[Index]) : std::string(); };

    if (Scenario == "make-wav")
    {
        WriteSilentWav(Arg(2));
        return 0;
    }

    const bool bConnected = Connect();
    if (Scenario == "ping") // quiet readiness probe for run.sh
    {
        return bConnected ? 0 : 1;
    }
    E2E_CHECK(bConnected);

    if (const char* Wav = std::getenv("RAMKOLFX_E2E_WAV"))
    {
        WavPath = Wav;
    }

    if (Scenario == "presets")
    {
        ScenarioPresets();
    }
    else if (Scenario == "unbind")
    {
        ScenarioUnbind(Arg(2), Arg(3));
    }
    else if (Scenario == "bound-preset")
    {
        ScenarioBoundPreset();
    }
    else if (Scenario == "spatial")
    {
        ScenarioSpatial(Arg(2), Arg(3));
    }
    else if (Scenario == "spatial-restart")
    {
        ScenarioSpatialRestart(Arg(2), Arg(3));
    }
    else if (Scenario == "burst")
    {
        ScenarioBurst(std::strtof(Arg(2).c_str(), nullptr));
    }
    else
    {
        std::fprintf(stderr, "unknown scenario '%s'\n", Scenario.c_str());
        return 2;
    }
    StopPlaying();
    return 0;
}
