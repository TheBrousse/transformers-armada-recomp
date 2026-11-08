// Frame timing: the governor - once a frame, after the frame's drawing, the game looks at how
// long the frame before took, chooses between frames of two fields and frames of one, and
// scales the detail to the load. (First decompiled by an analysis agent from the generated
// code; made resumable here.)
//
// - "Two fields" (wantTwoFields, 1 at boot) makes every frame last a pair of fields:
//   framesOfTwoFields = 1, the VBlank wait (0x25DE90) counts from now and only ends on field
//   0, and the in-between field is drawn halfway (inBetweenFactor 0.5). The shipped game's 30
//   frames a second. "One field" (0) lets a frame end at the first VBlank after the last flip
//   (countFromFlipA) and draws no in-between (factor 1).
// - The measure: the EE's time in the frame before (two profiler times) and the display
//   list's time on the GPU (VIF1 and the GS), each against the period (1 / the target rate).
//   With two fields, a frame where either comes within 0.2 ms of the period goes to one field
//   at once (two such frames in a row while countFromFlipB is set, which no code sets). With
//   one field, after 3 frames (not necessarily in a row) where both are more than 1.4 ms
//   under, back to two. So two fields while it fits, else frames that end at any VBlank.
// - The detail scale (read by a dozen of the game's systems): with two fields down 0.005 a
//   frame while either time is within 0.8 ms, up 0.005 while both are 1.4 ms under; with one
//   field down 0.005 every frame; always within 0.5 .. 1.
// - Held at 30 (GovernorHoldsAtThirty: from boot until a level is set up, and while one loads)
//   or switched off (the menus): two fields and the detail at 1, without looking.
// - Debug displays, all off in the shipped game: the detail as a bar with "Load balancing
//   detail %5.0f%%", the measured rate "%5.1fHz", frame stamps and a colour for frames over
//   the period; and a switch between two ways of showing fields that no code turns on (the
//   governor writes its "off" values every frame).
//
// Resumable (decomp/README.md): its locals are in the registers the original keeps them in,
// its frame (0xD0 bytes) on the guest stack as the original's, and each call returns to the
// original's return address, where the governor is entered again if the callee stopped.

#include "timing.h"

#include "port.h"

#include <cstdio>

namespace tf::timing
{
    namespace
    {
        // --- What the governor measures (seconds) ----------------------------------------
        // 0x25CFE0, at a frame's start (from 0x25BE20), copies them from where they were
        // measured: they are the frame before's.
        inline constexpr Var<f32> eeTimeInFrame{0x006CEF78u};       // gp-0x4178: the display list's kick to the end of the frame's drawing
        inline constexpr Var<f32> eeTimeBetweenFrames{0x006CEF7Cu}; // gp-0x4174: from there to the next frame's wait for the GPU
        inline constexpr Var<f32> measuredFramePeriod{0x006CEF90u}; // gp-0x4160: between the last two frame starts
        inline constexpr Var<f32> gpuTime{0x006CEF98u};             // gp-0x4158: the display list's, from its kick to its end
        inline constexpr Var<f32> physicsTime{0x006CEF80u};         // gp-0x4170: summed by 0x2C60D0 and 0x3C9E90 (probably the physics)

        // --- Its settings and state ---------------------------------------------------------
        inline constexpr Var<uint8_t> governorOn{0x006CD96Cu};             // gp-0x5784: the menus save and clear it
        inline constexpr Var<int32_t> framesWithRoom{0x006CF094u};         // gp-0x405C: frames at one field with both times 1.4 ms under
        inline constexpr Var<int32_t> framesLate{0x006CF098u};             // gp-0x4058: frames in a row at two fields within 0.2 ms
        inline constexpr Var<int32_t> roomyFramesToTwoFields{0x006CD988u}; // gp-0x5768: 3
        inline constexpr Var<int32_t> lateFramesToOneField{0x006CD98Cu};   // gp-0x5764: 2 (only while countFromFlipB is set)
        inline constexpr Var<f32> lateMargin{0x006CD95Cu};                 // gp-0x5794: 0.0002
        inline constexpr Var<f32> tightMargin{0x006CD960u};                // gp-0x5790: 0.0008
        inline constexpr Var<f32> roomMargin{0x006CD964u};                 // gp-0x578C: 0.0014
        inline constexpr Var<f32> detailScaleMin{0x006CD978u};             // gp-0x5778: 0.5
        inline constexpr Var<f32> detailStepDown{0x006CD97Cu};             // gp-0x5774: 0.005 (a level's begin may set it to 0)
        inline constexpr Var<f32> detailStepUp{0x006CD980u};               // gp-0x5770: 0.005 (likewise)
        inline constexpr Var<uint8_t> countFromFlipAtOneField{0x006CD970u}; // gp-0x5780: 1
        inline constexpr Var<uint8_t> noInBetweenWhenLate{0x006CD950u};    // gp-0x57A0: 1
        inline constexpr Var<uint8_t> inBetweenOn{0x006CD968u};            // gp-0x5788: written only here

        // The switch between two ways of showing fields (generic names; no code turns it on).
        inline constexpr Var<uint8_t> fieldSwitchOn{0x006CF080u};         // gp-0x4070
        inline constexpr Var<f32> fieldSwitchLateMargin{0x006CD954u};     // gp-0x579C: 0.0002
        inline constexpr Var<f32> fieldSwitchRoomMargin{0x006CD958u};     // gp-0x5798: 0.001
        inline constexpr Var<uint8_t> fieldSwitchActive{0x006CF078u};     // gp-0x4078
        inline constexpr Var<uint8_t> fieldSwitchAlternates{0x006CF07Cu}; // gp-0x4074
        inline constexpr Var<uint8_t> flipInVBlankHandler{0x006CF008u};   // gp-0x40E8: the VBlank handler (0x25DF60) sets the display from the per-field table
        inline constexpr Var<uint8_t> renderFlag6CD800{0x006CD800u};      // gp-0x58F0: read by a render pass (0x2448F0); unknown

        // --- Debug displays -------------------------------------------------------------------
        inline constexpr Var<uint8_t> showDetailBar{0x006CF090u};      // gp-0x4060
        inline constexpr Var<uint8_t> showFrameStats{0x006CD898u};     // gp-0x5858: cleared by main's set-up (0x2767A0)
        inline constexpr Var<uint8_t> showFrameStatsHeld{0x006CEE34u}; // gp-0x42BC
        inline constexpr Var<uint8_t> printRateWhenOff{0x006CF08Cu};   // gp-0x4064
        inline constexpr Var<uint8_t> printRateAlways{0x006CF088u};    // gp-0x4068
        inline constexpr Var<uint8_t> capturingScreen{0x006CEF04u};    // gp-0x41EC: a tiled screenshot in progress
        inline constexpr Var<int32_t> renderedFrames{0x006CEE2Cu};     // gp-0x42C4
        inline constexpr Var<int32_t> eeLateFrame{0x006CD998u};        // gp-0x5758
        inline constexpr Var<int32_t> gpuLateFrame{0x006CD99Cu};       // gp-0x5754
        inline constexpr Var<int32_t> physicsLateFrame{0x006CD9A0u};   // gp-0x5750
        inline constexpr Var<uint32_t> eeLateColour{0x006CD990u};      // gp-0x5760
        inline constexpr Var<uint32_t> gpuLateColour{0x006CD994u};     // gp-0x575C
        inline constexpr Var<uint32_t> lateFrameColour{0x006CD89Cu};   // gp-0x5854
        inline constexpr Var<uint32_t> consoleText{0x006CF084u};       // gp-0x406C: the renderer's text console
        inline constexpr Var<f32> overlayBottom{0x0070F06Cu};          // the screen's height (0x25B250)
        constexpr uint32_t kDetailFormat = 0x006310EAu;                // "Load balancing detail %5.0f%%"
        constexpr uint32_t kRateFormat = 0x00631108u;                  // "%5.1fHz"

        inline constexpr Var<uint32_t> renderer{0x006CE990u};  // gp-0x4760 (vtable 0x6B4450)
        inline constexpr Var<f32> ntscFieldRate{0x006CB200u};  // gp-0x7EF0: 59.9400558
        inline constexpr Var<f32> almostOne{0x006CB224u};      // gp-0x7ECC: 0.99
        inline constexpr Var<f32> onePercent{0x006CB124u};     // gp-0x7FCC: 0.01

        // Methods (vtable slots) and functions it calls.
        constexpr uint32_t kSetScheme = 0x9Cu;      // the renderer's: 0 = the default scheme (0x25ACD0)
        constexpr uint32_t kDisplayMode = 0x144u;   // the renderer's: the display mode (-1: the current one) into a0 (0x25DD50)
        constexpr uint32_t kSetColour = 0x0Cu;      // the console's (0x2282E0)
        constexpr uint32_t kPrint = 0x10u;          // the console's printf (0x228120)
        constexpr uint32_t kOverlayStates = 0x203300u; // the 2D overlay's render states, on or off
        constexpr uint32_t kTargetRate = 0x202FC0u;    // targetRates[a0]
        constexpr uint32_t kHoldsAtThirty = 0x202FE0u; // GovernorHoldsAtThirty
        constexpr uint32_t kFillRect = 0x203360u;      // a filled rectangle {x0, y0, x1, y1}
        constexpr uint32_t kFloatToDouble = 0x139520u; // __extendsfdf2 (for printf)

        // Registers.
        constexpr int kV0 = 2, kV1 = 3, kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7, kT0 = 8, kT9 = 25, kSp = 29, kRa = 31;
        constexpr int kS0 = 16, kS1 = 17, kS2 = 18, kS3 = 19, kS4 = 20;
        const decomp::GprVar inBetween{kS4};       // draw the in-between field
        const decomp::GprVar vblanksPerFrame{kS3}; // at the target rate
        const decomp::GprVar fieldsTaken{kS2};     // by the frame before, counted as frames of two fields
        const decomp::GprVar eeOver{kS1};          // the EE's time over the period
        const decomp::GprVar gpuOver{kS0};         // the GPU's
        const decomp::FprVar target{20};           // the target rate (f20, until the measure) ...
        const decomp::FprVar gpu{20};              // ... then the GPU's time
        const decomp::FprVar one{21};              // 1
        const decomp::FprVar fieldRate{22};        // the display's field rate (f22, until the measure) ...
        const decomp::FprVar ee{22};               // ... then the EE's time
        const decomp::FprVar measuredRate{23};     // fields a second, measured
        const decomp::FprVar targetRate{24};       // fields a second, aimed at
        const decomp::FprVar period{25};           // 1 / the target rate

        // The original's addresses where it is entered again.
        constexpr uint32_t kEntry = 0x25B670u;
        constexpr uint32_t kAfterScheme = 0x25B6C0u, kAfterOverlayOff = 0x25B6C8u, kAfterTarget = 0x25B6D0u,
                           kAfterDisplayMode = 0x25B6FCu, kCopy = 0x25B708u;
        constexpr uint32_t kBarScheme = 0x25B784u, kBarOverlayOff = 0x25B78Cu, kBarFilled = 0x25B7E8u,
                           kBarScheme2 = 0x25B800u, kBarOverlayOn = 0x25B808u, kBarColour = 0x25B81Cu,
                           kBarDouble = 0x25B830u, kBarPrinted = 0x25B854u, kAfterBar = 0x25B86Cu;
        constexpr uint32_t kAfterHeldA = 0x25B9FCu;
        constexpr uint32_t kStatScheme = 0x25BA9Cu, kStatOverlayOn = 0x25BAA4u, kStatColour = 0x25BAB8u,
                           kStatDouble = 0x25BAC0u, kStatPrinted = 0x25BAE4u, kStatScheme2 = 0x25BAFCu,
                           kStatOverlayOn2 = 0x25BB60u, kAfterStats = 0x25BB80u;
        constexpr uint32_t kAfterHeldB = 0x25BBA4u;

        // A virtual call's set-up: a0 = the object, t9 = its method.
        void SetUpVirtual(decomp::Guest &g, uint32_t object, uint32_t slot)
        {
            g.setGpr(kA0, object);
            g.setGpr(kT9, decomp::method(object, slot));
        }
    }

    // 0x25B670: the governor.
    void Governor(decomp::Guest &g)
    {
        R5900Context *ctx = g.ctx();
        switch (g.pc())
        {
        case kEntry:
        {
            // A frame of 0xD0 bytes: f20-f25 at 0x00-0x14, s0-s4 at 0x20-0x60, ra at 0x70, the
            // detail bar's rectangle at 0x80, the display mode at 0x90, its copy at 0xB0.
            const uint32_t sp = g.gpr(kSp) - 0xD0u;
            g.setGpr(kSp, sp);
            g.setGpr64(kA1, 0u);
            g.write<uint64_t>(sp + 0x70u, g.gpr64(kRa));
            g.setGpr64(kA2, 0u);
            g.storeQuad(sp + 0x60u, kS4);
            g.storeQuad(sp + 0x50u, kS3);
            g.storeQuad(sp + 0x40u, kS2);
            g.storeQuad(sp + 0x30u, kS1);
            g.storeQuad(sp + 0x20u, kS0);
            for (int f = 25; f >= 20; --f)
                g.write<f32>(sp + 4u * static_cast<uint32_t>(f - 20), ctx->f[f]);
            SetUpVirtual(g, renderer, kSetScheme);
            g.setGpr64(kS4, 1u); // inBetween (daddiu, in the call's delay slot)
        }
            DECOMP_CALL(g, g.gpr(kT9), kAfterScheme) // the renderer's default scheme
            g.setGpr64(kA0, 0u);
            DECOMP_CALL(g, kOverlayStates, kAfterOverlayOff) // the overlay's render states off
            g.setGpr64(kA0, 0u);
            DECOMP_CALL(g, kTargetRate, kAfterTarget) // f0 = targetRates[0]
            one = 1.0f;
            target = f32(ctx->f[0]);
            period = f32(one) / f32(target);
            g.setGpr(kA2, static_cast<uint32_t>(-1));
            g.setGpr(kA1, renderer);
            g.setGpr(kT9, decomp::method(renderer, kDisplayMode));
            g.setGpr(kA0, g.gpr(kSp) + 0x90u);
            vblanksPerFrame = 1;
            DECOMP_CALL(g, g.gpr(kT9), kAfterDisplayMode) // the current display mode, into sp+0x90
            // Copied to sp+0xB0, 8 bytes a step.
            g.setGpr(kA2, g.gpr(kSp) + 0x90u);
            g.setGpr(kA1, g.gpr(kSp) + 0xB0u);
            g.setGpr(kA0, 3u);
            for (;;)
            {
            case kCopy:
            {
                const uint32_t low = g.read<uint32_t>(g.gpr(kA2));
                g.setGpr(kV1, low);
                g.setGpr(kA0, g.gpr(kA0) - 1u);
                const uint32_t high = g.read<uint32_t>(g.gpr(kA2) + 4u);
                g.setGpr(kV0, high);
                g.write<uint32_t>(g.gpr(kA1), low);
                g.setGpr(kA2, g.gpr(kA2) + 8u);
                g.write<uint32_t>(g.gpr(kA1) + 4u, high);
                const bool again = static_cast<int32_t>(g.gpr(kA0)) > 0;
                g.setGpr(kA1, g.gpr(kA1) + 8u);
                if (!again)
                    break;
            }
                if (g.backEdge(kCopy))
                    return;
            }
            {
                f32 rate = g.read<f32>(g.gpr(kSp) + 0xC4u); // the display mode's field rate
                ctx->f[0] = 0.0f;
                if (f32(0.0f) == rate)
                    rate = ntscFieldRate;
                fieldRate = rate;
            }

            // DEBUG DISPLAY: the detail scale as a green bar, 100 pixels at 1, its bottom 210
            // pixels above the screen's, and "Load balancing detail  NN%".
            if (showDetailBar != 0 && detailScale < f32(1.0f))
            {
                SetUpVirtual(g, renderer, kSetScheme);
                g.setGpr64(kA1, 0u);
                g.setGpr64(kA2, 0u);
                DECOMP_CALL(g, g.gpr(kT9), kBarScheme)
                g.setGpr64(kA0, 0u);
                DECOMP_CALL(g, kOverlayStates, kBarOverlayOff)
                {
                    const uint32_t rect = g.gpr(kSp) + 0x80u;
                    const f32 screenBottom = overlayBottom;
                    const f32 detail = detailScale;
                    g.write<uint32_t>(rect + 0x0u, 0x42700000u); // x0 = 60
                    const f32 bottom = screenBottom - f32(210.0f);
                    const f32 length = f32(100.0f) * detail;
                    const f32 top = bottom - f32(10.0f);
                    const f32 right = f32(60.0f) + length;
                    g.write<f32>(rect + 0xCu, bottom);
                    g.write<f32>(rect + 0x4u, top);
                    g.write<f32>(rect + 0x8u, right);
                    g.setGpr(kA0, rect);
                    g.setGpr(kA1, 0xFF449944u);
                }
                DECOMP_CALL(g, kFillRect, kBarFilled)
                SetUpVirtual(g, renderer, kSetScheme);
                g.setGpr64(kA1, 0u);
                g.setGpr64(kA2, 0u);
                DECOMP_CALL(g, g.gpr(kT9), kBarScheme2)
                g.setGpr(kA0, 1u);
                DECOMP_CALL(g, kOverlayStates, kBarOverlayOn)
                SetUpVirtual(g, consoleText, kSetColour);
                g.setGpr(kA1, 0xFFFFFFFFu);
                DECOMP_CALL(g, g.gpr(kT9), kBarColour)
                ctx->f[12] = (f32(100.0f) * detailScale).v;
                DECOMP_CALL(g, kFloatToDouble, kBarDouble)
                SetUpVirtual(g, consoleText, kPrint);
                g.copyGpr(kT0, kV0); // the double
                g.setGpr64(kA1, 0u);
                g.setGpr(kA2, 0x10u);
                g.setGpr(kA3, kDetailFormat);
                DECOMP_CALL(g, g.gpr(kT9), kBarPrinted)
                SetUpVirtual(g, renderer, kSetScheme);
                g.setGpr64(kA1, 0u);
                g.setGpr64(kA2, 0u);
                DECOMP_CALL(g, g.gpr(kT9), kAfterBar)
            }

            // The measure: the frame before against the period.
            if (!(f32(0.0f) == f32(target)))
                vblanksPerFrame = (f32(0.5f) + f32(fieldRate) / f32(target)).toInt();
            {
                const f32 perFrame = f32::fromInt(vblanksPerFrame);
                targetRate = f32(fieldRate) / perFrame;
                const f32 inFrame = eeTimeInFrame;
                const f32 between = eeTimeBetweenFrames;
                const f32 measuredPeriod = measuredFramePeriod;
                const f32 roundUp = almostOne;
                gpu = gpuTime;
                ee = between + inFrame;
                measuredRate = f32(1.0f) / measuredPeriod;
                fieldsTaken = (roundUp + f32(targetRate) / f32(measuredRate) * f32(2.0f)).toInt();
                measuredRate = f32(measuredRate) * f32(2.0f);
                targetRate = f32(targetRate) * f32(2.0f);
                eeOver = !(f32(ee) <= f32(period)) ? 1 : 0;
                gpuOver = !(f32(gpu) <= f32(period)) ? 1 : 0;
            }

            // The field switch.
            if (fieldSwitchOn != 0)
            {
                const f32 late = fieldSwitchLateMargin;
                const bool gpuLate = !(f32(gpu) + late <= f32(period));
                const f32 room = fieldSwitchRoomMargin;
                const bool eeRoom = f32(ee) + room < f32(period);
                const bool gpuRoom = f32(gpu) + room < f32(period);
                if (!(f32(ee) + late <= f32(period)) || gpuLate)
                {
                    renderFlag6CD800 = 0u;
                    fieldSwitchActive = 1u;
                    fieldSwitchAlternates = 1u;
                    flipInVBlankHandler = 1u;
                    alternateFields = 1u;
                }
                if (eeRoom && gpuRoom)
                {
                    fieldSwitchActive = 0u;
                    renderFlag6CD800 = 1u;
                    fieldSwitchAlternates = 0u;
                    flipInVBlankHandler = 0u;
                    alternateFields = 0u;
                    nextFrameField = 0;
                }
            }
            else
            {
                fieldSwitchActive = 0u;
                renderFlag6CD800 = 1u;
                fieldSwitchAlternates = 0u;
                flipInVBlankHandler = 0u;
                alternateFields = 0u;
                nextFrameField = 0;
            }

            DECOMP_CALL(g, kHoldsAtThirty, kAfterHeldA)
            {
                // DEBUG DISPLAY: the frame statistics, unless a screenshot is being taken.
                bool show = g.gpr(kV0) == 0u && showFrameStats != 0;
                if (!show)
                    show = showFrameStatsHeld != 0;
                if (capturingScreen != 0)
                    show = false;
                g.setGpr(kV1, show ? 1u : 0u);
            }
            if (g.gpr(kV1) != 0u)
            {
                {
                    bool print = false;
                    if (printRateWhenOff != 0)
                        print = !(decomp::abs(f32(measuredRate) / f32(targetRate) - f32(1.0f)) <= f32(onePercent));
                    if (!print)
                        print = printRateAlways != 0;
                    g.setGpr(kV0, print ? 1u : 0u);
                }
                if (g.gpr(kV0) != 0u)
                {
                    SetUpVirtual(g, renderer, kSetScheme);
                    g.setGpr64(kA1, 0u);
                    g.setGpr64(kA2, 0u);
                    DECOMP_CALL(g, g.gpr(kT9), kStatScheme)
                    g.setGpr(kA0, 1u);
                    DECOMP_CALL(g, kOverlayStates, kStatOverlayOn)
                    SetUpVirtual(g, consoleText, kSetColour);
                    g.setGpr(kA1, 0xFFFFFFFFu);
                    DECOMP_CALL(g, g.gpr(kT9), kStatColour)
                    ctx->f[12] = ctx->f[23]; // the measured rate
                    DECOMP_CALL(g, kFloatToDouble, kStatDouble)
                    SetUpVirtual(g, consoleText, kPrint);
                    g.copyGpr(kT0, kV0);
                    g.setGpr64(kA1, 0u);
                    g.setGpr(kA2, 0x12u);
                    g.setGpr(kA3, kRateFormat);
                    DECOMP_CALL(g, g.gpr(kT9), kStatPrinted)
                    SetUpVirtual(g, renderer, kSetScheme);
                    g.setGpr64(kA1, 0u);
                    g.setGpr64(kA2, 0u);
                    DECOMP_CALL(g, g.gpr(kT9), kStatScheme2)
                }
                {
                    if (eeOver != 0)
                        eeLateFrame = renderedFrames;
                    if (gpuOver != 0)
                        gpuLateFrame = renderedFrames;
                    if (fieldsTaken != vblanksPerFrame)
                    {
                        uint32_t colour = 0xFF000000u;
                        if (eeOver != 0)
                            colour = eeLateColour;
                        if (gpuOver != 0)
                            colour = gpuLateColour;
                        if (eeOver != 0 && gpuOver != 0)
                            colour = 0xFFFFFFFFu;
                        lateFrameColour = colour;
                    }
                    g.setGpr(kA0, 1u);
                }
                DECOMP_CALL(g, kOverlayStates, kStatOverlayOn2)
                if (!(f32(physicsTime) <= f32(period)))
                    physicsLateFrame = renderedFrames;
                g.setGpr64(kA0, 0u);
                physicsTime = 0.0f;
                DECOMP_CALL(g, kOverlayStates, kAfterStats)
            }

            // A frame that took more fields than it should: no in-between, when so set.
            if (vblanksPerFrame < fieldsTaken && noInBetweenWhenLate != 0)
                g.setGpr64(kS4, 0u);
            DECOMP_CALL(g, kHoldsAtThirty, kAfterHeldB)
            if (g.gpr(kV0) != 0u || governorOn == 0)
            {
                // Held, or switched off: two fields, the detail at 1.
                countFromFlipA = 0u;
                framesOfTwoFields = 1u;
                detailScale = 1.0f;
            }
            else if (port::levelRate() != 0 && port::inBetweenPictures())
            {
                // PC: a level at the PC port's rate with in-between pictures has two fields a
                // frame, the in-between one drawn, and its full detail, every frame: its VBlanks
                // come twice as often as its frames, and a frame of one field would end at the
                // next one and run the level at twice its speed.
                wantTwoFields = 1u;
                detailScale = 1.0f;
                g.setGpr(kS4, 1u);
                framesOfTwoFields = 1u;
                countFromFlipA = 0u;
            }
            else if (port::levelRate() != 0)
            {
                // PC: a level at the PC port's rate has one field a frame and its full detail -
                // the choices for a console's load do not apply. (At one field the governor would
                // lower the detail every frame, to 0.995: the systems that read it in the next
                // frame's update compare it with 0.8, 0.9 and 0.995 or scale counts by it - 100 x
                // 0.995 truncates to 99 - and took their reduced paths, which the console at 30,
                // whose frames fit, does not.)
                wantTwoFields = 0u;
                detailScale = 1.0f;
                g.setGpr(kS4, 0u);
                framesOfTwoFields = 0u;
                countFromFlipA = countFromFlipAtOneField != 0 ? uint8_t(1u) : uint8_t(0u);
            }
            else
            {
                const f32 lateBy = lateMargin;
                const bool eeLate = !(f32(ee) + lateBy <= f32(period));
                const bool gpuLate = !(f32(gpu) + lateBy <= f32(period));
                const f32 tightBy = tightMargin;
                const bool eeTight = !(f32(ee) + tightBy <= f32(period));
                const bool gpuTight = !(f32(gpu) + tightBy <= f32(period));
                const f32 roomBy = roomMargin;
                const bool eeRoom = f32(ee) + roomBy < f32(period);
                const bool gpuRoom = f32(gpu) + roomBy < f32(period);
                if (wantTwoFields != 0)
                {
                    // Two fields: the detail to the load ...
                    if (eeTight || gpuTight)
                        detailScale = f32(detailScale) - f32(detailStepDown);
                    else if (eeRoom && gpuRoom)
                        detailScale = f32(detailScale) + f32(detailStepUp);
                    detailScale = decomp::max(decomp::min(detailScale, f32(one)), detailScaleMin);
                    // ... and a late frame goes to one field.
                    if (eeLate || gpuLate)
                    {
                        framesLate = framesLate + 1;
                        if (countFromFlipB == 0 || !(framesLate < lateFramesToOneField))
                        {
                            framesWithRoom = 0;
                            wantTwoFields = 0u;
                        }
                    }
                    else
                    {
                        framesLate = 0;
                    }
                }
                else
                {
                    // One field: the detail down every frame ...
                    {
                        const f32 lowered = f32(detailScale) - f32(detailStepDown);
                        detailScale = lowered;
                        detailScale = decomp::max(decomp::min(lowered, f32(one)), detailScaleMin);
                    }
                    // ... and after enough frames with room, two fields.
                    if (eeRoom && gpuRoom)
                    {
                        framesWithRoom = framesWithRoom + 1;
                        if (!(framesWithRoom < roomyFramesToTwoFields))
                        {
                            framesLate = 0;
                            wantTwoFields = 1u;
                        }
                    }
                }
                g.setGpr(kS4, wantTwoFields);
                framesOfTwoFields = static_cast<uint8_t>(static_cast<int32_t>(inBetween));
                countFromFlipA = countFromFlipAtOneField != 0 ? uint8_t(inBetween == 0) : uint8_t(0u);
            }
            // (PC, TODO 160: the detail scale is the console's load balancer - a dozen of the game's systems take reduced
            // paths under 1. A PC draws the full detail whatever the load: pinned at 1 here too - the console's 30 (a level
            // at a 30 cap, TF_FRAME_RATE=30), the one case the branches above leave it to the load. TF_DETAIL_PS2=1: as
            // the console; the checker's console rules keep the console's.)
            if (port::fullDetail())
                detailScale = 1.0f;
            inBetweenOn = static_cast<uint8_t>(static_cast<int32_t>(inBetween));
            inBetweenFactor = inBetween != 0 ? port::inBetweenFactor() : port::oneFieldFactor(); // (0.5 as the console; 0 for moment pictures; one field: 1, or 0 for N9's poses)
            if (fieldSwitchActive != 0)
            {
                inBetweenFactor = 0.5f;
                flipInVBlankHandler = uint8_t(inBetween == 0);
                if (fieldSwitchAlternates != 0)
                    alternateFields = flipInVBlankHandler;
            }
            {
                // Epilogue.
                const uint32_t sp = g.gpr(kSp);
                g.setGpr64(kRa, g.read<uint64_t>(sp + 0x70u));
                for (int f = 25; f >= 20; --f)
                    ctx->f[f] = g.read<f32>(sp + 4u * static_cast<uint32_t>(f - 20)).v;
                g.loadQuad(sp + 0x60u, kS4);
                g.loadQuad(sp + 0x50u, kS3);
                g.loadQuad(sp + 0x40u, kS2);
                g.loadQuad(sp + 0x30u, kS1);
                g.loadQuad(sp + 0x20u, kS0);
                g.setGpr(kSp, sp + 0xD0u);
            }
            g.returnToCaller();
            return;
        default:
            std::fprintf(stderr, "[decomp] Governor entered at %08x\n", g.pc());
            return;
        }
    }

    DECOMP_YIELDING(0x25B670, Governor, "timing", kAfterScheme, kAfterOverlayOff, kAfterTarget, kAfterDisplayMode, kCopy,
                    kBarScheme, kBarOverlayOff, kBarFilled, kBarScheme2, kBarOverlayOn, kBarColour, kBarDouble, kBarPrinted,
                    kAfterBar, kAfterHeldA, kStatScheme, kStatOverlayOn, kStatColour, kStatDouble, kStatPrinted, kStatScheme2,
                    kStatOverlayOn2, kAfterStats, kAfterHeldB);
}
