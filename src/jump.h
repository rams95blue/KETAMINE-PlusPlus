#pragma once
// Jump (the game has no jump key) with bunny hopping:
//  - manual timing (default): press jump as you land. Up to 120 ms early or 150 ms late chains the hop (+gain), within
//    30 ms of the landing it's perfect (+2x gain); later than that the chain is lost;
//  - auto hop (option): hold the jump key to hop on every landing (+gain, never perfect);
//  - momentum: the velocity vector carries through from hop to hop, and air strafing (strafe + turn) adds speed;
//  - no speed cap by default.
// Settings: feat toggles Jump / Bhop / BhopAuto / BhopHud, values JumpMult, BhopGain, BhopCap (0 = unlimited),
// BhopAirStrafe; the jump key lives in the menu.
#include <atomic>
#include <string>

namespace ue {
struct UObject;
}

namespace jump {

extern std::atomic<unsigned> g_key; // virtual-key code, 0 = none (default Space)

void Frame(ue::UObject* player, double nowSeconds); // game thread, once per engine frame (player may be null)
void OnKey(unsigned vk); // window thread: every key press, for precise jump timing (never consumes the key)

// The last hop, for the on-screen readout.
struct HudInfo {
    const char* grade = ""; // "PERFECT", "GOOD", "LATE" or ""
    int gain = 0;           // percent
    double speed = 0;       // horizontal, cm/s
    int hops = 0;           // chained so far
    double at = -10;        // PreciseSeconds of the hop
};
HudInfo Hud();
// The player's horizontal speed in cm/s (Unreal units per second) this frame, and whether there is a player.
double LiveSpeed(bool* valid = nullptr);

// The game's own actions on the jump key ("Dodge" for Space by default), from its input mapping contexts and legacy
// input settings: empty when the key is free (or not checked yet). Any thread.
std::string Conflict();
std::string KeyLabel(unsigned vk); // Unreal's name for a virtual key ("SpaceBar"), empty if unknown

} // namespace jump
