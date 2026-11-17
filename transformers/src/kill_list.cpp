// The game's kill list kept to the episode it was made in (kill_list.h, docs/TODO.md item 412).
//
// The progress record (taMetaGame::GetProgressData's object, 0x7170E0) keeps ONE list of the placed creatures killed:
// 512 bits at +0x2F1, by the creature's index among the level's placed creatures that stay dead (their placement's
// flag 2). The game writes it at a checkpoint (taCreatureSystem's RecordDeadCreatures, inlined in SuspendAll 0x2B0700
// and 0x2B0880, through SetCreatureDead 0x350F80) and reads it when a level of an UNFINISHED episode loads (the
// creature system's start-up and Reset 0x2B0CC0, through GetCreatureDead 0x350F10): a creature whose bit is set gets
// status 0x10 and is never spawned. It empties the list only when an episode is completed (0x351AD0), at a new game
// and at the carrier's second map - never when a DIFFERENT episode is entered. On the PS2 the order of the episodes
// keeps one unfinished at a time, so the list always belongs to it; with several unfinished ones (F1's unlocks, the
// cheat profile) one episode's kills were applied index for index to the next: Amazon's list made Deep Amazon's
// creature 66 - Cyclonus - and its grunts 53-93 dead before they ever appeared (the user's 412: no boss, the level
// stuck).
//
// Here the list belongs to the episode it was written in (the episode being played, 0x716EF0 +0: taMetaGame's
// episode data, GetEpisode 0x350D30):
//   - a level that loads in another episode than the list's finds it empty (cleared at the first GetCreatureDead of
//     the load, index 0);
//   - a checkpoint in another unfinished episode starts a new list; a checkpoint in a COMPLETED episode leaves the
//     list alone (the game never reads it there: a replay of a finished level does not touch the current one's list);
//   - the same unfinished episode entered again keeps its list, as the game does.
// The list's episode is the port's own (not in the game's memory, so not in a save state or on the card): where the
// list changed without us (a save state or the card loaded, the game's own clear) it is taken again, the same game
// frame (frame()), from the progress record's last warp gate used (+0x2E8, saved with the checkpoint that wrote the
// list; the HQ's Drop Zones write the NEXT episode there before its level loads, so it is read at the change, not at
// the load), or none for an empty list.
// TF_KILL_LIST=0: the original behaviour; TF_KILL_LIST_TRACE=1: every list write and read logged.

#include "kill_list.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "game_version.h" // (TF_GA: the code addresses that differ in the PAL release)

namespace kill_list
{
    namespace
    {
        constexpr uint32_t kGetDead = TF_GA(0x00350F10u); // taMetaGameProgressData::GetCreatureDead(index)
        constexpr uint32_t kSetDead = TF_GA(0x00350F80u); // taMetaGameProgressData::SetCreatureDead(index)
        constexpr uint32_t kProgress = 0x007170E0u;       // (taMetaGame::GetProgressData, the decomp's D_007170E0)
        constexpr uint32_t kEpisodeData = 0x00716EF0u;    // (taMetaGame::GetEpisodeData: +0 the episode played)
        constexpr uint32_t kBits = 0x2F1u, kBitBytes = 0x40u, kLastGateEpisode = 0x2E8u;
        constexpr uint32_t kSlots = 0x34u, kSlotStride = 0x4Cu; // (an episode's flags: bit 1 completed)
        constexpr int kEpisodes = 9;
        const char *const kNames[kEpisodes] = {"Amazon", "Antarctica", "Deep Amazon", "Mid-Atlantic", "Mid-Atlantic (defeated)",
                                               "Alaska", "Starship", "Pacific", "Cybertron"};

        PS2Runtime::RecompiledFunction g_getDead = nullptr, g_setDead = nullptr;
        bool g_trace = false;
        int g_listEpisode = -1;                    // the episode the list was written in (-1: none / not known)
        std::array<uint8_t, kBitBytes> g_seen{};   // the list as we last saw it (a change without us: a load)

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
        const char *name(int episode) { return episode >= 0 && episode < kEpisodes ? kNames[episode] : "?"; }
        int episodeOf(uint32_t value) { return static_cast<int32_t>(value) >= 0 && value < static_cast<uint32_t>(kEpisodes) ? static_cast<int>(value) : -1; }
        int playing(const uint8_t *rdram) { return episodeOf(word(rdram, kEpisodeData)); }
        bool completed(const uint8_t *rdram, int episode) { return episode >= 0 && (word(rdram, kProgress + kSlots + kSlotStride * episode) & 2u) != 0u; }

        uint8_t *list(uint8_t *rdram) { return getMemPtr(rdram, kProgress + kBits); }
        int count(const uint8_t *bits)
        {
            int n = 0;
            for (uint32_t k = 0; k < kBitBytes; ++k)
                for (uint8_t b = bits[k]; b; b &= static_cast<uint8_t>(b - 1u))
                    ++n;
            return n;
        }

        // The list changed without us (a save state or the card loaded, an episode completed): whose it is, again.
        void resync(uint8_t *rdram)
        {
            uint8_t *bits = list(rdram);
            if (!bits || std::memcmp(bits, g_seen.data(), kBitBytes) == 0)
                return;
            const int n = count(bits);
            g_listEpisode = n ? episodeOf(word(rdram, kProgress + kLastGateEpisode)) : -1;
            std::memcpy(g_seen.data(), bits, kBitBytes);
            if (g_trace || n)
                std::fprintf(stderr, "[kill list] %d creature(s) in the list, taken as %s's (the last warp gate used)\n", n, name(g_listEpisode));
        }

        void empty(uint8_t *rdram, int episode, const char *when)
        {
            uint8_t *bits = list(rdram);
            if (!bits)
                return;
            const int n = count(bits);
            std::fprintf(stderr, "[kill list] %s: %s's list of %d dead creature(s) not used in %s - emptied (TF_KILL_LIST=0: as the PS2)\n", when,
                         name(g_listEpisode), n, name(episode));
            std::memset(bits, 0, kBitBytes);
            std::memcpy(g_seen.data(), bits, kBitBytes);
        }

        // GetCreatureDead(progress, index): read by an unfinished episode's level as it loads, index 0 first.
        void getDead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kGetDead && getRegU32(ctx, 4) == kProgress && getRegU32(ctx, 5) == 0u)
            {
                resync(rdram);
                const int now = playing(rdram);
                uint8_t *bits = list(rdram);
                if (bits && now >= 0 && g_listEpisode >= 0 && g_listEpisode != now && count(bits))
                    empty(rdram, now, "a level loads");
                if (bits && count(bits))
                    g_listEpisode = now;
                if (g_trace)
                    std::fprintf(stderr, "[kill list] %s loads: %d dead creature(s) kept dead\n", name(now), bits ? count(bits) : 0);
            }
            g_getDead(rdram, ctx, runtime);
        }

        // SetCreatureDead(progress, index): a checkpoint writes the level's dead creatures.
        void setDead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kSetDead && getRegU32(ctx, 4) == kProgress)
            {
                resync(rdram);
                const int now = playing(rdram);
                if (now >= 0 && completed(rdram, now))
                {
                    // (a finished episode replayed: the game never reads the list there; the current one's kept)
                    if (g_trace)
                        std::fprintf(stderr, "[kill list] %s (completed): creature %u not written\n", name(now), getRegU32(ctx, 5));
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
                if (now >= 0 && g_listEpisode >= 0 && g_listEpisode != now && list(rdram) && count(list(rdram)))
                    empty(rdram, now, "a checkpoint");
                if (now >= 0)
                    g_listEpisode = now;
                g_setDead(rdram, ctx, runtime);
                if (const uint8_t *bits = list(rdram))
                    std::memcpy(g_seen.data(), bits, kBitBytes);
                if (g_trace)
                    std::fprintf(stderr, "[kill list] %s: creature %u written dead\n", name(now), getRegU32(ctx, 5));
                return;
            }
            g_setDead(rdram, ctx, runtime);
        }

        void hook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction &original, PS2Runtime::RecompiledFunction fn)
        {
            if ((original = runtime.lookupFunction(address)) != nullptr)
                runtime.registerFunction(address, fn);
            else
                std::fprintf(stderr, "[kill list] nothing at %08x to hook\n", address);
        }
    }

    void frame(uint8_t *rdram)
    {
        if (g_getDead)
            resync(rdram);
    }

    void install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("TF_KILL_LIST");
        if (v && *v == '0')
        {
            std::fprintf(stderr, "[kill list] TF_KILL_LIST=0: one kill list for every episode, as the PS2\n");
            return;
        }
        g_trace = std::getenv("TF_KILL_LIST_TRACE") && *std::getenv("TF_KILL_LIST_TRACE") == '1';
        hook(runtime, kGetDead, g_getDead, getDead);
        hook(runtime, kSetDead, g_setDead, setDead);
    }
}
