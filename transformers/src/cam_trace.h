#pragma once
// A debugging aid for the game's camera scripts (TF_CAM_TRACE=1): cam_trace.cpp.

#include <cstdint>

class PS2Runtime;

namespace cam_trace
{
    // From applyTransformers: hooks the camera scripts' updates (always - N9's turn for every picture asks which ran,
    // view_turn.cpp); TF_CAM_TRACE=1 also logs each change of script.
    void install(PS2Runtime &runtime);

    // The scripts whose update ran since the last call, a bit each (Script below), and clears them.
    enum Script
    {
        Camera, CybertronRagdoll, FirstPerson, Fixed, FixedLookat, Glide, Vehicle, JetPack, Ragdoll, ReverseRagdoll,
        ThirdPersonBehind, ThirdPersonThree, ThirdPersonCreature, ThirdPersonFixedOrbit, ThirdPersonMultiplayer, ThirdPersonOrbit,
        TransformToVehicle
    };
    uint32_t takeScripts();
    const char *scriptName(int script);
}
