// A geometry set's own packet, read as data (README.md): the DMA chain its draw calls into
// (vlGeometrySetPS2 +0x14), its VIF commands applied to a VU1 memory image of our own (nr_vif.h),
// and at each microprogram start the batch at ITOP converted as VU1's dispatcher converts it
// (docs/RENDERER.md: the header's x = vertex count, y = type, z = the bias the integer data carries;
// one to three qwords a vertex from ITOP + 1; the drawing-kick masks at ITOP + stride * n + 4). No
// VU1 or GS runs here: this is the conversion a PC build does once, when the set is loaded -
// serialised sets (the levels') have no vertex layout (+0x4C) beside their packet, so the packet
// is the only form there is.
//
// A vertex with its kick bit set starts a strip again (the GS's ADC: a vertex kicked without a
// triangle), so a triangle (v-2, v-1, v) is made only for a vertex whose bit is clear.
#include "nr_vif.h"
#include "nr_types.h"

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nr::vif
{
    namespace
    {
        uint32_t word(const uint8_t *rdram, uint32_t addr)
        {
            const uint8_t *p = getConstMemPtr(rdram, addr);
            uint32_t v = 0u;
            if (p)
                std::memcpy(&v, p, 4);
            return v;
        }

        // One UNPACK's elements into VU memory (vn: components less one, vl: 32/16/8/5-bit).
        size_t unpack(State &s, const uint32_t *w, size_t avail, uint32_t cmd, uint32_t num, uint32_t imm)
        {
            const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u;
            const bool masked = (cmd & 0x10u) != 0u, unsign = (imm & 0x4000u) != 0u;
            uint32_t addr = imm & 0x3FFu;
            if (imm & 0x8000u)
                addr += s.tops;
            const uint32_t n = num ? num : 256u;
            const bool v45 = vn == 3u && vl == 3u;
            const uint32_t bits = v45 ? 16u : (32u >> vl) * (vn + 1u);
            const size_t words = (static_cast<size_t>(n) * bits + 31u) / 32u;
            if (words > avail)
                return avail;
            if (n > 1u)
                s.stride = s.cl >= s.wl ? s.cl : 1u;
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(w);
            const uint32_t compBytes = 4u >> vl;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t slot = s.cl >= s.wl ? (i / s.wl) * s.cl + i % s.wl : i;
                uint32_t *q = s.vu[(addr + slot) & 0x3FFu];
                uint32_t value[4] = {};
                uint32_t comps = vn + 1u;
                if (v45)
                {
                    uint16_t c;
                    std::memcpy(&c, bytes + 2u * i, 2);
                    value[0] = (c & 0x1Fu) << 3;
                    value[1] = ((c >> 5) & 0x1Fu) << 3;
                    value[2] = ((c >> 10) & 0x1Fu) << 3;
                    value[3] = (c >> 15) << 7;
                    comps = 4u;
                }
                else
                    for (uint32_t c = 0; c < comps; ++c)
                    {
                        const uint8_t *p = bytes + (static_cast<size_t>(i) * comps + c) * compBytes;
                        if (vl == 0u)
                            std::memcpy(&value[c], p, 4);
                        else if (vl == 1u)
                        {
                            uint16_t h;
                            std::memcpy(&h, p, 2);
                            value[c] = unsign ? h : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h)));
                        }
                        else
                            value[c] = unsign ? *p : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(*p)));
                    }
                if (vn == 0u && !v45)
                    value[1] = value[2] = value[3] = value[0];
                const uint32_t written = vn == 0u ? 4u : comps;
                const uint32_t cycle = std::min(i % std::max(1u, s.wl), 3u);
                for (uint32_t c = 0; c < written; ++c)
                {
                    const uint32_t m = masked ? (s.mask >> (2u * (4u * cycle + c))) & 3u : 0u;
                    if (m == 3u)
                        continue;
                    if (m == 1u)
                        q[c] = s.row[c];
                    else if (m == 2u)
                        q[c] = s.col[cycle];
                    else if (s.mode == 1u)
                        q[c] = value[c] + s.row[c];
                    else if (s.mode == 2u)
                        q[c] = s.row[c] = value[c] + s.row[c];
                    else
                        q[c] = value[c];
                }
            }
            return words;
        }
    }

    float asFloat(uint32_t w)
    {
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    }

    bool gather(const uint8_t *rdram, uint32_t start, std::vector<uint32_t> &out, FILE *dump, const std::function<bool(uint32_t)> &skip,
                uint32_t stopAt, std::vector<std::pair<uint32_t, uint32_t>> *positions)
    {
        uint32_t tag = start;
        uint32_t stack[4];
        int depth = 0;
        uint32_t lastTag = 0, lastId = 0;
        for (int guard = 0; guard < 8192; ++guard)
        {
            if (stopAt && depth == 0)
            {
                if (tag == stopAt)
                    return true;
                // A CNT whose data runs over where the stretch was to end: the region ends inside it -
                // a set whose call added its shader context (a CNT of the routine table, then one of
                // the data blocks) but not its own call, which the next stretch holds (the carrier's
                // second map: the stretch was dropped and with it the lit metal's routine table - half
                // of Optimus drew with the table before it, 0x1ac0, black). Read whole, to its end.
                if (tag > stopAt && lastId == 1u && lastTag < stopAt)
                    return true;
                if (tag > stopAt && tag - stopAt < (1u << 24)) // (past it: not this region's chain)
                    return false;
            }
            const uint32_t lo = word(rdram, tag), hi = word(rdram, tag + 4u);
            const uint32_t qwc = lo & 0xFFFFu, id = (lo >> 28) & 7u, addr = hi & 0x7FFFFFF0u;
            if (depth == 0)
            {
                lastTag = tag, lastId = id;
                if (positions)
                    positions->push_back({static_cast<uint32_t>(out.size()), tag});
            }
            if (dump)
                std::fprintf(dump, "  dma %08x: id %u qwc %u addr %08x vif %08x %08x\n", tag, id, qwc, addr, word(rdram, tag + 8u),
                             word(rdram, tag + 12u));
            out.push_back(word(rdram, tag + 8u));
            out.push_back(word(rdram, tag + 12u));
            const uint32_t after = tag + 16u;
            const uint32_t data = (id == 0u || id == 3u || id == 4u) ? addr : after;
            // (The data in one copy where it is contiguous in guest memory - main RAM - else a word
            // at a time.)
            const size_t at = out.size();
            out.resize(at + qwc * 4u);
            const uint8_t *first = qwc ? getConstMemPtr(rdram, data) : nullptr;
            const uint8_t *last = qwc ? getConstMemPtr(rdram, data + qwc * 16u - 1u) : nullptr;
            if (first && last == first + qwc * 16u - 1u)
                std::memcpy(out.data() + at, first, qwc * 16u);
            else
                for (uint32_t i = 0; i < qwc * 4u; ++i)
                    out[at + i] = word(rdram, data + 4u * i);
            if (out.size() > (1u << 22))
                return false;
            switch (id)
            {
            case 1u: // cnt
                tag = after + 16u * qwc;
                break;
            case 2u: // next
                tag = addr;
                break;
            case 3u: // ref
            case 4u: // refs
                tag = after;
                break;
            case 5u: // call
                if (skip && skip(addr))
                {
                    tag = after + 16u * qwc;
                    break;
                }
                if (depth >= 4)
                    return false;
                stack[depth++] = after + 16u * qwc;
                tag = addr;
                break;
            case 6u: // ret
                if (depth == 0)
                    return true;
                tag = stack[--depth];
                break;
            default: // refe, end
                return true;
            }
        }
        return false;
    }

    void skim(const uint32_t *words, size_t count, const std::function<void(const uint32_t *, size_t)> &direct)
    {
        for (size_t i = 0; i < count;)
        {
            const uint32_t w = words[i++];
            const uint32_t cmd = (w >> 24) & 0x7Fu, num = (w >> 16) & 0xFFu, imm = w & 0xFFFFu;
            if ((cmd & 0x60u) == 0x60u)
            {
                // (unpack's own length: num elements of (vn + 1) components of 32 >> vl bits, V4-5 16)
                const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, n = num ? num : 256u;
                const uint32_t bits = (vn == 3u && vl == 3u) ? 16u : (32u >> vl) * (vn + 1u);
                i += (static_cast<size_t>(n) * bits + 31u) / 32u;
                continue;
            }
            switch (cmd)
            {
            case 0x20u:
                i += 1u;
                break;
            case 0x30u:
            case 0x31u:
                i += 4u;
                break;
            case 0x4Au:
                i += 2u * (num ? num : 256u);
                break;
            case 0x50u:
            case 0x51u:
            {
                const size_t q = imm ? imm : 65536u;
                if (direct && i + 4u * q <= count)
                    direct(words + i, q);
                i += 4u * q;
                break;
            }
            default:
                break;
            }
        }
    }

    void run(State &s, const uint32_t *words, size_t count, const Kick &kick, FILE *dump)
    {
        for (size_t i = 0; i < count;)
        {
            const uint32_t w = words[i++];
            const uint32_t cmd = (w >> 24) & 0x7Fu, num = (w >> 16) & 0xFFu, imm = w & 0xFFFFu;
            if (dump && (cmd & 0x60u) != 0x60u && cmd != 0u)
                std::fprintf(dump, "  vif %08x\n", w);
            if ((cmd & 0x60u) == 0x60u)
            {
                if (dump)
                {
                    std::fprintf(dump, "  vif %08x unpack %s%u-%u n %u at %u%s (cl %u wl %u mode %u)\n", w, (cmd & 0x10u) ? "masked " : "",
                                 ((cmd >> 2) & 3u) + 1u, 32u >> (cmd & 3u), num ? num : 256u, imm & 0x3FFu, (imm & 0x8000u) ? "+tops" : "",
                                 s.cl, s.wl, s.mode);
                    if ((imm & 0x3FFu) >= 1016u)
                        for (uint32_t k = 0; k < 4u * (num ? num : 256u) && i + k + 3 < count; k += 4u)
                            std::fprintf(dump, "      %08x %08x %08x %08x\n", words[i + k], words[i + k + 1], words[i + k + 2], words[i + k + 3]);
                }
                i += unpack(s, words + i, count - i, cmd, num, imm);
                continue;
            }
            switch (cmd)
            {
            case 0x01u:
                s.cl = imm & 0xFFu;
                s.wl = imm >> 8;
                if (s.wl == 0u)
                    s.wl = 1u;
                break;
            case 0x02u:
                s.offset = imm & 0x3FFu;
                s.dbf = false;
                s.tops = s.base;
                break;
            case 0x03u:
                s.base = imm & 0x3FFu;
                break;
            case 0x04u:
                s.itop = imm & 0x3FFu;
                break;
            case 0x05u:
                s.mode = imm & 3u;
                break;
            case 0x14u: // MSCAL
            case 0x15u: // MSCALF
            case 0x17u: // MSCNT
                s.at = words + i - 1u;
                if (kick)
                    kick(s, cmd == 0x17u ? ~0u : imm * 8u);
                s.dbf = !s.dbf;
                s.tops = s.dbf ? s.base + s.offset : s.base;
                break;
            case 0x20u:
                if (i < count)
                    s.mask = words[i++];
                break;
            case 0x30u:
                for (int c = 0; c < 4 && i < count; ++c)
                    s.row[c] = words[i++];
                break;
            case 0x31u:
                for (int c = 0; c < 4 && i < count; ++c)
                    s.col[c] = words[i++];
                break;
            case 0x4Au: // MPG: num 64-bit instructions at imm * 8
            {
                const uint32_t n = num ? num : 256u;
                if (s.micro && i + 2u * n <= count)
                {
                    for (uint32_t k = 0; k < n; ++k)
                    {
                        const uint32_t at = ((imm + k) * 8u) & 0x3FFFu;
                        std::memcpy(s.micro + at, &words[i + 2u * k], 8u);
                    }
                    ++s.microVersion;
                }
                i += 2u * n;
                break;
            }
            case 0x50u: // DIRECT, DIRECTHL: imm qwords for the GIF, right after the code (the packer pads before it)
            case 0x51u:
            {
                const size_t q = imm ? imm : 65536u;
                if (s.direct && i + 4u * q <= count)
                    s.direct(words + i, q);
                i += 4u * q;
                break;
            }
            default:
                break;
            }
        }
    }
}

namespace nr
{
    namespace
    {
        using vif::asFloat;

        // The batch at ITOP, appended to the mesh.
        void batch(const vif::State &s, Mesh &mesh, FILE *dump)
        {
            const uint32_t header = s.itop & 0x3FFu;
            const uint32_t count = s.vu[header][0] & 0x7FFFu;
            const uint32_t type = s.vu[header][1] & 0xFFFFu;
            const float bias = asFloat(s.vu[header][2]);
            if (dump)
                std::fprintf(dump, "  batch at %u: count %u, type %04x, bias %g, stride %u\n", header, count, type, bias, s.stride);
            // The vertex: one qword (position), two (colour, position) or three (ST, colour or
            // normal, position), as the unpacks laid it out.
            const uint32_t stride = std::clamp(s.stride, 1u, 3u);
            if (count == 0u || count > 256u || header + stride * count + 4u >= 1024u)
                return;
            const uint32_t masks0 = s.vu[header + stride * count + 4u][0], masks1 = s.vu[header + stride * count + 4u][1];
            const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
            mesh.type = type;

            for (uint32_t v = 0; v < count; ++v)
            {
                static const uint32_t kNone[4] = {0u, 0u, 0u, 0u};
                const uint32_t at = header + 1u + stride * v;
                const uint32_t *st = stride == 3u ? s.vu[at] : kNone;
                const uint32_t *second = stride >= 2u ? s.vu[at + stride - 2u] : kNone;
                const uint32_t *third = s.vu[at + stride - 1u];
                Vertex out;
                const float stBias = (type == 3u || type == 4u) ? bias : 0.0f;
                const float posBias = (type == 1u || type == 2u) ? bias : 0.0f; // (compressed: offset unpacks)
                out.u = asFloat(st[0]) - stBias;
                out.v = asFloat(st[1]) - stBias;
                out.x = asFloat(third[0]) - posBias;
                out.y = asFloat(third[1]) - posBias;
                out.z = asFloat(third[2]) - posBias;
                if (type == 1u)
                    out.size = asFloat(third[3]) - (1.0f + bias) / 1024.0f; // (sprites: the size)
                else if (type == 0u || type == 0x8001u)
                    out.size = asFloat(third[3]); // (no conversion: a sprite's size its w as it is - Antarctica's snow, type 0 on 0x2740)
                if (type == 4u)
                {
                    // Type 4's second qword is the normal (integers in the mantissa, like its ST).
                    float nx = asFloat(second[0]) - bias, ny = asFloat(second[1]) - bias, nz = asFloat(second[2]) - bias;
                    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 0.0f)
                        nx /= len, ny /= len, nz /= len;
                    out.nx = nx, out.ny = ny, out.nz = nz;
                    mesh.hasNormal = true;
                }
                else if (stride >= 2u)
                {
                    const uint32_t r = std::min(second[0], 255u), g = std::min(second[1], 255u), b = std::min(second[2], 255u),
                                   a = std::min(second[3], 255u);
                    out.colour = r | g << 8 | b << 16 | a << 24;
                    mesh.hasColour = true;
                }
                mesh.vertices.push_back(out);
                const uint32_t mask = v < 16u ? masks0 : masks1;
                const bool kickOnly = ((mask >> (15u - (v & 15u))) & 1u) != 0u;
                if (v >= 2u && !kickOnly)
                {
                    mesh.indices.push_back(first + v - 2u);
                    mesh.indices.push_back(first + v - 1u);
                    mesh.indices.push_back(first + v);
                }
            }
            mesh.hasUv = mesh.hasUv || stride == 3u;
            // The strips' GIF tag (field A's, at the top of VU memory: PRE set, PRIM in bits 47-57).
            // (CTXT, bit 9, set: context 2's - the lit meshes' base pass, q1020-1021.)
            for (uint32_t q = 1020u; q < 1024u; ++q)
                if (s.vu[q][1] & 0x4000u)
                {
                    const uint32_t prim = (s.vu[q][1] >> 15) & 0x7FFu;
                    uint32_t &slot = (prim & 0x200u) ? mesh.prim2 : mesh.prim;
                    if (!slot)
                        slot = prim;
                }
        }
    }

    void decodePacket(const uint8_t *rdram, uint32_t start, Mesh &mesh, FILE *dump)
    {
        // (TODO 94: kept from one call to the next - a packet decoded on the renderer's thread every frame, the forest's sprite
        // sets, each made its 16 KB anew)
        static thread_local std::vector<uint32_t> words;
        words.clear();
        words.reserve(4096);
        if (!vif::gather(rdram, start, words, dump))
        {
            if (dump)
                std::fprintf(dump, "  chain not followed to its end\n");
            return;
        }
        static thread_local vif::State s;
        s = vif::State{};
        vif::run(s, words.data(), words.size(), [&](vif::State &st, uint32_t) { batch(st, mesh, dump); }, dump);
    }

    void decodePrimitiveStrips(const uint32_t *words, size_t count, Mesh &mesh, FILE *dump)
    {
        static thread_local vif::State s;
        s = vif::State{};
        // (Its unpacks rely on the cycle the game leaves set: three qwords a vertex, ST, colour,
        // position - unless the words set another.)
        s.cl = 3u;
        s.wl = 1u;
        s.stride = 3u;
        vif::run(s, words, count, [&](vif::State &st, uint32_t start) {
            // Its batches: MSCNT with a header of the count and 0x8000 (the end's), each a strip.
            const uint32_t header = st.itop & 0x3FFu;
            if (start == ~0u && (st.vu[header][0] & 0x8000u) != 0u)
                batch(st, mesh, dump);
        }, dump);
    }
}
