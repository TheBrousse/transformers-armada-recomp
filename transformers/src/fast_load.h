#pragma once
// Fast loading (fast_load.cpp): loading screens run at the PC's full speed instead of the console's.
// TF_FAST_LOAD=0 turns it off; F5 Gameplay "Fast loading".

#include <cstdint>

class PS2Runtime;

namespace fast_load
{
    void addSettings();
    // Once a game frame (overrides.cpp). `inHq`: the HQ's executor (taUIPreGame) runs - never a
    // level's load, whatever the two signals say.
    void frame(uint8_t *rdram, PS2Runtime &runtime, bool inHq);
}
