// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "ramkolfx/types.hpp"

namespace ramkolfx
{

// Minimal binary control protocol spoken over the daemon's Unix domain
// socket. Deliberately not JSON/text: fixed 3-byte header (1-byte opcode +
// 2-byte payload length, big-endian) followed by a raw payload, no parsing
// or allocation beyond a byte buffer. This keeps the control path cheap
// enough to poll frequently from the GUI (e.g. live speaker repositioning)
// without framing/parsing overhead.
//
// Wire format:
//   [opcode: u8][length: u16 BE][payload: length bytes]
//
// The wire format is intentionally isolated to this header/.cpp pair so it
// can be swapped later (different opcode set, different framing) without
// touching daemon or GUI logic built on top of it.

enum class Opcode : uint8
{
    // Requests
    GetStatus = 0x01,
    SetSpatialMode = 0x02,
    SetSpeakerAzimuth = 0x03,
    GetDevices = 0x04,
    SetOutputDevice = 0x05,
    ResetSpeakerPositions = 0x06,
    SetSpeakerMute = 0x07,
    SetTestNoise = 0x08,
    GetHrtfCatalog = 0x09,
    SetHrtfFile = 0x0A,
    SetSpeakerDistance = 0x0B,
    SetNearFieldEnabled = 0x0C,
    SetHwEqBand = 0x0D,
    SetHwEqPreset = 0x0E,
    SaveHwEqPreset = 0x0F,
    GetContentStreams = 0x10,
    // 0x11 / 0x13 retired (were SetContentEqBand / SaveContentEqPreset,
    // for true per-app EQ - dropped in favor of per-app preset
    // auto-switching, see docs/eq-presets-plan.md).
    BindAppPreset = 0x12,
    GetEqPresetCatalog = 0x14,
    GetHwEqState = 0x15,
    SetBassEnhancer = 0x16,
    GetBassEnhancerState = 0x17,
    DeleteEqPreset = 0x18,
    BindDevicePreset = 0x19,
    // Auto-detect (docs/content-detect-plan.md).
    BindAppAutoDetect = 0x1A,    // raw app name; binds it to Auto-detect (BindAppPreset "" unbinds)
    BindDeviceAutoDetect = 0x1B, // raw device node.name; likewise
    BindDetectedClass = 0x1C,    // [class:1][preset: raw remainder, empty = no switch]
    GetDetectedClasses = 0x1D,
    SetAutoDetectDevice = 0x1E, // [InferenceDevice:1]
    SetAutoDetectTiming = 0x1F, // [AutoDetectTiming: AutoDetectTimingWireSize]
    GetChannelLevels = 0x20,
    // Responses
    StatusResponse = 0x81,
    ErrorResponse = 0x82,
    DeviceListResponse = 0x83,
    HrtfCatalogResponse = 0x84,
    ContentStreamListResponse = 0x85,
    EqPresetCatalogResponse = 0x86,
    HwEqStateResponse = 0x87,
    BassEnhancerStateResponse = 0x88,
    DetectedClassesResponse = 0x89,
    ChannelLevelsResponse = 0x90,
};

// The active spatialization DSP path. Off is a static-gain downmix
// (PassthroughStage); Basic is algebraic ambisonics decode
// (AmbisonicsStage); Advanced is HRTF-based binaural decode
// (BinauralStage). All three sit behind the same DspStage interface, so
// switching modes is just changing which stage AudioEngine dispatches to.
enum class SpatialMode : uint8
{
    Off = 0,
    Basic = 1,
    Advanced = 2,
};

// The hardware-output EQ (SetHwEqBand/GetHwEqState) is a single global
// stereo curve applied once to the final downmixed signal, right before
// HardwareOutput - not a curve per virtual 7.1 speaker channel. This was a
// deliberate choice: it matches how graphic EQs are normally used, costs
// one filter chain instead of eight, and lines up with the planned
// per-output-device preset system (presets are keyed by which real sink is
// active, not by speaker channel). Per-channel EQ (pre-downmix, on the 7.1
// signal) remains possible later without breaking this wire format - it
// would just mean reintroducing a channel-select byte to SetHwEqBand/
// SetHwEqPreset/SaveHwEqPreset the same way SpeakerIndex used to work for
// them, orthogonal to everything else here.
//
// A single parametric EQ band. Wire layout is fixed 13 bytes:
// [filterType:1][frequencyHz:4][gainDb:4][q:4], all floats big-endian.
enum class EqFilterType : uint8
{
    Peaking = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowPass = 3,
    HighPass = 4,
    Notch = 5,
};

struct EqBand
{
    EqFilterType FilterType = EqFilterType::Peaking;
    float FrequencyHz = 1000.0f;
    float GainDb = 0.0f;
    float Q = 0.707f;
};

// Wire size of one EqBand: filterType(1) + frequencyHz(4) + gainDb(4) + q(4).
inline constexpr size_t EqBandWireSize = 13;

// Upper bound on bands per curve, shared by daemon and GUI
// so both agree on BandIndex's valid range for SetHwEqBand.
inline constexpr size_t MaxEqBands = 10;

// ISO 10-band graphic EQ center frequencies (31.5Hz-16kHz, ~1 octave
// apart). Shared between the daemon (DefaultHwEqBands(), dsp/
// graphic_eq_filter.hpp) and the GUI (simple-mode slider labels), so both
// sides agree on what each band index nominally means regardless of
// whether its actual FrequencyHz has since been retuned in advanced mode.
inline constexpr std::array<float, MaxEqBands> DefaultEqCenterFrequenciesHz = {
    31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};

// Q for a peaking filter roughly one octave wide, the conventional graphic
// EQ bandwidth at this band spacing.
inline constexpr float DefaultGraphicEqQ = 1.41f;

// Settings for the headphone bass enhancer (see dsp/bass_enhancer_filter.hpp):
// isolates the sub-bass band below CutoffHz, drives it through a saturating
// nonlinearity to synthesize upper harmonics of the (often physically
// unreproducible, on small drivers) fundamental, and blends only those
// harmonics back in. A single global stage applied to the final downmixed
// stereo signal, same scope as the HW EQ, and it runs after it in the
// pipeline (see AudioEngine::HandleVirtualSinkAudio) - a final headphone-
// tuning layer on top of whatever curve the HW EQ already applied.
//
// Persisted the same flat way as HwEqBands (see settings_store.hpp), and
// captured alongside the EQ curve in every named EQ preset (see
// SetHwEqPreset).
struct BassEnhancerSettings
{
    bool bEnabled = false;
    float CutoffHz = 60.0f;
    float Drive = 0.5f;
    float Mix = 0.5f;
};

// Wire size of BassEnhancerSettings: bEnabled(1) + cutoffHz(4) + drive(4) + mix(4).
inline constexpr size_t BassEnhancerWireSize = 1 + 4 + 4 + 4;

struct MessageHeader
{
    Opcode MessageOpcode;
    uint16 PayloadLength;
};

inline constexpr size_t HeaderSize = 3;
inline constexpr size_t MaxPayloadSize = 4096;

// Number of virtual 7.1 speakers whose azimuth is controllable; mirrors
// AmbisonicsStage::SpeakerChannel's non-LFE channel order (FL,FR,FC,RL,RR,SL,SR).
inline constexpr size_t SpeakerCount = 7;

// Per-speaker distance range, in meters, shared by SpeakerLayout (daemon,
// which clamps to this range) and the GUI's position dial (which maps it
// to a pixel radius) - only meaningful when near-field mode is enabled
// (see SetNearFieldEnabled), but the value itself is always settable so a
// speaker can be positioned before switching that on. Floor is kept well
// above the ~0.0875m physical head radius used by the near-field model
// (daemon/src/dsp/synthetic_hrtf.cpp) so that model stays numerically
// well-behaved; ceiling is where the near-field/loudness-falloff effect
// becomes negligible.
inline constexpr float MinSpeakerDistanceMeters = 0.3f;
inline constexpr float MaxSpeakerDistanceMeters = 3.0f;
inline constexpr float DefaultSpeakerDistanceMeters = 1.5f;

// Distance at which near-field mode's loudness falloff and ILD correction
// are both exactly a no-op (1.0 gain) - roughly where HRTF measurements
// are typically made, and shared between AudioEngine's loudness falloff
// and near_field_filter.cpp's ILD shelf so both agree on what "no
// correction" means.
inline constexpr float ReferenceSpeakerDistanceMeters = 1.0f;

// What the content classifier thinks is playing, for Auto-detect
// bindings (docs/content-detect-plan.md). None = nothing confident yet.
// Speech/Movie/Music are the coarse classes; the rest are genres within
// Music. Sent as one byte, so new values go before Count.
enum class DetectedClass : uint8
{
    None = 0,
    Speech,
    Movie,
    Music,
    Rock,
    Metal,
    Pop,
    HipHop,
    RnB,
    Electronic,
    Jazz,
    Classical,
    Blues,
    Reggae,
    Acoustic,
    Count,
};

inline constexpr size_t DetectedClassCount = static_cast<size_t>(DetectedClass::Count);

constexpr bool IsGenreClass(DetectedClass Class)
{
    return Class >= DetectedClass::Rock && Class < DetectedClass::Count;
}

// Display name ("Hip-Hop", "R&B"); "Unknown" for a value this build
// doesn't know, "None" for None.
const char* DetectedClassLabel(DetectedClass Class);

// Stable lowercase id used in config files ("hiphop", "rnb"), or nullptr
// for None and values this build doesn't know.
const char* DetectedClassKey(DetectedClass Class);

// Inverse of DetectedClassKey; nullopt for an unknown key.
std::optional<DetectedClass> DetectedClassFromKey(const std::string& Key);

// Where Auto-detect's classifier runs (settings: auto_detect.device).
// Auto = an NPU if one is present and passes validation, else the CPU;
// the GPU is only ever used when asked for. A device that isn't there
// (or fails validation) falls back to the CPU.
enum class InferenceDevice : uint8
{
    Auto = 0,
    Cpu,
    Gpu,
    Npu,
    Count,
};

// Stable lowercase id for config files ("auto", "cpu", ...), nullptr for
// values this build doesn't know; and its inverse.
const char* InferenceDeviceKey(InferenceDevice Device);
std::optional<InferenceDevice> InferenceDeviceFromKey(const std::string& Key);

// How quickly Auto-detect reacts, against how steady it is and what it
// costs (settings: auto_detect.*). The daemon derives its smoothing,
// confirm/dwell and inference cadence from these four; see
// DetectionTuningFor / SchedulerTuningFor in the daemon.
struct AutoDetectTiming
{
    // Inference cadence while finding the class; three times this once
    // it has settled, or on battery.
    uint32 CheckEveryMs = 2000;
    // How long new content must lead before the EQ switches to it (the
    // score smoothing scales with it, too).
    uint32 SwitchAfterMs = 4000;
    // Minimum time between two switches, so ambiguous content doesn't
    // flip the EQ back and forth. Waived after a gap between tracks.
    uint32 KeepAtLeastMs = 10000;
    // Hard cap on inference time / interval, in percent of one core: the
    // cadence stretches to stay under it.
    float CpuLimitPercent = 1.0f;

    bool operator==(const AutoDetectTiming&) const = default;
};

// Accepted ranges; the daemon clamps what it's sent into them.
inline constexpr uint32 MinCheckEveryMs = 500;
inline constexpr uint32 MaxCheckEveryMs = 10000;
inline constexpr uint32 MaxSwitchAfterMs = 15000;
inline constexpr uint32 MaxKeepAtLeastMs = 60000;
inline constexpr float MinCpuLimitPercent = 0.5f;
inline constexpr float MaxCpuLimitPercent = 5.0f;

// Timing with every field clamped into its range (a NaN CPU limit
// becomes the default).
AutoDetectTiming ClampAutoDetectTiming(const AutoDetectTiming& Timing);

// Wire size of AutoDetectTiming: three u32 then a float.
inline constexpr size_t AutoDetectTimingWireSize = 16;

// Whether Auto-detect bindings can work on this machine. Anything but Ok
// makes them count as unbound. Sent as one byte, so new values go before
// Count.
enum class AutoDetectAvailability : uint8
{
    Ok = 0,    // runtime and model found (loaded lazily, on first use)
    NoRuntime, // ONNX Runtime (libonnxruntime) not installed, or too old
    NoModel,   // classifier model files missing
    Failed,    // found, but the model wouldn't load or run
    TooSlow,   // inference can't stay inside the CPU budget here
    Count,
};

// Why Auto-detect is unavailable, for the GUI's tooltip ("" for Ok).
const char* AutoDetectAvailabilityLabel(AutoDetectAvailability Availability);

// Command decoded from a request message.
struct Command
{
    Opcode CommandOpcode = Opcode::GetStatus;
    SpatialMode ModeValue = SpatialMode::Off; // valid when CommandOpcode == SetSpatialMode
    uint8 SpeakerIndex = 0;     // valid when CommandOpcode == SetSpeakerAzimuth, SetSpeakerMute, or SetSpeakerDistance, 0..SpeakerCount-1
    float AzimuthDegrees = 0.0f;  // valid when CommandOpcode == SetSpeakerAzimuth
    bool bMuted = false;          // valid when CommandOpcode == SetSpeakerMute
    bool bTestNoiseEnabled = false; // valid when CommandOpcode == SetTestNoise
    std::string OutputDeviceName; // valid when CommandOpcode == SetOutputDevice, BindDevicePreset or BindDeviceAutoDetect
    uint8 HrtfIndex = 0;         // valid when CommandOpcode == SetHrtfFile
    float DistanceMeters = 0.0f;  // valid when CommandOpcode == SetSpeakerDistance
    bool bNearFieldEnabled = false; // valid when CommandOpcode == SetNearFieldEnabled
    uint8 BandIndex = 0;          // valid when CommandOpcode == SetHwEqBand, 0..MaxEqBands-1
    EqBand Band;                  // valid when CommandOpcode == SetHwEqBand
    // Valid when CommandOpcode == SetHwEqPreset, SaveHwEqPreset,
    // DeleteEqPreset, BindDevicePreset, or BindAppPreset (where an empty
    // name means "unbind" - from a preset or Auto-detect alike), or
    // BindDetectedClass (where empty means "no switch"). Presets are addressed by
    // name rather than catalog index: deleting one would shift every later
    // index and silently retarget anything (e.g. a device binding) that
    // referred to them by position.
    std::string PresetName;
    std::string ContentAppName;   // valid when CommandOpcode == BindAppPreset or BindAppAutoDetect; the persistent key from ContentStreamInfo::AppName
    BassEnhancerSettings BassEnhancer; // valid when CommandOpcode == SetBassEnhancer
    DetectedClass ClassValue = DetectedClass::None; // valid when CommandOpcode == BindDetectedClass (never None)
    InferenceDevice DeviceValue = InferenceDevice::Auto; // valid when CommandOpcode == SetAutoDetectDevice
    AutoDetectTiming Timing;      // valid when CommandOpcode == SetAutoDetectTiming (not yet clamped)
};

enum class EqPresetSource : uint8
{
    Manual = 0,
    Device = 1,
    App = 2,
};

struct Status
{
    SpatialMode Mode = SpatialMode::Off;
    std::array<float, SpeakerCount> SpeakerAzimuthDegrees{};
    std::array<bool, SpeakerCount> SpeakerMuted{};
    bool bTestNoiseEnabled = false;
    std::string OutputDeviceName; // node.name of the currently pinned hardware output sink
    uint8 ActiveHrtfIndex = 0;  // index into the GetHrtfCatalog list Advanced mode is rendering through
    std::array<float, SpeakerCount> SpeakerDistanceMeters{};
    bool bNearFieldEnabled = false; // whether distance affects loudness (all modes) and ILD (Advanced mode)

    // Name of the EQ preset last applied or saved (see SetHwEqPreset/
    // SaveHwEqPreset), empty if none - i.e. a hand-tuned curve that was
    // never saved. bPresetModified is true once a band or the bass
    // enhancer has been changed since then (GUI shows "Custom (unsaved)").
    std::string ActivePresetName;
    bool bPresetModified = false;

    // Bumped by the daemon whenever the HW EQ curve / bass enhancer change
    // by any route other than the client's own SetHwEqBand/SetBassEnhancer
    // (applying a preset, or device/app auto-switching). Status
    // deliberately doesn't carry the curve itself, so a client that caches
    // it (the GUI) re-fetches GetHwEqState/GetBassEnhancerState when this
    // changes rather than polling them.
    uint32 EqRevision = 0;

    // Bumped whenever a preset is saved or deleted, by any client. Same
    // idea as EqRevision: a client caching the GetEqPresetCatalog list
    // re-fetches it when this changes.
    uint32 PresetCatalogRevision = 0;

    // Preset bound to the current output device (see BindDevicePreset),
    // empty if none - drives the GUI's per-device toggle.
    std::string DevicePresetName;

    // Why the active preset is active: picked by hand, or auto-switched
    // in because of the output device's binding or a playing app's
    // binding (PresetSourceApp names that app). See
    // docs/eq-presets-plan.md for the resolution rule.
    EqPresetSource PresetSource = EqPresetSource::Manual;
    std::string PresetSourceApp;

    // Auto-detect (docs/content-detect-plan.md). Decoded only if present,
    // so an older daemon's Status still decodes (with these defaults).
    // bHasAutoDetect isn't sent: the decoder sets it when the daemon sent
    // these fields - false means a daemon that predates Auto-detect and
    // would refuse its requests.
    bool bHasAutoDetect = false;
    // Whether Auto-detect bindings can work here (anything but Ok: they
    // count as unbound; AutoDetectAvailabilityLabel says why).
    AutoDetectAvailability AutoDetect = AutoDetectAvailability::Ok;
    // The current output device is bound to Auto-detect.
    bool bDeviceAutoDetect = false;
    // An Auto-detect binding is in effect (PresetSource/PresetSourceApp
    // say which), with the class it detected (None while still listening)
    // and that class's smoothed score in percent.
    bool bAutoDetectActive = false;
    DetectedClass Detected = DetectedClass::None;
    uint8 DetectedConfidence = 0;
    // The user changed the EQ by hand while it was in effect: detection is
    // paused until the binding next comes into effect.
    bool bDetectionHeld = false;
    // Where detection should run (SetAutoDetectDevice), and where it does
    // ("NPU (OpenVINO)", "CPU"; empty while no classifier is loaded).
    InferenceDevice AutoDetectDevice = InferenceDevice::Auto;
    std::string InferenceDeviceLabel;
    // Bumped when the class -> preset table changes (BindDetectedClass);
    // a client caching GetDetectedClasses re-fetches it then, and when
    // PresetCatalogRevision changes (a deleted preset leaves its classes
    // with no switch).
    uint32 DetectedTableRevision = 0;

    // Auto-detect timing (SetAutoDetectTiming). Decoded only if present:
    // bHasAutoDetectTiming false means a daemon that predates it and would
    // refuse SetAutoDetectTiming.
    bool bHasAutoDetectTiming = false;
    AutoDetectTiming DetectTiming;
};

// Per-channel activity of the 7.1 signal entering the spatializer, for the
// GUI's speaker-dial activity view (GetChannelLevels). Each value is the
// linear peak amplitude (0 = silence, 1 = full scale; can exceed 1 for
// clipping input) seen on that channel since the previous GetChannelLevels
// request - the daemon resets its peak-hold on every read, so with more
// than one client polling each sees only the peaks since *any* client's
// last read (fine for a cosmetic meter). Measured after per-speaker mute
// and near-field gain, so a muted speaker reads silent, and on whatever is
// actually being spatialized: real audio, or test noise while that's on.
struct ChannelLevels
{
    std::array<float, SpeakerCount> SpeakerPeak{}; // FL,FR,FC,RL,RR,SL,SR - same order as Status
    float LfePeak = 0.0f;
};

// Wire size of ChannelLevels: one float per speaker, then LFE.
inline constexpr size_t ChannelLevelsWireSize = 4 * (SpeakerCount + 1);

// Longest accepted EQ preset name, in bytes (after trimming whitespace).
inline constexpr size_t MaxEqPresetNameLength = 64;

// Longest app / output-device name accepted as a preset binding target or
// remembered per-device setting (see IsValidTargetName).
inline constexpr size_t MaxTargetNameLength = 256;

// Whether Name is usable as a binding target or remembered device name
// (an app's application.name, a PipeWire node name): 1..MaxTargetNameLength
// bytes with no control characters. Those names end up as values in the
// daemon's key=value config files, where a newline would inject extra keys.
bool IsValidTargetName(const std::string& Name);

// Trims surrounding spaces/tabs and checks the result is usable as a preset
// name: 1..MaxEqPresetNameLength bytes, no control characters (a newline
// would break the daemon's key=value files). Returns the trimmed name, or
// nullopt if it isn't valid. Shared so the GUI's pre-send feedback can't
// drift from what the daemon accepts.
std::optional<std::string> NormalizePresetName(const std::string& Name);

// Which group a preset belongs to. User = saved by the user; every other
// value is a built-in shipped with the daemon, grouped for the GUI's
// preset pickers. Sent as one byte, so new values go before Count. A
// decoder keeps a value it doesn't know (a newer daemon's category) as-is
// rather than rejecting the catalog: it's still a built-in, just one this
// build has no header text for.
enum class EqPresetCategory : uint8
{
    User = 0,
    General = 1,
    Music = 2,
    Games = 3,
    Count,
};

// The one rule for what may be overwritten or deleted, shared by the
// daemon's EqPreset and the wire's EqPresetInfo so the two can't drift.
constexpr bool IsBuiltInPresetCategory(EqPresetCategory Category)
{
    return Category != EqPresetCategory::User;
}

// Header shown above a category's presets in the GUI's preset pickers
// ("More" for a category this build doesn't know).
const char* EqPresetCategoryLabel(EqPresetCategory Category);

// One entry of the EqPresetCatalogResponse list. Built-in presets ship
// with the daemon and can't be overwritten or deleted (saving under a
// built-in's name is refused; "save as" a new name instead).
struct EqPresetInfo
{
    std::string Name;
    EqPresetCategory Category = EqPresetCategory::User;

    bool IsBuiltIn() const
    {
        return IsBuiltInPresetCategory(Category);
    }
};

// A real hardware playback sink the daemon can pin HardwareOutput to.
// Name is the PipeWire node.name (stable identifier, used to select the
// device); Description is node.description (human-readable, shown in UI).
struct AudioDeviceInfo
{
    std::string Name;
    std::string Description;
};

// A playback stream currently routed into the RamkolFX virtual sink.
// AppName (PipeWire application.name, falling back to the process binary)
// is the persistent key BindAppPreset binds presets to - not the stream's
// transient PipeWire id, so a binding survives the app reconnecting.
// MediaName is display-only (media.name), for telling multiple streams
// from the same app apart. BoundPresetName is AppName's binding (empty if
// none), repeated on every stream of that app.
struct ContentStreamInfo
{
    std::string AppName;
    std::string MediaName;
    bool bPlaying = false;
    std::string BoundPresetName;
    // AppName is bound to Auto-detect (BoundPresetName empty). On the wire
    // it's a flag byte per entry *after* the whole list, so an older GUI
    // ignores it and an older daemon's list still decodes (all false).
    bool bAutoDetect = false;
};

// One row of the class -> preset table (GetDetectedClasses).
struct DetectedClassEntry
{
    DetectedClass Class = DetectedClass::None;
    std::string PresetName;        // "" = no switch
    std::string DefaultPresetName; // what the class maps to unless changed ("" = no switch)
    float Score = 0.0f;     // live smoothed score, 0..1 (0 while no Auto binding is in effect)
};

// DetectedClassesResponse: the class table plus how detection is doing
// on this machine.
struct DetectionReport
{
    std::vector<DetectedClassEntry> Classes; // every class but None
    std::string DeviceLabel;                 // empty while no classifier is loaded
    uint32 P50Us = 0;                        // inference CPU time per run
    uint32 P95Us = 0;
    float DutyCycle = 0.0f; // share of one core at the current cadence
    uint32 Runs = 0;
    uint32 RssAddedKb = 0;
};

// Attempts to read a header from the front of `Buffer`. Returns nullopt if
// fewer than HeaderSize bytes are available yet (caller should wait for
// more data on the socket).
std::optional<MessageHeader> TryReadHeader(const uint8* Buffer, size_t Available);

// Decodes a request payload for the given opcode. `Payload` must point at
// exactly `PayloadLength` valid bytes. Returns nullopt if the opcode isn't
// a request or the payload doesn't match what that opcode expects.
std::optional<Command> DecodeCommand(Opcode InOpcode, const uint8* Payload, uint16 PayloadLength);

// Decodes a StatusResponse payload. Used by control clients (GUI, test
// tools) to parse what the daemon sends back; the daemon only encodes
// responses, it doesn't decode them. Returns nullopt if PayloadLength
// doesn't match the expected `1 + 4*SpeakerCount` layout.
std::optional<Status> DecodeStatusResponse(const uint8* Payload, uint16 PayloadLength);

// Decodes an ErrorResponse payload into its message string.
std::string DecodeErrorResponse(const uint8* Payload, uint16 PayloadLength);

// Decodes a DeviceListResponse payload. Used by control clients (GUI, test
// tools) to parse what the daemon sends back. Returns nullopt if the
// payload is truncated or malformed.
std::optional<std::vector<AudioDeviceInfo>> DecodeDeviceListResponse(const uint8* Payload,
                                                                       uint16 PayloadLength);

// Decodes an HrtfCatalogResponse payload into the ordered list of display
// names; an entry's index into this list is what SetHrtfFile's HrtfIndex
// and Status's ActiveHrtfIndex refer to. Returns nullopt if the payload is
// truncated or malformed.
std::optional<std::vector<std::string>> DecodeHrtfCatalogResponse(const uint8* Payload,
                                                                     uint16 PayloadLength);

// Decodes an HwEqStateResponse payload: always exactly MaxEqBands entries
// (no count prefix - the band count is fixed for now, see MaxEqBands).
// Returns nullopt if PayloadLength isn't exactly MaxEqBands * EqBandWireSize.
std::optional<std::array<EqBand, MaxEqBands>> DecodeHwEqStateResponse(const uint8* Payload,
                                                                        uint16 PayloadLength);

// Decodes a BassEnhancerStateResponse payload. Returns nullopt if
// PayloadLength isn't exactly BassEnhancerWireSize.
std::optional<BassEnhancerSettings> DecodeBassEnhancerStateResponse(const uint8* Payload,
                                                                       uint16 PayloadLength);

// Decodes a ContentStreamListResponse payload. Same truncated/malformed
// contract as DecodeDeviceListResponse.
std::optional<std::vector<ContentStreamInfo>> DecodeContentStreamListResponse(const uint8* Payload,
                                                                                 uint16 PayloadLength);

// Decodes a DetectedClassesResponse payload. Nullopt if truncated or
// malformed.
std::optional<DetectionReport> DecodeDetectedClassesResponse(const uint8* Payload, uint16 PayloadLength);

// Decodes an EqPresetCatalogResponse payload into the ordered list of
// presets (built-ins first). Same truncated/malformed contract as
// DecodeHrtfCatalogResponse.
std::optional<std::vector<EqPresetInfo>> DecodeEqPresetCatalogResponse(const uint8* Payload,
                                                                         uint16 PayloadLength);

// Decodes a ChannelLevelsResponse payload. Returns nullopt if
// PayloadLength isn't exactly ChannelLevelsWireSize.
std::optional<ChannelLevels> DecodeChannelLevelsResponse(const uint8* Payload, uint16 PayloadLength);

// Encodes a full status response message (header + payload), ready to
// write directly to the socket.
std::vector<uint8> EncodeStatusResponse(const Status& InStatus);

// Encodes a full error response message (header + payload), ready to
// write directly to the socket.
std::vector<uint8> EncodeErrorResponse(const std::string& Message);

// Encodes a full device list response message (header + payload), ready to
// write directly to the socket. Entries that would push the payload past
// MaxPayloadSize are dropped from the end rather than truncated mid-entry.
std::vector<uint8> EncodeDeviceListResponse(const std::vector<AudioDeviceInfo>& Devices);

// Encodes a full HRTF catalog response message (header + payload), ready
// to write directly to the socket. Same truncate-whole-entries behavior
// as EncodeDeviceListResponse if the list would exceed MaxPayloadSize.
std::vector<uint8> EncodeHrtfCatalogResponse(const std::vector<std::string>& DisplayNames);

// Encodes a full HwEqStateResponse message: always exactly MaxEqBands
// entries, no count prefix, no truncation behavior needed since the size
// is fixed (MaxEqBands * EqBandWireSize is well under MaxPayloadSize).
std::vector<uint8> EncodeHwEqStateResponse(const std::array<EqBand, MaxEqBands>& Bands);

// Encodes a full BassEnhancerStateResponse message (header + payload).
std::vector<uint8> EncodeBassEnhancerStateResponse(const BassEnhancerSettings& Settings);

// Encodes a full ChannelLevelsResponse message (header + payload).
std::vector<uint8> EncodeChannelLevelsResponse(const ChannelLevels& Levels);

// Encodes a full content stream list response message. Same drop-whole-
// entries-past-MaxPayloadSize behavior as EncodeDeviceListResponse.
std::vector<uint8> EncodeContentStreamListResponse(const std::vector<ContentStreamInfo>& Streams);

// Encodes a full DetectedClassesResponse message.
std::vector<uint8> EncodeDetectedClassesResponse(const DetectionReport& Report);

// Encodes a full EQ preset catalog response message. Same behavior as
// EncodeHrtfCatalogResponse.
std::vector<uint8> EncodeEqPresetCatalogResponse(const std::vector<EqPresetInfo>& Presets);

// Encodes a full request message. Used by control clients (GUI, test
// tools); the daemon only decodes requests, it doesn't encode them.
std::vector<uint8> EncodeGetStatusRequest();
std::vector<uint8> EncodeSetSpatialModeRequest(SpatialMode Mode);
std::vector<uint8> EncodeSetSpeakerAzimuthRequest(uint8 SpeakerIndex, float AzimuthDegrees);
std::vector<uint8> EncodeGetDevicesRequest();
std::vector<uint8> EncodeSetOutputDeviceRequest(const std::string& DeviceName);
std::vector<uint8> EncodeResetSpeakerPositionsRequest();
std::vector<uint8> EncodeSetSpeakerMuteRequest(uint8 SpeakerIndex, bool bMuted);
std::vector<uint8> EncodeSetTestNoiseRequest(bool bEnabled);
std::vector<uint8> EncodeGetHrtfCatalogRequest();
std::vector<uint8> EncodeSetHrtfFileRequest(uint8 HrtfIndex);
std::vector<uint8> EncodeSetSpeakerDistanceRequest(uint8 SpeakerIndex, float DistanceMeters);
std::vector<uint8> EncodeSetNearFieldEnabledRequest(bool bEnabled);
std::vector<uint8> EncodeSetHwEqBandRequest(uint8 BandIndex, const EqBand& Band);
std::vector<uint8> EncodeSetHwEqPresetRequest(const std::string& PresetName);
std::vector<uint8> EncodeSaveHwEqPresetRequest(const std::string& PresetName);
std::vector<uint8> EncodeDeleteEqPresetRequest(const std::string& PresetName);
std::vector<uint8> EncodeGetHwEqStateRequest();
std::vector<uint8> EncodeSetBassEnhancerRequest(const BassEnhancerSettings& Settings);
std::vector<uint8> EncodeGetBassEnhancerStateRequest();
std::vector<uint8> EncodeGetContentStreamsRequest();
// Empty PresetName unbinds.
std::vector<uint8> EncodeBindAppPresetRequest(const std::string& AppName, const std::string& PresetName);
std::vector<uint8> EncodeBindDevicePresetRequest(const std::string& DeviceName, const std::string& PresetName);
std::vector<uint8> EncodeGetEqPresetCatalogRequest();
std::vector<uint8> EncodeBindAppAutoDetectRequest(const std::string& AppName);
std::vector<uint8> EncodeBindDeviceAutoDetectRequest(const std::string& DeviceName);
// Empty PresetName = no switch for that class.
std::vector<uint8> EncodeBindDetectedClassRequest(DetectedClass Class, const std::string& PresetName);
std::vector<uint8> EncodeGetDetectedClassesRequest();
std::vector<uint8> EncodeSetAutoDetectDeviceRequest(InferenceDevice Device);
std::vector<uint8> EncodeSetAutoDetectTimingRequest(const AutoDetectTiming& Timing);
std::vector<uint8> EncodeGetChannelLevelsRequest();

// Resolves the default control socket path: $XDG_RUNTIME_DIR/ramkolfx/control.sock,
// falling back to /tmp/ramkolfx-<uid>/control.sock if XDG_RUNTIME_DIR isn't set.
// Shared between the daemon (binds it) and control clients like the GUI
// (connect to it), so it lives here rather than in daemon-only code.
std::string DefaultControlSocketPath();

} // namespace ramkolfx
