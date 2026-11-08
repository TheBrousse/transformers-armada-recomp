#pragma once
// A debugging aid for the game's sound voices' pitch (TF_PITCH_LOG=1): sound_trace.cpp.

class PS2Runtime;

namespace sound_trace
{
    // From applyTransformers: hooks the voices' pitch calls when TF_PITCH_LOG=1.
    void install(PS2Runtime &runtime);
}
