// The VIF1 command stream read as data (README.md, nr_vif.cpp): a VU1 memory image of our own that
// the game's packets are applied to - their UNPACKs (cycle, row/column, masks, double buffering),
// their microcode (MPG) - and a callback where a microprogram would start. Used both for a geometry
// set's own packet (converted to a mesh once) and for the display list between draws (which shader
// runs with which data). Nothing here executes VU1 code.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

namespace nr::vif
{
    struct State
    {
        uint32_t vu[1024][4] = {};
        uint32_t row[4] = {}, col[4] = {}, mask = 0u;
        uint32_t cl = 1u, wl = 1u, mode = 0u;
        uint32_t base = 0u, offset = 0u, tops = 0u, itop = 0u;
        bool dbf = false;
        uint32_t stride = 3u; // qwords a vertex: STCYCL's CL when the vertices were unpacked
        uint8_t *micro = nullptr; // 16 KB of microcode memory, when MPGs are to be kept
        uint64_t microVersion = 0; // counts MPG uploads
        std::function<void(const uint32_t *, size_t qwords)> direct; // DIRECT / DIRECTHL: GIF packets
        const uint32_t *at = nullptr; // (N5.3) the command a kick comes from, in the words run
    };

    // Kick: MSCAL / MSCALF (start: the address) or MSCNT (start: ~0u).
    using Kick = std::function<void(State &, uint32_t start)>;

    // The words in order; `dump`: the commands as text.
    void run(State &s, const uint32_t *words, size_t count, const Kick &kick, FILE *dump = nullptr);

    // The words a DMA chain from `start` sends to VIF1 (each tag's upper words, then its data),
    // following NEXT/CALL/RET/REF to its end. `skip`: a CALL whose target this returns true for is
    // not followed (its words are left out).
    // `positions` (N5.3): for each tag of the chain's own level (not inside a CALL), the index in `out` where its words
    // begin and the tag's address - where the words stand in the list.
    bool gather(const uint8_t *rdram, uint32_t start, std::vector<uint32_t> &out, FILE *dump = nullptr,
                const std::function<bool(uint32_t)> &skip = {}, uint32_t stopAt = 0u,
                std::vector<std::pair<uint32_t, uint32_t>> *positions = nullptr);

    // The words' commands followed without doing them (the UNPACKs' lengths need no state): the
    // DIRECT packets handed to `direct`, in order. The game's thread does this; the full run is the
    // renderer's (nr_thread.cpp).
    void skim(const uint32_t *words, size_t count, const std::function<void(const uint32_t *, size_t)> &direct);

    float asFloat(uint32_t w);
}
