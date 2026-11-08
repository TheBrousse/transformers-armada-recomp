// vrr_probe.h - what the graphics driver says about the screen's variable refresh (TODO 35, frame pacing).
//
// Windows itself does not say whether a screen's variable refresh (FreeSync, G-Sync, VESA Adaptive-Sync) is on: DXGI's
// tearing support says only that presents may tear (the precondition for it in a window), not that the screen follows
// them. AMD's driver does: ADL (atiadlxx.dll, installed with it) reads each display's FreeSync state and its range.
// The frame pacing (overrides.cpp) takes whether the screen follows the pictures from what it does - the vblank
// watcher's measure, ps2_host_display::screenVariable - and only the range from here: which rates it can follow.
// Elsewhere (NVIDIA, Intel, no driver library): unknown. Read once, logged once.
#pragma once

namespace tf::vrr
{
    struct Range
    {
        bool known = false;   // the driver named a range for the window's screen
        int state = 0;        // the driver's FreeSync state word (ADL's current setting, as it reports it)
        double minHz = 0.0;   // the refresh range it follows
        double maxHz = 0.0;
    };

    // The range of the screen whose refresh is `refreshHz` (the display among the connected ones whose range tops at it).
    Range screenRange(double refreshHz);
}
