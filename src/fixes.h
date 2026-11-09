#pragma once
// Game fixes, ported from the UE4SS Lua mods on Nexus Mods (no UE4SS needed). Credits in THIRD-PARTY-NOTICES.txt.
//  - Tool input fix (ToolInputFix, mod 42): held movement keys keep working when the flashlight or a heal ends.
//  - Keypad input fix (KeypadInputFix, mod 43): no input lockout on keypads, number keys press the buttons.
//  - Speed up interactions (SpeedUpInteractions, mod 44): save point, Lament puzzle, dock boat, cylinder puzzles and
//    reward pillars play faster, every part of each sequence by the same factor.
//  - Quick interact (QuickInteract, mod 45): faster pickup / ladder holds and ladder climbs.
//  - Cutscene auto-skip (CutsceneAutoSkip, mod 46): skips every skippable cutscene through the game's own skip.
#include <cstdint>
#include <string>

namespace ue {
struct UObject;
}

namespace fixes {

enum Why { WhyTool, WhyKeypad, WhySpeed, WhyQuick, WhyCut, WhyCount };

// Game thread, from the feature tick (~60 Hz). player may be null (title screen, loading). `why` gets the reason a
// fix can't work (empty = fine).
void Tick(ue::UObject* pc, ue::UObject* player, uint64_t nowMs, std::string why[WhyCount]);
// Game thread, once per engine frame: the keypad axis release is timed in milliseconds.
void Frame(ue::UObject* pc, double nowSeconds);
// Speed-ups of timers and waits need the script VM's GNatives table; without it only animations are sped up.
bool EditsAvailable();
void DumpUi(); // any thread: logs every in-viewport widget tree (queued to the game thread)
// Window thread: number row, numpad and Backspace while a keypad is open. True = handled (not passed to the game).
bool OnKey(unsigned vk);

} // namespace fixes
