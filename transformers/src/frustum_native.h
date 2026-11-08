#pragma once
// mlxFrustum::Test (0x1B2DD0) in one native call (frustum_native.cpp, TODO 94). The game's view-frustum
// test of a box - the forest's trees, the scenery's cells - with its two box tests (mlxAlignedBox::Test of
// a plane, 0x1B2630, and of a sphere, 0x1B2760) made in place instead of through the dispatcher: the same
// guest code, step for step, with the same EE clock and checkpoints. TF_FRUSTUM_NATIVE=0: the recompiled
// function as before.

class PS2Runtime;

namespace frustum_native
{
    // After decomp::install (a replaced box test leaves the recompiled path in use).
    void install(PS2Runtime &runtime);
}
