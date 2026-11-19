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

    CHECK(!DecodeStatusResponse(Payload(Message), PayloadLength(Message) - 1));
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
    const std::vector<ContentStreamInfo> In{{"Firefox", "Tab", true, "Vocal"}, {"mpv", "", false, ""}};
    const auto Message = EncodeContentStreamListResponse(In);
    const auto Out = DecodeContentStreamListResponse(Payload(Message), PayloadLength(Message));
    REQUIRE(Out);
    REQUIRE(Out->size() == 2);
    CHECK((*Out)[0].AppName == "Firefox" && (*Out)[0].MediaName == "Tab");
    CHECK((*Out)[0].bPlaying && (*Out)[0].BoundPresetName == "Vocal");
    CHECK(!(*Out)[1].bPlaying && (*Out)[1].BoundPresetName.empty());
    CHECK(!DecodeContentStreamListResponse(Payload(Message), PayloadLength(Message) - 1));
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

TEST(RetiredOpcodesRejected)
{
    const uint8 Dummy[4] = {0, 1, 'x', 0};
    CHECK(!DecodeCommand(static_cast<Opcode>(0x11), Dummy, sizeof(Dummy)));
    CHECK(!DecodeCommand(static_cast<Opcode>(0x13), Dummy, sizeof(Dummy)));
}

RAMKOLFX_TEST_MAIN()
