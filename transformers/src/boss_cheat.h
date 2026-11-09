#pragma once
// One-hit kills and the bosses (boss_cheat.cpp, TODO 199): the pause-menu cheat leaves a boss's fight to play out, so
// the level can end. A player option (F1 > Mods & cheats, TF_ONEHIT_BOSSES, kept in settings.ini).

class PS2Runtime;

namespace boss_cheat
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
    // Its setting (section "Cheats": the Mods & cheats page).
    void addSettings();
}
