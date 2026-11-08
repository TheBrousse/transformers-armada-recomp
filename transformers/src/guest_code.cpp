// Added guest code (guest_code.h).
//
// The self-test (TF_ADDED_CODE_TEST=1) proves the way in: the crosshair's object (distance_meter
// knows it) is given a vtable of its own, a copy of taModelPredictionCrosshair's with DoRender
// (0x2FF640) replaced by a function of the added code, which counts its calls and runs the original.
// The game's own indirect call (its render pass through the vtable) then reaches C++ through a guest
// address the executable never had. It logs "[guest code] test: ..." once a second.
//
// Decomp equivalent: none needed (new classes are plain source there); the classes written on this
// are meant to move over as they are.

#include "guest_code.h"

#include "distance_meter.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace guest_code
{
    namespace
    {
        // The added data's region, the runtime's (PS2Runtime::guestLayout, read once - after main() has set the
        // layout): at 256 MB (N7) the top 1 MB of RAM, outside the game's heap; at 32 MB right under the main
        // thread's stack, the runtime's guest heap ending below it (at 0x1F48000 it took 896 KB of the heap the
        // game gets on hardware; in kernel RAM the level loads stalled at the bike's factory).
        uint32_t dataBegin() { return PS2Runtime::guestLayout().addedDataBegin; }
        uint32_t dataEnd() { return PS2Runtime::guestLayout().addedDataEnd; }

        uint32_t g_nextCode = PS2Runtime::kAddedCodeBegin;
        uint32_t g_nextData = 0u; // (0: dataBegin() at the first block)

        struct Block
        {
            std::string name;
            uint32_t at, size;
            std::function<void(uint8_t *, uint32_t)> fill;
            std::vector<uint8_t> written; // what fill put there (the watch compares against it)
            bool reported = false;
        };
        std::vector<Block> g_blocks;
        // The layout of everything added (layoutKey): each function's and block's name and address.
        uint64_t g_layout = 1469598103934665603ull;
        void noteLayout(const char *name, uint32_t at, uint32_t size)
        {
            for (const char *c = name ? name : ""; *c; ++c)
                g_layout = (g_layout ^ static_cast<unsigned char>(*c)) * 1099511628211ull;
            for (const uint32_t v : {at, size})
                g_layout = (g_layout ^ v) * 1099511628211ull;
        }

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void setWord(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, sizeof(v));
        }

        // ---- the self-test ----------------------------------------------------------------
        constexpr uint32_t kCrosshairDoRender = 0x002FF730u; // (Update: DoRender, 0x2FF640, is not called through the vtable here)
        PS2Runtime::RecompiledFunction g_doRender = nullptr;
        uint32_t g_testVtable = 0u, g_testFrom = 0u;
        uint64_t g_testCalls = 0u;
        bool g_test = false;

        void testDoRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_testCalls;
            g_doRender(rdram, ctx, runtime); // (the original, as its caller would have reached it)
        }
    }

    uint32_t addFunction(PS2Runtime &runtime, const char *name, PS2Runtime::RecompiledFunction fn)
    {
        if (g_nextCode >= PS2Runtime::kAddedCodeEnd)
        {
            std::fprintf(stderr, "[guest code] no room for %s\n", name);
            return 0u;
        }
        const uint32_t at = g_nextCode;
        g_nextCode += 4u;
        noteLayout(name, at, 0u);
        if (!runtime.registerFunction(at, fn))
        {
            std::fprintf(stderr, "[guest code] %s: the runtime has no added code (patch 0014)\n", name);
            return 0u;
        }
        return at;
    }

    uint32_t addData(const char *name, uint32_t size, std::function<void(uint8_t *rdram, uint32_t at)> fill)
    {
        if (g_nextData == 0u)
            g_nextData = dataBegin();
        const uint32_t at = (g_nextData + 15u) & ~15u;
        if (at + size > dataEnd())
        {
            std::fprintf(stderr, "[guest code] no room for %s (%u bytes)\n", name, size);
            return 0u;
        }
        g_nextData = at + size;
        g_blocks.push_back(Block{name, at, size, std::move(fill)});
        noteLayout(name, at, size);
        return at;
    }

    uint64_t layoutKey() { return g_layout; }

    uint32_t addVtable(const char *name, uint32_t from, uint32_t words, std::vector<std::pair<uint32_t, uint32_t>> replace)
    {
        return addData(name, words * 4u, [from, words, replace](uint8_t *rdram, uint32_t at)
                       {
                           for (uint32_t i = 0; i < words; ++i)
                           {
                               uint32_t v = word(rdram, from + 4u * i);
                               for (const auto &[old, now] : replace)
                                   if (v == old)
                                       v = now;
                               setWord(rdram, at + 4u * i, v);
                           } });
    }

    uint32_t addState(const char *name, uint32_t size)
    {
        return addData(name, size, nullptr); // (no fill: writeAll and the watch leave it alone)
    }

    uint32_t addCString(const char *name, const char *text)
    {
        const std::string s(text);
        return addData(name, static_cast<uint32_t>(s.size() + 1u), [s](uint8_t *rdram, uint32_t at)
                       {
                           if (uint8_t *p = getMemPtr(rdram, at))
                               std::memcpy(p, s.c_str(), s.size() + 1u); });
    }

    uint32_t addStdString(const char *name, const char *text)
    {
        const std::string s(text);
        const uint32_t size = static_cast<uint32_t>(s.size());
        return addData(name, 16u + size + 1u, [s, size](uint8_t *rdram, uint32_t at)
                       {
                           setWord(rdram, at, (size << 1) | 1u); // (long form, capacity = size)
                           setWord(rdram, at + 4u, size);
                           setWord(rdram, at + 8u, at + 16u);
                           setWord(rdram, at + 12u, 0u);
                           if (uint8_t *p = getMemPtr(rdram, at + 16u))
                               std::memcpy(p, s.c_str(), size + 1u); });
    }

    uint32_t addResumable(PS2Runtime &runtime, const char *name, PS2Runtime::RecompiledFunction fn, int returns)
    {
        const uint32_t entry = addFunction(runtime, name, fn);
        for (int i = 0; i < returns && entry != 0u; ++i)
            if (addFunction(runtime, name, fn) == 0u)
                return 0u;
        return entry;
    }

    bool call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t target, uint32_t returnAddress)
    {
        SET_GPR_U32(ctx, 31, returnAddress);
        ctx->pc = target;
        return runtime->dispatchGuestBranch(rdram, ctx, target, returnAddress - 8u, returnAddress,
                                            PS2Runtime::GuestBranchKind::DirectCall, "added code");
    }

    void writeAll(uint8_t *rdram)
    {
        for (Block &b : g_blocks)
            if (b.fill)
            {
                b.fill(rdram, b.at);
                if (const uint8_t *p = getConstMemPtr(rdram, b.at))
                    b.written.assign(p, p + b.size);
            }
    }

    namespace
    {
        // The watch: the added data must hold what was written there. Anything else writing in its
        // range (the game, the runtime) is logged once and the block written again.
        void watch(uint8_t *rdram)
        {
            for (Block &b : g_blocks)
            {
                const uint8_t *p = getConstMemPtr(rdram, b.at);
                if (!p || b.written.size() != b.size || std::memcmp(p, b.written.data(), b.size) == 0)
                    continue;
                uint32_t first = 0u;
                while (first < b.size && p[first] == b.written[first])
                    ++first;
                if (!b.reported)
                    std::fprintf(stderr, "[guest code] something else wrote into %s at %08x: written again\n", b.name.c_str(), b.at + first);
                b.reported = true;
                b.fill(rdram, b.at);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_ADDED_CODE_TEST"); env && *env && *env != '0')
        {
            g_doRender = runtime.lookupFunction(kCrosshairDoRender);
            const uint32_t fn = addFunction(runtime, "crosshair Update (test)", testDoRender);
            // taModelPredictionCrosshair's vtable: its vptr points at 0x6B6A54 (the object's [0], read
            // once the crosshair exists); the copy takes 0x6B6A48..0x6B6A78, DoRender at 0x6B6A68.
            g_testFrom = 0x006B6A48u;
            g_testVtable = addVtable("crosshair vtable (test)", g_testFrom, 12u, {{kCrosshairDoRender, fn}});
            g_test = fn != 0u && g_testVtable != 0u;
            std::fprintf(stderr, "[guest code] test: DoRender at %08x, vtable copy at %08x\n", fn, g_testVtable);
        }
        writeAll(runtime.memory().getRDRAM());
    }

    void frame(uint8_t *rdram, PS2Runtime &runtime)
    {
        static bool written = false; // (once more at the first frame: the executable is in memory by then)
        if (!written)
        {
            written = true;
            writeAll(rdram);
        }
        static uint64_t watchedAt = 0u;
        if (const uint64_t now = runtime.eeScheduler().currentVSyncTick(); now >= watchedAt + 60u)
        {
            watchedAt = now;
            watch(rdram);
        }
        if (!g_test)
            return;
        const uint32_t crosshair = distance_meter::crosshairObject(runtime);
        if (crosshair)
        {
            const uint32_t vptr = word(rdram, crosshair);
            if (vptr >= g_testFrom && vptr < g_testFrom + 48u) // the game's own: give it the copy
            {
                setWord(rdram, crosshair, g_testVtable + (vptr - g_testFrom));
                std::fprintf(stderr, "[guest code] test: the crosshair %08x now uses the copy (vptr %08x -> %08x)\n", crosshair, vptr,
                             g_testVtable + (vptr - g_testFrom));
            }
        }
        static uint64_t lastTick = 0u;
        const uint64_t tick = runtime.eeScheduler().currentVSyncTick();
        if (tick >= lastTick + 60u)
        {
            lastTick = tick;
            std::fprintf(stderr, "[guest code] test: Update through the added code: %llu calls\n",
                         static_cast<unsigned long long>(g_testCalls));
        }
    }
}
