// A debugging aid: TF_PITCH_LOG=1 logs the pitch (a float in $f12, which TF_TRACE_FUNCS does not
// show) that each caller hands the game's sound voices, with the caller (ra) and the voice (a0):
// slVoiceImmediate_PlayStation2::DoSetPitchBend (0x1F1550) and the sound system's parameter call
// (0x282510, a1 the voice's id). A line when a caller's value for a voice changes by more than 5%.
// TF_PITCH_LOG_AFTER=<VBlank tick> starts later.
//
// It also logs (same switch), each with the VBlank tick:
//   [loud]     slVoiceImmediate_PlayStation2::DoSetLoudness (0x1F1540)
//   [engine]   taTransformerEngineSound::Update's car state (0x30D6B0; with TF_PITCH_LOG_ENGINE=1)
//   [vol3d]    slVoice3D_PlayStation2::UpdateChannelVolumes (0x1EA2D0): offset, pan, gain, and a
//   [voice]    dump of the voice (once a second a voice)
//   [doppler]  the 3D Doppler factor (0x1E9460, clamped to 0.25..4)
//   [vcall]    taSoundNode Play (0x27F650) / Stop (0x27F4A0) with the caller and wall-clock ms,
//   [snode]    the node (+0x20 sound id, +0x3C its voice), [sdef] its definition once
//   [pos]      slVoice3D SetPosition (0x1EA8B0; with TF_PITCH_LOG_POS=1)
//   [newv3d] / [delv3d]  the device's make / delete 3D voice (0x1EAB40 / 0x1EAAC0)
// Findings from it: sound_nodes.cpp.
//
// TF_MUSIC_LOG=1 (on its own, either sound server): the EE's level-music calls, each with the
// VBlank tick and the caller (ra) - what the game asks for, the same whichever server runs:
//   [music] switch   taMusicManager::Switch (0x2E65B0): a1 the music file (MusicLevel), f12 fade
//   [music] chan     slMusicStream_PlayStation2::DoSetChannelLoudness (0x1EAFE0): channel, left, right
//   [music] loud     ...::DoSetLoudness (0x1EB0B0): the stream's loudness
//   [music] part     ...::DoSetCurrentTunePartIndex (0x1EAD90)
//   [music] level    taMusicManager's intensity (0x2E8BA0, its result) with the threat counts behind
//                    it, a line when any of them changes: creatures near/in view/aware (+0x2C..+0x3C,
//                    +0x54), threat (+0x18), the player's speed (+0x70), playing/target stem (+0xE0/E4),
//                    the duck (+0x128), the in-view range factor (the player creature's +0xEC: 1, more aiming)
//   [music] sting    a sting on the sting stream (0x2E75C0: id, duck); [music] unduck (0x2E7490);
//   [music] done     the stream's finished query (0x281540), a line when its answer changes
//   [music] trigger  a level trigger starting its enter events / killed (its centre, radius, shape)
//   [music] update   taMusicManager's update (0x2E99E0): calls and the dt it is given, every 5 s of game time
//   [music] file     the music file opened by index (0x2E79A0, the level's start: the warp gate's
//                    WarpGateMusicTrack) / crossfaded to (0x2E6980, NextMusicTrack and the like)
// (TODO 83/85/93: the adaptive music's stems.)

#include "sound_trace.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

namespace sound_trace
{
    namespace
    {
        PS2Runtime::RecompiledFunction g_pitchBend = nullptr, g_parameter = nullptr;
        uint64_t g_after = 0u;
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, float> g_last;

        void note(PS2Runtime *runtime, const char *what, uint32_t ra, uint32_t voice, uint32_t id, float value)
        {
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            if (tick < g_after)
                return;
            auto &last = g_last[{ra, voice, id}];
            if (last != 0.0f && std::fabs(value - last) <= 0.05f * std::fabs(last))
                return;
            last = value;
            std::fprintf(stderr, "[pitch] vblank %llu %s ra=%08x voice=%08x id=%08x value=%.4f\n",
                         static_cast<unsigned long long>(tick), what, ra, voice, id, value);
        }

        void pitchBend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            note(runtime, "bend", getRegU32(ctx, 31), getRegU32(ctx, 4), 0u, ctx->f[12]);
            g_pitchBend(rdram, ctx, runtime);
        }

        PS2Runtime::RecompiledFunction g_loudness = nullptr;

        void loudness(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            note(runtime, "loud", getRegU32(ctx, 31), getRegU32(ctx, 4), 0u, ctx->f[12]);
            g_loudness(rdram, ctx, runtime);
        }

        void parameter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            note(runtime, "param", getRegU32(ctx, 31), getRegU32(ctx, 4), getRegU32(ctx, 5), ctx->f[12]);
            g_parameter(rdram, ctx, runtime);
        }
    }

    namespace
    {
        PS2Runtime::RecompiledFunction g_engineSound = nullptr;

        // taTransformerEngineSound::Update (0x30D6B0): the car's engine state it hands the sound.
        void engineSound(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            const auto word = [rdram](uint32_t a)
            {
                uint32_t v = 0u;
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    std::memcpy(&v, p, sizeof(v));
                return v;
            };
            const auto f = [&](uint32_t a)
            {
                const uint32_t v = word(a);
                float x;
                std::memcpy(&x, &v, sizeof(x));
                return x;
            };
            const uint32_t vehicle = word(getRegU32(ctx, 4) + 8u);
            const uint32_t car = vehicle ? word(vehicle + 0x3Cu) : 0u;
            static const bool engineLog = std::getenv("TF_PITCH_LOG_ENGINE") != nullptr;
            if (car && tick >= g_after && engineLog)
                std::fprintf(stderr, "[engine] vblank %llu rpm(+6C)=%.3f load(+810)=%.3f state(+84)=%u +44=%.3f +48=%.3f\n",
                             static_cast<unsigned long long>(tick), f(car + 0x6Cu), f(car + 0x810u), word(car + 0x84u),
                             f(car + 0x44u), f(car + 0x48u));
            g_engineSound(rdram, ctx, runtime);
        }
    }

    namespace
    {
        PS2Runtime::RecompiledFunction g_channelVolumes = nullptr;

        // slVoice3D_PlayStation2::UpdateChannelVolumes (0x1EA2D0): a0 the voice, a1 the microphone,
        // a2 a vector (x, y, z), f12 and f13. Logged once a second of game time per voice.
        void channelVolumes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            static std::map<uint32_t, uint64_t> lastTick;
            const uint32_t voice = getRegU32(ctx, 4);
            uint64_t &last = lastTick[voice];
            if (tick >= g_after && tick >= last + 60u)
            {
                last = tick;
                float v[3] = {};
                if (const uint8_t *p = getConstMemPtr(rdram, getRegU32(ctx, 6)))
                    std::memcpy(v, p, sizeof(v));
                std::fprintf(stderr, "[vol3d] vblank %llu voice=%08x mic=%08x ra=%08x vec=(%.2f %.2f %.2f) f12=%.4f f13=%.4f\n",
                             static_cast<unsigned long long>(tick), voice, getRegU32(ctx, 5), getRegU32(ctx, 31), v[0], v[1], v[2],
                             ctx->f[12], ctx->f[13]);
                if (const uint8_t *p = getConstMemPtr(rdram, voice))
                {
                    std::fprintf(stderr, "[voice] %08x:", voice);
                    for (uint32_t i = 0u; i < 0x90u; i += 4u)
                    {
                        uint32_t w;
                        std::memcpy(&w, p + i, sizeof(w));
                        std::fprintf(stderr, " %08x", w);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            g_channelVolumes(rdram, ctx, runtime);
        }
    }

    namespace
    {
        PS2Runtime::RecompiledFunction g_doppler = nullptr;

        // 0x1E9460 (leaf): the 3D voice's Doppler factor, clamped to [0.25, 4]. a0 the microphone
        // (+0x10 place, +0x20 velocity), a1 the voice's offset, a2 the voice's velocity, f12.
        void doppler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto vec = [rdram](uint32_t a, float *v)
            {
                if (const uint8_t *p = getConstMemPtr(rdram, a))
                    std::memcpy(v, p, 3 * sizeof(float));
            };
            float mp[3] = {}, mv[3] = {}, off[3] = {}, vv[3] = {};
            const uint32_t mic = getRegU32(ctx, 4), voiceVel = getRegU32(ctx, 6);
            vec(mic + 0x10u, mp);
            vec(mic + 0x20u, mv);
            vec(getRegU32(ctx, 5), off);
            vec(voiceVel, vv);
            const float f12 = ctx->f[12];
            g_doppler(rdram, ctx, runtime);
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            static std::map<uint32_t, uint64_t> lastTick;
            uint64_t &last = lastTick[voiceVel];
            if (tick >= g_after && (tick >= last + 30u || ctx->f[0] > 1.5f || ctx->f[0] < 0.67f))
            {
                last = tick;
                std::fprintf(stderr, "[doppler] vblank %llu vel@%08x=(%.2f %.2f %.2f) mic=(%.2f %.2f %.2f) micvel=(%.2f %.2f %.2f) off=(%.2f %.2f %.2f) f12=%.3f -> %.4f\n",
                             static_cast<unsigned long long>(tick), voiceVel, vv[0], vv[1], vv[2], mp[0], mp[1], mp[2],
                             mv[0], mv[1], mv[2], off[0], off[1], off[2], f12, ctx->f[0]);
            }
        }
    }

    namespace
    {
        PS2Runtime::RecompiledFunction g_newVoice3D = nullptr, g_deleteVoice3D = nullptr;

        // slAudioDevice_PlayStation2 slots 17 (0x1EAB40, makes a 3D voice) and 18 (DoDeleteVoice3D,
        // 0x1EAAC0, a1 the voice).
        void newVoice3D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
            const uint32_t sp = getRegU32(ctx, 29);
            g_newVoice3D(rdram, ctx, runtime);
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            if (tick >= g_after)
                std::fprintf(stderr, "[newv3d] vblank %llu ra=%08x a1=%08x a2=%08x a3=%08x sp=%08x -> %08x%s\n",
                             static_cast<unsigned long long>(tick), ra, a1, a2, a3, sp, getRegU32(ctx, 2),
                             ctx->pc != ra ? " (yielded)" : "");
        }

        void deleteVoice3D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            if (tick >= g_after)
                std::fprintf(stderr, "[delv3d] vblank %llu ra=%08x voice=%08x\n", static_cast<unsigned long long>(tick),
                             getRegU32(ctx, 31), getRegU32(ctx, 5));
            g_deleteVoice3D(rdram, ctx, runtime);
        }
    }

    namespace
    {
        // slVoice3D_PlayStation2's leaf slots: 15 Play (0x1EA760: +0x51 = 1), 16 Stop (0x1EA740:
        // +0x51 = 0) and 3 SetPosition (0x1EA8B0: a1 -> +0x20). Logged with the caller.
        PS2Runtime::RecompiledFunction g_play = nullptr, g_stop = nullptr, g_setPos = nullptr;

        void voiceCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *what)
        {
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            if (tick < g_after)
                return;
            float v[3] = {};
            if (what[0] == 'p' && what[1] == 'o')
                if (const uint8_t *p = getConstMemPtr(rdram, getRegU32(ctx, 5)))
                    std::memcpy(v, p, sizeof(v));
            std::fprintf(stderr, "[vcall] vblank %llu ms=%lld %s voice=%08x ra=%08x pos=(%.2f %.2f %.2f)\n",
                         static_cast<unsigned long long>(tick), static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()), what, getRegU32(ctx, 4), getRegU32(ctx, 31), v[0], v[1], v[2]);
            if (what[0] == 'p' && what[1] == 'o')
                return;
            // taSoundNode (0x27F650 Play / 0x27F4A0 Stop): +0x3C the voice, +0x20 the sound's definition.
            const uint32_t node = getRegU32(ctx, 4);
            uint32_t w[16] = {};
            if (const uint8_t *p = getConstMemPtr(rdram, node))
                std::memcpy(w, p, sizeof(w));
            std::fprintf(stderr, "[snode] %08x:", node);
            for (uint32_t x : w)
                std::fprintf(stderr, " %08x", x);
            std::fprintf(stderr, "\n");
            static std::map<uint32_t, bool> seen;
            if (w[8] && !seen[w[8]])
            {
                seen[w[8]] = true;
                uint32_t d[24] = {};
                if (const uint8_t *p = getConstMemPtr(rdram, w[8]))
                    std::memcpy(d, p, sizeof(d));
                std::fprintf(stderr, "[sdef] %08x:", w[8]);
                for (uint32_t x : d)
                    std::fprintf(stderr, " %08x", x);
                std::fprintf(stderr, "\n");
            }
        }

        void play(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            voiceCall(rdram, ctx, runtime, "play");
            g_play(rdram, ctx, runtime);
        }

        void stop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            voiceCall(rdram, ctx, runtime, "stop");
            g_stop(rdram, ctx, runtime);
        }

        void setPos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool on = std::getenv("TF_PITCH_LOG_POS") != nullptr;
            if (on)
                voiceCall(rdram, ctx, runtime, "pos");
            g_setPos(rdram, ctx, runtime);
        }
    }

    namespace
    {
        PS2Runtime::RecompiledFunction g_musicSwitch = nullptr, g_musicChannel = nullptr, g_musicLoudness = nullptr, g_musicPart = nullptr;
        PS2Runtime::RecompiledFunction g_musicOpenIndex = nullptr, g_musicFadeIndex = nullptr, g_musicUpdate = nullptr;
        PS2Runtime::RecompiledFunction g_musicLevel = nullptr, g_musicSting = nullptr, g_musicUnduck = nullptr, g_musicDone = nullptr;

        uint32_t guestWord(uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }
        float guestFloat(uint8_t *rdram, uint32_t a)
        {
            const uint32_t v = guestWord(rdram, a);
            float f;
            std::memcpy(&f, &v, sizeof(f));
            return f;
        }

        void musicLine(PS2Runtime *runtime, R5900Context *ctx, const char *what, const char *detail)
        {
            std::fprintf(stderr, "[music] vblank %llu %s ra=%08x obj=%08x %s\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), what, getRegU32(ctx, 31),
                         getRegU32(ctx, 4), detail);
        }

        void musicSwitch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            char b[64];
            std::snprintf(b, sizeof(b), "level %d fade %.3f", static_cast<int32_t>(getRegU32(ctx, 5)), ctx->f[12]);
            // (the game calls it every frame while the level is above the target pair: a line when it changes)
            static std::string last;
            if (last != b)
                musicLine(runtime, ctx, "switch", b);
            last = b;
            g_musicSwitch(rdram, ctx, runtime);
        }

        void musicChannel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            char b[64];
            std::snprintf(b, sizeof(b), "channel %d %.3f %.3f", static_cast<int32_t>(getRegU32(ctx, 5)), ctx->f[12], ctx->f[13]);
            musicLine(runtime, ctx, "chan", b);
            g_musicChannel(rdram, ctx, runtime);
        }

        void musicLoudness(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            char b[48];
            std::snprintf(b, sizeof(b), "%.3f", ctx->f[12]);
            musicLine(runtime, ctx, "loud", b);
            g_musicLoudness(rdram, ctx, runtime);
        }

        void musicPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            char b[48];
            std::snprintf(b, sizeof(b), "part %d", static_cast<int32_t>(getRegU32(ctx, 5)));
            musicLine(runtime, ctx, "part", b);
            g_musicPart(rdram, ctx, runtime);
        }

        // taPlayerManager::GetPlayer (0x2D4F30) as CheckCreatureThreat (0x2E7EF0-0x2E8610) calls it: the player, whose
        // creature (+8) holds the factor the in-view range is multiplied by (+0xEC: 1, more in the aiming view) - logged
        // with the counts (TODO 93b: a far enemy seen through the zoom).
        PS2Runtime::RecompiledFunction g_getPlayer = nullptr;
        uint32_t g_musicPlayer = 0u;
        void getPlayer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            g_getPlayer(rdram, ctx, runtime);
            if (ra >= 0x002E7EF0u && ra < 0x002E8610u && ctx->pc == ra)
                g_musicPlayer = getRegU32(ctx, 2);
        }

        void musicLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
            g_musicLevel(rdram, ctx, runtime);
            const int32_t level = static_cast<int32_t>(getRegU32(ctx, 2));
            char b[320];
            std::snprintf(b, sizeof(b),
                          "-> %d  threatening %d aware %d inview %d near %d alive %d alerted %d threat %d types %08x%08x/%08x%08x speed %.1f stem %d->%d duck %.2f range x%.2f",
                          level, static_cast<int32_t>(guestWord(rdram, self + 0x2Cu)), static_cast<int32_t>(guestWord(rdram, self + 0x30u)),
                          static_cast<int32_t>(guestWord(rdram, self + 0x34u)), static_cast<int32_t>(guestWord(rdram, self + 0x3Cu)),
                          static_cast<int32_t>(guestWord(rdram, self + 0x38u)), static_cast<int32_t>(guestWord(rdram, self + 0x54u)),
                          static_cast<int32_t>(guestWord(rdram, self + 0x18u)), guestWord(rdram, self + 0x0Cu), guestWord(rdram, self + 0x08u),
                          guestWord(rdram, self + 0x14u), guestWord(rdram, self + 0x10u), guestFloat(rdram, self + 0x70u),
                          static_cast<int32_t>(guestWord(rdram, self + 0xE0u)), static_cast<int32_t>(guestWord(rdram, self + 0xE4u)),
                          guestFloat(rdram, self + 0x128u),
                          g_musicPlayer && guestWord(rdram, g_musicPlayer + 8u) ? guestFloat(rdram, guestWord(rdram, g_musicPlayer + 8u) + 0xECu) : 0.0f);
            static std::string last;
            // (the speed only by whether it is over the game's 20)
            std::string key = b;
            if (const size_t at = key.find(" speed "); at != std::string::npos)
            {
                const size_t end = key.find(" stem ");
                key.replace(at, end - at, guestFloat(rdram, self + 0x70u) > 20.0f ? " fast" : " slow");
            }
            if (key == last)
                return;
            last = key;
            std::fprintf(stderr, "[music] vblank %llu level ra=%08x %s\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), ra, b);
        }

        void musicSting(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            char b[64];
            std::snprintf(b, sizeof(b), "id %d duck %d", static_cast<int32_t>(getRegU32(ctx, 5)), static_cast<int32_t>(getRegU32(ctx, 6)));
            musicLine(runtime, ctx, "sting", b);
            g_musicSting(rdram, ctx, runtime);
        }

        void musicUnduck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            musicLine(runtime, ctx, "unduck", "");
            g_musicUnduck(rdram, ctx, runtime);
        }

        void musicDone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stream = getRegU32(ctx, 4), ra = getRegU32(ctx, 31);
            g_musicDone(rdram, ctx, runtime);
            const uint32_t v = getRegU32(ctx, 2);
            static std::map<uint32_t, uint32_t> last;
            auto it = last.find(stream);
            if (it != last.end() && it->second == v)
                return;
            last[stream] = v;
            std::fprintf(stderr, "[music] vblank %llu done ra=%08x obj=%08x -> %u\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), ra, stream, v);
        }

        void musicOpenIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            char b[48];
            std::snprintf(b, sizeof(b), "open index %d", static_cast<int32_t>(getRegU32(ctx, 5)));
            musicLine(runtime, ctx, "file", b);
            g_musicOpenIndex(rdram, ctx, runtime);
        }

        void musicFadeIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            char b[48];
            std::snprintf(b, sizeof(b), "crossfade to index %d", static_cast<int32_t>(getRegU32(ctx, 5)));
            musicLine(runtime, ctx, "file", b);
            g_musicFadeIndex(rdram, ctx, runtime);
        }

        void musicUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static uint64_t from = 0u, calls = 0u;
            static double sum = 0.0;
            static float lo = 1e9f, hi = 0.0f;
            const float dt = ctx->f[12];
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            if (calls == 0u)
                from = tick;
            ++calls;
            sum += dt;
            lo = std::min(lo, dt);
            hi = std::max(hi, dt);
            if (tick >= from + 300u)
            {
                std::fprintf(stderr, "[music] vblank %llu update: %llu calls in %llu vblanks, dt sum %.3f s (%.4f..%.4f)\n",
                             static_cast<unsigned long long>(tick), static_cast<unsigned long long>(calls),
                             static_cast<unsigned long long>(tick - from), sum, lo, hi);
                calls = 0u;
                sum = 0.0;
                lo = 1e9f;
                hi = 0.0f;
            }
            g_musicUpdate(rdram, ctx, runtime);
        }

        // The level's triggers (taCreatureTrigger, TODO 83: the music cues like Antarctica's NextMusicTrack at the
        // cliff): a line when one starts its enter events (DelayBeforeTriggerEnterEvents 0x2F9CB0, its first call:
        // the state's time at +0x70 still 0) and when one is killed (vfunc_53 0x2FA070: fired, or by the trigger
        // that names it in TriggerLink), with its centre (+0x40), radius (+0x74 squared) and shape (+0x90).
        PS2Runtime::RecompiledFunction g_triggerEnter = nullptr, g_triggerKill = nullptr;
        void triggerLine(uint8_t *rdram, PS2Runtime *runtime, const char *what, uint32_t self, uint32_t ra)
        {
            std::fprintf(stderr, "[music] vblank %llu trigger %s ra=%08x obj=%08x at (%.1f, %.1f, %.1f) radius %.1f shape %d\n",
                         static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), what, ra, self,
                         guestFloat(rdram, self + 0x40u), guestFloat(rdram, self + 0x44u), guestFloat(rdram, self + 0x48u),
                         std::sqrt(std::max(0.0f, guestFloat(rdram, self + 0x74u))), static_cast<int32_t>(guestWord(rdram, self + 0x90u)));
        }
        void triggerEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = getRegU32(ctx, 4);
            if (guestFloat(rdram, self + 0x70u) == 0.0f)
                triggerLine(rdram, runtime, "enter", self, getRegU32(ctx, 31));
            g_triggerEnter(rdram, ctx, runtime);
        }
        // Where the player is, as the triggers see it (TODO 83: whether a playtest came near a music trigger - in every
        // level, Antarctica too, where TF_POS_LOG has nothing): WaitingForPlayerToEnter (0x2F9EF0) asks 0x2A4050(out,
        // centre) for the nearest player's position (x y z, z up) and then tests it against the shape. A line every 2 s of
        // VBlanks when the player moved: the position, and the waiting trigger nearest to entering (distance to its centre
        // less its radius; < 0 = inside, shape 1 = sphere).
        PS2Runtime::RecompiledFunction g_triggerPlayer = nullptr;
        void triggerPlayer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t out = getRegU32(ctx, 4), centre = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
            g_triggerPlayer(rdram, ctx, runtime);
            if (ra != 0x002F9FC0u)
                return;
            static uint64_t lastTick = 0u, frameTick = ~0ull;
            static float last[3] = {1e9f, 1e9f, 1e9f}, nearest = 1e9f, nearestCentre[3] = {}, done = 1e9f, doneCentre[3] = {};
            static uint32_t nearestObj = 0u, doneObj = 0u; // (done*: the last whole frame's)
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            const float p[3] = {guestFloat(rdram, out), guestFloat(rdram, out + 4u), guestFloat(rdram, out + 8u)};
            const float c[3] = {guestFloat(rdram, centre), guestFloat(rdram, centre + 4u), guestFloat(rdram, centre + 8u)};
            const float margin = std::sqrt((p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2])) -
                                 std::sqrt(std::max(0.0f, guestFloat(rdram, centre + 0x34u)));
            if (tick != frameTick)
            {
                frameTick = tick;
                done = nearest;
                doneObj = nearestObj;
                std::copy(nearestCentre, nearestCentre + 3, doneCentre);
                nearest = 1e9f;
            }
            if (margin < nearest)
            {
                nearest = margin;
                nearestObj = centre - 0x40u;
                std::copy(c, c + 3, nearestCentre);
            }
            if (tick >= lastTick + 120u && std::fabs(p[0] - last[0]) + std::fabs(p[1] - last[1]) + std::fabs(p[2] - last[2]) > 1.0f)
            {
                std::fprintf(stderr, "[music] vblank %llu player at (%.1f, %.1f, %.1f); nearest waiting trigger obj=%08x at (%.1f, %.1f, %.1f): %.1f outside\n",
                             static_cast<unsigned long long>(tick), p[0], p[1], p[2], doneObj, doneCentre[0], doneCentre[1], doneCentre[2], done);
                std::copy(p, p + 3, last);
                lastTick = tick;
            }
        }
        void triggerKill(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            triggerLine(rdram, runtime, "killed", getRegU32(ctx, 4), getRegU32(ctx, 31));
            g_triggerKill(rdram, ctx, runtime);
        }

        void installMusicLog(PS2Runtime &runtime)
        {
            const char *on = std::getenv("TF_MUSIC_LOG");
            if (!on || *on != '1')
                return;
            if ((g_musicSwitch = runtime.lookupFunction(0x002E65B0u)) != nullptr)
                runtime.registerFunction(0x002E65B0u, musicSwitch);
            if ((g_musicChannel = runtime.lookupFunction(0x001EAFE0u)) != nullptr)
                runtime.registerFunction(0x001EAFE0u, musicChannel);
            if ((g_musicLoudness = runtime.lookupFunction(0x001EB0B0u)) != nullptr)
                runtime.registerFunction(0x001EB0B0u, musicLoudness);
            if ((g_musicPart = runtime.lookupFunction(0x001EAD90u)) != nullptr)
                runtime.registerFunction(0x001EAD90u, musicPart);
            if ((g_musicUpdate = runtime.lookupFunction(0x002E99E0u)) != nullptr)
                runtime.registerFunction(0x002E99E0u, musicUpdate);
            if ((g_musicOpenIndex = runtime.lookupFunction(0x002E79A0u)) != nullptr)
                runtime.registerFunction(0x002E79A0u, musicOpenIndex);
            if ((g_musicFadeIndex = runtime.lookupFunction(0x002E6980u)) != nullptr)
                runtime.registerFunction(0x002E6980u, musicFadeIndex);
            if ((g_triggerEnter = runtime.lookupFunction(0x002F9CB0u)) != nullptr)
                runtime.registerFunction(0x002F9CB0u, triggerEnter);
            if ((g_triggerKill = runtime.lookupFunction(0x002FA070u)) != nullptr)
                runtime.registerFunction(0x002FA070u, triggerKill);
            if ((g_triggerPlayer = runtime.lookupFunction(0x002A4050u)) != nullptr)
                runtime.registerFunction(0x002A4050u, triggerPlayer);
            if ((g_getPlayer = runtime.lookupFunction(0x002D4F30u)) != nullptr)
                runtime.registerFunction(0x002D4F30u, getPlayer);
            if ((g_musicLevel = runtime.lookupFunction(0x002E8BA0u)) != nullptr)
                runtime.registerFunction(0x002E8BA0u, musicLevel);
            if ((g_musicSting = runtime.lookupFunction(0x002E75C0u)) != nullptr)
                runtime.registerFunction(0x002E75C0u, musicSting);
            if ((g_musicUnduck = runtime.lookupFunction(0x002E7490u)) != nullptr)
                runtime.registerFunction(0x002E7490u, musicUnduck);
            if ((g_musicDone = runtime.lookupFunction(0x00281540u)) != nullptr)
                runtime.registerFunction(0x00281540u, musicDone);
            std::fprintf(stderr, "[music] TF_MUSIC_LOG: the level music's calls logged\n");
        }
    }

    void install(PS2Runtime &runtime)
    {
        installMusicLog(runtime);
        const char *on = std::getenv("TF_PITCH_LOG");
        if (!on || *on != '1')
            return;
        if (const char *after = std::getenv("TF_PITCH_LOG_AFTER"))
            g_after = std::strtoull(after, nullptr, 10);
        if ((g_pitchBend = runtime.lookupFunction(0x001F1550u)) != nullptr)
            runtime.registerFunction(0x001F1550u, pitchBend);
        if ((g_parameter = runtime.lookupFunction(0x00282510u)) != nullptr)
            runtime.registerFunction(0x00282510u, parameter);
        if ((g_loudness = runtime.lookupFunction(0x001F1540u)) != nullptr)
            runtime.registerFunction(0x001F1540u, loudness);
        if ((g_channelVolumes = runtime.lookupFunction(0x001EA2D0u)) != nullptr)
            runtime.registerFunction(0x001EA2D0u, channelVolumes);
        if ((g_engineSound = runtime.lookupFunction(0x0030D6B0u)) != nullptr)
            runtime.registerFunction(0x0030D6B0u, engineSound);
        if ((g_play = runtime.lookupFunction(0x0027F650u)) != nullptr)
            runtime.registerFunction(0x0027F650u, play);
        if ((g_stop = runtime.lookupFunction(0x0027F4A0u)) != nullptr)
            runtime.registerFunction(0x0027F4A0u, stop);
        if ((g_setPos = runtime.lookupFunction(0x001EA8B0u)) != nullptr)
            runtime.registerFunction(0x001EA8B0u, setPos);
        if ((g_newVoice3D = runtime.lookupFunction(0x001EAB40u)) != nullptr)
            runtime.registerFunction(0x001EAB40u, newVoice3D);
        if ((g_deleteVoice3D = runtime.lookupFunction(0x001EAAC0u)) != nullptr)
            runtime.registerFunction(0x001EAAC0u, deleteVoice3D);
        if ((g_doppler = runtime.lookupFunction(0x001E9460u)) != nullptr)
            runtime.registerFunction(0x001E9460u, doppler);
    }
}
