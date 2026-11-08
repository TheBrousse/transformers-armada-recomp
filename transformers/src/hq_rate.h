#pragma once
// The HQ at the PC port's frame rate (hq_rate.cpp; TODO 56).
//
// The HQ (the executor taUIPreGame, vtable 0x6B7360) runs at the console's 30 frames by default.
// Its update (0x333630) hands most of its systems the frame's seconds (FramePeriod), but a few of
// its pieces move a fixed step every frame; at a raised rate those ran fast. This module holds what
// the HQ needs to run at the levels' rate, and its testing aids.

#include <cstdint>

class PS2Runtime;

namespace hq_rate
{
    // Once a game frame (overrides.cpp, the frame's start), with whether the HQ runs and the rate it
    // runs at (0: the console's 30).
    void frame(uint8_t *rdram, PS2Runtime &runtime, bool inHq, int rate);
}
