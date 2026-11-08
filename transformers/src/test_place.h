#pragma once
// A testing aid (test_place.cpp): TF_PLACE="tick:x,y,z[,heading];..." puts the player there (game
// units) through the game's own SetPosition at the first frame at or after the tick (TF_INPUT's: 60 a second of game time at every frame rate), and,
// with a heading (degrees, 0 = +x, counter-clockwise), turns the view that way. Unlike TF_WARP's
// write into the position, which the game may overwrite, this sticks. A tick written "+N" counts N
// ticks from the placement before it (the first such: from the level's first update), so a script
// moves the player at the same moments of the level whatever the load took. Applied in order.

class PS2Runtime;

namespace test_place
{
    // From applyTransformers (it adds guest code: after jet_bike's, before the self-test).
    void install(PS2Runtime &runtime);
}
