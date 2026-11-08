#pragma once
// A small crosshair (small_crosshair.cpp): the game's big 3D crosshair hidden and a small cross drawn
// where it aims, outlined in the Mini-Con weapon's colour while one is equipped (the in-range colour
// when it can fire, as the original's add-on). TF_SMALL_CROSSHAIR=0 / F1 Display "Crosshair".

class PS2Runtime;

namespace small_crosshair
{
    void addSettings();
    bool enabled();
    // Each rendered frame on the window thread, over the picture (mod_menu::draw).
    void draw(PS2Runtime &runtime);
}
