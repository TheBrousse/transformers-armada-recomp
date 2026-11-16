#pragma once

// The front end's screen fades sped up (TODO 386, the user's "quick menus"): menu_fade_speed.cpp.
//
// F1 > Gameplay > Menu speed: Original (the console's speed, the default) / Double / Four times - every screen's fade
// in and out runs that many times as fast, live. Nothing else of the screens changes. TF_MENU_FADE_SPEED=<n> (1, 2, 4;
// any up to 16) is the row's variable: set before the game starts it overrides the row (settings.ini's value).

class PS2Runtime;

namespace menu_fade_speed
{
    void addSettings();                 // the F1 row (before the window loop starts)
    void install(PS2Runtime &runtime);  // the base-update hook (always; at Original it writes the game's own rates)
    void stateLoaded();                 // a save state loaded: its screens' rates are taken as made at this speed
}
