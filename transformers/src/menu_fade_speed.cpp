// The front end's screen fades sped up (menu_fade_speed.h; TODO 386, the user's "quick menus").
//
// Every screen (krScreen) runs its fades in the screens' base update, 0x1A0C20 (each screen's update calls it; below
// 0x33B690, so the same in the PAL release): while fading in (state 1 of 0x1A0CF0) its level at +0x50 goes up by
// +0x48 (the fade-in rate, a second) x the frame's seconds to 1.0, while fading out (state 3) down by +0x4C x the
// seconds to 0.0; at the end it calls 0x1A0E50 (the next state). Here both rates are set to the game's own rate x the
// chosen speed, per screen, at the base update's entry (before the game reads them; the update can stop part way
// inside its callee and go on outside this wrapper, so nothing is done after the call). The game's own rate is kept
// per screen (a rate the game puts there later is taken as its new own rate), so a change of the F1 row is applied
// at each screen's next update from that rate - live, no restart. At Original (x1) the value written is the game's
// own, bit for bit. The seconds themselves are untouched, so everything else on the screen (its children, its
// timers) runs as before. The row's variable is TF_MENU_FADE_SPEED (1 / 2 / 4; any up to 16): set before the game
// starts it overrides the row.

#include "menu_fade_speed.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace menu_fade_speed
{
    namespace
    {
        constexpr uint32_t kBaseUpdate = 0x001A0C20u;
        constexpr uint32_t kFadeIn = 0x48u, kFadeOut = 0x4Cu;
        PS2Runtime::RecompiledFunction g_original = nullptr;
        std::atomic<float> g_k{1.0f};
        std::atomic<uint32_t> g_generation{0u}; // a new choice: every screen's rates set again from its own
        std::atomic<bool> g_stateLoaded{false};  // a save state just loaded (stateLoaded): the screens' rates are its
        uint64_t g_fromStateUntil = 0u;          // screens first seen up to this tick are the state's

        struct Scaled
        {
            uint32_t vtable = 0u;
            float in = 0.0f, out = 0.0f;         // the values this wrote (or found, when not a rate)
            float ownIn = 0.0f, ownOut = 0.0f;   // the game's own rates (0: none to scale)
            uint32_t generation = ~0u;
            bool fromState = false;              // its first rate read is the state's: own x the speed
        };
        std::unordered_map<uint32_t, Scaled> g_screens;

        float speedFrom(const std::string &value)
        {
            const float k = value.empty() ? 1.0f : static_cast<float>(std::atof(value.c_str()));
            return k > 0.0f ? std::fmin(k, 16.0f) : 1.0f;
        }
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
        // One rate: the game's own taken when it is not the value this wrote; set to own x k when new or re-chosen.
        bool rate(uint8_t *rdram, uint32_t at, float &wrote, float &own, bool again, float k, bool fromState)
        {
            const float v = real(rdram, at);
            const bool fresh = v != wrote;
            if (fresh)
                own = std::isfinite(v) && v > 0.0f ? (fromState ? v / k : v) : 0.0f;
            if (own > 0.0f && (fresh || again))
                setReal(rdram, at, wrote = own * k);
            else if (fresh)
                wrote = v;
            return fresh;
        }

        void baseUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4);
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            if (g_stateLoaded.exchange(false))
            {
                // (A state's screens hold the rates of the session that made it: x the speed it had, taken to be this
                // speed - a restart's state, a player's own. Made at another speed, a screen's fade runs at another
                // speed until the game sets its rate again; never stuck. At Original nothing differs.)
                g_screens.clear();
                g_fromStateUntil = tick + 2u;
            }
            if (inRam(self))
            {
                const uint32_t vtable = word(rdram, self);
                Scaled &s = g_screens[self];
                if (s.vtable != vtable)
                    s = Scaled{vtable, -1.0f, -1.0f, 0.0f, 0.0f, ~0u, tick <= g_fromStateUntil}; // (another screen in this memory now)
                const uint32_t generation = g_generation.load(std::memory_order_relaxed);
                const bool again = s.generation != generation;
                s.generation = generation;
                const float k = g_k.load(std::memory_order_relaxed);
                const bool newIn = rate(rdram, self + kFadeIn, s.in, s.ownIn, again, k, s.fromState);
                const bool newOut = rate(rdram, self + kFadeOut, s.out, s.ownOut, again, k, s.fromState);
                s.fromState = false;
                static std::unordered_set<uint32_t> told;
                if (k != 1.0f && (newIn || newOut) && told.size() < 64 && told.insert(vtable).second)
                    std::fprintf(stderr, "[menu fade] tick %llu: screen %08x (vtable %08x): fade in %g -> %g a second, out %g -> %g\n",
                                 static_cast<unsigned long long>(tick), self, vtable,
                                 static_cast<double>(s.ownIn), static_cast<double>(real(rdram, self + kFadeIn)),
                                 static_cast<double>(s.ownOut), static_cast<double>(real(rdram, self + kFadeOut)));
            }
            g_original(rdram, ctx, runtime);
        }
    }

    void addSettings()
    {
        if (const char *env = std::getenv("TF_MENU_FADE_SPEED"); env && *env)
            g_k.store(speedFrom(env));
        ps2_settings::Setting s;
        s.key = "TF_MENU_FADE_SPEED";
        s.section = "Gameplay";
        s.label = "Menu speed";
        s.order = 6;
        s.help = "How fast the menus fade in and out. Original is the console's speed; Four times is the quickest.";
        s.choices = {{"1", "Original"}, {"2", "Double"}, {"4", "Four times"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &value)
        {
            g_k.store(speedFrom(value));
            g_generation.fetch_add(1u);
            std::fprintf(stderr, "[menu fade] menu speed x%g (F1 > Gameplay > Menu speed)\n", static_cast<double>(g_k.load()));
            return true;
        };
        ps2_settings::add(s);
    }

    void stateLoaded() { g_stateLoaded.store(true); }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_MENU_FADE_SPEED"); env && *env)
            g_k.store(speedFrom(env));
        if ((g_original = runtime.lookupFunction(kBaseUpdate)) == nullptr)
        {
            std::fprintf(stderr, "[menu fade] no screens' base update at %08x: the menu speed stays the console's\n", kBaseUpdate);
            return;
        }
        runtime.registerFunction(kBaseUpdate, baseUpdate);
        std::fprintf(stderr, "[menu fade] menu speed x%g (F1 > Gameplay > Menu speed; TF_MENU_FADE_SPEED; 1 = the console's)\n",
                     static_cast<double>(g_k.load()));
    }
}
