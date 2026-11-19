// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <array>
#include <optional>
#include <vector>

#include "ramkolfx/protocol.hpp"
#include "control_client.hpp"
#include "gui_prefs.hpp"

namespace ramkolfx::gui
{

// Owns the control connection and draws the whole UI. Tick() is called
// once per frame between ImGui::NewFrame() and ImGui::Render().
class App
{
public:
    // InTargetContentWidth is how wide (in already-DPI-scaled screen
    // pixels) both tabs' content should target - see the comment at its
    // computation in main.cpp for why it has to come from the fixed OS
    // window size rather than be measured live via ImGui.
    // Why minimize-to-tray can't work in this session, if it can't - the
    // Settings popup then disables that toggle and says why. main.cpp
    // works it out once and applies the same value to its own startup
    // decision.
    enum class MinimizeToTrayBlocker
    {
        None,
        // Not an X11 session: on native Wayland, xdg-shell never tells a
        // client it was minimized (SDL only sees a focus loss).
        NotX11,
        // Built without Xlib's headers (see x11_window.hpp), so a minimized
        // window can't be taken off the X11 taskbar.
        BuiltWithoutX11,
    };
    App(float InDpiScale, float InTargetContentWidth, const GuiPreferences& InPreferences,
        MinimizeToTrayBlocker InMinimizeToTrayBlocker);

    // Daemon reconnects and status/device polling, then drawing the UI.
    // Only called while the window is drawn: hidden in the tray or
    // minimized, the GUI doesn't talk to the daemon at all.
    void Tick(float DeltaTimeSeconds);

    // The window just went to the tray: stops anything audible that only
    // the (now unreachable) UI could turn off - the test noise.
    void OnWindowHidden();

    // The window is about to be drawn again after a spell hidden or
    // minimized, during which nothing was polled: makes the next Tick()
    // refresh everything at once, instead of showing what was current
    // when the window went away until each poll interval runs out.
    void OnWindowShown();

    // Window-to-tray behavior chosen in the Settings popup - main.cpp's
    // event loop reads these to decide what close/minimize do, and to keep
    // the tray icon up while either is in effect. Minimize-to-tray only
    // counts where minimizing can be detected (see the constructor):
    // the toggle is kept for X11 sessions, but means nothing on Wayland.
    // The rule itself lives in GuiPreferences, shared with main.cpp.
    bool ShouldMinimizeToTrayOnClose() const { return Preferences.bMinimizeToTrayOnClose; }
    bool ShouldMinimizeToTrayOnMinimize() const;
    // Either of the above in effect: the tray icon should be up.
    bool WantsTrayIcon() const;

    // Fed each frame from Tray::CanShowIcon(), so the Settings popup can
    // warn when the tray options currently have nothing to minimize to.
    void SetTrayAvailable(bool bAvailable) { bTrayAvailable = bAvailable; }

private:
    void DrawUI();
    void DrawSettingsPopup();
    void DrawSpatialTab();
    void DrawEqTab();

    // Applies a control request's result to LastStatus and returns true,
    // or - on failure - marks the connection dropped (bConnected = false,
    // ReconnectTimerSeconds = 0 so Tick() retries immediately) and returns
    // false. Every control request in this file (set spatial mode, speaker
    // azimuth/distance/mute, output device, HRTF file, near-field, test
    // noise, HW EQ band, bass enhancer, ...) follows this exact
    // success-updates/failure-disconnects shape, so they all funnel through
    // here instead of repeating it at each call site.
    bool Apply(std::optional<Status> Result);

    // Writes every local-only GUI preference field to gui_prefs.hpp in one
    // call - used everywhere a preference-backed toggle changes, so no
    // call site can accidentally reset a sibling field to its default the
    // way constructing a partial GuiPreferences{} inline used to risk.
    void PersistGuiPreferences() const;

    float DpiScale = 1.0f;

    // Fixed target width (already DPI-scaled) both DrawSpatialTab() and
    // DrawEqTab() lay their content out to, so the two tabs read as the
    // same size - see the constructor comment and main.cpp's computation.
    float TargetContentWidth = 0.0f;

    // Local-only UX state, never sent to the daemon - mirror mode, advanced
    // EQ/bass editing, the active tab, the tray options (see
    // GuiPreferences). The Settings popup and toggles edit it in place and
    // PersistGuiPreferences() saves it.
    GuiPreferences Preferences;

    ControlClient Client;
    Status LastStatus;
    std::vector<AudioDeviceInfo> Devices;
    std::vector<std::string> HrtfCatalog;
    bool bConnected = false;

    float ReconnectTimerSeconds = 0.0f;
    float StatusPollTimerSeconds = 0.0f;
    float DevicePollTimerSeconds = 0.0f;
    float PositionSendTimerSeconds = 0.0f; // throttles azimuth+distance sends together while dragging
    // True when a locally-dragged azimuth/distance change hasn't been sent
    // to the daemon yet (throttled out by PositionSendTimerSeconds). Flushed
    // as soon as the drag ends so the next status poll can't overwrite the
    // handle with a stale server value - see the drag-release send in
    // DrawUI().
    bool bPositionSendPending = false;
    int PendingPositionIndex = -1;
    int PendingMirroredIndex = -1;

    // Local cache of the current 10-band HW EQ curve - fetched once on
    // connect/reconnect via RequestHwEqState() and kept in sync
    // optimistically as the user drags sliders (Status deliberately
    // doesn't carry EQ state - see protocol.hpp's HwEqStateResponse).
    // Default-constructed EqBands (all at EqBand's own 1kHz/0dB default)
    // until the first successful RequestHwEqState() overwrites them - the
    // panel is disabled (see ImGui::BeginDisabled(!bConnected) in DrawUI)
    // until then anyway, so the placeholder is never actually shown.
    std::array<EqBand, MaxEqBands> HwEqBands{};

    // Local cache of the current bass enhancer settings - fetched once on
    // connect/reconnect via RequestBassEnhancerState() and kept in sync
    // optimistically as the user adjusts controls, same pattern as
    // HwEqBands.
    BassEnhancerSettings BassEnhancer{};

    // EQ preset catalog (built-ins first), fetched on connect and re-fetched
    // whenever Status::PresetCatalogRevision changes - i.e. after a save/
    // delete by this or any other client. LastStatus carries which one is
    // active.
    std::vector<EqPresetInfo> EqPresets;

    // Re-fetches EqPresets. On failure marks the connection dropped and
    // returns false.
    bool RefreshPresetCatalog();

    // Text buffer behind the "Save EQ preset" popup's name field.
    char SavePresetNameBuffer[MaxEqPresetNameLength + 1] = {};

    // Re-fetches HwEqBands/BassEnhancer (the curve isn't part of Status).
    // Called when Status::EqRevision changes, i.e. the daemon changed the
    // curve by some route other than this GUI's own sliders - applying a
    // preset, or device/app auto-switching. On failure marks the
    // connection dropped (same as Apply) and returns false.
    bool RefreshEqState();

    // Marks the daemon connection lost and schedules an immediate
    // reconnect attempt - what every failed request does.
    void MarkDisconnected();

    // Name of the preset the open "Delete EQ preset" popup is asking
    // about. Captured when the popup opens: the active preset can change
    // under it (auto-switching), and the confirmation must delete the one
    // the user was shown.
    std::string PresetPendingDelete;

    // Draws the preset card's combo / Save as / Delete controls and their
    // popups.
    void DrawPresetControls();

    // While the current output device is bound (the "This device" toggle
    // is on), moves its binding to PresetName - so picking or saving a
    // preset with the toggle on means "use this one for this device".
    // Skipped when the pick was the playing app's own bound preset (the
    // daemon then reports PresetSource App), so maintaining an app's
    // preset never rewrites the device binding.
    void RebindDeviceIfBound(const std::string& PresetName);

    // Playback streams routed into RamkolFX (with each app's bound
    // preset), for the App preset card. Polled alongside status while the
    // Equalizer tab is showing, so its playing/paused markers stay live.
    std::vector<ContentStreamInfo> ContentStreams;

    // App picked in the App preset card's app combo (persists across
    // frames; re-picked automatically when that app goes away).
    std::string SelectedApp;

    // On failure marks the connection dropped, same as Apply.
    void RefreshContentStreams();
    void DrawAppPresetCard();

    // Mirrors the XDG autostart entry (see autostart.hpp), which stays the
    // source of truth - re-read whenever the Settings popup opens.
    bool bAutostartEnabled = false;
    bool bAutostartWriteFailed = false;

    bool bTrayAvailable = false;
    const MinimizeToTrayBlocker MinimizeBlocker;
    const bool bMinimizeToTraySupported;

    // Accumulated while LastStatus.bTestNoiseEnabled is true (reset to 0
    // otherwise, so the animation phase always restarts cleanly); drives
    // the speaker dial's test-noise pulse animation. Purely cosmetic - see
    // DrawPositionDial's PulseTimeSeconds parameter.
    float TestNoisePulseTimeSeconds = 0.0f;
};

} // namespace ramkolfx::gui
