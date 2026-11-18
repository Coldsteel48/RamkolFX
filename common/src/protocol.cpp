// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "ramkolfx/protocol.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ramkolfx
{

namespace
{

void AppendHeader(std::vector<uint8>& Out, Opcode InOpcode, uint16 PayloadLength)
{
    Out.push_back(static_cast<uint8>(InOpcode));
    Out.push_back(static_cast<uint8>((PayloadLength >> 8) & 0xFF));
    Out.push_back(static_cast<uint8>(PayloadLength & 0xFF));
}

// A message whose whole payload is one raw string (no length prefix - the
// header already carries the length): ErrorResponse, SetOutputDevice and
// the preset-name requests. Truncated to MaxPayloadSize.
std::vector<uint8> EncodeRawStringMessage(Opcode InOpcode, const std::string& Value)
{
    const size_t Len = std::min(Value.size(), static_cast<size_t>(MaxPayloadSize));
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + Len);
    AppendHeader(Out, InOpcode, static_cast<uint16>(Len));
    Out.insert(Out.end(), Value.begin(), Value.begin() + static_cast<std::ptrdiff_t>(Len));
    return Out;
}

void AppendU32BE(std::vector<uint8>& Out, uint32 Value)
{
    Out.push_back(static_cast<uint8>((Value >> 24) & 0xFF));
    Out.push_back(static_cast<uint8>((Value >> 16) & 0xFF));
    Out.push_back(static_cast<uint8>((Value >> 8) & 0xFF));
    Out.push_back(static_cast<uint8>(Value & 0xFF));
}

uint32 ReadU32BE(const uint8* Payload)
{
    return (static_cast<uint32>(Payload[0]) << 24) | (static_cast<uint32>(Payload[1]) << 16) |
           (static_cast<uint32>(Payload[2]) << 8) | static_cast<uint32>(Payload[3]);
}

void AppendFloatBE(std::vector<uint8>& Out, float Value)
{
    uint32 Bits;
    std::memcpy(&Bits, &Value, sizeof(Bits));
    AppendU32BE(Out, Bits);
}

float ReadFloatBE(const uint8* Payload)
{
    const uint32 Bits = ReadU32BE(Payload);
    float Value;
    std::memcpy(&Value, &Bits, sizeof(Value));
    return Value;
}

void AppendEqBand(std::vector<uint8>& Out, const EqBand& Band)
{
    Out.push_back(static_cast<uint8>(Band.FilterType));
    AppendFloatBE(Out, Band.FrequencyHz);
    AppendFloatBE(Out, Band.GainDb);
    AppendFloatBE(Out, Band.Q);
}

// Reads a fixed EqBandWireSize-byte EqBand from `Payload`. Caller must have
// already verified that many bytes are available.
EqBand ReadEqBand(const uint8* Payload)
{
    EqBand Band;
    Band.FilterType = static_cast<EqFilterType>(Payload[0]);
    Band.FrequencyHz = ReadFloatBE(Payload + 1);
    Band.GainDb = ReadFloatBE(Payload + 5);
    Band.Q = ReadFloatBE(Payload + 9);
    return Band;
}

void AppendBassEnhancerSettings(std::vector<uint8>& Out, const BassEnhancerSettings& Settings)
{
    Out.push_back(Settings.bEnabled ? 1 : 0);
    AppendFloatBE(Out, Settings.CutoffHz);
    AppendFloatBE(Out, Settings.Drive);
    AppendFloatBE(Out, Settings.Mix);
}

// Reads a fixed BassEnhancerWireSize-byte BassEnhancerSettings from
// `Payload`. Caller must have already verified that many bytes are
// available.
BassEnhancerSettings ReadBassEnhancerSettings(const uint8* Payload)
{
    BassEnhancerSettings Settings;
    Settings.bEnabled = Payload[0] != 0;
    Settings.CutoffHz = ReadFloatBE(Payload + 1);
    Settings.Drive = ReadFloatBE(Payload + 5);
    Settings.Mix = ReadFloatBE(Payload + 9);
    return Settings;
}

// Appends a length-prefixed (u16 BE) string. Used for fields embedded
// alongside fixed-size data (Status, DeviceListResponse) where a raw
// whole-payload string (like ErrorResponse's) wouldn't be unambiguous.
void AppendString(std::vector<uint8>& Out, const std::string& Value)
{
    const uint16 Len = static_cast<uint16>(std::min(Value.size(), static_cast<size_t>(0xFFFF)));
    Out.push_back(static_cast<uint8>((Len >> 8) & 0xFF));
    Out.push_back(static_cast<uint8>(Len & 0xFF));
    Out.insert(Out.end(), Value.begin(), Value.begin() + Len);
}

// Reads a length-prefixed string starting at `Offset`, advancing it past
// the string on success. Returns false (leaving OutValue untouched) if
// PayloadLength doesn't have enough bytes for the length or the string.
bool ReadString(const uint8* Payload, uint16 PayloadLength, size_t& Offset, std::string& OutValue)
{
    if (Offset + 2 > PayloadLength)
    {
        return false;
    }
    const uint16 Len = static_cast<uint16>((Payload[Offset] << 8) | Payload[Offset + 1]);
    Offset += 2;
    if (Offset + Len > PayloadLength)
    {
        return false;
    }
    OutValue.assign(reinterpret_cast<const char*>(Payload + Offset), Len);
    Offset += Len;
    return true;
}

// Shared shape behind every *ListResponse decoder: a u16 BE entry count
// followed by that many entries, each parsed by DecodeEntry (which must
// advance Offset past its own entry and return false on truncation/
// malformed data, same contract as ReadString). Pulled out once every
// list response (devices, HRTF catalog, content streams, EQ presets) was
// found to repeat this exact count-then-loop skeleton with only the
// per-entry decoding differing.
template <typename T, typename EntryDecoder>
std::optional<std::vector<T>> DecodeListResponse(const uint8* Payload, uint16 PayloadLength,
                                                  EntryDecoder&& DecodeEntry)
{
    size_t Offset = 0;
    if (Offset + 2 > PayloadLength)
    {
        return std::nullopt;
    }
    const uint16 Count = static_cast<uint16>((Payload[Offset] << 8) | Payload[Offset + 1]);
    Offset += 2;

    std::vector<T> Items;
    Items.reserve(Count);
    for (uint16 i = 0; i < Count; ++i)
    {
        T Item;
        if (!DecodeEntry(Payload, PayloadLength, Offset, Item))
        {
            return std::nullopt;
        }
        Items.push_back(std::move(Item));
    }
    return Items;
}

// Shared shape behind every *ListResponse encoder: a u16 BE entry count
// (patched in once the final count is known) followed by each entry
// EncodeEntry appends, stopping before - never mid-entry - a further
// entry would push the payload past MaxPayloadSize. Mirrors
// DecodeListResponse above; pulled out for the same reason.
template <typename T, typename EntryEncoder>
std::vector<uint8> EncodeListResponse(Opcode InOpcode, const std::vector<T>& Items, EntryEncoder&& EncodeEntry)
{
    std::vector<uint8> Payload;
    Payload.push_back(0); // count placeholder, patched below once the final count is known
    Payload.push_back(0);

    uint16 Count = 0;
    for (const T& Item : Items)
    {
        std::vector<uint8> Entry;
        EncodeEntry(Entry, Item);
        if (Payload.size() + Entry.size() > MaxPayloadSize)
        {
            break; // stop before exceeding the payload cap, never mid-entry
        }
        Payload.insert(Payload.end(), Entry.begin(), Entry.end());
        ++Count;
    }
    Payload[0] = static_cast<uint8>((Count >> 8) & 0xFF);
    Payload[1] = static_cast<uint8>(Count & 0xFF);

    std::vector<uint8> Out;
    Out.reserve(HeaderSize + Payload.size());
    AppendHeader(Out, InOpcode, static_cast<uint16>(Payload.size()));
    Out.insert(Out.end(), Payload.begin(), Payload.end());
    return Out;
}

} // namespace

std::optional<MessageHeader> TryReadHeader(const uint8* Buffer, size_t Available)
{
    if (Available < HeaderSize)
    {
        return std::nullopt;
    }
    MessageHeader Header;
    Header.MessageOpcode = static_cast<Opcode>(Buffer[0]);
    Header.PayloadLength = static_cast<uint16>((Buffer[1] << 8) | Buffer[2]);
    return Header;
}

std::optional<Command> DecodeCommand(Opcode InOpcode, const uint8* Payload, uint16 PayloadLength)
{
    Command OutCommand;
    OutCommand.CommandOpcode = InOpcode;

    switch (InOpcode)
    {
    case Opcode::GetStatus:
        return OutCommand;
    case Opcode::SetSpatialMode:
        if (PayloadLength < 1 || Payload[0] > static_cast<uint8>(SpatialMode::Advanced))
        {
            return std::nullopt;
        }
        OutCommand.ModeValue = static_cast<SpatialMode>(Payload[0]);
        return OutCommand;
    case Opcode::SetSpeakerAzimuth:
        if (PayloadLength < 5 || Payload[0] >= SpeakerCount)
        {
            return std::nullopt;
        }
        OutCommand.SpeakerIndex = Payload[0];
        OutCommand.AzimuthDegrees = ReadFloatBE(Payload + 1);
        return OutCommand;
    case Opcode::GetDevices:
        return OutCommand;
    case Opcode::SetOutputDevice:
        // Raw whole-payload string, same convention as ErrorResponse: the
        // message envelope already carries the length, so no extra prefix
        // is needed here.
        OutCommand.OutputDeviceName = std::string(reinterpret_cast<const char*>(Payload), PayloadLength);
        return OutCommand;
    case Opcode::ResetSpeakerPositions:
        return OutCommand;
    case Opcode::SetSpeakerMute:
        if (PayloadLength < 2 || Payload[0] >= SpeakerCount)
        {
            return std::nullopt;
        }
        OutCommand.SpeakerIndex = Payload[0];
        OutCommand.bMuted = Payload[1] != 0;
        return OutCommand;
    case Opcode::SetTestNoise:
        if (PayloadLength < 1)
        {
            return std::nullopt;
        }
        OutCommand.bTestNoiseEnabled = Payload[0] != 0;
        return OutCommand;
    case Opcode::GetHrtfCatalog:
        return OutCommand;
    case Opcode::SetHrtfFile:
        if (PayloadLength < 1)
        {
            return std::nullopt;
        }
        OutCommand.HrtfIndex = Payload[0];
        return OutCommand;
    case Opcode::SetSpeakerDistance:
        if (PayloadLength < 5 || Payload[0] >= SpeakerCount)
        {
            return std::nullopt;
        }
        OutCommand.SpeakerIndex = Payload[0];
        OutCommand.DistanceMeters = ReadFloatBE(Payload + 1);
        return OutCommand;
    case Opcode::SetNearFieldEnabled:
        if (PayloadLength < 1)
        {
            return std::nullopt;
        }
        OutCommand.bNearFieldEnabled = Payload[0] != 0;
        return OutCommand;
    case Opcode::SetHwEqBand:
    {
        const uint16 Needed = static_cast<uint16>(1 + EqBandWireSize);
        if (PayloadLength < Needed || Payload[0] >= MaxEqBands ||
            Payload[1] > static_cast<uint8>(EqFilterType::Notch))
        {
            return std::nullopt;
        }
        OutCommand.BandIndex = Payload[0];
        OutCommand.Band = ReadEqBand(Payload + 1);
        return OutCommand;
    }
    case Opcode::SetHwEqPreset:
    case Opcode::SaveHwEqPreset:
    case Opcode::DeleteEqPreset:
        // Raw whole-payload preset name, same convention as SetOutputDevice.
        if (PayloadLength == 0)
        {
            return std::nullopt;
        }
        OutCommand.PresetName = std::string(reinterpret_cast<const char*>(Payload), PayloadLength);
        return OutCommand;
    case Opcode::GetHwEqState:
        return OutCommand;
    case Opcode::SetBassEnhancer:
        if (PayloadLength != static_cast<uint16>(BassEnhancerWireSize))
        {
            return std::nullopt;
        }
        OutCommand.BassEnhancer = ReadBassEnhancerSettings(Payload);
        return OutCommand;
    case Opcode::GetBassEnhancerState:
        return OutCommand;
    case Opcode::GetContentStreams:
        return OutCommand;
    case Opcode::BindAppPreset:
    case Opcode::BindDevicePreset:
    {
        // [target: length-prefixed string][preset name: raw remainder,
        // empty = unbind]
        std::string& Target = InOpcode == Opcode::BindAppPreset ? OutCommand.ContentAppName
                                                                 : OutCommand.OutputDeviceName;
        size_t Offset = 0;
        if (!ReadString(Payload, PayloadLength, Offset, Target) || Target.empty())
        {
            return std::nullopt;
        }
        OutCommand.PresetName =
            std::string(reinterpret_cast<const char*>(Payload + Offset), PayloadLength - Offset);
        return OutCommand;
    }
    case Opcode::GetEqPresetCatalog:
        return OutCommand;
    default:
        return std::nullopt;
    }
}

std::optional<Status> DecodeStatusResponse(const uint8* Payload, uint16 PayloadLength)
{
    // Layout: [mode:1][azimuths: 4*SpeakerCount][muted: SpeakerCount][noise:1][activeHrtfIndex:1]
    //         [distances: 4*SpeakerCount][nearFieldEnabled:1][device: length-prefixed string]
    //         [activePreset: length-prefixed string][presetModified:1][eqRevision: u32]
    //         [presetCatalogRevision: u32]
    //         [devicePreset: length-prefixed string][presetSource:1][presetSourceApp: length-prefixed string]
    const uint16 FixedSize =
        static_cast<uint16>(1 + 4 * SpeakerCount + SpeakerCount + 1 + 1 + 4 * SpeakerCount + 1);
    if (PayloadLength < FixedSize || Payload[0] > static_cast<uint8>(SpatialMode::Advanced))
    {
        return std::nullopt;
    }
    Status OutStatus;
    OutStatus.Mode = static_cast<SpatialMode>(Payload[0]);
    for (size_t i = 0; i < SpeakerCount; ++i)
    {
        OutStatus.SpeakerAzimuthDegrees[i] = ReadFloatBE(Payload + 1 + 4 * i);
    }
    const size_t MutedOffset = 1 + 4 * SpeakerCount;
    for (size_t i = 0; i < SpeakerCount; ++i)
    {
        OutStatus.SpeakerMuted[i] = Payload[MutedOffset + i] != 0;
    }
    OutStatus.bTestNoiseEnabled = Payload[MutedOffset + SpeakerCount] != 0;
    OutStatus.ActiveHrtfIndex = Payload[MutedOffset + SpeakerCount + 1];

    const size_t DistanceOffset = MutedOffset + SpeakerCount + 2;
    for (size_t i = 0; i < SpeakerCount; ++i)
    {
        OutStatus.SpeakerDistanceMeters[i] = ReadFloatBE(Payload + DistanceOffset + 4 * i);
    }
    OutStatus.bNearFieldEnabled = Payload[DistanceOffset + 4 * SpeakerCount] != 0;

    size_t Offset = FixedSize;
    if (!ReadString(Payload, PayloadLength, Offset, OutStatus.OutputDeviceName) ||
        !ReadString(Payload, PayloadLength, Offset, OutStatus.ActivePresetName) ||
        Offset + 1 + 4 > PayloadLength)
    {
        return std::nullopt;
    }
    OutStatus.bPresetModified = Payload[Offset] != 0;
    OutStatus.EqRevision = ReadU32BE(Payload + Offset + 1);
    Offset += 1 + 4;
    if (Offset + 4 > PayloadLength)
    {
        return std::nullopt;
    }
    OutStatus.PresetCatalogRevision = ReadU32BE(Payload + Offset);
    Offset += 4;
    if (!ReadString(Payload, PayloadLength, Offset, OutStatus.DevicePresetName) || Offset + 1 > PayloadLength ||
        Payload[Offset] > static_cast<uint8>(EqPresetSource::App))
    {
        return std::nullopt;
    }
    OutStatus.PresetSource = static_cast<EqPresetSource>(Payload[Offset++]);
    if (!ReadString(Payload, PayloadLength, Offset, OutStatus.PresetSourceApp))
    {
        return std::nullopt;
    }
    return OutStatus;
}

std::string DecodeErrorResponse(const uint8* Payload, uint16 PayloadLength)
{
    return std::string(reinterpret_cast<const char*>(Payload), PayloadLength);
}

std::optional<std::vector<AudioDeviceInfo>> DecodeDeviceListResponse(const uint8* Payload,
                                                                       uint16 PayloadLength)
{
    return DecodeListResponse<AudioDeviceInfo>(
        Payload, PayloadLength,
        [](const uint8* P, uint16 Len, size_t& Offset, AudioDeviceInfo& Info)
        {
            return ReadString(P, Len, Offset, Info.Name) && ReadString(P, Len, Offset, Info.Description);
        });
}

std::optional<std::vector<std::string>> DecodeHrtfCatalogResponse(const uint8* Payload,
                                                                    uint16 PayloadLength)
{
    return DecodeListResponse<std::string>(Payload, PayloadLength, ReadString);
}

std::optional<std::array<EqBand, MaxEqBands>> DecodeHwEqStateResponse(const uint8* Payload,
                                                                        uint16 PayloadLength)
{
    if (PayloadLength != static_cast<uint16>(MaxEqBands * EqBandWireSize))
    {
        return std::nullopt;
    }
    std::array<EqBand, MaxEqBands> Bands;
    for (size_t i = 0; i < MaxEqBands; ++i)
    {
        Bands[i] = ReadEqBand(Payload + i * EqBandWireSize);
    }
    return Bands;
}

std::optional<BassEnhancerSettings> DecodeBassEnhancerStateResponse(const uint8* Payload,
                                                                       uint16 PayloadLength)
{
    if (PayloadLength != static_cast<uint16>(BassEnhancerWireSize))
    {
        return std::nullopt;
    }
    return ReadBassEnhancerSettings(Payload);
}

std::optional<std::vector<ContentStreamInfo>> DecodeContentStreamListResponse(const uint8* Payload,
                                                                                 uint16 PayloadLength)
{
    return DecodeListResponse<ContentStreamInfo>(
        Payload, PayloadLength,
        [](const uint8* P, uint16 Len, size_t& Offset, ContentStreamInfo& Info)
        {
            if (!ReadString(P, Len, Offset, Info.AppName) || !ReadString(P, Len, Offset, Info.MediaName) ||
                Offset + 1 > Len)
            {
                return false;
            }
            Info.bPlaying = P[Offset++] != 0;
            return ReadString(P, Len, Offset, Info.BoundPresetName);
        });
}

std::optional<std::vector<EqPresetInfo>> DecodeEqPresetCatalogResponse(const uint8* Payload,
                                                                         uint16 PayloadLength)
{
    return DecodeListResponse<EqPresetInfo>(
        Payload, PayloadLength,
        [](const uint8* P, uint16 Len, size_t& Offset, EqPresetInfo& Info)
        {
            if (!ReadString(P, Len, Offset, Info.Name) || Offset + 1 > Len)
            {
                return false;
            }
            Info.bBuiltIn = P[Offset++] != 0;
            return true;
        });
}

std::vector<uint8> EncodeStatusResponse(const Status& InStatus)
{
    std::vector<uint8> Payload;
    Payload.push_back(static_cast<uint8>(InStatus.Mode));
    for (float Azimuth : InStatus.SpeakerAzimuthDegrees)
    {
        AppendFloatBE(Payload, Azimuth);
    }
    for (bool bMuted : InStatus.SpeakerMuted)
    {
        Payload.push_back(bMuted ? 1 : 0);
    }
    Payload.push_back(InStatus.bTestNoiseEnabled ? 1 : 0);
    Payload.push_back(InStatus.ActiveHrtfIndex);
    for (float Distance : InStatus.SpeakerDistanceMeters)
    {
        AppendFloatBE(Payload, Distance);
    }
    Payload.push_back(InStatus.bNearFieldEnabled ? 1 : 0);
    AppendString(Payload, InStatus.OutputDeviceName);
    AppendString(Payload, InStatus.ActivePresetName);
    Payload.push_back(InStatus.bPresetModified ? 1 : 0);
    AppendU32BE(Payload, InStatus.EqRevision);
    AppendU32BE(Payload, InStatus.PresetCatalogRevision);
    AppendString(Payload, InStatus.DevicePresetName);
    Payload.push_back(static_cast<uint8>(InStatus.PresetSource));
    AppendString(Payload, InStatus.PresetSourceApp);

    std::vector<uint8> Out;
    Out.reserve(HeaderSize + Payload.size());
    AppendHeader(Out, Opcode::StatusResponse, static_cast<uint16>(Payload.size()));
    Out.insert(Out.end(), Payload.begin(), Payload.end());
    return Out;
}

std::vector<uint8> EncodeErrorResponse(const std::string& Message)
{
    return EncodeRawStringMessage(Opcode::ErrorResponse, Message);
}

std::vector<uint8> EncodeDeviceListResponse(const std::vector<AudioDeviceInfo>& Devices)
{
    return EncodeListResponse(Opcode::DeviceListResponse, Devices,
                               [](std::vector<uint8>& Entry, const AudioDeviceInfo& Device)
                               {
                                   AppendString(Entry, Device.Name);
                                   AppendString(Entry, Device.Description);
                               });
}

std::vector<uint8> EncodeHrtfCatalogResponse(const std::vector<std::string>& DisplayNames)
{
    return EncodeListResponse(Opcode::HrtfCatalogResponse, DisplayNames,
                               [](std::vector<uint8>& Entry, const std::string& Name)
                               { AppendString(Entry, Name); });
}

std::vector<uint8> EncodeHwEqStateResponse(const std::array<EqBand, MaxEqBands>& Bands)
{
    std::vector<uint8> Payload;
    Payload.reserve(MaxEqBands * EqBandWireSize);
    for (const EqBand& Band : Bands)
    {
        AppendEqBand(Payload, Band);
    }

    std::vector<uint8> Out;
    Out.reserve(HeaderSize + Payload.size());
    AppendHeader(Out, Opcode::HwEqStateResponse, static_cast<uint16>(Payload.size()));
    Out.insert(Out.end(), Payload.begin(), Payload.end());
    return Out;
}

std::vector<uint8> EncodeBassEnhancerStateResponse(const BassEnhancerSettings& Settings)
{
    std::vector<uint8> Payload;
    Payload.reserve(BassEnhancerWireSize);
    AppendBassEnhancerSettings(Payload, Settings);

    std::vector<uint8> Out;
    Out.reserve(HeaderSize + Payload.size());
    AppendHeader(Out, Opcode::BassEnhancerStateResponse, static_cast<uint16>(Payload.size()));
    Out.insert(Out.end(), Payload.begin(), Payload.end());
    return Out;
}

std::vector<uint8> EncodeContentStreamListResponse(const std::vector<ContentStreamInfo>& Streams)
{
    return EncodeListResponse(Opcode::ContentStreamListResponse, Streams,
                               [](std::vector<uint8>& Entry, const ContentStreamInfo& Stream)
                               {
                                   AppendString(Entry, Stream.AppName);
                                   AppendString(Entry, Stream.MediaName);
                                   Entry.push_back(Stream.bPlaying ? 1 : 0);
                                   AppendString(Entry, Stream.BoundPresetName);
                               });
}

std::vector<uint8> EncodeEqPresetCatalogResponse(const std::vector<EqPresetInfo>& Presets)
{
    return EncodeListResponse(Opcode::EqPresetCatalogResponse, Presets,
                               [](std::vector<uint8>& Entry, const EqPresetInfo& Info)
                               {
                                   AppendString(Entry, Info.Name);
                                   Entry.push_back(Info.bBuiltIn ? 1 : 0);
                               });
}

std::vector<uint8> EncodeGetStatusRequest()
{
    std::vector<uint8> Out;
    AppendHeader(Out, Opcode::GetStatus, 0);
    return Out;
}

std::vector<uint8> EncodeSetSpatialModeRequest(SpatialMode Mode)
{
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + 1);
    AppendHeader(Out, Opcode::SetSpatialMode, 1);
    Out.push_back(static_cast<uint8>(Mode));
    return Out;
}

std::vector<uint8> EncodeSetSpeakerAzimuthRequest(uint8 SpeakerIndex, float AzimuthDegrees)
{
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + 5);
    AppendHeader(Out, Opcode::SetSpeakerAzimuth, 5);
    Out.push_back(SpeakerIndex);
    AppendFloatBE(Out, AzimuthDegrees);
    return Out;
}

std::vector<uint8> EncodeGetDevicesRequest()
{
    std::vector<uint8> Out;
    AppendHeader(Out, Opcode::GetDevices, 0);
    return Out;
}

std::vector<uint8> EncodeSetOutputDeviceRequest(const std::string& DeviceName)
{
    return EncodeRawStringMessage(Opcode::SetOutputDevice, DeviceName);
}

std::vector<uint8> EncodeResetSpeakerPositionsRequest()
{
    std::vector<uint8> Out;
    AppendHeader(Out, Opcode::ResetSpeakerPositions, 0);
    return Out;
}

std::vector<uint8> EncodeSetSpeakerMuteRequest(uint8 SpeakerIndex, bool bMuted)
{
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + 2);
    AppendHeader(Out, Opcode::SetSpeakerMute, 2);
    Out.push_back(SpeakerIndex);
    Out.push_back(bMuted ? 1 : 0);
    return Out;
}

std::vector<uint8> EncodeSetTestNoiseRequest(bool bEnabled)
{
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + 1);
    AppendHeader(Out, Opcode::SetTestNoise, 1);
    Out.push_back(bEnabled ? 1 : 0);
    return Out;
}

std::vector<uint8> EncodeGetHrtfCatalogRequest()
{
    std::vector<uint8> Out;
    AppendHeader(Out, Opcode::GetHrtfCatalog, 0);
    return Out;
}

std::vector<uint8> EncodeSetHrtfFileRequest(uint8 HrtfIndex)
{
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + 1);
    AppendHeader(Out, Opcode::SetHrtfFile, 1);
    Out.push_back(HrtfIndex);
    return Out;
}

std::vector<uint8> EncodeSetSpeakerDistanceRequest(uint8 SpeakerIndex, float DistanceMeters)
{
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + 5);
    AppendHeader(Out, Opcode::SetSpeakerDistance, 5);
    Out.push_back(SpeakerIndex);
    AppendFloatBE(Out, DistanceMeters);
    return Out;
}

std::vector<uint8> EncodeSetNearFieldEnabledRequest(bool bEnabled)
{
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + 1);
    AppendHeader(Out, Opcode::SetNearFieldEnabled, 1);
    Out.push_back(bEnabled ? 1 : 0);
    return Out;
}

std::vector<uint8> EncodeSetHwEqBandRequest(uint8 BandIndex, const EqBand& Band)
{
    std::vector<uint8> Out;
    const size_t PayloadSize = 1 + EqBandWireSize;
    Out.reserve(HeaderSize + PayloadSize);
    AppendHeader(Out, Opcode::SetHwEqBand, static_cast<uint16>(PayloadSize));
    Out.push_back(BandIndex);
    AppendEqBand(Out, Band);
    return Out;
}

std::vector<uint8> EncodeSetHwEqPresetRequest(const std::string& PresetName)
{
    return EncodeRawStringMessage(Opcode::SetHwEqPreset, PresetName);
}

std::vector<uint8> EncodeSaveHwEqPresetRequest(const std::string& PresetName)
{
    return EncodeRawStringMessage(Opcode::SaveHwEqPreset, PresetName);
}

std::vector<uint8> EncodeDeleteEqPresetRequest(const std::string& PresetName)
{
    return EncodeRawStringMessage(Opcode::DeleteEqPreset, PresetName);
}

std::vector<uint8> EncodeGetHwEqStateRequest()
{
    std::vector<uint8> Out;
    AppendHeader(Out, Opcode::GetHwEqState, 0);
    return Out;
}

std::vector<uint8> EncodeSetBassEnhancerRequest(const BassEnhancerSettings& Settings)
{
    std::vector<uint8> Out;
    Out.reserve(HeaderSize + BassEnhancerWireSize);
    AppendHeader(Out, Opcode::SetBassEnhancer, static_cast<uint16>(BassEnhancerWireSize));
    AppendBassEnhancerSettings(Out, Settings);
    return Out;
}

std::vector<uint8> EncodeGetBassEnhancerStateRequest()
{
    std::vector<uint8> Out;
    AppendHeader(Out, Opcode::GetBassEnhancerState, 0);
    return Out;
}

std::vector<uint8> EncodeGetContentStreamsRequest()
{
    std::vector<uint8> Out;
    AppendHeader(Out, Opcode::GetContentStreams, 0);
    return Out;
}

namespace
{
// [target: length-prefixed string][preset name: raw remainder].
std::vector<uint8> EncodeBindPresetRequest(Opcode InOpcode, const std::string& Target, const std::string& PresetName)
{
    std::vector<uint8> Payload;
    AppendString(Payload, Target);
    const size_t Available = Payload.size() < MaxPayloadSize ? MaxPayloadSize - Payload.size() : 0;
    const size_t Len = PresetName.size() > Available ? Available : PresetName.size();
    Payload.insert(Payload.end(), PresetName.begin(), PresetName.begin() + static_cast<std::ptrdiff_t>(Len));

    std::vector<uint8> Out;
    Out.reserve(HeaderSize + Payload.size());
    AppendHeader(Out, InOpcode, static_cast<uint16>(Payload.size()));
    Out.insert(Out.end(), Payload.begin(), Payload.end());
    return Out;
}
} // namespace

std::vector<uint8> EncodeBindAppPresetRequest(const std::string& AppName, const std::string& PresetName)
{
    return EncodeBindPresetRequest(Opcode::BindAppPreset, AppName, PresetName);
}

std::vector<uint8> EncodeBindDevicePresetRequest(const std::string& DeviceName, const std::string& PresetName)
{
    return EncodeBindPresetRequest(Opcode::BindDevicePreset, DeviceName, PresetName);
}

std::vector<uint8> EncodeGetEqPresetCatalogRequest()
{
    std::vector<uint8> Out;
    AppendHeader(Out, Opcode::GetEqPresetCatalog, 0);
    return Out;
}

std::string DefaultControlSocketPath()
{
    if (const char* RuntimeDir = std::getenv("XDG_RUNTIME_DIR"))
    {
        std::string Dir = std::string(RuntimeDir) + "/ramkolfx";
        mkdir(Dir.c_str(), 0700);
        return Dir + "/control.sock";
    }
    std::string Dir = "/tmp/ramkolfx-" + std::to_string(getuid());
    mkdir(Dir.c_str(), 0700);
    return Dir + "/control.sock";
}

namespace
{
// The one rule both name validators share: no byte that could break a
// key=value line (newline, NUL, ...).
bool HasControlCharacters(const std::string& Name)
{
    return std::any_of(Name.begin(), Name.end(),
                       [](unsigned char C) { return C < 0x20 || C == 0x7F; });
}
} // namespace

std::optional<std::string> NormalizePresetName(const std::string& Name)
{
    const auto IsSpace = [](unsigned char C) { return C == ' ' || C == '\t'; };
    size_t Begin = 0;
    size_t End = Name.size();
    while (Begin < End && IsSpace(static_cast<unsigned char>(Name[Begin])))
    {
        ++Begin;
    }
    while (End > Begin && IsSpace(static_cast<unsigned char>(Name[End - 1])))
    {
        --End;
    }
    std::string Trimmed = Name.substr(Begin, End - Begin);
    if (Trimmed.empty() || Trimmed.size() > MaxEqPresetNameLength || HasControlCharacters(Trimmed))
    {
        return std::nullopt;
    }
    return Trimmed;
}

bool IsValidTargetName(const std::string& Name)
{
    return !Name.empty() && Name.size() <= MaxTargetNameLength && !HasControlCharacters(Name);
}

} // namespace ramkolfx
