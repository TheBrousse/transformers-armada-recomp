// A debugging aid: TF_FILE_LOG=1 logs the files the game opens through flFile::Open (0x185820),
// with the VBlank tick (which file of a pair - CREATURE.TDB / .BDB, ... - a level reads).

#include "file_trace.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace file_trace
{
    namespace
    {
        constexpr uint32_t kOpen = 0x00185820u;
        PS2Runtime::RecompiledFunction g_open = nullptr;

        // A C string, or a CodeWarrior std::string (byte 0's low bit: the long form, chars at +8).
        std::string text(const uint8_t *rdram, uint32_t a)
        {
            const uint8_t *s = getConstMemPtr(rdram, a);
            if (!s)
                return "";
            uint32_t chars = a;
            uint32_t size = 0u, data = 0u;
            std::memcpy(&size, s + 4, sizeof(size));
            std::memcpy(&data, s + 8, sizeof(data));
            if ((s[0] & 1u) && size > 0u && size < 256u && data >= 0x00100000u && data < PS2_RAM_SIZE)
                chars = data; // (a std::string's long form, whatever its first byte reads as)
            else if (s[0] < 0x20 || s[0] >= 0x7F)
            {
                chars = a + 1u;
                if ((s[0] & 1u) != 0u)
                    std::memcpy(&chars, s + 8, sizeof(chars));
            }
            std::string t;
            if (const uint8_t *p = getConstMemPtr(rdram, chars))
                for (int i = 0; i < 160 && p[i] >= 0x20 && p[i] < 0x7F; ++i)
                    t += static_cast<char>(p[i]);
            return t;
        }

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void open(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::string name;
            for (int r = 4; r <= 6 && name.size() < 3; ++r)
                name = text(rdram, getRegU32(ctx, r));
            if (name.size() < 4) // (a std::string the reader above does not know: show the raw arguments)
            {
                char raw[96];
                std::snprintf(raw, sizeof(raw), " [a0 %08x a1 %08x a2 %08x a1[0] %08x a1[1] %08x]", getRegU32(ctx, 4), getRegU32(ctx, 5),
                              getRegU32(ctx, 6), word(rdram, getRegU32(ctx, 5)), word(rdram, getRegU32(ctx, 5) + 4u));
                name += raw;
            }
            std::fprintf(stderr, "[file] vblank %llu open %s\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), name.c_str());
            g_open(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *on = std::getenv("TF_FILE_LOG");
        if (!on || *on != '1')
            return;
        if ((g_open = runtime.lookupFunction(kOpen)) != nullptr)
            runtime.registerFunction(kOpen, open);
    }
}
