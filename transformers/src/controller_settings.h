#pragma once
// The Controller tab of the settings panel (F5): the native DualSense / DualShock 4 support of the
// runtime (ps2_sony_pad.h, patch 0008) - which pads to read, the vibration's strength and the
// light bar's colour. They are read at start: the panel offers to restart.

namespace controller_settings
{
    // Before the window loop starts (overrides.cpp's addSettings).
    void add();
}
