// The sound ledger (sound_ledger.h), TF_SOUND_LEDGER=1: one line per voice the game keys on and
// what became of it, read on the EE side so that both sound servers can be compared on the same run.
//
// Requests: the frame packet (command 4 of slSIFCommander's SendAsynchronousCommand, 0x1F0470) -
// tag 0x23 / command 17 set the voice's tone, 0x22 its volumes and priority, 0x20 plays it, 0x21
// stops it. Answers: the server's status buffer as the EE parses it (ProcessDeviceStatusBuffer,
// 0x1E3760; the device's copy of the buffer at +0x70): its sorted list of playing voice handles.
// A voice is "heard" when it shows up in that list after its key-on; it "ends" when it leaves the
// list - on its own (sound over), after a stop, or neither (the server took it away: stolen). A
// key-on that never shows up within 0.25 s (and 5 status reads) is NEVER - a sound shorter than the
// server's listing delay can also end that way without being dropped.
//   [ledger] <s> play h <handle> '<tone>' prio <p> vol <l> <r>
//   [ledger] <s> NEVER h <handle> '<tone>' prio <p> (playing <n>)
//   [ledger] <s> end h <handle> '<tone>' after <t> s (stopped after <t> | not stopped) playing <n>
//   [ledger] <s> peak <n> playing voices
// <s> is the EE clock in seconds (as the native server's own logs).

#include "sound_ledger.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace sound_ledger
{
    namespace
    {
        constexpr uint32_t kSendCommand = 0x001F0470u, kStatusParse = 0x001E3760u;
        constexpr uint32_t kCommander = 0x006CE8F4u;
        constexpr uint32_t kClockCycles = 0x006CE5B0u, kSecondsPerCycle = 0x006CB1A4u;

        PS2Runtime::RecompiledFunction g_send = nullptr, g_parse = nullptr;
        uint8_t *g_rdram = nullptr;

        uint32_t rd(uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(g_rdram, a))
                std::memcpy(&v, p, 4);
            return v;
        }
        uint8_t rd8(uint32_t a)
        {
            const uint8_t *p = getConstMemPtr(g_rdram, a);
            return p ? *p : 0u;
        }
        double now()
        {
            const uint64_t cycles = static_cast<uint64_t>(rd(kClockCycles)) | (static_cast<uint64_t>(rd(kClockCycles + 4u)) << 32);
            const uint32_t bits = rd(kSecondsPerCycle);
            float perCycle;
            std::memcpy(&perCycle, &bits, 4);
            return static_cast<double>(cycles) * perCycle;
        }

        struct Voice
        {
            std::string tone;
            int32_t priority = 0;
            uint32_t volumes = 0u;
            double playedAt = -1.0, stoppedAt = -1.0;
            int waits = 0; // status reads since the key-on without it listed
            bool pending = false;
        };
        std::map<int32_t, Voice> g_voices;
        std::set<int32_t> g_playing;
        size_t g_peak = 0u;

        void onPlay(int32_t h)
        {
            Voice &v = g_voices[h];
            if (v.pending && v.playedAt == now())
                return; // (a rewind and a play in one packet)
            v.playedAt = now();
            v.stoppedAt = -1.0;
            v.pending = true;
            v.waits = 0;
            std::fprintf(stderr, "[ledger] %.3f play h %d '%s' prio %d vol %04x %04x\n", now(), h, v.tone.c_str(), v.priority,
                         v.volumes & 0xFFFFu, v.volumes >> 16);
        }

        std::string packetString(uint32_t &at, uint32_t end)
        {
            const uint32_t n = at < end ? rd8(at) : 0u;
            std::string s;
            for (uint32_t i = 0u; i < n && at + 1u + i < end; ++i)
                s += static_cast<char>(rd8(at + 1u + i));
            at += (1u + n + 3u) & ~3u;
            return s;
        }

        void packet(uint32_t at, uint32_t end)
        {
            while (at + 4u <= end)
            {
                const uint32_t tag = rd(at);
                at += 4u;
                switch (tag)
                {
                case 0x01:
                    return;
                case 0x02:
                    at += 8u;
                    break;
                case 0x20:
                case 0x24:
                    onPlay(static_cast<int32_t>(rd(at)));
                    at += 4u;
                    break;
                case 0x21:
                {
                    Voice &v = g_voices[static_cast<int32_t>(rd(at))];
                    if (v.stoppedAt < 0.0)
                        v.stoppedAt = now();
                    at += 4u;
                    break;
                }
                case 0x22:
                {
                    Voice &v = g_voices[static_cast<int32_t>(rd(at))];
                    v.volumes = rd(at + 4u);
                    v.priority = static_cast<int32_t>(rd(at + 12u));
                    at += 24u;
                    break;
                }
                case 0x23:
                {
                    const int32_t h = static_cast<int32_t>(rd(at));
                    at += 4u;
                    g_voices[h].tone = packetString(at, end);
                    break;
                }
                case 0x25:
                    break;
                case 0x30:
                case 0x32:
                case 0x33:
                case 0x42:
                case 0x43:
                case 0x44:
                case 0x46:
                case 0x47:
                    at += 4u;
                    break;
                case 0x31:
                    packetString(at, end);
                    break;
                case 0x40:
                case 0x48:
                    at += 8u;
                    break;
                case 0x41:
                    at += 4u;
                    packetString(at, end);
                    break;
                case 0x45:
                    at += 8u;
                    packetString(at, end);
                    break;
                case 0x49:
                    at += 16u;
                    break;
                default:
                    return; // (unknown: the rest cannot be read)
                }
            }
        }

        void sendCommand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            const uint32_t number = getRegU32(ctx, 4), size = getRegU32(ctx, 5);
            const uint32_t args = rd(rd(kCommander) + 0x18u);
            if (number == 4u)
                packet(args, args + size);
            else if (number == 17u) // VoiceImmediate_SetTone: handle, string (u16 length, 2-aligned)
            {
                const int32_t h = static_cast<int32_t>(rd(args));
                const uint32_t n = rd8(args + 4u) | (rd8(args + 5u) << 8);
                std::string s;
                for (uint32_t i = 0u; i < n && i < 256u; ++i)
                    s += static_cast<char>(rd8(args + 6u + i));
                g_voices[h].tone = s;
            }
            g_send(rdram, ctx, runtime);
        }

        // The game's own requests, before any server sees them: the sound list's play of a sound
        // definition as a 3D world sound (0x27B7F0: a1 the definition, t0 the priority) or an
        // immediate one (0x27B460: a1 the definition). Each takes one of the list's sound nodes (35
        // world / 10 immediate) or steals the least important; 0 back = no node, the sound dropped.
        PS2Runtime::RecompiledFunction g_ask3d = nullptr, g_askImmediate = nullptr;

        // A definition's name: the first string it points at (or holds) - a guess, logged once each.
        std::string defName(uint32_t def)
        {
            static std::map<uint32_t, std::string> names;
            if (auto it = names.find(def); it != names.end())
                return it->second;
            const auto stringAt = [](uint32_t a) {
                std::string s;
                for (uint32_t i = 0u; i < 64u; ++i)
                {
                    const uint8_t c = rd8(a + i);
                    if (c == 0u)
                        break;
                    if (c < 0x20u || c > 0x7Eu)
                        return std::string();
                    s += static_cast<char>(c);
                }
                return s.size() >= 3u ? s : std::string();
            };
            std::string name;
            for (uint32_t i = 0u; i < 16u && name.empty(); ++i)
            {
                const uint32_t w = rd(def + i * 4u);
                if (w >= 0x00100000u && w < PS2_RAM_SIZE)
                    name = stringAt(w);
            }
            if (name.empty())
                name = "def" + std::to_string(def);
            names[def] = name;
            return name;
        }

        void ask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool immediate)
        {
            g_rdram = rdram;
            const uint32_t def = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
            const int32_t priority = static_cast<int32_t>(getRegU32(ctx, 8));
            const std::string name = defName(def);
            (immediate ? g_askImmediate : g_ask3d)(rdram, ctx, runtime);
            const bool done = ctx->pc == ra;
            std::fprintf(stderr, "[ledger] %.3f ask %s '%s' prio %d ra %08x -> %s\n", now(), immediate ? "imm" : "3d", name.c_str(),
                         immediate ? 0 : priority, ra, !done ? "(yielded)" : getRegU32(ctx, 2) ? "node" : "NO NODE");
        }
        void ask3d(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { ask(rdram, ctx, runtime, false); }
        void askImmediate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { ask(rdram, ctx, runtime, true); }

        // At the parse's entry the device's copy holds the newest buffer: three timings, twelve
        // statistics, the pending loads, then n and n handles (-1 empty).
        void statusParse(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            const uint32_t device = getRegU32(ctx, 4);
            const uint32_t buf = rd(device + 0x70u), size = rd(device + 0x6Cu);
            if (buf && size >= 0x48u)
            {
                const uint32_t n = rd(buf + 16u * 4u);
                std::set<int32_t> playing;
                for (uint32_t i = 0u; i < n && i < 64u && (17u + i) * 4u < size; ++i)
                {
                    const int32_t h = static_cast<int32_t>(rd(buf + (17u + i) * 4u));
                    if (h != -1)
                        playing.insert(h);
                }
                const double t = now();
                for (int32_t h : playing)
                    g_voices[h].pending = false;
                for (int32_t h : g_playing)
                    if (!playing.count(h))
                    {
                        Voice &v = g_voices[h];
                        char stop[48];
                        if (v.stoppedAt >= 0.0)
                            std::snprintf(stop, sizeof(stop), "stopped after %.3f", v.stoppedAt - v.playedAt);
                        else
                            std::snprintf(stop, sizeof(stop), "not stopped");
                        std::fprintf(stderr, "[ledger] %.3f end h %d '%s' after %.3f s (%s) playing %zu\n", t, h, v.tone.c_str(),
                                     t - v.playedAt, stop, playing.size());
                    }
                // (by time, not status reads: at 240 frames a second five reads are 21 ms, less than the
                // emulated IOP takes to list a voice)
                for (auto &[h, v] : g_voices)
                    if (v.pending && !playing.count(h) && ++v.waits >= 5 && t - v.playedAt >= 0.25)
                    {
                        v.pending = false;
                        // (TODO 109: whether the game itself stopped it first - its sound node stops a voice the
                        // server has not listed as playing within three of the console's frames, sound_nodes.cpp)
                        char stop[48];
                        if (v.stoppedAt >= 0.0)
                            std::snprintf(stop, sizeof(stop), "stopped after %.3f", v.stoppedAt - v.playedAt);
                        else
                            std::snprintf(stop, sizeof(stop), "not stopped");
                        std::fprintf(stderr, "[ledger] %.3f NEVER h %d '%s' prio %d vol %04x %04x (playing %zu; %s)\n", t, h, v.tone.c_str(),
                                     v.priority, v.volumes & 0xFFFFu, v.volumes >> 16, playing.size(), stop);
                    }
                if (playing.size() > g_peak)
                {
                    g_peak = playing.size();
                    std::fprintf(stderr, "[ledger] %.3f peak %zu playing voices\n", t, g_peak);
                }
                g_playing.swap(playing);
            }
            g_parse(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("TF_SOUND_LEDGER");
        if (!env || *env != '1')
            return;
        g_send = runtime.lookupFunction(kSendCommand);
        g_parse = runtime.lookupFunction(kStatusParse);
        if (!g_send || !g_parse)
            return;
        runtime.registerFunction(kSendCommand, sendCommand);
        runtime.registerFunction(kStatusParse, statusParse);
        if ((g_ask3d = runtime.lookupFunction(0x0027B7F0u)) != nullptr)
            runtime.registerFunction(0x0027B7F0u, ask3d);
        if ((g_askImmediate = runtime.lookupFunction(0x0027B460u)) != nullptr)
            runtime.registerFunction(0x0027B460u, askImmediate);
        std::fprintf(stderr, "[ledger] TF_SOUND_LEDGER: the voices the game keys on and the server's playing list logged\n");
    }
}
