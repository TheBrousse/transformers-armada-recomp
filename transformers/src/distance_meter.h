#pragma once
// The distance meter: the distance to what the crosshair points at, above it (distance_meter.cpp).
// A cut feature of the prototype's: off by default; F1 > Mods & cheats switches it (TF_DISTANCE_METER,
// kept in settings.ini - TODO 74).

#include <cstdint>

class PS2Runtime;

namespace distance_meter
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
    // Its setting (section "Prototype": the overlay's Mods & cheats page).
    void addSettings();
    bool enabled();
    void setEnabled(bool on);
    // The distance to show now (the window thread), or false while there is none to show.
    bool distance(PS2Runtime &runtime, float &metres);
    // The crosshair (taModelPredictionCrosshair) being updated now, or 0 (no level, a menu or a movie).
    uint32_t crosshairObject(PS2Runtime &runtime);
    // An in-game menu (pause, its screens, the Mini-Con / Data-Con pickup menus) is on the screen.
    bool menuOpen(PS2Runtime &runtime);
}
