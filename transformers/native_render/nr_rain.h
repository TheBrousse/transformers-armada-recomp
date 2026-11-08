// The rain drawn by the native renderer (TODO 146: Mid-Atlantic after the boss had no rain - nr_rain.cpp).
#pragma once

#include <cstdint>
#include <vector>

namespace nr
{
    namespace rain
    {
        // At start-up (game thread): the rain image's head read from the ELF in guest memory, and the switch
        // (TF_NATIVE_RAIN=0: not drawn, as before).
        void install(const uint8_t *rdram);
        // Whether the batch VU1 would run now (the routine table in `vu`, as the core's dispatcher reads it) is the
        // rain's: a table of one routine whose code in `micro` is the rain image.
        bool isRainBatch(const uint32_t (*vu)[4], const uint8_t *micro);
        // The batch's packets as VU1 would send them (GIF packets, whole qwords), appended to `packets`, from the
        // tracker's VU1 data memory `vu` with the batch at `itop`.
        void build(const uint32_t (*vu)[4], uint32_t itop, std::vector<uint8_t> &packets, uint32_t start = ~0u);
        // (The frame the renderer's thread expands now, for TF_NATIVE_RAINLOG=<tick>: that frame's rain batches logged.)
        void noteTick(uint64_t tick);
        // Whether the rain is drawn (TF_NATIVE_RAIN not 0): the capture then copies its blocks (nr_capture.cpp).
        bool enabled();
        // The frame's copies of the rain's blocks (Frame::rainBlocks), one per pass in order, for the replay that follows;
        // a rain MSCAL (a pass's start) moves on to the next one. Renderer's thread.
        void beginFrame(const std::vector<std::vector<uint32_t>> *blocks);
        void nextPass();
    }
}
