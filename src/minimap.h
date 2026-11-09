#pragma once
// Minimap, ported from Hellraiser True Minimap v0.12 (UE4SS Lua, Nexus Mods 39): a copy of the game's own TAB map widget,
// clipped to a small window at the top right and centred on the player. Optimised where the Lua mod was slow (see
// minimap.cpp), with map shapes (square, circle, octagon, diamond) and an option to rotate the map with the player.
// Settings live in feat::g_on (Minimap, MinimapRotate, MinimapAlways, MinimapScreenEdge, MinimapFrame) and
// feat::g_value (MinimapSize, MinimapZoom, MinimapOpacity, MinimapShape, MinimapOffsetX, MinimapOffsetY).

namespace ue {
struct UObject;
}

namespace minimap {

enum Shape { Square, Circle, Octagon, Diamond, ShapeCount };
extern const char* const kShapeNames[ShapeCount];

void Tick(ue::UObject* pc, ue::UObject* player);     // game thread, ~60 Hz: status (4 Hz), refreshes, markers
void Frame(ue::UObject* pc, ue::UObject* player);    // game thread, once per engine frame: follows the player
const char* Status();                                 // any thread: what the minimap is doing, for the menu
// The minimap on screen in pixels and its shape, while it shows and its frame is on (the overlay draws the frame).
bool FrameRect(float& x, float& y, float& size, int& shape);

} // namespace minimap
