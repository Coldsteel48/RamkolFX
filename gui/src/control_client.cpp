// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "control_client.hpp"

#include <cstring>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "ramkolfx/protocol.hpp"

namespace ramkolfx::gui
{

namespace
{
// Local Unix socket: a few hundred ms is generous headroom for a live
// daemon while still failing fast if it's hung or gone.
constexpr int TimeoutMs = 200;

bool RecvExact(int Fd, uint8* Out, size_t Count)
{
    size_t Received = 0;
    while (Received < Count)
    {
        const ssize_t BytesRead = recv(Fd, Out + Received, Count - Received, 0);
        if (BytesRead <= 0)
        {
            return false;
        }
        Received += static_cast<size_t>(BytesRead);
    }
    return true;
}
} // namespace

bool ControlClient::Connect()
{
    Disconnect();

    SocketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (SocketFd < 0)
    {
        return false;
    }

    timeval Timeout{};
    Timeout.tv_sec = TimeoutMs / 1000;
    Timeout.tv_usec = (TimeoutMs % 1000) * 1000;
    setsockopt(SocketFd, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout));
    setsockopt(SocketFd, SOL_SOCKET, SO_SNDTIMEO, &Timeout, sizeof(Timeout));

    const std::string SocketPath = DefaultControlSocketPath();
    sockaddr_un Addr{};
    Addr.sun_family = AF_UNIX;
    if (SocketPath.size() >= sizeof(Addr.sun_path))
    {
        Disconnect();
        return false;
    }
    std::strncpy(Addr.sun_path, SocketPath.c_str(), sizeof(Addr.sun_path) - 1);

    if (connect(SocketFd, reinterpret_cast<sockaddr*>(&Addr), sizeof(Addr)) < 0)
    {
        Disconnect();
        return false;
    }

    return true;
}

void ControlClient::Disconnect()
{
    if (SocketFd >= 0)
    {
        close(SocketFd);
        SocketFd = -1;
    }
}

std::optional<ControlClient::RawResponse> ControlClient::SendRawRequest(const std::vector<uint8>& Request)
{
    if (!IsConnected())
    {
        return std::nullopt;
    }

    if (send(SocketFd, Request.data(), Request.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(Request.size()))
    {
        Disconnect();
        return std::nullopt;
    }

    uint8 HeaderBytes[HeaderSize];
    if (!RecvExact(SocketFd, HeaderBytes, HeaderSize))
    {
        Disconnect();
        return std::nullopt;
    }
    const auto Header = TryReadHeader(HeaderBytes, HeaderSize);
    if (!Header || Header->PayloadLength > MaxPayloadSize)
    {
        Disconnect();
        return std::nullopt;
    }

    std::vector<uint8> Payload(Header->PayloadLength);
    if (!Payload.empty() && !RecvExact(SocketFd, Payload.data(), Payload.size()))
    {
        Disconnect();
        return std::nullopt;
    }

    return RawResponse{Header->MessageOpcode, std::move(Payload)};
}

template <typename DecodeFn>
auto ControlClient::RequestDecoded(const std::vector<uint8>& Request, Opcode ExpectedOpcode, DecodeFn Decode)
    -> decltype(Decode(nullptr, uint16{}))
{
    auto Response = SendRawRequest(Request);
    if (!Response || Response->MessageOpcode != ExpectedOpcode)
    {
        // No reply, an ErrorResponse, or anything unexpected - not fatal
        // to the connection itself, but this request didn't succeed.
        return std::nullopt;
    }

    auto Decoded = Decode(Response->Payload.data(), static_cast<uint16>(Response->Payload.size()));
    if (!Decoded)
    {
        // The daemon answered with the right opcode but a payload we can't
        // parse: the two are out of sync, so drop the connection.
        Disconnect();
    }
    return Decoded;
}

std::optional<Status> ControlClient::SendStatusRequest(const std::vector<uint8>& Request)
{
    return RequestDecoded(Request, Opcode::StatusResponse, DecodeStatusResponse);
}

std::optional<Status> ControlClient::RequestStatus()
{
    return SendStatusRequest(EncodeGetStatusRequest());
}

std::optional<Status> ControlClient::SetSpatialMode(SpatialMode Mode)
{
    return SendStatusRequest(EncodeSetSpatialModeRequest(Mode));
}

std::optional<Status> ControlClient::SetSpeakerAzimuth(uint8 SpeakerIndex, float AzimuthDegrees)
{
    return SendStatusRequest(EncodeSetSpeakerAzimuthRequest(SpeakerIndex, AzimuthDegrees));
}

std::optional<Status> ControlClient::SetSpeakerDistance(uint8 SpeakerIndex, float DistanceMeters)
{
    return SendStatusRequest(EncodeSetSpeakerDistanceRequest(SpeakerIndex, DistanceMeters));
}

std::optional<Status> ControlClient::SetNearFieldEnabled(bool bEnabled)
{
    return SendStatusRequest(EncodeSetNearFieldEnabledRequest(bEnabled));
}

std::optional<Status> ControlClient::ResetSpeakerPositions()
{
    return SendStatusRequest(EncodeResetSpeakerPositionsRequest());
}

std::optional<Status> ControlClient::SetOutputDevice(const std::string& DeviceName)
{
    return SendStatusRequest(EncodeSetOutputDeviceRequest(DeviceName));
}

std::optional<Status> ControlClient::SetSpeakerMute(uint8 SpeakerIndex, bool bMuted)
{
    return SendStatusRequest(EncodeSetSpeakerMuteRequest(SpeakerIndex, bMuted));
}

std::optional<Status> ControlClient::SetTestNoiseEnabled(bool bEnabled)
{
    return SendStatusRequest(EncodeSetTestNoiseRequest(bEnabled));
}

std::optional<std::vector<AudioDeviceInfo>> ControlClient::RequestDevices()
{
    return RequestDecoded(EncodeGetDevicesRequest(), Opcode::DeviceListResponse, DecodeDeviceListResponse);
}

std::optional<std::vector<std::string>> ControlClient::RequestHrtfCatalog()
{
    return RequestDecoded(EncodeGetHrtfCatalogRequest(), Opcode::HrtfCatalogResponse, DecodeHrtfCatalogResponse);
}

std::optional<Status> ControlClient::SetHrtfFile(uint8 HrtfIndex)
{
    return SendStatusRequest(EncodeSetHrtfFileRequest(HrtfIndex));
}

std::optional<Status> ControlClient::SetHwEqBand(uint8 BandIndex, const EqBand& Band)
{
    return SendStatusRequest(EncodeSetHwEqBandRequest(BandIndex, Band));
}

std::optional<std::array<EqBand, MaxEqBands>> ControlClient::RequestHwEqState()
{
    return RequestDecoded(EncodeGetHwEqStateRequest(), Opcode::HwEqStateResponse, DecodeHwEqStateResponse);
}

std::optional<Status> ControlClient::SetBassEnhancer(const BassEnhancerSettings& Settings)
{
    return SendStatusRequest(EncodeSetBassEnhancerRequest(Settings));
}

std::optional<BassEnhancerSettings> ControlClient::RequestBassEnhancerState()
{
    return RequestDecoded(EncodeGetBassEnhancerStateRequest(), Opcode::BassEnhancerStateResponse, DecodeBassEnhancerStateResponse);
}

std::optional<std::vector<EqPresetInfo>> ControlClient::RequestEqPresetCatalog()
{
    return RequestDecoded(EncodeGetEqPresetCatalogRequest(), Opcode::EqPresetCatalogResponse, DecodeEqPresetCatalogResponse);
}

std::optional<Status> ControlClient::ApplyEqPreset(const std::string& PresetName)
{
    return SendStatusRequest(EncodeSetHwEqPresetRequest(PresetName));
}

std::optional<Status> ControlClient::SaveEqPreset(const std::string& PresetName)
{
    return SendStatusRequest(EncodeSaveHwEqPresetRequest(PresetName));
}

std::optional<Status> ControlClient::DeleteEqPreset(const std::string& PresetName)
{
    return SendStatusRequest(EncodeDeleteEqPresetRequest(PresetName));
}

std::optional<Status> ControlClient::BindDevicePreset(const std::string& DeviceName, const std::string& PresetName)
{
    return SendStatusRequest(EncodeBindDevicePresetRequest(DeviceName, PresetName));
}

std::optional<Status> ControlClient::BindAppPreset(const std::string& AppName, const std::string& PresetName)
{
    return SendStatusRequest(EncodeBindAppPresetRequest(AppName, PresetName));
}

std::optional<std::vector<ContentStreamInfo>> ControlClient::RequestContentStreams()
{
    return RequestDecoded(EncodeGetContentStreamsRequest(), Opcode::ContentStreamListResponse, DecodeContentStreamListResponse);
}

std::optional<ChannelLevels> ControlClient::RequestChannelLevels()
{
    return RequestDecoded(EncodeGetChannelLevelsRequest(), Opcode::ChannelLevelsResponse, DecodeChannelLevelsResponse);
}

} // namespace ramkolfx::gui
