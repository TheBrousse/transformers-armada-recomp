// The native TwoFish disc server (native_disc.h).
//
// The EE side of TwoFish (docs/NOTES.md, "Boot sequence and the TwoFish file server"):
//   0x192FF0 SendRequest(request): request +0x00 LSN, +0x04 sector count, +0x08 EE buffer, +0x0C
//            the thread to wake (cleared here), +0x10 status (0x13 pending, 0 done, 0xFE a read
//            error, 0x1000 not sent). It needs the server ready (gp-0x4938), waits for its last
//            SIF command, refuses an eighth request in flight (status 0x1000, returns 0), then
//            sends SIF command 0 and returns 1 with the status pending. The reply (SIF command 0
//            from the IOP, handler 0x192F10) sets the status and wakes the thread, if any.
//   0x192FB0 / 0x192FE0 spin on, or poll, the status until it leaves 0x13.
// Replaced here by the read itself: the sectors go into the buffer at once and the status is
// final when the call returns, so both the spinning and the polling callers see a finished
// request. Nothing reaches the IOP's TwoFish (loaded and idle; with the sound server native too,
// the IOP is parked). Decomp equivalent: the disc layer reads the files from the host.

#include "native_disc.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

namespace native_disc
{
    namespace
    {
        constexpr uint32_t kSendRequest = 0x00192FF0u;
        constexpr uint32_t kStatusDone = 0u, kStatusError = 0xFEu;
        constexpr uint32_t kSectorSize = 2048u;
        constexpr uint32_t kMaxSectors = 0x4000u; // 32 MB: a larger request is not a real one

        bool g_active = false;
        bool g_trace = false;
        // TF_FILE_READ_LOG=<ms> (TODO 385 (1), a measuring aid, off by default): every TwoFish request that took this many
        // milliseconds or more is logged with the thread it ran on, and once a second the totals of the requests of that
        // second (count, sectors, ms in all, the slowest) while any came. Nothing else changes: the request is answered as
        // before. 0 or "all" logs every request.
        double g_logOverMs = -1.0;
        struct ReadSecond
        {
            std::chrono::steady_clock::time_point start{};
            uint32_t count = 0u, sectors = 0u, slow = 0u;
            double totalMs = 0.0, maxMs = 0.0;
            uint32_t maxLsn = 0u, maxSectors = 0u;
        } g_second;
        PS2Runtime *g_runtime = nullptr;
        std::vector<uint8_t> g_buffer;

        uint32_t read32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getConstMemPtr(rdram, a))
                std::memcpy(&v, p, 4);
            return v;
        }

        void write32(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (uint8_t *p = getMemPtr(rdram, a))
                std::memcpy(p, &v, 4);
        }

        // The bytes into EE memory, a page at a time (the buffer may be a mirror of main RAM).
        bool copyToEe(uint8_t *rdram, uint32_t dest, const uint8_t *bytes, size_t size)
        {
            constexpr uint32_t kPage = 0x1000u;
            size_t done = 0u;
            while (done < size)
            {
                const uint32_t a = dest + static_cast<uint32_t>(done);
                const size_t chunk = std::min<size_t>(size - done, kPage - (a & (kPage - 1u)));
                uint8_t *p = getMemPtr(rdram, a);
                if (!p)
                    return false;
                std::memcpy(p, bytes + done, chunk);
                done += chunk;
            }
            return true;
        }

        void noteRead(uint32_t lsn, uint32_t sectors, double ms)
        {
            const auto now = std::chrono::steady_clock::now();
            if (g_second.count != 0u && now - g_second.start >= std::chrono::seconds(1))
            {
                std::fprintf(stderr, "[file read] last second: %u requests, %u sectors (%.1f MB), %.2f ms in all, slowest %.2f ms (%u sectors at LSN %u), %u over %.1f ms\n",
                             g_second.count, g_second.sectors, g_second.sectors * 2048.0 / 1048576.0, g_second.totalMs, g_second.maxMs, g_second.maxSectors,
                             g_second.maxLsn, g_second.slow, g_logOverMs);
                g_second = ReadSecond{};
            }
            if (g_second.count == 0u)
                g_second.start = now;
            ++g_second.count;
            g_second.sectors += sectors;
            g_second.totalMs += ms;
            if (ms >= g_logOverMs)
            {
                ++g_second.slow;
                std::fprintf(stderr, "[file read] %.2f ms: %u sectors at LSN %u, thread %zu\n", ms, sectors, lsn,
                             std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000u);
            }
            if (ms > g_second.maxMs)
                g_second.maxMs = ms, g_second.maxLsn = lsn, g_second.maxSectors = sectors;
        }

        void sendRequestFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const auto t0 = g_logOverMs >= 0.0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const uint32_t request = getRegU32(ctx, 4);
            const uint32_t lsn = read32(rdram, request + 0x00u);
            const uint32_t sectors = read32(rdram, request + 0x04u);
            const uint32_t buffer = read32(rdram, request + 0x08u);
            write32(rdram, request + 0x0Cu, 0u);
            bool ok = sectors <= kMaxSectors;
            if (ok && sectors > 0u)
            {
                g_buffer.assign(static_cast<size_t>(sectors) * kSectorSize, 0u);
                ok = g_runtime->readDiscSectors(lsn, sectors, g_buffer.data());
                if (!ok)
                {
                    // A chunk that runs past the last file (TwoFish reads 16 sectors at a time): the
                    // sectors there read as zeros, as a pressed disc's padding does. A request whose
                    // first sector is not on the disc still fails.
                    ok = g_runtime->readDiscSectors(lsn, 1u, g_buffer.data());
                    // (2026-10-28, your playtest - Quit Game in Amazon, a black screen: the game's
                    // fatal error after TwoFish read 16 sectors starting 7 past the virtual disc's
                    // end, LSN 1415869, beyond its last file TFA2.ZIP. The virtual disc ends at the
                    // last file; a pressed one has its padding after it, so a chunk that starts just
                    // past the end reads as zeros too - within kPadding of a sector that is there.)
                    constexpr uint32_t kPadding = 1024u;
                    std::vector<uint8_t> probe(kSectorSize);
                    if (!ok && lsn >= kPadding && g_runtime->readDiscSectors(lsn - kPadding, 1u, probe.data()))
                    {
                        std::memset(g_buffer.data(), 0, kSectorSize);
                        ok = true;
                        std::fprintf(stderr, "[native disc] %u sectors at LSN %u past the disc's end: read as padding (zeros)\n", sectors, lsn);
                    }
                    for (uint32_t i = 1u; ok && i < sectors; ++i)
                        if (!g_runtime->readDiscSectors(lsn + i, 1u, g_buffer.data() + static_cast<size_t>(i) * kSectorSize))
                            std::memset(g_buffer.data() + static_cast<size_t>(i) * kSectorSize, 0, kSectorSize);
                }
                ok = ok && copyToEe(rdram, buffer, g_buffer.data(), g_buffer.size());
            }
            if (!ok)
                std::fprintf(stderr, "[native disc] read of %u sectors at LSN %u into %08x failed\n", sectors, lsn, buffer);
            else if (g_trace)
                std::fprintf(stderr, "[native disc] read %u sectors at LSN %u into %08x\n", sectors, lsn, buffer);
            if (g_logOverMs >= 0.0)
                noteRead(lsn, sectors, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            write32(rdram, request + 0x10u, ok ? kStatusDone : kStatusError);
            SET_GPR_U32(ctx, 2, 1u);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    bool active() { return g_active; }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_NATIVE_DISC"); env && *env == '0')
            std::fprintf(stderr, "[native disc] TF_NATIVE_DISC=0 is ignored: TwoFish's IOP path is gone, the disc is read on the host\n");
        g_trace = std::getenv("TF_NATIVE_DISC_TRACE") != nullptr;
        if (const char *log = std::getenv("TF_FILE_READ_LOG"); log && *log)
        {
            g_logOverMs = std::strcmp(log, "all") == 0 ? 0.0 : std::max(0.0, std::atof(log));
            std::fprintf(stderr, "[file read] logging the disc requests of %.1f ms and more (TF_FILE_READ_LOG)\n", g_logOverMs);
        }
        if (!runtime.lookupFunction(kSendRequest))
        {
            std::fprintf(stderr, "[native disc] the TwoFish request function was not found: not installed\n");
            return;
        }
        g_runtime = &runtime;
        runtime.registerFunction(kSendRequest, sendRequestFn);
        g_active = true;
        std::fprintf(stderr, "[native disc] TwoFish's reads are answered on the host\n");
    }
}
