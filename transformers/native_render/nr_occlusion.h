// The sun's visibility test answered from the native renderer's own depth (roadmap N3a, nr_occlusion.cpp).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace nr
{
    namespace occlusion
    {
        // The wrappers on the game's test and read-back (game thread, at start-up).
        void install(PS2Runtime &runtime);
        // Render thread: the rect (GS pixels of the 3D target: x0, y0, x1, y1) the test copied in the frame drawn now -
        // the newest copy made after the frame drawn before (`tick`: Frame::tick). False: none (the sun not on screen),
        // or the native answer off.
        bool probeFor(uint64_t tick, float rect[4]);
        // Render thread, once the GPU has drawn that frame: its 64 x 64 depth over the rect (the native depth, GS Z * 16 /
        // 2^32, row by row).
        void deliver(uint64_t tick, const float *depth);
    }
    // (Debug, game thread: a native shot of the frame begun at or after `tick` - nr_capture.cpp, as TF_NATIVE_SHOT's.)
    void requestShot(uint64_t tick);
}
