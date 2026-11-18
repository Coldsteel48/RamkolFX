// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include <cstdio>
#include <cstdlib>
#include <string>

#include <fontconfig/fontconfig.h>

#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"

#include "app.hpp"
#include "ramkolfx/version.hpp"
#include "renderer.hpp"
#include "ui_palette.hpp"

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
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--version" || std::string(argv[i]) == "-V")
        {
            printf("ramkolfx-gui %s\n", ramkolfx::Version);
            return 0;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        fprintf(stderr, "[ramkolfx-gui] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const float DpiScale = ResolveDpiScale();

    auto Renderer = ramkolfx::gui::CreateOpenGL3Renderer();

    SDL_Window* Window = SDL_CreateWindow(
        "RamkolFX Control", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(BaseWindowWidth * DpiScale), static_cast<int>(BaseWindowHeight * DpiScale),
        Renderer->GetSDLWindowFlags() | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!Window)
    {
        fprintf(stderr, "[ramkolfx-gui] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
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
        SDL_Quit();
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

    ramkolfx::gui::App App(DpiScale, TargetContentWidth);

    bool bQuit = false;
    Uint64 LastFrameTicks = SDL_GetPerformanceCounter();
    while (!bQuit)
    {
        SDL_Event Event;
        while (SDL_PollEvent(&Event))
        {
            ImGui_ImplSDL2_ProcessEvent(&Event);
            if (Event.type == SDL_QUIT)
            {
                bQuit = true;
            }
            if (Event.type == SDL_WINDOWEVENT && Event.window.event == SDL_WINDOWEVENT_CLOSE &&
                Event.window.windowID == SDL_GetWindowID(Window))
            {
                bQuit = true;
            }
        }

        const Uint64 NowTicks = SDL_GetPerformanceCounter();
        const float DeltaTimeSeconds =
            static_cast<float>(NowTicks - LastFrameTicks) / static_cast<float>(SDL_GetPerformanceFrequency());
        LastFrameTicks = NowTicks;

        Renderer->NewFrame();
        ImGui::NewFrame();

        App.Tick(DeltaTimeSeconds);

        ImGui::Render();
        Renderer->RenderFrame(ImGui::GetDrawData());
    }

    Renderer->Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(Window);
    SDL_Quit();
    return 0;
}
