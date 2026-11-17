// The pacing log (nr_pacelog.h): how evenly the native pictures reach the screen, `[tf] pacing ...` every 2 s, and the pictures'
// 1% low for the frame-rate steps (TODO 291) - moved here from nr_present.cpp into the core (nr_core S2b). What it needs of the
// host (the screen's refresh timing, the view turn's own figures, the hand-over's gate and cap counts) comes through
// nr_core::PresentHost; the window side (nr_present.cpp) tells it each picture shown.
#include "nr_pacelog.h"
#include "nr_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nr
{
    namespace pacelog
    {
    namespace
    {
        // ---- N9: the pacing log --------------------------------------------------------------------------
        // Every 2 s, `[tf] pacing ...`: how evenly the native pictures reach the screen - the time between the
        // new pictures the window shows (average, spread, the longest 1% and the 1% low), the draws that showed
        // the picture before again and the pictures never shown; from each picture's moment (its frame's VBlank,
        // Frame::dueNs) to being ready (the GPU done) and to the screen; and the screen's refresh rate, whether
        // its refresh is variable (FreeSync, G-Sync). On by default with the native picture live (TF_PACING_LOG=0
        // leaves it out). TF_PACING_TRACE=<tick>,<count>: every picture from that VBlank on, a line each.
        struct Pacing
        {
            std::vector<double> between, toScreen, toReady;
            uint32_t repeats = 0u, unshown = 0u;
            int64_t lastShownNs = 0;
            // (TODO 35: on a screen with a refresh of its own, how many refreshes each picture stayed up - the refresh after
            // its present, from the vblank watcher's phase: 0 = replaced before it was shown, then 1, 2, 3 or more)
            uint32_t held[4] = {};
            int64_t lastRefresh = 0, refreshCount = 0, lastVbNs = 0; // (refreshCount: the refreshes counted since the watch began)
            std::chrono::steady_clock::time_point since{};
            // (TODO 377, TF_PACING_HIST=1, a test aid: the pictures over 8.3 ms with their time since the first picture -
            // for the slow pictures' classes and whether they come at a beat or at events)
            std::vector<std::pair<double, double>> slow;
            int64_t firstNs = 0;
        };
        Pacing g_pacing;

        bool pacingHistOn()
        {
            static const bool on = std::getenv("TF_PACING_HIST") && *std::getenv("TF_PACING_HIST") == '1';
            return on;
        }

        // (TODO 291) The shown pictures' intervals of the last 30 s, for the fps line's 1% low (nr::pictureLowRate): what
        // the player sees. Before, the line's 1% low came from the game's frames, which the game's own wait for a VBlank
        // inside a frame pairs into a late one and a short one while the pictures stay even - at a pinned 80 on the RX 5700
        // XT 216 of 535 frames took 18-23 ms, the pictures' 1% low 71: the user's "1% low sinking 66 -> 44" at a flat 78.
        struct ShownIntervals
        {
            std::mutex mutex;
            std::deque<std::pair<int64_t, float>> v; // (when shown, ns; the interval before it, ms)
        } g_shownIntervals;

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool pacingLogOn()
        {
            static const bool on = [] {
                const char *v = std::getenv("TF_PACING_LOG");
                return !v || *v != '0';
            }();
            return on;
        }

        void spreadOf(std::vector<double> v, double &avg, double &spread, double &lo, double &hi)
        {
            avg = spread = lo = hi = 0.0;
            if (v.empty())
                return;
            for (double x : v)
                avg += x;
            avg /= static_cast<double>(v.size());
            for (double x : v)
                spread += (x - avg) * (x - avg);
            spread = v.size() > 1u ? std::sqrt(spread / static_cast<double>(v.size() - 1u)) : 0.0;
            std::sort(v.begin(), v.end());
            lo = v.front();
            hi = v.back();
        }

        void printPacing(double seconds)
        {
            Pacing &p = g_pacing;
            std::vector<double> b = p.between;
            std::sort(b.begin(), b.end());
            double mean, sd, lo, hi;
            spreadOf(b, mean, sd, lo, hi);
            // (the 1% low: the rate of the slowest 1% of the pictures - their intervals' mean - at least one)
            const size_t worst = std::min(b.size(), std::max<size_t>(1u, b.size() / 100u));
            double worstMean = 0.0;
            for (size_t i = 0; i < worst; ++i)
                worstMean += b[b.size() - 1u - i];
            worstMean = worst ? worstMean / static_cast<double>(worst) : 0.0;
            double sAvg, sSd, sLo, sHi, rAvg, rSd, rLo, rHi;
            spreadOf(p.toScreen, sAvg, sSd, sLo, sHi);
            spreadOf(p.toReady, rAvg, rSd, rLo, rHi);
            int64_t vbNs = 0, periodNs = 0;
            const nr_core::PresentHost &host = nr_core::presentHost();
            const bool known = host.screenTiming && host.screenTiming(vbNs, periodNs) && periodNs > 0;
            const double hz = known ? 1.0 / std::chrono::duration<double>(std::chrono::nanoseconds(periodNs)).count() : 0.0;
            char turn[384] = "";
            if (host.appendTurnPacing)
                host.appendTurnPacing(turn, sizeof(turn)); // (N9: how evenly the view turned picture to picture)
            char cadence[192] = "";
            {
                const double n = static_cast<double>(p.held[0] + p.held[1] + p.held[2] + p.held[3]);
                if (n > 0.0)
                    std::snprintf(cadence, sizeof(cadence),
                                  "; up for 1/2/3+ refreshes %.0f/%.0f/%.0f%%, replaced unshown %.0f%% (estimated from the refresh after each present)",
                                  100.0 * p.held[1] / n, 100.0 * p.held[2] / n, 100.0 * p.held[3] / n, 100.0 * p.held[0] / n);
            }
            unsigned gatePictures = 0u, capDrops = 0u;
            double gateHeldMs = 0.0, gateSlicesMs = 0.0;
            if (host.takeGateHolds)
                host.takeGateHolds(gatePictures, gateHeldMs, gateSlicesMs); // (TODO 336 (24))
            if (host.takeCapDrops)
                capDrops = host.takeCapDrops();
            std::fprintf(stderr,
                         "[tf] pacing: %.1f new pictures a second (%zu in %.2f s); between them %.2f ms on average, spread %.2f ms, "
                         "longest 1%% %.2f ms (1%% low %.1f a second), shortest %.2f, longest %.2f; repeats %u, never shown %u, "
                         "left out %u (the cap), %u held for its gate (%.1f ms in all, %.1f ms of it in time slices); "
                         "from the frame's moment to ready %.1f ms (spread %.1f), to the screen %.1f ms (spread %.2f, %.1f..%.1f); "
                         "screen %.0f Hz, variable refresh %s%s%s\n",
                         static_cast<double>(b.size()) / seconds, b.size(), seconds, mean, sd, worstMean,
                         worstMean > 0.0 ? 1000.0 / worstMean : 0.0, lo, hi, p.repeats, p.unshown, capDrops, gatePictures, gateHeldMs, gateSlicesMs, rAvg, rSd, sAvg, sSd,
                         sLo, sHi, hz,
                         !known ? "unknown" : host.screenVariable && host.screenVariable() ? "on" : "off", cadence, turn);
            if (pacingHistOn() && !b.empty())
            {
                // (b is sorted: the counts over each frame time, and the 0.1% low next to the 1% low)
                const auto over = [&](double ms) { return static_cast<size_t>(b.end() - std::upper_bound(b.begin(), b.end(), ms)); };
                const size_t w01 = std::min(b.size(), std::max<size_t>(1u, b.size() / 1000u));
                double m01 = 0.0;
                for (size_t i = 0; i < w01; ++i)
                    m01 += b[b.size() - 1u - i];
                m01 /= static_cast<double>(w01);
                std::string list;
                for (size_t i = 0; i < p.slow.size() && i < 48u; ++i)
                {
                    char one[32];
                    std::snprintf(one, sizeof(one), " %.2f:%.1f", p.slow[i].first, p.slow[i].second);
                    list += one;
                }
                std::fprintf(stderr, "[tf] pacing hist: %zu pictures; over 5 ms %zu, 8.3 ms %zu, 16.7 ms %zu, 33 ms %zu; 1%% low %.1f, 0.1%% low %.1f a second; "
                                     "over 8.3 ms (s since the first picture : ms)%s\n",
                             b.size(), over(5.0), over(1000.0 / 120.0), over(1000.0 / 60.0), over(1000.0 / 30.0),
                             worstMean > 0.0 ? 1000.0 / worstMean : 0.0, m01 > 0.0 ? 1000.0 / m01 : 0.0, list.c_str());
            }
            p.slow.clear();
            std::fill(std::begin(p.held), std::end(p.held), 0u);
            p.between.clear();
            p.toScreen.clear();
            p.toReady.clear();
            p.repeats = p.unshown = 0u;
        }

        // A new picture to the screen now (`skipped`: pictures handed over since the last one that never were).
        void noteShownHere(const Shown &sp, uint64_t skipped)
        {
            if (!pacingLogOn())
                return;
            Pacing &p = g_pacing;
            const auto now = std::chrono::steady_clock::now();
            const int64_t ns = nowNs();
            if (p.since == std::chrono::steady_clock::time_point{})
                p.since = now;
            if (p.lastShownNs != 0 && ns - p.lastShownNs < 1000000000ll) // (not across a pause of a second: a movie, a load)
            {
                p.between.push_back(static_cast<double>(ns - p.lastShownNs) / 1.0e6);
                if (pacingHistOn() && p.between.back() > 1000.0 / 120.0)
                    p.slow.emplace_back(static_cast<double>(ns - p.firstNs) / 1.0e9, p.between.back());
                std::lock_guard<std::mutex> lock(g_shownIntervals.mutex); // (TODO 291: nr::pictureLowRate)
                auto &v = g_shownIntervals.v;
                v.emplace_back(ns, static_cast<float>(ns - p.lastShownNs) / 1.0e6f);
                while (!v.empty() && (ns - v.front().first > 30000000000ll || v.size() > 32768u))
                    v.pop_front();
            }
            p.lastShownNs = ns;
            if (p.firstNs == 0)
                p.firstNs = ns;
            {
                const nr_core::PresentHost &host = nr_core::presentHost();
                int64_t vbNs = 0, per = 0;
                if (host.screenTiming && host.screenTiming(vbNs, per) && per > 0 && !(host.screenVariable && host.screenVariable()))
                {
                    // (the watcher gives the latest refresh: counted on from the one before by whole periods)
                    if (p.lastVbNs != 0 && vbNs != p.lastVbNs)
                        p.refreshCount += (vbNs - p.lastVbNs + per / 2) / per;
                    p.lastVbNs = vbNs;
                    const int64_t d = ns - vbNs;
                    const int64_t refresh = p.refreshCount + (d >= 0 ? d / per : -((-d + per - 1) / per)) + 1; // (the first refresh after now)
                    if (p.lastRefresh != 0)
                    {
                        const int64_t up = refresh - p.lastRefresh; // (the picture before's refreshes on the screen)
                        if (up >= 0 && up < 64)
                            ++p.held[std::min<int64_t>(up, 3)];
                    }
                    p.lastRefresh = refresh;
                }
                else
                    p.lastRefresh = p.lastVbNs = 0;
            }
            if (const nr_core::PresentHost &host = nr_core::presentHost(); host.noteShownPicture)
                host.noteShownPicture(sp.turnId, ns, sp.dueNs);
            if (sp.dueNs != 0)
            {
                p.toScreen.push_back(static_cast<double>(ns - sp.dueNs) / 1.0e6);
                if (sp.readyNs != 0)
                    p.toReady.push_back(static_cast<double>(sp.readyNs - sp.dueNs) / 1.0e6);
            }
            p.unshown += static_cast<uint32_t>(skipped);
            static const std::pair<uint64_t, uint64_t> trace = [] {
                std::pair<uint64_t, uint64_t> t{0u, 0u};
                if (const char *v = std::getenv("TF_PACING_TRACE"))
                {
                    unsigned long long a = 0, n = 0;
                    if (std::sscanf(v, "%llu,%llu", &a, &n) == 2)
                        t = {a, n};
                }
                return t;
            }();
            static uint64_t traced = 0u;
            if (trace.second && sp.tick >= trace.first && traced < trace.second)
            {
                ++traced;
                std::fprintf(stderr, "[pace] tick %llu due %.3f ready +%.2f shown +%.2f (skipped %llu)\n",
                             static_cast<unsigned long long>(sp.tick), static_cast<double>(sp.dueNs) / 1.0e6,
                             static_cast<double>(sp.readyNs - sp.dueNs) / 1.0e6, static_cast<double>(ns - sp.dueNs) / 1.0e6,
                             static_cast<unsigned long long>(skipped));
            }
            const double seconds = std::chrono::duration<double>(now - p.since).count();
            if (seconds >= 2.0)
            {
                printPacing(seconds);
                p.since = now;
            }
        }


    }

    void noteShown(const Shown &sp, uint64_t skipped) { noteShownHere(sp, skipped); }

    void noteRepeat()
    {
        if (pacingLogOn())
            ++g_pacing.repeats;
    }

    int64_t now() { return nowNs(); }
    }

    double pictureLowRate(double seconds)
    {
        std::vector<float> v;
        {
            std::lock_guard<std::mutex> lock(pacelog::g_shownIntervals.mutex);
            const int64_t now = pacelog::nowNs(), window = static_cast<int64_t>(seconds * 1.0e9);
            for (auto it = pacelog::g_shownIntervals.v.rbegin(); it != pacelog::g_shownIntervals.v.rend() && now - it->first <= window; ++it)
                v.push_back(it->second);
        }
        if (v.size() < 100u)
            return 0.0;
        const size_t count = v.size() / 100u;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(count - 1u), v.end(), std::greater<float>());
        double sum = 0.0;
        for (size_t i = 0; i < count; ++i)
            sum += v[i];
        return sum > 0.0 ? 1000.0 * static_cast<double>(count) / sum : 0.0;
    }
}
