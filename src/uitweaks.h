#pragma once
// Changes to the game's own UI:
//  - Pause menu entry: a copy of one of the pause menu's own buttons, labelled KETAMINE++, that opens this menu. It is
//    added to the panel holding the pause menu's buttons when the menu opens; when the layout isn't what it expects it
//    adds nothing and logs why.
//  - Simple prompts (experimental): interaction prompts lose their artwork and show just the key, as "[F]", in a colour
//    and size you choose. Prompt widgets are found by class name (KetaminePlusPlus.ini [Prompts] Classes), and only
//    prompts that show the key as text are changed, so a key is never hidden. Everything it touches is logged.

namespace ue {
struct UObject;
}

namespace uitweaks {

void Tick(ue::UObject* pc); // game thread, ~60 Hz

} // namespace uitweaks
