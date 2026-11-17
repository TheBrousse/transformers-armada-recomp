// TODO 369: a dropped pickup falls and bounces by the frame's time, not by one step a frame.
//
// taPickupBase::Update (0x37D3A0, every pickup's: the Mini-Cons, the Data-Cons, health, energon) lets a pickup that is
// neither held, nor thrown, nor resting fall onto its height (+0x88) and bounce: the fall speed (+0x84) grows by the
// fall acceleration (gp-0x63CC) x dt and is capped at 5 (0x37D894..0x37D8C8) - in seconds - but the move is the speed
// itself, once a frame: GetPosition (vtable +0x08, returning to 0x37D8E0), then z -= speed (0x37D8EC); below the
// height the speed turns -0.5 x itself (the bounce); resting when within gp-0x5F50 of the height and slower than
// gp-0x630C. So the drop went as many steps a second as there are frames: at 240 eight times as far and as fast as
// on the console's 30, and with the game stopped (photo mode: dt near zero, tuning.cpp) the speed stopped changing
// while the steps went on: a pickup on its way up rose for as long as photo mode lasted, then fell back and bounced
// when it was left (bbatts' report), and the ones still settling went on floating up and down.
// Here the position GetPosition hands to that one call is moved by speed x (1 - 30 dt), so the step becomes
// speed x 30 dt: the console's at 1/30 s, the same distance a second at every rate, nothing while the game is stopped.
// The height test, the bounce and the rest test then run on that position as before.
// TF_PICKUP_STEP=0: as before. TF_PICKUP_LOG=<ticks>: each falling pickup's place, speed and state that often.
//
// Decomp equivalent: taPickupBase::Update's fall, position.z -= speed * dt * 30.
#include "pickup_step.h"

#include "port.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace pickup_step
{
    namespace
    {
        // taPickupBase's GetPosition (no pickup class replaces it) and the fall's call of it.
        constexpr uint32_t kGetPositionBase = TF_GA(0x0037D0C0u);
        constexpr uint32_t kFallGetPositionReturn = TF_GA(0x0037D8E0u);
        constexpr uint32_t kSpeed = 0x84u, kHeight = 0x88u;
        PS2Runtime::RecompiledFunction g_base = nullptr;
        bool g_on = true;
        int g_logEvery = 0;

        float loadFloat(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            if (a >= 0x00100000u && a < PS2_RAM_SIZE - 4u)
                std::memcpy(&v, rdram + a, sizeof(v));
            return v;
        }

        void afterFallPosition(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t out, uint32_t pickup)
        {
            if (out < 0x00100000u || out >= PS2_RAM_SIZE - 12u || pickup < 0x00100000u || pickup >= PS2_RAM_SIZE - 0x100u)
                return;
            const float dt = ctx->f[20]; // (the frame's time: Update's f20, kept across the call)
            const float speed = loadFloat(rdram, pickup + kSpeed);
            float z = 0.0f;
            std::memcpy(&z, rdram + out + 8u, sizeof(z));
            if (g_logEvery > 0)
            {
                const uint64_t tick = tf::port::testTickNow();
                if (tick % static_cast<uint64_t>(g_logEvery) == 0u)
                {
                    float x = 0.0f, y = 0.0f;
                    std::memcpy(&x, rdram + out, sizeof(x));
                    std::memcpy(&y, rdram + out + 4u, sizeof(y));
                    uint32_t vtable = 0u;
                    std::memcpy(&vtable, rdram + pickup, sizeof(vtable));
                    std::fprintf(stderr, "[pickup] tick %llu %08x (vtable %06x): %.3f %.3f %.3f, the height %.3f, speed %.4f a step, dt %.6f\n",
                                 static_cast<unsigned long long>(tick), pickup, vtable, x, y, z, loadFloat(rdram, pickup + kHeight), speed, dt);
                }
            }
            if (!g_on || !(dt >= 0.0f) || !(dt < 1.0f) || !std::isfinite(speed))
                return;
            const float share = std::clamp(dt * 30.0f, 0.0f, 4.0f);
            z += speed * (1.0f - share); // (the game then takes the whole speed off: speed x share in all)
            std::memcpy(rdram + out + 8u, &z, sizeof(z));
            (void)runtime;
        }

        void getPositionBase(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31), out = getRegU32(ctx, 4), pickup = getRegU32(ctx, 5);
            g_base(rdram, ctx, runtime);
            if (ra == kFallGetPositionReturn && ctx->pc == ra)
                afterFallPosition(rdram, ctx, runtime, out, pickup);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *v = std::getenv("TF_PICKUP_STEP"); v && *v == '0')
            g_on = false;
        if (const char *v = std::getenv("TF_PICKUP_LOG"))
            g_logEvery = std::max(0, std::atoi(v));
        if (!g_on && g_logEvery == 0)
            return;
        if ((g_base = runtime.lookupFunction(kGetPositionBase)) != nullptr)
            runtime.registerFunction(kGetPositionBase, getPositionBase);
    }
}
