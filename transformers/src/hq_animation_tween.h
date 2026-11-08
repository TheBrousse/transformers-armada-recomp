#pragma once
#include <cstddef>
#include <cstdint>
class PS2Runtime;
struct R5900Context;
namespace hq_animation_tween {
// No guest allocations. Preserve the original SetEnabled and its continuation.
bool install(PS2Runtime&);
bool preserve_pending(const uint8_t*,std::size_t,uint32_t controller,uint32_t caller);
void dispatch(uint8_t*,R5900Context*,PS2Runtime*);
// The Decepticon HQ shows (an extension's answer, set before install): Megatron's treads blend only there. None: never.
void setDecepticonHQ(bool (*active)(const uint8_t*));
}
