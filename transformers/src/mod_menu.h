#pragma once
// The mod menu (F1): the home of every mod, over the game.
//
// - Mods: the folders in Mods\ (mod_loader.h), each with its switch (in effect at the next
//   start) and what this start applied.
// - Cheats: the game's 32 button codes (docs/CHEATS.md of the mods page). "Enter" types the
//   code for you: the game reads it as pad presses and runs its own cheat, so open the screen
//   the code belongs to (the difficulty screen, Extras, the HQ or the pause menu) first.
// - Game settings: the game's own hidden config settings (game_config.h), as in F5.
//
// Ctrl+F1 opens the runtime's debugger, which F1 opened before. The menu needs the debug UI
// build (PS2X_ENABLE_DEBUG_UI, on by default). TF_MOD_MENU=1 opens it from the start.

#include <cstdint>

class PS2Runtime;

namespace mod_menu
{
    // From the debug UI callbacks (main.cpp): after the debugger's initialize, each frame.
    // Its own settings (before the window loop starts): driving with W / S.
    void addSettings();
    void draw(PS2Runtime &runtime);
    bool isOpen();
    // Once a rendered frame, with the pad override (overrides.cpp): the buttons of a code being
    // entered, as TF_INPUT's (1 << n, see padButton there), 0 when none.
    uint16_t padButtons(uint64_t tick);
    // Once a game frame (overrides.cpp): what the menu asked to change in the game's memory (the
    // Levels tab), and what it shows of it.
    void frame(uint8_t *rdram);
    // At start (overrides.cpp): the pause screen's input watched (the menu keys: Escape resumes
    // at its top, goes back in its pages).
    void install(PS2Runtime &runtime);
}
