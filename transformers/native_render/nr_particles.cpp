// The particle pipelines drawn (TODO 144: no smoke anywhere - the crashed plane's column in Antarctica, the dropships hit,
// the missile Mini-Cons' trails).
//
// The game's particle effects (vlxParticleManager, decomp notes section 6) are not geometry sets and not microcode
// primitives: each of its 19 pipelines ("vlxPPP_*": cube / point / spiral / spline emitters with simple-graph or table
// colours, drawn as sprites, quads, lines or ribbons) is a VU1 microcode image of its own, uploaded into microcode
// memory when the render list (taComponent_ParticlePipeline, 0x3F8790) switches to it (vlMicroManager::Upload, an MPG)
// and started with MSCAL at that address; then the particles go in batches of UNPACKs + MSCNT, and the image itself
// expands every particle into its primitives (XGKICK). Nothing on the EE side computes the corners, so the capture saw
// nothing to draw: the emulated GS drew them from VU1's packets, the native renderer never.
//
// Here the renderer's thread runs those programs itself, on the VU1 memory its list tracker keeps (nr_shader.cpp: the
// display list's UNPACKs and MPGs applied as VIF1 applies them), with PS2Recomp's own VU1 engine (ps2vu1fast, the one
// VIF1's thread runs - its XGKICK packets captured instead of sent), and the packets go through the list's GIF reader
// (nr_gif.cpp) at their place in the list, as the GS would have taken them: GS-space primitives with their Z, drawn into
// the scene buffer with the state the list set (the texture, the blend, the depth test). The program is told apart by its
// first 64 bytes: the 18 images in the ELF (0x005E3BC0..0x005F9F50; Ribbon and RibbonTrail share one) read from guest
// memory. VU1's own run of them goes on as before (its packets reach no renderer while the native one draws alone).
//
// TF_NATIVE_PARTICLEFX=0: not drawn (as before). TF_NATIVE_PARTICLELOG=<n>: the first n program runs logged (start,
// image, packets).
#include "nr_particles.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <array>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

class GS;
class PS2Memory;

// PS2Recomp's fast VU1 engine (ps2xRuntime/src/lib/vu/ps2_vu1_fast.h; not on the include path).
namespace ps2vu1fast
{
    void run(const void *owner, VU1State &state, uint64_t &cycle, bool &stopRequested, const uint8_t *code, uint32_t codeSize,
             uint8_t *data, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t maxCycles, std::vector<uint8_t> *capture);
    void resetPipelines(const void *owner, const VU1State &state);
}

namespace nr
{
    namespace particles
    {
        namespace
        {
            // The pipelines' microcode images in the ELF (vlxParticleManager::BindParticlePipelines' list).
            constexpr uint32_t kImages[] = {0x005E3BC0u, 0x005E4CD0u, 0x005E5E20u, 0x005E7300u, 0x005E7F30u, 0x005E9630u,
                                            0x005EA6C0u, 0x005EBAF0u, 0x005ECAE0u, 0x005EDD10u, 0x005EF010u, 0x005F0070u,
                                            0x005F1310u, 0x005F23D0u, 0x005F33A0u, 0x005F5DF0u, 0x005F7470u, 0x005F91C0u};
            constexpr uint32_t kHeadBytes = 64u;
            constexpr size_t kImageCount = sizeof(kImages) / sizeof(kImages[0]);

            GS *g_gs = nullptr;
            const uint8_t *g_rdram = nullptr;
            bool g_on = false;
            int g_log = 0;
            uint64_t g_logFrom = 0u, g_tick = 0u; // (TF_NATIVE_PARTICLELOG=<n>[,<from tick>])

            std::array<std::array<uint8_t, kHeadBytes>, kImageCount> g_heads{};
            bool g_headsRead = false, g_headsOk = false;

            // The renderer's thread's VU1 (one: the list is replayed on that thread alone).
            VU1State g_state{};
            uint64_t g_cycle = 0u;
            bool g_stop = false;
            bool g_stateReady = false;
            const int g_owner = 0; // (the engine's key)

            void readHeads()
            {
                g_headsRead = true;
                if (!g_rdram)
                    return;
                for (size_t i = 0; i < kImageCount; ++i)
                {
                    const uint8_t *p = getConstMemPtr(g_rdram, kImages[i]);
                    const uint8_t *q = getConstMemPtr(g_rdram, kImages[i] + kHeadBytes - 1u);
                    if (!p || q != p + kHeadBytes - 1u)
                        return;
                    std::memcpy(g_heads[i].data(), p, kHeadBytes);
                }
                g_headsOk = true;
            }

            // The pipelines' field headers (func_002516C0, vlxParticleManager's base constants: the render list's VIF packet
            // for VU data D_006CD880 + 4): six qwords, an A+D header for each of the two copies every particle is drawn as
            // (field A's and field B's, D_0061AB50 and D_0061AB80, which the game's field set-up rewrites: SCISSOR_1 and
            // SCANMSK). Without betwixtMode (D_006CD8DC: frames of two fields) the game copies the FIRST header twice - and in
            // that mode the first is the pass of one pixel (the views' "other pass" of a frame of one field, SCISSOR 0-0) - so
            // both copies of every particle went into one pixel and no pipeline was ever seen: the smoke, the sparks, the
            // ribbons. The PC port runs its levels without betwixtMode whenever there are no in-between pictures (the native
            // renderer's frames, a field a frame), so with the native renderer that was every level (TODO 144). Here the
            // second copy takes the second header in either mode, as betwixtMode's packet does: one copy into the pass of
            // one pixel, the other into the field's full screen. Written as the game's function writes it (the VIF words, the
            // rows from the tables as they stand), not called: it cannot stop part-way at a VBlank's checkpoint either.
            constexpr uint32_t kUploadFieldHeaders = 0x002516C0u;
            constexpr uint32_t kPacketPointer = 0x70000D30u; // D_70000D30: the display list's write pointer
            constexpr uint32_t kHeaderAddress = 0x006CD880u;  // D_006CD880: the VU address the pipelines' constants go to
            constexpr uint32_t kBetwixtMode = 0x006CD8DCu;
            constexpr uint32_t kHeaderA = 0x0061AB50u, kHeaderB = 0x0061AB80u;

            void uploadFieldHeaders(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                (void)runtime;
                auto readWord = [&](uint32_t a) {
                    uint32_t v = 0u;
                    if (const uint8_t *p = getConstMemPtr(rdram, a))
                        std::memcpy(&v, p, 4);
                    return v;
                };
                auto writeWord = [&](uint32_t a, uint32_t v) {
                    if (uint8_t *p = getMemPtr(rdram, a))
                        std::memcpy(p, &v, 4);
                };
                const uint32_t p = readWord(kPacketPointer);
                const uint32_t words[8] = {0x10000007u, 0u, 0x01000101u, 0x11000000u, 0x20000000u, 0u, 0u,
                                           (readWord(kHeaderAddress) + 4u) | 0x6C060000u};
                for (uint32_t i = 0; i < 8u; ++i)
                    writeWord(p + 4u * i, words[i]);
                const bool betwixt = (getConstMemPtr(rdram, kBetwixtMode) ? *getConstMemPtr(rdram, kBetwixtMode) : 0u) != 0u;
                static int told = 1;
                if (told > 0 && !betwixt)
                {
                    --told;
                    std::fprintf(stderr, "[native render] particle field headers: frames of one field - the second copy takes the second header (TODO 144)\n");
                }
                for (uint32_t row = 0; row < 6u; ++row)
                {
                    const uint32_t from = (row < 3u ? kHeaderA : kHeaderB) + 16u * (row % 3u);
                    for (uint32_t w = 0; w < 4u; ++w)
                        writeWord(p + 32u + 16u * row + 4u * w, readWord(from + 4u * w));
                }
                writeWord(kPacketPointer, p + 32u + 96u);
                ctx->pc = getRegU32(ctx, 31);
            }

            void resetState()
            {
                std::memset(&g_state, 0, sizeof(g_state));
                g_state.vf[0][3] = 1.0f; // (vf00 = 0, 0, 0, 1)
                g_stateReady = true;
            }
        }

        void install(PS2Runtime &runtime, const uint8_t *rdram)
        {
            const char *on = std::getenv("TF_NATIVE_PARTICLEFX");
            g_on = !(on && *on == '0');
            if (const char *log = std::getenv("TF_NATIVE_PARTICLELOG"))
            {
                g_log = std::atoi(log);
                if (const char *comma = std::strchr(log, ','))
                    g_logFrom = std::strtoull(comma + 1, nullptr, 10);
            }
            g_gs = &runtime.gs();
            g_rdram = rdram;
            // The field headers' fix (game-side: both renderers; TF_PARTICLE_FIELDS=0: the game's own copy).
            const char *fields = std::getenv("TF_PARTICLE_FIELDS");
            if (!(fields && *fields == '0') && runtime.lookupFunction(kUploadFieldHeaders))
                runtime.registerFunction(kUploadFieldHeaders, uploadFieldHeaders);
        }

        bool enabled() { return g_on && g_gs; }

        void noteTick(uint64_t tick) { g_tick = tick; }

        bool isParticleProgram(const uint8_t *micro, uint32_t start)
        {
            if (!g_on || !micro || start + kHeadBytes > 0x4000u)
                return false;
            // (once, whichever thread asks first: the renderer's replay and - emurest step 2 - VU1's own thread, vu1_native.cpp)
            static std::once_flag once;
            std::call_once(once, [] { if (!g_headsRead) readHeads(); });
            if (!g_headsOk)
                return false;
            for (size_t i = 0; i < kImageCount; ++i)
                if (std::memcmp(micro + start, g_heads[i].data(), kHeadBytes) == 0)
                    return true;
            return false;
        }

        void run(uint8_t *data, const uint8_t *micro, uint32_t start, uint32_t top, uint32_t itop, std::vector<uint8_t> &packets)
        {
            if (!enabled())
                return;
            if (!g_stateReady)
                resetState();
            if (start != ~0u)
            {
                g_state.pc = start & 0x3FF8u;
                ps2vu1fast::resetPipelines(&g_owner, g_state);
            }
            g_state.top = top & 0x3FFu;
            g_state.itop = itop & 0x3FFu;
            g_state.ebit = false;
            g_stop = false;
            const size_t before = packets.size();
            const uint32_t from = g_state.pc;
            // (TF_NATIVE_PARTICLEDUMP: VU1 data memory as the run starts, beside its packets)
            static std::vector<uint8_t> memBefore;
            if (g_log > 0 && g_tick >= g_logFrom && std::getenv("TF_NATIVE_PARTICLEDUMP"))
                memBefore.assign(data, data + 0x4000u);
            ps2vu1fast::run(&g_owner, g_state, g_cycle, g_stop, micro, 0x4000u, data, 0x4000u, *g_gs, nullptr, 1u << 22, &packets);
            if (g_log > 0 && g_tick >= g_logFrom)
            {
                --g_log;
                std::fprintf(stderr, "[native render] particle program %s at %04x (top %03x itop %03x): ended at %04x, %zu bytes of packets\n",
                             start == ~0u ? "MSCNT" : "MSCAL", from, top & 0x3FFu, itop & 0x3FFu, g_state.pc, packets.size() - before);
                // (TF_NATIVE_PARTICLEDUMP=1: each logged run's packets and its VU1 data memory before, runs/particle_<n>.bin)
                static const bool dump = std::getenv("TF_NATIVE_PARTICLEDUMP") != nullptr;
                static int dumped = 0;
                if (dump && packets.size() > before)
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "runs/particle_%03d.bin", dumped++);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(packets.data() + before, 1, packets.size() - before, f);
                        std::fclose(f);
                    }
                    std::snprintf(name, sizeof(name), "runs/particle_%03d_vu.bin", dumped - 1);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        // (the data memory before, then the registers as the run left them)
                        std::fwrite(memBefore.data(), 1, memBefore.size(), f);
                        std::fwrite(&g_state, 1, sizeof(g_state), f);
                        std::fclose(f);
                    }
                }
            }
        }
    }
}
