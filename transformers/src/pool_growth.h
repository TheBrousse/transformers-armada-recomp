#pragma once
// N7 step 3 (docs/N7_PLAN.md): the game's look-and-sound pools, sized for the PS2's 32 MB, made bigger with the
// 256 MB RAM (pool_growth.cpp). Only the counts change: the game makes and links every entry itself, from its own
// heap. With TF_RAM_MB=32 nothing changes (the original constants).
//
//   TF_POOL_GROW=<n>  the factor, 1-16 (16 by default - the census: 0 refused on every level at 16, not at 4; 1 = the game's)

#include <cstdint>

class PS2Runtime;

namespace pool_growth
{
    // (The one-shot sound player's emitters: the console's 35, raised by instruction patches -
    // transformers/config/overrides.toml, "(N7 step 3) The one-shot sound player's emitters"; both layouts.
    // Must be the patches' number.)
    constexpr unsigned kSoundEmitters = 256u;
    // (N7 step 4, with the 256 MB RAM) The display lists' blocks: the game's 75 (125 on episode type 7) times this;
    // the creature lists: this many items and entries each (the game's 500).
    constexpr unsigned kDisplayBlocksFactor = 8u;
    constexpr unsigned kCreatureListItems = 4000u;
    // At start (overrides.cpp, with the other installs): the pools' wrappers, when the layout is above 32 MB.
    void install(PS2Runtime &runtime);
    // Once a game frame (overrides.cpp): the creature lists grown when the game has made them anew; the actor budget
    // set to the enemy waves' choice.
    void frame(uint8_t *rdram);
    // (N7 step 5) Enemy waves (enemy_waves.cpp, an extension: TF_ENEMY_WAVES): false = the PS2's pace (the game's 1 MB budget for its
    // actors), true = unlimited (with the 256 MB RAM only; refused, and logged, with TF_RAM_MB=32).
    void setUnlimitedWaves(bool on);
    bool unlimitedWaves();
    // The factor in force (1 when off).
    int factor();
}
