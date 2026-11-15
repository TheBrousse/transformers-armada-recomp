#pragma once

// The front end's screen fades sped up (TODO 386, the user's "quick menus" test): menu_fade_speed.cpp.
//
// TF_MENU_FADE_SPEED=<n> (1 today; 2, 4 ...): every screen's fade in and out runs n times as fast. Nothing else of
// the screens changes. Unset or 1: not installed.

class PS2Runtime;

namespace menu_fade_speed
{
    void install(PS2Runtime &runtime);
}
