#pragma once
// A dropped pickup's fall and bounce by the frame's time, not one step a frame (pickup_step.cpp, TODO 369).

class PS2Runtime;

namespace pickup_step
{
    // From applyTransformers.
    void install(PS2Runtime &runtime);
}
