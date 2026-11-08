#pragma once
// TODO 94: the scratchpad accessed inline in the game's functions that use it most.
//
// The translated code keeps only main RAM's usual case inline in each load and store; every other address - the
// scratchpad, I/O, VU memory - takes one shared out-of-line function (FPS round 5: the code a third the size, and
// faster overall). But a few functions read and write their matrices and vectors in the scratchpad through pointers
// (not a constant address, which is inline already): Deep Amazon makes ~20 million such accesses a second, a third
// of them in one function (0x1B0D50, a matrix by matrix), each a call. For the functions listed in
// transformers\config\scratchpad_hot.txt, sync_generated.py puts scratchpad_hot_begin.inc / _end.inc around the
// function's file in its batch: inside, a load or store tests the scratchpad inline after main RAM - the same test
// and the same bytes the out-of-line function would read or write (Ps2ScratchpadInlineRead/Write, then the slow way
// for anything else), so the game is unchanged; only where the test is made differs.

#include "ps2_runtime_macros.h"

#if PS2X_MEMORY_COLD_INLINE
// (every rare path inline already: the plain loads)
#define TF_SPR_HOT_READ(bits, type) static PS2_FORCEINLINE type TfSprHotRead##bits(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr) { return Ps2Read##bits(rdram, ctx, runtime, addr); }
#else
#define TF_SPR_HOT_READ(bits, type)                                                                            \
    static PS2_FORCEINLINE type TfSprHotRead##bits(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,     \
                                                   uint32_t addr)                                              \
    {                                                                                                          \
        type value;                                                                                            \
        const uint32_t offset = PS2_RAM_INLINE_OFFSET(addr);                                                   \
        if (offset <= PS2_RAM_SIZE - sizeof(type)) [[likely]]                                                  \
        {                                                                                                      \
            std::memcpy(&value, rdram + offset, sizeof(type));                                                 \
            return value;                                                                                      \
        }                                                                                                      \
        if (Ps2ScratchpadInlineRead(addr, value))                                                              \
            return value;                                                                                      \
        return Ps2ReadSlow##bits(rdram, ctx, runtime, addr);                                                   \
    }
#endif
TF_SPR_HOT_READ(8, uint8_t)
TF_SPR_HOT_READ(16, uint16_t)
TF_SPR_HOT_READ(32, uint32_t)
TF_SPR_HOT_READ(64, uint64_t)
TF_SPR_HOT_READ(128, __m128i)
#undef TF_SPR_HOT_READ
