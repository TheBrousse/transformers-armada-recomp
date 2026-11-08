// N5 (docs/N5_PLAN.md): the render device native. nr_n5.cpp.
#pragma once

#include <cstdint>

struct R5900Context;
class PS2Runtime;

namespace nr::n5
{
    // At start-up (game thread), after nr::install: N5's own hooks. N5.0: TF_N5_CENSUS=<seconds> - who writes the display
    // list, by function, every <seconds> ("[n5:census]").
    void install(PS2Runtime &runtime);
    // (N5 step 1) A level loaded or the HQ entered / left: the geometry sets' packet summaries dropped (made again as met).
    void dropSummaries();
    // (TODO 169, TF_NATIVE_LATLOG) When the game last read the pad (steady clock, ns; 0: not watched).
    int64_t lastInputNs();
    // (N5 step 2 part 2) Whether VIF1 goes lite now (TF_N5 on, the native renderer drawing alone, the game's frames):
    // what the list's transfers will skip - the capture takes the shaders' contexts out of the list only then.
    bool liteNow();

    // N5.3: the render device's functions native - no packet; what they set handed to the renderer directly, the
    // recompiled original kept as the check's oracle and for the GS (the lost-device fallback, the game's own screens).
    // TF_N5_DEVICE: on by default (with the native renderer drawing alone, in the game's frames); =0 off; =check: the
    // original also run - into the list, rewound after - and compared ("[n5:device]"). TF_N5_DEVICE_OFF=<names, comma
    // separated>: those left to the original (setgsregister, applystateblock, usetexture, renderenvironment).
    enum class Device
    {
        SetGSRegister,
        ApplyStateBlock,
        UseTexture,
        RenderEnvironment,
        Count
    };
    bool deviceNative(Device f); // (game thread: now, for this call)
    bool deviceCheck();
    void deviceMismatch(Device f, const char *what, uint64_t native, uint64_t original); // (=check: one counted, the first logged)
    void deviceChecked(Device f);
    // The EE clock as the recompiled original would have moved it (the scheduler's safe points it passes: 32 cycles a
    // loop's back edge, 8 a call or a jump to another function; returns are free - EeScheduler::checkpointDue, the counts
    // measured against the originals in TF_N5_DEVICE=check, "EE cycles"): a native function returns through this, so
    // the game's timers, VBlanks and time slices fall where they fell with the original (without it the game's own
    // timing drifts and a run comes apart from the original's - map 3, SetHardwareRenderEnvironment). pc set to the
    // return address first; a checkpoint due now is taken at the caller's next safe point. TF_N5_CYCLES=0: not charged.
    void deviceReturn(R5900Context *ctx, PS2Runtime *runtime, uint32_t cycles);
    // (=check) The cycles an original moved the EE clock (its context's COUNT, after less before) against the count its
    // native version charges - at the clock's scale now (PS2X_EE_CYCLE_SCALE / the adaptive rate), within a cycle.
    bool deviceCyclesMatch(PS2Runtime *runtime, uint32_t charged, uint32_t spent);                                                      // (=check: one call compared)
}
