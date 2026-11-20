// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

// Encode -> decode round trips for the control protocol, plus rejection of
// truncated/malformed payloads.

#include <string>

#include <cmath>

#include "ramkolfx/protocol.hpp"
#include "test_harness.hpp"

using namespace ramkolfx;

namespace
{
// Payload pointer/length of an encoded message (skipping the header).
const uint8* Payload(const std::vector<uint8>& Message)
{
    return Message.data() + HeaderSize;
}
uint16 PayloadLength(const std::vector<uint8>& Message)
{
    return static_cast<uint16>(Message.size() - HeaderSize);
}
} // namespace

TEST(HeaderMatchesPayload)
{
    const auto Message = EncodeSetHwEqPresetRequest("Vocal");
    const auto Header = TryReadHeader(Message.data(), Message.size());
    REQUIRE(Header);
    CHECK(Header->MessageOpcode == Opcode::SetHwEqPreset);
    CHECK(Header->PayloadLength == PayloadLength(Message));
    CHECK(!TryReadHeader(Message.data(), HeaderSize - 1));
}

TEST(StatusRoundTrip)
{
    Status In;
    In.Mode = SpatialMode::Advanced;
    In.SpeakerAzimuthDegrees[2] = 42.5f;
    In.SpeakerMuted[6] = true;
    In.bTestNoiseEnabled = true;
    In.ActiveHrtfIndex = 3;
    In.SpeakerDistanceMeters[1] = 0.75f;
    In.bNearFieldEnabled = true;
    In.OutputDeviceName = "alsa_output.usb";
    In.ActivePresetName = "My Preset";
    In.bPresetModified = true;
    In.EqRevision = 0xDEADBEEF;
    In.PresetCatalogRevision = 0x01020304;
    In.DevicePresetName = "Vocal";
    In.PresetSource = EqPresetSource::App;
    In.PresetSourceApp = "Firefox";

    const auto Message = EncodeStatusResponse(In);
    const auto Out = DecodeStatusResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    CHECK(Out->Mode == SpatialMode::Advanced);
    CHECK(Out->SpeakerAzimuthDegrees[2] == 42.5f);
    CHECK(Out->SpeakerMuted[6]);
    CHECK(Out->bTestNoiseEnabled);
    CHECK(Out->ActiveHrtfIndex == 3);
    CHECK(Out->SpeakerDistanceMeters[1] == 0.75f);
    CHECK(Out->bNearFieldEnabled);
    CHECK(Out->OutputDeviceName == "alsa_output.usb");
    CHECK(Out->ActivePresetName == "My Preset");
    CHECK(Out->bPresetModified);
    CHECK(Out->EqRevision == 0xDEADBEEF);
    CHECK(Out->PresetCatalogRevision == 0x01020304);
    CHECK(Out->DevicePresetName == "Vocal");
    CHECK(Out->PresetSource == EqPresetSource::App);
    CHECK(Out->PresetSourceApp == "Firefox");
    CHECK(Out->bHasAutoDetect && Out->AutoDetect == AutoDetectAvailability::Ok && !Out->bAutoDetectActive);

    CHECK(!DecodeStatusResponse(Payload(Message), PayloadLength(Message) - 1));
}

TEST(StatusAutoDetectFields)
{
    Status In;
    In.PresetSource = EqPresetSource::Device;
    In.AutoDetect = AutoDetectAvailability::TooSlow;
    In.bDeviceAutoDetect = true;
    In.bAutoDetectActive = true;
    In.Detected = DetectedClass::Jazz;
    In.DetectedConfidence = 82;
    In.bDetectionHeld = true;
    In.AutoDetectDevice = InferenceDevice::Npu;
    In.InferenceDeviceLabel = "NPU (OpenVINO)";
    In.DetectedTableRevision = 7;
    const auto Message = EncodeStatusResponse(In);
    const auto Out = DecodeStatusResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    CHECK(Out->AutoDetect == AutoDetectAvailability::TooSlow);
    CHECK(Out->bDeviceAutoDetect && Out->bAutoDetectActive && Out->bDetectionHeld);
    CHECK(Out->Detected == DetectedClass::Jazz && Out->DetectedConfidence == 82);
    CHECK(Out->AutoDetectDevice == InferenceDevice::Npu && Out->InferenceDeviceLabel == "NPU (OpenVINO)");
    CHECK(Out->DetectedTableRevision == 7);
    CHECK(Out->bHasAutoDetectTiming);

    // A daemon from before the timing settings: Auto-detect, no timing.
    const auto NoTiming =
        DecodeStatusResponse(Payload(Message), static_cast<uint16>(PayloadLength(Message) - AutoDetectTimingWireSize));
    REQUIRE(NoTiming);
    CHECK(NoTiming->bHasAutoDetect && NoTiming->DetectedTableRevision == 7);
    CHECK(!NoTiming->bHasAutoDetectTiming && NoTiming->DetectTiming == AutoDetectTiming{});

    // An older daemon's Status stops right after PresetSourceApp: still
    // decodes, with the defaults.
    const size_t AutoDetectBytes = 7 + 2 + In.InferenceDeviceLabel.size() + 4 + AutoDetectTimingWireSize;
    const auto Old = DecodeStatusResponse(Payload(Message), static_cast<uint16>(PayloadLength(Message) - AutoDetectBytes));
    REQUIRE(Old);
    CHECK(Old->PresetSource == EqPresetSource::Device && !Old->bAutoDetectActive && !Old->bHasAutoDetect);
    CHECK(Old->AutoDetect == AutoDetectAvailability::Ok);
    // But a partial block is malformed.
    CHECK(!DecodeStatusResponse(Payload(Message), static_cast<uint16>(PayloadLength(Message) - AutoDetectBytes + 3)));
}

TEST(StatusCarriesAutoDetectTiming)
{
    Status In;
    In.DetectTiming = {750, 0, 30000, 2.5f};
    const auto Message = EncodeStatusResponse(In);
    const auto Out = DecodeStatusResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    CHECK(Out->bHasAutoDetectTiming && Out->DetectTiming == In.DetectTiming);
}

TEST(SetAutoDetectTimingRoundTrip)
{
    const AutoDetectTiming Timing{1500, 2500, 5000, 3.0f};
    auto Message = EncodeSetAutoDetectTimingRequest(Timing);
    const auto Command = DecodeCommand(Opcode::SetAutoDetectTiming, Payload(Message), PayloadLength(Message));
    REQUIRE(Command);
    CHECK(Command->Timing == Timing);
    CHECK(!DecodeCommand(Opcode::SetAutoDetectTiming, Payload(Message), PayloadLength(Message) - 1));
}

TEST(AutoDetectTimingIsClampedIntoRange)
{
    CHECK(ClampAutoDetectTiming({}) == AutoDetectTiming{});
    const AutoDetectTiming Low = ClampAutoDetectTiming({0, 0, 0, 0.0f});
    CHECK(Low.CheckEveryMs == MinCheckEveryMs && Low.SwitchAfterMs == 0 && Low.KeepAtLeastMs == 0);
    CHECK(Low.CpuLimitPercent == MinCpuLimitPercent);
    const AutoDetectTiming High = ClampAutoDetectTiming({1u << 31, 1u << 31, 1u << 31, 1e9f});
    CHECK(High.CheckEveryMs == MaxCheckEveryMs && High.SwitchAfterMs == MaxSwitchAfterMs);
    CHECK(High.KeepAtLeastMs == MaxKeepAtLeastMs && High.CpuLimitPercent == MaxCpuLimitPercent);
    CHECK(ClampAutoDetectTiming({2000, 4000, 10000, std::nanf("")}).CpuLimitPercent == 1.0f);
}

TEST(DetectedClassesRoundTrip)
{
    DetectionReport In;
    In.DeviceLabel = "CPU";
    In.P50Us = 4200;
    In.P95Us = 5100;
    In.DutyCycle = 0.0021f;
    In.Runs = 99;
    In.RssAddedKb = 38000;
    In.Classes = {{DetectedClass::Speech, "Spoken Word", "Spoken Word", 0.25f}, {DetectedClass::Jazz, "", "Jazz", 0.75f}};
    const auto Message = EncodeDetectedClassesResponse(In);
    const auto Out = DecodeDetectedClassesResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    CHECK(Out->DeviceLabel == "CPU" && Out->P50Us == 4200 && Out->P95Us == 5100);
    CHECK(Out->DutyCycle == 0.0021f && Out->Runs == 99 && Out->RssAddedKb == 38000);
    REQUIRE(Out->Classes.size() == 2);
    CHECK(Out->Classes[0].Class == DetectedClass::Speech && Out->Classes[0].PresetName == "Spoken Word");
    CHECK(Out->Classes[0].DefaultPresetName == "Spoken Word" && Out->Classes[0].Score == 0.25f);
    CHECK(Out->Classes[1].Class == DetectedClass::Jazz && Out->Classes[1].PresetName.empty());
    CHECK(Out->Classes[1].DefaultPresetName == "Jazz" && Out->Classes[1].Score == 0.75f);
    CHECK(!DecodeDetectedClassesResponse(Payload(Message), PayloadLength(Message) - 1));
}

TEST(DeviceListRoundTrip)
{
    const std::vector<AudioDeviceInfo> In{{"a", "Alpha"}, {"b", "Beta"}, {"", ""}};
    const auto Message = EncodeDeviceListResponse(In);
    const auto Out = DecodeDeviceListResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    REQUIRE(Out->size() == 3);
    CHECK((*Out)[1].Name == "b" && (*Out)[1].Description == "Beta");
    CHECK((*Out)[2].Name.empty());
    CHECK(!DecodeDeviceListResponse(Payload(Message), PayloadLength(Message) - 1));
}

TEST(HrtfCatalogRoundTripAndEmpty)
{
    const std::vector<std::string> In{"x", "yy", "zzz"};
    auto Message = EncodeHrtfCatalogResponse(In);
    auto Out = DecodeHrtfCatalogResponse(Payload(Message), PayloadLength(Message));
    CHECK(Out && *Out == In);
    CHECK(!DecodeHrtfCatalogResponse(Payload(Message), 1));

    Message = EncodeHrtfCatalogResponse({});
    Out = DecodeHrtfCatalogResponse(Payload(Message), PayloadLength(Message));
    CHECK(Out && Out->empty());
}

TEST(ListOverflowDropsWholeEntries)
{
    std::vector<std::string> In;
    for (int i = 0; i < 1000; ++i)
    {
        In.push_back(std::string(50, static_cast<char>('a' + i % 26)));
    }
    const auto Message = EncodeHrtfCatalogResponse(In);
    CHECK(PayloadLength(Message) <= MaxPayloadSize);
    const auto Out = DecodeHrtfCatalogResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    CHECK(!Out->empty() && Out->size() < In.size());
    CHECK(Out->front() == In.front() && Out->back() == In[Out->size() - 1]);
}

TEST(EqPresetCatalogRoundTrip)
{
    const std::vector<EqPresetInfo> In{
        {"Flat", EqPresetCategory::General}, {"FPS", EqPresetCategory::Games}, {"mine", EqPresetCategory::User}};
    auto Message = EncodeEqPresetCatalogResponse(In);
    const auto Out = DecodeEqPresetCatalogResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    REQUIRE(Out->size() == 3);
    CHECK((*Out)[0].Name == "Flat" && (*Out)[0].Category == EqPresetCategory::General && (*Out)[0].IsBuiltIn());
    CHECK((*Out)[1].Name == "FPS" && (*Out)[1].Category == EqPresetCategory::Games);
    CHECK((*Out)[2].Name == "mine" && !(*Out)[2].IsBuiltIn());
    CHECK(!DecodeEqPresetCatalogResponse(Payload(Message), PayloadLength(Message) - 1));

    // An unknown category byte (the last byte of the payload) - a newer
    // daemon's - keeps the catalog, as a built-in under a generic header.
    Message.back() = static_cast<uint8>(EqPresetCategory::Count) + 3;
    const auto Newer = DecodeEqPresetCatalogResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Newer);
    REQUIRE(Newer->size() == 3);
    CHECK((*Newer)[0].Name == "Flat" && (*Newer)[2].Name == "mine");
    CHECK((*Newer)[2].IsBuiltIn());
    CHECK(std::string(EqPresetCategoryLabel((*Newer)[2].Category)) == "More");
}

TEST(ContentStreamListRoundTrip)
{
    const std::vector<ContentStreamInfo> In{{"Firefox", "Tab", true, "Vocal", false}, {"mpv", "", false, "", true}};
    const auto Message = EncodeContentStreamListResponse(In);
    const auto Out = DecodeContentStreamListResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    REQUIRE(Out->size() == 2);
    CHECK((*Out)[0].AppName == "Firefox" && (*Out)[0].MediaName == "Tab");
    CHECK((*Out)[0].bPlaying && (*Out)[0].BoundPresetName == "Vocal");
    CHECK(!(*Out)[1].bPlaying && (*Out)[1].BoundPresetName.empty());
    CHECK(!(*Out)[0].bAutoDetect && (*Out)[1].bAutoDetect);

    // An older daemon's list has no trailer: still decodes, nothing Auto.
    const auto Old = DecodeContentStreamListResponse(Payload(Message), PayloadLength(Message) - 2);
    REQUIRE(Old && Old->size() == 2);
    CHECK((*Old)[1].AppName == "mpv" && !(*Old)[1].bAutoDetect);
    CHECK(!DecodeContentStreamListResponse(Payload(Message), PayloadLength(Message) - 3));
}

TEST(ContentStreamListOverflowKeepsTheTrailer)
{
    std::vector<ContentStreamInfo> In;
    for (int i = 0; i < 200; ++i)
    {
        In.push_back({"app" + std::to_string(i), std::string(30, 'm'), true, "Preset", i % 2 == 1});
    }
    const auto Message = EncodeContentStreamListResponse(In);
    CHECK(PayloadLength(Message) <= MaxPayloadSize);
    const auto Out = DecodeContentStreamListResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out && !Out->empty() && Out->size() < In.size());
    for (size_t i = 0; i < Out->size(); ++i)
    {
        CHECK((*Out)[i].bAutoDetect == (i % 2 == 1));
    }
}

TEST(HwEqStateAndBassRoundTrip)
{
    std::array<EqBand, MaxEqBands> Bands{};
    Bands[4] = {EqFilterType::HighShelf, 4000.0f, -3.5f, 0.9f};
    auto Message = EncodeHwEqStateResponse(Bands);
    const auto OutBands = DecodeHwEqStateResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(OutBands);
    CHECK((*OutBands)[4].FilterType == EqFilterType::HighShelf && (*OutBands)[4].GainDb == -3.5f);

    BassEnhancerSettings Bass{true, 80.0f, 0.3f, 0.7f};
    Message = EncodeBassEnhancerStateResponse(Bass);
    const auto OutBass = DecodeBassEnhancerStateResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(OutBass);
    CHECK(OutBass->bEnabled && OutBass->CutoffHz == 80.0f && OutBass->Mix == 0.7f);
}

TEST(PresetNameRequests)
{
    for (const Opcode Op : {Opcode::SetHwEqPreset, Opcode::SaveHwEqPreset, Opcode::DeleteEqPreset})
    {
        const auto Message = Op == Opcode::SetHwEqPreset    ? EncodeSetHwEqPresetRequest("x y")
                             : Op == Opcode::SaveHwEqPreset ? EncodeSaveHwEqPresetRequest("x y")
                                                            : EncodeDeleteEqPresetRequest("x y");
        const auto Command = DecodeCommand(Op, Payload(Message), PayloadLength(Message));
        CHECK(Command && Command->CommandOpcode == Op && Command->PresetName == "x y");
    }
    const auto Empty = EncodeSetHwEqPresetRequest("");
    CHECK(!DecodeCommand(Opcode::SetHwEqPreset, Payload(Empty), PayloadLength(Empty)));
}

TEST(BindRequests)
{
    auto Message = EncodeBindDevicePresetRequest("alsa_out", "Vocal");
    auto Command = DecodeCommand(Opcode::BindDevicePreset, Payload(Message), PayloadLength(Message));
    REQUIRE(Command);
    CHECK(Command->OutputDeviceName == "alsa_out" && Command->PresetName == "Vocal");

    // Empty preset = unbind, which must still decode.
    Message = EncodeBindAppPresetRequest("mpv", "");
    Command = DecodeCommand(Opcode::BindAppPreset, Payload(Message), PayloadLength(Message));
    REQUIRE(Command);
    CHECK(Command->ContentAppName == "mpv" && Command->PresetName.empty());

    // Empty target is meaningless and rejected.
    Message = EncodeBindAppPresetRequest("", "Vocal");
    CHECK(!DecodeCommand(Opcode::BindAppPreset, Payload(Message), PayloadLength(Message)));
}

TEST(AutoDetectRequests)
{
    auto Message = EncodeBindAppAutoDetectRequest("Firefox");
    auto Command = DecodeCommand(Opcode::BindAppAutoDetect, Payload(Message), PayloadLength(Message));
    CHECK(Command && Command->ContentAppName == "Firefox");
    Message = EncodeBindDeviceAutoDetectRequest("alsa_out");
    Command = DecodeCommand(Opcode::BindDeviceAutoDetect, Payload(Message), PayloadLength(Message));
    CHECK(Command && Command->OutputDeviceName == "alsa_out");
    Message = EncodeBindAppAutoDetectRequest("");
    CHECK(!DecodeCommand(Opcode::BindAppAutoDetect, Payload(Message), PayloadLength(Message)));

    Message = EncodeBindDetectedClassRequest(DetectedClass::RnB, "My Soul");
    Command = DecodeCommand(Opcode::BindDetectedClass, Payload(Message), PayloadLength(Message));
    CHECK(Command && Command->ClassValue == DetectedClass::RnB && Command->PresetName == "My Soul");
    Message = EncodeBindDetectedClassRequest(DetectedClass::Speech, ""); // no switch
    Command = DecodeCommand(Opcode::BindDetectedClass, Payload(Message), PayloadLength(Message));
    CHECK(Command && Command->ClassValue == DetectedClass::Speech && Command->PresetName.empty());
    for (const DetectedClass Bad : {DetectedClass::None, DetectedClass::Count})
    {
        Message = EncodeBindDetectedClassRequest(Bad, "x");
        CHECK(!DecodeCommand(Opcode::BindDetectedClass, Payload(Message), PayloadLength(Message)));
    }

    Message = EncodeSetAutoDetectDeviceRequest(InferenceDevice::Gpu);
    Command = DecodeCommand(Opcode::SetAutoDetectDevice, Payload(Message), PayloadLength(Message));
    CHECK(Command && Command->DeviceValue == InferenceDevice::Gpu);
    Message = EncodeSetAutoDetectDeviceRequest(InferenceDevice::Count);
    CHECK(!DecodeCommand(Opcode::SetAutoDetectDevice, Payload(Message), PayloadLength(Message)));

    Message = EncodeGetDetectedClassesRequest();
    CHECK(DecodeCommand(Opcode::GetDetectedClasses, Payload(Message), PayloadLength(Message)));
}

TEST(ChannelLevelsRoundTrip)
{
    const auto Request = EncodeGetChannelLevelsRequest();
    const auto Command = DecodeCommand(Opcode::GetChannelLevels, Payload(Request), PayloadLength(Request));
    CHECK(Command && Command->CommandOpcode == Opcode::GetChannelLevels);

    ChannelLevels In;
    In.SpeakerPeak = {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f};
    In.LfePeak = 0.125f;
    const auto Message = EncodeChannelLevelsResponse(In);
    const auto Header = TryReadHeader(Message.data(), Message.size());
    REQUIRE(Header);
    CHECK(Header->MessageOpcode == Opcode::ChannelLevelsResponse);
    const auto Out = DecodeChannelLevelsResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    CHECK(Out->SpeakerPeak == In.SpeakerPeak);
    CHECK(Out->LfePeak == In.LfePeak);
    CHECK(!DecodeChannelLevelsResponse(Payload(Message), PayloadLength(Message) - 1));
}

TEST(RetiredOpcodesRejected)
{
    const uint8 Dummy[4] = {0, 1, 'x', 0};
    CHECK(!DecodeCommand(static_cast<Opcode>(0x11), Dummy, sizeof(Dummy)));
    CHECK(!DecodeCommand(static_cast<Opcode>(0x13), Dummy, sizeof(Dummy)));
}

TEST(AutoDetectKeysRoundTrip)
{
    for (size_t i = 1; i < DetectedClassCount; ++i)
    {
        const auto Class = static_cast<DetectedClass>(i);
        REQUIRE(DetectedClassKey(Class) != nullptr);
        CHECK(DetectedClassFromKey(DetectedClassKey(Class)) == Class);
    }
    CHECK(DetectedClassKey(DetectedClass::None) == nullptr);
    CHECK(!DetectedClassFromKey("polka"));

    for (size_t i = 0; i < static_cast<size_t>(InferenceDevice::Count); ++i)
    {
        const auto Device = static_cast<InferenceDevice>(i);
        CHECK(InferenceDeviceFromKey(InferenceDeviceKey(Device)) == Device);
    }
    CHECK(!InferenceDeviceFromKey("tpu"));
    CHECK(std::string(AutoDetectAvailabilityLabel(AutoDetectAvailability::Ok)).empty());
    CHECK(!std::string(AutoDetectAvailabilityLabel(AutoDetectAvailability::Count)).empty());
}

RAMKOLFX_TEST_MAIN()
