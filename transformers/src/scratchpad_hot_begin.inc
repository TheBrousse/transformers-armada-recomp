// (TODO 94, scratchpad_hot.h) Included by a batch before a function of transformers\config\scratchpad_hot.txt: its
// loads and stores test the scratchpad inline after main RAM, until scratchpad_hot_end.inc.
#include "scratchpad_hot.h"
#pragma push_macro("READ8")
#pragma push_macro("READ16")
#pragma push_macro("READ32")
#pragma push_macro("READ64")
#pragma push_macro("READ128")
#pragma push_macro("PS2_SPR_TRY_WRITE")
#undef READ8
#undef READ16
#undef READ32
#undef READ64
#undef READ128
#undef PS2_SPR_TRY_WRITE
#define READ8(addr) TfSprHotRead8(rdram, ctx, runtime, (uint32_t)(addr))
#define READ16(addr) TfSprHotRead16(rdram, ctx, runtime, (uint32_t)(addr))
#define READ32(addr) TfSprHotRead32(rdram, ctx, runtime, (uint32_t)(addr))
#define READ64(addr) TfSprHotRead64(rdram, ctx, runtime, (uint32_t)(addr))
#define READ128(addr) TfSprHotRead128(rdram, ctx, runtime, (uint32_t)(addr))
#define PS2_SPR_TRY_WRITE(addr, value) Ps2ScratchpadInlineWrite(addr, value)
