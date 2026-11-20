// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "tray.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <dbus/dbus.h>
#include <unistd.h>

#include "logo.hpp"

namespace ramkolfx::gui
{

namespace
{

// Single-instance activation: the first GUI instance owns this name; a
// later one calls Activate on it and exits.
constexpr const char* AppBusName = "io.github.Coldsteel48.RamkolFX";
constexpr const char* AppObjectPath = "/io/github/Coldsteel48/RamkolFX";
constexpr const char* AppInterface = "io.github.Coldsteel48.RamkolFX";

constexpr const char* WatcherBusName = "org.kde.StatusNotifierWatcher";
constexpr const char* WatcherObjectPath = "/StatusNotifierWatcher";
constexpr const char* WatcherInterface = "org.kde.StatusNotifierWatcher";

constexpr const char* ItemObjectPath = "/StatusNotifierItem";
constexpr const char* ItemInterface = "org.kde.StatusNotifierItem";

constexpr const char* MenuObjectPath = "/MenuBar";
constexpr const char* MenuInterface = "com.canonical.dbusmenu";

constexpr const char* WatcherMatchRule = "type='signal',sender='org.freedesktop.DBus',"
                                         "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                                         "arg0='org.kde.StatusNotifierWatcher'";

// How long a second launch waits for the running instance to acknowledge
// Activate. Not answering in time doesn't change the outcome (see
// ClaimSingleInstance) - it only bounds how long the launch hangs around.
constexpr int ActivateTimeoutMs = 3000;
// How often a second launch tries again when the running instance exits
// between "the name is taken" and its Activate call.
constexpr int ClaimAttempts = 3;

// How long each registration step (claiming the item's bus name, then
// RegisterStatusNotifierItem) may wait for its reply before it counts as
// failed. libdbus has no timeouts of its own without a main loop
// integration, so Dispatch() enforces these.
constexpr std::chrono::milliseconds RegistrationStepTimeout{5000};

// A failed registration is retried after 1 s, then twice as long each
// time, up to this. Only after a few failures in a row is the tray
// reported as unusable (CanShowIcon) - the first ones may just be the
// panel still starting up.
constexpr std::chrono::milliseconds MaxRegisterRetryDelay{30000};
constexpr int FailuresBeforeReportingUnusable = 3;

// How long an xdg-activation token received from the tray host is kept
// waiting for the request it came with (normally a moment behind it).
constexpr std::chrono::milliseconds ActivationTokenLifetime{2000};

// A lost bus connection is re-established after this, doubling up to the
// maximum while attempts keep failing.
constexpr std::chrono::milliseconds InitialReconnectDelay{1000};
constexpr std::chrono::milliseconds MaxReconnectDelay{30000};
// Only a connection that stayed up this long resets that delay when it's
// lost, so a bus that accepts and then drops the connection at once isn't
// retried every second forever.
constexpr std::chrono::milliseconds StableConnectionTime{60000};
// How long a reconnect may take to get through authentication and the
// bus's Hello reply before it counts as failed (and is retried later).
constexpr std::chrono::milliseconds ReconnectHandshakeTimeout{5000};

// On exit, how long the last messages (the item's name release, so the
// panel drops the icon at once) may take to go out. A stalled bus would
// otherwise hold up quitting indefinitely; past this, the bus notices the
// closed socket instead.
constexpr std::chrono::milliseconds ShutdownFlushTimeout{250};

// dbusmenu item ids; 0 is always the root.
enum MenuItemId : int
{
    MenuRoot = 0,
    MenuShowHide = 1,
    MenuSeparator = 2,
    MenuQuit = 3,
    // The Priority submenu (docs/playback-priority-plan.md), with one
    // radio item per PlaybackPriority, in enum order.
    MenuPriority = 4,
    MenuPriorityFirstChoice = 5,
    MenuPriorityLastChoice = MenuPriorityFirstChoice + static_cast<int>(PlaybackPriority::Count) - 1,
    MenuPrioritySeparator = MenuPriorityLastChoice + 1,
    MenuLastId = MenuPrioritySeparator,
};
constexpr int MenuChildren[] = {MenuShowHide, MenuSeparator, MenuPriority, MenuPrioritySeparator, MenuQuit};

// One dbusmenu item property: dbusmenu mixes strings ("label"), ints
// ("toggle-state") and booleans ("visible").
struct MenuProperty
{
    const char* Name;
    int Type; // DBUS_TYPE_STRING, DBUS_TYPE_INT32 or DBUS_TYPE_BOOLEAN
    const char* String = nullptr;
    dbus_int32_t Int = 0;
};
MenuProperty StringProperty(const char* Name, const char* Value)
{
    return {Name, DBUS_TYPE_STRING, Value};
}
MenuProperty IntProperty(const char* Name, dbus_int32_t Value)
{
    return {Name, DBUS_TYPE_INT32, nullptr, Value};
}
MenuProperty BoolProperty(const char* Name, bool bValue)
{
    return {Name, DBUS_TYPE_BOOLEAN, nullptr, bValue ? 1 : 0};
}

#define RAMKOLFX_STANDARD_INTERFACES_XML                                                                       \
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"                                             \
    "    <method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>\n"              \
    "  </interface>\n"                                                                                         \
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"                                                 \
    "    <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"            \
    "<arg type=\"v\" direction=\"out\"/></method>\n"                                                           \
    "    <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/>"                                           \
    "<arg type=\"a{sv}\" direction=\"out\"/></method>\n"                                                       \
    "  </interface>\n"

#define RAMKOLFX_INTROSPECT_HEADER                                                                             \
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"                       \
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"

constexpr const char* ItemIntrospectXml =
    RAMKOLFX_INTROSPECT_HEADER
    "<node>\n"
    "  <interface name=\"org.kde.StatusNotifierItem\">\n"
    "    <property name=\"Category\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Id\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Title\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"WindowId\" type=\"i\" access=\"read\"/>\n"
    "    <property name=\"IconThemePath\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"IconName\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"IconPixmap\" type=\"a(iiay)\" access=\"read\"/>\n"
    "    <property name=\"OverlayIconName\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"OverlayIconPixmap\" type=\"a(iiay)\" access=\"read\"/>\n"
    "    <property name=\"AttentionIconName\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"AttentionIconPixmap\" type=\"a(iiay)\" access=\"read\"/>\n"
    "    <property name=\"AttentionMovieName\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"ToolTip\" type=\"(sa(iiay)ss)\" access=\"read\"/>\n"
    "    <property name=\"ItemIsMenu\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"Menu\" type=\"o\" access=\"read\"/>\n"
    "    <method name=\"ContextMenu\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "    <method name=\"Activate\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "    <method name=\"SecondaryActivate\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "    <method name=\"Scroll\"><arg name=\"delta\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"orientation\" type=\"s\" direction=\"in\"/></method>\n"
    "    <method name=\"ProvideXdgActivationToken\"><arg name=\"token\" type=\"s\" direction=\"in\"/></method>\n"
    "    <signal name=\"NewTitle\"/>\n"
    "    <signal name=\"NewIcon\"/>\n"
    "    <signal name=\"NewAttentionIcon\"/>\n"
    "    <signal name=\"NewOverlayIcon\"/>\n"
    "    <signal name=\"NewToolTip\"/>\n"
    "    <signal name=\"NewStatus\"><arg name=\"status\" type=\"s\"/></signal>\n"
    "  </interface>\n" RAMKOLFX_STANDARD_INTERFACES_XML "</node>\n";

constexpr const char* MenuIntrospectXml =
    RAMKOLFX_INTROSPECT_HEADER
    "<node>\n"
    "  <interface name=\"com.canonical.dbusmenu\">\n"
    "    <property name=\"Version\" type=\"u\" access=\"read\"/>\n"
    "    <property name=\"TextDirection\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"IconThemePath\" type=\"as\" access=\"read\"/>\n"
    "    <method name=\"GetLayout\"><arg name=\"parentId\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"recursionDepth\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"propertyNames\" type=\"as\" direction=\"in\"/>"
    "<arg name=\"revision\" type=\"u\" direction=\"out\"/>"
    "<arg name=\"layout\" type=\"(ia{sv}av)\" direction=\"out\"/></method>\n"
    "    <method name=\"GetGroupProperties\"><arg name=\"ids\" type=\"ai\" direction=\"in\"/>"
    "<arg name=\"propertyNames\" type=\"as\" direction=\"in\"/>"
    "<arg name=\"properties\" type=\"a(ia{sv})\" direction=\"out\"/></method>\n"
    "    <method name=\"GetProperty\"><arg name=\"id\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"name\" type=\"s\" direction=\"in\"/><arg name=\"value\" type=\"v\" direction=\"out\"/></method>\n"
    "    <method name=\"Event\"><arg name=\"id\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"eventId\" type=\"s\" direction=\"in\"/><arg name=\"data\" type=\"v\" direction=\"in\"/>"
    "<arg name=\"timestamp\" type=\"u\" direction=\"in\"/></method>\n"
    "    <method name=\"EventGroup\"><arg name=\"events\" type=\"a(isvu)\" direction=\"in\"/>"
    "<arg name=\"idErrors\" type=\"ai\" direction=\"out\"/></method>\n"
    "    <method name=\"AboutToShow\"><arg name=\"id\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"needUpdate\" type=\"b\" direction=\"out\"/></method>\n"
    "    <method name=\"AboutToShowGroup\"><arg name=\"ids\" type=\"ai\" direction=\"in\"/>"
    "<arg name=\"updatesNeeded\" type=\"ai\" direction=\"out\"/>"
    "<arg name=\"idErrors\" type=\"ai\" direction=\"out\"/></method>\n"
    "    <signal name=\"ItemsPropertiesUpdated\"><arg name=\"updatedProps\" type=\"a(ia{sv})\"/>"
    "<arg name=\"removedProps\" type=\"a(ias)\"/></signal>\n"
    "    <signal name=\"LayoutUpdated\"><arg name=\"revision\" type=\"u\"/>"
    "<arg name=\"parent\" type=\"i\"/></signal>\n"
    "  </interface>\n" RAMKOLFX_STANDARD_INTERFACES_XML "</node>\n";

#undef RAMKOLFX_STANDARD_INTERFACES_XML
#undef RAMKOLFX_INTROSPECT_HEADER

// ---------------------------------------------------------------------------
// Tray icon image

// A few common panel sizes, so hosts can pick one instead of rescaling.
const std::vector<logo::ArgbImage>& LogoIcons()
{
    // ARGB with straight alpha, bytes in network order - what SNI's IconPixmap wants.
    static const std::vector<logo::ArgbImage> Icons = {logo::RenderArgb(22), logo::RenderArgb(32),
                                                       logo::RenderArgb(48), logo::RenderArgb(64)};
    return Icons;
}

// ---------------------------------------------------------------------------
// Marshalling helpers

void AppendBasicVariant(DBusMessageIter* It, int Type, const void* Value)
{
    const char Signature[2] = {static_cast<char>(Type), '\0'};
    DBusMessageIter Variant;
    dbus_message_iter_open_container(It, DBUS_TYPE_VARIANT, Signature, &Variant);
    dbus_message_iter_append_basic(&Variant, Type, Value);
    dbus_message_iter_close_container(It, &Variant);
}

void AppendStringVariant(DBusMessageIter* It, const char* Value)
{
    AppendBasicVariant(It, DBUS_TYPE_STRING, &Value);
}

void AppendPropertyVariant(DBusMessageIter* It, const MenuProperty& Property)
{
    if (Property.Type == DBUS_TYPE_STRING)
    {
        AppendStringVariant(It, Property.String);
        return;
    }
    // A dbus_bool_t is a 32-bit int too, so Int serves both.
    AppendBasicVariant(It, Property.Type, &Property.Int);
}

// a(iiay)
void AppendPixmaps(DBusMessageIter* It, const std::vector<logo::ArgbImage>& Images)
{
    DBusMessageIter Array;
    dbus_message_iter_open_container(It, DBUS_TYPE_ARRAY, "(iiay)", &Array);
    for (const logo::ArgbImage& Image : Images)
    {
        DBusMessageIter Struct;
        DBusMessageIter Bytes;
        const dbus_int32_t Size = Image.Size;
        const unsigned char* Data = Image.Argb.data();
        dbus_message_iter_open_container(&Array, DBUS_TYPE_STRUCT, nullptr, &Struct);
        dbus_message_iter_append_basic(&Struct, DBUS_TYPE_INT32, &Size);
        dbus_message_iter_append_basic(&Struct, DBUS_TYPE_INT32, &Size);
        dbus_message_iter_open_container(&Struct, DBUS_TYPE_ARRAY, "y", &Bytes);
        dbus_message_iter_append_fixed_array(&Bytes, DBUS_TYPE_BYTE, &Data, static_cast<int>(Image.Argb.size()));
        dbus_message_iter_close_container(&Struct, &Bytes);
        dbus_message_iter_close_container(&Array, &Struct);
    }
    dbus_message_iter_close_container(It, &Array);
}

template <typename AppendValueFn>
void AppendDictEntry(DBusMessageIter* Dict, const char* Key, AppendValueFn&& AppendValue)
{
    DBusMessageIter Entry;
    dbus_message_iter_open_container(Dict, DBUS_TYPE_DICT_ENTRY, nullptr, &Entry);
    dbus_message_iter_append_basic(&Entry, DBUS_TYPE_STRING, &Key);
    AppendValue(&Entry);
    dbus_message_iter_close_container(Dict, &Entry);
}

// Reads one basic-typed argument and advances; false on a type mismatch.
bool ReadBasic(DBusMessageIter* It, int Type, void* Out)
{
    if (dbus_message_iter_get_arg_type(It) != Type)
    {
        return false;
    }
    dbus_message_iter_get_basic(It, Out);
    dbus_message_iter_next(It);
    return true;
}

void SendAndRelease(DBusConnection* Connection, DBusMessage* Message)
{
    if (Message)
    {
        dbus_connection_send(Connection, Message, nullptr);
        dbus_message_unref(Message);
    }
}

void ReplyEmpty(DBusConnection* Connection, DBusMessage* Call)
{
    SendAndRelease(Connection, dbus_message_new_method_return(Call));
}

void ReplyString(DBusConnection* Connection, DBusMessage* Call, const char* Value)
{
    DBusMessage* Reply = dbus_message_new_method_return(Call);
    dbus_message_append_args(Reply, DBUS_TYPE_STRING, &Value, DBUS_TYPE_INVALID);
    SendAndRelease(Connection, Reply);
}

void ReplyError(DBusConnection* Connection, DBusMessage* Call, const char* Name, const char* Text)
{
    SendAndRelease(Connection, dbus_message_new_error(Call, Name, Text));
}

// A call to the bus daemon itself (org.freedesktop.DBus) with one string
// argument, plus a uint32 one if Flags is given.
DBusMessage* NewBusCall(const char* Method, const char* Name, const dbus_uint32_t* Flags = nullptr)
{
    DBusMessage* Call = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, Method);
    if (!Call)
    {
        return nullptr;
    }
    if (Flags)
    {
        dbus_message_append_args(Call, DBUS_TYPE_STRING, &Name, DBUS_TYPE_UINT32, Flags, DBUS_TYPE_INVALID);
    }
    else
    {
        dbus_message_append_args(Call, DBUS_TYPE_STRING, &Name, DBUS_TYPE_INVALID);
    }
    return Call;
}

// Where to reconnect to the session bus without blocking: the address in
// the environment, else the standard per-user socket. Empty if neither is
// there - unlike dbus_bus_get_private(), this never tries X11 autolaunch,
// which would block.
std::string SessionBusAddress()
{
    if (const char* Address = std::getenv("DBUS_SESSION_BUS_ADDRESS"); Address && Address[0] != '\0')
    {
        return Address;
    }
    if (const char* RuntimeDir = std::getenv("XDG_RUNTIME_DIR"); RuntimeDir && RuntimeDir[0] != '\0')
    {
        const std::string Socket = std::string(RuntimeDir) + "/bus";
        if (access(Socket.c_str(), F_OK) == 0)
        {
            char* Escaped = dbus_address_escape_value(Socket.c_str());
            if (Escaped)
            {
                std::string Address = std::string("unix:path=") + Escaped;
                dbus_free(Escaped);
                return Address;
            }
        }
    }
    return {};
}

// An error reply's text, for logging.
std::string ErrorText(DBusMessage* Message)
{
    DBusError Error;
    dbus_error_init(&Error);
    dbus_set_error_from_message(&Error, Message);
    std::string Text = Error.message ? Error.message : (Error.name ? Error.name : "unknown error");
    dbus_error_free(&Error);
    return Text;
}

// ---------------------------------------------------------------------------
// StatusNotifierItem properties

constexpr const char* ItemPropertyNames[] = {
    "Category",          "Id",          "Title",           "Status",
    "WindowId",          "IconThemePath", "IconName",      "IconPixmap",
    "OverlayIconName",   "OverlayIconPixmap", "AttentionIconName", "AttentionIconPixmap",
    "AttentionMovieName", "ToolTip",    "ItemIsMenu",      "Menu",
};

// Appends Name's value as a variant; false if there's no such property.
bool AppendItemProperty(DBusMessageIter* It, const char* Name)
{
    const auto Is = [Name](const char* Candidate) { return std::strcmp(Name, Candidate) == 0; };

    if (Is("Category"))
    {
        AppendStringVariant(It, "ApplicationStatus");
    }
    else if (Is("Id"))
    {
        AppendStringVariant(It, "ramkolfx");
    }
    else if (Is("Title"))
    {
        AppendStringVariant(It, "RamkolFX");
    }
    else if (Is("Status"))
    {
        AppendStringVariant(It, "Active");
    }
    else if (Is("WindowId"))
    {
        const dbus_int32_t Zero = 0;
        AppendBasicVariant(It, DBUS_TYPE_INT32, &Zero);
    }
    else if (Is("IconThemePath") || Is("IconName") || Is("OverlayIconName") || Is("AttentionIconName") ||
             Is("AttentionMovieName"))
    {
        AppendStringVariant(It, ""); // empty IconName -> hosts use IconPixmap
    }
    else if (Is("IconPixmap") || Is("OverlayIconPixmap") || Is("AttentionIconPixmap"))
    {
        static const std::vector<logo::ArgbImage> NoImages;
        DBusMessageIter Variant;
        dbus_message_iter_open_container(It, DBUS_TYPE_VARIANT, "a(iiay)", &Variant);
        AppendPixmaps(&Variant, Is("IconPixmap") ? LogoIcons() : NoImages);
        dbus_message_iter_close_container(It, &Variant);
    }
    else if (Is("ToolTip"))
    {
        // (icon name, icon pixmaps, title, description)
        const char* Empty = "";
        const char* Title = "RamkolFX";
        const char* Description = "Spatial audio & EQ";
        DBusMessageIter Variant;
        DBusMessageIter Struct;
        dbus_message_iter_open_container(It, DBUS_TYPE_VARIANT, "(sa(iiay)ss)", &Variant);
        dbus_message_iter_open_container(&Variant, DBUS_TYPE_STRUCT, nullptr, &Struct);
        dbus_message_iter_append_basic(&Struct, DBUS_TYPE_STRING, &Empty);
        AppendPixmaps(&Struct, {});
        dbus_message_iter_append_basic(&Struct, DBUS_TYPE_STRING, &Title);
        dbus_message_iter_append_basic(&Struct, DBUS_TYPE_STRING, &Description);
        dbus_message_iter_close_container(&Variant, &Struct);
        dbus_message_iter_close_container(It, &Variant);
    }
    else if (Is("ItemIsMenu"))
    {
        // false: a left click activates (toggles the window); the menu is
        // on right click.
        const dbus_bool_t False = FALSE;
        AppendBasicVariant(It, DBUS_TYPE_BOOLEAN, &False);
    }
    else if (Is("Menu"))
    {
        AppendBasicVariant(It, DBUS_TYPE_OBJECT_PATH, &MenuObjectPath);
    }
    else
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// dbusmenu root properties

constexpr const char* MenuPropertyNames[] = {"Version", "TextDirection", "Status", "IconThemePath"};

bool AppendMenuRootProperty(DBusMessageIter* It, const char* Name)
{
    if (std::strcmp(Name, "Version") == 0)
    {
        const dbus_uint32_t Version = 3;
        AppendBasicVariant(It, DBUS_TYPE_UINT32, &Version);
    }
    else if (std::strcmp(Name, "TextDirection") == 0)
    {
        AppendStringVariant(It, "ltr");
    }
    else if (std::strcmp(Name, "Status") == 0)
    {
        AppendStringVariant(It, "normal");
    }
    else if (std::strcmp(Name, "IconThemePath") == 0)
    {
        DBusMessageIter Variant;
        DBusMessageIter Array;
        dbus_message_iter_open_container(It, DBUS_TYPE_VARIANT, "as", &Variant);
        dbus_message_iter_open_container(&Variant, DBUS_TYPE_ARRAY, "s", &Array);
        dbus_message_iter_close_container(&Variant, &Array);
        dbus_message_iter_close_container(It, &Variant);
    }
    else
    {
        return false;
    }
    return true;
}

// Whether a Properties call's interface argument names Interface. The
// D-Bus spec lets a caller pass "" to mean "whichever interface has it" -
// every property here belongs to the object's one interface.
bool MatchesPropertiesInterface(const char* RequestedInterface, const char* Interface)
{
    return RequestedInterface[0] == '\0' || std::strcmp(RequestedInterface, Interface) == 0;
}

// Shared Properties.Get / Properties.GetAll handling for both objects.
template <size_t N>
bool HandlePropertiesCall(DBusConnection* Connection, DBusMessage* Message, const char* Interface,
                          const char* const (&Names)[N], bool (*AppendProperty)(DBusMessageIter*, const char*))
{
    if (dbus_message_is_method_call(Message, DBUS_INTERFACE_PROPERTIES, "Get"))
    {
        const char* RequestedInterface = nullptr;
        const char* Name = nullptr;
        if (!dbus_message_get_args(Message, nullptr, DBUS_TYPE_STRING, &RequestedInterface, DBUS_TYPE_STRING, &Name,
                                   DBUS_TYPE_INVALID))
        {
            ReplyError(Connection, Message, DBUS_ERROR_INVALID_ARGS, "Expected (ss)");
            return true;
        }
        DBusMessage* Reply = dbus_message_new_method_return(Message);
        DBusMessageIter It;
        dbus_message_iter_init_append(Reply, &It);
        if (!MatchesPropertiesInterface(RequestedInterface, Interface) || !AppendProperty(&It, Name))
        {
            dbus_message_unref(Reply);
            ReplyError(Connection, Message, DBUS_ERROR_UNKNOWN_PROPERTY, "No such property");
            return true;
        }
        SendAndRelease(Connection, Reply);
        return true;
    }
    if (dbus_message_is_method_call(Message, DBUS_INTERFACE_PROPERTIES, "GetAll"))
    {
        const char* RequestedInterface = nullptr;
        if (!dbus_message_get_args(Message, nullptr, DBUS_TYPE_STRING, &RequestedInterface, DBUS_TYPE_INVALID))
        {
            ReplyError(Connection, Message, DBUS_ERROR_INVALID_ARGS, "Expected (s)");
            return true;
        }
        DBusMessage* Reply = dbus_message_new_method_return(Message);
        DBusMessageIter It;
        DBusMessageIter Dict;
        dbus_message_iter_init_append(Reply, &It);
        dbus_message_iter_open_container(&It, DBUS_TYPE_ARRAY, "{sv}", &Dict);
        if (MatchesPropertiesInterface(RequestedInterface, Interface))
        {
            for (const char* Name : Names)
            {
                AppendDictEntry(&Dict, Name, [&](DBusMessageIter* Entry) { AppendProperty(Entry, Name); });
            }
        }
        dbus_message_iter_close_container(&It, &Dict);
        SendAndRelease(Connection, Reply);
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------

struct Tray::Filter
{
    static DBusHandlerResult Handle(DBusConnection*, DBusMessage* Message, void* UserData)
    {
        return static_cast<Tray*>(UserData)->HandleMessage(Message) ? DBUS_HANDLER_RESULT_HANDLED
                                                                    : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
};

Tray::Tray()
{
    if (!Connect(true))
    {
        fprintf(stderr, "[ramkolfx-gui] no D-Bus session bus - tray icon unavailable\n");
    }
}

Tray::~Tray()
{
    if (!Connection)
    {
        return;
    }
    UnregisterItem();
    // Not dbus_connection_flush(): it waits without a timeout for the
    // socket to take everything queued, which a stalled bus never does.
    // Each read_write() pass waits at most for what's left of the budget
    // (the socket is non-blocking), and stops early once all is written or
    // the connection is gone.
    const Clock::time_point Deadline = Clock::now() + ShutdownFlushTimeout;
    while (dbus_connection_has_messages_to_send(Connection))
    {
        const auto RemainingMs =
            std::chrono::ceil<std::chrono::milliseconds>(Deadline - Clock::now()).count();
        if (RemainingMs <= 0 || !dbus_connection_read_write(Connection, static_cast<int>(RemainingMs)))
        {
            break;
        }
    }
    dbus_connection_close(Connection);
    dbus_connection_unref(Connection);
}

bool Tray::Connect(bool bAtStartup)
{
    // A private connection, so nothing else in the process (SDL talks to
    // the session bus too) shares its dispatch queue.
    DBusError Error;
    dbus_error_init(&Error);
    DBusConnection* NewConnection = nullptr;
    if (bAtStartup)
    {
        // Allowed to block (no window yet): authenticates and waits for
        // the bus's Hello reply, and finds the bus every way libdbus knows.
        NewConnection = dbus_bus_get_private(DBUS_BUS_SESSION, &Error);
    }
    else
    {
        // Must not block: opening only connects the local socket.
        // Authentication then runs in Dispatch()'s non-blocking I/O, and
        // Hello is sent like any other call - its reply picked out by
        // serial, with a deadline (RunTimedWork). Everything sent below
        // queues behind it, and the bus handles it in order.
        const std::string Address = SessionBusAddress();
        if (!Address.empty())
        {
            NewConnection = dbus_connection_open_private(Address.c_str(), &Error);
        }
    }
    dbus_error_free(&Error);
    if (!NewConnection)
    {
        return false;
    }
    Connection = NewConnection;
    dbus_connection_set_exit_on_disconnect(Connection, FALSE);
    dbus_connection_add_filter(Connection, &Filter::Handle, this, nullptr);
    if (!bAtStartup)
    {
        HelloSerial = SendCall(dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
                                                            DBUS_INTERFACE_DBUS, "Hello"));
        bHelloFailed = HelloSerial == 0;
        HelloDeadline = Clock::now() + ReconnectHandshakeTimeout;
    }

    // Subscribe before querying, so a watcher that appears in between
    // still gets noticed. A null error makes add_match not wait.
    dbus_bus_add_match(Connection, WatcherMatchRule, nullptr);
    WatcherQuerySerial = SendCall(NewBusCall("NameHasOwner", WatcherBusName));
    if (bOwnsAppName)
    {
        // Back after a lost connection. Queued rather than refused if
        // someone took the name meanwhile: it comes back to this process
        // once they let go of it.
        const dbus_uint32_t Flags = 0;
        AppNameSerial = SendCall(NewBusCall("RequestName", AppBusName, &Flags));
    }
    ConnectedAt = Clock::now();
    return true;
}

void Tray::DropConnection()
{
    fprintf(stderr, "[ramkolfx-gui] lost the D-Bus session bus connection - reconnecting\n");
    dbus_connection_close(Connection);
    dbus_connection_unref(Connection);
    Connection = nullptr;

    // Everything below belonged to that connection.
    HelloSerial = 0;
    bHelloFailed = false;
    bWatcherPresent = false;
    WatcherQuerySerial = 0;
    AppNameSerial = 0;
    bRegistered = false;
    Step = RegistrationStep::None;
    RegistrationSerial = 0;
    ItemServiceName.clear(); // released along with the connection
    ResetRegistrationFailures();

    const bool bWasStable = Clock::now() - ConnectedAt >= StableConnectionTime;
    ReconnectDelay = (ReconnectDelay.count() == 0 || bWasStable) ? InitialReconnectDelay
                                                                 : std::min(ReconnectDelay * 2, MaxReconnectDelay);
    ReconnectAt = Clock::now() + ReconnectDelay;
    bReconnectScheduled = true;
}

unsigned int Tray::SendCall(DBusMessage* Call)
{
    if (!Call)
    {
        return 0;
    }
    dbus_uint32_t Serial = 0;
    if (!Connection || !dbus_connection_send(Connection, Call, &Serial))
    {
        Serial = 0;
    }
    dbus_message_unref(Call);
    return Serial;
}

bool Tray::ClaimSingleInstance(bool bActivateExisting)
{
    if (!Connection)
    {
        return true;
    }

    // Startup only, before any window exists - so these calls block.
    for (int Attempt = 0; Attempt < ClaimAttempts; ++Attempt)
    {
        DBusError Error;
        dbus_error_init(&Error);
        const int Result = dbus_bus_request_name(Connection, AppBusName, DBUS_NAME_FLAG_DO_NOT_QUEUE, &Error);
        if (Result != DBUS_REQUEST_NAME_REPLY_EXISTS)
        {
            // Ours - or the bus refused outright, and this just runs
            // without single-instance activation.
            bOwnsAppName = Result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
                           Result == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
            if (!bOwnsAppName)
            {
                fprintf(stderr, "[ramkolfx-gui] couldn't claim %s (%s) - running without single-instance activation\n",
                        AppBusName, Error.message ? Error.message : "unknown error");
            }
            dbus_error_free(&Error);
            return true;
        }
        if (!bActivateExisting)
        {
            return false;
        }

        DBusMessage* Call = dbus_message_new_method_call(AppBusName, AppObjectPath, AppInterface, "Activate");
        // Only the running instance, never a service file it doesn't have.
        dbus_message_set_auto_start(Call, FALSE);
        const char* Token = std::getenv("XDG_ACTIVATION_TOKEN");
        if (!Token)
        {
            Token = "";
        }
        dbus_message_append_args(Call, DBUS_TYPE_STRING, &Token, DBUS_TYPE_INVALID);
        DBusMessage* Reply = dbus_connection_send_with_reply_and_block(Connection, Call, ActivateTimeoutMs, &Error);
        dbus_message_unref(Call);
        if (Reply)
        {
            dbus_message_unref(Reply);
            return false;
        }
        if (dbus_error_has_name(&Error, DBUS_ERROR_NAME_HAS_NO_OWNER) ||
            dbus_error_has_name(&Error, DBUS_ERROR_SERVICE_UNKNOWN))
        {
            // It exited in the meantime - the name is free to claim now.
            dbus_error_free(&Error);
            continue;
        }
        // Busy (still starting up, or stuck in a slow call) rather than
        // gone: it still owns the name, and the Activate stays queued for
        // it - it shows its window once it catches up. A second window now
        // would leave two GUIs sharing one config, and this one couldn't
        // take over the name for later launches anyway.
        fprintf(stderr,
                "[ramkolfx-gui] another instance is running but hasn't answered yet (%s) - it will show its "
                "window when it catches up\n",
                Error.message ? Error.message : "unknown error");
        dbus_error_free(&Error);
        return false;
    }
    return true; // the name kept changing hands - run, without it
}

void Tray::SetIconVisible(bool bVisible)
{
    if (bVisible == bIconWanted)
    {
        return;
    }
    bIconWanted = bVisible;
    ResetRegistrationFailures();
    if (!Connection)
    {
        return;
    }
    if (bVisible)
    {
        if (bWatcherPresent)
        {
            RegisterItem();
        }
    }
    else
    {
        UnregisterItem();
    }
}

void Tray::SetWindowShown(bool bShown)
{
    if (bShown == bWindowShown)
    {
        return;
    }
    bWindowShown = bShown;
    ++MenuRevision;
    EmitLayoutUpdated();
}

void Tray::SetPlaybackPriority(bool bSupported, std::optional<PlaybackPriority> Current)
{
    if (bSupported == bPrioritySupported && Current == CurrentPriority)
    {
        return;
    }
    bPrioritySupported = bSupported;
    CurrentPriority = Current;
    ++MenuRevision;
    EmitLayoutUpdated();
}

int Tray::GetPollFd() const
{
    int Fd = -1;
    if (!Connection || !dbus_connection_get_unix_fd(Connection, &Fd))
    {
        return -1;
    }
    return Fd;
}

bool Tray::HasPendingOutput() const
{
    return Connection && dbus_connection_has_messages_to_send(Connection);
}

void Tray::Dispatch(bool bSocketReady)
{
    if (!Connection)
    {
        if (!bReconnectScheduled || Clock::now() < ReconnectAt)
        {
            return;
        }
        bReconnectScheduled = false;
        if (!Connect(false))
        {
            // Not back yet - DropConnection's bookkeeping, minus a
            // connection to drop.
            ReconnectDelay = std::min(ReconnectDelay * 2, MaxReconnectDelay);
            ReconnectAt = Clock::now() + ReconnectDelay;
            bReconnectScheduled = true;
            return;
        }
        bSocketReady = true; // the caller's hint was about the old socket
    }
    if (bSocketReady && !dbus_connection_read_write(Connection, 0))
    {
        DropConnection();
        return;
    }
    // Nothing here blocks or reads from the socket, so after this the
    // incoming queue stays empty until the socket turns readable again -
    // which is what an idle caller waits on. Run even without I/O: the
    // blocking calls at startup (connecting, ClaimSingleInstance) may have
    // queued messages that arrived alongside their replies.
    while (dbus_connection_dispatch(Connection) == DBUS_DISPATCH_DATA_REMAINS)
    {
    }
    // read_write() can still succeed on the pass that reads the hangup -
    // libdbus then just queues its Disconnected message, dispatched above.
    // Notice it now: an idle caller has no socket left to wake it for the
    // next pass, and the icon must stop counting as shown right away.
    if (!dbus_connection_get_is_connected(Connection))
    {
        DropConnection();
        return;
    }
    RunTimedWork();
}

void Tray::RunTimedWork()
{
    const Clock::time_point Now = Clock::now();
    if (bHelloFailed || (HelloSerial != 0 && Now >= HelloDeadline))
    {
        // Not here in the reply handler: that runs inside
        // dbus_connection_dispatch(), which mustn't see its connection go.
        fprintf(stderr, "[ramkolfx-gui] the D-Bus session bus didn't accept the connection in time\n");
        DropConnection();
        return;
    }
    if (Step != RegistrationStep::None && Now >= RegistrationDeadline)
    {
        FailRegistration("the tray host didn't answer");
    }
    if (bRetryScheduled && Now >= RetryAt)
    {
        bRetryScheduled = false;
        if (bIconWanted && bWatcherPresent)
        {
            RegisterItem();
        }
    }
}

int Tray::GetTimeoutMs() const
{
    std::optional<Clock::time_point> Next;
    const auto Consider = [&Next](bool bActive, Clock::time_point At)
    {
        if (bActive && (!Next || At < *Next))
        {
            Next = At;
        }
    };
    Consider(bRetryScheduled, RetryAt);
    Consider(Step != RegistrationStep::None, RegistrationDeadline);
    Consider(!Connection && bReconnectScheduled, ReconnectAt);
    Consider(Connection && (HelloSerial != 0 || bHelloFailed), HelloDeadline);
    if (!Next)
    {
        return -1;
    }
    const auto Remaining = std::chrono::ceil<std::chrono::milliseconds>(*Next - Clock::now()).count();
    return static_cast<int>(std::max<decltype(Remaining)>(Remaining, 0));
}

Tray::Requests Tray::TakeRequests()
{
    // The token usually arrives just ahead of the Activate call it belongs
    // to, possibly in an earlier Dispatch() - so it's held until a request
    // comes along to carry it, but only briefly: one that came with
    // something else (a context menu, a scroll) must not be handed to an
    // unrelated request much later, by when the compositor would reject
    // it. It never outlives the request that does pick it up either.
    const bool bTokenFresh =
        !ActivationToken.empty() && Clock::now() - ActivationTokenReceivedAt < ActivationTokenLifetime;
    if (PendingRequests.Any() || !bTokenFresh)
    {
        std::string Token = std::exchange(ActivationToken, std::string());
        if (bTokenFresh)
        {
            PendingRequests.ActivationToken = std::move(Token);
        }
    }
    return std::exchange(PendingRequests, Requests{});
}

void Tray::OnWatcherChanged(bool bPresent)
{
    bWatcherPresent = bPresent;
    ResetRegistrationFailures();
    if (bIconWanted && bPresent)
    {
        RegisterItem();
    }
    else
    {
        UnregisterItem();
    }
}

void Tray::RegisterItem()
{
    UnregisterItem();
    bRetryScheduled = false;

    ItemServiceName =
        "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-" + std::to_string(++RegistrationCount);
    const dbus_uint32_t Flags = DBUS_NAME_FLAG_DO_NOT_QUEUE;
    RegistrationSerial = SendCall(NewBusCall("RequestName", ItemServiceName.c_str(), &Flags));
    if (RegistrationSerial == 0)
    {
        FailRegistration("couldn't send the request");
        return;
    }
    Step = RegistrationStep::ClaimingName;
    RegistrationDeadline = Clock::now() + RegistrationStepTimeout;
}

void Tray::HandleRegistrationReply(DBusMessage* Message)
{
    RegistrationSerial = 0;
    const bool bError = dbus_message_get_type(Message) == DBUS_MESSAGE_TYPE_ERROR;

    if (Step == RegistrationStep::ClaimingName)
    {
        dbus_uint32_t Result = 0;
        if (bError)
        {
            FailRegistration(("couldn't claim " + ItemServiceName + ": " + ErrorText(Message)).c_str());
            return;
        }
        if (!dbus_message_get_args(Message, nullptr, DBUS_TYPE_UINT32, &Result, DBUS_TYPE_INVALID) ||
            Result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        {
            FailRegistration(("couldn't claim " + ItemServiceName + ": name taken").c_str());
            return;
        }
        DBusMessage* Call = dbus_message_new_method_call(WatcherBusName, WatcherObjectPath, WatcherInterface,
                                                         "RegisterStatusNotifierItem");
        const char* Name = ItemServiceName.c_str();
        if (Call)
        {
            dbus_message_append_args(Call, DBUS_TYPE_STRING, &Name, DBUS_TYPE_INVALID);
        }
        RegistrationSerial = SendCall(Call);
        if (RegistrationSerial == 0)
        {
            FailRegistration("couldn't send the registration");
            return;
        }
        Step = RegistrationStep::Registering;
        RegistrationDeadline = Clock::now() + RegistrationStepTimeout;
        return;
    }

    // RegistrationStep::Registering
    if (bError)
    {
        FailRegistration(("the tray host rejected the tray icon: " + ErrorText(Message)).c_str());
        return;
    }
    Step = RegistrationStep::None;
    bRegistered = true;
    ResetRegistrationFailures();
}

void Tray::FailRegistration(const char* Reason)
{
    fprintf(stderr, "[ramkolfx-gui] tray icon registration failed (%s) - retrying\n", Reason);
    // Releases the item name too, so a watcher that does answer late (or a
    // name grant that arrives late) is undone rather than left registered
    // under a name this object no longer tracks: the bus handles this
    // release after any request still in flight for it.
    UnregisterItem();

    ++ConsecutiveFailures;
    bRegistrationFailed = ConsecutiveFailures >= FailuresBeforeReportingUnusable;
    const std::chrono::milliseconds Delay =
        std::min(std::chrono::milliseconds(1000 << std::min(ConsecutiveFailures - 1, 5)), MaxRegisterRetryDelay);
    RetryAt = Clock::now() + Delay;
    bRetryScheduled = true;
}

void Tray::ResetRegistrationFailures()
{
    ConsecutiveFailures = 0;
    bRegistrationFailed = false;
    bRetryScheduled = false;
}

void Tray::UnregisterItem()
{
    bRegistered = false;
    Step = RegistrationStep::None;
    RegistrationSerial = 0; // a late reply to it is now just ignored
    if (ItemServiceName.empty() || !Connection)
    {
        ItemServiceName.clear();
        return;
    }
    // SNI has no unregister call: the watcher drops an item when its bus
    // name goes away. Fire and forget - there's nothing to do on failure.
    DBusMessage* Call = NewBusCall("ReleaseName", ItemServiceName.c_str());
    if (Call)
    {
        dbus_message_set_no_reply(Call, TRUE);
        SendCall(Call);
    }
    ItemServiceName.clear();
}

void Tray::EmitLayoutUpdated()
{
    if (!Connection || ItemServiceName.empty())
    {
        return;
    }
    DBusMessage* Signal = dbus_message_new_signal(MenuObjectPath, MenuInterface, "LayoutUpdated");
    const dbus_uint32_t Revision = MenuRevision;
    const dbus_int32_t Parent = MenuRoot;
    dbus_message_append_args(Signal, DBUS_TYPE_UINT32, &Revision, DBUS_TYPE_INT32, &Parent, DBUS_TYPE_INVALID);
    SendAndRelease(Connection, Signal);
}

bool Tray::HandleReply(DBusMessage* Message)
{
    const dbus_uint32_t ReplySerial = dbus_message_get_reply_serial(Message);
    if (ReplySerial == 0)
    {
        return false;
    }
    const bool bError = dbus_message_get_type(Message) == DBUS_MESSAGE_TYPE_ERROR;

    if (ReplySerial == HelloSerial)
    {
        HelloSerial = 0;
        const char* UniqueName = nullptr;
        if (bError || !dbus_message_get_args(Message, nullptr, DBUS_TYPE_STRING, &UniqueName, DBUS_TYPE_INVALID))
        {
            bHelloFailed = true; // dropped by RunTimedWork, after dispatching
            return true;
        }
        dbus_bus_set_unique_name(Connection, UniqueName);
        fprintf(stderr, "[ramkolfx-gui] reconnected to the D-Bus session bus\n");
        return true;
    }
    if (ReplySerial == WatcherQuerySerial)
    {
        WatcherQuerySerial = 0;
        dbus_bool_t bHasOwner = FALSE;
        if (!bError && dbus_message_get_args(Message, nullptr, DBUS_TYPE_BOOLEAN, &bHasOwner, DBUS_TYPE_INVALID) &&
            static_cast<bool>(bHasOwner) != bWatcherPresent)
        {
            // Only on a change: a NameOwnerChanged that arrived first
            // already said the same, and registered.
            OnWatcherChanged(bHasOwner);
        }
        return true;
    }
    if (ReplySerial == AppNameSerial)
    {
        AppNameSerial = 0;
        dbus_uint32_t Result = 0;
        if (bError || !dbus_message_get_args(Message, nullptr, DBUS_TYPE_UINT32, &Result, DBUS_TYPE_INVALID) ||
            (Result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER && Result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER))
        {
            fprintf(stderr, "[ramkolfx-gui] couldn't re-claim %s after reconnecting - queued for it\n", AppBusName);
        }
        return true;
    }
    if (Step != RegistrationStep::None && ReplySerial == RegistrationSerial)
    {
        HandleRegistrationReply(Message);
        return true;
    }
    return false;
}

void Tray::StoreActivationToken(const char* Token)
{
    // An empty one (a second launch that had none) carries nothing, and
    // must not displace a real token the tray host just handed over.
    if (Token && Token[0] != '\0')
    {
        ActivationToken = Token;
        ActivationTokenReceivedAt = Clock::now();
    }
}

bool Tray::HandleMessage(DBusMessage* Message)
{
    if (dbus_message_is_signal(Message, DBUS_INTERFACE_DBUS, "NameOwnerChanged"))
    {
        const char* Name = nullptr;
        const char* OldOwner = nullptr;
        const char* NewOwner = nullptr;
        if (dbus_message_get_args(Message, nullptr, DBUS_TYPE_STRING, &Name, DBUS_TYPE_STRING, &OldOwner,
                                  DBUS_TYPE_STRING, &NewOwner, DBUS_TYPE_INVALID) &&
            std::strcmp(Name, WatcherBusName) == 0)
        {
            // A new watcher knows nothing of earlier registrations, and a
            // vanished one can't show the icon either way.
            OnWatcherChanged(NewOwner[0] != '\0');
        }
        return false; // let other filters see bus signals too
    }

    const int Type = dbus_message_get_type(Message);
    if (Type == DBUS_MESSAGE_TYPE_METHOD_RETURN || Type == DBUS_MESSAGE_TYPE_ERROR)
    {
        return HandleReply(Message);
    }
    if (Type != DBUS_MESSAGE_TYPE_METHOD_CALL)
    {
        return false;
    }
    const char* Path = dbus_message_get_path(Message);
    if (!Path)
    {
        return false;
    }

    if (std::strcmp(Path, AppObjectPath) == 0 && dbus_message_is_method_call(Message, AppInterface, "Activate"))
    {
        const char* Token = "";
        dbus_message_get_args(Message, nullptr, DBUS_TYPE_STRING, &Token, DBUS_TYPE_INVALID);
        StoreActivationToken(Token);
        PendingRequests.bShowWindow = true;
        ReplyEmpty(Connection, Message);
        return true;
    }
    if (std::strcmp(Path, ItemObjectPath) == 0)
    {
        return HandleItemCall(Message);
    }
    if (std::strcmp(Path, MenuObjectPath) == 0)
    {
        return HandleMenuCall(Message);
    }
    return false;
}

bool Tray::HandleItemCall(DBusMessage* Message)
{
    if (dbus_message_is_method_call(Message, DBUS_INTERFACE_INTROSPECTABLE, "Introspect"))
    {
        ReplyString(Connection, Message, ItemIntrospectXml);
        return true;
    }
    if (dbus_message_is_method_call(Message, ItemInterface, "Activate") ||
        dbus_message_is_method_call(Message, ItemInterface, "SecondaryActivate"))
    {
        PendingRequests.bToggleWindow = true;
        ReplyEmpty(Connection, Message);
        return true;
    }
    if (dbus_message_is_method_call(Message, ItemInterface, "ContextMenu") ||
        dbus_message_is_method_call(Message, ItemInterface, "Scroll"))
    {
        ReplyEmpty(Connection, Message); // the host shows Menu itself
        return true;
    }
    if (dbus_message_is_method_call(Message, ItemInterface, "ProvideXdgActivationToken"))
    {
        const char* Token = "";
        dbus_message_get_args(Message, nullptr, DBUS_TYPE_STRING, &Token, DBUS_TYPE_INVALID);
        StoreActivationToken(Token);
        ReplyEmpty(Connection, Message);
        return true;
    }
    return HandlePropertiesCall(Connection, Message, ItemInterface, ItemPropertyNames, &AppendItemProperty);
}

bool Tray::HandleMenuCall(DBusMessage* Message)
{
    const auto ItemProperties = [this](int Id) -> std::vector<MenuProperty>
    {
        if (Id >= MenuPriorityFirstChoice && Id <= MenuPriorityLastChoice)
        {
            const auto Choice = static_cast<PlaybackPriority>(Id - MenuPriorityFirstChoice);
            // No choice is checked until the GUI has heard from the daemon.
            return {StringProperty("label", PlaybackPriorityLabel(Choice)),
                    StringProperty("toggle-type", "radio"),
                    IntProperty("toggle-state", CurrentPriority == Choice ? 1 : 0)};
        }
        switch (Id)
        {
        case MenuRoot:
            return {StringProperty("children-display", "submenu")};
        case MenuShowHide:
            return {StringProperty("label", bWindowShown ? "Hide RamkolFX" : "Show RamkolFX")};
        case MenuSeparator:
            return {StringProperty("type", "separator")};
        case MenuPriority:
            return {StringProperty("label", "Priority"), StringProperty("children-display", "submenu"),
                    BoolProperty("visible", bPrioritySupported)};
        case MenuPrioritySeparator:
            return {StringProperty("type", "separator"), BoolProperty("visible", bPrioritySupported)};
        case MenuQuit:
            return {StringProperty("label", "Quit")};
        default:
            return {};
        }
    };
    const auto IsValidId = [](int Id) { return Id >= MenuRoot && Id <= MenuLastId; };
    const auto ChildrenOf = [](int Id) -> std::vector<int>
    {
        if (Id == MenuRoot)
        {
            return {std::begin(MenuChildren), std::end(MenuChildren)};
        }
        if (Id == MenuPriority)
        {
            std::vector<int> Choices;
            for (int Choice = MenuPriorityFirstChoice; Choice <= MenuPriorityLastChoice; ++Choice)
            {
                Choices.push_back(Choice);
            }
            return Choices;
        }
        return {};
    };

    // a{sv}
    const auto AppendProperties = [&](DBusMessageIter* It, int Id)
    {
        DBusMessageIter Dict;
        dbus_message_iter_open_container(It, DBUS_TYPE_ARRAY, "{sv}", &Dict);
        for (const MenuProperty& Property : ItemProperties(Id))
        {
            AppendDictEntry(&Dict, Property.Name,
                            [&](DBusMessageIter* Entry) { AppendPropertyVariant(Entry, Property); });
        }
        dbus_message_iter_close_container(It, &Dict);
    };

    // (ia{sv}av), recursing into children up to Depth levels (-1 = all).
    const auto AppendLayout = [&](DBusMessageIter* It, int Id, int Depth, const auto& Self) -> void
    {
        DBusMessageIter Struct;
        DBusMessageIter Children;
        const dbus_int32_t Id32 = Id;
        dbus_message_iter_open_container(It, DBUS_TYPE_STRUCT, nullptr, &Struct);
        dbus_message_iter_append_basic(&Struct, DBUS_TYPE_INT32, &Id32);
        AppendProperties(&Struct, Id);
        dbus_message_iter_open_container(&Struct, DBUS_TYPE_ARRAY, "v", &Children);
        if (Depth != 0)
        {
            for (const int Child : ChildrenOf(Id))
            {
                DBusMessageIter Variant;
                dbus_message_iter_open_container(&Children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &Variant);
                Self(&Variant, Child, Depth - 1, Self);
                dbus_message_iter_close_container(&Children, &Variant);
            }
        }
        dbus_message_iter_close_container(&Struct, &Children);
        dbus_message_iter_close_container(It, &Struct);
    };

    DBusMessageIter Args;
    dbus_message_iter_init(Message, &Args);

    if (dbus_message_is_method_call(Message, DBUS_INTERFACE_INTROSPECTABLE, "Introspect"))
    {
        ReplyString(Connection, Message, MenuIntrospectXml);
        return true;
    }
    if (dbus_message_is_method_call(Message, MenuInterface, "GetLayout"))
    {
        dbus_int32_t ParentId = 0;
        dbus_int32_t Depth = -1;
        if (!ReadBasic(&Args, DBUS_TYPE_INT32, &ParentId) || !ReadBasic(&Args, DBUS_TYPE_INT32, &Depth) ||
            !IsValidId(ParentId))
        {
            ReplyError(Connection, Message, DBUS_ERROR_INVALID_ARGS, "Unknown menu item");
            return true;
        }
        DBusMessage* Reply = dbus_message_new_method_return(Message);
        DBusMessageIter It;
        const dbus_uint32_t Revision = MenuRevision;
        dbus_message_iter_init_append(Reply, &It);
        dbus_message_iter_append_basic(&It, DBUS_TYPE_UINT32, &Revision);
        AppendLayout(&It, ParentId, Depth, AppendLayout);
        SendAndRelease(Connection, Reply);
        return true;
    }
    if (dbus_message_is_method_call(Message, MenuInterface, "GetGroupProperties"))
    {
        std::vector<int> Ids;
        if (dbus_message_iter_get_arg_type(&Args) == DBUS_TYPE_ARRAY)
        {
            DBusMessageIter IdArray;
            dbus_message_iter_recurse(&Args, &IdArray);
            dbus_int32_t Id = 0;
            while (ReadBasic(&IdArray, DBUS_TYPE_INT32, &Id))
            {
                Ids.push_back(Id);
            }
        }
        if (Ids.empty())
        {
            for (int Id = MenuRoot; Id <= MenuLastId; ++Id)
            {
                Ids.push_back(Id);
            }
        }
        DBusMessage* Reply = dbus_message_new_method_return(Message);
        DBusMessageIter It;
        DBusMessageIter Array;
        dbus_message_iter_init_append(Reply, &It);
        dbus_message_iter_open_container(&It, DBUS_TYPE_ARRAY, "(ia{sv})", &Array);
        for (const int Id : Ids)
        {
            if (!IsValidId(Id))
            {
                continue;
            }
            DBusMessageIter Struct;
            const dbus_int32_t Id32 = Id;
            dbus_message_iter_open_container(&Array, DBUS_TYPE_STRUCT, nullptr, &Struct);
            dbus_message_iter_append_basic(&Struct, DBUS_TYPE_INT32, &Id32);
            AppendProperties(&Struct, Id);
            dbus_message_iter_close_container(&Array, &Struct);
        }
        dbus_message_iter_close_container(&It, &Array);
        SendAndRelease(Connection, Reply);
        return true;
    }
    if (dbus_message_is_method_call(Message, MenuInterface, "GetProperty"))
    {
        dbus_int32_t Id = 0;
        const char* Name = nullptr;
        if (ReadBasic(&Args, DBUS_TYPE_INT32, &Id) && ReadBasic(&Args, DBUS_TYPE_STRING, &Name))
        {
            for (const MenuProperty& Property : ItemProperties(Id))
            {
                if (std::strcmp(Property.Name, Name) == 0)
                {
                    DBusMessage* Reply = dbus_message_new_method_return(Message);
                    DBusMessageIter It;
                    dbus_message_iter_init_append(Reply, &It);
                    AppendPropertyVariant(&It, Property);
                    SendAndRelease(Connection, Reply);
                    return true;
                }
            }
        }
        ReplyError(Connection, Message, DBUS_ERROR_INVALID_ARGS, "Unknown menu item or property");
        return true;
    }
    if (dbus_message_is_method_call(Message, MenuInterface, "Event"))
    {
        dbus_int32_t Id = 0;
        const char* EventId = nullptr;
        if (ReadBasic(&Args, DBUS_TYPE_INT32, &Id) && ReadBasic(&Args, DBUS_TYPE_STRING, &EventId))
        {
            HandleMenuEvent(Id, EventId);
        }
        ReplyEmpty(Connection, Message);
        return true;
    }
    if (dbus_message_is_method_call(Message, MenuInterface, "EventGroup"))
    {
        if (dbus_message_iter_get_arg_type(&Args) == DBUS_TYPE_ARRAY)
        {
            DBusMessageIter Events;
            dbus_message_iter_recurse(&Args, &Events);
            while (dbus_message_iter_get_arg_type(&Events) == DBUS_TYPE_STRUCT)
            {
                DBusMessageIter Event;
                dbus_message_iter_recurse(&Events, &Event);
                dbus_int32_t Id = 0;
                const char* EventId = nullptr;
                if (ReadBasic(&Event, DBUS_TYPE_INT32, &Id) && ReadBasic(&Event, DBUS_TYPE_STRING, &EventId))
                {
                    HandleMenuEvent(Id, EventId);
                }
                dbus_message_iter_next(&Events);
            }
        }
        DBusMessage* Reply = dbus_message_new_method_return(Message);
        DBusMessageIter It;
        DBusMessageIter NoErrors;
        dbus_message_iter_init_append(Reply, &It);
        dbus_message_iter_open_container(&It, DBUS_TYPE_ARRAY, "i", &NoErrors);
        dbus_message_iter_close_container(&It, &NoErrors);
        SendAndRelease(Connection, Reply);
        return true;
    }
    if (dbus_message_is_method_call(Message, MenuInterface, "AboutToShow"))
    {
        // The menu is opening: the GUI refreshes what the Priority submenu
        // shows (it doesn't talk to the daemon while hidden). Whatever
        // changes goes out as LayoutUpdated, which hosts apply to an open
        // menu too.
        PendingRequests.bMenuOpening = true;
        DBusMessage* Reply = dbus_message_new_method_return(Message);
        const dbus_bool_t NeedUpdate = FALSE;
        dbus_message_append_args(Reply, DBUS_TYPE_BOOLEAN, &NeedUpdate, DBUS_TYPE_INVALID);
        SendAndRelease(Connection, Reply);
        return true;
    }
    if (dbus_message_is_method_call(Message, MenuInterface, "AboutToShowGroup"))
    {
        PendingRequests.bMenuOpening = true;
        DBusMessage* Reply = dbus_message_new_method_return(Message);
        DBusMessageIter It;
        dbus_message_iter_init_append(Reply, &It);
        for (int i = 0; i < 2; ++i) // updatesNeeded, idErrors - both empty
        {
            DBusMessageIter Empty;
            dbus_message_iter_open_container(&It, DBUS_TYPE_ARRAY, "i", &Empty);
            dbus_message_iter_close_container(&It, &Empty);
        }
        SendAndRelease(Connection, Reply);
        return true;
    }
    return HandlePropertiesCall(Connection, Message, MenuInterface, MenuPropertyNames, &AppendMenuRootProperty);
}

void Tray::HandleMenuEvent(int ItemId, const char* EventId)
{
    if (std::strcmp(EventId, "clicked") != 0)
    {
        return;
    }
    if (ItemId == MenuShowHide)
    {
        // Not a toggle: the label already told the user which one it does,
        // and opening the menu has taken focus from the window, which would
        // fool the toggle's "is it up front?" check.
        (bWindowShown ? PendingRequests.bHideWindow : PendingRequests.bShowWindow) = true;
    }
    else if (ItemId == MenuQuit)
    {
        PendingRequests.bQuit = true;
    }
    else if (ItemId >= MenuPriorityFirstChoice && ItemId <= MenuPriorityLastChoice)
    {
        PendingRequests.PriorityChoice = static_cast<PlaybackPriority>(ItemId - MenuPriorityFirstChoice);
    }
}

} // namespace ramkolfx::gui
