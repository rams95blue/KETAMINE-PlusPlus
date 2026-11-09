#pragma once

namespace esp {

// Draws the ESP on the background draw list (behind the menu). Render thread, inside an ImGui frame.
void Draw();

// ESP colours by type (KetaminePlusPlus.ini [EspColors], #RRGGBB), independent of the menu skin.
enum Color {
    Box, Name, Distance, Health, Skeleton, Head, Chams, Snaplines, Offscreen, Items, Containers, SavePoints, Labels,
    AimCircle, ColorCount
};
struct ColorInfo {
    const char* key;   // ini key
    const char* label; // menu
    unsigned def;      // 0xRRGGBB
};
extern const ColorInfo kColors[ColorCount];
unsigned GetColor(int i); // 0xRRGGBB
void SetColor(int i, unsigned rgb);
void ResetColors();

} // namespace esp
