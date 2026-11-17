#pragma once
// The game's kill list kept to the episode it was made in (kill_list.cpp, docs/TODO.md item 412).
// TF_KILL_LIST=0: the original game's behaviour (one list, applied by index to whichever unfinished episode loads).

#include <cstdint>

class PS2Runtime;

namespace kill_list
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
    // Once a game frame: a list changed without us (the card or a save state loaded) is given its episode at once,
    // from the progress record's last warp gate used - before the HQ's Drop Zones write the next episode there.
    void frame(uint8_t *rdram);
}
