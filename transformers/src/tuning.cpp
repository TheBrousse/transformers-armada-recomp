// Tuning (tuning.h): the game's speed.
//
// Every system of a level steps by the level's frame period (taGame +0x5C, set when the level
// begins: 1/R at R frames a second). The game clock's own scale (+0x0C) changes nothing a player sees
// (measured: the same distance walked at scale 0.25 and 1). So game speed changes what a level begun
// at another rate would have (the port's convertLevel, overrides.cpp): the frame period, the clock's
// step (GameClock_SetStep, 0x41E820: step +0x08, frames since +0x10, the time at the step +0x14, the
// time going on from the next frame's) and the physics' gravity a step (0x6EC698 = the gravity a
// second at 0x6CBA1C times the period). The pictures keep their rate; the game moves on faster or
// slower in them. 0 freezes the game (photo mode).
//
// Decomp equivalent: the frame period times the speed, where the level sets it, and its two
// followers (clock step, gravity step) with it.

#include "tuning.h"

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

namespace tuning
{
    namespace
    {
        constexpr uint32_t kGame = 0x006CF2B8u, kGamePeriod = 0x5Cu;
        constexpr uint32_t kGameClock = 0x006CF8A4u;
        constexpr uint32_t kGravityStep = 0x006EC698u, kGravityPerSecond = 0x006CBA1Cu;
        constexpr uint32_t kCreatureGravity = 0x006CF9B4u;
        std::atomic<float> g_speed{1.0f};
        std::atomic<float> g_gravity{1.0f}; // the gravity's factor (the gravity step only)
        float g_writtenGravity = 1.0f;
        float g_base = 0.0f;    // the level's own period
        float g_written = 0.0f; // the period last written (0: none)
        uint32_t g_game = 0u;

        float loadFloat(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void storeFloat(uint8_t *rdram, uint32_t a, float v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, sizeof(v));
        }
    }

    float speed() { return g_speed.load(); }
    float gravity() { return g_gravity.load(); }
    void setSpeed(float s) { g_speed.store(std::clamp(s, 0.0f, 4.0f)); }
    void setGravity(float g) { g_gravity.store(std::clamp(g, 0.1f, 4.0f)); }

    void frame(uint8_t *rdram, bool inLevel)
    {
        uint32_t game = 0u, clock = 0u;
        std::memcpy(&game, rdram + kGame, sizeof(game));
        std::memcpy(&clock, rdram + kGameClock, sizeof(clock));
        if (!inLevel || !game || !clock)
        {
            g_game = 0u;
            return;
        }
        const float period = loadFloat(rdram, game + kGamePeriod);
        if (game != g_game || (period != g_written && period > 0.0f))
        {
            // (a new level, or the port set a new rate: that is the level's own period now)
            g_game = game;
            g_base = period;
            g_written = 0.0f;
            g_writtenGravity = 1.0f; // (the level set its own gravity step)
        }
        if (!(g_base > 0.0005f && g_base < 0.1f))
            return;
        const float want = g_base * std::max(g_speed.load(std::memory_order_relaxed), 1.0e-4f);
        const float current = g_written != 0.0f ? g_written : g_base;
        const float gravity = g_gravity.load(std::memory_order_relaxed);
        {
            // The creatures' own gravity (0x6CF9B4: a jump's and a fall's, 0x470F70), the game's value
            // taken again whenever it is not the one written here.
            static float base = 0.0f, written = 0.0f;
            const float now = loadFloat(rdram, kCreatureGravity);
            if (now != written)
                base = now;
            if (base != 0.0f && now != base * gravity)
                storeFloat(rdram, kCreatureGravity, base * gravity);
            written = base * gravity;
        }
        if (want == current && gravity == g_writtenGravity)
            return;
        // (the gravity a step: the game's gravity a second x the period, and our factor)
        storeFloat(rdram, kGravityStep, loadFloat(rdram, kGravityPerSecond) * want * gravity);
        g_writtenGravity = gravity;
        if (want == current)
            return;
        storeFloat(rdram, game + kGamePeriod, want);
        // The clock: SetStep (the step, from the next frame on, its time going on from now)
        const float time = loadFloat(rdram, clock + 0x04u), step = loadFloat(rdram, clock + 0x08u);
        const float scale = loadFloat(rdram, clock + 0x0Cu);
        storeFloat(rdram, clock + 0x08u, want);
        if (uint8_t *p = getMemPtr(rdram, clock + 0x10u))
            std::memset(p, 0, 4);
        storeFloat(rdram, clock + 0x14u, time + step * scale);
        g_written = want;
    }
}
