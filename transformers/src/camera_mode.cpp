// First person / third person (camera_mode.h).
//
// The player's camera manager keeps up to 28 camera scripts (+0x18, a pointer each). It switches with
// 0x2C4990 (manager, script index, blend time, flag): +0x8C is the script it is on; the old script
// fades out and the new one in by their weights (+0x64; FadeTo 0x2CB920 sets +0x68 the time, +0x6C 0,
// +0x70 the weight now, +0x74 the target, +0x79 = 1; SetWeight 0x2CB910) and their switches (+0x78,
// SetActive, slot 4: +0x7A "just on"). Its update (0x2C4CE0) treats "no weight at all" as a fatal
// error. The switches between the player's views come from a chooser script (the manager's request
// points at it), not from the requests themselves. Measured (TF_CAM_TRACE=1): robot mode is
// taThirdPersonCameraScriptThree, R3's aiming view taFirstPersonCameraScript, the car
// taVehicleCameraScript.
//
// First person mode: a switch to the third-person or the car script goes to the first-person script
// instead (given the replaced script's target, +0x80 the creature and +0x84 its node, which the game
// sets on the first-person script only for R3's view). Switching the mode while playing makes the
// same 0.3 s cross-fade the switch makes, field by field, before the manager's next update.
//
// On foot the first person is the game's own aiming view (R3), not just its camera: there the
// stick strafes and R3 zooms, where the camera alone left the robot turning toward the stick and
// the view swinging with it (the user, 2026-10-23: "the aim and the camera move too much when
// walking"). So while the mode is on and the game's own view is the third-person one, R3 is
// pressed once (padButtons) to go into the aiming view; the game leaves it when the Autobot
// transforms (R3 only steps its zoom), and back on foot it is pressed again.
// TF_FIRST_PERSON_AIM=1: that (the user's choice of 2026-10-23); since TODO 190 (2026-11-08) off by default - the aiming
// state lets the robot only creep (no run, no jump).
//
// Free look (TODO 190, the default): on foot the robot keeps its normal states - walk, run, jump, as in third person - and
// the first-person view is turned by the mouse and the right stick, not by the robot's head: once the camera manager's
// update has run, the camera's transform (the vlCamera's +0x14, an mlTransform: rotation w x y z at +0x40, its matrix
// rebuilt when +0x0C is 0) is given the third-person camera control's angles (the context, actor +0x7C: pitch +0x118, yaw
// +0x11C - 0x46D410 turns them in every state). Measured in the aiming view, where the game itself looks that way: the
// rotation is Rz(yaw) x Ry(pitch), forward +X, a positive pitch looking down. The left stick goes by the camera's forward,
// so walking follows the view, and the robot turning under it no longer swings it. Its position stays the eye's. R3 /
// Circle still aim: in the aiming state the game's own view is left alone. TF_FIRST_PERSON_FREE=0: the head's view.
//
// Decomp equivalent: in the camera manager's switch (0x2C4990), the same substitution while the
// option is on.

#include "camera_mode.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace camera_mode
{
    namespace
    {
        constexpr uint32_t kSwitch = 0x002C4990u; // (manager, index, blend, flag)
        constexpr uint32_t kUpdate = 0x002C4CE0u; // (manager)
        constexpr uint32_t kFirstPersonVtable = 0x006B5FC0u;
        constexpr uint32_t kThirdPersonVtable = 0x006B6110u; // taThirdPersonCameraScriptThree
        constexpr uint32_t kVehicleVtable = 0x006B6850u;     // taVehicleCameraScript
        constexpr uint32_t kScripts = 0x18u, kCurrent = 0x8Cu, kSlots = 28u;
        constexpr float kBlend = 0.3f;

        PS2Runtime::RecompiledFunction g_switch = nullptr, g_update = nullptr, g_setState = nullptr;
        // The transformer's states (taTransformer::SetState 0x28D000: actor, state id; the states'
        // objects at actor +0x80 + 4 x id, each with its vptr at +0x18): the state the player is in.
        constexpr uint32_t kSetState = 0x0028D000u, kStates = 0x80u, kStateVptr = 0x18u;
        std::atomic<uint32_t> g_stateId{~0u}, g_stateActor{0u};
        constexpr uint32_t kAimStateVtable = 0x006BFC50u; // taTransformerStateAim (R3: state 9, measured)
        std::atomic<bool> g_firstPerson{false}; // the mode (the window thread sets it)
        bool g_applied = false;                  // the mode the view was last set for
        uint32_t g_manager = 0u;
        int g_wanted = -1; // the script the game last switched to (before our substitution)
        std::atomic<bool> g_inVehicle{false}; // the game's own view is the car's (whatever first person shows)
        std::atomic<int64_t> g_updatedMs{0};  // when the manager last updated (none in menus or the HQ)
        std::atomic<bool> g_aim{false};       // first person on foot as the aiming view (TF_FIRST_PERSON_AIM)
        std::atomic<bool> g_freeLook{true};   // first person on foot: the view by the camera control (TF_FIRST_PERSON_FREE)
        std::atomic<uint32_t> g_cameraTransform{0u}; // the current camera's mlTransform (noteCameraTransform)
        constexpr uint32_t kActorContext = 0x7Cu, kControlPitch = 0x118u, kControlYaw = 0x11Cu, kWeight = 0x64u;
        std::atomic<int> g_r3Frames{0};       // R3 to press, frames left (padButtons)
        int64_t g_r3At = 0;                   // when R3 was last pressed for it
        constexpr uint16_t kR3 = 1u << 2u;     // (TF_INPUT's mask: select, l3, r3, start, ...)

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void setWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        void setFloat(uint8_t *rdram, uint32_t address, float value)
        {
            uint32_t v;
            std::memcpy(&v, &value, sizeof(v));
            setWord(rdram, address, v);
        }

        void setByte(uint8_t *rdram, uint32_t address, uint8_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                *p = value;
        }

        uint32_t scriptAt(const uint8_t *rdram, uint32_t manager, uint32_t index)
        {
            return index < kSlots ? word(rdram, manager + kScripts + index * 4u) : 0u;
        }

        uint32_t vtableAt(const uint8_t *rdram, uint32_t manager, uint32_t index)
        {
            const uint32_t script = scriptAt(rdram, manager, index);
            return script ? word(rdram, script) : 0u;
        }

        int indexOf(const uint8_t *rdram, uint32_t manager, uint32_t vtable)
        {
            for (uint32_t i = 0; i < kSlots; ++i)
                if (vtableAt(rdram, manager, i) == vtable)
                    return static_cast<int>(i);
            return -1;
        }

        bool replaceable(uint32_t vt) { return vt == kThirdPersonVtable || vt == kVehicleVtable; }

        void copyTarget(uint8_t *rdram, uint32_t from, uint32_t to)
        {
            for (uint32_t field : {0x80u, 0x84u})
                if (const uint32_t value = word(rdram, from + field))
                    setWord(rdram, to + field, value);
        }

        void switchTo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t manager = getRegU32(ctx, 4);
            const uint32_t index = getRegU32(ctx, 5);
            g_manager = manager;
            g_wanted = static_cast<int>(index);
            if (g_firstPerson.load(std::memory_order_relaxed) && replaceable(vtableAt(rdram, manager, index)))
            {
                const int fp = indexOf(rdram, manager, kFirstPersonVtable);
                if (fp >= 0)
                {
                    copyTarget(rdram, scriptAt(rdram, manager, index), scriptAt(rdram, manager, static_cast<uint32_t>(fp)));
                    SET_GPR_U32(ctx, 5, static_cast<uint32_t>(fp));
                }
            }
            g_switch(rdram, ctx, runtime);
        }

        // The switch's cross-fade, field by field (its blend path).
        void crossFade(uint8_t *rdram, uint32_t manager, uint32_t toIndex)
        {
            const uint32_t from = scriptAt(rdram, manager, word(rdram, manager + kCurrent));
            const uint32_t to = scriptAt(rdram, manager, toIndex);
            if (!to || from == to)
                return;
            const auto weight = [rdram](uint32_t s) { uint32_t v = word(rdram, s + 0x64u); float f; std::memcpy(&f, &v, 4); return f; };
            if (from)
            {
                setFloat(rdram, from + 0x68u, kBlend); // FadeTo(0, blend)
                setWord(rdram, from + 0x6Cu, 0u);
                setFloat(rdram, from + 0x70u, weight(from));
                setFloat(rdram, from + 0x74u, 0.0f);
                setByte(rdram, from + 0x79u, 1u);
            }
            const uint8_t *on = getConstMemPtr(rdram, to + 0x78u);
            if (!(on && *on))
            {
                setByte(rdram, to + 0x7Au, 1u); // SetActive(true)
                setByte(rdram, to + 0x78u, 1u);
                setFloat(rdram, to + 0x64u, 0.0f);
            }
            setFloat(rdram, to + 0x68u, kBlend); // FadeTo(1, blend)
            setWord(rdram, to + 0x6Cu, 0u);
            setFloat(rdram, to + 0x70u, weight(to));
            setFloat(rdram, to + 0x74u, 1.0f);
            setByte(rdram, to + 0x79u, 1u);
            setWord(rdram, manager + kCurrent, toIndex);
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            const uint32_t v = word(rdram, address);
            float f;
            std::memcpy(&f, &v, sizeof(f));
            return f;
        }

        // (TODO 190) The free look: the first-person view on foot turned to the camera control's angles (the header).
        void freeLook(uint8_t *rdram, uint32_t manager)
        {
            if (!g_firstPerson.load(std::memory_order_relaxed) || !g_freeLook.load(std::memory_order_relaxed) ||
                g_aim.load(std::memory_order_relaxed))
                return;
            const uint32_t current = word(rdram, manager + kCurrent);
            const uint32_t script = scriptAt(rdram, manager, current);
            if (!script || word(rdram, script) != kFirstPersonVtable || readFloat(rdram, script + kWeight) < 0.999f)
                return; // (not the first-person view, or still fading in)
            if (g_wanted < 0 || vtableAt(rdram, manager, static_cast<uint32_t>(g_wanted)) != kThirdPersonVtable)
                return; // (the car's view, the game's own aiming view, a cutscene's...)
            const uint32_t actor = g_stateActor.load(std::memory_order_relaxed), id = g_stateId.load(std::memory_order_relaxed);
            const uint32_t stateObj = actor && id < 64u ? word(rdram, actor + kStates + 4u * id) : 0u;
            if (stateObj && word(rdram, stateObj + kStateVptr) == kAimStateVtable)
                return; // (R3 / Circle: the game's aiming view)
            const uint32_t context = actor ? word(rdram, actor + kActorContext) : 0u;
            const uint32_t transform = g_cameraTransform.load(std::memory_order_relaxed);
            if (context < 0x00100000u || context >= PS2_RAM_SIZE - 0x200u || transform < 0x00100000u || transform >= PS2_RAM_SIZE - 0x50u ||
                word(rdram, context) != actor)
                return;
            const float pitch = readFloat(rdram, context + kControlPitch), yaw = readFloat(rdram, context + kControlYaw);
            if (!std::isfinite(pitch) || !std::isfinite(yaw))
                return;
            // Rz(yaw) x Ry(pitch), stored w x y z
            const float cz = std::cos(0.5f * yaw), sz = std::sin(0.5f * yaw), cy = std::cos(0.5f * pitch), sy = std::sin(0.5f * pitch);
            setFloat(rdram, transform + 0x40u, cz * cy);
            setFloat(rdram, transform + 0x44u, -sz * sy);
            setFloat(rdram, transform + 0x48u, cz * sy);
            setFloat(rdram, transform + 0x4Cu, sz * cy);
            setByte(rdram, transform + 0x0Cu, 0u); // (its matrix rebuilt)
        }

        void setStateFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t actor = getRegU32(ctx, 4), id = getRegU32(ctx, 5);
            g_stateId.store(id, std::memory_order_relaxed);
            g_stateActor.store(actor, std::memory_order_relaxed);
            static const bool trace = std::getenv("TF_STATE_TRACE") != nullptr;
            if (trace)
            {
                const uint32_t state = id < 64u ? word(rdram, actor + kStates + 4u * id) : 0u;
                std::fprintf(stderr, "[camera] SetState actor %08x id %u (state %08x vptr %08x)\n", actor, id, state,
                             state ? word(rdram, state + kStateVptr) : 0u);
            }
            g_setState(rdram, ctx, runtime);
        }

        void update(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t manager = getRegU32(ctx, 4);
            if (manager != g_manager)
            {
                g_manager = manager; // (a new level, or a state loaded)
                g_wanted = static_cast<int>(word(rdram, manager + kCurrent));
            }
            const bool want = g_firstPerson.load(std::memory_order_relaxed);
            if (want != g_applied)
            {
                const uint32_t current = word(rdram, manager + kCurrent);
                const int fp = indexOf(rdram, manager, kFirstPersonVtable);
                if (want && fp >= 0 && replaceable(vtableAt(rdram, manager, current)))
                {
                    g_wanted = static_cast<int>(current);
                    copyTarget(rdram, scriptAt(rdram, manager, current), scriptAt(rdram, manager, static_cast<uint32_t>(fp)));
                    crossFade(rdram, manager, static_cast<uint32_t>(fp));
                }
                else if (!want && fp >= 0 && current == static_cast<uint32_t>(fp) && g_wanted >= 0 &&
                         replaceable(vtableAt(rdram, manager, static_cast<uint32_t>(g_wanted))))
                    crossFade(rdram, manager, static_cast<uint32_t>(g_wanted));
                g_applied = want;
                std::fprintf(stderr, "[camera] %s\n", want ? "first person" : "third person");
            }
            const uint32_t wantedVt = g_wanted >= 0 ? vtableAt(rdram, manager, static_cast<uint32_t>(g_wanted)) : 0u;
            g_inVehicle.store(wantedVt == kVehicleVtable, std::memory_order_relaxed);
            // (on foot in first person and not aiming: into the aiming view, once a second at most)
            const uint32_t actor = g_stateActor.load(std::memory_order_relaxed), id = g_stateId.load(std::memory_order_relaxed);
            const uint32_t stateObj = actor && id < 64u ? word(rdram, actor + kStates + 4u * id) : 0u;
            const bool aiming = stateObj && word(rdram, stateObj + kStateVptr) == kAimStateVtable;
            if (want && g_aim.load(std::memory_order_relaxed) && !aiming && wantedVt == kThirdPersonVtable && nowMs() - g_r3At > 1000)
            {
                g_r3At = nowMs();
                g_r3Frames.store(4, std::memory_order_relaxed);
            }
            g_updatedMs.store(nowMs(), std::memory_order_relaxed);
            const uint32_t ra = getRegU32(ctx, 31);
            g_update(rdram, ctx, runtime); // (our writes come first: the recompiled update may stop part way)
            if (ctx->pc == ra) // (it finished: the camera placed for the frame)
                freeLook(rdram, manager);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_FIRST_PERSON"); env && *env)
            g_firstPerson.store(*env != '0');
        if (const char *env = std::getenv("TF_FIRST_PERSON_AIM"); env && *env)
            g_aim.store(*env != '0');
        if (const char *env = std::getenv("TF_FIRST_PERSON_FREE"); env && *env)
            g_freeLook.store(*env != '0');
        if ((g_switch = runtime.lookupFunction(kSwitch)) != nullptr)
            runtime.registerFunction(kSwitch, switchTo);
        if ((g_update = runtime.lookupFunction(kUpdate)) != nullptr)
            runtime.registerFunction(kUpdate, update);
        if ((g_setState = runtime.lookupFunction(kSetState)) != nullptr)
            runtime.registerFunction(kSetState, setStateFn);
    }

    bool firstPerson() { return g_firstPerson.load(); }

    uint16_t padButtons()
    {
        int left = g_r3Frames.load(std::memory_order_relaxed);
        if (left <= 0)
            return 0u;
        g_r3Frames.store(left - 1, std::memory_order_relaxed);
        return kR3;
    }

    void setAim(bool on) { g_aim.store(on); }
    void noteCameraTransform(uint32_t transform) { g_cameraTransform.store(transform, std::memory_order_relaxed); }
    bool aim() { return g_aim.load(); }
    bool inVehicle()
    {
        // (a view not updated for half a second: no level running, not the car)
        return g_inVehicle.load(std::memory_order_relaxed) && nowMs() - g_updatedMs.load(std::memory_order_relaxed) < 500;
    }
    void setFirstPerson(bool on) { g_firstPerson.store(on); }

    void frame(uint8_t *, bool) {}
}
