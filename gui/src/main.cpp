// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <fontconfig/fontconfig.h>

#include <dlfcn.h>

#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"

#include "app.hpp"
#include "fd_wakeup.hpp"
#include "gui_prefs.hpp"
#include "ramkolfx/version.hpp"
#include "renderer.hpp"
#include "tray.hpp"
#include "ui_palette.hpp"
#include "x11_window.hpp"

namespace
{

// Base window/widget size the UI was designed at (100% / 96 DPI); every
// other size the UI computes is this times DpiScale. Sized to comfortably
// fit the widest/tallest tab (the two-column Spatial Audio & Speakers tab,
// and the Equalizer & Bass tab with advanced bass tuning + advanced band
// editing both open) without needing a manual resize - see
// ImGuiWindowFlags_AlwaysAutoResize in App::DrawUI(), which only autosizes
// ImGui's own virtual window within this fixed OS-level window, not the
// other way around. SDL_WINDOW_RESIZABLE (below) remains a manual escape
// hatch for anything unusually wide (e.g. a long device/HRTF name).
constexpr int BaseWindowWidth = 1000;
constexpr int BaseWindowHeight = 1000;
constexpr float BaseFontSizePixels = 13.0f; // ImGui's own default bake size

// While the window is hidden in the tray or minimized nothing is drawn and
// nothing is polled, so the loop sleeps in SDL_WaitEventTimeout instead of
// spinning on vsync (a vsync'd swap on an unmapped X11 window returns at
// once). Everything that can need it while idle wakes the wait itself:
// window events through SDL, tray clicks and menu picks through an
// FdWakeup on the bus socket, and timed tray work and the startup tray
// wait through their own timeouts. So this is only a backstop.
constexpr int IdleFallbackWakeMs = 10000;

// A tray click hides the window only if it's "up front" - focused, or
// focused until just now: on some desktops (GNOME Shell + AppIndicator)
// pressing the panel takes focus from the window a moment before the
// click's D-Bus call arrives.
constexpr Uint64 TrayClickFocusGraceMs = 500;

// With --minimized (the autostart entry), how long to wait for a tray host
// to show up before giving up and minimizing to the taskbar instead - at
// login the panel commonly starts after autostart apps do.
constexpr float StartupTrayWaitSeconds = 5.0f;

// Picks a UI scale factor so the window and widgets read at a sane
// physical size on high-DPI displays instead of staying pinned to a tiny
// fixed pixel size. SDL's per-display DPI query is authoritative where
// it works, but Linux/X11 in particular is notorious for not reporting
// the desktop's actual scale factor through it - RAMKOLFX_GUI_SCALE lets
// a user override the guess on setups where auto-detection gets it wrong.
float ResolveDpiScale()
{
    if (const char* Override = std::getenv("RAMKOLFX_GUI_SCALE"))
    {
        const float Value = static_cast<float>(std::atof(Override));
        if (Value >= 0.5f && Value <= 4.0f)
        {
            return Value;
        }
    }

    float DiagonalDpi = 96.0f;
    if (SDL_GetDisplayDPI(0, &DiagonalDpi, nullptr, nullptr) != 0 || DiagonalDpi <= 0.0f)
    {
        DiagonalDpi = 96.0f;
    }
    float Scale = DiagonalDpi / 96.0f;
    if (Scale < 1.0f)
    {
        Scale = 1.0f; // never shrink below the design size
    }
    if (Scale > 4.0f)
    {
        Scale = 4.0f; // guard against a bogus DPI reading blowing up the window
    }
    return Scale;
}

// Whether SDL2's API is running on SDL3 (sdl2-compat) rather than classic
// SDL2: sdl2-compat loads libSDL3 into the process (already when it's
// loaded itself), classic SDL2 never does. Checked directly instead of
// guessed from the reported version, which sdl2-compat merely happens to
// number 2.x.50 and up.
bool IsRunningOnSdl3()
{
    void* const Sdl3 = dlopen("libSDL3.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!Sdl3)
    {
        return false;
    }
    dlclose(Sdl3); // only drops the reference just taken
    return true;
}

// SDL before 2.0.22 ignores SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE: closing
// the only window always queues an SDL_QUIT right behind the CLOSE event.
bool QuitFollowsLastWindowClose()
{
    SDL_version Version;
    SDL_GetVersion(&Version);
    return SDL_VERSIONNUM(Version.major, Version.minor, Version.patch) < SDL_VERSIONNUM(2, 0, 22);
}

// Overrides the stock Dark theme's colors/rounding with the redesigned
// palette (see ui_palette.hpp) so combos, buttons, and other stock ImGui
// widgets read consistently with the custom-drawn pills/toggles/EQ bars
// (ui_widgets.hpp, position_dial.hpp) rather than clashing with them.
void ApplyPaletteStyle()
{
    using namespace ramkolfx::gui::palette;
    ImGuiStyle& Style = ImGui::GetStyle();
    ImVec4* Colors = Style.Colors;

    Colors[ImGuiCol_WindowBg] = PageBgV4;
    Colors[ImGuiCol_ChildBg] = PanelBgV4;
    Colors[ImGuiCol_PopupBg] = NestedCardBgV4;
    Colors[ImGuiCol_Border] = BorderV4;
    Colors[ImGuiCol_Text] = TextPrimaryV4;
    Colors[ImGuiCol_TextDisabled] = TextMutedV4;

    Colors[ImGuiCol_FrameBg] = InputBgV4;
    Colors[ImGuiCol_FrameBgHovered] = InputBgV4;
    Colors[ImGuiCol_FrameBgActive] = InputBgV4;

    Colors[ImGuiCol_Button] = InputBgV4;
    Colors[ImGuiCol_ButtonHovered] = ImVec4(AccentSolidV4.x, AccentSolidV4.y, AccentSolidV4.z, 0.4f);
    Colors[ImGuiCol_ButtonActive] = AccentGradientEndV4;

    Colors[ImGuiCol_Header] = ImVec4(AccentSolidV4.x, AccentSolidV4.y, AccentSolidV4.z, 0.3f);
    Colors[ImGuiCol_HeaderHovered] = ImVec4(AccentSolidV4.x, AccentSolidV4.y, AccentSolidV4.z, 0.45f);
    Colors[ImGuiCol_HeaderActive] = AccentGradientEndV4;

    Colors[ImGuiCol_CheckMark] = AccentSolidV4;
    Colors[ImGuiCol_SliderGrab] = AccentSolidV4;
    Colors[ImGuiCol_SliderGrabActive] = AccentHoverV4;

    Colors[ImGuiCol_Tab] = InputBgV4;
    Colors[ImGuiCol_TabHovered] = ImVec4(AccentSolidV4.x, AccentSolidV4.y, AccentSolidV4.z, 0.45f);
    Colors[ImGuiCol_TabActive] = AccentGradientEndV4;
    Colors[ImGuiCol_TabUnfocused] = InputBgV4;
    Colors[ImGuiCol_TabUnfocusedActive] = AccentGradientEndV4;

    Style.WindowRounding = RadiusTopCard;
    Style.WindowPadding = ImVec2(20.0f, 20.0f);
    Style.ChildRounding = RadiusNestedCard;
    Style.FrameRounding = RadiusInput;
    Style.PopupRounding = RadiusNestedCard;
    Style.GrabRounding = RadiusInput;
    Style.ScrollbarRounding = RadiusSmall;
    Style.TabRounding = RadiusSmall;
}

// Asks fontconfig for the file path of whatever font it would actually
// hand to an app requesting Family (a generic alias like "sans-serif")
// for Lang (an RFC-3066-ish code such as "ja", or nullptr for "just the
// default"). This is the same match `fc-match` performs, so it resolves
// to whatever font the user's own desktop/locale already has installed -
// no font is bundled with RamkolFX, sidestepping any font-license
// question entirely. Returns an empty string if fontconfig has nothing
// suitable (e.g. a minimal container image with no fonts installed).
std::string FindSystemFont(const char* Family, const char* Lang)
{
    FcPattern* Pattern = FcPatternCreate();
    FcPatternAddString(Pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(Family));

    FcLangSet* LangSet = nullptr;
    if (Lang)
    {
        LangSet = FcLangSetCreate();
        FcLangSetAdd(LangSet, reinterpret_cast<const FcChar8*>(Lang));
        FcPatternAddLangSet(Pattern, FC_LANG, LangSet);
    }

    FcConfigSubstitute(nullptr, Pattern, FcMatchPattern);
    FcDefaultSubstitute(Pattern);

    std::string Result;
    FcResult MatchResult;
    if (FcPattern* Match = FcFontMatch(nullptr, Pattern, &MatchResult))
    {
        FcChar8* File = nullptr;
        if (FcPatternGetString(Match, FC_FILE, 0, &File) == FcResultMatch && File)
        {
            Result = reinterpret_cast<const char*>(File);
        }
        FcPatternDestroy(Match);
    }

    if (LangSet)
    {
        FcLangSetDestroy(LangSet);
    }
    FcPatternDestroy(Pattern);
    return Result;
}

// One script merged into the font atlas on top of the base Latin font,
// each resolved to its own system font via FindSystemFont() so e.g.
// Japanese text is rasterized from whatever CJK font is actually
// installed rather than the base font's (typically empty) coverage of
// it. Two (or more) of these commonly resolve to the very same physical
// file - e.g. a single installed Noto Sans CJK file covers ja/ko/zh-cn
// at once, and the plain Latin "sans-serif" match often already covers
// Cyrillic/Greek too - but each entry is still loaded with its own
// distinct glyph range: AddFontFromFileTTF's range parameter controls
// which codepoints get *baked into the atlas*, not which the underlying
// file contains, so skipping a script just because its file was already
// opened for a different, narrower range would silently drop glyphs
// that file does contain. zh-cn intentionally uses the "common"
// ~2500-character subset rather than GetGlyphRangesChineseFull()'s
// ~21000: full coverage bloats the atlas texture and startup time far
// more than a control-panel app like this one warrants - see Dear
// ImGui's own font FAQ.
struct ScriptFallback
{
    const char* FontconfigLang;
    const ImWchar* (ImFontAtlas::*GetRanges)();
};

constexpr ScriptFallback ScriptFallbacks[] = {
    {"ja", &ImFontAtlas::GetGlyphRangesJapanese},
    {"ko", &ImFontAtlas::GetGlyphRangesKorean},
    {"zh-cn", &ImFontAtlas::GetGlyphRangesChineseSimplifiedCommon},
    {"th", &ImFontAtlas::GetGlyphRangesThai},
    {"vi", &ImFontAtlas::GetGlyphRangesVietnamese},
    {"ru", &ImFontAtlas::GetGlyphRangesCyrillic},
    {"el", &ImFontAtlas::GetGlyphRangesGreek},
};

// Builds the font atlas from system fonts found via fontconfig: a base
// Latin font, plus one merged-in fallback per ScriptFallbacks entry
// (skipped only when fontconfig has no font at all to offer for that
// script) - see FindSystemFont() and ScriptFallback's doc comment for
// why entries resolving to a file already used elsewhere still get
// loaded again. Falls back to ImGui's built-in bitmap font only if
// fontconfig can't find any system font whatsoever.
void LoadFonts(float DpiScale)
{
    ImGuiIO& IO = ImGui::GetIO();
    const float SizePixels = BaseFontSizePixels * DpiScale;

    const std::string BaseFontPath = FindSystemFont("sans-serif", nullptr);
    if (BaseFontPath.empty())
    {
        ImFontConfig FallbackConfig;
        FallbackConfig.SizePixels = SizePixels;
        IO.Fonts->AddFontDefault(&FallbackConfig);
    }
    else
    {
        IO.Fonts->AddFontFromFileTTF(BaseFontPath.c_str(), SizePixels);
    }

    for (const ScriptFallback& Fallback : ScriptFallbacks)
    {
        const std::string Path = FindSystemFont("sans-serif", Fallback.FontconfigLang);
        if (Path.empty())
        {
            continue;
        }

        ImFontConfig MergeConfig;
        MergeConfig.MergeMode = true;
        IO.Fonts->AddFontFromFileTTF(Path.c_str(), SizePixels, &MergeConfig, (IO.Fonts->*Fallback.GetRanges)());
    }
}

} // namespace

int main(int argc, char** argv)
{
    // --minimized: start in the background (the autostart entry passes
    // it, see autostart.hpp) - hidden in the tray if a tray option is on
    // and a tray exists, otherwise minimized to the taskbar.
    bool bStartMinimized = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string Arg = argv[i];
        if (Arg == "--version" || Arg == "-V")
        {
            printf("ramkolfx-gui %s\n", ramkolfx::Version);
            return 0;
        }
        if (Arg == "--minimized")
        {
            bStartMinimized = true;
        }
    }

    // Runs SDL_Quit on every way out of main(). Declared ahead of the Tray
    // so it runs *after* ~Tray: SDL_Quit may call dbus_shutdown() (older
    // SDL2 always does; newer ones when SDL_HINT_SHUTDOWN_DBUS_ON_QUIT is
    // set), and the Tray's own libdbus connection must be gone by then.
    // Harmless on the exits before SDL_Init.
    struct SdlQuitOnExit
    {
        ~SdlQuitOnExit() { SDL_Quit(); }
    } SdlQuitGuard;

    // Before any window exists: if the GUI is already running (say, hidden
    // in the tray), bring that one up instead of opening a second - unless
    // this is a background start, which must not pop up a window.
    ramkolfx::gui::Tray Tray;
    if (!Tray.ClaimSingleInstance(!bStartMinimized))
    {
        printf(bStartMinimized ? "[ramkolfx-gui] already running\n"
                               : "[ramkolfx-gui] already running - showing the existing window\n");
        return 0;
    }

    // Closing the window must not imply quitting when it can go to the
    // tray instead - window close is handled explicitly below, so SDL_QUIT
    // only ever means a real quit request (SIGINT/SIGTERM). By name rather
    // than SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, which headers before 2.0.22
    // lack; such an SDL doesn't know the hint either - see
    // bQuitFollowsClose below for how the loop copes with that.
    SDL_SetHint("SDL_QUIT_ON_LAST_WINDOW_CLOSE", "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        fprintf(stderr, "[ramkolfx-gui] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const float DpiScale = ResolveDpiScale();
    const ramkolfx::gui::GuiPreferences Preferences = ramkolfx::gui::LoadGuiPreferences();

    const char* VideoDriver = SDL_GetCurrentVideoDriver();
    const bool bWayland = VideoDriver && std::string(VideoDriver) == "wayland";
    const bool bX11 = VideoDriver && std::string(VideoDriver) == "x11";
    // xdg-shell never tells a Wayland client it was minimized.
    const bool bMinimizeReported = !bWayland;
    // Minimize-to-tray also needs to take the minimized window off the
    // taskbar, which only X11 support built in can (see TryHideToTray). The
    // one copy: App gets it, and the startup decision below uses it too.
    using MinimizeToTrayBlocker = ramkolfx::gui::App::MinimizeToTrayBlocker;
    const MinimizeToTrayBlocker MinimizeBlocker = !bX11                                     ? MinimizeToTrayBlocker::NotX11
                                                  : !ramkolfx::gui::IsX11WindowControlBuilt() ? MinimizeToTrayBlocker::BuiltWithoutX11
                                                                                            : MinimizeToTrayBlocker::None;
    const bool bMinimizeToTraySupported = MinimizeBlocker == MinimizeToTrayBlocker::None;
    const bool bSdl3 = IsRunningOnSdl3();

    auto Renderer = ramkolfx::gui::CreateOpenGL3Renderer();

    // A --minimized start that may end up in the tray begins hidden, so
    // there's no window flash before the tray shows up. The same rule the
    // loop's tray icon follows (App::WantsTrayIcon), applied to the saved
    // settings since there's no App yet.
    const bool bStartHidden = bStartMinimized && Preferences.WantsTrayIcon(bMinimizeToTraySupported);

    SDL_Window* Window = SDL_CreateWindow(
        "RamkolFX Control", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(BaseWindowWidth * DpiScale), static_cast<int>(BaseWindowHeight * DpiScale),
        Renderer->GetSDLWindowFlags() | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
            (bStartHidden ? SDL_WINDOW_HIDDEN : 0));
    if (!Window)
    {
        fprintf(stderr, "[ramkolfx-gui] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ApplyPaletteStyle();
    ImGui::GetStyle().ScaleAllSizes(DpiScale);

    FcInit();
    LoadFonts(DpiScale);
    FcFini(); // path lookups are done; AddFontFromFileTTF already read the file bytes it needs

    if (!Renderer->Init(Window))
    {
        ImGui::DestroyContext();
        SDL_DestroyWindow(Window);
        return 1;
    }

    // The width both tabs target so they read as the same size - derived
    // from the same fixed BaseWindowWidth that sizes the OS window itself
    // (minus the window's own left+right padding), not from ImGui's
    // GetContentRegionAvail() at runtime: for an AlwaysAutoResize window,
    // that value is self-referential (it tracks whatever content the
    // window's own size last converged to), so two tabs with different
    // natural content widths converge to two different steady-state
    // values even though the OS-level window is a fixed physical size -
    // querying it live would just reintroduce that per-tab drift.
    const float TargetContentWidth = BaseWindowWidth * DpiScale - ImGui::GetStyle().WindowPadding.x * 2.0f;

    ramkolfx::gui::App App(DpiScale, TargetContentWidth, Preferences, MinimizeBlocker);

    bool bQuit = false;
    // In the tray, as opposed to on screen or minimized to the taskbar.
    // SDL's own window flags stay the truth about the window itself - see
    // TryHideToTray - this only adds what SDL can't know: why it's away.
    bool bHidden = bStartHidden;
    // Set while a window hidden to the tray is a minimized X11 one kept off
    // the taskbar, rather than an unmapped one - see TryHideToTray.
    bool bSkippingTaskbar = false;
    // Counts down while a hidden --minimized start waits for a tray host.
    float StartupTrayWaitRemaining = bStartHidden ? StartupTrayWaitSeconds : 0.0f;
    // Minimizing is deferred until a frame has been presented - a window
    // that was never mapped can't be minimized (notably on Wayland).
    bool bMinimizeAfterFrame = bStartMinimized && !bStartHidden;
    Uint64 LastFrameTicks = SDL_GetPerformanceCounter();
    // When the window last lost focus (0 = never) - see TrayClickFocusGraceMs.
    Uint64 LastFocusLostMs = 0;
    // Whether the previous pass skipped drawing - see App::OnWindowShown.
    bool bWasIdle = false;
    // The window belongs in the tray but is in the taskbar because the icon
    // isn't up - set by TryHideToTray whenever it finds no icon (a close or
    // minimize asking for the tray, a --minimized start that gave up
    // waiting, the icon lost while the window was in it). It moves on into
    // the tray whenever the icon comes up - unless the user brings it back
    // first. A tray host that's missing or failing for now doesn't cancel
    // this: it may still come up later (a panel started late, or restarted).
    bool bHideWhenIconShown = false;
    // ParkInTaskbar's own show-then-minimize, while still under way: the
    // window manager may focus or restore the window as part of it, which
    // isn't the user bringing it back (see bHideWhenIconShown). It's over
    // once the minimize is reported - or, where minimizing never is
    // (Wayland), once the minimize has been requested and any focus the
    // show brought has gone again.
    enum class ParkPhase
    {
        None,
        UntilMinimized,
        UntilFocusLost,
    };
    ParkPhase Park = ParkPhase::None;
    // See QuitFollowsLastWindowClose(). Such an SDL_QUIT is the window's own
    // close, already handled, not a quit request.
    const bool bQuitFollowsClose = QuitFollowsLastWindowClose();
    bool bExpectCloseQuit = false;

    // Signals tray D-Bus traffic - see FdWakeup: it wakes the hidden/
    // minimized-window wait, and while drawing lets Tray.Dispatch() skip the
    // socket on the (nearly all) frames with no traffic.
    const Uint32 WakeEventType = SDL_RegisterEvents(1);
    std::unique_ptr<ramkolfx::gui::FdWakeup> TrayWakeup;
    // Called once per pass, after the last tray call of the pass - so output
    // any of them left queued is covered too. A waker that gave up (see
    // FdWakeup::IsWatching) isn't replaced while the socket stays the same:
    // whatever stopped it would most likely stop the next one at once, and
    // each would wake the loop on its way out. Without one, every pass
    // services the socket and an idle wait falls back to its timeout.
    const auto ArmTrayWakeup = [&]()
    {
        // A waker must not outlive the connection it was made for, which
        // Tray replaces after losing the bus (its thread polls a duplicate,
        // so it can't be misled - this just stops it holding the dead
        // socket open).
        const int Fd = Tray.GetPollFd();
        if (TrayWakeup && TrayWakeup->GetFd() != Fd)
        {
            TrayWakeup.reset();
        }
        if (!TrayWakeup && Fd >= 0 && WakeEventType != static_cast<Uint32>(-1))
        {
            TrayWakeup = std::make_unique<ramkolfx::gui::FdWakeup>(Fd, WakeEventType);
        }
        if (TrayWakeup)
        {
            // Also wake once the socket drains, if a reply is still queued.
            TrayWakeup->Arm(Tray.HasPendingOutput());
        }
    };

    const auto IsWindowMinimized = [&]() { return (SDL_GetWindowFlags(Window) & SDL_WINDOW_MINIMIZED) != 0; };
    const auto StopSkippingTaskbar = [&]()
    {
        if (bSkippingTaskbar)
        {
            ramkolfx::gui::SetX11WindowSkipTaskbar(Window, false);
            bSkippingTaskbar = false;
        }
    };

    // Only ever hides while the tray icon is actually up - otherwise
    // there'd be nothing to bring the window back with, so it just notes
    // that the window is to go there once the icon is (the one place that's
    // decided - see bHideWhenIconShown) and leaves it to the caller where
    // it stays meanwhile.
    enum class HideResult
    {
        Hidden,
        NoIcon,      // will be hidden once the icon is up
        Unsupported, // minimized, and can't leave the taskbar - see below
    };
    const auto TryHideToTray = [&]() -> HideResult
    {
        // A minimized window is already unmapped (minimizing is only ever
        // reported on X11), so SDL_HideWindow would leave it in the
        // taskbar. It stays minimized instead - so SDL's MINIMIZED flag,
        // which Dear ImGui reads too, stays true - and only leaves the
        // taskbar and window switcher. Where that can't be done (a build
        // without X11 support, or a driver other than X11 reporting a
        // minimize), it isn't hidden at all - now or once the icon is up:
        // claiming it's in the tray while it's still in the taskbar would
        // only confuse both.
        if (IsWindowMinimized() && !bMinimizeToTraySupported)
        {
            return HideResult::Unsupported;
        }
        if (!Tray.IsIconShown())
        {
            bHideWhenIconShown = true;
            return HideResult::NoIcon;
        }
        if (IsWindowMinimized())
        {
            if (!ramkolfx::gui::SetX11WindowSkipTaskbar(Window, true))
            {
                return HideResult::Unsupported;
            }
            bSkippingTaskbar = true;
        }
        else
        {
            SDL_HideWindow(Window);
        }
        bHidden = true;
        // A park still under way is moot now - and its minimize must not
        // fire later, on the window's next return to the screen.
        bMinimizeAfterFrame = false;
        Park = ParkPhase::None;
        App.OnWindowHidden();
        return HideResult::Hidden;
    };
    const auto ShowWindow = [&](const std::string& ActivationToken)
    {
        StopSkippingTaskbar();
        bHideWhenIconShown = false;
        bMinimizeAfterFrame = false;
        Park = ParkPhase::None;

        // On Wayland, a window that's mapped but not focused may be
        // minimized - which xdg-shell never reports, and which no client
        // request can undo - or just behind other windows. With an
        // xdg-activation token, remapping it fixes either: the compositor
        // maps a new toplevel un-minimized and, given the token, focused.
        // SDL restores maximized/fullscreen state across the hide.
        //
        // Only SDL3 (which sdl2-compat runs on) can hand the token over: it
        // passes XDG_ACTIVATION_TOKEN to the compositor when it maps the
        // window, then clears it from its own copy of the environment;
        // classic SDL2 never reads it. Without a token (or on classic SDL2)
        // a remap couldn't take focus - it would only flicker the window
        // and lose its placement - so the window is just raised in place.
        // The token is only set right before a map: one left unread would
        // sit in SDL3's copy - out of reach of unsetenv() - and be
        // replayed, stale and rejected, on some later show.
        const Uint32 Flags = SDL_GetWindowFlags(Window);
        if (bWayland && bSdl3 && !ActivationToken.empty() && !(Flags & SDL_WINDOW_INPUT_FOCUS))
        {
            if (!(Flags & SDL_WINDOW_HIDDEN))
            {
                SDL_HideWindow(Window);
            }
            SDL_setenv("XDG_ACTIVATION_TOKEN", ActivationToken.c_str(), 1);
        }
        SDL_ShowWindow(Window);
        // Un-minimize. On X11, SDL2's SDL_RestoreWindow would also
        // un-maximize, so it's the fallback for other drivers only. SDL
        // hears of the change through its own X events either way.
        if (IsWindowMinimized() && !ramkolfx::gui::DeiconifyX11Window(Window))
        {
            SDL_RestoreWindow(Window);
        }
        SDL_RaiseWindow(Window);
        // SDL_setenv set it in the C environment too, which SDL3 leaves
        // alone - keep it out of any child process's environment.
        unsetenv("XDG_ACTIVATION_TOKEN");
        bHidden = false;
        StartupTrayWaitRemaining = 0.0f;
    };
    // Moves a window that's in the tray, or waiting for it, to the taskbar
    // while there's no icon to hold it - minimized, so it stays out of the
    // way as the user asked, and still within reach - and has it go on
    // into the tray once the icon is up.
    const auto ParkInTaskbar = [&]()
    {
        if (IsWindowMinimized())
        {
            // In the tray as a minimized X11 window: back onto the taskbar
            // is all it takes.
            StopSkippingTaskbar();
            bHidden = false;
            StartupTrayWaitRemaining = 0.0f;
        }
        else
        {
            // Unmapped: it has to be shown to be minimized, and only once a
            // frame has been presented (see bMinimizeAfterFrame).
            ShowWindow({});
            bMinimizeAfterFrame = true;
            Park = ParkPhase::UntilMinimized;
        }
        bHideWhenIconShown = true;
    };

    while (!bQuit)
    {
        // Whether the tray's waker fired since the last pass.
        bool bTrayWoke = false;
        SDL_Event Event;
        while (SDL_PollEvent(&Event))
        {
            if (Event.type == WakeEventType)
            {
                bTrayWoke = true;
                continue; // not for Dear ImGui
            }
            ImGui_ImplSDL2_ProcessEvent(&Event);
            const bool bCloseQuit = std::exchange(bExpectCloseQuit, false);
            if (Event.type == SDL_QUIT && !bCloseQuit)
            {
                bQuit = true;
            }
            if (Event.type == SDL_WINDOWEVENT && Event.window.windowID == SDL_GetWindowID(Window))
            {
                if (Event.window.event == SDL_WINDOWEVENT_CLOSE)
                {
                    bExpectCloseQuit = bQuitFollowsClose;
                    if (!App.ShouldMinimizeToTrayOnClose())
                    {
                        bQuit = true;
                    }
                    else
                    {
                        switch (TryHideToTray())
                        {
                        case HideResult::Hidden:
                            break;
                        case HideResult::NoIcon:
                            // No icon to hide to right now. With a tray host
                            // present it's most likely just between attempts
                            // (registration in flight or being retried, host
                            // restarting) - the user asked to keep RamkolFX
                            // running, so minimize instead of quitting (a
                            // window that already is just stays so). This
                            // can't go on indefinitely: a host that never
                            // answers fails each attempt at its deadline, and
                            // CanShowIcon() turns false after a few. Without
                            // one, the Settings popup already says close
                            // quits. Once the icon is up, the window moves on
                            // into the tray (see bHideWhenIconShown).
                            if (!Tray.CanShowIcon())
                            {
                                bQuit = true;
                            }
                            else if (!IsWindowMinimized())
                            {
                                SDL_MinimizeWindow(Window);
                            }
                            break;
                        case HideResult::Unsupported:
                            // Closed while minimized (from the taskbar's
                            // menu), somewhere a minimized window can't
                            // leave the taskbar. Doing nothing would drop
                            // the request, and restoring the window only to
                            // hide it would flash it on screen - so quit,
                            // as without a tray. The daemon keeps running.
                            bQuit = true;
                            break;
                        }
                    }
                }
                else if (Event.window.event == SDL_WINDOWEVENT_MINIMIZED)
                {
                    if (Park == ParkPhase::UntilMinimized)
                    {
                        Park = ParkPhase::None;
                    }
                    if (!bHidden && App.ShouldMinimizeToTrayOnMinimize())
                    {
                        // Without an icon right now, it stays minimized in
                        // the taskbar until there is one.
                        TryHideToTray();
                    }
                }
                else if (Event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                {
                    LastFocusLostMs = SDL_GetTicks64();
                    if (Park == ParkPhase::UntilFocusLost)
                    {
                        Park = ParkPhase::None;
                    }
                }
                else if (Event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                         Event.window.event == SDL_WINDOWEVENT_RESTORED)
                {
                    // Brought back from the taskbar by the user (focus is
                    // the only sign of that on Wayland) - it stays. Unless
                    // it's ParkInTaskbar's own show, still under way.
                    if (Park == ParkPhase::None)
                    {
                        bHideWhenIconShown = false;
                    }
                }
            }
        }

        // Brought back by someone else while in the tray (the window
        // manager - say, a "show desktop" toggle that restores minimized
        // windows) - resume rather than leave an undrawn window on screen.
        if (bHidden && !(SDL_GetWindowFlags(Window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)))
        {
            StopSkippingTaskbar();
            bHidden = false;
        }

        // The icon is shown whenever either tray option is on, not just
        // while hidden - so it's already in place (and the panel has laid
        // it out) by the time the window goes there.
        Tray.SetIconVisible(App.WantsTrayIcon());
        // Socket I/O only when the waker saw the socket ready - or when
        // there's no working waker on the current socket to say so.
        const bool bTraySocketReady = bTrayWoke || !TrayWakeup || !TrayWakeup->IsWatching() ||
                                      TrayWakeup->GetFd() != Tray.GetPollFd();
        Tray.Dispatch(bTraySocketReady);
        App.SetTrayAvailable(Tray.CanShowIcon());
        if (bHideWhenIconShown && Tray.IsIconShown())
        {
            bHideWhenIconShown = false;
            if (!bHidden)
            {
                TryHideToTray();
            }
        }

        const ramkolfx::gui::Tray::Requests Requests = Tray.TakeRequests();
        if (Requests.bQuit || bQuit)
        {
            // Out now - in particular not into the idle wait below, which
            // could hold up the exit.
            break;
        }
        bool bShowRequested = Requests.bShowWindow;
        bool bHideRequested = !bShowRequested && Requests.bHideWindow;
        if (!bShowRequested && !bHideRequested && Requests.bToggleWindow)
        {
            // Same rule as KDE's own tray apps: a click hides the window
            // only when it's already up front; otherwise it brings it up.
            const Uint32 Flags = SDL_GetWindowFlags(Window);
            const bool bFrontmost = (Flags & SDL_WINDOW_INPUT_FOCUS) ||
                                    (LastFocusLostMs != 0 && SDL_GetTicks64() - LastFocusLostMs < TrayClickFocusGraceMs);
            bShowRequested = bHidden || IsWindowMinimized() || !bFrontmost;
            bHideRequested = !bShowRequested;
        }
        if (bShowRequested)
        {
            ShowWindow(Requests.ActivationToken);
        }
        else if (bHideRequested && !bHidden)
        {
            TryHideToTray();
        }

        const Uint64 NowTicks = SDL_GetPerformanceCounter();
        const float DeltaTimeSeconds =
            static_cast<float>(NowTicks - LastFrameTicks) / static_cast<float>(SDL_GetPerformanceFrequency());
        LastFrameTicks = NowTicks;

        if (StartupTrayWaitRemaining > 0.0f)
        {
            StartupTrayWaitRemaining -= DeltaTimeSeconds;
            if (Tray.IsIconShown())
            {
                StartupTrayWaitRemaining = 0.0f; // stay hidden in the tray
            }
            else if (StartupTrayWaitRemaining <= 0.0f)
            {
                ParkInTaskbar();
            }
        }
        else if (bHidden && !Tray.IsIconShown())
        {
            // The icon went away while the window was in it (tray host
            // crashed or restarted, AppIndicator extension disabled, bus
            // lost) - don't strand it there, and send it back once the icon
            // is.
            ParkInTaskbar();
        }

        // The menu's Show/Hide entry follows what's on screen: a window
        // minimized to the taskbar gets "Show" (which restores it), not a
        // "Hide" that would only send it further away.
        const bool bIdle = bHidden || IsWindowMinimized();
        Tray.SetWindowShown(!bIdle);
        ArmTrayWakeup();

        if (bIdle)
        {
            // Nothing to draw, and nothing to poll the daemon for - see
            // App::OnWindowShown. Sleep until an SDL event (a restore, for a
            // minimized window), tray traffic (via the waker), or timed work
            // is due. Waited on only here, after the tray has been serviced
            // this pass - so even the very first pass of a hidden start
            // registers the icon before sleeping. The event is left queued
            // for the next pass's poll.
            bWasIdle = true;
            int WaitMs = IdleFallbackWakeMs;
            if (StartupTrayWaitRemaining > 0.0f)
            {
                WaitMs = std::min(WaitMs, static_cast<int>(std::ceil(StartupTrayWaitRemaining * 1000.0f)));
            }
            if (const int TrayTimeoutMs = Tray.GetTimeoutMs(); TrayTimeoutMs >= 0)
            {
                WaitMs = std::min(WaitMs, TrayTimeoutMs);
            }
            SDL_WaitEventTimeout(nullptr, WaitMs);
            continue;
        }

        if (bWasIdle)
        {
            bWasIdle = false;
            App.OnWindowShown();
        }

        Renderer->NewFrame();
        ImGui::NewFrame();

        App.Tick(DeltaTimeSeconds);

        ImGui::Render();
        Renderer->RenderFrame(ImGui::GetDrawData());

        if (bMinimizeAfterFrame)
        {
            bMinimizeAfterFrame = false;
            SDL_MinimizeWindow(Window);
            if (Park == ParkPhase::UntilMinimized && !bMinimizeReported)
            {
                // No MINIMIZED event will end the park: it's over once the
                // focus the show brought, if any, is gone again.
                Park = (SDL_GetWindowFlags(Window) & SDL_WINDOW_INPUT_FOCUS) ? ParkPhase::UntilFocusLost
                                                                             : ParkPhase::None;
            }
        }
    }

    TrayWakeup.reset(); // its thread may push SDL events - stop it before SDL_Quit
    Renderer->Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(Window);
    return 0; // ~Tray, then SDL_Quit (SdlQuitGuard)
}
