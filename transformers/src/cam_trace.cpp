// A debugging aid: TF_CAM_TRACE=1 logs which camera script updates the view (each script class's
// vtable slot 1, its per-frame update), with the VBlank tick, the script object and the caller,
// whenever that changes. For finding how the game picks its camera (first person, car, ...).

#include "cam_trace.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>

namespace cam_trace
{
    namespace
    {
        struct Script
        {
            uint32_t update;
            const char *name;
            PS2Runtime::RecompiledFunction original = nullptr;
        };
        Script g_scripts[] = {
            {0x002CB740u, "taCameraScript"},
            {0x002CEDA0u, "taCybertronRagdollCameraScript"},
            {0x002CD670u, "taFirstPersonCameraScript"},
            {0x002D1AB0u, "taFixedCameraScript"},
            {0x002CC580u, "taFixedLookatCameraScript"},
            {0x002CE0A0u, "taGlideCameraScript"},
            {0x002F5910u, "taVehicleCameraScript"},
            {0x002CD9D0u, "taJetPackCameraScript"},
            {0x002CFDE0u, "taRagdollCameraScript"},
            {0x002CF370u, "taReverseRagdollCameraScript"},
            {0x002CCFC0u, "taThirdPersonBehindCameraScript"},
            {0x002D0B50u, "taThirdPersonCameraScriptThree"},
            {0x002D18C0u, "taThirdPersonCreatureCameraScript"},
            {0x002CC760u, "taThirdPersonFixedOrbitCameraScript"},
            {0x002CC450u, "taThirdPersonMultiplayerCameraScript"},
            {0x002CCC60u, "taThirdPersonOrbitCameraScript"},
            {0x002CE880u, "taTransformToVehicleCameraScript"},
        };
        uint32_t g_lastObject = 0u;
        bool g_log = false;
        std::atomic<uint32_t> g_ran{0u}; // (the scripts updated since takeScripts - bits by Script)

        template <int N>
        void update(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = getRegU32(ctx, 4);
            g_ran.fetch_or(1u << N, std::memory_order_relaxed);
            if (g_log && object != g_lastObject)
            {
                g_lastObject = object;
                std::fprintf(stderr, "[cam] vblank %llu %s object=%08x ra=%08x\n",
                             static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), g_scripts[N].name, object,
                             getRegU32(ctx, 31));
            }
            g_scripts[N].original(rdram, ctx, runtime);
        }

        template <int N>
        void hookAll(PS2Runtime &runtime)
        {
            if constexpr (N < static_cast<int>(sizeof(g_scripts) / sizeof(g_scripts[0])))
            {
                if ((g_scripts[N].original = runtime.lookupFunction(g_scripts[N].update)) != nullptr)
                    runtime.registerFunction(g_scripts[N].update, update<N>);
                hookAll<N + 1>(runtime);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *on = std::getenv("TF_CAM_TRACE");
        g_log = on && *on == '1';
        hookAll<0>(runtime);
    }

    uint32_t takeScripts() { return g_ran.exchange(0u, std::memory_order_relaxed); }

    const char *scriptName(int script)
    {
        return script >= 0 && script < static_cast<int>(sizeof(g_scripts) / sizeof(g_scripts[0])) ? g_scripts[script].name : "?";
    }
}
