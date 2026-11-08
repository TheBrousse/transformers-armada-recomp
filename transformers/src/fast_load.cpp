// Loading (fast_load.h): the sound muted while a level loads, and an experimental turbo.
//
// The port holds the game's CPU to the console's speed: its scheduler waits for the host's clock
// at every VBlank and timer. A level's load is the game's own work, a frame at a time, behind the
// warp tunnel: measured on Amazon from the HQ, the game object (taGame, 0x6CF2B8) is made at
// VBlank 6532 and the load runs until the game clears its hold-at-30 flag (0x6CD324, "from boot
// until a level is set up") at 6940: 408 VBlanks, 10.5 s on this PC, one frame of it
// 3 s of the console's CPU. While both are so (a game object, the flag still set), the
// scheduler's turbo mode is held on (patch 0012: no waiting for the host clock; the game's own
// time goes on as before, VBlank by VBlank) and the sound is muted (it would run ahead of real
// time). The tunnel then flies by and the level begins as soon as the PC has done the work.
//
// TF_FAST_LOAD=0 or F5 Gameplay "Fast loading" Off: the console's pace. TF_FAST_LOAD_TRACE=1 logs
// the signals as they change.
//
// Decomp equivalent: no frame pacing (and no sound) while the level loads (the same two signals,
// or the loader itself).

#include "fast_load.h"
#include "nr_install.h"
#include "nr_n5.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_audio_stream.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace fast_load
{
    namespace
    {
        constexpr uint32_t kHoldAtThirty = 0x006CD324u, kGame = 0x006CF2B8u;
        std::atomic<bool> g_enabled{false};
        bool g_held = false;
        uint64_t g_heldAt = 0u;
        std::chrono::steady_clock::time_point g_heldSince{};

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
    }

    void addSettings()
    {
        if (const char *env = std::getenv("TF_FAST_LOAD"); env && *env)
            g_enabled.store(*env != '0');
        ps2_settings::Setting s;
        s.key = "TF_FAST_LOAD";
        s.section = "Gameplay";
        s.label = "Turbo loading (experimental)";
        s.order = 50;
        s.help = "Runs the game unthrottled while a level loads. Measured slower for now (the load is tied to frames drawn): off by default.";
        s.choices = {{"0", "Off"}, {"1", "On"}};
        s.defaultValue = "0";
        s.apply = [](const std::string &value)
        {
            g_enabled.store(value != "0");
            return true;
        };
        ps2_settings::add(s);
    }

    void frame(uint8_t *rdram, PS2Runtime &runtime, bool inHq)
    {
        EeScheduler &ee = runtime.eeScheduler();
        const uint32_t hold = word(rdram, kHoldAtThirty), game = word(rdram, kGame);
        static const bool trace = std::getenv("TF_FAST_LOAD_TRACE") != nullptr;
        if (trace)
        {
            static uint32_t seenHold = ~0u;
            static bool seenGame = false;
            if (hold != seenHold || (game != 0u) != seenGame)
                std::fprintf(stderr, "[fast load] vblank %llu: hold %u, game %08x\n",
                             static_cast<unsigned long long>(ee.currentVSyncTick()), hold, game);
            seenHold = hold;
            seenGame = game != 0u;
        }
        // (Not in the HQ: after a level the game object's pointer stays set and the HQ holds itself
        // at 30, so both signals said "loading" for the whole HQ - it was muted, its music and every
        // sound, until the next level had loaded: the user's "no HQ music after warping back from a
        // level", 2026-10-28, TODO 66. A boot's first HQ has no game object yet, so it was heard.)
        const bool loading = game != 0u && hold != 0u && !inHq;
        if (loading == g_held)
            return;
        g_held = loading;
        // (Measured on this PC: 10.5 s at the console's pace, 10.9 s with turbo. The load takes the
        // same 408 VBlanks either way, and its time is the work itself: the tunnel's frames wait for
        // VU1 and the GS (PS2Runtime::vu1Busy from 0x26F740 / 0x26E990), and one frame does 3 s of
        // the console's CPU. So turbo stays an experiment, off; the silence is the useful part: the
        // sound broke up while the load held the game up.)
        ee.setTurboHeld(loading && g_enabled.load(std::memory_order_relaxed));
        ps2_audio_stream::setMuted(loading);
        if (loading)
        {
            g_heldAt = ee.currentVSyncTick();
            g_heldSince = std::chrono::steady_clock::now();
        }
        else
        {
            nr::placeChanged(); // (TODO 55: the native renderer lets the place before's textures go)
            nr::n5::dropSummaries(); // (N5 step 1: the packets of the place before)
            std::fprintf(stderr, "[fast load] level loaded: %llu VBlanks in %.1f s\n",
                         static_cast<unsigned long long>(ee.currentVSyncTick() - g_heldAt),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - g_heldSince).count());
        }
    }
}
