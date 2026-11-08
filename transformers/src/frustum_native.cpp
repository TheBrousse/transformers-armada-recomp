// mlxFrustum::Test (0x1B2DD0) in one native call (TODO 94). See frustum_native.h.
//
// What it is: Test(frustum a0, box a1, clip status a2) - the box against the frustum's bounding sphere
// (mlxAlignedBox::Test of a sphere, 0x1B2760, when the flag at gp-0x5CC8 asks for it), then against each of
// the six planes still set in *a2 (mlxAlignedBox::Test of a plane, 0x1B2630): 0 outside, 1 crossing, 2 inside
// (a plane the box lies wholly inside is cleared from *a2). Deep Amazon calls it ~200 M times a run, most from
// the forest (0x430190, 0x429510); the work is a few loads and multiplies, the cost the seven calls a box made
// through the dispatcher.
//
// Same bits: this is the generated code of the three functions joined, step for step - the same loads and
// stores (the stack frame's saves included) through the same READ/WRITE paths, the same FPU helpers
// (FPU_MUL_S, FPU_ADD_S_NK, ...) in the same order, every register the guest code writes written the same way
// (the low 64 bits, the upper kept: the register cache's RC_SET_*), and fcr31's condition bit as left by the
// last compare. The EE clock and its checkpoints are the dispatcher's own: each box test inside
// PS2Runtime::leafBegin / leafEnd (what dispatchGuestCall does before and after the call), the loop's back edge
// through eeCheckpointDue - so a checkpoint that comes due finds the context exactly as the recompiled code
// leaves it there, and the call returns as that code returns. A call resumed inside the function (ctx->pc not
// its start: a checkpoint unwound it) goes to the recompiled function, which picks it up at its label; so does
// any call while either box test is not the recompiled one (another replacement is kept).
//
// Each function here is named after the game's own (mlxFrustum::Test / mlxAlignedBox::Test, at the same
// addresses), so another replacement of them can take their place - install() then finds the three replaced and
// leaves them to it.

#include "frustum_native.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_leaf_call.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

void sub_001B2630_0x1b2630(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void sub_001B2760_0x1b2760(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void sub_001B2DD0_0x1b2dd0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

namespace frustum_native
{
    namespace
    {
        constexpr uint32_t kTest = 0x001B2DD0u, kPlane = 0x001B2630u, kSphere = 0x001B2760u;
        constexpr uint32_t kAfterSphere = 0x001B2E20u, kLoop = 0x001B2E3Cu, kAfterPlane = 0x001B2E64u;
        constexpr uint32_t kCond = 0x800000u; // (fcr31's condition bit)

        PS2Runtime::RecompiledFunction g_test = nullptr; // (the recompiled Test, for the calls this does not take)

        // The general registers as the register cache writes them: the low 64 bits, the upper 64 kept.
        PS2_FORCEINLINE uint64_t lo(const R5900Context *ctx, int n)
        {
            uint64_t v;
            std::memcpy(&v, &ctx->r[n], sizeof(v));
            return v;
        }
        PS2_FORCEINLINE uint32_t lo32(const R5900Context *ctx, int n) { return static_cast<uint32_t>(lo(ctx, n)); }
        PS2_FORCEINLINE void setLo(R5900Context *ctx, int n, uint64_t v) { std::memcpy(&ctx->r[n], &v, sizeof(v)); }
        PS2_FORCEINLINE void setS32(R5900Context *ctx, int n, uint32_t v)
        {
            setLo(ctx, n, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))));
        }
        PS2_FORCEINLINE float loadF(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr)
        {
            const uint32_t bits = READ32(addr);
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }
        PS2_FORCEINLINE void setCond(R5900Context *ctx, bool c) { ctx->fcr31 = c ? (ctx->fcr31 | kCond) : (ctx->fcr31 & ~kCond); }

        // 0x1B2630, mlxAlignedBox::Test(const mlxPlane &) const (decomp: Test__13mlxAlignedBoxCFRC8mlxPlane), box a0,
        // plane a1: the box's nearest and farthest corner along the plane's normal against its distance. f0..f4,
        // fcr31's bit and v0 as the guest code leaves them.
        PS2_FORCEINLINE void mlxAlignedBox_TestPlane(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t box = lo32(ctx, 4), plane = lo32(ctx, 5);
            float f0, f1, f2, f3, f4;
            f3 = loadF(rdram, ctx, runtime, ADD32(plane, 0));
            f0 = loadF(rdram, ctx, runtime, ADD32(plane, 12));
            f1 = 0.0f;
            bool c = FPU_C_OLT_S(f3, f1);
            f0 = FPU_NEG_S(f0);
            f4 = FPU_MOV_S(f0);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                if (axis != 0u)
                {
                    f3 = loadF(rdram, ctx, runtime, ADD32(plane, 4u * axis));
                    f1 = 0.0f;
                    c = FPU_C_OLT_S(f3, f1);
                }
                if (!c)
                {
                    f2 = loadF(rdram, ctx, runtime, ADD32(box, 4u * axis));
                    f1 = loadF(rdram, ctx, runtime, ADD32(box, 16u + 4u * axis));
                }
                else
                {
                    f2 = loadF(rdram, ctx, runtime, ADD32(box, 16u + 4u * axis));
                    f1 = loadF(rdram, ctx, runtime, ADD32(box, 4u * axis));
                }
                f2 = FPU_MUL_S(f3, f2);
                f1 = FPU_MUL_S(f3, f1);
                f0 = FPU_ADD_S_NK(f0, f2);
                f4 = FPU_ADD_S_NK(f4, f1);
            }
            f1 = 0.0f;
            uint64_t v0;
            c = FPU_C_OLT_S(f4, f1);
            if (c)
                v0 = 0u;
            else
            {
                c = FPU_C_OLT_S(f0, f1);
                v0 = c ? 1u : 2u;
            }
            setCond(ctx, c);
            ctx->f[0] = f0;
            ctx->f[1] = f1;
            ctx->f[2] = f2;
            ctx->f[3] = f3;
            ctx->f[4] = f4;
            setLo(ctx, 2, v0);
            ctx->pc = lo32(ctx, 31);
        }

        // 0x1B2760, mlxAlignedBox::Test(const mlxSphere &) const (decomp: Test__13mlxAlignedBoxCFRC9mlxSphere), box a0,
        // sphere a1: the box's point nearest the centre within the radius.
        PS2_FORCEINLINE void mlxAlignedBox_TestSphere(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t box = lo32(ctx, 4), sphere = lo32(ctx, 5);
            float f0, f1, f2, f3, f4, f5, f6;
            f0 = loadF(rdram, ctx, runtime, ADD32(box, 24));
            f2 = loadF(rdram, ctx, runtime, ADD32(sphere, 8));
            f1 = loadF(rdram, ctx, runtime, ADD32(box, 8));
            f6 = FPU_MIN_S(f2, f0);
            f6 = FPU_MAX_S(f6, f1);
            f0 = loadF(rdram, ctx, runtime, ADD32(box, 20));
            f1 = loadF(rdram, ctx, runtime, ADD32(sphere, 4));
            f3 = loadF(rdram, ctx, runtime, ADD32(box, 4));
            f5 = FPU_MIN_S(f1, f0);
            f5 = FPU_MAX_S(f5, f3);
            f3 = loadF(rdram, ctx, runtime, ADD32(box, 16));
            f0 = loadF(rdram, ctx, runtime, ADD32(sphere, 0));
            f4 = loadF(rdram, ctx, runtime, ADD32(box, 0));
            f3 = FPU_MIN_S(f0, f3);
            f3 = FPU_MAX_S(f3, f4);
            f0 = FPU_SUB_S(f0, f3);
            f1 = FPU_SUB_S(f1, f5);
            f2 = FPU_SUB_S(f2, f6);
            FPU_SET_ACC(ctx, FPU_MUL_S_KK(f0, f0));
            FPU_SET_ACC(ctx, FPU_ADD_S_KK(ctx->f_acc, FPU_MUL_S_KK(f1, f1)));
            f1 = FPU_ADD_S_KK(ctx->f_acc, FPU_MUL_S_KK(f2, f2));
            f0 = loadF(rdram, ctx, runtime, ADD32(sphere, 16));
            f0 = FPU_MUL_S(f0, f0);
            const bool c = FPU_C_OLE_S(f1, f0);
            setCond(ctx, c);
            ctx->f[0] = f0;
            ctx->f[1] = f1;
            ctx->f[2] = f2;
            ctx->f[3] = f3;
            ctx->f[4] = f4;
            ctx->f[5] = f5;
            ctx->f[6] = f6;
            setLo(ctx, 2, c ? 2u : 0u);
            ctx->pc = lo32(ctx, 31);
        }

        // The call of a box test at a `jal` of Test: dispatchGuestCall's before and after (the checkpoint, the
        // call's level, its end) around the body. false: the call did not come back here (a checkpoint, a stop) -
        // Test returns as the recompiled one does.
        template <void (*Body)(uint8_t *, R5900Context *, PS2Runtime *)>
        PS2_FORCEINLINE bool callBox(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t target, uint32_t fallthrough)
        {
            ctx->pc = target;
            const uint32_t level = runtime->leafBegin(ctx, target);
            if (level == 0u)
                return false;
            Body(rdram, ctx, runtime);
            return runtime->leafEnd(rdram, ctx, target, fallthrough, level);
        }

        // 0x1B2DD0, mlxFrustum::Test(const mlxAlignedBox &, mlxFrustum::ClipStatus &) const
        // (decomp: Test__10mlxFrustumCFRC13mlxAlignedBoxRQ210mlxFrustum10ClipStatus).
        void mlxFrustum_Test(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kTest || PS2_CALL_TARGET_IN(kPlane) != &sub_001B2630_0x1b2630 ||
                PS2_CALL_TARGET_IN(kSphere) != &sub_001B2760_0x1b2760) [[unlikely]]
            {
                g_test(rdram, ctx, runtime);
                return;
            }
            // addiu $sp, -0x60; sd $ra / sq $s4, $s3 (then $s4 = a0), $s2 (then $s3 = a1), $s1, $s0
            const uint32_t sp = ADD32(lo32(ctx, 29), 4294967200u);
            setS32(ctx, 29, sp);
            WRITE64(ADD32(sp, 80), lo(ctx, 31));
            WRITE128(ADD32(sp, 64), ctx->r[20]);
            WRITE128(ADD32(sp, 48), ctx->r[19]);
            setLo(ctx, 20, lo(ctx, 4));
            WRITE128(ADD32(sp, 32), ctx->r[18]);
            setLo(ctx, 19, lo(ctx, 5));
            WRITE128(ADD32(sp, 16), ctx->r[17]);
            WRITE128(ADD32(sp, 0), ctx->r[16]);
            setS32(ctx, 2, READ32(lo32(ctx, 6)));
            setLo(ctx, 18, lo(ctx, 6));
            bool loadRa = true;
            if (lo(ctx, 2) == 0u)
            {
                setS32(ctx, 2, 2u);
                goto epilogue;
            }
            setLo(ctx, 2, static_cast<uint8_t>(READ8(ADD32(lo32(ctx, 28), 4294943544u))));
            setLo(ctx, 17, 0u);
            if (lo(ctx, 2) != 0u)
            {
                setLo(ctx, 4, lo(ctx, 19));
                setS32(ctx, 31, kAfterSphere);
                setLo(ctx, 5, lo(ctx, 20));
                if (!callBox<mlxAlignedBox_TestSphere>(rdram, ctx, runtime, kSphere, kAfterSphere))
                    return;
                if (lo(ctx, 2) == 0u)
                {
                    setLo(ctx, 2, 0u);
                    goto epilogue;
                }
                setLo(ctx, 17, 0u);
            }
            setLo(ctx, 2, static_cast<int64_t>(lo(ctx, 17)) < 6 ? 1u : 0u);
            while (lo(ctx, 2) != 0u)
            {
                ctx->pc = kLoop;
                if (runtime->eeCheckpointDue())
                    return;
                // 0x1B2E3C: the plane's bit in *s2
                setS32(ctx, 2, READ32(lo32(ctx, 18)));
                setS32(ctx, 3, 1u);
                setS32(ctx, 16, SLL32(lo32(ctx, 3), lo32(ctx, 17) & 0x1Fu));
                setLo(ctx, 2, lo(ctx, 2) & lo(ctx, 16));
                const bool skip = lo(ctx, 2) == 0u;
                setS32(ctx, 2, SLL32(lo32(ctx, 17), 4));
                if (!skip)
                {
                    setLo(ctx, 4, lo(ctx, 19));
                    setS32(ctx, 2, ADD32(lo32(ctx, 20), lo32(ctx, 2)));
                    setS32(ctx, 31, kAfterPlane);
                    setS32(ctx, 5, ADD32(lo32(ctx, 2), 32u));
                    if (!callBox<mlxAlignedBox_TestPlane>(rdram, ctx, runtime, kPlane, kAfterPlane))
                        return;
                    // 0x1B2E64
                    const bool hit = lo(ctx, 2) != 0u;
                    setS32(ctx, 3, 2u);
                    if (!hit)
                    {
                        setLo(ctx, 2, 0u);
                        goto epilogue;
                    }
                    if (lo(ctx, 2) == lo(ctx, 3))
                    {
                        // wholly inside this plane: its bit cleared
                        setS32(ctx, 2, READ32(lo32(ctx, 18)));
                        setLo(ctx, 2, lo(ctx, 2) ^ lo(ctx, 16));
                        WRITE32(lo32(ctx, 18), lo32(ctx, 2));
                        setS32(ctx, 2, READ32(lo32(ctx, 18)));
                        const bool more = lo(ctx, 2) != 0u;
                        setLo(ctx, 2, lo(ctx, 3));
                        if (!more)
                        {
                            setLo(ctx, 31, READ64(ADD32(lo32(ctx, 29), 80)));
                            loadRa = false;
                            goto epilogue;
                        }
                    }
                }
                // 0x1B2E9C
                setS32(ctx, 17, ADD32(lo32(ctx, 17), 1u));
                setLo(ctx, 2, static_cast<int64_t>(lo(ctx, 17)) < 6 ? 1u : 0u);
            }
            setS32(ctx, 4, READ32(lo32(ctx, 18)));
            setS32(ctx, 2, 2u);
            setS32(ctx, 3, 1u);
            if (lo(ctx, 4) != 0u)
                setLo(ctx, 2, lo(ctx, 3));
        epilogue:
            {
                const uint32_t frame = lo32(ctx, 29);
                if (loadRa)
                    setLo(ctx, 31, READ64(ADD32(frame, 80)));
                ctx->r[20] = READ128(ADD32(frame, 64));
                ctx->r[19] = READ128(ADD32(frame, 48));
                ctx->r[18] = READ128(ADD32(frame, 32));
                ctx->r[17] = READ128(ADD32(frame, 16));
                ctx->r[16] = READ128(ADD32(frame, 0));
                const uint32_t target = lo32(ctx, 31);
                setS32(ctx, 29, ADD32(frame, 96u));
                ctx->pc = target;
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
#if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        return; // (the generated returns go through the dispatcher there; this does not)
#else
        if (const char *e = std::getenv("TF_FRUSTUM_NATIVE"); e && *e == '0')
            return;
        g_test = runtime.lookupFunction(kTest);
        if (g_test != &sub_001B2DD0_0x1b2dd0 || runtime.lookupFunction(kPlane) != &sub_001B2630_0x1b2630 ||
            runtime.lookupFunction(kSphere) != &sub_001B2760_0x1b2760)
        {
            std::fprintf(stderr, "[frustum] mlxFrustum::Test left recompiled: it or a box test is replaced\n");
            return;
        }
        runtime.registerFunction(kTest, mlxFrustum_Test);
        std::fprintf(stderr, "[frustum] mlxFrustum::Test native (TF_FRUSTUM_NATIVE=0: the recompiled one)\n");
#endif
    }
}
