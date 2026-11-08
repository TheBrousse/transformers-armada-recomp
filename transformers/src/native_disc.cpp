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
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

        void sendRequestFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
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
            write32(rdram, request + 0x10u, ok ? kStatusDone : kStatusError);
            SET_GPR_U32(ctx, 2, 1u);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    bool active() { return g_active; }

    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("TF_NATIVE_DISC");
        if (env && *env == '0')
            return;
        g_trace = std::getenv("TF_NATIVE_DISC_TRACE") != nullptr;
        if (!runtime.lookupFunction(kSendRequest))
        {
            std::fprintf(stderr, "[native disc] the TwoFish request function was not found: not installed\n");
            return;
        }
        g_runtime = &runtime;
        runtime.registerFunction(kSendRequest, sendRequestFn);
        g_active = true;
        std::fprintf(stderr, "[native disc] TwoFish's reads are answered on the host (TF_NATIVE_DISC=0: the IOP's)\n");
    }
}
