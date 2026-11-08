// The rain drawn by the native renderer (TODO 146, 2026-11-03: Mid-Atlantic after the boss had no rain).
//
// taRain (decomp notes, section_device_fmv_fx.md) draws one prebuilt "rain patch" per visible leaf of its oct-tree:
// taShader_Rain::RenderRainPatches (0x00565480) calls the patch's microcode client (func_221B80: a DMA CALL into the
// display list) with the routine table of one routine, the rain image (0x00602240..0x006028D0, uploaded into microcode
// memory: image e1472a8c96f62e62's 0x3950, table "3950/q35"). Not a geometry set, so the capture made no draw of it.
//
// What the routine does (read off captured batches with tools/vu1_trace.py; a Python model of it matches every word of
// the packets VU1 sent but for Z's last bits, VU1 rounding toward zero):
// - the block (q35 in the table): q0-q2 and q3-q5 two GIF headers (A+D: SCISSOR_1, SCANMSK - field B's and field A's,
//   as the game's field set-up writes them at 0x0061AB50), q6-q9 and q10-q13 two copies of the patch's 4x4 transform,
//   q14 (patches in this batch, the alpha's scale, its cap), q15 (colour; alpha numerator), q16.. the patches' two
//   positions each (A, B: where the patch is and where it was - the drops' fall over the frame);
// - the batch at ITOP: the drops' count (14), then per drop two qwords, the second its offset in the patch;
// - per patch one packet: for each copy its header, a line-strip tag (PRIM 0x5A from q3FF, NLOOP 2 x drops; the second
//   copy's with EOP) and per drop two vertices - copy 1 from the middle C = (A + B) / 2 to A, copy 2 from B to C, each
//   at P = M (point) + M (offset) - RGBA (q15's colour; alpha min(q15.w q14.y / w, q14.z) at the first end, 0 at the
//   second), XYZF2 (q11-q14's projection of P over its w, in 12.4): the first end always with ADC (w 0xFFFF8000: the
//   strip moved on, nothing drawn), the second with ADC too when either end is outside P's clip volume, else 0x7FFF.
//   (The texture is the device's null one, TEX0 at TBP 0x3FFE: white with alpha 0x7F in GS memory - grey 152, alpha
//   ~23/128 drops, a faint lightening of the sky.)
// Built here from the list tracker's VU1 memory (nr_shader.cpp) and drawn through the list's GIF reader at its place, as
// the particle pipelines' packets are (nr_particles.cpp) - the same scissor and blend the GS took.
// TF_NATIVE_RAIN=0: not drawn (as before).

#include "nr_rain.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace nr
{
    namespace rain
    {
        namespace
        {
            constexpr uint32_t kRainImage = 0x00602240u;
            constexpr uint32_t kHeadBytes = 64u;
            uint8_t g_head[kHeadBytes] = {};
            bool g_on = false, g_headRead = false, g_headOk = false;
            const uint8_t *g_rdram = nullptr;

            // (Read at the first batch, as the particles' heads are: the ELF is in guest memory by then.)
            bool head()
            {
                if (!g_headRead)
                {
                    g_headRead = true;
                    const uint8_t *p = g_rdram ? getConstMemPtr(g_rdram, kRainImage) : nullptr;
                    const uint8_t *q = g_rdram ? getConstMemPtr(g_rdram, kRainImage + kHeadBytes - 1u) : nullptr;
                    if (p && q == p + kHeadBytes - 1u)
                    {
                        std::memcpy(g_head, p, kHeadBytes);
                        g_headOk = true;
                    }
                }
                return g_headOk;
            }

            float f(const uint32_t (*vu)[4], uint32_t q, int lane)
            {
                float v;
                std::memcpy(&v, &vu[q & 0x3FFu][lane], 4);
                return v;
            }

            // FTOI0 / FTOI4: truncated, saturated to the int32 range.
            uint32_t ftoi(float x, float scale)
            {
                const double v = std::trunc(static_cast<double>(x) * scale);
                if (!(v == v))
                    return 0u;
                if (v >= 2147483647.0)
                    return 0x7FFFFFFFu;
                if (v <= -2147483648.0)
                    return 0x80000000u;
                return static_cast<uint32_t>(static_cast<int32_t>(v));
            }

            struct Vec
            {
                float v[4];
            };

            Vec transform(const Vec m[4], const float p[4])
            {
                Vec r{};
                for (int l = 0; l < 4; ++l)
                    r.v[l] = m[0].v[l] * p[0] + m[1].v[l] * p[1] + m[2].v[l] * p[2] + m[3].v[l] * p[3];
                return r;
            }

            bool outside(const Vec &p)
            {
                const float w = std::fabs(p.v[3]);
                return std::fabs(p.v[0]) > w || std::fabs(p.v[1]) > w || std::fabs(p.v[2]) > w;
            }

            void put(std::vector<uint8_t> &out, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
            {
                const uint32_t q[4] = {a, b, c, d};
                const size_t at = out.size();
                out.resize(at + 16u);
                std::memcpy(out.data() + at, q, 16u);
            }
        }

        void install(const uint8_t *rdram)
        {
            const char *env = std::getenv("TF_NATIVE_RAIN");
            g_on = !(env && *env == '0');
            g_rdram = rdram;
        }

        bool isRainBatch(const uint32_t (*vu)[4], const uint8_t *micro)
        {
            if (!g_on || !micro || vu[4][0] != 1u || !head())
                return false;
            const uint32_t address = (vu[5][0] & 0x7FFu) * 8u;
            return address + kHeadBytes <= 0x4000u && std::memcmp(micro + address, g_head, kHeadBytes) == 0;
        }

        uint64_t g_tick = 0u;
        void noteTick(uint64_t tick) { g_tick = tick; }

        bool enabled() { return g_on; }

        // (The frame's copies of the passes' blocks; the pass now: -1 before the first rain MSCAL.)
        const std::vector<std::vector<uint32_t>> *g_blocks = nullptr;
        int g_pass = -1;
        void beginFrame(const std::vector<std::vector<uint32_t>> *blocks)
        {
            g_blocks = blocks;
            g_pass = -1;
        }
        void nextPass()
        {
            ++g_pass;
            static const uint64_t logTick = std::getenv("TF_NATIVE_RAINLOG") ? std::strtoull(std::getenv("TF_NATIVE_RAINLOG"), nullptr, 10) : 0u;
            if (logTick && g_tick == logTick)
                std::fprintf(stderr, "[native render] rain pass %d of the frame's %zu copied blocks\n", g_pass, g_blocks ? g_blocks->size() : size_t(0));
        }

        void build(const uint32_t (*vu)[4], uint32_t itop, std::vector<uint8_t> &out, uint32_t start)
        {
            const uint32_t blk = vu[5][1] & 0x3FFu;
            // (The block's rows: the pass's copy where the capture made one (TODO 146), else the tracker's memory.)
            const std::vector<uint32_t> *copy = g_blocks && g_pass >= 0 && static_cast<size_t>(g_pass) < g_blocks->size() ? &(*g_blocks)[g_pass] : nullptr;
            auto row = [&](uint32_t i) -> const uint32_t * {
                if (copy && 4u * i + 4u <= copy->size())
                    return copy->data() + 4u * i;
                return vu[(blk + i) & 0x3FFu];
            };
            auto rf = [&](uint32_t i, int lane) {
                float v;
                std::memcpy(&v, row(i) + lane, 4);
                return v;
            };
            const uint32_t drops = std::min<uint32_t>(vu[itop & 0x3FFu][0] & 0x7FFFu, 64u);
            const uint32_t patches = std::min<uint32_t>(row(14u)[0], 23u);
            static const uint64_t logTick = std::getenv("TF_NATIVE_RAINLOG") ? std::strtoull(std::getenv("TF_NATIVE_RAINLOG"), nullptr, 10) : 0u;
            if (logTick && g_tick == logTick)
            {
                // (FNV-1a of the block's patches and of the batch's drops)
                auto hash = [&](uint32_t from, uint32_t qwords) {
                    uint64_t h = 1469598103934665603ull;
                    for (uint32_t q = 0; q < qwords; ++q)
                        for (int l = 0; l < 4; ++l)
                            h = (h ^ vu[(from + q) & 0x3FFu][l]) * 1099511628211ull;
                    return h;
                };
                std::fprintf(stderr, "[native render] rain batch (tick %llu): start %s%04x itop %03x block %03x patches %u drops %u, patches %016llx drops %016llx\n",
                             static_cast<unsigned long long>(g_tick), start == ~0u ? "MSCNT " : "", start == ~0u ? 0u : start, itop & 0x3FFu, blk,
                             patches, drops, static_cast<unsigned long long>(hash(blk + 14u, 2u + 2u * patches)),
                             static_cast<unsigned long long>(hash(itop, 1u + 2u * drops)));
            }
            if (drops == 0u || patches == 0u)
                return;
            auto vec = [&](uint32_t q) {
                Vec r;
                for (int l = 0; l < 4; ++l)
                    r.v[l] = f(vu, q, l);
                return r;
            };
            auto bvec = [&](uint32_t i) {
                Vec r;
                for (int l = 0; l < 4; ++l)
                    r.v[l] = rf(i, l);
                return r;
            };
            Vec screen[4], m[2][4];
            for (uint32_t r = 0; r < 4u; ++r)
            {
                screen[r] = vec(0x11u + r);
                m[0][r] = bvec(6u + r);
                m[1][r] = bvec(10u + r);
            }
            const uint32_t *tag = vu[0x3FFu];
            const uint32_t rgb[3] = {ftoi(rf(15u, 0), 1.0f), ftoi(rf(15u, 1), 1.0f), ftoi(rf(15u, 2), 1.0f)};
            const float alphaK = rf(15u, 3) * rf(14u, 1), alphaMax = rf(14u, 2);
            // (TF_NATIVE_RAINEXACT=1: the packets exactly as VU1 sends them - a strip a copy a patch, every drop in it.
            // Otherwise, drawn the same but cheaper: a copy whose header is the pass of one pixel (SCISSOR_1 0-0: copy 2
            // whenever a frame is one field) left out, the drops the GS would not draw (their second end with ADC) left
            // out, and the rest of all the batch's patches as one list of separate lines per copy - one draw, not twelve.)
            static const bool exact = std::getenv("TF_NATIVE_RAINEXACT") && *std::getenv("TF_NATIVE_RAINEXACT") == '1';
            struct Drop
            {
                uint32_t rgba[2][4], xyz[2][4];
            };
            static std::vector<Drop> visible;
            for (int copy = 0; copy < 2; ++copy)
            {
                const uint32_t header = copy == 0 ? 3u : 0u;
                const uint32_t *scissor = row(header + 1u);
                if (!exact && scissor[2] == 0x40u && scissor[0] == 0u && scissor[1] == 0u)
                    continue; // (SCISSOR_1 of one pixel: nothing of this copy reaches the picture)
                const Vec *mm = m[copy];
                visible.clear();
                for (uint32_t k = 0; k < patches; ++k)
                {
                    const Vec a = bvec(16u + 2u * k), b = bvec(17u + 2u * k);
                    Vec c;
                    for (int l = 0; l < 4; ++l)
                        c.v[l] = a.v[l] * 0.5f + b.v[l] * 0.5f;
                    const Vec &s0 = copy == 0 ? c : b, &s1 = copy == 0 ? a : c;
                    const Vec base0 = transform(mm, s0.v), base1 = transform(mm, s1.v);
                    if (exact)
                    {
                        for (uint32_t r = 0; r < 3u; ++r)
                        {
                            const uint32_t *h = row(header + r);
                            put(out, h[0], h[1], h[2], h[3]);
                        }
                        put(out, (2u * drops) | (copy == 1 ? 0xFFFF8000u : 0u), tag[1], tag[2], 0u);
                    }
                    for (uint32_t i = 0; i < drops; ++i)
                    {
                        const uint32_t q = itop + 2u + 2u * i;
                        const float o[3] = {f(vu, q, 0), f(vu, q, 1), f(vu, q, 2)};
                        Vec p[2] = {base0, base1};
                        for (int e = 0; e < 2; ++e)
                            for (int l = 0; l < 4; ++l)
                                p[e].v[l] += mm[0].v[l] * o[0] + mm[1].v[l] * o[1] + mm[2].v[l] * o[2];
                        // (The strip's first end always with ADC - vi02, 0x8000 from the routine's start: it only moves
                        // the strip on - the second end's with it when either end is outside: vi01 = FCAND + 0x7FFF.)
                        const bool drawn = !(outside(p[0]) || outside(p[1]));
                        if (!exact && !drawn)
                            continue;
                        Drop d{};
                        for (int e = 0; e < 2; ++e)
                        {
                            const Vec sp = transform(screen, p[e].v);
                            const float inv = 1.0f / sp.v[3];
                            const uint32_t alpha = e == 0 ? ftoi(std::min(alphaK * inv, alphaMax), 1.0f) : 0u;
                            const uint32_t w = e == 0 || !drawn ? 0xFFFF8000u : 0x7FFFu;
                            const uint32_t rgba[4] = {rgb[0], rgb[1], rgb[2], alpha};
                            const uint32_t xyz[4] = {ftoi(sp.v[0] * inv, 16.0f), ftoi(sp.v[1] * inv, 16.0f), ftoi(sp.v[2] * inv, 16.0f), w};
                            std::memcpy(d.rgba[e], rgba, sizeof(rgba));
                            std::memcpy(d.xyz[e], xyz, sizeof(xyz));
                        }
                        if (exact)
                            for (int e = 0; e < 2; ++e)
                            {
                                put(out, d.rgba[e][0], d.rgba[e][1], d.rgba[e][2], d.rgba[e][3]);
                                put(out, d.xyz[e][0], d.xyz[e][1], d.xyz[e][2], d.xyz[e][3]);
                            }
                        else
                            visible.push_back(d);
                    }
                }
                if (exact || visible.empty())
                    continue;
                for (uint32_t r = 0; r < 3u; ++r)
                {
                    const uint32_t *h = row(header + r);
                    put(out, h[0], h[1], h[2], h[3]);
                }
                // (The tag's PRIM a line list: PRIM type 1 in place of the strip's 2 - bits 15-17 of the tag's second word.)
                const uint32_t lines = (tag[1] & ~(7u << 15)) | (1u << 15);
                for (size_t from = 0; from < visible.size(); from += 0x3FFFu)
                {
                    const size_t n = std::min<size_t>(0x3FFFu, visible.size() - from);
                    put(out, static_cast<uint32_t>(2u * n), lines, tag[2], 0u);
                    for (size_t j = from; j < from + n; ++j)
                        for (int e = 0; e < 2; ++e)
                        {
                            const Drop &d = visible[j];
                            put(out, d.rgba[e][0], d.rgba[e][1], d.rgba[e][2], d.rgba[e][3]);
                            put(out, d.xyz[e][0], d.xyz[e][1], d.xyz[e][2], 0x7FFFu); // (both ends drawn: no ADC)
                        }
                }
            }
        }
    }
}
