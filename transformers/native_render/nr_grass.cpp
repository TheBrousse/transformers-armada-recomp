// Grass (README.md): the blades the grass shader grows on the ground's second draw, generated here
// from the ground triangles and the shader's data block - our own code, from a reading of the
// VU1 routine: per corner a number of packets of
// eight blades from the corner's density (its alpha's low 7 bits), the triangle's area and the
// corner's depth, the leftover fraction as a packet of shorter blades, the triangle in four where a
// corner wants more than eight; a blade is a screen-aligned sprite from its base to its tip, which is
// the base plus the blade's offset in projective space, at a point from the block's fixed table, one
// quadrant of the grass texture each. One progressive view: field A's matrix, no scan mask.
//
// Checked against the microcode itself (batches of 0x1380 saved with PS2X_VU1_CAPTURE, run through
// this file): the same packets and the same blades drawn, blade for blade. What that took: w as the
// matrix gives it (positive in front of the eye; turning a sign mixed points behind the eye into the
// view), the frustum test on x, y and z (z: the near plane), the depth thinning on the signed w, the
// base on the quadrant's corner and the tip on the texture's centre.
//
// For now on the CPU, into a screen-space mesh; a compute pass is the next form (the counts per
// corner, a prefix sum, a vertex shader making the quads).
#include "nr_types.h"

#include "nr_install.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace nr
{
    namespace
    {
        // N8 (TODO 139): the grass's reach times this (TF_GRASS_DISTANCE; overrides.cpp sets it): the blades thin out with
        // depth past q23.x at q23.w a unit (n / (1 + q23.w (w - q23.x))), and the thinning's rate over the factor puts the
        // same density that many times as far. At 1 the routine's own, bit for bit.
        std::atomic<float> g_grassReach{1.0f};

        float thinning(const float *q23) { return q23[3] / g_grassReach.load(std::memory_order_relaxed); }

        struct Corner
        {
            float p[4];        // projective (field A's matrix)
            float rgb[3];      // 0..255
            float density;     // 0..1
        };

        struct Grower
        {
            const Qword *block;
            Mesh *out;
        };

        // VU1's CLIP of the frustum test point (q0 * P + q1 * P.w against w): a bit per plane outside.
        uint32_t outside(const Qword *block, const float *p)
        {
            uint32_t bits = 0u;
            const float w = std::fabs(p[3]);
            for (int k = 0; k < 3; ++k) // (x, y and z: CLIPw.xyz - z is the near plane)
            {
                const float t = block[0].v[k] * p[k] + block[1].v[k] * p[3];
                if (t > w)
                    bits |= 1u << (2 * k);
                if (t < -w)
                    bits |= 2u << (2 * k);
            }
            return bits;
        }

        Corner lerp3(const Corner &c, const Corner &a, const Corner &b, float u, float v)
        {
            Corner r;
            for (int k = 0; k < 4; ++k)
                r.p[k] = c.p[k] + u * (a.p[k] - c.p[k]) + v * (b.p[k] - c.p[k]);
            for (int k = 0; k < 3; ++k) // (the three colour channels)
                r.rgb[k] = c.rgb[k] + u * (a.rgb[k] - c.rgb[k]) + v * (b.rgb[k] - c.rgb[k]);
            r.density = c.density;
            return r;
        }

        Corner mid(const Corner &a, const Corner &b)
        {
            Corner r;
            for (int k = 0; k < 4; ++k)
                r.p[k] = 0.5f * (a.p[k] + b.p[k]);
            for (int k = 0; k < 3; ++k) // (the three colour channels)
                r.rgb[k] = 0.5f * (a.rgb[k] + b.rgb[k]);
            r.density = 0.5f * (a.density + b.density);
            return r;
        }

        // The blade texture's quadrants: blade b's sprite from ST[b-1] (blade 0: the centre) to ST[b].
        constexpr float kSt[8][2] = {{0, 0}, {0.5f, 0.5f}, {1, 0}, {0.5f, 0.5f}, {1, 1}, {0.5f, 0.5f}, {0, 1}, {0.5f, 0.5f}};

        void packet(Grower &g, const Corner &c, const Corner &a, const Corner &b, int index, float height)
        {
            const Qword *points = g.block + 24 + 4 * index; // two points a qword: (x, y) and (x + z, w)
            const float dx = g.block[2].v[0];
            for (int k = 0; k < 8; ++k)
            {
                const float *q = points[k >> 1].v;
                const float u = (k & 1) ? q[0] + q[2] : q[0], v = (k & 1) ? q[3] : q[1];
                const Corner base = lerp3(c, a, b, u, v);
                float tip[4] = {base.p[0] + dx, base.p[1] + height, base.p[2], base.p[3]};
                const bool culled = std::fabs(base.p[3]) < 1e-6f || std::fabs(tip[3]) < 1e-6f || (outside(g.block, base.p) & outside(g.block, tip)) != 0u;
                if (culled)
                    continue;
                const float x1 = base.p[0] / base.p[3], y1 = base.p[1] / base.p[3];
                const float x2 = tip[0] / tip[3], y2 = tip[1] / tip[3], z = tip[2] / tip[3];
                // The base takes the quadrant's corner, the tip the texture's centre (the microcode
                // draws even blades tip first, odd ones base first, the ST running on in turn).
                const float *s1 = kSt[k & 6];
                const float s2[2] = {0.5f, 0.5f};
                Vertex v1;
                v1.colour = static_cast<uint32_t>(std::clamp(base.rgb[0], 0.0f, 255.0f)) |
                            static_cast<uint32_t>(std::clamp(base.rgb[1], 0.0f, 255.0f)) << 8 |
                            static_cast<uint32_t>(std::clamp(base.rgb[2], 0.0f, 255.0f)) << 16 | 0x7Fu << 24;
                // z / w as the ground's own draw has it (nr_d3d.cpp: z * 16 / w for the meshes), so the
                // blades test against the ground they stand on. (The GS's Z is z / w - XYZF2 - for both.)
                v1.z = z;
                Vertex corners[4] = {v1, v1, v1, v1};
                const float xs[2] = {x1, x2}, ys[2] = {y1, y2};
                const float us[2] = {s1[0], s2[0]}, vs[2] = {s1[1], s2[1]};
                for (int i = 0; i < 4; ++i)
                {
                    corners[i].x = xs[i & 1];
                    corners[i].y = ys[i >> 1];
                    corners[i].u = us[i & 1];
                    corners[i].v = vs[i >> 1];
                }
                const uint32_t first = static_cast<uint32_t>(g.out->vertices.size());
                for (const Vertex &cv : corners)
                    g.out->vertices.push_back(cv);
                for (uint32_t i : {0u, 1u, 2u, 2u, 1u, 3u})
                    g.out->indices.push_back(first + i);
            }
        }

        void tufts(Grower &g, const Corner t[3], float area2, float sub, int depth)
        {
            const float *q23 = g.block[23].v;
            if ((outside(g.block, t[0].p) & outside(g.block, t[1].p) & outside(g.block, t[2].p)) != 0u)
                return;
            bool split = false;
            for (int k = 0; k < 3; ++k) // (each of the triangle's three corners)
            {
                // The corner order of the routine: C = the third first, then the others in turn.
                const Corner &c = t[(k + 2) % 3], &a = t[k % 3], &b = t[(k + 1) % 3];
                float n = std::trunc(q23[2] * c.density * area2) / (1.0f + thinning(q23) * std::max(c.p[3] - q23[0], 0.0f));
                n -= sub;
                const float capped = std::min(n, 8.0f);
                const int packets = static_cast<int>(std::trunc(std::max(capped, 0.0f)));
                const float excess = n - capped;
                const float frac = excess == 0.0f ? n - std::trunc(n) : 0.0f;
                if (excess != 0.0f)
                    split = true;
                const float height = g.block[2].v[1];
                for (int p = 0; p < packets; ++p)
                    packet(g, c, a, b, p, height);
                if (frac > 0.1f && packets < 8)
                    packet(g, c, a, b, packets, height * frac);
            }
            if (split && depth < 4)
            {
                const Corner m01 = mid(t[0], t[1]), m12 = mid(t[1], t[2]), m02 = mid(t[0], t[2]);
                const Corner quarters[4][3] = {{m12, m01, m02}, {t[0], m01, m02}, {m01, t[1], m12}, {m02, m12, t[2]}};
                for (const auto &q : quarters)
                    tufts(g, q, area2 * 0.25f, 2.0f, depth + 1);
            }
        }
    }

    void setGrassReach(float factor) { g_grassReach.store(std::max(1.0f, factor), std::memory_order_relaxed); }

    // (grassgpu) The blades of grassBlades below, grown on the GPU instead (nr_d3d.cpp: the grass compute pass - the same packets
    // and blades in the same order, its divisions in double so that they round as these do): here only what it needs.
    std::shared_ptr<Mesh> grassBladesGpu(const std::shared_ptr<const Mesh> &ground, const Qword *block, uint64_t id, bool check)
    {
        auto out = std::make_shared<Mesh>();
        out->id = id;
        out->immediate = true;
        out->hasColour = true;
        out->hasUv = true;
        out->prim = 0x56u; // SPRITE, TME, ABE
        auto gg = std::make_shared<GpuGrass>();
        gg->ground = ground;
        std::memcpy(gg->block, block, sizeof(gg->block));
        gg->thinning = thinning(block[23].v);
        if (check)
            gg->cpu = grassBlades(*ground, block, id);
        out->gpuGrass = std::move(gg);
        return out;
    }

    std::shared_ptr<Mesh> grassBlades(const Mesh &ground, const Qword *block, uint64_t id)
    {
        auto out = std::make_shared<Mesh>();
        out->id = id;
        out->immediate = true;
        out->hasColour = true;
        out->hasUv = true;
        out->prim = 0x56u; // SPRITE, TME, ABE
        // (FPS round 2) The blades' arrays reserved for as many as this ground (its geometry set) grew the last time:
        // grown a blade at a time they were copied over and over as they filled (~0.5 ms a frame of the render
        // thread in Amazon). The same blades.
        thread_local std::unordered_map<uint32_t, size_t> t_lastBlades;
        // Guest geometry addresses change across level/HQ visits. These are only
        // reserve hints, so forget old hints instead of retaining every address
        // for the worker's lifetime. A hit at the limit keeps the current set.
        if (t_lastBlades.size() >= 512u && t_lastBlades.find(ground.source) == t_lastBlades.end())
            t_lastBlades.clear();
        size_t &lastBlades = t_lastBlades[ground.source];
        out->vertices.reserve(4u * (lastBlades + lastBlades / 8u) + 64u);
        out->indices.reserve(6u * (lastBlades + lastBlades / 8u) + 96u);
        Grower g{block, out.get()};
        const float *m = block[3].v; // q3-q6: field A's matrix (moved half a blade left)
        auto corner = [&](const Vertex &v) {
            Corner c;
            for (int r = 0; r < 4; ++r)
                c.p[r] = m[r] * v.x + m[4 + r] * v.y + m[8 + r] * v.z + m[12 + r];
            c.rgb[0] = static_cast<float>(v.colour & 0xFFu);
            c.rgb[1] = static_cast<float>((v.colour >> 8) & 0xFFu);
            c.rgb[2] = static_cast<float>((v.colour >> 16) & 0xFFu);
            c.density = static_cast<float>((v.colour >> 24) & 0x7Fu) / 127.0f;
            return c;
        };
        static int logs = std::getenv("TF_NATIVE_GRASSLOG") ? std::atoi(std::getenv("TF_NATIVE_GRASSLOG")) : 0;
        float maxDensity = 0.0f, maxArea = 0.0f, maxN = 0.0f, minW = 1e30f, maxW = -1e30f;
        for (size_t i = 0; i + 2 < ground.indices.size(); i += 3)
        {
            const Vertex &v0 = ground.vertices[ground.indices[i]], &v1 = ground.vertices[ground.indices[i + 1]],
                         &v2 = ground.vertices[ground.indices[i + 2]];
            const float ax = v0.x - v2.x, ay = v0.y - v2.y, az = v0.z - v2.z;
            const float bx = v1.x - v2.x, by = v1.y - v2.y, bz = v1.z - v2.z;
            const float cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
            const float area2 = std::sqrt(cx * cx + cy * cy + cz * cz);
            const Corner t[3] = {corner(v0), corner(v1), corner(v2)};
            for (const Corner &c : t)
            {
                maxDensity = std::max(maxDensity, c.density);
                minW = std::min(minW, c.p[3]);
                maxW = std::max(maxW, c.p[3]);
                const float n = std::trunc(block[23].v[2] * c.density * area2) / (1.0f + thinning(block[23].v) * std::max(c.p[3] - block[23].v[0], 0.0f));
                maxN = std::max(maxN, n);
            }
            maxArea = std::max(maxArea, area2);
            tufts(g, t, area2, 0.0f, 0);
        }
        lastBlades = out->vertices.size() / 4u;
        if (logs > 0)
        {
            --logs;
            std::fprintf(stderr, "[native render] grass: %zu triangles, density up to %.2f, area2 up to %.1f, w %.1f to %.1f, n up to %.2f, %zu blades; q2 %g %g q23 %g %g %g %g\n",
                         ground.indices.size() / 3, maxDensity, maxArea, minW, maxW, maxN, out->vertices.size() / 4, block[2].v[0], block[2].v[1],
                         block[23].v[0], block[23].v[1], block[23].v[2], block[23].v[3]);
        }
        return out;
    }

    // The ground mist's drift (vlShader_Drift: the grass image's 0x0480 alone in the routine table -
    // Antarctica's drifting snow streaks; read from the microcode, tools' disassembly of captured
    // batches): the grass's blades, the wind's way. Per triangle of the strip (A the oldest corner, B
    // the next, C the newest) a number of packets of eight streaks n = q23.y * |AB x AC| / (1 + q23.w *
    // max(C.w - q24.w, 0)), at most 8 (the whole ones); where n is 12 or more, the triangle again in four
    // (the midpoints; area a quarter, two packets fewer each) - once. Streak k of a packet at the block's
    // point pair 4p + k/2 (q25 on: (x, y), then (x + z, y + w)): the colour the corners' at that point,
    // its base moved along the wind tangent to the triangle (q24.xyz, through the matrix q3-q5) by t, its
    // tip the base plus q2; t = frac(R - 1 + q23.x), R the VU's random generator, seeded with 1.34582
    // at each triangle (RINIT) and stepped once a streak (RNEXT); alpha (1 - (2t - 1)^2) * q23.z. The
    // sprite from the base (the texture quadrant's corner) to the tip (its centre), as the grass's.
    std::shared_ptr<Mesh> driftStreaks(const Mesh &ground, const Qword *block, uint64_t id)
    {
        auto out = std::make_shared<Mesh>();
        out->id = id;
        out->immediate = true;
        out->hasColour = true;
        out->hasUv = true;
        out->prim = 0x56u; // SPRITE, TME, ABE
        const float *m = block[3].v; // q3-q6: field A's matrix
        const float *q23 = block[23].v, *q24 = block[24].v, *q2 = block[2].v;
        struct P
        {
            float p[4];
            float c[3];
        };
        auto project = [&](const Vertex &v) {
            P r;
            for (int k = 0; k < 4; ++k)
                r.p[k] = m[k] * v.x + m[4 + k] * v.y + m[8 + k] * v.z + m[12 + k];
            r.c[0] = static_cast<float>(v.colour & 0xFFu);
            r.c[1] = static_cast<float>((v.colour >> 8) & 0xFFu);
            r.c[2] = static_cast<float>((v.colour >> 16) & 0xFFu);
            return r;
        };
        auto half = [](const P &a, const P &b) {
            P r;
            for (int k = 0; k < 4; ++k)
                r.p[k] = 0.5f * (a.p[k] + b.p[k]);
            for (int k = 0; k < 3; ++k)
                r.c[k] = 0.5f * (a.c[k] + b.c[k]);
            return r;
        };
        auto fracOf = [](float x) { return x - std::trunc(x); };
        uint32_t r = 0u;
        auto next = [&r]() {
            const uint32_t x = (r >> 4) & 1u, y = (r >> 22) & 1u;
            r = (((r << 1) ^ x ^ y) & 0x007FFFFFu) | 0x3F800000u;
            float f;
            std::memcpy(&f, &r, 4);
            return f;
        };
        float wind[4] = {};
        // One corner pass: (A, B, C) with the area (twice the triangle's) and what is taken off n.
        auto cornerPass = [&](const P &a, const P &b, const P &c, float area2, float sub) {
            const float n = q23[1] * area2 / (1.0f + q23[3] * std::max(c.p[3] - q24[3], 0.0f)) - sub;
            const float capped = std::min(n, 8.0f);
            const int packets = static_cast<int>(std::trunc(capped));
            r = 0x3F800000u | (0x3FAC43D8u & 0x007FFFFFu); // (RINIT with 1.34582)
            for (int p = 0; p < packets; ++p)
                for (int k = 0; k < 8; ++k)
                {
                    const float *pt = block[25 + 4 * p + (k >> 1)].v;
                    const float u = (k & 1) ? pt[0] + pt[2] : pt[0], v = (k & 1) ? pt[1] + pt[3] : pt[1];
                    const float t = fracOf(next() - 1.0f + q23[0]);
                    float base[4], tip[4];
                    for (int i = 0; i < 4; ++i)
                    {
                        base[i] = c.p[i] + u * (a.p[i] - c.p[i]) + v * (b.p[i] - c.p[i]) + wind[i] * t;
                        tip[i] = base[i] + q2[i];
                    }
                    if (std::fabs(base[3]) < 1e-6f || std::fabs(tip[3]) < 1e-6f || (outside(block, base) & outside(block, tip)) != 0u)
                        continue;
                    const float alpha = std::trunc((1.0f - (2.0f * t - 1.0f) * (2.0f * t - 1.0f)) * q23[2]);
                    float col[3];
                    for (int i = 0; i < 3; ++i)
                        col[i] = std::trunc(c.c[i] + u * (a.c[i] - c.c[i]) + v * (b.c[i] - c.c[i]));
                    Vertex v1;
                    v1.colour = static_cast<uint32_t>(std::clamp(col[0], 0.0f, 255.0f)) | static_cast<uint32_t>(std::clamp(col[1], 0.0f, 255.0f)) << 8 |
                                static_cast<uint32_t>(std::clamp(col[2], 0.0f, 255.0f)) << 16 |
                                static_cast<uint32_t>(std::clamp(alpha, 0.0f, 255.0f)) << 24;
                    v1.z = tip[2] / tip[3];
                    const float *s1 = kSt[k & 6];
                    const float xs[2] = {base[0] / base[3], tip[0] / tip[3]}, ys[2] = {base[1] / base[3], tip[1] / tip[3]};
                    const float us[2] = {s1[0], 0.5f}, vs[2] = {s1[1], 0.5f};
                    const uint32_t first = static_cast<uint32_t>(out->vertices.size());
                    for (int i = 0; i < 4; ++i)
                    {
                        Vertex cv = v1;
                        cv.x = xs[i & 1];
                        cv.y = ys[i >> 1];
                        cv.u = us[i & 1];
                        cv.v = vs[i >> 1];
                        out->vertices.push_back(cv);
                    }
                    for (uint32_t i : {0u, 1u, 2u, 2u, 1u, 3u})
                        out->indices.push_back(first + i);
                }
            return n - capped; // (the excess)
        };
        // The routine's call (0x0c28) runs three passes, each corner in turn as C - (A, B, C),
        // (B, C, A), (C, A, B), the count by that corner's depth, the random sequence restarted
        // each pass - and hands back the largest excess (tools/vu1_trace.py with RNEXT/RINIT on
        // captured batches: every packet as the VU sent it).
        auto streaks = [&](const P &a, const P &b, const P &c, float area2, float sub) {
            float excess = cornerPass(a, b, c, area2, sub);
            excess = std::max(excess, cornerPass(b, c, a, area2, sub));
            return std::max(excess, cornerPass(c, a, b, area2, sub));
        };
        for (size_t i = 0; i + 2 < ground.indices.size(); i += 3)
        {
            const Vertex &va = ground.vertices[ground.indices[i]], &vb = ground.vertices[ground.indices[i + 1]],
                         &vc = ground.vertices[ground.indices[i + 2]];
            const P a = project(va), b = project(vb), c = project(vc);
            if ((outside(block, a.p) & outside(block, b.p) & outside(block, c.p)) != 0u)
                continue;
            // |(B - C) x (A - C)| (ELENG), and the wind less its part along the normal, through the
            // matrix's rotation (no translation: a direction).
            const float e1[3] = {vb.x - vc.x, vb.y - vc.y, vb.z - vc.z}, e2[3] = {va.x - vc.x, va.y - vc.y, va.z - vc.z};
            const float nx = e2[1] * e1[2] - e2[2] * e1[1], ny = e2[2] * e1[0] - e2[0] * e1[2], nz = e2[0] * e1[1] - e2[1] * e1[0];
            const float area2 = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (area2 <= 0.0f)
                continue;
            const float d = (q24[0] * nx + q24[1] * ny + q24[2] * nz) / (area2 * area2);
            const float wt[3] = {q24[0] - nx * d, q24[1] - ny * d, q24[2] - nz * d};
            for (int k = 0; k < 4; ++k)
                wind[k] = m[k] * wt[0] + m[4 + k] * wt[1] + m[8 + k] * wt[2];
            const float excess = streaks(a, b, c, area2, 0.0f);
            if (excess >= 4.0f)
            {
                const P ab = half(a, b), ac = half(a, c), bc = half(b, c);
                streaks(bc, ab, ac, area2 * 0.25f, 2.0f);
                streaks(a, ab, ac, area2 * 0.25f, 2.0f);
                streaks(bc, ab, b, area2 * 0.25f, 2.0f);
                streaks(bc, c, ac, area2 * 0.25f, 2.0f);
            }
        }
        return out;
    }
}
