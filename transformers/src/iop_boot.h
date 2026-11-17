#pragma once
// The IOP-less boot (iop_boot.cpp): the "emulation out" plan's step 2 (S2). With the native sound server and the native
// disc the IOP has nothing to do once the game has booted (S1, iop_tripwire.h: no call reaches it after); here it is not
// even booted - its modules are not loaded, its processor never runs. The only boot on the USA disc (TODO 388 E: the
// IOP's boot, TF_IOP_BOOT=1, and the emulated sound and disc choices are gone); the European disc keeps the IOP's boot
// until it is tested without (TF_IOP_BOOT=0: without the IOP there; the native sound server and disc serve both boots).

#include <cstdint>

class PS2Runtime;

namespace iop_boot
{
    // From applyTransformers, after native_disc::install and native_sound::install (it asks both whether they are on).
    void install(PS2Runtime &runtime);
    // True when the session boots without the IOP.
    bool active();
    // From the port's _sceSifLoadModuleBuffer (overrides.cpp): the module at IOP memory `iopAddr` answered as if loaded -
    // its module id and start result as the IOP's own load gives them - without loading it. False: load it on the IOP.
    bool takeModule(PS2Runtime &runtime, uint8_t *rdram, uint32_t iopAddr, int32_t &moduleId, int32_t &startResult);
    // A save state loaded (overrides.cpp): told when it does not fit this session's boot.
    void stateLoaded(PS2Runtime &runtime);
}
