// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spa/utils/hook.h>

#include "ramkolfx/protocol.hpp"
#include "ramkolfx/types.hpp"

struct pw_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_metadata;
struct pw_proxy;
struct pw_node_info;
struct spa_dict;

namespace ramkolfx
{

// Maintains a live list of real hardware playback sinks (PipeWire nodes
// with media.class "Audio/Sink") by listening for registry global-add/
// remove events on its own context/core connection - separate from the
// VirtualSink/HardwareOutput streams so device discovery keeps working
// independently of whether those streams are currently connected.
//
// Also claims the PipeWire virtual sink as the system default output as
// soon as the session's "default" metadata object appears in the
// registry (see HandleGlobalAdded/ClaimVirtualSinkAsDefault): this isn't
// just convenience routing - re-asserting default.configured.audio.sink
// nudges WirePlumber's default-nodes policy to reconcile any client
// stream that got created before it had resolved a target and was
// otherwise sitting unlinked (silently producing no sound) until
// something touched that metadata, which is what manually opening a
// system volume mixer was observed to fix. Whatever default was
// configured beforehand is captured at the same time (see
// HandleMetadataProperty) so RestorePreviousDefaultSink() can hand it
// back to the system once the daemon shuts down.
//
// Also tracks playback streams (media.class "Stream/Output/Audio") and the
// links between nodes, so GetContentStreams() can report which apps are
// currently routed into the virtual sink and whether each is actually
// running - the input to EQ preset auto-switching (see
// docs/eq-presets-plan.md). Each stream node is bound so its info events
// (state changes: running vs. paused/idle/suspended) reach us.
//
// Registry events land on the PipeWire main-loop thread (the same thread
// AudioEngine::Run() drives); GetDevices() is called from ControlServer's
// per-client threads, so the device map is behind a mutex.
class DeviceRegistry
{
public:
    explicit DeviceRegistry(pw_loop* InLoop);
    ~DeviceRegistry();

    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    // Connects to PipeWire and starts listening for registry events.
    // Returns false on failure.
    bool Start();

    // Blocks (pumping Loop directly) until a pw_core_sync roundtrip
    // confirms every registry event pending at the time of the call has
    // been dispatched - in particular, the 'global' events for sinks that
    // already existed when Start() connected, which otherwise only arrive
    // once something else drives Loop (normally pw_main_loop_run(), not
    // yet running at the call site this exists for). Only safe to call
    // before the loop is being driven by another thread. Gives up and
    // returns false after TimeoutMs if no sync response arrives.
    bool WaitForInitialSync(int TimeoutMs = 1000);

    // Safe to call from any thread.
    std::vector<AudioDeviceInfo> GetDevices() const;

    // Playback streams currently linked into the virtual sink.
    // ContentStreamInfo::BoundPresetName is left empty - bindings aren't
    // this class's concern. Safe to call from any thread.
    std::vector<ContentStreamInfo> GetContentStreams() const;

    // True if a currently-known real hardware sink's node.name matches
    // Name. Safe to call from any thread.
    bool HasDevice(const std::string& Name) const;

    // Returns the node.name of an arbitrary currently-known real hardware
    // sink (the one with the lowest PipeWire global id, so the result is
    // deterministic for a given graph rather than depending on
    // unordered_map iteration order), or nullopt if none are known yet.
    // Safe to call from any thread.
    std::optional<std::string> PickAnyDevice() const;

    // Returns the node.name of the real hardware sink that was configured
    // as the system default before this daemon claimed the virtual sink
    // (see ClaimVirtualSinkAsDefault/PreviousDefaultSinkValue) - a useful
    // fallback target when PickAnyDevice() finds nothing (e.g. the
    // registry sync raced a slow hardware probe), without risking
    // AUTOCONNECT resolving to the live "default", which is always our own
    // virtual sink while this daemon runs. Returns nullopt if no previous
    // default was captured, it fails to parse, or - guarding the case
    // where an earlier instance of this daemon crashed before
    // RestorePreviousDefaultSink() could run - it names our own virtual
    // sink. Safe to call from any thread.
    std::optional<std::string> GetPreviousDefaultSinkName() const;

    // A bound Stream/Output/Audio node (public only so the pw_node event
    // trampoline can reach it; not for external use). Heap-allocated so Listener's
    // address stays stable for the proxy's lifetime.
    struct TrackedStream
    {
        DeviceRegistry* Owner = nullptr;
        uint32 Id = 0;
        std::string AppName;
        std::string MediaName;
        bool bRunning = false;
        // Looks like a game (game_detection.hpp), decided from the node's
        // props; GameHintsKey is what it was decided from, so it's only
        // re-decided (with /proc reads) when those change, not on every
        // media.name update.
        bool bGame = false;
        std::string GameHintsKey;
        pw_proxy* Proxy = nullptr;
        spa_hook Listener{};
    };
    // pw_registry callback trampoline targets; not for external use.
    void HandleGlobalAdded(uint32 Id, const char* Type, const spa_dict* Props);
    void HandleGlobalRemoved(uint32 Id);

    // pw_node info callback trampoline target; not for external use.
    void HandleStreamInfo(uint32 Id, const pw_node_info* Info);

    // pw_metadata callback trampoline target; not for external use.
    void HandleMetadataProperty(uint32 Subject, const char* Key, const char* Type, const char* Value);

    // Sets default.configured.audio.sink back to whatever it was before
    // ClaimVirtualSinkAsDefault() first overwrote it (captured via
    // HandleMetadataProperty's initial property dump - see
    // PreviousDefaultSinkValue). No-op if that value was never captured
    // (e.g. the "default" metadata object never appeared). Must run while
    // Core/DefaultMetadata are still alive, so callers need to invoke this
    // before tearing this object down - AudioEngine::Teardown() does so
    // explicitly, the same way it stops ControlServer before resetting it.
    void RestorePreviousDefaultSink();

private:
    // Sets default.configured.audio.sink to VirtualSink::NodeName via the
    // bound "default" metadata object. Called once, as soon as that
    // object is bound (see HandleGlobalAdded) - re-sent every time the
    // daemon (re)starts and this registry connection reconnects, since a
    // fresh connection re-discovers the metadata global from scratch.
    void ClaimVirtualSinkAsDefault();

    pw_loop* Loop;
    pw_context* Context = nullptr;
    pw_core* Core = nullptr;
    pw_registry* Registry = nullptr;
    spa_hook RegistryListener{};
    pw_metadata* DefaultMetadata = nullptr;
    spa_hook MetadataListener{};

    // The pre-existing default.configured.audio.sink value, captured from
    // the first property event the "default" metadata object sends after
    // ClaimVirtualSinkAsDefault()'s listener is added - which is the
    // server's dump of current state, arriving before our own overwrite
    // takes effect (bind and the following set_property are processed in
    // order on the same connection). Empty/unset if never captured.
    bool bCapturedPreviousDefaultSink = false;
    std::string PreviousDefaultSinkValue;

    mutable std::mutex DevicesMutex;
    std::unordered_map<uint32, AudioDeviceInfo> DevicesById;

    struct LinkEnds
    {
        uint32 OutputNode = 0;
        uint32 InputNode = 0;
    };

    // Guards everything below. Written on the main-loop thread (registry/
    // node events), read from control threads (GetContentStreams).
    mutable std::mutex StreamsMutex;
    std::unordered_map<uint32, std::unique_ptr<TrackedStream>> StreamsById;
    std::unordered_map<uint32, LinkEnds> LinksById;
    uint32 VirtualSinkNodeId = 0xFFFFFFFFu; // SPA_ID_INVALID until our sink's global appears

    // Client global id -> pipewire.sec.pid (set by the server from the
    // socket's credentials). The process behind a native PipeWire stream,
    // whose node carries no application.process.id. Main-loop thread only.
    std::unordered_map<uint32, int64_t> ClientPids;
    // Apps already logged as games, so a game with several streams is
    // logged once. Main-loop thread only.
    std::unordered_set<std::string> LoggedGameApps;
};

} // namespace ramkolfx
