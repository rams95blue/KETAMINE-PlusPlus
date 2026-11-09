# KETAMINE++

A trainer for Clive Barker's Hellraiser: Revival, based on Hellraiser Revival's Ketamine Menu 0.4.14 by omxga (MIT). One
`winmm.dll`, no UE4SS, no runtime DLLs. F1 / F8 opens the menu, or the KETAMINE++ entry in the game's pause menu.

## Credits
KETAMINE++ is put together by **rams95** ([github.com/rams95blue](https://github.com/rams95blue)): every mod below
bundled into one native DLL, plus the added features (jump and bunny hop, skins, the pause menu entry, simple prompts,
idle HUD hiding, look limits, speedometer and the rest). It stands on other people's work. Thank you:
- **omxga** (GitHub: literalynn): Hellraiser Revival's Ketamine Menu, the trainer this is built on
  ([GitHub](https://github.com/literalynn/Hellraiser-Revivals-Ketamine-Menu), [Nexus mod 27](https://www.nexusmods.com/clivebarkershellraiserrevival/mods/27)).
- **Reynbow**: TrueFOV, the FOV editor ([GitHub](https://github.com/Reynbow/HellraiserFOV), [Nexus mod 12](https://www.nexusmods.com/clivebarkershellraiserrevival/mods/12)).
- **uwumatic101**: ToolInputFix, KeypadInputFix, SpeedUpInteractions, QuickInteract and CutsceneAutoSkip
  (Nexus mods [42](https://www.nexusmods.com/clivebarkershellraiserrevival/mods/42), [43](https://www.nexusmods.com/clivebarkershellraiserrevival/mods/43),
  [44](https://www.nexusmods.com/clivebarkershellraiserrevival/mods/44), [45](https://www.nexusmods.com/clivebarkershellraiserrevival/mods/45),
  [46](https://www.nexusmods.com/clivebarkershellraiserrevival/mods/46)).
- **Thidrec**: Hellraiser True Minimap ([Nexus mod 39](https://www.nexusmods.com/clivebarkershellraiserrevival/mods/39)).
- Dear ImGui (Omar Cornut, MIT), MinHook (Tsuda Kageyu, BSD-2-Clause), Inter font (Rasmus Andersson, OFL 1.1).
The same list is in the menu (Settings, Credits). Licences: THIRD-PARTY-NOTICES.txt.

## Install
Copy `winmm.dll` (and optionally `KetaminePlusPlus.ini`) into `Hellraiser\Binaries\Win64`. An existing
`KetamineMenu.ini` there is copied to `KetaminePlusPlus.ini` on first run, so your settings carry over.
Remove TrueFOV's `winmm.dll`, and the UE4SS versions of the five fixes below (`ue4ss\Mods\...`), so nothing runs twice.

## What's added to the Ketamine Menu
- **FOV tab**: Reynbow's TrueFOV gameplay-only FOV editor, with world FOV, viewmodel FOV and aiming FOV (Page Up / Page Down, End).
- **Fixes tab**: native ports of uwumatic101's five UE4SS mods, following their main.lua:
  - *Keep movement after tools* (ToolInputFix, mod 42).
  - *Keypad input fix* (KeypadInputFix, mod 43): no lockout after cursor moves, number row / numpad / Backspace press buttons.
  - *Speed up interactions* (SpeedUpInteractions, mod 44): save point x2.5, Lament puzzle x2, reward pillars x3.5,
    cylinder puzzles x2 (input x3), dock boat x3.5. Each has its own slider; 1 turns that one off.
  - *Quick interact* (QuickInteract, mod 45): pickup / ladder hold time, ladder climb speed.
  - *Auto-skip cutscenes* (CutsceneAutoSkip, mod 46).
- **Jump and bunny hop** (Movement tab). Jump height x1.2 by default. Bunny hop uses manual timing by default: press jump
  as you land, up to 0.12 s early or 0.15 s late chains the hop (+10%), within 0.03 s of the landing it's perfect (+20%).
  The velocity vector carries over hop to hop, in every direction (backwards too), and air strafing (strafe + turn the
  mouse) builds speed. No speed cap by default. Auto hop (hold the key) and a hop readout are options. Space is the game's
  Evade key by default: the tab warns you to rebind Evade or pick another jump key.
- **Speedometer and speed units** (Movement tab): an always-on horizontal speed readout under the crosshair (off by
  default), and the unit for it and the hop readout: km/h (default), mph, hammer units/s (Source, 1 unit = 1 inch) or
  Unreal units/s (cm/s).
- **Look straight up and down** (Movement tab, on by default): lifts the game's look limit to the full 90 degrees.
- **Minimap tab**: a native port of Thidrec's True Minimap (Nexus mod 39) without its stutter (see below), with square,
  circle (default), octagon or diamond shapes, rotate with the player (default: the way you face points up) or north-up,
  size, zoom, transparency, placement and offsets.
- **Pause menu entry** (Settings, on by default): a KETAMINE++ line under the game's pause menu items; click it to open
  this menu. It is a plain text item of our own on the pause menu's root canvas, placed below VB_Buttons: nothing is
  added to the game's own list of items, whose navigation code broke when 1.2.x put a copy of an item in it (the
  "can't move, can't look, can't open the menu" bug). Mouse only; the keys and the gamepad go through the game's items.
- **Hide leftover HUD when idle** (Fixes tab, on by default): the game's dynamic HUD hides its player panel when nothing
  happens, but some pieces stay: the "DROP [Z]" hint of breakable melee weapons (W_ToolUseParent_C > SizeBox_DropInput), the
  flashlight toggle (W_EquipmentLoadout_SlotSmall_Flashlight) and the crafting wrench (Overlay_CraftableIcon). Once the
  player panel has been hidden for the delay, the hint fades too, and it comes back with the HUD. 1.3 looked for HUD
  widgets with "HUD" in their class name, and this game has none, so it did nothing. More pieces can be added in
  KetaminePlusPlus.ini, [HudIdle] Leftovers=Class:WidgetName,... The delay is 1 s by default; "Always hide them"
  keeps those pieces hidden all the time, whatever the HUD does.
- **Simple prompts** (World tab, experimental, off by default; 1.4.1: save point prompts too, they are owned by the
  save point actor and were mistaken for the save screen): interaction and key hints lose all their artwork (key
  cap, diamond, pulses, plates, hold rings) and become plain text in the game's font: the key as [F] and its
  description (Barter, Unlock, (Hold) Take) beside it, both in your colour, the key at your size and the description a
  little smaller. The game's own texts are hidden and replaced by ours; what is hidden is re-hidden every frame and made
  transparent as well, so neither the game's animations nor prompts that reset their pieces every frame (the save
  point's) bring the artwork back. Hints inside menus, the map and the shop are left alone, and so are
  prompts showing a gamepad glyph instead of a key letter. Turning it off puts everything back.
- **Dump on-screen UI** (Settings): logs every widget on screen, to help add UI features. The crashes after a dump came
  from reading texts: UE5's FText is 16 bytes and the dump copied 24, overwriting the returned string, which the engine
  then freed. Every text call now takes its size from the function itself. Hidden screens take one line each, so the
  HUD fits in the log too.
- **ESP colours** (ESP tab): a colour for each kind of thing (enemy box, name, distance, health bar, skeleton, head,
  chams, snaplines, off-screen arrows, items, containers, save points, object names, aim circle), saved in
  KetaminePlusPlus.ini [EspColors]. The ESP no longer follows the skin: Win95's black text and navy accent were
  unreadable over the game.
- **Close button**: an X at the top right of the menu closes it (as do F1 / F8).
- **Skinned cursor**: while the menu is open its own cursor is drawn on top of everything: the Windows 9x pixel arrow
  in retro skins, the same arrow in the accent colour in terminal skins, a smooth arrow in the others.
- **Minimap frame**: an outline of the minimap's shape (square, circle, octagon or diamond) in the skin's accent,
  drawn by the overlay. The 1.1 frame was the game's texture, square only, so with the default circle it did nothing.
- **Genesis Configuration** (Combat tab): shows what the loaded save has. "Powers cost nothing" and "No power cooldown"
  start off every time a save loads (they're not restored from the ini); turn them on after loading if you want them.
- **Retro and terminal skins**: Win95, Black Hat and Zero Day are retro Windows skins (square bevelled controls, a
  gradient title bar, Tahoma); Phosphor, Amber CRT, Root Shell, Mainframe, Netrunner and BSOD are terminal skins
  (Consolas, a `root@ketamine:~#` prompt with a blinking cursor, `>` section headers). The fonts come from Windows
  itself; if one is missing the skin uses Inter.
- **Skins** (Settings tab): Win95 (the default, at full opacity), KETAMINE++ (cyan), KETAMINE (the original violet), Lament, Cenobite, Leviathan, Toxic, Frost, Ember, Mono, or Custom with your own
  accent and highlight colors, plus window opacity.

## Defaults
F1 and F8 open the menu, 100% UI scale, Win95 skin at full opacity. On: jump (x1.2) and bunny hop with manual timing (auto hop off), skip intros,
FOV editor (world 120, viewmodel 110, aiming 90, sprint / ladder effects kept), every fix except cutscene skip, look
straight up and down, idle HUD hiding, pause menu entry. Off: hop readout, aim FOV circle, cutscene skip, minimap
(circle, x1.6 zoom, rotating with you so your forward points up), simple prompts. Settings, Reset configuration
returns to these.

## Performance notes
- Keypad input fix lag: the fix asked for the keypad class 60 times a second, and while that class isn't loaded (most of
  the game) every miss scanned the whole object list. Class and object lookups now remember a miss for 2 s, which fixes
  this everywhere the pattern occurs.
- True Minimap's stutter came from its Lua: 9 full object scans for menu screens every 250 ms (~36 a second), a forced
  map rebuild and full widget-tree copy every 5 s, the 3840x2160 map moved by re-layout 60 times a second, and every
  marker re-written every second. The port does one scan every 2 s (class pointer compares), refreshes only on zone or
  floor changes (20 s fallback), moves the map by render translation (paint only), and writes markers only when changed.
- UI scale below 100% hid the mouse cursor (ImGui rounds the cursor scale down to 0): fixed.
- Hop readout: the overlay only draws while something needs it; the readout now counts, so it shows over the game and
  not just while the menu is open.

## Minimap (1.2.2)
The minimap is back to the 1.1.0 code. The 1.2.x additions (compass letters, north offset, the refresh guards and focus
handling) are gone. One change on top of 1.1.0: rotate with the player is on by default, and it turns the map by your
heading projected onto the map exactly as positions are, so your first-person forward always points straight up.

## Speed up interactions and the script tables
The save point and Lament puzzle are paced by timers and waits as much as by their animations. Changing those needs the
game's script opcode table (GNatives); on the last tested build it wasn't found, so only the animations sped up and the
sequence got out of step (the "same animation sped up, a bit buggy" report). The cause: UE5's table has 255 entries
(EX_Max is 0xFF), and the search checked entry 0xFF, which lies past its end. 1.2.1 checks 0x00..0xFE, scans sections
that are both writable and executable too, and falls back to finding the table from the code that dispatches
through it. If it still isn't found, the log lists the executable's sections. Without the table, the save point and Lament puzzle stay at
normal speed rather than going out of step; pillars, cylinders and the boat don't depend on it.

## Audit notes (why the first flashlight fix froze the game, and what changed)
- The freeze: the first port of ToolInputFix *skipped* `CoreController.BP_FlushPressedKeys`. That function is native and
  is called from Blueprint, where a native function itself steps the script VM past its call. Skipped, the VM was left
  pointing at the call's own bytecode, so the game froze. Now, as the original mod does, the flush always runs, and
  `PlayerInput` is cleared only for that one call (the flush null-checks it), armed by `GA_Flashlight_C` /
  `GA_UseToolMedKit_C` `K2_OnEndAbility`.
- Every hook now goes through one layer (`src/hooks.cpp`) that **always runs the original function**. The only hooks that
  change arguments (timer, Delay and MoveComponentTo, for the speed-ups) evaluate the arguments themselves, check that
  the bytecode lands exactly on the end-of-parameters marker (else they undo and pass the call through untouched), step
  past it, and make the call through ProcessEvent. They only do that while a save / puzzle sequence is running.
- Startup self-checks: the script frame layout is measured on a harmless engine function and the VM's opcode table is
  verified before any argument edit is enabled. If either fails, those hooks stay off and the log says so; the fixes
  that don't need them keep working.
- Hooks follow level changes: a Blueprint class that unloads and reloads is hooked again, and a hook record is only
  trusted while the function still runs the hook's code.
- KeypadInputFix: the original binds the number keys globally, scans every keypad on each key press and polls from a
  separate 8 ms loop. Here the number keys only act while a keypad is open (found through the player state), and the
  poll runs inside the game's own frame.
- Not testable here: everything was built and checked under Wine without the game (DLL loads, winmm forwarding, every
  menu tab draws). The first in-game run writes the hook checks to `KetaminePlusPlus.log`.

## Building
- Windows / MSVC: `cmake -S . -B build -A x64 && cmake --build build --config Release`.
- Linux cross-build (how the included DLL was built): `./build-mingw.sh` with clang 18, lld, mingw-w64 (posix) and
  ImGui v1.92.9 / MinHook v1.3.4 sources in `deps/`.
