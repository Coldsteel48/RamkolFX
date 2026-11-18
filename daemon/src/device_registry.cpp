// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "device_registry.hpp"

#include <algorithm>
#include <unordered_set>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
#include <spa/utils/json.h>

#include "virtual_sink.hpp"

namespace ramkolfx
{

namespace
{

void OnGlobalAdded(void* UserData, uint32 Id, uint32 Permissions, const char* Type,
                    uint32 Version, const spa_dict* Props)
{
    (void)Permissions;
    (void)Version;
    static_cast<DeviceRegistry*>(UserData)->HandleGlobalAdded(Id, Type, Props);
}

void OnGlobalRemoved(void* UserData, uint32 Id)
{
    static_cast<DeviceRegistry*>(UserData)->HandleGlobalRemoved(Id);
}

const struct pw_registry_events RegistryEvents = []
{
    struct pw_registry_events Events{};
    Events.version = PW_VERSION_REGISTRY_EVENTS;
    Events.global = OnGlobalAdded;
    Events.global_remove = OnGlobalRemoved;
    return Events;
}();

// WaitForInitialSync's roundtrip state: a pointer to this struct is the
// pw_core_events 'done' listener's user data.
struct CoreSyncState
{
    int PendingSeq = -1;
    bool bDone = false;
};

void OnCoreDone(void* UserData, uint32 Id, int Seq)
{
    auto* State = static_cast<CoreSyncState*>(UserData);
    if (Id == PW_ID_CORE && Seq == State->PendingSeq)
    {
        State->bDone = true;
    }
}

const struct pw_core_events CoreEvents = []
{
    struct pw_core_events Events{};
    Events.version = PW_VERSION_CORE_EVENTS;
    Events.done = OnCoreDone;
    return Events;
}();

int OnMetadataProperty(void* UserData, uint32 Subject, const char* Key, const char* Type, const char* Value)
{
    static_cast<DeviceRegistry*>(UserData)->HandleMetadataProperty(Subject, Key, Type, Value);
    return 0;
}

void OnStreamNodeInfo(void* UserData, const pw_node_info* Info);

const struct pw_node_events StreamNodeEvents = []
{
    struct pw_node_events Events{};
    Events.version = PW_VERSION_NODE_EVENTS;
    Events.info = OnStreamNodeInfo;
    return Events;
}();

// spa_dict lookup of the first present key, or nullptr.
const char* LookupFirst(const spa_dict* Props, std::initializer_list<const char*> Keys)
{
    for (const char* Key : Keys)
    {
        if (const char* Value = spa_dict_lookup(Props, Key); Value && *Value)
        {
            return Value;
        }
    }
    return nullptr;
}

const struct pw_metadata_events MetadataEvents = []
{
    struct pw_metadata_events Events{};
    Events.version = PW_VERSION_METADATA_EVENTS;
    Events.property = OnMetadataProperty;
    return Events;
}();

void OnStreamNodeInfo(void* UserData, const pw_node_info* Info)
{
    auto* Stream = static_cast<DeviceRegistry::TrackedStream*>(UserData);
    Stream->Owner->HandleStreamInfo(Stream->Id, Info);
}

} // namespace

DeviceRegistry::DeviceRegistry(pw_loop* InLoop) : Loop(InLoop)
{
}

DeviceRegistry::~DeviceRegistry()
{
    for (auto& [Id, Stream] : StreamsById)
    {
        spa_hook_remove(&Stream->Listener);
        pw_proxy_destroy(Stream->Proxy);
    }
    StreamsById.clear();
    if (DefaultMetadata)
    {
        spa_hook_remove(&MetadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(DefaultMetadata));
    }
    if (Registry)
    {
        spa_hook_remove(&RegistryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(Registry));
    }
    if (Core)
    {
        pw_core_disconnect(Core);
    }
    if (Context)
    {
        pw_context_destroy(Context);
    }
}

bool DeviceRegistry::Start()
{
    Context = pw_context_new(Loop, nullptr, 0);
    if (!Context)
    {
        fprintf(stderr, "[ramkolfxd] failed to create device registry context\n");
        return false;
    }

    Core = pw_context_connect(Context, nullptr, 0);
    if (!Core)
    {
        fprintf(stderr, "[ramkolfxd] failed to connect device registry core\n");
        return false;
    }

    Registry = pw_core_get_registry(Core, PW_VERSION_REGISTRY, 0);
    if (!Registry)
    {
        fprintf(stderr, "[ramkolfxd] failed to get PipeWire registry\n");
        return false;
    }

    pw_registry_add_listener(Registry, &RegistryListener, &RegistryEvents, this);
    return true;
}

bool DeviceRegistry::WaitForInitialSync(int TimeoutMs)
{
    CoreSyncState State;
    spa_hook CoreListener{};
    pw_core_add_listener(Core, &CoreListener, &CoreEvents, &State);
    State.PendingSeq = pw_core_sync(Core, PW_ID_CORE, 0);

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
    while (!State.bDone && std::chrono::steady_clock::now() < Deadline)
    {
        pw_loop_iterate(Loop, 50);
    }

    spa_hook_remove(&CoreListener);
    if (!State.bDone)
    {
        fprintf(stderr, "[ramkolfxd] device registry initial sync timed out after %dms\n", TimeoutMs);
    }
    return State.bDone;
}

void DeviceRegistry::HandleGlobalAdded(uint32 Id, const char* Type, const spa_dict* Props)
{
    if (!Props)
    {
        return;
    }

    if (std::strcmp(Type, PW_TYPE_INTERFACE_Metadata) == 0)
    {
        // The session-wide "default" metadata object - there's exactly
        // one, holding default.configured.audio.sink/source among other
        // things. Bind it once and immediately claim the virtual sink.
        const char* MetadataName = spa_dict_lookup(Props, PW_KEY_METADATA_NAME);
        if (!DefaultMetadata && MetadataName && std::strcmp(MetadataName, "default") == 0)
        {
            DefaultMetadata = reinterpret_cast<pw_metadata*>(
                pw_registry_bind(Registry, Id, Type, PW_VERSION_METADATA, 0));
            if (DefaultMetadata)
            {
                // Must be added before ClaimVirtualSinkAsDefault() below: the
                // server replays its current property set to a freshly-bound
                // metadata object's listener before processing any further
                // request on this connection, so HandleMetadataProperty sees
                // the pre-existing default.configured.audio.sink (if any)
                // ahead of our own overwrite.
                pw_metadata_add_listener(DefaultMetadata, &MetadataListener, &MetadataEvents, this);
                ClaimVirtualSinkAsDefault();
            }
        }
        return;
    }

    if (std::strcmp(Type, PW_TYPE_INTERFACE_Link) == 0)
    {
        const char* OutputNode = spa_dict_lookup(Props, PW_KEY_LINK_OUTPUT_NODE);
        const char* InputNode = spa_dict_lookup(Props, PW_KEY_LINK_INPUT_NODE);
        if (OutputNode && InputNode)
        {
            std::lock_guard<std::mutex> Lock(StreamsMutex);
            LinksById[Id] = {static_cast<uint32>(std::strtoul(OutputNode, nullptr, 10)),
                             static_cast<uint32>(std::strtoul(InputNode, nullptr, 10))};
        }
        return;
    }

    if (std::strcmp(Type, PW_TYPE_INTERFACE_Node) != 0)
    {
        return;
    }

    const char* MediaClass = spa_dict_lookup(Props, PW_KEY_MEDIA_CLASS);
    if (MediaClass && std::strcmp(MediaClass, "Stream/Output/Audio") == 0)
    {
        auto Stream = std::make_unique<TrackedStream>();
        Stream->Owner = this;
        Stream->Id = Id;
        const char* AppName =
            LookupFirst(Props, {PW_KEY_APP_NAME, PW_KEY_APP_PROCESS_BINARY, PW_KEY_NODE_NAME});
        Stream->AppName = AppName ? AppName : "Unknown";
        if (const char* MediaName = LookupFirst(Props, {PW_KEY_MEDIA_NAME, PW_KEY_NODE_DESCRIPTION}))
        {
            Stream->MediaName = MediaName;
        }
        Stream->Proxy = static_cast<pw_proxy*>(pw_registry_bind(Registry, Id, Type, PW_VERSION_NODE, 0));
        if (!Stream->Proxy)
        {
            return;
        }
        pw_proxy_add_object_listener(Stream->Proxy, &Stream->Listener, &StreamNodeEvents, Stream.get());

        std::lock_guard<std::mutex> Lock(StreamsMutex);
        StreamsById[Id] = std::move(Stream);
        return;
    }

    if (!MediaClass || std::strcmp(MediaClass, "Audio/Sink") != 0)
    {
        return;
    }

    const char* NodeName = spa_dict_lookup(Props, PW_KEY_NODE_NAME);
    if (NodeName && std::strcmp(NodeName, VirtualSink::NodeName) == 0)
    {
        // Our own virtual sink - remembered so GetContentStreams() can
        // tell which streams feed it, but never listed as a device:
        // selecting ourselves as the hardware output target would loop the
        // signal back into the capture side.
        std::lock_guard<std::mutex> Lock(StreamsMutex);
        VirtualSinkNodeId = Id;
        return;
    }
    if (!NodeName)
    {
        return;
    }

    const char* Description = spa_dict_lookup(Props, PW_KEY_NODE_DESCRIPTION);

    AudioDeviceInfo Info;
    Info.Name = NodeName;
    Info.Description = Description ? Description : NodeName;

    std::lock_guard<std::mutex> Lock(DevicesMutex);
    DevicesById[Id] = std::move(Info);
}

void DeviceRegistry::ClaimVirtualSinkAsDefault()
{
    // Same key/value shape `wpctl set-default` writes. Re-sending it even
    // when it's already the persisted default is the point: PipeWire's
    // metadata implementation broadcasts a property event on every
    // set_property() call regardless of whether the value actually
    // changed, and that's what nudges WirePlumber's default-nodes policy
    // to reconcile any stream that's been sitting unlinked since before
    // this daemon (re)started.
    char Value[128];
    std::snprintf(Value, sizeof(Value), "{ \"name\": \"%s\" }", VirtualSink::NodeName);
    pw_metadata_set_property(DefaultMetadata, PW_ID_CORE, "default.configured.audio.sink",
                              "Spa:String:JSON", Value);
}

void DeviceRegistry::HandleMetadataProperty(uint32 Subject, const char* Key, const char* Type, const char* Value)
{
    (void)Type;
    if (bCapturedPreviousDefaultSink || Subject != PW_ID_CORE || !Key ||
        std::strcmp(Key, "default.configured.audio.sink") != 0)
    {
        return;
    }

    // Only the first event matters - it's the server's dump of whatever was
    // configured before we got here (see the doc comment on the listener
    // registration in HandleGlobalAdded). A null Value means no default was
    // configured at all; leave PreviousDefaultSinkValue empty so
    // RestorePreviousDefaultSink() knows there's nothing to restore.
    bCapturedPreviousDefaultSink = true;
    if (Value)
    {
        PreviousDefaultSinkValue = Value;
    }
}

void DeviceRegistry::RestorePreviousDefaultSink()
{
    if (!DefaultMetadata || !Core || PreviousDefaultSinkValue.empty())
    {
        return;
    }

    pw_metadata_set_property(DefaultMetadata, PW_ID_CORE, "default.configured.audio.sink",
                              "Spa:String:JSON", PreviousDefaultSinkValue.c_str());

    // The main loop has already stopped running by the time AudioEngine::
    // Teardown() calls this, so nothing will otherwise flush the request
    // above (or the property change event it triggers) before Teardown()
    // goes on to destroy Core/Context right after. Pump a manual sync
    // round-trip instead, same pattern as WaitForInitialSync.
    CoreSyncState State;
    spa_hook CoreListener{};
    pw_core_add_listener(Core, &CoreListener, &CoreEvents, &State);
    State.PendingSeq = pw_core_sync(Core, PW_ID_CORE, 0);

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!State.bDone && std::chrono::steady_clock::now() < Deadline)
    {
        pw_loop_iterate(Loop, 50);
    }
    spa_hook_remove(&CoreListener);

    if (!State.bDone)
    {
        fprintf(stderr, "[ramkolfxd] timed out restoring previous default audio sink\n");
    }
}

void DeviceRegistry::HandleGlobalRemoved(uint32 Id)
{
    {
        std::lock_guard<std::mutex> Lock(DevicesMutex);
        DevicesById.erase(Id);
    }

    std::unique_ptr<TrackedStream> RemovedStream;
    {
        std::lock_guard<std::mutex> Lock(StreamsMutex);
        LinksById.erase(Id);
        if (Id == VirtualSinkNodeId)
        {
            VirtualSinkNodeId = 0xFFFFFFFFu;
        }
        if (auto It = StreamsById.find(Id); It != StreamsById.end())
        {
            RemovedStream = std::move(It->second);
            StreamsById.erase(It);
        }
    }
    if (RemovedStream)
    {
        spa_hook_remove(&RemovedStream->Listener);
        pw_proxy_destroy(RemovedStream->Proxy);
    }
}

void DeviceRegistry::HandleStreamInfo(uint32 Id, const pw_node_info* Info)
{
    std::lock_guard<std::mutex> Lock(StreamsMutex);
    const auto It = StreamsById.find(Id);
    if (It == StreamsById.end())
    {
        return;
    }
    TrackedStream& Stream = *It->second;
    if (Info->change_mask & PW_NODE_CHANGE_MASK_STATE)
    {
        Stream.bRunning = Info->state == PW_NODE_STATE_RUNNING;
    }
    if ((Info->change_mask & PW_NODE_CHANGE_MASK_PROPS) && Info->props)
    {
        // Registry globals only carry a filtered subset of properties;
        // the bound node's full props are authoritative for the app name.
        if (const char* AppName = LookupFirst(Info->props, {PW_KEY_APP_NAME, PW_KEY_APP_PROCESS_BINARY}))
        {
            Stream.AppName = AppName;
        }
        // media.name tends to follow the current track/tab title.
        if (const char* MediaName = LookupFirst(Info->props, {PW_KEY_MEDIA_NAME, PW_KEY_NODE_DESCRIPTION}))
        {
            Stream.MediaName = MediaName;
        }
    }
}

std::vector<ContentStreamInfo> DeviceRegistry::GetContentStreams() const
{
    std::lock_guard<std::mutex> Lock(StreamsMutex);
    // One pass over the links (PipeWire has one per channel) to find who
    // feeds the virtual sink, instead of scanning them all per stream.
    std::unordered_set<uint32> FeedingNodes;
    for (const auto& [LinkId, Link] : LinksById)
    {
        if (Link.InputNode == VirtualSinkNodeId)
        {
            FeedingNodes.insert(Link.OutputNode);
        }
    }

    std::vector<ContentStreamInfo> Result;
    for (const auto& [Id, Stream] : StreamsById)
    {
        if (FeedingNodes.contains(Id))
        {
            Result.push_back({Stream->AppName, Stream->MediaName, Stream->bRunning, {}});
        }
    }
    std::sort(Result.begin(), Result.end(),
              [](const ContentStreamInfo& A, const ContentStreamInfo& B) { return A.AppName < B.AppName; });
    return Result;
}

std::vector<AudioDeviceInfo> DeviceRegistry::GetDevices() const
{
    std::lock_guard<std::mutex> Lock(DevicesMutex);
    std::vector<AudioDeviceInfo> Result;
    Result.reserve(DevicesById.size());
    for (const auto& [Id, Info] : DevicesById)
    {
        Result.push_back(Info);
    }
    return Result;
}

bool DeviceRegistry::HasDevice(const std::string& Name) const
{
    std::lock_guard<std::mutex> Lock(DevicesMutex);
    for (const auto& [Id, Info] : DevicesById)
    {
        if (Info.Name == Name)
        {
            return true;
        }
    }
    return false;
}

std::optional<std::string> DeviceRegistry::PickAnyDevice() const
{
    std::lock_guard<std::mutex> Lock(DevicesMutex);
    uint32 LowestId = std::numeric_limits<uint32>::max();
    const AudioDeviceInfo* Picked = nullptr;
    for (const auto& [Id, Info] : DevicesById)
    {
        if (Id < LowestId)
        {
            LowestId = Id;
            Picked = &Info;
        }
    }
    return Picked ? std::make_optional(Picked->Name) : std::nullopt;
}

std::optional<std::string> DeviceRegistry::GetPreviousDefaultSinkName() const
{
    if (!bCapturedPreviousDefaultSink || PreviousDefaultSinkValue.empty())
    {
        return std::nullopt;
    }

    // Value is JSON of the shape { "name": "alsa_output..." } - same shape
    // ClaimVirtualSinkAsDefault() writes, and what `wpctl set-default`
    // writes too.
    char NameBuffer[256];
    const int Length = spa_json_str_object_find(PreviousDefaultSinkValue.c_str(), PreviousDefaultSinkValue.size(),
                                                  "name", NameBuffer, sizeof(NameBuffer));
    if (Length <= 0)
    {
        return std::nullopt;
    }

    std::string Name(NameBuffer, static_cast<size_t>(Length));
    if (Name == VirtualSink::NodeName)
    {
        // A previous instance of this daemon crashed before it could
        // restore the real default sink, so the "previous" value captured
        // above is actually our own earlier claim, not a real device.
        // Using it would loop our own output back into our own capture -
        // exactly what pinning by node name exists to prevent.
        return std::nullopt;
    }
    return Name;
}

} // namespace ramkolfx
