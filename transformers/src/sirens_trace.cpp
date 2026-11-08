// SIF RPC recorder (sirens_trace.h).
//
// Wraps the game's sceSifBindRpc (0x1225D8: a0 the client, a1 the server id) and sceSifCallRpc
// (0x1227A8), both host stubs in the runtime. The game is built with GCC's EABI, so the call's
// nine arguments are a0-a3, t0-t3 and one on the stack: client, function, mode, send buffer,
// send size, receive buffer, receive size, end function, end parameter.
//
// The file is binary, one record a call:
//   "SRPC" u32 tick-low u32 tick-high u32 sid u32 fn u32 mode u32 caller (ra - 8)
//          u32 sendSize u32 receiveSize u32 sentBytes u32 receivedBytes, then the bytes.
// At most 64 KB of a send buffer and 4 KB of a receive buffer are kept; a nowait call (mode bit
// 0) records no received bytes (they arrive later). A summary per (server, function, caller) goes
// to stderr at exit.

#include "sirens_trace.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace sirens_trace
{
    namespace
    {
        constexpr uint32_t kBindRpc = 0x001225D8u, kCallRpc = 0x001227A8u;
        constexpr uint32_t kMaxSend = 0x10000u, kMaxReceive = 0x1000u;

        PS2Runtime::RecompiledFunction g_bind = nullptr, g_call = nullptr;
        std::FILE *g_out = nullptr;
        std::mutex g_mutex;
        std::unordered_map<uint32_t, uint32_t> g_sidOf; // client -> server id
        struct Stat
        {
            uint64_t calls = 0u, bytes = 0u;
            uint32_t minSize = 0xFFFFFFFFu, maxSize = 0u;
        };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Stat> g_stats; // (sid, fn, caller)

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, sizeof(v));
            return v;
        }

        void put(uint32_t v) { std::fwrite(&v, 4, 1, g_out); }

        void bytes(const uint8_t *rdram, uint32_t address, uint32_t size)
        {
            // Page by page: the buffer may cross the end of a mapping.
            for (uint32_t done = 0u; done < size;)
            {
                const uint32_t n = std::min(size - done, 0x1000u - ((address + done) & 0xFFFu));
                const uint8_t *p = getConstMemPtr(rdram, address + done);
                if (p)
                    std::fwrite(p, 1, n, g_out);
                else
                {
                    static const uint8_t zeros[0x1000] = {};
                    std::fwrite(zeros, 1, n, g_out);
                }
                done += n;
            }
        }

        void bindHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t client = getRegU32(ctx, 4), sid = getRegU32(ctx, 5);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_sidOf[client] = sid;
                std::fprintf(stderr, "[sirens] bind client %08x to server %08x\n", client, sid);
            }
            g_bind(rdram, ctx, runtime);
        }

        void callHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t client = getRegU32(ctx, 4), fn = getRegU32(ctx, 5), mode = getRegU32(ctx, 6);
            const uint32_t send = getRegU32(ctx, 7), sendSize = getRegU32(ctx, 8);
            const uint32_t receive = getRegU32(ctx, 9), receiveSize = getRegU32(ctx, 10);
            const uint32_t returnAddress = getRegU32(ctx, 31), caller = returnAddress - 8u;
            const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
            uint32_t sid = 0u;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (auto it = g_sidOf.find(client); it != g_sidOf.end())
                    sid = it->second;
                Stat &s = g_stats[{sid, fn, caller}];
                ++s.calls;
                s.bytes += sendSize;
                s.minSize = std::min(s.minSize, sendSize);
                s.maxSize = std::max(s.maxSize, sendSize);
            }
            // The bytes sent: read before the call (the server may reuse the buffer).
            const uint32_t sent = std::min(sendSize, kMaxSend);
            std::lock_guard<std::mutex> lock(g_mutex);
            const long recordAt = std::ftell(g_out);
            std::fwrite("SRPC", 1, 4, g_out);
            put(static_cast<uint32_t>(tick));
            put(static_cast<uint32_t>(tick >> 32));
            put(sid);
            put(fn);
            put(mode);
            put(caller);
            put(sendSize);
            put(receiveSize);
            put(sent);
            put(0u); // received bytes: filled in below
            bytes(rdram, send, sent);
            g_call(rdram, ctx, runtime); // (a host stub: it returns)
            uint32_t received = 0u;
            if (!(mode & 1u) && receive && receiveSize)
            {
                received = std::min(receiveSize, kMaxReceive);
                bytes(rdram, receive, received);
                const long end = std::ftell(g_out);
                std::fseek(g_out, recordAt + 40, SEEK_SET); // the received-bytes word (header: magic + 10 words = 44 bytes)
                put(received);
                std::fseek(g_out, end, SEEK_SET);
            }
        }

        void summary()
        {
            if (!g_out)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            std::fflush(g_out);
            std::fprintf(stderr, "[sirens] summary: server function caller calls bytes min max\n");
            for (const auto &[key, s] : g_stats)
                std::fprintf(stderr, "[sirens]   %08x %4u %08x %8llu %10llu %6u %6u\n", std::get<0>(key), std::get<1>(key),
                             std::get<2>(key), static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.bytes),
                             s.minSize, s.maxSize);
        }
    }

    // TF_SIRENS_SCHEMA=<file>: the commands as the EE builds them, through slSIFTransport_PS2.
    // A text line per BeginCommand
    //   "B tick transport mode command name caller"
    // and one per value streamed in or out
    //   "< transport type value" (an argument), "> transport type" (a result read),
    //   "R transport type" (a result required).
    // Values are read before the original runs (the recompiled originals may stop mid-call).
    namespace schema
    {
        std::FILE *g_text = nullptr;
        PS2Runtime *g_runtime = nullptr;

        const char *const kNames[] = {
            "Sys_NOP", "Sys_VersionQuery", "Sys_Startup", "Sys_Shutdown", "Sys_FramePacket", "Sys_CommandBatch",
            "Sys_SetOptions", "Sys_QueryStatus", "Sys_QueryIOPMemory", "Sys_QuerySPUMemory", "Sys_QueryIOPLocalMemory",
            "Sys_QueryIOPStartupMemory", "Master_SilenceVoices", "Master_SetOptions", "VoiceImmediate_Create",
            "VoiceImmediate_Destroy", "VoiceImmediate_Update", "VoiceImmediate_SetTone", "VoiceImmediate_Play",
            "VoiceImmediate_Stop", "VoiceImmediate_Rewind", "VoiceImmediate_ConnectMIDIChannelSlave", "VoiceImmediate_Sort",
            "ToneLibrary_Create", "ToneLibrary_Destroy", "ToneLibrary_StringCommand", "ToneLibrary_Query",
            "DigitalFX_Command", "DigitalFX_StringCommand", "SoundStream_Create", "SoundStream_Destroy",
            "SoundStream_AllocateDMABuffer", "SoundStream_Command", "SoundStream_Query", "SoundStream_StringCommand",
            "SoundStream_SetChannelBalance", "SoundStream_ReserveBufferSpace", "SoundStream_ReserveChannels",
            "Debug_ShowSPUFragmentationReport", "TDG_Core_Command", "TDG_Fish_Command"};

        enum Kind
        {
            kBegin,
            kArg,
            kResult,
            kRequire,
            kFrame, // slAudioDevice_PlayStation2's frame update (0x1E5850): the packet starts
            kStatus, // ProcessDeviceStatusBuffer (0x1E3760): the status the server sent, as it arrives
            kMemory, // SendAsynchronousMemoryRequest (0x1F0170): dest, src (IOP), size
            kWriter // a function that appends to the frame packet: its object and the packet offset
        };
        struct Hook
        {
            uint32_t address;
            Kind kind;
            const char *type; // the value's type: i u us f b p cmd str
            PS2Runtime::RecompiledFunction original;
        };
        Hook g_hooks[] = {
            {0x001EF9A0u, kBegin, "cmd", nullptr},  // BeginCommand
            {0x001EFAD0u, kArg, "cmd", nullptr},    // << slSIFCommand
            {0x001EF6E0u, kArg, "p", nullptr},      // << void*
            {0x001EF7A0u, kArg, "u", nullptr},      // << unsigned int
            {0x001EFB30u, kArg, "us", nullptr},     // << unsigned short
            {0x001EF800u, kArg, "b", nullptr},      // << bool
            {0x001EF740u, kArg, "f", nullptr},      // << float
            {0x001EF830u, kArg, "i", nullptr},      // << int
            {0x001EF890u, kArg, "str", nullptr},    // << std::string
            {0x001EF590u, kResult, "u", nullptr},   // >> unsigned int
            {0x001EFB90u, kResult, "us", nullptr},  // >> unsigned short
            {0x001EF5F0u, kResult, "i", nullptr},   // >> int
            {0x001EF650u, kResult, "str", nullptr}, // >> std::string
            {0x001EF4A0u, kRequire, "u", nullptr},  // Require unsigned int
            {0x001EFBF0u, kRequire, "us", nullptr}, // Require unsigned short
            {0x001EF4F0u, kRequire, "i", nullptr},  // Require int
            {0x001EF540u, kRequire, "str", nullptr}, // Require std::string
            {0x001E5850u, kFrame, "frame", nullptr},  // slAudioDevice_PlayStation2::vfunc_08
            {0x001E3760u, kStatus, "status", nullptr}, // ProcessDeviceStatusBuffer
            {0x001F0170u, kMemory, "memory", nullptr}, // SendAsynchronousMemoryRequest
            {0x001F1120u, kWriter, "slVoiceImmediate_PlayStation2::EnqueuePlayingState", nullptr},
            {0x001EA490u, kWriter, "func_001EA490", nullptr},
            {0x001EA1C0u, kWriter, "func_001EA1C0", nullptr},
            {0x001EA230u, kWriter, "func_001EA230", nullptr},
            {0x001E8AB0u, kWriter, "func_001E8AB0", nullptr},
            {0x001EB470u, kWriter, "slMusicStream_PlayStation2_slot1", nullptr},
            {0x001E3500u, kWriter, "slVoice3D_PlayStation2_slot1", nullptr},
            {0x001E9DA0u, kWriter, "slToneLibrary_PlayStation2_slot1", nullptr},
            {0x001E92E0u, kWriter, "slMicrophone3D_PlayStation2_slot1", nullptr},
            {0x001E88F0u, kWriter, "slDigitalFx_PlayStation2_slot1", nullptr},
            {0x001F1560u, kWriter, "slVoiceImmediate_PlayStation2_slot1", nullptr}
        };
        uint32_t g_device = 0u; // the audio device of the last frame update

        // A CodeWarrior std::string (long form: word0 = capacity<<1 | 1, +4 size, +8 pointer;
        // short form: the first byte = size<<1, the characters from +1).
        std::string guestString(const uint8_t *rdram, uint32_t s)
        {
            const uint32_t w0 = word(rdram, s);
            uint32_t size = 0u, chars = 0u;
            if (w0 & 1u)
            {
                size = word(rdram, s + 4u);
                chars = word(rdram, s + 8u);
            }
            else
            {
                size = (w0 & 0xFFu) >> 1;
                chars = s + 1u;
            }
            std::string out;
            for (uint32_t i = 0u; i < size && i < 200u; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, chars + i);
                const char c = p ? static_cast<char>(*p) : '?';
                out += (c >= 32 && c < 127 && c != '"') ? c : '.';
            }
            return out;
        }

        template <int I>
        void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Hook &h = g_hooks[I];
            const uint32_t self = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
            if (g_text && ctx->pc == h.address)
            {
                switch (h.kind)
                {
                case kBegin:
                {
                    const uint32_t command = a1, mode = word(rdram, self + 4u);
                    std::fprintf(g_text, "B %llu %08x %u %u %s %08x\n",
                                 static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), self, mode,
                                 command, command < std::size(kNames) ? kNames[command] : "?", getRegU32(ctx, 31) - 8u);
                    static uint32_t begun = 0u;
                    if ((++begun % 2000u) == 0u)
                        std::fflush(g_text); // (the quit tick ends the process without exit handlers)
                    break;
                }
                case kArg:
                {
                    const std::string t = h.type;
                    if (t == "str")
                        std::fprintf(g_text, "< %08x str \"%s\"\n", self, guestString(rdram, a1).c_str());
                    else if (t == "us")
                    {
                        const uint8_t *p = getConstMemPtr(rdram, a1);
                        std::fprintf(g_text, "< %08x us %u\n", self, p ? static_cast<unsigned>(p[0] | (p[1] << 8)) : 0u);
                    }
                    else if (t == "b")
                    {
                        const uint8_t *p = getConstMemPtr(rdram, a1);
                        std::fprintf(g_text, "< %08x b %u\n", self, p ? *p : 0u);
                    }
                    else if (t == "f")
                    {
                        const uint32_t v = word(rdram, a1);
                        float f;
                        std::memcpy(&f, &v, 4);
                        std::fprintf(g_text, "< %08x f %g\n", self, f);
                    }
                    else
                        std::fprintf(g_text, "< %08x %s %u\n", self, h.type, word(rdram, a1));
                    break;
                }
                case kResult:
                {
                    // The value is in the results buffer at the cursor (transport +0x20), aligned
                    // to its own size; a string is a ushort length then the characters.
                    const std::string t = h.type;
                    uint32_t cursor = word(rdram, self + 0x20u);
                    const uint32_t size = (t == "us" || t == "str") ? 2u : 4u;
                    cursor = (cursor + size - 1u) & ~(size - 1u);
                    if (t == "us" || t == "str")
                    {
                        const uint8_t *q = getConstMemPtr(rdram, cursor);
                        const unsigned v = q ? static_cast<unsigned>(q[0] | (q[1] << 8)) : 0u;
                        if (t == "us")
                            std::fprintf(g_text, "> %08x us %u\n", self, v);
                        else
                        {
                            std::string text;
                            for (unsigned i = 0u; i < v && i < 200u; ++i)
                            {
                                const uint8_t *c = getConstMemPtr(rdram, cursor + 2u + i);
                                text += (c && *c >= 32 && *c < 127 && *c != '"') ? static_cast<char>(*c) : '.';
                            }
                            std::fprintf(g_text, "> %08x str \"%s\"\n", self, text.c_str());
                        }
                    }
                    else
                        std::fprintf(g_text, "> %08x %s %u\n", self, h.type, word(rdram, cursor));
                    break;
                }
                case kRequire:
                    std::fprintf(g_text, "R %08x %s\n", self, h.type);
                    break;
                case kFrame:
                    g_device = self;
                    std::fprintf(g_text, "F %llu %08x\n", static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), self);
                    break;
                case kStatus:
                {
                    // device +0x70 the EE copy, +0x6C its size (vfunc_08's memory request)
                    const uint32_t buffer = word(rdram, self + 0x70u), size = std::min(word(rdram, self + 0x6Cu), 0x1000u);
                    std::fprintf(g_text, "S %llu %08x %u ", static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), buffer, size);
                    for (uint32_t i = 0u; i < size; i += 4u)
                        std::fprintf(g_text, "%08x", word(rdram, buffer + i));
                    std::fputc('\n', g_text);
                    break;
                }
                case kMemory:
                {
                    static uint32_t logged = 0u;
                    if (logged++ < 50u)
                        std::fprintf(g_text, "M %08x %08x %u\n", a1 ? getRegU32(ctx, 4) : 0u, a1, getRegU32(ctx, 6));
                    break;
                }
                case kWriter:
                    if (g_device)
                    {
                        const uint32_t start = word(rdram, g_device + 0x54u), cursor = word(rdram, g_device + 0x58u);
                        std::fprintf(g_text, "W %08x %08x %d %s\n", h.address, self, static_cast<int>(cursor - start), h.type);
                    }
                    break;
                }
            }
            h.original(rdram, ctx, runtime);
        }

        template <int... I>
        void installAll(PS2Runtime &runtime, std::integer_sequence<int, I...>)
        {
            const PS2Runtime::RecompiledFunction fns[] = {&hook<I>...};
            for (size_t i = 0; i < sizeof...(I); ++i)
                if ((g_hooks[i].original = runtime.lookupFunction(g_hooks[i].address)) != nullptr)
                    runtime.registerFunction(g_hooks[i].address, fns[i]);
                else
                    std::fprintf(stderr, "[sirens] no function at %08x\n", g_hooks[i].address);
        }

        void install(PS2Runtime &runtime)
        {
            const char *path = std::getenv("TF_SIRENS_SCHEMA");
            if (!path || !*path || !(g_text = std::fopen(path, "w")))
                return;
            std::setvbuf(g_text, nullptr, _IOFBF, 1 << 20);
            g_runtime = &runtime;
            installAll(runtime, std::make_integer_sequence<int, static_cast<int>(std::size(g_hooks))>{});
            std::fprintf(stderr, "[sirens] recording the command schema to %s\n", path);
        }
    }

    void install(PS2Runtime &runtime)
    {
        schema::install(runtime);
        const char *path = std::getenv("TF_SIRENS_LOG");
        if (!path || !*path)
            return;
        g_bind = runtime.lookupFunction(kBindRpc);
        g_call = runtime.lookupFunction(kCallRpc);
        if (!g_bind || !g_call || !(g_out = std::fopen(path, "wb")))
        {
            std::fprintf(stderr, "[sirens] TF_SIRENS_LOG: not installed\n");
            return;
        }
        runtime.registerFunction(kBindRpc, bindHook);
        runtime.registerFunction(kCallRpc, callHook);
        std::atexit(summary);
        std::fprintf(stderr, "[sirens] recording SIF RPC calls to %s\n", path);
    }
}
