// The distance meter (a cut feature brought back): the distance to what the crosshair points at,
// shown above it.
//
// Retail still works the distance out: the prediction crosshair's Update (0x2FF730, a vtable
// function) copies the targeting system's +0x2C8 into the crosshair's +0x30 every frame (the store
// at 0x2FF9E0). What was cut is the drawing: the prototype printed it with
// vlxText2D::PrintfImmediate at taView::WorldToScreenCoord's place, both gone from retail. Here the
// port draws it instead (ImGui, over the picture), above the screen's centre where the crosshair is.
//
// Decomp equivalent: restore the two functions and the print (the prototype's text sits at the
// crosshair's +0xA8/+0xAC; retail allocates 0xB0 bytes, so they fit), or draw +0x30 from Update.

#include "distance_meter.h"

#include "port_ext.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_settings.h"

#include <string>
#include "runtime/ps2_memory.h"

#include <atomic>
#include <iterator>
#include <utility>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace distance_meter
{
    namespace
    {
        constexpr uint32_t kCrosshairUpdate = 0x002FF730u;
        constexpr uint32_t kDistance = 0x30u, kShown = 0x14u; // (the distance; the crosshair is shown)
        PS2Runtime::RecompiledFunction g_update = nullptr;
        std::atomic<uint32_t> g_crosshair{0u};
        std::atomic<uint64_t> g_lastTick{0u};
        std::atomic<bool> g_enabled{false}; // (off by default: the prototype's feature, a mod - TODO 74)

        // The in-game menus (the pause menu and its screens, the Mini-Con and Data-Con pickup menus):
        // their Render functions, passed through; the meter hides while one draws.
        constexpr uint32_t kMenuRenders[] = {
            0x00311BF0u, // taScreenInGPause::Render
            0x00313F30u, // taScreenInGMenu::RenderAsInGame (the pause menu's options, objectives, ...)
            0x0030EEF0u, // taScreenInGDataconPickupMenu::Render
            0x00310220u, // taScreenInGMiniconPickupMenu::Render
        };
        PS2Runtime::RecompiledFunction g_menuRender[std::size(kMenuRenders)] = {};
        std::atomic<uint64_t> g_menuTick{0u};

        template <size_t I>
        void menuRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_menuTick.store(runtime->eeScheduler().currentVSyncTick(), std::memory_order_relaxed);
            g_menuRender[I](rdram, ctx, runtime);
        }

        template <size_t... I>
        void hookMenus(PS2Runtime &runtime, std::index_sequence<I...>)
        {
            ((g_menuRender[I] = runtime.lookupFunction(kMenuRenders[I]),
              g_menuRender[I] ? (void)runtime.registerFunction(kMenuRenders[I], menuRender<I>) : (void)0),
             ...);
        }

        void update(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_crosshair.store(getRegU32(ctx, 4), std::memory_order_relaxed);
            g_lastTick.store(runtime->eeScheduler().currentVSyncTick(), std::memory_order_relaxed);
            g_update(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_DISTANCE_METER"); env && *env)
            g_enabled.store(*env != '0');
        if ((g_update = runtime.lookupFunction(kCrosshairUpdate)) != nullptr)
            runtime.registerFunction(kCrosshairUpdate, update);
        hookMenus(runtime, std::make_index_sequence<std::size(kMenuRenders)>{});
    }

    bool menuOpen(PS2Runtime &runtime)
    {
        const uint64_t at = g_menuTick.load(std::memory_order_relaxed);
        return at != 0u && runtime.eeScheduler().currentVSyncTick() <= at + 5u;
    }

    uint32_t crosshairObject(PS2Runtime &runtime)
    {
        const uint32_t crosshair = g_crosshair.load(std::memory_order_relaxed);
        const uint64_t now = runtime.eeScheduler().currentVSyncTick();
        return (crosshair && now <= g_lastTick.load(std::memory_order_relaxed) + 10u) ? crosshair : 0u;
    }

    void addSettings()
    {
        ps2_settings::Setting s;
        s.key = "TF_DISTANCE_METER";
        s.section = "Prototype";
        s.label = "Distance meter";
        s.help = "The distance to what the crosshair points at, above it: a cut feature of the game's prototype, back. Off by default.";
        s.choices = {{"0", "Off"}, {"1", "On"}};
        s.defaultValue = "0";
        s.apply = [](const std::string &value)
        {
            g_enabled.store(value == "1");
            return true;
        };
        ps2_settings::add(s);
    }

    bool enabled() { return g_enabled.load(); }
    void setEnabled(bool on)
    {
        g_enabled.store(on);
        ps2_settings::noteChanged("TF_DISTANCE_METER", on ? "1" : "0"); // (into settings.ini: kept for the next start, TODO 74)
    }

    bool distance(PS2Runtime &runtime, float &metres)
    {
        if (!g_enabled.load(std::memory_order_relaxed))
            return false;
        const uint32_t crosshair = g_crosshair.load(std::memory_order_relaxed);
        const uint64_t now = runtime.eeScheduler().currentVSyncTick();
        // (shown while the crosshair is being updated: it is not while a menu or a movie is up)
        if (!crosshair || now > g_lastTick.load(std::memory_order_relaxed) + 10u || menuOpen(runtime))
            return false;
        // (on the jet bike the targeting ray starts at the rider and meets the bike under him: 0.2 m;
        // its guns fire along the bike, not at the crosshair, so there is no distance to show)
        if (ext::hideDistance(runtime.memory().getRDRAM())) // (an extension's: the jet bike, port_ext.h)
            return false;
        // (shown only with the crosshair: its +0x14, which the game clears in the car, in menus and on
        // the Mini-Con screens)
        const uint8_t *shown = getConstMemPtr(runtime.memory().getRDRAM(), crosshair + kShown);
        if (!shown || *shown == 0u)
            return false;
        const uint8_t *p = getConstMemPtr(runtime.memory().getRDRAM(), crosshair + kDistance);
        if (!p)
            return false;
        float d;
        std::memcpy(&d, p, sizeof(d));
        if (!std::isfinite(d) || d <= 0.05f || d > 100000.0f)
            return false;
        metres = d;
        return true;
    }
}
