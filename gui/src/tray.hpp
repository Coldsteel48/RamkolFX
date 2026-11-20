// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "ramkolfx/protocol.hpp"

struct DBusConnection;
struct DBusMessage;

namespace ramkolfx::gui
{

// The GUI's presence on the D-Bus session bus: a system tray icon, plus
// single-instance activation (both end in the same "show the window"
// request, so they share one connection).
//
// The tray icon speaks the freedesktop StatusNotifierItem protocol (with
// a com.canonical.dbusmenu context menu) directly over libdbus. That's
// what KDE Plasma, GNOME (with the AppIndicator extension), COSMIC, Xfce,
// waybar etc. all host. SDL2 has no tray API, and the usual helper
// library (libappindicator) would pull in GTK, which the GUI deliberately
// avoids - see gui/CMakeLists.txt.
//
// Everything degrades to a no-op without a session bus. IsIconShown()
// tells callers whether the icon is actually up, so they never hide the
// window somewhere the user can't get it back from.
//
// Apart from the constructor and ClaimSingleInstance() (startup, before
// any window exists), nothing here ever blocks on the bus: every call is sent asynchronously,
// its reply picked out of the message stream by serial, and every step
// that waits for a reply has a deadline - so a hung or overloaded bus or
// tray host can neither stall the render thread nor leave the icon state
// stuck in "in progress". A lost bus connection is re-established with a
// growing delay, re-claiming the single-instance name and the icon - also
// without blocking: authentication and the bus's Hello run like any other
// awaited step.
class Tray
{
public:
    Tray();
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // Claims the GUI's well-known bus name. When another GUI instance
    // already holds it, returns false - the caller should then exit - after
    // asking that instance to show its window if bActivateExisting (passing
    // on this process's XDG_ACTIVATION_TOKEN so the compositor lets it take
    // focus). A background start (--minimized) passes false, so it never
    // pops up a window the user already has. An instance that's too busy
    // to answer in time still counts as running: the request stays queued
    // for it, so it shows its window once it catches up. True otherwise -
    // including when there's no session bus at all, or the bus refuses the
    // name outright - and then this process owns the name (if there's a
    // bus) and keeps re-claiming it after a reconnect.
    bool ClaimSingleInstance(bool bActivateExisting);

    // Shows/removes the tray icon. Idempotent. While wanted, the icon is
    // re-registered automatically whenever a tray host (re)appears - e.g.
    // when started at login before the panel is up.
    void SetIconVisible(bool bVisible);

    // Keeps the context menu's "Show"/"Hide" entry in step with the window:
    // bShown means the window is up on screen - mapped *and* not minimized.
    // A minimized window gets "Show", which brings it back.
    void SetWindowShown(bool bShown);

    // The context menu's Priority submenu (docs/playback-priority-plan.md):
    // hidden unless bSupported (false only for a daemon known to predate
    // the setting), with Current's radio item checked - none while the GUI
    // hasn't heard from the daemon yet.
    void SetPlaybackPriority(bool bSupported, std::optional<PlaybackPriority> Current);

    // Whether the icon can be shown: a tray host (StatusNotifierWatcher) is
    // on the bus and hasn't kept rejecting - or ignoring - the icon. Stays
    // true while a registration is still in flight, or after a failure or
    // two that are being retried, so it doesn't flicker - this is for
    // telling the user the tray options won't work, not for deciding
    // whether to hide (that's IsIconShown). A host that never answers counts
    // as failing once a registration step's deadline passes, so this can't
    // stay true forever without the icon ever appearing.
    bool CanShowIcon() const { return bWatcherPresent && !bRegistrationFailed; }

    // Whether the icon is up right now: registered, and the watcher
    // acknowledged the registration. The only safe condition for hiding
    // the window - it goes false again if the tray host goes away.
    bool IsIconShown() const { return bWatcherPresent && bRegistered; }

    // Non-blocking: handles any pending bus traffic, and runs timed work
    // that's due (a registration retry or deadline, a reconnect). Call
    // every frame (or every wakeup while the window is hidden).
    //
    // bSocketReady false skips the socket I/O (messages libdbus already
    // queued are still handled): pass it only when GetPollFd() is known to
    // have turned neither readable nor - while HasPendingOutput() - writable
    // since the previous call, e.g. through an FdWakeup armed after it that
    // hasn't fired. Saves a syscall or two per frame on a quiet bus.
    void Dispatch(bool bSocketReady = true);

    // How long an idle caller may sleep before Dispatch() has timed work
    // to do, or -1 if there's none - bus traffic is signalled through
    // GetPollFd() instead.
    int GetTimeoutMs() const;

    // The bus connection's socket (-1 without a connection): readable
    // whenever Dispatch() has something to do, so an idle caller can sleep
    // on it instead of polling. Changes to -1 if the connection is lost,
    // and to a new descriptor once it has been re-established.
    int GetPollFd() const;

    // Whether libdbus still holds outgoing data it couldn't write yet (the
    // socket was full). An idle caller then also waits for GetPollFd() to
    // turn writable, so the next Dispatch() can send the rest.
    bool HasPendingOutput() const;

    // What the user asked for via the icon, its menu, or a second
    // instance since the last call - cleared on read.
    struct Requests
    {
        bool bToggleWindow = false; // icon clicked
        bool bShowWindow = false;   // menu "Show", or a second instance was launched
        bool bHideWindow = false;   // menu "Hide"
        bool bQuit = false;         // menu Quit
        // A Priority submenu choice (applied by the GUI, not a window
        // request: it neither carries nor uses an activation token).
        std::optional<PlaybackPriority> PriorityChoice;
        // The menu is about to open: a chance to refresh what it shows.
        bool bMenuOpening = false;
        // The xdg-activation token handed over by the tray host or a second
        // instance for this request (empty if none). Wayland compositors
        // only let a window grab focus with one of these. Single-use, so it
        // is handed out with the first request after it arrived and then
        // dropped, whether that request ends up showing the window or not -
        // and dropped unused if no request follows within a moment.
        std::string ActivationToken;

        bool Any() const { return bToggleWindow || bShowWindow || bHideWindow || bQuit; }
    };
    Requests TakeRequests();

private:
    using Clock = std::chrono::steady_clock;

    struct Filter; // libdbus message-filter trampoline (tray.cpp)

    // Opens the private session-bus connection and starts (without waiting
    // on anything) the setup that goes with it: watching for and querying
    // the tray watcher, and re-claiming the single-instance name if this
    // process held it. False, with nothing changed, if there's no bus.
    // Only bAtStartup may block (on authentication and the bus's Hello);
    // otherwise those run asynchronously, with HelloDeadline.
    bool Connect(bool bAtStartup);
    // The connection is gone: forgets everything tied to it and schedules
    // a reconnect.
    void DropConnection();
    // Sends Call without waiting and releases it; returns its serial, to
    // pick the reply out of the message stream (0 if it couldn't be sent).
    unsigned int SendCall(DBusMessage* Call);
    // Runs whatever timed work is due - see GetTimeoutMs().
    void RunTimedWork();

    // Returns whether Message was consumed (and replied to).
    bool HandleMessage(DBusMessage* Message);
    // Replies to this object's own calls; false if Message isn't one.
    bool HandleReply(DBusMessage* Message);
    bool HandleItemCall(DBusMessage* Message);
    bool HandleMenuCall(DBusMessage* Message);
    void HandleMenuEvent(int ItemId, const char* EventId);

    // A watcher appeared, went away, or was replaced: whatever it knew of
    // the icon is gone, so register afresh (or drop the registration).
    void OnWatcherChanged(bool bPresent);
    // Starts a registration: claims a fresh item bus name, then (once the
    // bus confirms it - see HandleRegistrationReply) registers it with the
    // watcher.
    void RegisterItem();
    void HandleRegistrationReply(DBusMessage* Message);
    // The registration attempt in flight failed (refused, or its deadline
    // passed): drops it and schedules the next attempt.
    void FailRegistration(const char* Reason);
    // Forgets earlier failures (and any pending retry) - for a fresh start
    // when the watcher changes or the icon is switched off and on.
    void ResetRegistrationFailures();
    // Drops the icon: abandons any attempt in flight and releases the item
    // bus name (the watcher then drops the item).
    void UnregisterItem();
    void EmitLayoutUpdated();
    // Keeps Token for the next request (see TakeRequests); ignores an
    // empty one.
    void StoreActivationToken(const char* Token);

    DBusConnection* Connection = nullptr;

    // This process owns (or, after a reconnect, should re-claim) the
    // single-instance bus name - see ClaimSingleInstance().
    bool bOwnsAppName = false;
    // Serial of an in-flight async re-claim of it (0 = none).
    unsigned int AppNameSerial = 0;

    // Lost connections are re-established after this long, doubling on
    // each failed attempt - and on each connection that drops again soon
    // after it was made, so a bus that keeps accepting and then dropping
    // this one backs off too.
    bool bReconnectScheduled = false;
    Clock::time_point ReconnectAt;
    std::chrono::milliseconds ReconnectDelay{0};
    // When the current connection was made.
    Clock::time_point ConnectedAt;
    // A reconnect's Hello in flight (0 = none, or the connection was made
    // at startup): its reply - and before it, authentication - must arrive
    // by HelloDeadline, or the connection is dropped and retried later.
    unsigned int HelloSerial = 0;
    bool bHelloFailed = false;
    Clock::time_point HelloDeadline;

    bool bWatcherPresent = false;
    // Serial of the in-flight "is a watcher there?" query (0 = none).
    unsigned int WatcherQuerySerial = 0;
    bool bIconWanted = false;

    // The registration in flight: claiming the item's bus name, then
    // registering it with the watcher. Each step's reply is picked out by
    // RegistrationSerial, and must arrive by RegistrationDeadline - a host
    // that never answers is a failure like one that refuses.
    enum class RegistrationStep
    {
        None,
        ClaimingName,
        Registering,
    };
    RegistrationStep Step = RegistrationStep::None;
    unsigned int RegistrationSerial = 0;
    Clock::time_point RegistrationDeadline;
    // Set once the watcher has acknowledged RegisterStatusNotifierItem,
    // not merely when the call was sent.
    bool bRegistered = false;
    // Registration has failed (bus name refused, or the watcher replied
    // with an error or not at all) several times running - see
    // CanShowIcon(). Attempts keep being retried with a growing delay
    // regardless: a panel that's still starting up at login may reject
    // the first ones. Cleared by a successful registration.
    bool bRegistrationFailed = false;
    int ConsecutiveFailures = 0;
    bool bRetryScheduled = false;
    Clock::time_point RetryAt;

    // org.kde.StatusNotifierItem-<pid>-<n>; n bumps on every registration
    // so a host never confuses a fresh registration with a stale one.
    std::string ItemServiceName;
    int RegistrationCount = 0;

    bool bWindowShown = true;
    bool bPrioritySupported = true;
    std::optional<PlaybackPriority> CurrentPriority;
    unsigned int MenuRevision = 1;

    Requests PendingRequests;
    // The latest xdg-activation token, until a request picks it up or it
    // expires - see TakeRequests().
    std::string ActivationToken;
    Clock::time_point ActivationTokenReceivedAt;
};

} // namespace ramkolfx::gui
