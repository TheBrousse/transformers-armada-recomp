// The front end's screen fades sped up (menu_fade_speed.h; TODO 386, the user's "quick menus" test).
//
// Every screen (krScreen) runs its fades in the screens' base update, 0x1A0C20 (each screen's update calls it; below
// 0x33B690, so the same in the PAL release): while fading in (state 1 of 0x1A0CF0) its level at +0x50 goes up by
// +0x48 (the fade-in rate, a second) x the frame's seconds to 1.0, while fading out (state 3) down by +0x4C x the
// seconds to 0.0; at the end it calls 0x1A0E50 (the next state). Here both rates are made n times as large, once
// per screen, at the base update's entry (before the game reads them; the update can stop part way inside its callee
// and go on outside this wrapper, so nothing is done after the call): a rate the game puts there later is scaled again
// the next time. The seconds themselves are untouched, so everything else on the screen (its children, its timers)
// runs as before. TF_MENU_FADE_SPEED=<n> (2, 4, ...; at most 16); unset or 1: not installed.

#include "menu_fade_speed.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace menu_fade_speed
{
    namespace
    {
        constexpr uint32_t kBaseUpdate = 0x001A0C20u;
        constexpr uint32_t kFadeIn = 0x48u, kFadeOut = 0x4Cu;
        PS2Runtime::RecompiledFunction g_original = nullptr;
        float g_k = 1.0f;

        struct Scaled
        {
            uint32_t vtable = 0u;
            float in = 0.0f, out = 0.0f; // the values this wrote (scaled)
        };
        std::unordered_map<uint32_t, Scaled> g_screens;

        bool inRam(uint32_t a) { return a >= 0x00100000u && a + 0x60u < PS2_RAM_SIZE; }
        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, 4);
            return v;
        }
        float real(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t w = word(rdram, a);
            float f;
            std::memcpy(&f, &w, 4);
            return f;
        }
        void setReal(uint8_t *rdram, uint32_t a, float f)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &f, 4);
        }

        void baseUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4);
            if (inRam(self))
            {
                const uint32_t vtable = word(rdram, self);
                Scaled &s = g_screens[self];
                if (s.vtable != vtable)
                    s = Scaled{vtable, -1.0f, -1.0f}; // (another screen in this memory now)
                const float in = real(rdram, self + kFadeIn), out = real(rdram, self + kFadeOut);
                const bool newIn = in != s.in, newOut = out != s.out;
                if (newIn && std::isfinite(in) && in > 0.0f)
                    setReal(rdram, self + kFadeIn, s.in = in * g_k);
                else if (newIn)
                    s.in = in;
                if (newOut && std::isfinite(out) && out > 0.0f)
                    setReal(rdram, self + kFadeOut, s.out = out * g_k);
                else if (newOut)
                    s.out = out;
                static std::unordered_set<uint32_t> told;
                if ((newIn || newOut) && told.size() < 64 && told.insert(vtable).second)
                    std::fprintf(stderr, "[menu fade] tick %llu: screen %08x (vtable %08x): fade in %g -> %g a second, out %g -> %g\n",
                                 static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), self, vtable,
                                 static_cast<double>(in), static_cast<double>(real(rdram, self + kFadeIn)), static_cast<double>(out),
                                 static_cast<double>(real(rdram, self + kFadeOut)));
            }
            g_original(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("TF_MENU_FADE_SPEED");
        const float k = env && *env ? static_cast<float>(std::atof(env)) : 1.0f;
        if (!(k > 0.0f) || k == 1.0f)
            return;
        g_k = std::fmin(k, 16.0f);
        if ((g_original = runtime.lookupFunction(kBaseUpdate)) == nullptr)
        {
            std::fprintf(stderr, "[menu fade] no screens' base update at %08x: not installed\n", kBaseUpdate);
            return;
        }
        runtime.registerFunction(kBaseUpdate, baseUpdate);
        std::fprintf(stderr, "[menu fade] the screens' fades run %g times as fast (TF_MENU_FADE_SPEED; 1 = the game's)\n", static_cast<double>(g_k));
    }
}
