// The speedrun timer (speedrun_timer.h): the time in the current level, and the levels before it.
//
// A level is the life of the taGame object ([0x6CF2B8], gp-0x3E38: there from a level's start to its
// end). The time counts the VBlanks in it at their rate (so 60 fps or 30 fps give the same seconds,
// and a slow host does not add time), not while a movie plays (the game's own
// movies; cutscenes in the engine count, as in the game's own timing). The pause menu: counted too
// (a run's time is the real time it took).
//
// Decomp equivalent: a counter in taGame's update.

#include "speedrun_timer.h"
#include "runtime/ps2_settings.h"

#include <string>

#include "port.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace speedrun_timer
{
    namespace
    {
        constexpr uint32_t kGame = 0x006CF2B8u;
        std::atomic<bool> g_enabled{[]() {
            const char *env = std::getenv("TF_SPEEDRUN_TIMER"); // (=1: shown from the start)
            return env && *env == '1';
        }()};
        std::mutex g_mutex;
        uint32_t g_game = 0u;
        double g_levelSeconds = 0.0;
        double g_totalSeconds = 0.0;
        std::vector<double> g_splits;
        bool g_running = false;
    }

    void addSettings()
    {
        ps2_settings::Setting s;
        s.key = "TF_SPEEDRUN_TIMER";
        s.section = "On screen";
        s.label = "Speedrun timer";
        s.help = "The level's time and the run's total, top right, with a split per level.";
        s.choices = {{"0", "Off"}, {"1", "On"}};
        s.defaultValue = "0";
        s.apply = [](const std::string &value)
        {
            g_enabled.store(value == "1");
            return true;
        };
        ps2_settings::add(s);
    }

    bool enabled() { return g_enabled.load(); }
    void setEnabled(bool on)
    {
        g_enabled.store(on);
        ps2_settings::noteChanged("TF_SPEEDRUN_TIMER", on ? "1" : "0"); // (into settings.ini: kept for the next start, TODO 74)
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_levelSeconds = g_totalSeconds = 0.0;
        g_splits.clear();
    }

    void frame(const uint8_t *rdram, bool moviePlaying, uint64_t tick)
    {
        static uint64_t lastTick = 0u;
        const uint64_t ticks = (tick > lastTick && tick - lastTick < 60u) ? tick - lastTick : 0u; // (not a state load's jump)
        lastTick = tick;
        uint32_t game = 0u;
        std::memcpy(&game, rdram + kGame, sizeof(game));
        std::lock_guard<std::mutex> lock(g_mutex);
        if (game != g_game)
        {
            if (g_game && g_running && g_levelSeconds > 1.0)
                g_splits.push_back(g_levelSeconds); // (a level ended)
            g_game = game;
            g_levelSeconds = 0.0;
            g_running = game != 0u;
        }
        if (!g_running || moviePlaying)
            return;
        // (VBlank ticks: the level's rate, twice it with in-between pictures, the console's 60 else)
        const int vblanks = tf::port::vblankRate();
        const double step = static_cast<double>(ticks) / (vblanks > 0 ? vblanks : 60);
        g_levelSeconds += step;
        g_totalSeconds += step;
    }

    View view()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return View{g_running, g_levelSeconds, g_totalSeconds, g_splits};
    }

    std::string format(double seconds)
    {
        const int total = static_cast<int>(seconds * 100.0);
        char text[32];
        const int h = total / 360000, m = total / 6000 % 60, s = total / 100 % 60, c = total % 100;
        if (h > 0)
            std::snprintf(text, sizeof(text), "%d:%02d:%02d.%02d", h, m, s, c);
        else
            std::snprintf(text, sizeof(text), "%d:%02d.%02d", m, s, c);
        return text;
    }
}
