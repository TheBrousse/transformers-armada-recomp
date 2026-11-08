#pragma once
// A decompilation aid (sirens_trace.cpp): TF_SIRENS_LOG=<file> records every SIF RPC call the
// game makes to the IOP (its sound server SIRENS3 above all): the server it was bound to, the
// function number, mode, sizes, the calling EE function and the bytes sent (and, for a call that
// waits, the bytes received). The protocol a native sound backend has to answer.

class PS2Runtime;

namespace sirens_trace
{
    // From applyTransformers, before anything that adds guest code (it adds none).
    void install(PS2Runtime &runtime);
}
