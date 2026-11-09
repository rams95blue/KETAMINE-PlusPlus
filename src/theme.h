#pragma once
// Palette shared by the menu and the ESP, set by the selected skin. KETAMINE++ (the default) is near-black with a cyan
// #1EF8FF accent and #ECECEC highlights; KETAMINE is the original Ketamine Menu look (violet #AC6EE2, pink #F89FCD). `Violet` is the accent and `Pink` the
// highlight whatever the skin.
#include <imgui.h>

namespace theme {

inline ImVec4 Bg{0.043f, 0.043f, 0.059f, 1.f};      // window
inline ImVec4 Panel{0.071f, 0.071f, 0.094f, 1.f};   // panels, sidebar
inline ImVec4 Panel2{0.106f, 0.106f, 0.137f, 1.f};  // controls
inline ImVec4 Border{0.149f, 0.149f, 0.192f, 1.f};
inline ImVec4 Text{0.949f, 0.949f, 0.965f, 1.f};
inline ImVec4 Muted{0.557f, 0.557f, 0.604f, 1.f};
inline ImVec4 Violet{0.118f, 0.973f, 1.f, 1.f};    // accent
inline ImVec4 Pink{0.925f, 0.925f, 0.925f, 1.f};    // highlight
inline const ImVec4 Black{0.f, 0.f, 0.f, 1.f};
inline float Opacity = 1.f; // window background opacity
// Look of the selected skin: 0 modern (Inter, rounded), 1 retro Windows (Tahoma, square, bevelled frames, a title bar),
// 2 terminal (a monospace font, square, thin lines).
enum { StyleModern, StyleRetro, StyleTerminal };
inline int Style = StyleModern;
inline float R(float rounding) { return Style == StyleModern ? rounding : 0.f; } // corner rounding for the current look

inline ImFont* regular; // Inter Regular
inline ImFont* bold;    // Inter SemiBold

inline ImVec4 Hex(unsigned rgb) { return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1.f}; }
inline unsigned ToHex(ImVec4 c)
{
    auto b = [](float v) { return unsigned(v < 0 ? 0 : v > 1 ? 255 : v * 255.f + 0.5f); };
    return (b(c.x) << 16) | (b(c.y) << 8) | b(c.z);
}

struct Skin {
    const char* name;
    unsigned bg, panel, panel2, border, text, muted, accent, highlight;
    int style = StyleModern;
};
// The last entry is Custom: the KETAMINE surfaces with your own accent and highlight.
inline const Skin kSkins[] = {
    {"KETAMINE++", 0x0B0B0F, 0x121218, 0x1B1B23, 0x262631, 0xF2F2F6, 0x8E8E9A, 0x1EF8FF, 0xECECEC},
    {"KETAMINE", 0x0B0B0F, 0x121218, 0x1B1B23, 0x262631, 0xF2F2F6, 0x8E8E9A, 0xAC6EE2, 0xF89FCD},
    {"Lament", 0x0D0B08, 0x15120D, 0x201B13, 0x2E271B, 0xF4EFE4, 0x9A907C, 0xC9A227, 0xF0D98A},
    {"Cenobite", 0x0C0708, 0x150C0E, 0x1F1315, 0x2E1B1E, 0xF5ECEC, 0x9C8587, 0xC0182F, 0xFF6B78},
    {"Leviathan", 0x070B0C, 0x0D1416, 0x141E21, 0x1F2D31, 0xEAF4F3, 0x84999A, 0x2BB5A6, 0x8EF0E2},
    {"Toxic", 0x080B07, 0x0F150D, 0x172014, 0x232F1E, 0xEEF5EA, 0x8C9A86, 0x5BD65B, 0xC6FF7A},
    {"Frost", 0x080A0F, 0x0E1219, 0x161C26, 0x222A38, 0xEDF2FA, 0x8691A5, 0x4F8DF7, 0xA9C8FF},
    {"Ember", 0x0D0907, 0x16100C, 0x211812, 0x30231A, 0xF7EFE9, 0xA0907F, 0xF2742B, 0xFFC27A},
    {"Mono", 0x0A0A0A, 0x121212, 0x1B1B1B, 0x2A2A2A, 0xF2F2F2, 0x8C8C8C, 0xC8C8C8, 0xFFFFFF},
    // Retro Windows: silver bevels, a gradient title bar (accent to highlight), Tahoma.
    {"Win95", 0xC0C0C0, 0xFFFFFF, 0xD4D0C8, 0xFFFFFF, 0x000000, 0x505050, 0x000080, 0x1084D0, StyleRetro},
    {"Black Hat", 0x2B2B2B, 0x1C1C1C, 0x3A3A3A, 0x7A7A7A, 0xEAEAEA, 0x9A9A9A, 0x7A0000, 0xFF3B3B, StyleRetro},
    {"Zero Day", 0x0E140E, 0x080C08, 0x1C281C, 0x5A7A5A, 0xD8FFD8, 0x88A888, 0x0A5A0A, 0x39FF14, StyleRetro},
    // Terminal: monospace, square, scanline-dark backgrounds.
    {"Phosphor", 0x020A04, 0x06140A, 0x0B2012, 0x14532A, 0x7CFF9A, 0x3C8F55, 0x33FF66, 0xB6FFC8, StyleTerminal},
    {"Amber CRT", 0x0A0600, 0x140C02, 0x221604, 0x5A3A08, 0xFFC452, 0x9C7428, 0xFFB000, 0xFFE08A, StyleTerminal},
    {"Root Shell", 0x0A0204, 0x140508, 0x220A0E, 0x5A1520, 0xFFD6DC, 0xA05A66, 0xFF2E4D, 0xFF9AAA, StyleTerminal},
    {"Mainframe", 0x020814, 0x061024, 0x0A1A36, 0x16406E, 0xCFEFFF, 0x5F87A8, 0x00E5FF, 0x9AF6FF, StyleTerminal},
    {"Netrunner", 0x0B0312, 0x14061F, 0x200B30, 0x4A1A66, 0xF3E6FF, 0x9A79B5, 0xFF2BD6, 0x2BF0FF, StyleTerminal},
    {"BSOD", 0x0000AA, 0x00008A, 0x1A1AC8, 0xAAAAAA, 0xFFFFFF, 0xC0C0FF, 0xFFFFFF, 0x55FFFF, StyleTerminal},
    {"Custom", 0x0B0B0F, 0x121218, 0x1B1B23, 0x262631, 0xF2F2F6, 0x8E8E9A, 0x1EF8FF, 0xECECEC},
};
inline constexpr int kSkinCount = int(sizeof(kSkins) / sizeof(kSkins[0]));
inline constexpr int kCustomSkin = kSkinCount - 1;

// Sets the palette. Custom keeps the given accent / highlight.
inline void ApplySkin(int index, unsigned customAccent, unsigned customHighlight)
{
    const Skin& s = kSkins[index < 0 || index >= kSkinCount ? 0 : index];
    Bg = Hex(s.bg), Panel = Hex(s.panel), Panel2 = Hex(s.panel2), Border = Hex(s.border);
    Text = Hex(s.text), Muted = Hex(s.muted);
    bool custom = index == kCustomSkin;
    Violet = Hex(custom ? customAccent : s.accent);
    Pink = Hex(custom ? customHighlight : s.highlight);
    Style = s.style;
}

inline ImVec4 Alpha(ImVec4 c, float a) { return {c.x, c.y, c.z, c.w * a}; }
inline ImVec4 Mix(ImVec4 a, ImVec4 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}; }
inline ImU32 U32(ImVec4 c) { return ImGui::GetColorU32(c); } // honours BeginDisabled's fade
inline ImU32 Raw(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

} // namespace theme
