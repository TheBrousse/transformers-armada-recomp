#pragma once
// The ragdolls stepped at the console's 1/30 s at any frame rate (ragdoll_step.cpp, TODO 182 / 172 / 183).

class PS2Runtime;

namespace ragdoll_step
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
}
