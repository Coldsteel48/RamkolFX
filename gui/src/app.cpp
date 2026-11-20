// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "autostart.hpp"
#include "imgui.h"
#include "position_dial.hpp"
#include "ramkolfx/version.hpp"
#include "ui_palette.hpp"
#include "ui_widgets.hpp"

namespace ramkolfx::gui
{

namespace
{
constexpr float ReconnectIntervalSeconds = 1.0f;
constexpr float StatusPollIntervalSeconds = 0.25f;
constexpr float DevicePollIntervalSeconds = 2.0f; // hotplug doesn't need to feel instant
constexpr float PositionSendIntervalSeconds = 0.03f; // ~33 Hz cap while dragging
constexpr float LevelPollIntervalSeconds = 1.0f / 30.0f; // smooth enough for the dial animation

// Channel-activity display mapping: peaks are shown on a dB scale from
// ActivityFloorDb (0) to full scale (1), rise instantly and fall by
// ActivityReleasePerSecond of that range per second - fast enough that
// beats read as separate pulses. The floor is low enough that quiet game
// audio still shows: a game menu peaking around -40 dBFS lights a speaker
// at a third of full (at a -48 dB floor it was 17%, easy to read as off).
constexpr float ActivityFloorDb = -60.0f;
constexpr float ActivityReleasePerSecond = 1.8f;

float PeakToActivity(float Peak)
{
    if (!(Peak > 0.0f)) // also catches NaN
    {
        return 0.0f;
    }
    const float Db = 20.0f * std::log10(Peak);
    return std::clamp((Db - ActivityFloorDb) / -ActivityFloorDb, 0.0f, 1.0f);
}

// Shared, unscaled (multiply by DpiScale at the call site) layout
// constants - kept in one place so the CardScope padding values used when
// constructing a card and the width math that right-aligns content within
// it (see DrawSpatialTab/DrawEqTab's TabContentWidth) can't drift apart.
constexpr float PanelPadding = 16.0f; // outer panel CardScope's own padding
// Inner padding of every nested card (Bass Enhancer, preset cards, genre
// row) - and the inset of the tab's free-standing rows (title, EQ label,
// band row) too, so text and controls line up on one left and one right
// edge across all of them, with card borders on the outer edge.
constexpr float NestedCardPadding = 14.0f;
constexpr float ColumnGap = 22.0f; // gap between the spatial tab's two columns
} // namespace

App::App(float InDpiScale, float InMinContentWidth, const GuiPreferences& InPreferences,
         MinimizeToTrayBlocker InMinimizeToTrayBlocker)
    : DpiScale(InDpiScale), MinContentWidth(InMinContentWidth), TargetContentWidth(InMinContentWidth),
      Preferences(InPreferences), MinimizeBlocker(InMinimizeToTrayBlocker),
      bMinimizeToTraySupported(InMinimizeToTrayBlocker == MinimizeToTrayBlocker::None)
{
}

bool App::Connect()
{
    if (!Client.Connect())
    {
        return false;
    }
    auto Result = Client.RequestStatus();
    if (!Result)
    {
        Client.Disconnect();
        return false;
    }
    LastStatus = *Result;
    bChannelLevelsUnsupported = false; // may be a newer daemon now
    bDetectedClassesUnsupported = false;
    bLevelPollPrimed = false;
    bConnected = true;
    // The rest only matters for drawing: fetched by Tick(), so a connect
    // made from the tray while the window is away stays one request.
    bFullRefreshPending = true;
    return true;
}

void App::FetchFullState()
{
    bFullRefreshPending = false;
    if (auto DeviceResult = Client.RequestDevices())
    {
        Devices = std::move(*DeviceResult);
    }
    if (auto HrtfResult = Client.RequestHrtfCatalog())
    {
        HrtfCatalog = std::move(*HrtfResult);
    }
    if (auto EqResult = Client.RequestHwEqState())
    {
        HwEqBands = *EqResult;
    }
    if (auto BassResult = Client.RequestBassEnhancerState())
    {
        BassEnhancer = *BassResult;
    }
    if (auto PresetResult = Client.RequestEqPresetCatalog())
    {
        EqPresets = std::move(*PresetResult);
    }
    DevicePollTimerSeconds = DevicePollIntervalSeconds;
}

bool App::IsPlaybackPrioritySupported() const
{
    // Unknown until connected: offered, so a click can connect.
    return !bConnected || LastStatus.bHasPlaybackPriority;
}

std::optional<PlaybackPriority> App::CurrentPlaybackPriority() const
{
    if (bConnected && LastStatus.bHasPlaybackPriority)
    {
        return LastStatus.Priority;
    }
    return std::nullopt;
}

bool App::RefreshStatusOnDemand()
{
    // Nothing is polled while the window is away, so the connection may
    // be one the daemon dropped (a restart) - and a window started hidden
    // has never connected at all. A failed request reconnects once.
    if (bConnected)
    {
        if (auto Result = Client.RequestStatus())
        {
            // Status only: whatever else changed is fetched once the
            // window draws again (Tick), not from the tray.
            LastStatus = std::move(*Result);
            bFullRefreshPending = true;
            return true;
        }
        MarkDisconnected();
    }
    return Connect();
}

void App::RefreshForTray()
{
    // Hosts send AboutToShow for the menu and again for each submenu: one
    // status request per opening is enough.
    const auto Now = std::chrono::steady_clock::now();
    if (Now - LastTrayRefresh < std::chrono::seconds(1))
    {
        return;
    }
    LastTrayRefresh = Now;
    RefreshStatusOnDemand();
}

void App::SetPlaybackPriority(PlaybackPriority Priority)
{
    // A daemon too old for the setting would refuse it.
    if ((!bConnected && !Connect()) || !LastStatus.bHasPlaybackPriority)
    {
        return;
    }
    auto Result = Client.SetPlaybackPriority(Priority);
    if (!Result)
    {
        // The daemon went away while the window was hidden (nothing
        // polled, so the socket is stale): reconnect and ask again.
        if (!RefreshStatusOnDemand() || !LastStatus.bHasPlaybackPriority)
        {
            return;
        }
        Result = Client.SetPlaybackPriority(Priority);
    }
    Apply(std::move(Result));
}

void App::Tick(float DeltaTimeSeconds)
{
    if (!bConnected)
    {
        ReconnectTimerSeconds -= DeltaTimeSeconds;
        if (ReconnectTimerSeconds <= 0.0f)
        {
            ReconnectTimerSeconds = ReconnectIntervalSeconds;
            if (Connect())
            {
                FetchFullState();
            }
        }
    }
    else
    {
        if (bFullRefreshPending)
        {
            FetchFullState(); // connected from the tray while the window was away
        }
        StatusPollTimerSeconds -= DeltaTimeSeconds;
        if (StatusPollTimerSeconds <= 0.0f)
        {
            StatusPollTimerSeconds = StatusPollIntervalSeconds;
            if (Apply(Client.RequestStatus()) && Preferences.ActiveTabIndex == 1)
            {
                RefreshContentStreams(); // only the Equalizer tab shows them
            }
        }

        DevicePollTimerSeconds -= DeltaTimeSeconds;
        if (bConnected && DevicePollTimerSeconds <= 0.0f)
        {
            DevicePollTimerSeconds = DevicePollIntervalSeconds;
            if (auto Result = Client.RequestDevices())
            {
                Devices = std::move(*Result);
            }
            else
            {
                MarkDisconnected();
            }
            if (bConnected)
            {
                if (auto HrtfResult = Client.RequestHrtfCatalog())
                {
                    HrtfCatalog = std::move(*HrtfResult);
                }
                else
                {
                    MarkDisconnected();
                }
            }
        }
    }

    // Drives the speaker dial's test-noise pulse animation (see
    // DrawPositionDial's PulseTimeSeconds parameter) - reset whenever
    // test noise isn't playing so the animation always restarts cleanly
    // from the same phase the next time it's turned on.
    if (LastStatus.bTestNoiseEnabled)
    {
        TestNoisePulseTimeSeconds += DeltaTimeSeconds;
    }
    else
    {
        TestNoisePulseTimeSeconds = 0.0f;
    }

    UpdateChannelActivity(DeltaTimeSeconds);

    DrawUI();
}

void App::UpdateChannelActivity(float DeltaTimeSeconds)
{
    const float Release = ActivityReleasePerSecond * DeltaTimeSeconds;
    for (float& Activity : SpeakerActivity)
    {
        Activity = std::max(0.0f, Activity - Release);
    }
    LfeActivity = std::max(0.0f, LfeActivity - Release);

    // Tick() only runs while the window is visible, so a hidden window
    // never polls either.
    const bool bPolling = bConnected && !bChannelLevelsUnsupported && Preferences.bShowChannelActivity &&
                          Preferences.ActiveTabIndex == 0;
    if (!bPolling)
    {
        LevelPollTimerSeconds = 0.0f;
        bLevelPollPrimed = false;
        return;
    }

    LevelPollTimerSeconds -= DeltaTimeSeconds;
    if (LevelPollTimerSeconds > 0.0f)
    {
        return;
    }
    LevelPollTimerSeconds = LevelPollIntervalSeconds;

    const std::optional<ChannelLevels> Levels = Client.RequestChannelLevels();
    if (!Levels)
    {
        if (Client.IsConnected())
        {
            bChannelLevelsUnsupported = true;
        }
        else
        {
            MarkDisconnected();
        }
        return;
    }
    if (!bLevelPollPrimed)
    {
        bLevelPollPrimed = true;
        return;
    }
    for (size_t i = 0; i < SpeakerCount; ++i)
    {
        SpeakerActivity[i] = std::max(SpeakerActivity[i], PeakToActivity(Levels->SpeakerPeak[i]));
    }
    LfeActivity = std::max(LfeActivity, PeakToActivity(Levels->LfePeak));
}

void App::OnWindowHidden()
{
    // Nothing on screen could turn it off any more - a test signal left
    // playing from the tray would just be noise with no visible source.
    if (bConnected && LastStatus.bTestNoiseEnabled)
    {
        Apply(Client.SetTestNoiseEnabled(false));
    }
}

void App::OnWindowShown()
{
    ReconnectTimerSeconds = 0.0f;
    StatusPollTimerSeconds = 0.0f;
    DevicePollTimerSeconds = 0.0f;
}

bool App::Apply(std::optional<Status> Result)
{
    if (Result)
    {
        const bool bEqChangedElsewhere = Result->EqRevision != LastStatus.EqRevision;
        const bool bCatalogChanged = Result->PresetCatalogRevision != LastStatus.PresetCatalogRevision;
        LastStatus = *Result;
        return (!bEqChangedElsewhere || RefreshEqState()) && (!bCatalogChanged || RefreshPresetCatalog());
    }
    MarkDisconnected();
    return false;
}

bool App::RefreshPresetCatalog()
{
    if (auto Result = Client.RequestEqPresetCatalog())
    {
        EqPresets = std::move(*Result);
        return true;
    }
    MarkDisconnected();
    return false;
}

void App::MarkDisconnected()
{
    bConnected = false;
    ReconnectTimerSeconds = 0.0f; // retry immediately
}

bool App::RefreshEqState()
{
    auto EqResult = Client.RequestHwEqState();
    auto BassResult = EqResult ? Client.RequestBassEnhancerState() : std::nullopt;
    if (!EqResult || !BassResult)
    {
        MarkDisconnected();
        return false;
    }
    HwEqBands = *EqResult;
    BassEnhancer = *BassResult;
    return true;
}

void App::PersistGuiPreferences() const
{
    SaveGuiPreferences(Preferences);
}

bool App::ShouldMinimizeToTrayOnMinimize() const
{
    return Preferences.MinimizesToTrayOnMinimize(bMinimizeToTraySupported);
}

bool App::WantsTrayIcon() const
{
    return Preferences.WantsTrayIcon(bMinimizeToTraySupported);
}

void App::DrawUI()
{
    // The UI *is* the OS window: one ImGui window pinned to the whole
    // viewport every frame, with no title bar, move, resize, collapse or
    // rounding - so there's no inner panel to drag off somewhere. Content
    // taller than the OS window scrolls; narrower than the design width
    // scrolls sideways rather than squashing the layout.
    {
        const ImGuiViewport* Viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(Viewport->WorkPos);
        ImGui::SetNextWindowSize(Viewport->WorkSize);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("RamkolFX Control", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar(2);

    // Lay out to the window's real width (minus any scrollbar), floored at
    // the width the UI was designed for. Measuring it live is safe now: the
    // ImGui window's size comes from the OS window, not from its content.
    TargetContentWidth = std::max(MinContentWidth, ImGui::GetContentRegionAvail().x);

    // Branding row: gradient logo mark + name/subtitle on the left,
    // connection status pill on the right.
    {
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        const float LogoSize = 36.0f * DpiScale;
        DrawLogoMark(DrawList, ImGui::GetCursorScreenPos(), LogoSize);
        ImGui::Dummy(ImVec2(LogoSize, LogoSize));

        ImGui::SameLine(0.0f, 10.0f * DpiScale);
        ImGui::BeginGroup();
        ImGui::TextUnformatted("RamkolFX");
        ImGui::TextColored(palette::TextMutedV4, "Audio Hub  v%s", ramkolfx::Version);
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 24.0f * DpiScale);
        {
            // A real drawn dot rather than a Unicode bullet glyph - the
            // default ImGui font's glyph range doesn't include U+25CF, so
            // a text bullet renders as a missing-glyph box.
            const ImU32 DotColor = bConnected ? palette::Success : palette::Warning;
            const float DotRadius = 4.0f * DpiScale;
            const ImVec2 DotOrigin = ImGui::GetCursorScreenPos();
            const float LineHeight = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(DotOrigin.x + DotRadius, DotOrigin.y + LineHeight * 0.5f), DotRadius, DotColor);
            ImGui::Dummy(ImVec2(DotRadius * 2.0f + 6.0f * DpiScale, LineHeight));
        }
        ImGui::SameLine(0.0f, 0.0f);
        if (bConnected)
        {
            ImGui::TextColored(palette::SuccessV4, "Connected");
        }
        else
        {
            ImGui::TextColored(palette::WarningV4, "Daemon not running - retrying...");
        }

        // Right edge lines up with the tab bar / panel below (both
        // TargetContentWidth wide from the window's content start, which
        // moves left as the window scrolls sideways).
        const char* SettingsLabel = "Settings";
        const float SettingsWidth = ImGui::CalcTextSize(SettingsLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        RightAlignCursor(ImGui::GetWindowPos().x - ImGui::GetScrollX() + ImGui::GetStyle().WindowPadding.x,
                         TargetContentWidth,
                         SettingsWidth);
        if (ImGui::Button(SettingsLabel))
        {
            bAutostartEnabled = IsAutostartEnabled(); // may have been changed outside the app
            bAutostartWriteFailed = false;
            ImGui::OpenPopup("Settings");
        }
        DrawSettingsPopup();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Tab nav is deliberately kept outside BeginDisabled(!bConnected) below
    // so it stays browsable while reconnecting, rather than locking the
    // whole panel to whichever tab happened to be active when the
    // connection dropped.
    // Explicit TargetContentWidth - exactly the width of the panel below
    // (its TabContentWidth plus PanelPadding on both sides) - so the tab
    // bar's edges line up with the panel's, rather than tracking the
    // window's live (possibly stale) content region.
    static const char* const TabLabels[] = {"Spatial Audio & Speakers", "Equalizer & Bass"};
    if (DrawSegmentedPill("tab_nav", TabLabels, IM_ARRAYSIZE(TabLabels), &Preferences.ActiveTabIndex, DpiScale,
                          TargetContentWidth))
    {
        PersistGuiPreferences();
    }

    ImGui::Spacing();

    {
        CardScope Panel(palette::PanelBg, palette::RadiusTopCard * DpiScale,
                        ImVec2(PanelPadding * DpiScale, PanelPadding * DpiScale));
        ImGui::BeginDisabled(!bConnected);
        if (Preferences.ActiveTabIndex == 0)
        {
            DrawSpatialTab();
        }
        else
        {
            DrawEqTab();
        }
        ImGui::EndDisabled();
    }

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::End();
}

void App::DrawSettingsPopup()
{
    if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    // One label + right-aligned toggle per row, all toggles on one edge.
    const float RowWidth = 380.0f * DpiScale;
    const auto ToggleRow = [&](const char* Id, const char* Label, bool* Value, const char* Tooltip) -> bool
    {
        const float RowStartX = ImGui::GetCursorScreenPos().x;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Label);
        ImGui::SameLine();
        RightAlignCursor(RowStartX, RowWidth, ToggleTrackWidth * DpiScale);
        const bool bChanged = DrawToggleSwitch(Id, Value, DpiScale);
        // AllowWhenDisabled: a greyed-out row is exactly when the user most
        // wants to know what it does.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", Tooltip);
        }
        return bChanged;
    };

    ImGui::TextColored(palette::TextMutedV4, "Startup");
    if (ToggleRow("autostart", "Start RamkolFX when I log in", &bAutostartEnabled,
                  "Launches RamkolFX in the background at login: hidden in the system tray if a tray "
                  "option below is on, otherwise minimized.\nThis controls the RamkolFX window only - the "
                  "audio daemon runs as its own systemd user service (ramkolfxd)."))
    {
        bAutostartWriteFailed = !SetAutostartEnabled(bAutostartEnabled);
        if (bAutostartWriteFailed)
        {
            bAutostartEnabled = !bAutostartEnabled; // reflect what's actually on disk
        }
    }
    if (bAutostartWriteFailed)
    {
        // The real path ($XDG_CONFIG_HOME may point anywhere), wrapped like
        // the tray warnings below since it can be long.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth);
        ImGui::TextColored(palette::WarningV4, "Couldn't update %s (see terminal output)", AutostartEntryPath().c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::TextColored(palette::TextMutedV4, "System tray");
    bool bTrayChanged = false;
    bTrayChanged |= ToggleRow("tray_on_close", "Minimize to tray when closed", &Preferences.bMinimizeToTrayOnClose,
                              "The window's close button hides RamkolFX to the system tray instead of "
                              "quitting.\nQuit from the tray icon's menu.");
    // Kept (and still saved) while disabled, so an X11 session picks it up.
    ImGui::BeginDisabled(!bMinimizeToTraySupported);
    bTrayChanged |= ToggleRow("tray_on_minimize", "Minimize to tray when minimized", &Preferences.bMinimizeToTrayOnMinimize,
                              "Minimizing hides RamkolFX to the system tray instead of the taskbar.");
    ImGui::EndDisabled();
    if (bTrayChanged)
    {
        PersistGuiPreferences();
    }

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth);
    if (MinimizeBlocker == MinimizeToTrayBlocker::NotX11)
    {
        ImGui::TextColored(palette::TextMutedV4,
                           "Wayland doesn't tell apps when they're minimized, so minimize-to-tray only works in "
                           "X11 sessions. Use the close button instead, or click the tray icon to hide the window.");
    }
    else if (MinimizeBlocker == MinimizeToTrayBlocker::BuiltWithoutX11)
    {
        ImGui::TextColored(palette::TextMutedV4,
                           "This build of RamkolFX lacks X11 support (it was built without the libX11 headers), "
                           "so minimize-to-tray can't work. Use the close button instead, or click the tray icon "
                           "to hide the window.");
    }
    if (!bTrayAvailable && WantsTrayIcon())
    {
        ImGui::TextColored(palette::WarningV4,
                           "No usable system tray found - close and minimize behave normally until one is "
                           "available (on GNOME, install the AppIndicator extension).");
    }
    ImGui::PopTextWrapPos();

    // The same setting as the App preset card's combo and the tray menu's
    // Priority submenu - all three follow LastStatus.
    ImGui::Spacing();
    ImGui::TextColored(palette::TextMutedV4, "EQ auto-switch");
    {
        const float RowStartX = ImGui::GetCursorScreenPos().x;
        const float ComboWidth = PlaybackPriorityComboWidth();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("When a game and music play together");
        ImGui::SameLine();
        RightAlignCursor(RowStartX, RowWidth, ComboWidth);
        DrawPlaybackPriorityCombo("##settings_playback_priority", ComboWidth);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth);
    ImGui::TextColored(palette::TextMutedV4,
                       "Only one EQ curve plays at a time. Only apps with a binding take part. Also in the "
                       "tray menu, to change in the middle of a game.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Spacing();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::DrawSpatialTab()
{
    // Sends one mute/unmute command and folds the response into LastStatus;
    // on failure, flags disconnected the same way every other request
    // below does and reports the failure to the caller so a multi-command
    // sequence (Solo, Unmute all) stops rather than spamming a dead
    // connection.
    auto SetSpeakerMuted = [&](uint8 SpeakerIndex, bool bMuted) -> bool
    {
        return Apply(Client.SetSpeakerMute(SpeakerIndex, bMuted));
    };

    // Sends one speaker's given (already-captured) azimuth and distance and
    // folds the response into LastStatus, the same fail-and-disconnect
    // pattern as SetSpeakerMuted above. Takes the values as parameters
    // rather than reading them from LastStatus itself: the azimuth
    // round-trip's LastStatus assignment overwrites every field, including
    // other speakers', with whatever the daemon had *before* this call, so
    // a caller sending more than one speaker's position (e.g. a mirrored
    // pair) must capture both speakers' target values up front rather than
    // reading them fresh between calls.
    auto SendSpeakerPositionValues = [&](uint8 SpeakerIndex, float AzimuthDegrees, float DistanceMeters)
    {
        if (Apply(Client.SetSpeakerAzimuth(SpeakerIndex, AzimuthDegrees)))
        {
            Apply(Client.SetSpeakerDistance(SpeakerIndex, DistanceMeters));
        }
    };

    // Sends PrimaryIndex's current (locally-dragged) position, and
    // MirrorIndex's too if it's >= 0 (mirror mode dragged a counterpart
    // alongside it). Both speakers' target values are captured before
    // either round-trip starts, for the reason described above.
    auto SendSpeakerPosition = [&](int PrimaryIndex, int MirrorIndex)
    {
        const float PrimaryAzimuth = LastStatus.SpeakerAzimuthDegrees[static_cast<size_t>(PrimaryIndex)];
        const float PrimaryDistance = LastStatus.SpeakerDistanceMeters[static_cast<size_t>(PrimaryIndex)];
        float MirrorAzimuth = 0.0f;
        float MirrorDistance = 0.0f;
        if (MirrorIndex >= 0)
        {
            MirrorAzimuth = LastStatus.SpeakerAzimuthDegrees[static_cast<size_t>(MirrorIndex)];
            MirrorDistance = LastStatus.SpeakerDistanceMeters[static_cast<size_t>(MirrorIndex)];
        }
        SendSpeakerPositionValues(static_cast<uint8>(PrimaryIndex), PrimaryAzimuth, PrimaryDistance);
        if (MirrorIndex >= 0 && bConnected)
        {
            SendSpeakerPositionValues(static_cast<uint8>(MirrorIndex), MirrorAzimuth, MirrorDistance);
        }
    };

    // Two side-by-side columns. Widths must be explicit (not "available
    // space") because DrawSegmentedPill's spatial_mode pill below fills
    // whatever width it's given - inside a plain BeginGroup() (no child
    // window constraining it), "available space" reflects the whole
    // window, not an informal column, which would stretch the pill (and
    // therefore this group's own measured bounding box) to the full
    // window width and push the right column off its right edge.
    //
    // TabContentWidth is derived from App::TargetContentWidth - the same
    // per-frame value DrawEqTab() uses, measured once in DrawUI() before
    // either tab draws - so both tabs always lay out to the same width,
    // whatever their own natural content width. The right column gets a fixed
    // size (the radar card); the left column gets whatever's left,
    // floored at MinLeftColumnWidth as a defensive minimum.
    const float TabContentWidth = TargetContentWidth - 2.0f * PanelPadding * DpiScale;
    const float RadarPadding = 14.0f * DpiScale;
    const float RadarCardSize = PositionDialSize * DpiScale + RadarPadding * 2.0f;
    constexpr float MinLeftColumnWidth = 260.0f; // safety floor - today's old fixed value
    const float LeftColumnWidth = std::max(MinLeftColumnWidth * DpiScale,
                                           TabContentWidth - ColumnGap * DpiScale - RadarCardSize);
    const ImVec2 RowStart = ImGui::GetCursorScreenPos();

    ImGui::BeginGroup();
    {
        ImGui::TextUnformatted("Spatial Audio");
        ImGui::Spacing();

        static const char* const ModeLabels[] = {"Off", "Basic", "Advanced"};
        int ModeIndex = static_cast<int>(LastStatus.Mode);
        if (DrawSegmentedPill("spatial_mode", ModeLabels, IM_ARRAYSIZE(ModeLabels), &ModeIndex, DpiScale,
                              LeftColumnWidth))
        {
            Apply(Client.SetSpatialMode(static_cast<SpatialMode>(ModeIndex)));
        }

        ImGui::TextColored(palette::TextMutedV4, "What does this mean?");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Off - plain stereo, no processing.\nBasic - lightweight virtual surround, "
                               "low CPU.\nAdvanced - HRTF binaural render, most accurate.\n\n"
                              "Remembered per output device: switching devices restores\n"
                              "the mode you last used with that device.");
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Output device");

        std::string PreviewLabel = LastStatus.OutputDeviceName.empty() ? "(none)" : LastStatus.OutputDeviceName;
        for (const auto& Device : Devices)
        {
            if (Device.Name == LastStatus.OutputDeviceName)
            {
                PreviewLabel = Device.Description;
                break;
            }
        }

        ImGui::SetNextItemWidth(LeftColumnWidth);
        if (ImGui::BeginCombo("##output_device", PreviewLabel.c_str()))
        {
            for (const auto& Device : Devices)
            {
                const bool bSelected = Device.Name == LastStatus.OutputDeviceName;
                if (ImGui::Selectable(Device.Description.c_str(), bSelected))
                {
                    Apply(Client.SetOutputDevice(Device.Name));
                }
                if (bSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (LastStatus.Mode == SpatialMode::Advanced)
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("HRTF profile");

            const int CurrentHrtfIndex = static_cast<int>(LastStatus.ActiveHrtfIndex);
            const char* CurrentHrtfLabel =
                (CurrentHrtfIndex >= 0 && CurrentHrtfIndex < static_cast<int>(HrtfCatalog.size()))
                    ? HrtfCatalog[static_cast<size_t>(CurrentHrtfIndex)].c_str()
                    : "(none)";

            ImGui::SetNextItemWidth(LeftColumnWidth);
            if (ImGui::BeginCombo("##hrtf_source", CurrentHrtfLabel))
            {
                for (int i = 0; i < static_cast<int>(HrtfCatalog.size()); ++i)
                {
                    const bool bSelected = i == CurrentHrtfIndex;
                    if (ImGui::Selectable(HrtfCatalog[static_cast<size_t>(i)].c_str(), bSelected))
                    {
                        Apply(Client.SetHrtfFile(static_cast<uint8>(i)));
                    }
                    if (bSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Near-field distance");
            ImGui::SameLine();
            RightAlignCursor(RowStart.x, LeftColumnWidth, ToggleTrackWidth * DpiScale);
            bool bNearFieldEnabled = LastStatus.bNearFieldEnabled;
            if (DrawToggleSwitch("near_field", &bNearFieldEnabled, DpiScale))
            {
                Apply(Client.SetNearFieldEnabled(bNearFieldEnabled));
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Off: speaker distance (drag radially on the dial) has no effect - today's "
                    "behavior.\nOn: closer speakers get louder in every mode, and Advanced (HRTF) mode "
                    "additionally gets a real interaural-level-difference proximity effect.");
            }
        }
    }
    ImGui::EndGroup();

    const ImVec2 RightColumnStart(RowStart.x + LeftColumnWidth + ColumnGap * DpiScale, RowStart.y);
    ImGui::SetCursorScreenPos(RightColumnStart);

    int MuteToggledIndex = -1;
    int SoloIndex = -1;

    ImGui::BeginGroup();
    {
        ImGui::TextUnformatted("Speaker Layout");
        // "Mirror" shares this row with the section title above, so the
        // label+toggle pair moves as a unit to the row's right edge -
        // unlike a row where the toggle is the only other thing on the
        // line (Near-field, Test noise, Bass Enhancer, Advanced tuning),
        // where only the toggle itself needs to move.
        const float MirrorGap = 6.0f * DpiScale;
        const float MirrorUnitWidth =
            ImGui::CalcTextSize("Mirror").x + MirrorGap + ToggleTrackWidth * DpiScale;
        ImGui::SameLine();
        RightAlignCursor(RightColumnStart.x, RadarCardSize, MirrorUnitWidth);
        ImGui::TextUnformatted("Mirror");
        ImGui::SameLine(0.0f, MirrorGap);
        if (DrawToggleSwitch("mirror", &Preferences.bMirrorModeEnabled, DpiScale))
        {
            PersistGuiPreferences();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("When on, dragging FL/FR, RL/RR, or SL/SR also drags its counterpart to the "
                               "mirrored angle at the same distance.\nOff: every speaker moves "
                               "independently. FC has no counterpart either way.");
        }

        ImGui::TextColored(palette::TextMutedV4, "Drag to set angle/distance.");
        ImGui::TextColored(palette::TextMutedV4, "Click to mute, Ctrl+click to solo.");

        // Dark card behind the radar canvas, matching the reference
        // design's nested panel. DrawPositionDial only draws the canvas
        // itself, so the card background is drawn here around it -
        // RadarPadding/RadarCardSize are computed once at the top of this
        // function (LeftColumnWidth depends on RadarCardSize too).
        const ImVec2 RadarCardMin = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            RadarCardMin, ImVec2(RadarCardMin.x + RadarCardSize, RadarCardMin.y + RadarCardSize),
            palette::NestedCardBg, palette::RadiusNestedCard * DpiScale);
        ImGui::SetCursorScreenPos(ImVec2(RadarCardMin.x + RadarPadding, RadarCardMin.y + RadarPadding));

        int ChangedIndex = -1;
        int MirroredIndex = -1;
        const bool bDialChanged = DrawPositionDial(
            "speaker_dial", LastStatus.SpeakerAzimuthDegrees, LastStatus.SpeakerDistanceMeters,
            LastStatus.SpeakerMuted, Preferences.bMirrorModeEnabled, &ChangedIndex, &MirroredIndex, &MuteToggledIndex,
            &SoloIndex, DpiScale, LastStatus.bTestNoiseEnabled, TestNoisePulseTimeSeconds,
            Preferences.bShowChannelActivity ? &SpeakerActivity : nullptr, LfeActivity);

        ImGui::SetCursorScreenPos(ImVec2(RadarCardMin.x, RadarCardMin.y + RadarCardSize));

        if (bDialChanged)
        {
            PendingPositionIndex = ChangedIndex;
            PendingMirroredIndex = MirroredIndex;
            PositionSendTimerSeconds -= ImGui::GetIO().DeltaTime;
            if (PositionSendTimerSeconds <= 0.0f)
            {
                PositionSendTimerSeconds = PositionSendIntervalSeconds;
                SendSpeakerPosition(ChangedIndex, MirroredIndex);
                bPositionSendPending = false;
            }
            else
            {
                bPositionSendPending = true;
            }
        }
        else
        {
            PositionSendTimerSeconds = 0.0f;
            // The drag just ended (or this is an unrelated frame). If the
            // last dragged position was throttled out and never sent,
            // flush it now - otherwise the handle sits at its final
            // dragged spot only until the next status poll, which still
            // has the last-sent (stale) value and snaps the handle back
            // to it.
            if (bPositionSendPending && PendingPositionIndex >= 0)
            {
                SendSpeakerPosition(PendingPositionIndex, PendingMirroredIndex);
                bPositionSendPending = false;
            }
            PendingPositionIndex = -1;
            PendingMirroredIndex = -1;
        }

        // Explicit per-speaker mute/solo chip row - a more discoverable
        // alternative to the dial's own click/Ctrl+click gesture. Feeds
        // the exact same MuteToggledIndex/SoloIndex the dial's out-params
        // feed, so the dispatch below doesn't need to know which of the
        // two triggered it. Display order (FC,FL,FR / SL,SR,RL / RR,
        // wrapping every 3) is purely cosmetic grouping - it indexes into
        // the same fixed FL,FR,FC,RL,RR,SL,SR data arrays DrawPositionDial
        // itself uses, which stays unchanged.
        static constexpr uint8 ChipDisplayOrder[SpeakerCount] = {2, 0, 1, 5, 6, 3, 4};
        ImGui::Spacing();
        for (uint8 Slot = 0; Slot < SpeakerCount; ++Slot)
        {
            const uint8 i = ChipDisplayOrder[Slot];
            ImGui::PushID(i);
            if (Slot > 0)
            {
                ImGui::SameLine();
                if (Slot % 3 == 0)
                {
                    ImGui::NewLine();
                }
            }

            CardScope Chip(palette::ChipBg, palette::RadiusSmall * DpiScale,
                           ImVec2(6.0f * DpiScale, 3.0f * DpiScale));
            ImGui::TextUnformatted(GetSpeakerLabel(i));
            ImGui::SameLine();

            const bool bChipMuted = LastStatus.SpeakerMuted[i];
            if (bChipMuted)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, palette::WarningV4);
            }
            if (ImGui::SmallButton("M"))
            {
                MuteToggledIndex = static_cast<int>(i);
            }
            if (bChipMuted)
            {
                ImGui::PopStyleColor();
            }

            ImGui::SameLine();
            // Unlike mute, solo has no lasting per-speaker flag in Status
            // to reflect here - it's a one-shot "mute everyone else"
            // action (see the *OutSoloIndex dispatch below), so the S
            // button has no persistent "currently soloed" look to show.
            if (ImGui::SmallButton("S"))
            {
                SoloIndex = static_cast<int>(i);
            }

            ImGui::PopID();
        }

        ImGui::Spacing();
        if (ImGui::Button("Reset"))
        {
            Apply(Client.ResetSpeakerPositions());
        }

        ImGui::SameLine();
        if (ImGui::Button("Unmute all"))
        {
            for (uint8 i = 0; i < SpeakerCount && bConnected; ++i)
            {
                if (!SetSpeakerMuted(i, false))
                {
                    break;
                }
            }
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Test noise");
        ImGui::SameLine();
        RightAlignCursor(RightColumnStart.x, RadarCardSize, ToggleTrackWidth * DpiScale);
        bool bTestNoiseEnabled = LastStatus.bTestNoiseEnabled;
        if (DrawToggleSwitch("test_noise", &bTestNoiseEnabled, DpiScale))
        {
            Apply(Client.SetTestNoiseEnabled(bTestNoiseEnabled));
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Channel activity");
        ImGui::SameLine();
        RightAlignCursor(RightColumnStart.x, RadarCardSize, ToggleTrackWidth * DpiScale);
        if (DrawToggleSwitch("channel_activity", &Preferences.bShowChannelActivity, DpiScale))
        {
            PersistGuiPreferences();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("When on, each speaker on the dial pulses with the live level of its channel, "
                               "so you can see which channels the playing audio actually uses.\nThe glow in "
                               "the center is the LFE (subwoofer) channel.");
        }
        if (Preferences.bShowChannelActivity && bChannelLevelsUnsupported)
        {
            ImGui::TextColored(palette::TextMutedV4, "Restart the RamkolFX daemon to enable this.");
        }
    }
    ImGui::EndGroup();

    if (MuteToggledIndex >= 0)
    {
        SetSpeakerMuted(static_cast<uint8>(MuteToggledIndex),
                         !LastStatus.SpeakerMuted[static_cast<size_t>(MuteToggledIndex)]);
    }

    if (SoloIndex >= 0)
    {
        // Mute every other speaker and unmute this one so it can be heard
        // in isolation; stop early if the connection drops.
        for (uint8 j = 0; j < SpeakerCount && bConnected; ++j)
        {
            if (!SetSpeakerMuted(j, j != static_cast<uint8>(SoloIndex)))
            {
                break;
            }
        }
    }
}

void App::DrawEqTab()
{
    // Same formula (and same App::TargetContentWidth) as DrawSpatialTab()
    // - see its comment.
    const float TabContentWidth = TargetContentWidth - 2.0f * PanelPadding * DpiScale;
    const ImVec2 TabStart = ImGui::GetCursorScreenPos();

    // The shared alignment edges (see NestedCardPadding): card borders sit
    // on TabStart.x / TabStart.x + TabContentWidth, all text and controls on
    // InnerX / InnerX + InnerWidth.
    const float CardPad = NestedCardPadding * DpiScale;
    const float InnerX = TabStart.x + CardPad;
    const float InnerWidth = TabContentWidth - 2.0f * CardPad;
    const auto MoveToInnerEdge = [&]
    {
        ImGui::SetCursorScreenPos(ImVec2(InnerX, ImGui::GetCursorScreenPos().y));
    };
    // ImGui child windows (the preset cards, genre row) take their padding
    // from the style at BeginChild time; this matches them to CardScope's.
    const auto BeginNestedChild = [&](const char* Id, ImVec2 Size)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, palette::NestedCardBgV4);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(CardPad, CardPad));
        ImGui::BeginChild(Id, Size, true, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(); // only needed at Begin - keeps popups opened inside on the default
    };
    const auto EndNestedChild = []
    {
        ImGui::EndChild();
        ImGui::PopStyleColor();
    };

    MoveToInnerEdge();
    ImGui::TextUnformatted("Equalizer & Bass");
    // "Advanced editing" shares this row with the tab title, so the
    // label+toggle pair moves as a unit to the row's right edge - see the
    // matching comment on DrawSpatialTab()'s "Mirror" row.
    const float AdvancedEditGap = 8.0f * DpiScale;
    const float AdvancedEditUnitWidth =
        ImGui::CalcTextSize("Advanced editing").x + AdvancedEditGap + ToggleTrackWidth * DpiScale;
    ImGui::SameLine();
    RightAlignCursor(InnerX, InnerWidth, AdvancedEditUnitWidth);
    ImGui::TextUnformatted("Advanced editing");
    ImGui::SameLine(0.0f, AdvancedEditGap);
    if (DrawToggleSwitch("advanced_eq", &Preferences.bAdvancedEqMode, DpiScale))
    {
        PersistGuiPreferences();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Off: gain-only bars at fixed ISO center frequencies.\nOn: also edit each "
                           "band's filter type, frequency, and Q - the wire format supports this either "
                           "way, this only changes what the panel shows.");
    }

    ImGui::Spacing();

    // Sends the current (already locally-updated) BassEnhancer settings
    // and folds the response into LastStatus, same fail-and-disconnect
    // pattern as every other control here.
    auto SendBassEnhancer = [&]()
    {
        Apply(Client.SetBassEnhancer(BassEnhancer));
    };

    {
        CardScope Card(palette::NestedCardBg, palette::RadiusNestedCard * DpiScale,
                       ImVec2(CardPad, CardPad));
        const ImVec2 BassCardStart = ImGui::GetCursorScreenPos();
        const float BassContentWidth = InnerWidth;

        ImGui::TextUnformatted("Bass Enhancer");
        ImGui::SameLine();
        RightAlignCursor(BassCardStart.x, BassContentWidth, ToggleTrackWidth * DpiScale);
        if (DrawToggleSwitch("bass_enabled", &BassEnhancer.bEnabled, DpiScale))
        {
            SendBassEnhancer();
        }
        ImGui::TextColored(palette::TextMutedV4, "Boosts low end for headphones");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Psychoacoustic bass enhancement for headphones: synthesizes harmonics of "
                               "the sub-bass so small drivers that can't reproduce it directly still make "
                               "it audible.");
        }

        if (BassEnhancer.bEnabled)
        {
            ImGui::SetNextItemWidth(180.0f * DpiScale);
            float AmountPercent = BassEnhancer.Mix * 100.0f;
            if (ImGui::SliderFloat("Amount##bass", &AmountPercent, 0.0f, 100.0f, "%.0f%%"))
            {
                BassEnhancer.Mix = AmountPercent / 100.0f;
                SendBassEnhancer();
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Advanced tuning");
            ImGui::SameLine();
            RightAlignCursor(BassCardStart.x, BassContentWidth, ToggleTrackWidth * DpiScale);
            if (DrawToggleSwitch("bass_advanced", &Preferences.bAdvancedBassMode, DpiScale))
            {
                PersistGuiPreferences();
            }

            if (Preferences.bAdvancedBassMode)
            {
                ImGui::SetNextItemWidth(160.0f * DpiScale);
                float Cutoff = BassEnhancer.CutoffHz;
                if (ImGui::DragFloat("Cutoff Hz##bass", &Cutoff, 1.0f, 40.0f, 250.0f, "%.0f Hz"))
                {
                    BassEnhancer.CutoffHz = Cutoff;
                    SendBassEnhancer();
                }

                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f * DpiScale);
                float Drive = BassEnhancer.Drive;
                if (ImGui::DragFloat("Drive##bass", &Drive, 0.01f, 0.0f, 1.0f, "%.2f"))
                {
                    BassEnhancer.Drive = Drive;
                    SendBassEnhancer();
                }
            }
        }
    }

    // Named EQ presets (docs/eq-presets-plan.md): the left card manages
    // presets and the current output device's binding, the right card
    // binds presets to apps for auto-switching.
    ImGui::Spacing();
    {
        // Derived from TabContentWidth, not a live GetContentRegionAvail()
        // read, so these cards stay in step with the Bass Enhancer card and
        // EQ bars below, which both hug TabContentWidth exactly.
        const float CardWidth = (TabContentWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const float CardHeight = 138.0f * DpiScale;

        BeginNestedChild("device_preset_card", ImVec2(CardWidth, CardHeight));
        {
            // A real BeginChild() (unlike the plain BeginGroup() "columns"
            // used elsewhere in this file), so GetContentRegionAvail()
            // here correctly reflects this card's own constrained width.
            const ImVec2 RowStart = ImGui::GetCursorScreenPos();
            const float RowWidth = ImGui::GetContentRegionAvail().x;
            const float Gap = 6.0f * DpiScale;
            const float UnitWidth = ImGui::CalcTextSize("This device").x + Gap + ToggleTrackWidth * DpiScale;
            ImGui::TextUnformatted("EQ preset");
            ImGui::SameLine();
            RightAlignCursor(RowStart.x, RowWidth, UnitWidth);
            ImGui::TextUnformatted("This device");
            ImGui::SameLine(0.0f, Gap);
        }
        // On = the current output device is bound to a preset, applied
        // automatically whenever this device is selected (phase 2 of
        // docs/eq-presets-plan.md). Turning it on binds the active preset,
        // so it needs one; while on, picking or saving a preset rebinds.
        bool bDeviceBound = !LastStatus.DevicePresetName.empty();
        ImGui::BeginDisabled(LastStatus.OutputDeviceName.empty() ||
                             (!bDeviceBound && LastStatus.ActivePresetName.empty()));
        if (DrawToggleSwitch("device_preset_binding", &bDeviceBound, DpiScale))
        {
            Apply(Client.BindDevicePreset(LastStatus.OutputDeviceName,
                                          bDeviceBound ? LastStatus.ActivePresetName : std::string()));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(bDeviceBound ? "Uses \"%s\" whenever this output device is selected"
                                           : "Use the current preset whenever this output device is selected",
                              LastStatus.DevicePresetName.c_str());
        }
        {
            std::string DeviceLabel = LastStatus.OutputDeviceName.empty() ? "(none)" : LastStatus.OutputDeviceName;
            for (const AudioDeviceInfo& Device : Devices)
            {
                if (Device.Name == LastStatus.OutputDeviceName)
                {
                    DeviceLabel = Device.Description;
                }
            }
            const ImVec2 RowStart = ImGui::GetCursorScreenPos();
            const float RowWidth = ImGui::GetContentRegionAvail().x;
            ImGui::TextColored(palette::TextMutedV4, "Device: %s", DeviceLabel.c_str());

            // Auto-detect for this device: on binds the device to it
            // (replacing a preset binding), off unbinds.
            const float Gap = 6.0f * DpiScale;
            const char* AutoLabel = "Auto-detect";
            ImGui::SameLine();
            RightAlignCursor(RowStart.x, RowWidth,
                             ImGui::CalcTextSize(AutoLabel).x + Gap + ToggleTrackWidth * DpiScale);
            const bool bUnavailable = !IsAutoDetectUsable();
            ImGui::TextColored(LastStatus.bDeviceAutoDetect && bUnavailable ? palette::WarningV4
                                                                            : palette::TextMutedV4,
                               "%s", AutoLabel);
            ImGui::SameLine(0.0f, Gap);
            bool bDeviceAuto = LastStatus.bDeviceAutoDetect;
            ImGui::BeginDisabled(LastStatus.OutputDeviceName.empty() || (bUnavailable && !bDeviceAuto));
            if (DrawToggleSwitch("device_auto_detect", &bDeviceAuto, DpiScale))
            {
                Apply(bDeviceAuto ? Client.BindDeviceAutoDetect(LastStatus.OutputDeviceName)
                                  : Client.BindDevicePreset(LastStatus.OutputDeviceName, std::string()));
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (bUnavailable)
                {
                    ImGui::SetTooltip("%s", AutoDetectUnavailableReason());
                }
                else
                {
                    ImGui::SetTooltip("Pick a preset for whatever plays on this device (speech, music genre, "
                                      "film) - see Auto-detect below");
                }
            }
        }
        DrawPresetControls();
        EndNestedChild();

        ImGui::SameLine();

        BeginNestedChild("app_preset_card", ImVec2(CardWidth, CardHeight));
        DrawAppPresetCard();
        EndNestedChild();
    }

    ImGui::Spacing();
    {
        // Explicit TabContentWidth rather than 0.0f ("fill available") for
        // the same reason as CardWidth above - fill-available reads the
        // window's live (possibly stale) content region instead of the
        // fixed per-tab width every other row in this tab is flush to.
        // Height comes from AutoDetectCardHeight: one row collapsed, the
        // device row and class table expanded.
        BeginNestedChild("auto_detect_card", ImVec2(TabContentWidth, AutoDetectCardHeight(CardPad)));
        DrawAutoDetectCard();
        EndNestedChild();
    }

    ImGui::Spacing();
    MoveToInnerEdge();
    ImGui::TextUnformatted("Equalizer (10-band)");

    // Sends BandIndex's current (already locally-updated) EqBand and folds
    // the response into LastStatus, same fail-and-disconnect pattern as
    // every other control above.
    auto SendHwEqBand = [&](uint8 BandIndex)
    {
        Apply(Client.SetHwEqBand(BandIndex, HwEqBands[BandIndex]));
    };

    static const char* EqFilterTypeLabels[] = {"Peaking", "Low Shelf", "High Shelf",
                                                "Low Pass", "High Pass", "Notch"};

    // Band layout: every band is a fixed-width column (the width of the
    // advanced-mode controls), with the bar and frequency label centered in
    // it, and the 10 columns spread evenly across InnerWidth - first/last
    // column flush with the same inner edges as the card contents above.
    // If the panel were ever too narrow for that, fall back to the
    // normal item spacing and center the row instead.
    const float ColumnWidth = 72.0f * DpiScale;
    const float BarWidth = 28.0f * DpiScale;
    const float MinGap = ImGui::GetStyle().ItemSpacing.x;
    const float SpreadGap = (InnerWidth - MaxEqBands * ColumnWidth) / static_cast<float>(MaxEqBands - 1);
    const float ColumnGapPx = std::max(SpreadGap, MinGap);
    const float RowWidth = MaxEqBands * ColumnWidth + (MaxEqBands - 1) * ColumnGapPx;
    const ImVec2 RowOrigin = ImGui::GetCursorScreenPos();
    const float RowStartX = InnerX + std::max(0.0f, (InnerWidth - RowWidth) * 0.5f);

    // Moves the cursor (on its current line) so an item of ItemWidth is
    // centered within the column starting at ColumnX.
    const auto CenterInColumn = [&](float ColumnX, float ItemWidth)
    {
        ImGui::SetCursorScreenPos(
            ImVec2(ColumnX + (ColumnWidth - ItemWidth) * 0.5f, ImGui::GetCursorScreenPos().y));
    };

    for (size_t i = 0; i < MaxEqBands; ++i)
    {
        const float ColumnX = RowStartX + static_cast<float>(i) * (ColumnWidth + ColumnGapPx);
        ImGui::SetCursorScreenPos(ImVec2(ColumnX, RowOrigin.y));

        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginGroup();

        float Gain = HwEqBands[i].GainDb;
        CenterInColumn(ColumnX, BarWidth);
        if (DrawEqBandBar("##gain", &Gain, -12.0f, 12.0f, ImVec2(BarWidth, 120.0f * DpiScale), DpiScale))
        {
            HwEqBands[i].GainDb = Gain;
            SendHwEqBand(static_cast<uint8>(i));
        }

        const float NominalHz = DefaultEqCenterFrequenciesHz[i];
        const std::string FreqLabel = NominalHz >= 1000.0f
                                           ? std::to_string(static_cast<int>(NominalHz / 1000.0f)) + "k"
                                           : std::to_string(static_cast<int>(NominalHz));
        CenterInColumn(ColumnX, ImGui::CalcTextSize(FreqLabel.c_str()).x);
        ImGui::TextUnformatted(FreqLabel.c_str());

        if (Preferences.bAdvancedEqMode)
        {
            ImGui::SetCursorScreenPos(ImVec2(ColumnX, ImGui::GetCursorScreenPos().y));
            ImGui::SetNextItemWidth(ColumnWidth);
            int FilterTypeIndex = static_cast<int>(HwEqBands[i].FilterType);
            if (ImGui::Combo("##type", &FilterTypeIndex, EqFilterTypeLabels, IM_ARRAYSIZE(EqFilterTypeLabels)))
            {
                HwEqBands[i].FilterType = static_cast<EqFilterType>(FilterTypeIndex);
                SendHwEqBand(static_cast<uint8>(i));
            }

            ImGui::SetNextItemWidth(ColumnWidth);
            float Frequency = HwEqBands[i].FrequencyHz;
            if (ImGui::DragFloat("##freq", &Frequency, 1.0f, 20.0f, 20000.0f, "%.0f Hz"))
            {
                HwEqBands[i].FrequencyHz = Frequency;
                SendHwEqBand(static_cast<uint8>(i));
            }

            ImGui::SetNextItemWidth(ColumnWidth);
            float Q = HwEqBands[i].Q;
            if (ImGui::DragFloat("##q", &Q, 0.01f, 0.1f, 10.0f, "Q %.2f"))
            {
                HwEqBands[i].Q = Q;
                SendHwEqBand(static_cast<uint8>(i));
            }
        }
        // Pins the group to the full column width in both modes, so every
        // band is the same size and toggling Advanced editing only changes
        // the bands' height, never the tab's width.
        ImGui::SetCursorScreenPos(ImVec2(ColumnX, ImGui::GetCursorScreenPos().y));
        ImGui::Dummy(ImVec2(ColumnWidth, 0.0f));

        ImGui::EndGroup();
        ImGui::PopID();
    }
}

void App::RefreshContentStreams()
{
    if (auto Result = Client.RequestContentStreams())
    {
        ContentStreams = std::move(*Result);
    }
    else
    {
        MarkDisconnected();
    }
}

void App::DrawAppPresetCard()
{
    // One entry per app (an app can have several streams - e.g. one per
    // browser tab); playing if any of its streams is.
    struct AppEntry
    {
        std::string Name;
        std::string MediaName;
        std::string BoundPreset;
        bool bPlaying = false;
        bool bAutoDetect = false;
        bool bGame = false; // any of its streams looks like a game
    };
    std::vector<AppEntry> Apps;
    for (const ContentStreamInfo& Stream : ContentStreams)
    {
        auto It = std::find_if(Apps.begin(), Apps.end(), [&](const AppEntry& E) { return E.Name == Stream.AppName; });
        if (It == Apps.end())
        {
            Apps.push_back({Stream.AppName, Stream.MediaName, Stream.BoundPresetName, Stream.bPlaying,
                            Stream.bAutoDetect, Stream.bGame});
            continue;
        }
        It->bGame = It->bGame || Stream.bGame;
        if (Stream.bPlaying && !It->bPlaying)
        {
            It->bPlaying = true;
            It->MediaName = Stream.MediaName; // show what's actually playing
        }
    }

    // Keep the user's pick while that app is around; otherwise follow the
    // app currently driving the EQ, else the first playing one, else the
    // first listed.
    const auto Find = [&](const std::string& Name) -> const AppEntry*
    {
        auto It = std::find_if(Apps.begin(), Apps.end(), [&](const AppEntry& E) { return E.Name == Name; });
        return It != Apps.end() ? &*It : nullptr;
    };
    if (!Find(SelectedApp))
    {
        auto Playing = std::find_if(Apps.begin(), Apps.end(), [](const AppEntry& E) { return E.bPlaying; });
        SelectedApp = Find(LastStatus.PresetSourceApp) ? LastStatus.PresetSourceApp
                      : Playing != Apps.end()          ? Playing->Name
                      : !Apps.empty()                  ? Apps.front().Name
                                                       : std::string();
    }
    const AppEntry* Selected = Find(SelectedApp);

    {
        const ImVec2 RowStart = ImGui::GetCursorScreenPos();
        const float RowWidth = ImGui::GetContentRegionAvail().x;
        ImGui::TextUnformatted("App preset");
        ImGui::SameLine();
        if (LastStatus.bHasPlaybackPriority)
        {
            // No vertical frame padding: the combo is exactly one text line
            // tall, so the header row keeps its height and lines up with the
            // device card's.
            const ImGuiStyle& Style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Style.FramePadding.x, 0.0f));
            const float ComboWidth = PlaybackPriorityComboWidth();
            const char* Caption = "Priority";
            RightAlignCursor(RowStart.x, RowWidth, ImGui::CalcTextSize(Caption).x + Style.ItemSpacing.x + ComboWidth);
            ImGui::TextColored(palette::TextMutedV4, "%s", Caption);
            ImGui::SameLine();
            DrawPlaybackPriorityCombo("##card_playback_priority", ComboWidth);
            ImGui::PopStyleVar();
        }
        else
        {
            // A daemon from before the priority setting.
            const size_t PlayingCount =
                std::count_if(Apps.begin(), Apps.end(), [](const AppEntry& E) { return E.bPlaying; });
            char Summary[32];
            std::snprintf(Summary, sizeof(Summary), "%zu playing", PlayingCount);
            RightAlignCursor(RowStart.x, RowWidth, ImGui::CalcTextSize(Summary).x);
            ImGui::TextColored(palette::TextMutedV4, "%s", Summary);
        }
    }

    // App picker - only apps currently routed into RamkolFX are listed,
    // since that's all the daemon reports (bindings for absent apps stay
    // in effect, they just can't be edited until the app shows up again).
    ImGui::BeginDisabled(Apps.empty());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##app_select", Selected ? Selected->Name.c_str() : "(no apps)"))
    {
        for (const AppEntry& Entry : Apps)
        {
            ImGui::PushID(Entry.Name.c_str());
            if (ImGui::Selectable(Entry.Name.c_str(), Entry.Name == SelectedApp))
            {
                SelectedApp = Entry.Name;
            }
            if (!Entry.MediaName.empty() && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", Entry.MediaName.c_str());
            }
            ImGui::SameLine();
            ImGui::TextColored(Entry.bPlaying ? palette::SuccessV4 : palette::TextMutedV4,
                               Entry.bPlaying ? "playing" : "paused");
            if (Entry.bGame)
            {
                ImGui::SameLine();
                ImGui::TextColored(palette::TextMutedV4, "- game");
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    // Preset to switch to while the selected app plays; "(none)" unbinds,
    // leaving the app to whatever the device / manual choice is.
    ImGui::BeginDisabled(!Selected);
    ImGui::SetNextItemWidth(-1);
    const bool bSelectedAuto = Selected && Selected->bAutoDetect;
    const bool bAutoUnavailable = !IsAutoDetectUsable();
    const char* Preview = bSelectedAuto                                 ? "Auto-detect (AI)"
                          : Selected && !Selected->BoundPreset.empty() ? Selected->BoundPreset.c_str()
                                                                         : "(none)";
    if (ImGui::BeginCombo("##app_preset_select", Preview, ImGuiComboFlags_HeightLarge))
    {
        std::optional<std::string> Choice;
        bool bChoseAuto = false;
        if (ImGui::Selectable("(none)", Selected && !bSelectedAuto && Selected->BoundPreset.empty()))
        {
            Choice = std::string();
        }
        // Above the presets: picks a preset by what's playing.
        ImGui::BeginDisabled(bAutoUnavailable && !bSelectedAuto);
        if (ImGui::Selectable("Auto-detect (AI)", bSelectedAuto))
        {
            bChoseAuto = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", bAutoUnavailable ? AutoDetectUnavailableReason()
                                                     : "Listens to what this app plays and picks a preset for it "
                                                       "(speech, music genre, film)");
        }
        if (auto Picked = DrawPresetSelectables(Selected && !bSelectedAuto ? Selected->BoundPreset : std::string()))
        {
            Choice = std::move(Picked);
        }
        ImGui::EndCombo();
        if (Selected && (Choice || bChoseAuto) &&
            Apply(bChoseAuto ? Client.BindAppAutoDetect(Selected->Name) : Client.BindAppPreset(Selected->Name, *Choice)))
        {
            RefreshContentStreams(); // pick up the new binding right away
        }
    }
    ImGui::EndDisabled();

    // Who else wants the EQ right now (docs/playback-priority-plan.md),
    // as the daemon resolved it: the winning contender and the ones
    // waiting behind it. Empty from a daemon that predates the setting.
    const std::vector<std::string>& Waiting = LastStatus.WaitingApps;
    const bool bSelectedWaiting = Selected && std::find(Waiting.begin(), Waiting.end(), Selected->Name) != Waiting.end();

    // Apps holds copies, so Selected stays valid even if the bind above
    // refreshed ContentStreams; its BoundPreset just catches up next frame.
    if (!Selected)
    {
        ImGui::TextColored(palette::TextMutedV4, "Play something to bind it");
    }
    else if (Selected->bAutoDetect && !IsAutoDetectUsable())
    {
        ImGui::TextColored(palette::WarningV4, "Auto-detect unavailable");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", AutoDetectUnavailableReason());
        }
    }
    else if (LastStatus.PresetSource == EqPresetSource::App && LastStatus.PresetSourceApp == Selected->Name)
    {
        if (Selected->bAutoDetect && LastStatus.bAutoDetectActive)
        {
            ImGui::TextColored(palette::SuccessV4, "Active now - %s", AutoDetectSummary().c_str());
        }
        else
        {
            ImGui::TextColored(palette::SuccessV4, "Active now");
        }
    }
    else if (bSelectedWaiting)
    {
        ImGui::TextColored(palette::TextMutedV4, "Waiting: %s has priority", LastStatus.ContestWinnerApp.c_str());
    }
    else if (Selected->bAutoDetect)
    {
        ImGui::TextColored(palette::TextMutedV4, Selected->bGame ? "Recognised as a game: uses the Game preset"
                                                                 : "Detects what it plays");
    }
    else if (!Selected->BoundPreset.empty())
    {
        ImGui::TextColored(palette::TextMutedV4, "Switches in while it plays");
    }
    else
    {
        ImGui::TextColored(palette::TextMutedV4, "Follows device / manual");
    }

    // Each name selects that app above - if it's listed there (a waiting
    // app's streams may have just gone).
    const auto AppLink = [&](const std::string& Name, const ImVec4& Color)
    {
        if (!Find(Name))
        {
            ImGui::TextColored(palette::TextMutedV4, "%s", Name.c_str());
            return;
        }
        ImGui::TextColored(Color, "%s", Name.c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemClicked())
        {
            SelectedApp = Name;
        }
    };
    if (!Waiting.empty())
    {
        ImGui::TextColored(palette::TextMutedV4, "Waiting:");
        for (size_t i = 0; i < Waiting.size(); ++i)
        {
            ImGui::SameLine(0.0f, i == 0 ? -1.0f : 0.0f);
            if (i > 0)
            {
                ImGui::TextColored(palette::TextMutedV4, ",");
                ImGui::SameLine();
            }
            AppLink(Waiting[i], palette::AccentSolidV4);
        }
        return;
    }

    // Music first only lets *bound* apps take over from a game: say so
    // when an unbound one plays alongside it, the one case where the
    // setting seems to do nothing.
    if (LastStatus.bHasPlaybackPriority && LastStatus.Priority == PlaybackPriority::MusicFirst &&
        LastStatus.bContestWinnerIsGame)
    {
        auto Unbound = std::find_if(Apps.begin(), Apps.end(), [](const AppEntry& E)
                                    { return E.bPlaying && !E.bGame && E.BoundPreset.empty() && !E.bAutoDetect; });
        if (Unbound != Apps.end())
        {
            AppLink(Unbound->Name, palette::WarningV4);
            ImGui::SameLine();
            ImGui::TextColored(palette::TextMutedV4, "isn't bound: bind it to beat the game");
        }
    }
}

namespace
{
struct PlaybackPriorityChoice
{
    PlaybackPriority Value;
    const char* Help;
};
constexpr PlaybackPriorityChoice PlaybackPriorityChoices[] = {
    {PlaybackPriority::GameFirst, "While a game is open, its preset plays. Bound music and video apps wait."},
    {PlaybackPriority::MusicFirst, "A bound app that plays (Spotify, say) takes over from the game. The game's "
                                   "preset comes back once it stops."},
    {PlaybackPriority::Latest, "Whichever app started playing most recently wins."},
};
constexpr const char* PlaybackPriorityHelp = "Only one EQ curve plays at a time. This decides whose it is when a "
                                             "game and another bound app play together.";
} // namespace

float App::PlaybackPriorityComboWidth() const
{
    const ImGuiStyle& Style = ImGui::GetStyle();
    float LabelWidth = 0.0f;
    for (const PlaybackPriorityChoice& Entry : PlaybackPriorityChoices)
    {
        LabelWidth = std::max(LabelWidth, ImGui::CalcTextSize(PlaybackPriorityLabel(Entry.Value)).x);
    }
    return LabelWidth + Style.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + Style.ItemInnerSpacing.x;
}

void App::DrawPlaybackPriorityCombo(const char* Id, float Width)
{
    // Every copy of this combo (App preset card, Settings) and the tray's
    // Priority submenu read LastStatus and change it only through
    // SetPlaybackPriority, so they always show the same choice.
    const bool bUsable = bConnected && LastStatus.bHasPlaybackPriority;
    ImGui::BeginDisabled(!bUsable);
    ImGui::SetNextItemWidth(Width);
    // A value from a newer daemon has no label here: none of the choices
    // is shown as current, so each can still be picked.
    const char* Current = bUsable ? PlaybackPriorityLabel(LastStatus.Priority) : "-";
    if (ImGui::BeginCombo(Id, Current ? Current : "Unknown"))
    {
        for (const PlaybackPriorityChoice& Entry : PlaybackPriorityChoices)
        {
            if (ImGui::Selectable(PlaybackPriorityLabel(Entry.Value), Entry.Value == LastStatus.Priority) &&
                Entry.Value != LastStatus.Priority)
            {
                SetPlaybackPriority(Entry.Value);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", Entry.Help);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(!bConnected                         ? "Not connected to the RamkolFX daemon"
                               : !LastStatus.bHasPlaybackPriority ? "Restart the RamkolFX daemon to use this setting"
                                                                  : PlaybackPriorityHelp);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string App::AutoDetectSummary() const
{
    if (LastStatus.bDetectionHeld)
    {
        return "paused (EQ changed by hand)";
    }
    if (LastStatus.Detected == DetectedClass::None)
    {
        return "listening...";
    }
    char Text[64];
    std::snprintf(Text, sizeof(Text), "%s %u%%", DetectedClassLabel(LastStatus.Detected),
                  static_cast<unsigned>(LastStatus.DetectedConfidence));
    return Text;
}

bool App::IsAutoDetectUsable() const
{
    return LastStatus.bHasAutoDetect && !bDetectedClassesUnsupported &&
           LastStatus.AutoDetect == AutoDetectAvailability::Ok;
}

const char* App::AutoDetectUnavailableReason() const
{
    // A daemon from before Auto-detect sends no Auto-detect status and
    // refuses its requests.
    return !LastStatus.bHasAutoDetect || bDetectedClassesUnsupported
               ? "Restart the RamkolFX daemon to enable Auto-detect"
               : AutoDetectAvailabilityLabel(LastStatus.AutoDetect);
}

void App::RefreshAutoDetectReport()
{
    if (auto Result = Client.RequestDetectedClasses())
    {
        AutoDetectReport = std::move(*Result);
        bHaveAutoDetectReport = true;
        ReportTableRevision = LastStatus.DetectedTableRevision;
        ReportCatalogRevision = LastStatus.PresetCatalogRevision;
        AutoDetectReportTimerSeconds = 1.0f; // live scores
    }
    else if (Client.IsConnected())
    {
        // Refused: a daemon that predates the request. Stop asking until
        // the next reconnect rather than dropping the connection.
        bDetectedClassesUnsupported = true;
    }
    else
    {
        MarkDisconnected();
    }
}

namespace
{
// The class table is drawn three classes per row: the window has a fixed
// height (see main.cpp), and the Equalizer tab is already tall.
constexpr size_t AutoDetectTableColumns = 3;
constexpr size_t AutoDetectTableRows = (DetectedClassCount - 1 + AutoDetectTableColumns - 1) / AutoDetectTableColumns;
} // namespace

void App::DrawAutoDetectTimingRow(float RowStartX, float RowWidth)
{
    const bool bSupported = LastStatus.bHasAutoDetectTiming;
    AutoDetectTiming Timing = bEditingTiming ? EditedTiming : LastStatus.DetectTiming;

    // The fastest cadence the CPU limit allows, from the measured run time
    // (0 before any run): the daemon stretches the interval up to it.
    const double LimitedToS =
        AutoDetectReport.Runs > 0 && Timing.CpuLimitPercent > 0.0f
            ? static_cast<double>(AutoDetectReport.P50Us) / 1e6 / (static_cast<double>(Timing.CpuLimitPercent) / 100.0)
            : 0.0;
    const bool bCpuLimited = LimitedToS * 1000.0 > static_cast<double>(Timing.CheckEveryMs) + 50.0;

    const ImGuiStyle& Style = ImGui::GetStyle();
    constexpr int Columns = 4;
    const float ColumnGap = 2.0f * Style.ItemSpacing.x;
    const float ColumnWidth = (RowWidth - ColumnGap * (Columns - 1)) / Columns;
    const float LabelWidth = ImGui::CalcTextSize("Keep at least").x + Style.ItemSpacing.x;
    const float SliderWidth = ColumnWidth - LabelWidth;
    const float Y = ImGui::GetCursorScreenPos().y + Style.ItemSpacing.y;

    bool bAnyActive = false;
    bool bCommit = false;
    int Column = 0;
    // One labelled slider. Values in ms are shown in seconds and snapped
    // to StepMs.
    auto Slider = [&](const char* Label, const char* Id, float* Value, float Min, float Max, const char* Format,
                      const char* Tooltip, bool bWarn)
    {
        const float X = RowStartX + static_cast<float>(Column++) * (ColumnWidth + ColumnGap);
        ImGui::SetCursorScreenPos(ImVec2(X, Y + Style.FramePadding.y));
        ImGui::TextColored(bWarn ? palette::WarningV4 : palette::TextSecondaryV4, "%s", Label);
        const bool bLabelHovered = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(ImVec2(X + LabelWidth, Y));
        ImGui::SetNextItemWidth(SliderWidth);
        const bool bChanged = ImGui::SliderFloat(Id, Value, Min, Max, Format, ImGuiSliderFlags_AlwaysClamp);
        bAnyActive |= ImGui::IsItemActive();
        bCommit |= ImGui::IsItemDeactivatedAfterEdit();
        if ((bLabelHovered || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) && !ImGui::IsItemActive())
        {
            if (!bSupported)
            {
                ImGui::SetTooltip("Restart the RamkolFX daemon to adjust Auto-detect's timing");
            }
            else
            {
                ImGui::SetTooltip("%s", Tooltip);
            }
        }
        return bChanged;
    };
    auto MsSlider = [&](const char* Label, const char* Id, uint32& Ms, uint32 MinMs, uint32 MaxMs, uint32 StepMs,
                        const char* Format, const char* Tooltip, bool bWarn = false)
    {
        float Seconds = static_cast<float>(Ms) / 1000.0f;
        if (Slider(Label, Id, &Seconds, static_cast<float>(MinMs) / 1000.0f, static_cast<float>(MaxMs) / 1000.0f,
                   Format, Tooltip, bWarn))
        {
            Ms = static_cast<uint32>(std::lround(Seconds * 1000.0f / static_cast<float>(StepMs))) * StepMs;
        }
    };

    ImGui::BeginDisabled(!bSupported);
    std::string CheckTooltip =
        "How often the AI listens to what's playing while it works out what it is (three times less often "
        "once that has settled, or on battery). Shorter notices changes sooner and costs more CPU.";
    if (bCpuLimited)
    {
        char Note[160];
        snprintf(Note, sizeof(Note), "\n\nRight now each check takes %.1f ms, so the %.1f%% CPU limit holds it to "
                 "one every %.1f s.",
                 static_cast<double>(AutoDetectReport.P50Us) / 1000.0, static_cast<double>(Timing.CpuLimitPercent),
                 LimitedToS);
        CheckTooltip += Note;
    }
    MsSlider("Check every", "##detect_check_every", Timing.CheckEveryMs, MinCheckEveryMs, MaxCheckEveryMs, 100,
             bCpuLimited ? "%.1f s (CPU limit)" : "%.1f s", CheckTooltip.c_str(), bCpuLimited);
    MsSlider("Switch after", "##detect_switch_after", Timing.SwitchAfterMs, 0, MaxSwitchAfterMs, 100, "%.1f s",
             "How long new content has to stand out before the EQ switches to it. Shorter reacts faster; "
             "longer ignores brief sounds, like a jingle or someone talking over music.");
    MsSlider("Keep at least", "##detect_keep_at_least", Timing.KeepAtLeastMs, 0, MaxKeepAtLeastMs, 1000, "%.0f s",
             "The shortest time between two switches, so content that's on the fence doesn't flip the EQ back "
             "and forth. Skipped after a gap between tracks.");
    float CpuPercent = Timing.CpuLimitPercent;
    if (Slider("CPU limit", "##detect_cpu_limit", &CpuPercent, MinCpuLimitPercent, MaxCpuLimitPercent, "%.1f%%",
               "The most of one CPU core detection may use. When a check takes longer than this allows, checks "
               "are spaced further apart to stay under it.",
               false))
    {
        Timing.CpuLimitPercent = std::round(CpuPercent * 10.0f) / 10.0f;
    }
    ImGui::EndDisabled();

    if (bAnyActive)
    {
        EditedTiming = Timing;
    }
    if (bCommit && bSupported && Timing != LastStatus.DetectTiming)
    {
        Apply(Client.SetAutoDetectTiming(Timing));
    }
    bEditingTiming = bAnyActive;
    // Leave the cursor below the row, as a normal row would.
    ImGui::SetCursorScreenPos(ImVec2(RowStartX, Y + ImGui::GetFrameHeight()));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

float App::AutoDetectCardHeight(float CardPad) const
{
    const float Row = ImGui::GetFrameHeight();
    const float Spacing = ImGui::GetStyle().ItemSpacing.y;
    float Height = 2.0f * CardPad + Row; // header
    if (Preferences.bAutoDetectExpanded)
    {
        // "Run detection on" row, the timing row, then the table.
        Height += (2 + AutoDetectTableRows) * (Spacing + Row) + Spacing;
    }
    return Height;
}

void App::DrawAutoDetectCard()
{
    const ImVec2 RowStart = ImGui::GetCursorScreenPos();
    const float RowWidth = ImGui::GetContentRegionAvail().x;
    const bool bUnavailable = !IsAutoDetectUsable();

    // Header: expand arrow, title, and what detection is doing right now.
    if (ImGui::ArrowButton("##auto_detect_expand", Preferences.bAutoDetectExpanded ? ImGuiDir_Down : ImGuiDir_Right))
    {
        Preferences.bAutoDetectExpanded = !Preferences.bAutoDetectExpanded;
        PersistGuiPreferences();
        bHaveAutoDetectReport = false; // fetch fresh on expand
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Auto-detect (AI)");
    {
        std::string State;
        ImVec4 Color = palette::TextMutedV4;
        if (bUnavailable)
        {
            State = "Unavailable";
            Color = palette::WarningV4;
        }
        else if (LastStatus.bAutoDetectActive)
        {
            State = (LastStatus.PresetSource == EqPresetSource::App ? LastStatus.PresetSourceApp : std::string("device")) +
                    ": " + AutoDetectSummary();
            Color = LastStatus.bDetectionHeld ? palette::TextMutedV4 : palette::AccentSolidV4;
        }
        else
        {
            State = "Bind an app or device to Auto-detect to use it";
        }
        ImGui::SameLine();
        RightAlignCursor(RowStart.x, RowWidth, ImGui::CalcTextSize(State.c_str()).x);
        ImGui::TextColored(Color, "%s", State.c_str());
        if (bUnavailable && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", AutoDetectUnavailableReason());
        }
    }
    if (!Preferences.bAutoDetectExpanded)
    {
        return;
    }

    AutoDetectReportTimerSeconds -= ImGui::GetIO().DeltaTime;
    if (bConnected && LastStatus.bHasAutoDetect && !bDetectedClassesUnsupported &&
        (!bHaveAutoDetectReport || AutoDetectReportTimerSeconds <= 0.0f ||
                       ReportTableRevision != LastStatus.DetectedTableRevision ||
                       ReportCatalogRevision != LastStatus.PresetCatalogRevision))
    {
        RefreshAutoDetectReport();
    }

    // Where it runs. GPU only when asked for: for a model this small, the
    // transfer and wake-up overhead costs more than it saves.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Run detection on");
    ImGui::SameLine();
    static const char* DeviceLabels[] = {"Auto (NPU, else CPU)", "CPU", "GPU", "NPU"};
    const auto DeviceIndex = static_cast<size_t>(LastStatus.AutoDetectDevice);
    const bool bDaemonKnowsAutoDetect = LastStatus.bHasAutoDetect && !bDetectedClassesUnsupported;
    ImGui::BeginDisabled(!bDaemonKnowsAutoDetect);
    ImGui::SetNextItemWidth(170.0f * DpiScale);
    if (ImGui::BeginCombo("##auto_detect_device",
                          DeviceIndex < std::size(DeviceLabels) ? DeviceLabels[DeviceIndex] : "?"))
    {
        for (size_t i = 0; i < std::size(DeviceLabels); ++i)
        {
            if (ImGui::Selectable(DeviceLabels[i], i == DeviceIndex))
            {
                Apply(Client.SetAutoDetectDevice(static_cast<InferenceDevice>(i)));
            }
            if (i == static_cast<size_t>(InferenceDevice::Gpu) && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Rarely worth it: the model is tiny, and keeping a GPU awake for it costs "
                                  "more power than running it on the CPU");
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (bUnavailable)
    {
        ImGui::TextColored(palette::WarningV4, "%s", AutoDetectUnavailableReason());
    }
    else if (!LastStatus.InferenceDeviceLabel.empty() && AutoDetectReport.Runs > 0)
    {
        ImGui::TextColored(palette::TextMutedV4, "%s - %.2f%% of a core - %.1f ms per run",
                           LastStatus.InferenceDeviceLabel.c_str(),
                           static_cast<double>(AutoDetectReport.DutyCycle) * 100.0,
                           static_cast<double>(AutoDetectReport.P50Us) / 1000.0);
    }
    else
    {
        ImGui::TextColored(palette::TextMutedV4, "%s",
                           LastStatus.InferenceDeviceLabel.empty() ? "Loads when first needed"
                                                                   : LastStatus.InferenceDeviceLabel.c_str());
    }

    DrawAutoDetectTimingRow(RowStart.x, RowWidth);

    // Class -> preset table, AutoDetectTableColumns per row: name, preset, live score.
    const float Row = ImGui::GetFrameHeight();
    const ImGuiStyle& Style = ImGui::GetStyle();
    const ImVec2 TableStart(RowStart.x, ImGui::GetCursorScreenPos().y + Style.ItemSpacing.y);
    const float ColumnGap = 2.0f * Style.ItemSpacing.x;
    const float ColumnWidth = (RowWidth - ColumnGap * (AutoDetectTableColumns - 1)) / AutoDetectTableColumns;
    const float LabelWidth = ImGui::CalcTextSize("Electronic").x + Style.ItemSpacing.x;
    const float BarWidth = 44.0f * DpiScale;
    const float ComboWidth = ColumnWidth - LabelWidth - BarWidth - Style.ItemSpacing.x;
    bool bTableChanged = false;
    for (size_t i = 0; i < AutoDetectReport.Classes.size(); ++i)
    {
        const DetectedClassEntry Entry = AutoDetectReport.Classes[i]; // copy: a pick re-fetches the report
        const float X = TableStart.x + static_cast<float>(i % AutoDetectTableColumns) * (ColumnWidth + ColumnGap);
        const float Y = TableStart.y + static_cast<float>(i / AutoDetectTableColumns) * (Row + Style.ItemSpacing.y);
        ImGui::PushID(static_cast<int>(Entry.Class));

        ImGui::SetCursorScreenPos(ImVec2(X, Y + Style.FramePadding.y));
        const bool bDetectedNow = LastStatus.bAutoDetectActive && LastStatus.Detected == Entry.Class;
        ImGui::TextColored(bDetectedNow ? palette::AccentSolidV4 : palette::TextSecondaryV4, "%s",
                           DetectedClassLabel(Entry.Class));
        if (Entry.Class == DetectedClass::Game && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Recognised from the app, not the sound: launched by Steam,\n"
                              "run through Wine/Proton, or marked as a game by the app");
        }

        ImGui::SetCursorScreenPos(ImVec2(X + LabelWidth, Y));
        ImGui::SetNextItemWidth(ComboWidth);
        if (ImGui::BeginCombo("##class_preset", Entry.PresetName.empty() ? "No switch" : Entry.PresetName.c_str(),
                              ImGuiComboFlags_HeightLarge))
        {
            std::optional<std::string> Choice;
            if (ImGui::Selectable("No switch", Entry.PresetName.empty()))
            {
                Choice = std::string();
            }
            if (auto Picked = DrawPresetSelectables(Entry.PresetName))
            {
                Choice = std::move(Picked);
            }
            ImGui::EndCombo();
            if (Choice && *Choice != Entry.PresetName && Apply(Client.BindDetectedClass(Entry.Class, *Choice)))
            {
                bTableChanged = true;
            }
        }
        if (ImGui::IsItemHovered())
        {
            const std::string Default = Entry.DefaultPresetName.empty() ? "No switch" : Entry.DefaultPresetName;
            ImGui::SetTooltip(Entry.PresetName == Entry.DefaultPresetName ? "%s (the default)" : "Default: %s",
                              Default.c_str());
        }

        // Live smoothed score while an Auto-detect binding is in effect.
        ImGui::SetCursorScreenPos(ImVec2(X + ColumnWidth - BarWidth, Y + Row * 0.3f));
        ImGui::ProgressBar(Entry.Score, ImVec2(BarWidth, Row * 0.4f), "");

        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(
        ImVec2(TableStart.x, TableStart.y + static_cast<float>(AutoDetectTableRows) * (Row + Style.ItemSpacing.y)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    if (bTableChanged)
    {
        RefreshAutoDetectReport();
    }
}

void App::RebindDeviceIfBound(const std::string& PresetName)
{
    // Called right after the pick/save, so LastStatus is the daemon's
    // answer to it. Source App there means the preset was the playing
    // app's own bound preset (reverted or updated in place) - that's
    // about the app's binding, and must not rewrite the device's.
    if (LastStatus.PresetSource != EqPresetSource::App && !LastStatus.DevicePresetName.empty() &&
        LastStatus.DevicePresetName != PresetName && !LastStatus.OutputDeviceName.empty())
    {
        Apply(Client.BindDevicePreset(LastStatus.OutputDeviceName, PresetName));
    }
}

std::optional<std::string> App::DrawPresetSelectables(const std::string& Current) const
{
    // Built-ins arrive grouped by category, then the user's presets - a
    // header wherever the category changes. Own ID scope, so a preset
    // named like an entry the caller draws next to these (the app
    // combo's "(none)") can't share its ImGui ID.
    std::optional<std::string> Picked;
    ImGui::PushID("eq_presets");
    for (size_t i = 0; i < EqPresets.size(); ++i)
    {
        const EqPresetInfo& Info = EqPresets[i];
        if (i == 0 || Info.Category != EqPresets[i - 1].Category)
        {
            ImGui::SeparatorText(EqPresetCategoryLabel(Info.Category));
        }
        if (ImGui::Selectable(Info.Name.c_str(), Info.Name == Current))
        {
            Picked = Info.Name;
        }
    }
    ImGui::PopID();
    return Picked;
}

void App::DrawPresetControls()
{
    const std::string& Active = LastStatus.ActivePresetName;
    const auto FindPreset = [&](const std::string& Name) -> const EqPresetInfo*
    {
        for (const EqPresetInfo& Info : EqPresets)
        {
            if (Info.Name == Name)
            {
                return &Info;
            }
        }
        return nullptr;
    };
    // Preset picker. Selecting the already-active preset re-applies it,
    // which is how a "Custom (unsaved)" tweak gets reverted.
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##eq_preset_select", Active.empty() ? "(none)" : Active.c_str(),
                          ImGuiComboFlags_HeightLarge))
    {
        // Applied after the list is drawn: Apply can replace EqPresets
        // (PresetCatalogRevision moved), and Active refers into LastStatus.
        const std::optional<std::string> Picked = DrawPresetSelectables(Active);
        ImGui::EndCombo();
        if (Picked && Apply(Client.ApplyEqPreset(*Picked)))
        {
            RebindDeviceIfBound(*Picked);
        }
    }

    // Looked up only now, after the combo: any Apply above may have
    // replaced EqPresets, so a pointer taken earlier could dangle.
    const EqPresetInfo* ActiveInfo = Active.empty() ? nullptr : FindPreset(Active);
    const bool bActiveIsUserPreset = ActiveInfo && !ActiveInfo->IsBuiltIn();

    ImGui::PushStyleColor(ImGuiCol_Button, palette::AccentGradientEndV4);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.024f, 0.071f, 0.11f, 1.0f));
    if (ImGui::Button("Save as...##eq_preset"))
    {
        // Pre-fill with the active user preset's name (so "save" over it is
        // one click), but not a built-in's - those can't be overwritten.
        const std::string Prefill = bActiveIsUserPreset ? Active : std::string();
        std::snprintf(SavePresetNameBuffer, sizeof(SavePresetNameBuffer), "%s", Prefill.c_str());
        ImGui::OpenPopup("Save EQ preset");
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::BeginDisabled(!bActiveIsUserPreset);
    if (ImGui::Button("Delete##eq_preset"))
    {
        PresetPendingDelete = Active;
        ImGui::OpenPopup("Delete EQ preset");
    }
    ImGui::EndDisabled();

    if (Active.empty())
    {
        ImGui::TextColored(palette::TextMutedV4, "No preset");
    }
    else if (LastStatus.bPresetModified)
    {
        ImGui::TextColored(palette::WarningV4, "Custom (unsaved)");
    }
    else
    {
        ImGui::TextColored(palette::TextMutedV4, "Saved");
    }
    // Why this preset is active, when it was switched in automatically.
    if (LastStatus.PresetSource == EqPresetSource::Device)
    {
        ImGui::SameLine();
        ImGui::TextColored(palette::TextMutedV4, "- auto (device)");
    }
    else if (LastStatus.PresetSource == EqPresetSource::App)
    {
        ImGui::SameLine();
        ImGui::TextColored(palette::TextMutedV4, "- auto (%s)", LastStatus.PresetSourceApp.c_str());
    }
    if (LastStatus.bAutoDetectActive)
    {
        ImGui::SameLine();
        ImGui::TextColored(palette::AccentSolidV4, "AI: %s", AutoDetectSummary().c_str());
    }

    if (ImGui::BeginPopupModal("Save EQ preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(260.0f * DpiScale);
        const bool bEnterPressed = ImGui::InputText("Name", SavePresetNameBuffer, sizeof(SavePresetNameBuffer),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);

        // The same normalization the daemon applies, so the warnings below
        // match what it will actually do; the daemon still has the final
        // say.
        const std::optional<std::string> Normalized = NormalizePresetName(SavePresetNameBuffer);
        const std::string Name = Normalized.value_or(std::string());
        const EqPresetInfo* Existing = Normalized ? FindPreset(Name) : nullptr;
        const bool bCanSave = Normalized && !(Existing && Existing->IsBuiltIn());
        if (Existing && Existing->IsBuiltIn())
        {
            ImGui::TextColored(palette::WarningV4, "Built-in presets can't be overwritten");
        }
        else if (Existing)
        {
            ImGui::TextColored(palette::WarningV4, "Overwrites the existing preset");
        }
        else
        {
            ImGui::TextUnformatted("");
        }

        ImGui::BeginDisabled(!bCanSave);
        const bool bSave = ImGui::Button("Save") || (bEnterPressed && bCanSave);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            ImGui::CloseCurrentPopup();
        }
        if (bSave)
        {
            // Apply also re-fetches the catalog (PresetCatalogRevision moved).
            if (Apply(Client.SaveEqPreset(Name)))
            {
                RebindDeviceIfBound(LastStatus.ActivePresetName); // the daemon's normalized name
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete EQ preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete preset \"%s\"?", PresetPendingDelete.c_str());
        ImGui::TextColored(palette::TextMutedV4, "The current sound stays as it is.");
        if (ImGui::Button("Delete"))
        {
            Apply(Client.DeleteEqPreset(PresetPendingDelete)); // re-fetches the catalog too
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace ramkolfx::gui
