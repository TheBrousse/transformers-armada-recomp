#pragma once
#include <cstddef>
#include <cstdint>
#include "nr_types.h"

namespace nr
{
    using FrameTexturePolicy = bool (*)(const uint8_t *, size_t);
    // A missing or failed ownership proof always retains the renderer's default.
    inline bool sampleFrameTexturePolicy(FrameTexturePolicy policy,
                                         const uint8_t *ram, size_t bytes) noexcept
    {
        if (!policy || !ram || !bytes)
            return false;
        try { return policy(ram, bytes); }
        catch (...) { return false; }
    }
    inline void resetFrameTexturePolicySnapshot(Frame &frame) noexcept
    {
        frame.synchronousGameTextures = false;
    }
    inline void captureFrameTexturePolicy(Frame &frame, FrameTexturePolicy policy,
                                         const uint8_t *ram, size_t bytes) noexcept
    {
        frame.synchronousGameTextures = sampleFrameTexturePolicy(policy, ram, bytes);
    }
}
