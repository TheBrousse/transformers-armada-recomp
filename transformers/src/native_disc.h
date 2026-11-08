#pragma once
// The native TwoFish disc server (native_disc.cpp): the game reads its disc through TwoFish, its
// own IOP module ("CD/DVD server v1.6"), by LSN. Every read request the EE sends is answered on
// the host instead, from the runtime's disc (the same LSNs the IOP's CD reads see), so the IOP
// is no longer needed for the disc. On by default; TF_NATIVE_DISC=0 sends the requests to the
// IOP's TwoFish as before.

class PS2Runtime;

namespace native_disc
{
    // From applyTransformers (it adds no guest code).
    void install(PS2Runtime &runtime);
    // True when the reads are answered on the host.
    bool active();
}
