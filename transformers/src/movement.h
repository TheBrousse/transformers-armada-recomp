#pragma once
// The player's movement on a modern controller (movement.cpp, docs/TODO.md item 41): the run
// starts when the stick is pushed all the way as a modern stick reaches it.
// TF_RUN_THRESHOLD=<0..1> sets the move length past which it runs (0.99 = the PS2's).

class PS2Runtime;

namespace movement
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
}
