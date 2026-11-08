#pragma once
// The speedrun timer: level time and splits (speedrun_timer.cpp). The mod menu shows and switches it.

#include <cstdint>
#include <string>
#include <vector>

namespace speedrun_timer
{
    struct View
    {
        bool inLevel = false;
        double level = 0.0, total = 0.0; // seconds
        std::vector<double> splits;       // the levels finished since the last reset
    };
    // Its setting (section "On screen": the overlay's Display page; TF_SPEEDRUN_TIMER, kept in settings.ini).
    void addSettings();
    bool enabled();
    void setEnabled(bool on);
    void reset();
    // Once a game frame (overrides.cpp).
    void frame(const uint8_t *rdram, bool moviePlaying, uint64_t vblankTick);
    View view();
    std::string format(double seconds);
}
