// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#pragma once

#include <string>
#include <vector>

// The RamkolFX logo mark - a rounded accent-gradient square with a dark
// pixel "R" - and every rendering of it. This is the one definition: the
// in-app header (App::DrawUI), the window icon (main.cpp), the tray icon
// (tray.cpp) and the installed app icon (packaging/common/ramkolfx.svg,
// generated from SvgIcon() by tools/logo_svg.cpp and kept in sync by the
// logo_svg_up_to_date test) all draw from here, so they can't drift apart.
namespace ramkolfx::gui::logo
{

// The name the desktop knows this app by: the installed ramkolfx.desktop,
// the icon theme's ramkolfx.svg, and the Wayland app_id / X11 WM_CLASS the
// window carries so the compositor can match it to those two.
inline constexpr const char* AppId = "ramkolfx";

// Geometry, as fractions of the icon's side.
inline constexpr float CornerRadius = 0.22f;
inline constexpr int GlyphColumns = 5;
inline constexpr int GlyphRows = 7;
inline constexpr float GlyphCell = 0.09f;
inline constexpr const char* Glyph[GlyphRows] = {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"};

// Glyph color; the background is palette::AccentGradientStart -> End, left to right.
inline constexpr unsigned char InkRgb[3] = {6, 18, 28};

struct ArgbImage
{
    int Size = 0;
    std::vector<unsigned char> Argb; // Size*Size pixels, bytes A,R,G,B (straight alpha)
};

// Antialiased Size x Size raster of the logo.
ArgbImage RenderArgb(int Size);

// The logo as a standalone SVG document.
std::string SvgIcon();

} // namespace ramkolfx::gui::logo
