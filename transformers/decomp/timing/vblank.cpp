// Frame timing: the wait for the VBlank at the end of a frame.

#include "timing.h"

#include "port.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_host_display.h"

namespace tf::timing
{
    namespace
    {
        constexpr uint32_t kWaitLoop = 0x25DEE8u;

        // The loop's state is where the original keeps it, so that it survives a yield (and
        // a save state taken during one).
        constexpr int kReached = 4;    // a0
        constexpr int kTwoFields = 5;  // a1
        constexpr int kTarget = 6;     // a2
        constexpr int kStartField = 3; // v1
        constexpr int kResult = 2;     // v0

        // PC: the VBlank count the frame now beginning began at (for port::lateFramesTakeNextVBlank).
        int32_t g_frameBegan = 0;

        // After the wait: with frames of two fields, the next frame starts on the other one.
        void FinishWait(decomp::Guest &g)
        {
            if (framesOfTwoFields != 0 && alternateFields != 0)
                nextFrameField = nextFrameField == 0 ? 1 : 0;
            const int32_t count = vblankCount;
            g_frameBegan = count;
            g.setGpr(kResult, static_cast<uint32_t>(count));
            port::vblankWaitEnds(g.cop0Count());
            if (ps2_host_display::tracing())
                ps2_host_display::trace("waited for VBlank", g.runtime()->memory().gs().dispfb2);
            g.returnToCaller();
        }
    }

    // 0x25DE90: waits for the VBlank the next frame is to start at, and returns the VBlank
    // count. That is the next VBlank - or, when the game counts from its last flip, the one
    // after it, or with frames of two fields the second one after it, and then (alternating
    // fields) only on the field the next frame is to start on. The original polls the
    // VBlank counter in a loop (0x25DEE8-0x25DF1C).
    void WaitVBlank(decomp::Guest &g)
    {
        if (g.pc() == kWaitLoop)
        {
            // PC: entered again at the loop after a yield, the EE clock jumps to the next event
            // instead of going round the loop until it comes.
            g.runtime()->eeScheduler().skipIdleCycles();
        }
        else
        {
            ps2_host_display::trace("wait for VBlank", g.runtime()->memory().gs().dispfb2);
            port::vblankWaitBegins(g.cop0Count());
            // PC: stop at the loop's first back edge, where the clock can jump.
            g.runtime()->eeScheduler().requestCheckpoint();
            if (waitForVBlank == 0)
            {
                FinishWait(g);
                return;
            }
            const uint8_t twoFields = framesOfTwoFields;
            int32_t target = vblankCount + 1;
            if (countFromFlipB != 0 || countFromFlipA != 0)
                target = lastFlipVBlank + (twoFields != 0 ? 2 : 1);
            g.setGpr(kReached, 0u);
            g.setGpr(kTwoFields, twoFields);
            g.setGpr(kTarget, static_cast<uint32_t>(target));
            g.setGpr(kStartField, static_cast<uint32_t>(static_cast<int32_t>(nextFrameField)));
            // The first pass goes round the back edge before it looks at the counter.
            g.setGpr(kResult, 1u);
            if (g.backEdge(kWaitLoop))
                return;
        }
        for (;;)
        {
            uint32_t reached = g.gpr(kReached);
            if (!(vblankCount < static_cast<int32_t>(g.gpr(kTarget))))
                reached = 1u;
            if (g.gpr(kTwoFields) != 0u && (currentField ^ port::fieldOffset()) != static_cast<int32_t>(g.gpr(kStartField)))
            {
                // PC: a frame already two VBlanks old starts now, on the other field (the port's
                // offset flips; the pictures follow it) - not a whole frame later.
                if (reached != 0u && port::lateFramesTakeNextVBlank() && vblankCount >= g_frameBegan + 2)
                    port::flipFieldOffset();
                else
                    reached = 0u; // (the right VBlank, but not yet the right field)
            }
            g.setGpr(kReached, reached);
            if (reached != 0u)
                break;
            g.setGpr(kResult, 1u);
            if (g.backEdge(kWaitLoop))
                return;
        }
        FinishWait(g);
    }

    DECOMP_YIELDING(0x25DE90, WaitVBlank, "timing", kWaitLoop);
}
