#pragma once
// Frame timing: the clocks, the frame rates and the wait for the VBlank.
//
// How the game paces itself (docs/NOTES.md, "Frame rates"): the main loop (0x295310) runs
// a mode's update and drawing once a frame and waits until TvFieldRate() / TargetFrameRate()
// VBlanks have passed; the frame end waits for the VBlank (WaitVBlank). The shipped game asks
// for half rate when it makes its modes, so it targets 29.97 frames a second; the PC port
// keeps the half rate off in levels and runs them at its own rate R (port::levelRate(): 60 by
// default), the rates scaled to it. FramePeriod() is the seconds a frame, which a level
// reads when it begins (its clock's step, the gravity a frame) and a few places every frame.

#include "decomp.h"

namespace tf::timing
{
    using decomp::Array;
    using decomp::f32;
    using decomp::Object;
    using decomp::Ref;
    using decomp::Var;

    // --- The rates (rates.cpp) -------------------------------------------------------

    inline constexpr Var<int32_t> tvStandard{0x006CDD38u};   // gp-0x53B8: 0 NTSC, 1 PAL
    inline constexpr Var<uint8_t> pal60{0x006CF658u};        // gp-0x3A98: a PAL console in its 60 Hz mode
    inline constexpr Var<uint8_t> halfRate{0x006CF654u};     // gp-0x3A9C: frames of two TV fields
    inline constexpr Array<f32> frameRates{0x0063D4A8u};     // by standard: 59.94, 50
    inline constexpr Array<f32> tvFieldRates{0x0063D4B0u};   // by standard: 59.94, 50
    inline constexpr Array<f32> targetRates{0x006CE980u};    // gp-0x4770: [0] the governor's target
    inline constexpr Var<int32_t> holdAtThirty{0x006CD324u}; // gp-0x5DCC: from boot until a level is set up

    f32 TvFieldRate();                         // 0x32E840
    f32 TargetFrameRate();                     // 0x32E8B0
    f32 FramePeriod();                         // 0x32E910
    void SetHalfRate(uint8_t on);              // 0x32E8A0
    void SetTargetRate(int32_t index, f32 rate); // 0x202FF0
    int32_t HoldsAtThirty();                   // 0x106710
    int32_t GovernorHoldsAtThirty();           // 0x202FE0

    // PC: the target frame rate and the frame period of a level at the PC port's rate `rate`
    // (59.94 x R / 60; 0 or 60: the console's 59.94), and the console's period at 30.
    f32 LevelFrameRate(int rate);
    f32 LevelFramePeriod(int rate);
    f32 ConsoleFramePeriod();

    // --- The clocks (clock.cpp) -------------------------------------------------------

    inline constexpr Var<uint32_t> clockLastCount{0x006CE5B8u}; // gp-0x4B38: COP0 Count when last read
    inline constexpr Var<uint64_t> clockCycles{0x006CE5B0u};    // gp-0x4B40: EE cycles counted so far
    inline constexpr Var<f32> secondsPerCycle{0x006CB1A4u};     // gp-0x7F4C

    f32 ClockSeconds(); // 0x108780

    // A level's clock: made first thing in taGame's begin (0x294950), its step set to
    // FramePeriod(), advanced once a frame.
    struct GameClock : Object
    {
        Ref<int32_t> frames() const { return field<int32_t>(0x00); }
        Ref<f32> time() const { return field<f32>(0x04); }
        Ref<f32> step() const { return field<f32>(0x08); }              // seconds a frame
        Ref<f32> scale() const { return field<f32>(0x0C); }             // 1
        Ref<int32_t> framesSinceStep() const { return field<int32_t>(0x10); }
        Ref<f32> timeAtStep() const { return field<f32>(0x14); }        // the time when the step was set
    };
    inline constexpr Var<uint32_t> currentGameClock{0x006CF8A4u}; // gp-0x384C

    GameClock GameClock_Construct(GameClock self);    // 0x41E8F0
    void GameClock_SetStep(GameClock self, f32 step);  // 0x41E820
    void GameClock_Advance(GameClock self);            // 0x41E840

    // --- The wait for the VBlank (vblank.cpp) ------------------------------------------

    inline constexpr Var<uint8_t> waitForVBlank{0x006F8539u};      // 0 skips the wait
    inline constexpr Var<int32_t> vblankCount{0x006CEED0u};        // gp-0x4220: counted by the VBlank handler
    inline constexpr Var<int32_t> currentField{0x006CEED4u};       // gp-0x421C
    inline constexpr Var<int32_t> nextFrameField{0x006CEED8u};     // gp-0x4218: the field the next frame starts on
    inline constexpr Var<uint8_t> framesOfTwoFields{0x006CF010u};  // gp-0x40E0: the governor's "30" flag
    inline constexpr Var<uint8_t> countFromFlipA{0x006CF014u};     // gp-0x40DC  (either: count from the
    inline constexpr Var<uint8_t> countFromFlipB{0x006CF020u};     // gp-0x40D0   last flip, not from now)
    inline constexpr Var<int32_t> lastFlipVBlank{0x006CF018u};     // gp-0x40D8
    inline constexpr Var<uint8_t> alternateFields{0x006CF01Cu};    // gp-0x40D4

    void WaitVBlank(decomp::Guest &g); // 0x25DE90 (yields at 0x25DEE8)

    // --- The governor (governor.cpp) ------------------------------------------------------

    inline constexpr Var<uint8_t> wantTwoFields{0x006CD984u};  // gp-0x576C: its choice - frames of two fields (1 at boot) or of one; also read by 0x244E10
    inline constexpr Var<f32> detailScale{0x006CD974u};        // gp-0x577C: 0.5 .. 1, read by about a dozen of the game's systems
    inline constexpr Var<f32> inBetweenFactor{0x006CD6F0u};    // gp-0x5A00: 0.5 or 1, the motion interpolators' blend (0x20D3A0, 0x20DBC0)

    void Governor(decomp::Guest &g); // 0x25B670 (enters again at its calls' return addresses and its loop)

    // --- The main loop (main_loop.cpp) --------------------------------------------------

    // A mode of the game run by the main loop: the intro (taUIIntro), the menus
    // (taUIPreGame) or a level (taGame).
    struct Mode : Object
    {
        static constexpr uint32_t kDraw = 0x18u, kUpdate = 0x1Cu; // its vtable's slots

        static Mode at(uint32_t address)
        {
            Mode mode;
            mode.address = address;
            return mode;
        }
        Ref<uint32_t> vtable() const { return field<uint32_t>(0x00); }
        Ref<int32_t> quit() const { return field<int32_t>(0x04); } // set to leave the loop
        uint32_t method(uint32_t slot) const { return decomp::guest().read<uint32_t>(vtable() + slot); }
    };
    inline constexpr Var<uint32_t> loopStartListener{0x006CFA04u}; // gp-0x36EC: told when a mode's loop starts
    inline constexpr Var<uint8_t> betwixtMode{0x006CD8DCu};        // gp-0x5814: frames drawn as two fields

    void Mode_Run(decomp::Guest &g); // 0x295310 (enters again at its calls' return addresses and its loops)
}
