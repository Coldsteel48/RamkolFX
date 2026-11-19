// RamkolFX
// Copyright (C) 2026 Roman Levin (Coldsteel48)
//
// This file is part of RamkolFX, dual-licensed under the GNU General
// Public License v3.0 (see LICENSE) or a separate commercial license
// (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
// terms of the Contributor License Agreement (see CLA.md).

#include "logo.hpp"

#include "ui_palette.hpp"

#include <cmath>
#include <cstdio>

// Only palette constants and ImVec4 are used here - no ImGui functions - so
// the build-time SVG generator can compile this without linking ImGui.
namespace ramkolfx::gui::logo
{

namespace
{
constexpr float GlyphLeft = (1.0f - GlyphColumns * GlyphCell) * 0.5f;
constexpr float GlyphTop = (1.0f - GlyphRows * GlyphCell) * 0.5f;

std::string HexColor(const ImVec4& Color)
{
    char Buffer[8];
    std::snprintf(Buffer, sizeof(Buffer), "#%02x%02x%02x", static_cast<int>(Color.x * 255.0f + 0.5f),
                  static_cast<int>(Color.y * 255.0f + 0.5f), static_cast<int>(Color.z * 255.0f + 0.5f));
    return Buffer;
}
} // namespace

ArgbImage RenderArgb(int Size)
{
    constexpr int Samples = 4; // per axis, for antialiased edges
    const ImVec4 Start = palette::AccentGradientStartV4;
    const ImVec4 End = palette::AccentGradientEndV4;
    const ImVec4 Ink(InkRgb[0] / 255.0f, InkRgb[1] / 255.0f, InkRgb[2] / 255.0f, 1.0f);

    ArgbImage Image;
    Image.Size = Size;
    Image.Argb.reserve(static_cast<size_t>(Size) * Size * 4);
    for (int Y = 0; Y < Size; ++Y)
    {
        for (int X = 0; X < Size; ++X)
        {
            float R = 0.0f, G = 0.0f, B = 0.0f;
            int Covered = 0;
            for (int SY = 0; SY < Samples; ++SY)
            {
                for (int SX = 0; SX < Samples; ++SX)
                {
                    const float U = (static_cast<float>(X) + (static_cast<float>(SX) + 0.5f) / Samples) / Size;
                    const float V = (static_cast<float>(Y) + (static_cast<float>(SY) + 0.5f) / Samples) / Size;

                    const float DX = std::fmax(std::fmax(CornerRadius - U, U - (1.0f - CornerRadius)), 0.0f);
                    const float DY = std::fmax(std::fmax(CornerRadius - V, V - (1.0f - CornerRadius)), 0.0f);
                    if (DX * DX + DY * DY > CornerRadius * CornerRadius)
                    {
                        continue;
                    }

                    ImVec4 Color(Start.x + (End.x - Start.x) * U, Start.y + (End.y - Start.y) * U,
                                 Start.z + (End.z - Start.z) * U, 1.0f);
                    const int Column = static_cast<int>(std::floor((U - GlyphLeft) / GlyphCell));
                    const int Row = static_cast<int>(std::floor((V - GlyphTop) / GlyphCell));
                    if (Column >= 0 && Column < GlyphColumns && Row >= 0 && Row < GlyphRows &&
                        Glyph[Row][Column] == '#')
                    {
                        Color = Ink;
                    }
                    R += Color.x;
                    G += Color.y;
                    B += Color.z;
                    ++Covered;
                }
            }

            const auto ToByte = [](float Value) { return static_cast<unsigned char>(Value * 255.0f + 0.5f); };
            const float Inv = Covered > 0 ? 1.0f / static_cast<float>(Covered) : 0.0f;
            Image.Argb.push_back(ToByte(static_cast<float>(Covered) / (Samples * Samples)));
            Image.Argb.push_back(ToByte(R * Inv));
            Image.Argb.push_back(ToByte(G * Inv));
            Image.Argb.push_back(ToByte(B * Inv));
        }
    }
    return Image;
}

std::string SvgIcon()
{
    constexpr float Side = 256.0f;
    char Number[32];
    const auto Px = [&](float Fraction) {
        std::snprintf(Number, sizeof(Number), "%g", Fraction * Side);
        return std::string(Number);
    };

    // The glyph as one path of per-row runs, so adjacent cells share edges
    // instead of leaving antialiasing seams between separate shapes.
    std::string GlyphPath;
    for (int Row = 0; Row < GlyphRows; ++Row)
    {
        for (int Column = 0; Column < GlyphColumns;)
        {
            if (Glyph[Row][Column] != '#')
            {
                ++Column;
                continue;
            }
            const int First = Column;
            while (Column < GlyphColumns && Glyph[Row][Column] == '#')
            {
                ++Column;
            }
            GlyphPath += "M" + Px(GlyphLeft + First * GlyphCell) + " " + Px(GlyphTop + Row * GlyphCell) + "h" +
                      Px((Column - First) * GlyphCell) + "v" + Px(GlyphCell) + "h-" +
                      Px((Column - First) * GlyphCell) + "z";
        }
    }

    const ImVec4 Ink(InkRgb[0] / 255.0f, InkRgb[1] / 255.0f, InkRgb[2] / 255.0f, 1.0f);
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"256\" height=\"256\" viewBox=\"0 0 256 256\">\n"
           "  <defs>\n"
           "    <linearGradient id=\"accent\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\">\n"
           "      <stop offset=\"0\" stop-color=\"" +
           HexColor(palette::AccentGradientStartV4) +
           "\"/>\n"
           "      <stop offset=\"1\" stop-color=\"" +
           HexColor(palette::AccentGradientEndV4) +
           "\"/>\n"
           "    </linearGradient>\n"
           "  </defs>\n"
           "  <rect width=\"256\" height=\"256\" rx=\"" +
           Px(CornerRadius) + "\" fill=\"url(#accent)\"/>\n" + "  <path fill=\"" + HexColor(Ink) + "\" d=\"" +
           GlyphPath + "\"/>\n" + "</svg>\n";
}

} // namespace ramkolfx::gui::logo
