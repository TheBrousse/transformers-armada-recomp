// A vlTexturePS2 read as data into a PC texture (README.md): its texels and palette are in main
// memory (VL_TextureNative_PlayStation2.cpp: the levels at 0x98, the palette levels[unk_E0], the
// image levels[unk_90]); the upload packet (func_00267060) sends each level as an IMAGE transfer in
// the level's own pixel format, in columns `tile` wide, and the GS samples it in the texture's
// format (0x34). Where the two differ - 8-bit data stored as a half-size 32-bit image, 4-bit as a
// 16-bit one (ITF flag 0x80) - the GS's page layouts do the reordering, so the conversion writes the
// level the way the GS stores it and reads it back the way the GS samples it (the runtime's GS
// address functions). A one-time conversion when the texture is first seen, not GS emulation at draw
// time; a PC build stores the result.
#include "nr_types.h"
#include "nr_movie.h"

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/gs_threaded_backend.h" // (gsupload: the upload skip's check)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace nr
{
    // (TODO 164) A key's guest memory as convertTexture reads it, copied at the draw: each level it touches - the
    // image, the palette, the game's mip levels - its object's first 0x20 bytes (data pointer, format, size, bpp,
    // tile) and its texels. A worker converts from it (t_inputs) while the game goes on writing its memory.
    struct TextureInputs
    {
        struct Level
        {
            uint32_t at = 0;
            uint8_t header[0x20] = {};
            std::vector<uint8_t> texels;
        };
        std::vector<Level> levels;
        const Level *find(uint32_t level) const
        {
            for (const Level &l : levels)
                if (l.at == level)
                    return &l;
            return nullptr;
        }
    };

    namespace
    {
        thread_local const TextureInputs *t_inputs = nullptr; // (a worker's conversion: guest memory from here)

        constexpr uint32_t kPsmCt32 = 0x00u, kPsmCt24 = 0x01u, kPsmCt16 = 0x02u, kPsmT8 = 0x13u, kPsmT4 = 0x14u;
        // The indexed formats kept in a 32-bit buffer's top byte (its alpha: the picture's pages
        // reused for an 8-bit image, the colour beside it untouched): 8 bits, or 4 in either half.
        constexpr uint32_t kPsmT8H = 0x1Bu, kPsmT4HL = 0x24u, kPsmT4HH = 0x2Cu;

        bool highByte(uint32_t psm) { return psm == kPsmT8H || psm == kPsmT4HL || psm == kPsmT4HH; }
        bool indexed(uint32_t psm) { return psm == kPsmT8 || psm == kPsmT4 || highByte(psm); }
        uint32_t entriesOf(uint32_t psm) { return psm == kPsmT8 || psm == kPsmT8H ? 256u : 16u; }

        uint32_t word(const uint8_t *rdram, uint32_t addr)
        {
            if (t_inputs)
            {
                uint32_t v = 0u;
                for (const TextureInputs::Level &l : t_inputs->levels)
                    if (addr >= l.at && addr + 4u <= l.at + sizeof(l.header))
                    {
                        std::memcpy(&v, l.header + (addr - l.at), 4);
                        break;
                    }
                return v;
            }
            const uint8_t *p = getConstMemPtr(rdram, addr);
            uint32_t v = 0u;
            if (p)
                std::memcpy(&v, p, 4);
            return v;
        }

        // The bytes of [addr, addr + size) in guest memory (main RAM: contiguous).
        const uint8_t *bytes(const uint8_t *rdram, uint32_t addr, uint32_t size)
        {
            const uint8_t *first = getConstMemPtr(rdram, addr);
            const uint8_t *last = size ? getConstMemPtr(rdram, addr + size - 1u) : first;
            return first && last && last == first + size - 1u ? first : nullptr;
        }

        uint32_t pageWidth(uint32_t psm, uint32_t width)
        {
            return psm == kPsmT8 || psm == kPsmT4 ? ((width + 127u) / 128u) * 2u : (width + 63u) / 64u;
        }

        // The byte (or, for 4-bit, nibble) address of pixel (x, y) in a GS buffer at block 0.
        uint32_t address(uint32_t psm, uint32_t bw, uint32_t x, uint32_t y)
        {
            switch (psm)
            {
            case kPsmCt16:
                return GSPSMCT16::addrPSMCT16(0u, bw, x, y);
            case kPsmT8:
                return GSPSMT8::addrPSMT8(0u, bw, x, y);
            case kPsmT4:
                return GSPSMT4::addrPSMT4(0u, bw, x, y);
            default:
                return GSPSMCT32::addrPSMCT32(0u, bw, x, y);
            }
        }

        uint32_t gsColour(uint32_t c) // R in the low byte, alpha 0x80 = 1.0
        {
            const uint32_t a = std::min((c >> 24) * 2u, 255u);
            return (c & 0x00FFFFFFu) | a << 24;
        }

        constexpr size_t kGsBytes = 4u << 20;

        uint32_t paletteSwizzle(uint32_t i) { return (i & ~0x18u) | ((i & 8u) << 1) | ((i & 0x10u) >> 1); }

        // Levels' texels as they were when uploaded, by level object (their data pointer then).
        struct Snapshot
        {
            uint32_t data = 0;
            std::vector<uint8_t> bytes;
            uint32_t generation = 0; // (its texels changed since the first upload, this many times)
        };
        std::unordered_map<uint32_t, Snapshot> g_snapshots;
        uint64_t g_snapshotEpoch = 0; // (TODO 94: a generation changed - snapshotEpoch)

        uint32_t levelSize(const uint8_t *rdram, uint32_t level)
        {
            const uint32_t uw = word(rdram, level + 0x0Cu), uh = word(rdram, level + 0x10u), bpp = word(rdram, level + 0x14u);
            if (uw == 0u || uh == 0u || uw > 2048u || uh > 2048u || (bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u))
                return 0u;
            return (uw * uh * bpp + 7u) / 8u;
        }

        // A level's texels: in main memory while its copy is there, else the snapshot of its upload.
        const uint8_t *levelBytes(const uint8_t *rdram, uint32_t level, uint32_t size)
        {
            if (t_inputs)
            {
                const TextureInputs::Level *l = t_inputs->find(level);
                return l && l->texels.size() >= size && size ? l->texels.data() : nullptr;
            }
            const uint32_t data = word(rdram, level);
            if (data)
                if (const uint8_t *live = bytes(rdram, data, size))
                    return live;
            auto it = g_snapshots.find(level);
            return it != g_snapshots.end() && it->second.bytes.size() >= size ? it->second.bytes.data() : nullptr;
        }
    }

    namespace
    {
        const uint8_t *g_vram = nullptr;
    }

    void setGsMemory(const uint8_t *vram) { g_vram = vram; }

    // (Debug: a region of GS memory as a PPM - 16- or 32-bit pixels at TBP, TBW.)
    void dumpGsRegion(const char *path, uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t w, uint32_t h)
    {
        if (!g_vram || w == 0u || h == 0u || w > 2048u || h > 2048u)
            return;
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        const uint32_t bw = tbw ? tbw : pageWidth(psm, w);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                uint8_t rgb[3] = {};
                if (psm == kPsmCt16 || psm == 0x0Au)
                {
                    const uint32_t at = GSPSMCT16::addrPSMCT16(tbp, bw, x, y);
                    if (at + 2u <= kGsBytes)
                    {
                        const uint32_t c = g_vram[at] | g_vram[at + 1u] << 8;
                        rgb[0] = static_cast<uint8_t>((c & 31u) << 3), rgb[1] = static_cast<uint8_t>(((c >> 5) & 31u) << 3),
                        rgb[2] = static_cast<uint8_t>(((c >> 10) & 31u) << 3);
                    }
                }
                else if (psm == 0x7Fu) // (a debug choice: a CT32 buffer's alpha, as grey)
                {
                    const uint32_t at = GSPSMCT32::addrPSMCT32(tbp, bw, x, y);
                    if (at + 4u <= kGsBytes)
                        rgb[0] = rgb[1] = rgb[2] = g_vram[at + 3u];
                }
                else
                {
                    const uint32_t at = GSPSMCT32::addrPSMCT32(tbp, bw, x, y);
                    if (at + 4u <= kGsBytes)
                        std::memcpy(rgb, g_vram + at, 3u);
                }
                std::fwrite(rgb, 1u, 3u, f);
            }
        std::fclose(f);
    }

    void buildMips(Texture &texture)
    {
        texture.mips.clear();
        const std::vector<uint32_t> *src = &texture.pixels;
        uint32_t pw = texture.width, ph = texture.height;
        while (pw > 1u || ph > 1u)
        {
            const uint32_t w = std::max(1u, pw / 2u), h = std::max(1u, ph / 2u);
            std::vector<uint32_t> dst(static_cast<size_t>(w) * h);
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint32_t *r0 = src->data() + static_cast<size_t>(std::min(ph - 1u, 2u * y)) * pw;
                const uint32_t *r1 = src->data() + static_cast<size_t>(std::min(ph - 1u, 2u * y + 1u)) * pw;
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t x0 = std::min(pw - 1u, 2u * x), x1 = std::min(pw - 1u, 2u * x + 1u);
                    const uint32_t c[4] = {r0[x0], r0[x1], r1[x0], r1[x1]};
                    uint32_t out = 0u;
                    for (int k = 0; k < 4; ++k)
                    {
                        const uint32_t sum = ((c[0] >> (8 * k)) & 0xFFu) + ((c[1] >> (8 * k)) & 0xFFu) + ((c[2] >> (8 * k)) & 0xFFu) +
                                             ((c[3] >> (8 * k)) & 0xFFu);
                        out |= ((sum + 2u) / 4u) << (8 * k);
                    }
                    dst[static_cast<size_t>(y) * w + x] = out;
                }
            }
            texture.mips.push_back(std::move(dst));
            src = &texture.mips.back();
            pw = w;
            ph = h;
        }
    }

    void rememberLevel(const uint8_t *rdram, uint32_t level)
    {
        if (!level)
            return;
        const uint32_t data = word(rdram, level), size = levelSize(rdram, level);
        if (!data || !size)
            return;
        // (Roadmap N6: a movie's picture drawn from the decoder - the game's copy of it not written, nothing to keep.)
        if (movie::skipsSnapshot(data))
            return;
        Snapshot &s = g_snapshots[level];
        if (s.data == data && s.bytes.size() == size)
        {
            // (The same upload again: refreshed only if the texels changed - a level's own data may be
            // rewritten in place, a palette animated, say.)
            const uint8_t *live = bytes(rdram, data, size);
            if (live && std::memcmp(live, s.bytes.data(), size) == 0)
                return;
        }
        if (const uint8_t *live = bytes(rdram, data, size))
        {
            if (!s.bytes.empty())
            {
                ++s.generation; // (uploaded again with other texels: its conversions are stale - TODO 58)
                ++g_snapshotEpoch;
            }
            s.data = data;
            s.bytes.assign(live, live + size);
        }
    }

    uint64_t snapshotEpoch() { return g_snapshotEpoch; }

    uint32_t levelGeneration(uint32_t level)
    {
        auto it = g_snapshots.find(level);
        return it != g_snapshots.end() ? it->second.generation : 0u;
    }

    std::shared_ptr<const Texture> convertPalette(const uint8_t *rdram, const TextureKey &key, uint64_t id)
    {
        if (!key.clut)
            return nullptr;
        const uint32_t clutEntries = word(rdram, key.clut + 0x0Cu) * word(rdram, key.clut + 0x10u);
        const uint8_t *c = clutEntries >= 256u ? levelBytes(rdram, key.clut, clutEntries * 4u) : nullptr;
        if (!c)
            return nullptr;
        auto out = std::make_shared<Texture>();
        out->id = id;
        out->width = 256u;
        out->height = 1u;
        out->pixels.resize(256u);
        for (uint32_t i = 0; i < 256u; ++i)
        {
            uint32_t v;
            std::memcpy(&v, c + 4u * paletteSwizzle(i), 4);
            out->pixels[i] = gsColour(v);
        }
        return out;
    }

    std::shared_ptr<const Texture> convertTexture(const uint8_t *rdram, const TextureKey &key, uint64_t id)
    {
        if (key.mip[0])
        {
            // The game's own levels below the image (what the GS reads past level 0 under TEX1): each
            // converted as the image is - its texels in the level's layout, read in TEX0's format at
            // half the size of the one above, the image's palette - and kept as the texture's first
            // mips; the GPU side box-filters only below the last of them (nr_d3d.cpp: makeTexture).
            TextureKey base = key;
            base.mip[0] = base.mip[1] = base.mip[2] = 0u;
            auto top = convertTexture(rdram, base, id);
            if (!top)
                return top;
            auto out = std::make_shared<Texture>(*top);
            for (uint32_t k = 1; k <= 3u && key.mip[k - 1u]; ++k)
            {
                TextureKey m = base;
                m.image = key.mip[k - 1u];
                m.width = std::max(1u, key.width >> k);
                m.height = std::max(1u, key.height >> k);
                m.tbw = 0u; // (the level's own buffer width, as its upload laid it out)
                const uint32_t size = levelSize(rdram, m.image);
                if (!size || !levelBytes(rdram, m.image, size))
                    break; // (no texels: not read from GS memory)
                auto level = convertTexture(rdram, m, 0u);
                if (!level || level->pixels.size() != static_cast<size_t>(m.width) * m.height)
                    break;
                out->mips.push_back(level->pixels);
                ++out->gameMips;
            }
            static int mipLog = std::getenv("TF_NATIVE_MIPLOG") ? std::atoi(std::getenv("TF_NATIVE_MIPLOG")) : 0;
            if (mipLog > 0 && out->gameMips)
            {
                // (Debug: how far the game's level 1 is from the image box-filtered: per channel, mean.)
                --mipLog;
                Texture boxed;
                boxed.width = out->width;
                boxed.height = out->height;
                boxed.pixels = out->pixels;
                buildMips(boxed);
                double diff[4] = {};
                const auto &a = out->mips[0], &b = boxed.mips[0];
                for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                    for (int k = 0; k < 4; ++k)
                        diff[k] += std::abs(static_cast<int>((a[i] >> (8 * k)) & 0xFFu) - static_cast<int>((b[i] >> (8 * k)) & 0xFFu));
                const double n = static_cast<double>(std::max<size_t>(a.size(), 1u));
                std::fprintf(stderr, "[native render] game mips of %08x %ux%u: %u levels, level 1 against the box filter: R %.2f G %.2f B %.2f A %.2f\n", key.texture,
                             out->width, out->height, out->gameMips, diff[0] / n, diff[1] / n, diff[2] / n, diff[3] / n);
            }
            return out;
        }
        const uint32_t psm = key.psm, width = key.width, height = key.height, level = key.image;
        if (width == 0u || height == 0u || width > 2048u || height > 2048u || !level)
            return nullptr;
        uint32_t upsm = word(rdram, level + 0x04u);
        // A group with 24-bit textures packs its 8-bit and 4-bit levels into those pages' alpha bytes
        // (vlTextureGroupRegion's capture: T8 -> T8H, T4 -> T4HL / T4HH, the upload sent in that
        // format) and puts the level's own format back afterwards: the texels in main memory are the
        // plain indices, stored in GS memory as TEX0's format says, not as the level's.
        if (highByte(psm) && (upsm == kPsmT8 || upsm == kPsmT4) && (upsm == kPsmT8) == (psm == kPsmT8H))
            upsm = psm;
        const uint32_t uw = word(rdram, level + 0x0Cu), uh = word(rdram, level + 0x10u);
        const uint32_t bpp = word(rdram, level + 0x14u);
        uint32_t tile = word(rdram, level + 0x1Cu);
        if (uw == 0u || uh == 0u || uw > 2048u || uh > 2048u || (bpp != 4u && bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u))
            return nullptr;
        if (tile == 0u || tile > uw)
            tile = uw;
        const uint8_t *src = levelBytes(rdram, level, (uw * uh * bpp + 7u) / 8u);
        // (No texels in main memory: a top-byte image read from GS memory where TEX0 points, if the
        // GS memory is known - the light maps. Never on a worker: captureTextureInputs leaves those to the draw.)
        const bool fromGs = !src && g_vram && highByte(psm) && !t_inputs;
        if (!src && !fromGs)
            return nullptr;
        if (fromGs)
            censusNote(1u, static_cast<uint64_t>(width) * height); // (TF_VU1_CENSUS: a light map read from GS memory)

        // The level as the GS stores it. (TODO 164: only the pages the level and the reading reach are cleared - the
        // whole 4 MB each time was a fifth of a conversion's time; what lies beyond them is never read.)
        static thread_local std::vector<uint8_t> gs;
        if (gs.size() != kGsBytes)
            gs.assign(kGsBytes, 0u);
        {
            const uint32_t rbw0 = key.tbw ? key.tbw : pageWidth(psm, width);
            auto pageEnd = [](uint32_t p, uint32_t bw, uint32_t w, uint32_t h) -> size_t {
                const uint32_t a = address(p, std::max(bw, 1u), w - 1u, h - 1u);
                const size_t at = (p == kPsmT4 ? a >> 1 : a) + 4u;
                return std::min(kGsBytes, (at + 8191u) / 8192u * 8192u + 8192u);
            };
            size_t reach = std::max(pageEnd(upsm, pageWidth(upsm, uw), uw, uh), pageEnd(psm, rbw0, width, height));
            if (fromGs)
                reach = std::max(reach, pageEnd(kPsmCt32, rbw0, width, height));
            static const bool wholeClear = std::getenv("TF_NATIVE_TEXCLEAR") && *std::getenv("TF_NATIVE_TEXCLEAR") == '1';
            std::fill(gs.begin(), gs.begin() + static_cast<std::ptrdiff_t>(wholeClear ? kGsBytes : reach), uint8_t{0});
        }
        if (fromGs)
        {
            // Its pages copied to block 0, where the reading below looks.
            const uint32_t rbw = key.tbw ? key.tbw : pageWidth(psm, width);
            // (gsupload: the uploads a skip left out that reach into its pages written first - PS2Recomp patch 0073)
            gsupload::ensureWhole(static_cast<uint64_t>(key.tbp) * 256u,
                                  static_cast<uint64_t>(key.tbp) * 256u + ((static_cast<uint64_t>(height - 1u) / 32u) * std::max(rbw, 1u) + (width - 1u) / 64u + 1u) * 8192u);
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t from = GSPSMCT32::addrPSMCT32(key.tbp, rbw, x, y), to = GSPSMCT32::addrPSMCT32(0u, rbw, x, y);
                    if (from + 4u <= kGsBytes && to + 4u <= kGsBytes)
                        std::memcpy(&gs[to], g_vram + from, 4u);
                }
            // (PS2X_GS_SKIP_UPLOADS=check: would a skip of the uploads have left any of it stale - its top bytes, a light map's)
            if (gsupload::mode() == gsupload::Mode::Check)
            {
                uint32_t stale = 0u;
                for (uint32_t y = 0; y < height; ++y)
                    for (uint32_t x = 0; x < width; ++x)
                        if (gsupload::staleAt(GSPSMCT32::addrPSMCT32(key.tbp, rbw, x, y), true))
                            ++stale;
                gsupload::reportRead("a light map read from GS memory", key.tbp, psm, stale, width * height);
            }
        }
        const uint32_t dbw = pageWidth(upsm, uw);
        for (uint32_t x0 = 0; !fromGs && x0 < uw; x0 += tile)
            for (uint32_t y = 0; y < uh; ++y)
                for (uint32_t i = 0; i < tile && x0 + i < uw; ++i)
                {
                    const uint32_t n = x0 * uh + y * tile + i; // the pixel's index in the level's data
                    const uint32_t x = x0 + i;
                    const uint32_t at = address(upsm, dbw, x, y);
                    switch (upsm)
                    {
                    case kPsmT4:
                    {
                        const uint8_t nib = (src[n >> 1] >> ((n & 1u) * 4u)) & 0xFu;
                        if ((at >> 1) < kGsBytes)
                            gs[at >> 1] = static_cast<uint8_t>((gs[at >> 1] & ~(0xFu << ((at & 1u) * 4u))) | nib << ((at & 1u) * 4u));
                        break;
                    }
                    case kPsmT8:
                        if (at < kGsBytes)
                            gs[at] = src[n];
                        break;
                    case kPsmCt16:
                        if (at + 2u <= kGsBytes)
                            std::memcpy(&gs[at], src + 2u * n, 2u);
                        break;
                    case kPsmT8H:
                        if (at + 4u <= kGsBytes)
                            gs[at + 3u] = src[n];
                        break;
                    case kPsmT4HL:
                    case kPsmT4HH:
                        if (at + 4u <= kGsBytes)
                        {
                            const uint8_t nib = (src[n >> 1] >> ((n & 1u) * 4u)) & 0xFu;
                            const uint32_t shift = upsm == kPsmT4HH ? 4u : 0u;
                            gs[at + 3u] = static_cast<uint8_t>((gs[at + 3u] & ~(0xFu << shift)) | nib << shift);
                        }
                        break;
                    case kPsmCt24:
                        if (at + 3u <= kGsBytes)
                            std::memcpy(&gs[at], src + 3u * n, 3u);
                        break;
                    default:
                        if (at + 4u <= kGsBytes)
                            std::memcpy(&gs[at], src + 4u * n, 4u);
                        break;
                    }
                }

        // The palette (indexed formats): its level, in 32-bit GS form (an 8-bit one with its
        // entries' bits 3 and 4 exchanged, the GS's CSM1 order); a 4-bit one at CSA * 16.
        std::vector<uint32_t> palette;
        if (indexed(psm))
        {
            const uint32_t entries = entriesOf(psm);
            const uint32_t clutEntries = key.clut ? word(rdram, key.clut + 0x0Cu) * word(rdram, key.clut + 0x10u) : 0u;
            const uint32_t offset = entries == 16u ? key.csa * 16u : 0u;
            const uint8_t *c = key.clut && clutEntries >= offset + entries ? levelBytes(rdram, key.clut, clutEntries * 4u) : nullptr;
            if (!c)
                return nullptr;
            palette.resize(entries);
            for (uint32_t i = 0; i < entries; ++i)
            {
                uint32_t v;
                std::memcpy(&v, c + 4u * (entries == 256u ? paletteSwizzle(i) : offset + i), 4);
                palette[i] = gsColour(v);
            }
        }

        // Read back the way the GS samples it.
        auto out = std::make_shared<Texture>();
        out->id = id;
        out->source = key.texture;
        out->width = width;
        out->height = height;
        out->pixels.resize(static_cast<size_t>(width) * height);
        const uint32_t rbw = key.tbw ? key.tbw : pageWidth(psm, width);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t at = address(psm, rbw, x, y);
                uint32_t c = 0u;
                switch (psm)
                {
                case kPsmT4:
                    c = (at >> 1) < kGsBytes ? palette[(gs[at >> 1] >> ((at & 1u) * 4u)) & 0xFu] : 0u;
                    break;
                case kPsmT8:
                    c = at < kGsBytes ? palette[gs[at]] : 0u;
                    break;
                case kPsmT8H:
                    c = at + 4u <= kGsBytes ? palette[gs[at + 3u]] : 0u;
                    break;
                case kPsmT4HL:
                    c = at + 4u <= kGsBytes ? palette[gs[at + 3u] & 0xFu] : 0u;
                    break;
                case kPsmT4HH:
                    c = at + 4u <= kGsBytes ? palette[gs[at + 3u] >> 4] : 0u;
                    break;
                case kPsmCt16:
                {
                    uint16_t v = 0;
                    if (at + 2u <= kGsBytes)
                        std::memcpy(&v, &gs[at], 2);
                    c = (v & 0x1Fu) << 3 | ((v >> 5) & 0x1Fu) << 11 | ((v >> 10) & 0x1Fu) << 19 | (v & 0x8000u ? 0xFF000000u : 0u);
                    break;
                }
                case kPsmCt24:
                    if (at + 4u <= kGsBytes)
                        std::memcpy(&c, &gs[at], 4);
                    c = (c & 0x00FFFFFFu) | 0xFF000000u;
                    break;
                default:
                    if (at + 4u <= kGsBytes)
                        std::memcpy(&c, &gs[at], 4);
                    c = gsColour(c);
                    break;
                }
                out->pixels[static_cast<size_t>(y) * width + x] = c;
            }
        return out;
    }

    std::shared_ptr<TextureInputs> captureTextureInputs(const uint8_t *rdram, const TextureKey &key)
    {
        if (!key.image || key.width == 0u || key.height == 0u)
            return nullptr;
        auto in = std::make_shared<TextureInputs>();
        // (a level: its header, and its texels when `size` - false when they are not there)
        auto add = [&](uint32_t level, uint32_t size) {
            if (!level || in->find(level))
                return true;
            const uint8_t *h = bytes(rdram, level, sizeof(TextureInputs::Level::header));
            if (!h)
                return false;
            TextureInputs::Level l;
            l.at = level;
            std::memcpy(l.header, h, sizeof(l.header));
            bool ok = true;
            if (size)
            {
                if (const uint8_t *t = levelBytes(rdram, level, size))
                    l.texels.assign(t, t + size);
                else
                    ok = false;
            }
            in->levels.push_back(std::move(l));
            return ok;
        };
        const uint32_t imageSize = levelSize(rdram, key.image);
        if (!imageSize || !add(key.image, imageSize))
            return nullptr; // (no texels in main memory: GS memory's, or none - at once, as before)
        if (key.clut)
        {
            const uint32_t entries = word(rdram, key.clut + 0x0Cu) * word(rdram, key.clut + 0x10u);
            if (entries && entries <= 65536u && !add(key.clut, entries * 4u))
                return nullptr;
            add(key.clut, 0u);
        }
        for (uint32_t k = 0; k < 3u && key.mip[k]; ++k)
            if (const uint32_t size = levelSize(rdram, key.mip[k]))
                add(key.mip[k], size); // (a level without texels: the conversion stops its mips there, as it did)
        return in;
    }

    std::shared_ptr<const Texture> convertTextureFrom(const TextureInputs &inputs, const TextureKey &key, uint64_t id)
    {
        const TextureInputs *before = t_inputs;
        t_inputs = &inputs;
        auto out = convertTexture(nullptr, key, id);
        t_inputs = before;
        return out;
    }

    uint64_t pixelHash(const Texture &t)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const std::vector<uint32_t> &v) {
            const size_t n = v.size();
            size_t i = 0;
            for (; i + 2u <= n; i += 2u)
            {
                uint64_t q;
                std::memcpy(&q, v.data() + i, 8);
                h = (h ^ q) * 1099511628211ull;
            }
            if (i < n)
                h = (h ^ v[i]) * 1099511628211ull;
        };
        mix(t.pixels);
        for (size_t k = 0; k < t.gameMips && k < t.mips.size(); ++k)
            mix(t.mips[k]);
        return h ? h : 1u;
    }

    // (N3b) A texture read straight from GS memory as TEX0 says - its pixels and palette uploaded there by the game's own
    // IMAGE transfers, no texture object of the render device (the load screens drawn outside the game's frames).
    // CT32 / CT24 / CT16, T8 / T4 (and their top-byte views) with a CT32 or CT16 palette in CSM1 order. Null: none.
    std::shared_ptr<Texture> textureFromGsMemory(uint64_t tex0, uint64_t id)
    {
        if (!g_vram)
            return nullptr;
        const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu), tbw = static_cast<uint32_t>((tex0 >> 14) & 0x3Fu);
        const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3Fu);
        const uint32_t width = 1u << ((tex0 >> 26) & 0xFu), height = 1u << ((tex0 >> 30) & 0xFu);
        const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFFu), cpsm = static_cast<uint32_t>((tex0 >> 51) & 0xFu);
        const uint32_t csa = static_cast<uint32_t>((tex0 >> 56) & 0x1Fu);
        if (width > 1024u || height > 1024u)
            return nullptr;
        censusNote(2u, static_cast<uint64_t>(width) * height); // (TF_VU1_CENSUS)
        {
            // (gsupload: the uploads a skip left out that reach into its texels or its palette written first - patch 0073)
            const uint32_t pw = tbw ? tbw : std::max(1u, width / 64u);
            gsupload::ensureWhole(static_cast<uint64_t>(tbp) * 256u,
                                  static_cast<uint64_t>(tbp) * 256u + ((static_cast<uint64_t>(height - 1u) / 32u) * pw + (width - 1u) / 64u + 1u) * 8192u);
            gsupload::ensureWhole(static_cast<uint64_t>(cbp) * 256u, static_cast<uint64_t>(cbp) * 256u + 8192u);
        }
        if (gsupload::mode() == gsupload::Mode::Check)
        {
            // (PS2X_GS_SKIP_UPLOADS=check) Its texels' and palette's GS memory, every 4th texel each way: stale under a skip?
            uint32_t stale = 0u, seen = 0u;
            const uint32_t w = tbw ? tbw : std::max(1u, width / 64u);
            for (uint32_t y = 0; y < height; y += 4u)
                for (uint32_t x = 0; x < width; x += 4u)
                {
                    uint32_t at;
                    switch (psm)
                    {
                    case 0x02u: case 0x0Au: at = GSPSMCT16::addrPSMCT16(tbp, w, x, y); break;
                    case 0x13u: at = GSPSMT8::addrPSMT8(tbp, w, x, y); break;
                    case 0x14u: at = GSPSMT4::addrPSMT4(tbp, w, x, y) >> 1u; break;
                    default: at = GSPSMCT32::addrPSMCT32(tbp, w, x, y); break;
                    }
                    ++seen;
                    if (gsupload::staleAt(at, psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu))
                        ++stale;
                }
            gsupload::reportRead("a texture read from GS memory (TEX0)", tbp, psm, stale, seen);
        }
        const uint32_t bw = tbw ? tbw : std::max(1u, width / 64u);
        const auto ct32 = [](uint32_t block, uint32_t w, uint32_t x, uint32_t y) {
            const uint32_t at = GSPSMCT32::addrPSMCT32(block, w, x, y);
            uint32_t c = 0u;
            if (at + 4u <= kGsBytes)
                std::memcpy(&c, g_vram + at, 4u);
            return c;
        };
        const auto ct16 = [](uint32_t block, uint32_t w, uint32_t x, uint32_t y) {
            const uint32_t at = GSPSMCT16::addrPSMCT16(block, w, x, y);
            const uint32_t v = at + 2u <= kGsBytes ? static_cast<uint32_t>(g_vram[at] | g_vram[at + 1u] << 8) : 0u;
            return (v & 0x1Fu) << 3 | ((v >> 5) & 0x1Fu) << 11 | ((v >> 10) & 0x1Fu) << 19 | (v & 0x8000u ? 0x80000000u : 0u);
        };
        std::vector<uint32_t> palette;
        if (indexed(psm))
        {
            const uint32_t entries = entriesOf(psm);
            palette.resize(entries);
            for (uint32_t i = 0; i < entries; ++i)
            {
                uint32_t x, y;
                if (entries == 256u)
                {
                    const uint32_t s = paletteSwizzle(i);
                    x = s & 15u, y = s >> 4;
                }
                else
                {
                    // (16 entries: an 8 x 2 tile of the palette's 16 x 16, chosen by CSA)
                    x = (csa & 1u) * 8u + (i & 7u), y = (csa >> 1) * 2u + (i >> 3);
                }
                palette[i] = gsColour(cpsm == 0u || cpsm == 1u ? ct32(cbp, 1u, x, y) : ct16(cbp, 1u, x, y));
            }
        }
        auto out = std::make_shared<Texture>();
        out->id = id;
        out->width = width;
        out->height = height;
        out->pixels.resize(static_cast<size_t>(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t c = 0u;
                switch (psm)
                {
                case kPsmT4:
                {
                    const uint32_t at = GSPSMT4::addrPSMT4(tbp, bw, x, y);
                    c = (at >> 1) < kGsBytes ? palette[(g_vram[at >> 1] >> ((at & 1u) * 4u)) & 0xFu] : 0u;
                    break;
                }
                case kPsmT8:
                {
                    const uint32_t at = GSPSMT8::addrPSMT8(tbp, bw, x, y);
                    c = at < kGsBytes ? palette[g_vram[at]] : 0u;
                    break;
                }
                case kPsmT8H:
                    c = palette[ct32(tbp, bw, x, y) >> 24];
                    break;
                case kPsmT4HL:
                    c = palette[(ct32(tbp, bw, x, y) >> 24) & 0xFu];
                    break;
                case kPsmT4HH:
                    c = palette[ct32(tbp, bw, x, y) >> 28];
                    break;
                case kPsmCt16:
                case 0x0Au:
                    c = gsColour(ct16(tbp, bw, x, y));
                    break;
                case kPsmCt24:
                    c = (ct32(tbp, bw, x, y) & 0x00FFFFFFu) | 0xFF000000u;
                    break;
                default:
                    c = gsColour(ct32(tbp, bw, x, y));
                    break;
                }
                out->pixels[static_cast<size_t>(y) * width + x] = c;
            }
        return out;
    }
}
