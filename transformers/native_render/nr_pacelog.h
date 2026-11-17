// The pacing log (nr_pacelog.cpp, in the core): `[tf] pacing ...` every 2 s and the pictures' 1% low. The window side tells it
// each picture shown (noteShown) or shown again (noteRepeat); what it needs of the host comes through nr_core::PresentHost.
#pragma once

#include <cstdint>

namespace nr::pacelog
{
    // A picture as it reached the screen: its camera turn (view_turn.h, N9), its frame's tick and moment, when it was ready.
    struct Shown
    {
        uint64_t turnId = 0;
        uint64_t tick = 0;
        int64_t dueNs = 0, readyNs = 0;
    };
    // A new picture to the screen now (`skipped`: pictures handed over since the last one that never were). Window thread.
    void noteShown(const Shown &sp, uint64_t skipped);
    // The picture before shown again. Window thread.
    void noteRepeat();
    // The steady clock, nanoseconds.
    int64_t now();
}
