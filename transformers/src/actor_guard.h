#pragma once
// Freed actors kept by creatures across a level's end (actor_guard.cpp, docs/TODO.md item 51):
// the creatures let go of them when the pool manager deletes the pools (the source fix), and a
// guard on taActorEvenSimpler's child walk stays as the safety net.
// TF_ACTOR_GUARD=0 turns both off; TF_ACTOR_GUARD=log only reports what the fix would do.

class PS2Runtime;

namespace actor_guard
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
}
