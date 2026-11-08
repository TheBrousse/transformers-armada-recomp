#pragma once
// Added guest code (guest_code.cpp): C++ the game can call as its own code, and the guest memory its
// new classes need. The base of the port's C++ mods: new classes (a jet bike), new Mini-Con abilities.
//
//   addFunction  a C++ function at an address of the added code (patch 0014's range, 0x01F40000-
//                0x01F48000: addresses only, nothing is stored there), registered like the game's
//                own: call it through a vtable or a pointer.
//   addData      a block of the added data (N7: the top 1 MB of the 256 MB RAM, 0x0FF00000-0x10000000, above the
//                runtime's heap; with TF_RAM_MB=32 0x01FA8000-0x01FE0000, under the main thread's stack; until 2026-10-23 at 0x01F48000, with
//                the heap ending at 0x1F00000: states made before then are refused, save-state
//                format 3), filled by the caller's function at start and after every state load (so a
//                state made by another session, or by an older version, still has it right).
//   addVtable    a vtable for a new class: a copy of a class's own (read from the executable) with
//                entries replaced.
//
// Addresses come in the order things are added, at start: the same in every session, so save
// states and the game's pointers to them stay valid. Add everything from install-time code only.
//
// Register every added function at every start, whatever the settings say (a feature that is off
// keeps its functions and does nothing in them): a save state made while one was in use holds the
// game's pointers to it, and loading it where the function is not registered jumps to no code.
// Tested (TF_ADDED_CODE_TEST): the game's per-frame call through a cloned vtable reaches added code,
// and goes on across a save state loaded in a new session; RAM 0x01F40000-0x01F80000 is untouched
// at the HQ and in a level (TF_RAM_SCAN).

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace guest_code
{
    uint32_t addFunction(PS2Runtime &runtime, const char *name, PS2Runtime::RecompiledFunction fn);
    uint32_t addData(const char *name, uint32_t size, std::function<void(uint8_t *rdram, uint32_t at)> fill);
    // A copy of `words` words of guest memory from `from` (a class's vtable with what precedes it, as
    // the object's vptr points into it), each entry whose value is a replacement's first changed to
    // its second. Returns the copy's address; the object's vptr goes to it plus (vptr - from).
    uint32_t addVtable(const char *name, uint32_t from, uint32_t words, std::vector<std::pair<uint32_t, uint32_t>> replace);
    // The added data written (at start, after a state loads: overrides.cpp).
    void writeAll(uint8_t *rdram);

    // A block the game owns once it runs (a class's globals: its pool, its static data): zero at
    // start, never written again, so a save state keeps what the game put there.
    uint32_t addState(const char *name, uint32_t size);
    // Constants in the game's own formats, in the added data: a C string, and a CodeWarrior
    // std::string (its long form: word 0 = capacity << 1 | 1, +4 size, +8 the characters) for the
    // game's functions that take a const std::string&.
    uint32_t addCString(const char *name, const char *text);
    uint32_t addStdString(const char *name, const char *text);

    // An added function that calls the game's functions: entered at its address and at each of
    // `returns` return addresses after it (address + 4, + 8, ...), all registered to `fn`, which
    // switches on ctx->pc. Its own state lives in guest memory (an addState block), as a callee may
    // stop at a checkpoint and the scheduler then enters the function again at the return address.
    uint32_t addResumable(PS2Runtime &runtime, const char *name, PS2Runtime::RecompiledFunction fn, int returns);
    // A call from such a function (jal): ra = returnAddress, then the callee runs. True when it
    // returned; false when it stopped: the caller returns at once (it is entered again at
    // returnAddress when the callee is done).
    bool call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t target, uint32_t returnAddress);

    // The calling convention, for an added function (GCC's EABI, as the recompiled code has it):
    // `this` and integer/pointer arguments in a0-a3 then t0-t3, floats in f12-f19 (each kind counted on
    // its own); the result in v0 or f0. An added function returns through one of the ret* below: they
    // hand the caller's return address back (ctx->pc = ra), as a recompiled `jr ra` does.
    //
    // An added function that calls guest code (recompiled or decompiled) may be stopped inside the
    // callee: the callee then returns to the scheduler, not to the C++ that called it. Such a function
    // is written as a resumable one, the decomp layer's way (transformers/decomp/README.md: Guest::call,
    // DECOMP_YIELDING): its state in guest registers and memory, entered again at the return address.
    // A plain pass-through that calls the original last (and returns what it set) is safe as it is.
    inline uint32_t arg(const R5900Context *ctx, int i) { return getRegU32(ctx, i < 4 ? 4 + i : 8 + (i - 4)); }
    inline float farg(const R5900Context *ctx, int i) { return ctx->f[12 + i]; }
    inline void ret(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }
    inline void retU32(R5900Context *ctx, uint32_t v)
    {
        SET_GPR_U32(ctx, 2, v);
        ret(ctx);
    }
    inline void retFloat(R5900Context *ctx, float v)
    {
        ctx->f[0] = v;
        ret(ctx);
    }

    // Where everything added lives (each added function's and data block's name and address, in the
    // order added): a save state holds the game's pointers to them, so a state loads into a build
    // whose key is the same (updates.cpp checks it before resuming after an update, TODO 104).
    uint64_t layoutKey();

    // From applyTransformers, last: the self-test (TF_ADDED_CODE_TEST=1).
    void install(PS2Runtime &runtime);
    // Once a game frame (overrides.cpp): the self-test's work.
    void frame(uint8_t *rdram, PS2Runtime &runtime);
}
