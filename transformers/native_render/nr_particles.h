// The particle pipelines (vlxParticleManager's "vlxPPP_*": smoke, sparks, ribbons...) run on the renderer's thread
// (TODO 144, nr_particles.cpp).
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace nr
{
    namespace particles
    {
        // At start-up (game thread): the GS object the VU1 engine wants a reference to, and the switch.
        void install(PS2Runtime &runtime, const uint8_t *rdram);
        // TF_NATIVE_PARTICLEFX=0: off (the particle pipelines not drawn, as before).
        bool enabled();
        // (Debug: the frame the renderer's thread expands now, for TF_NATIVE_PARTICLELOG's <from tick>.)
        void noteTick(uint64_t tick);
        // Whether the program starting at `start` in the 16 KB of microcode memory `micro` is one of the particle
        // pipelines' images (as vlMicroManager uploaded it there).
        bool isParticleProgram(const uint8_t *micro, uint32_t start);
        // Runs a program on the tracker's own VU1 memory (renderer's thread): `start` its address for MSCAL / MSCALF,
        // ~0u for MSCNT (where the program before ended). Its XGKICK packets appended to `packets` (each packet's bytes
        // in turn, whole qwords).
        void run(uint8_t *data, const uint8_t *micro, uint32_t start, uint32_t top, uint32_t itop, std::vector<uint8_t> &packets);
    }
}
