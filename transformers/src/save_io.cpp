// Transformers saves in and out of the port's memory card: save_io.h has the long form.

#include "save_io.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace save_io
{
    namespace
    {
        uint16_t get16(const std::vector<uint8_t> &d, size_t o) { return o + 2 <= d.size() ? static_cast<uint16_t>(d[o] | (d[o + 1] << 8)) : 0; }
        uint32_t get32(const std::vector<uint8_t> &d, size_t o)
        {
            return o + 4 <= d.size() ? d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24) : 0;
        }
        std::string cstr(const std::vector<uint8_t> &d, size_t o, size_t n)
        {
            std::string s;
            for (size_t i = 0; i < n && o + i < d.size() && d[o + i] != 0; ++i)
                s.push_back(static_cast<char>(d[o + i]));
            return s;
        }
        std::vector<uint8_t> slice(const std::vector<uint8_t> &d, size_t o, size_t n)
        {
            if (o >= d.size())
                return {};
            return std::vector<uint8_t>(d.begin() + static_cast<long>(o), d.begin() + static_cast<long>(std::min(d.size(), o + n)));
        }
        std::vector<uint8_t> readAll(const fs::path &p)
        {
            std::ifstream in(p, std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }

        bool keep(Save &save, std::vector<Save> &out)
        {
            save.region = regionOf(save.directory);
            if (save.region == Region::Unknown || save.files.empty())
                return false;
            out.push_back(std::move(save));
            return true;
        }

        // ---------------------------------------------------------------------------------
        // LZARI (Haruhiko Okumura, 1989), the compression of Action Replay Max saves.

        std::vector<uint8_t> lzariDecode(const std::vector<uint8_t> &src, size_t begin, size_t textSize)
        {
            constexpr int N = 4096, F = 60, THRESHOLD = 2, M = 15;
            constexpr int64_t Q1 = 1 << M, Q2 = 2 * Q1, Q3 = 3 * Q1, Q4 = 4 * Q1, MAX_CUM = Q1 - 1;
            constexpr int N_CHAR = 256 - THRESHOLD + F;
            size_t bitpos = begin * 8;
            const size_t nbits = src.size() * 8;
            auto getbit = [&]() -> int64_t
            {
                if (bitpos >= nbits)
                {
                    ++bitpos;
                    return 0;
                }
                const int b = (src[bitpos >> 3] >> (7 - (bitpos & 7))) & 1;
                ++bitpos;
                return b;
            };
            std::vector<int> charToSym(N_CHAR), symToChar(N_CHAR + 1), symFreq(N_CHAR + 1), symCum(N_CHAR + 1),
                positionCum(N + 1);
            symCum[N_CHAR] = 0;
            for (int sym = N_CHAR; sym >= 1; --sym)
            {
                const int ch = sym - 1;
                charToSym[ch] = sym;
                symToChar[sym] = ch;
                symFreq[sym] = 1;
                symCum[sym - 1] = symCum[sym] + symFreq[sym];
            }
            symFreq[0] = 0;
            positionCum[N] = 0;
            for (int i = N; i >= 1; --i)
                positionCum[i - 1] = positionCum[i] + 10000 / (i + 200);

            auto update = [&](int sym)
            {
                if (symCum[0] >= MAX_CUM)
                {
                    int c = 0;
                    for (int i = N_CHAR; i > 0; --i)
                    {
                        symCum[i] = c;
                        symFreq[i] = (symFreq[i] + 1) >> 1;
                        c += symFreq[i];
                    }
                    symCum[0] = c;
                }
                int i = sym;
                while (symFreq[i] == symFreq[i - 1])
                    --i;
                if (i < sym)
                {
                    const int chI = symToChar[i], chSym = symToChar[sym];
                    symToChar[i] = chSym;
                    symToChar[sym] = chI;
                    charToSym[chI] = sym;
                    charToSym[chSym] = i;
                }
                ++symFreq[i];
                while (--i >= 0)
                    ++symCum[i];
            };

            int64_t low = 0, high = Q4, value = 0;
            for (int i = 0; i < M + 2; ++i)
                value = 2 * value + getbit();
            auto normalize = [&]()
            {
                for (;;)
                {
                    if (low >= Q2)
                    {
                        value -= Q2;
                        low -= Q2;
                        high -= Q2;
                    }
                    else if (low >= Q1 && high <= Q3)
                    {
                        value -= Q1;
                        low -= Q1;
                        high -= Q1;
                    }
                    else if (high > Q2)
                        break;
                    low += low;
                    high += high;
                    value = 2 * value + getbit();
                }
            };
            auto decodeChar = [&]()
            {
                const int64_t range = high - low;
                const int64_t x = ((value - low + 1) * symCum[0] - 1) / range;
                int i = 1, j = N_CHAR;
                while (i < j)
                {
                    const int k = (i + j) / 2;
                    if (symCum[k] > x)
                        i = k + 1;
                    else
                        j = k;
                }
                const int sym = i;
                high = low + (range * symCum[sym - 1]) / symCum[0];
                low = low + (range * symCum[sym]) / symCum[0];
                normalize();
                const int ch = symToChar[sym];
                update(sym);
                return ch;
            };
            auto decodePosition = [&]()
            {
                const int64_t range = high - low;
                const int64_t x = ((value - low + 1) * positionCum[0] - 1) / range;
                int i = 1, j = N;
                while (i < j)
                {
                    const int k = (i + j) / 2;
                    if (positionCum[k] > x)
                        i = k + 1;
                    else
                        j = k;
                }
                const int pos = i - 1;
                high = low + (range * positionCum[pos]) / positionCum[0];
                low = low + (range * positionCum[pos + 1]) / positionCum[0];
                normalize();
                return pos;
            };

            std::vector<uint8_t> text(N, ' '), out;
            out.reserve(textSize);
            int r = N - F;
            while (out.size() < textSize && bitpos <= nbits + 64)
            {
                const int c = decodeChar();
                if (c < 256)
                {
                    out.push_back(static_cast<uint8_t>(c));
                    text[r] = static_cast<uint8_t>(c);
                    r = (r + 1) & (N - 1);
                }
                else
                {
                    const int i = (r - decodePosition() - 1) & (N - 1);
                    const int j = c - 255 + THRESHOLD;
                    for (int k = 0; k < j; ++k)
                    {
                        const uint8_t c2 = text[(i + k) & (N - 1)];
                        out.push_back(c2);
                        text[r] = c2;
                        r = (r + 1) & (N - 1);
                    }
                }
            }
            out.resize(std::min(out.size(), textSize));
            return out;
        }

        // ---------------------------------------------------------------------------------
        // The formats

        bool loadMax(const std::vector<uint8_t> &d, std::vector<Save> &out)
        {
            // "Ps2PowerSave", CRC, the directory name at 16, then at 80: compressed length (not
            // counting the length word at 88), the file count and the uncompressed length; the
            // LZARI stream from 92. Inside: per file a u32 length, a 32-byte name and the data,
            // the next entry at an offset where offset + 8 is a multiple of 16.
            Save save;
            save.directory = cstr(d, 16, 32);
            const uint32_t count = get32(d, 84), size = get32(d, 88);
            const std::vector<uint8_t> data = lzariDecode(d, 92, size);
            size_t o = 0;
            for (uint32_t i = 0; i < count && o + 36 <= data.size(); ++i)
            {
                const uint32_t length = get32(data, o);
                File f{cstr(data, o + 4, 32), slice(data, o + 36, length)};
                o += 36u + length;
                o = ((o + 8 + 15) / 16) * 16 - 8;
                save.files.push_back(std::move(f));
            }
            return keep(save, out);
        }

        bool loadPsu(const std::vector<uint8_t> &d, std::vector<Save> &out)
        {
            // 512-byte entries: mode u16, size u32 at 4, name at 0x40. The first is the directory
            // (its size = its entry count, with "." and ".."); each file's data follows its entry,
            // padded to 1024 bytes.
            Save save;
            save.directory = cstr(d, 0x40, 32);
            const uint32_t count = get32(d, 4);
            size_t o = 512;
            for (uint32_t i = 0; i < count && o + 512 <= d.size(); ++i)
            {
                const uint16_t mode = get16(d, o);
                const uint32_t size = get32(d, o + 4);
                const std::string name = cstr(d, o + 0x40, 32);
                o += 512;
                if (mode & 0x20)
                    continue; // "." and ".."
                save.files.push_back({name, slice(d, o, size)});
                o += (size + 1023u) / 1024u * 1024u;
            }
            return keep(save, out);
        }

        bool loadPsv(const std::vector<uint8_t> &d, std::vector<Save> &out)
        {
            // PS3 export of a PS2 save: type 2 at 0x3C, the PS2 header at 0x40 (file count at
            // +36), the directory info at 0x68 (name at +24), then 0x3C bytes per file (size at
            // +16, name at +24, data offset at +56).
            if (get32(d, 0x3C) != 2)
                return false;
            Save save;
            const uint32_t count = get32(d, 0x40 + 36);
            save.directory = cstr(d, 0x68 + 24, 32);
            for (uint32_t i = 0; i < count; ++i)
            {
                const size_t o = 0x68 + 0x38 + i * 0x3C;
                save.files.push_back({cstr(d, o + 24, 32), slice(d, get32(d, o + 56), get32(d, o + 16))});
            }
            return keep(save, out);
        }

        // The Sony memory card file system (PCSX2 .ps2 images): page 0 is the superblock; the FAT
        // is reached through the indirect FAT clusters (ifc_list); cluster numbers in directory
        // entries and the FAT are relative to alloc_offset; a FAT entry's bit 31 = in use, its
        // low 31 bits = the next cluster, 0x7FFFFFFF = the end. Directory entries are 512 bytes
        // (mode u16, length u32 at 4, cluster u32 at 0x10, name at 0x40).
        struct Card
        {
            const std::vector<uint8_t> &raw;
            uint32_t pageLength = 0, pagesPerCluster = 0, clusters = 0, allocOffset = 0, rootCluster = 0, stride = 0,
                     clusterSize = 0;
            std::array<uint32_t, 32> ifc{};

            explicit Card(const std::vector<uint8_t> &r) : raw(r)
            {
                pageLength = get16(raw, 0x28);
                pagesPerCluster = get16(raw, 0x2A);
                clusters = get32(raw, 0x30);
                allocOffset = get32(raw, 0x34);
                rootCluster = get32(raw, 0x3C);
                for (int i = 0; i < 32; ++i)
                    ifc[i] = get32(raw, 0x50 + 4 * i);
                const uint64_t pages = static_cast<uint64_t>(clusters) * pagesPerCluster;
                stride = raw.size() >= pages * (pageLength + 16u) ? pageLength + 16u : pageLength;
                clusterSize = pageLength * pagesPerCluster;
            }

            std::vector<uint8_t> cluster(uint32_t absolute) const
            {
                std::vector<uint8_t> out;
                for (uint32_t p = 0; p < pagesPerCluster; ++p)
                {
                    const size_t o = (static_cast<size_t>(absolute) * pagesPerCluster + p) * stride;
                    const auto part = slice(raw, o, pageLength);
                    out.insert(out.end(), part.begin(), part.end());
                }
                return out;
            }

            uint32_t fat(uint32_t relative) const
            {
                const uint32_t per = clusterSize / 4;
                const uint32_t n = relative / per;
                if (n / per >= ifc.size())
                    return 0;
                const auto indirect = cluster(ifc[n / per]);
                const auto table = cluster(get32(indirect, 4 * (n % per)));
                return get32(table, 4 * (relative % per));
            }

            std::vector<uint8_t> chain(uint32_t first, uint32_t length) const
            {
                std::vector<uint8_t> out;
                std::set<uint32_t> seen;
                for (uint32_t c = first; c != 0x7FFFFFFFu && !seen.count(c) && out.size() < length;)
                {
                    seen.insert(c);
                    const auto part = cluster(allocOffset + c);
                    out.insert(out.end(), part.begin(), part.end());
                    const uint32_t next = fat(c);
                    if (!(next & 0x80000000u))
                        break;
                    c = next & 0x7FFFFFFFu;
                }
                out.resize(std::min<size_t>(out.size(), length));
                return out;
            }

            struct Entry
            {
                std::string name;
                uint16_t mode;
                uint32_t length, cluster;
            };

            std::vector<Entry> entries(uint32_t first, uint32_t count) const
            {
                const auto blob = chain(first, count * 512u);
                std::vector<Entry> out;
                for (uint32_t i = 0; i < count && (i + 1) * 512u <= blob.size(); ++i)
                    out.push_back({cstr(blob, i * 512u + 0x40, 32), get16(blob, i * 512u), get32(blob, i * 512u + 4),
                                   get32(blob, i * 512u + 0x10)});
                return out;
            }
        };

        bool loadCard(const std::vector<uint8_t> &d, std::vector<Save> &out)
        {
            const Card card(d);
            if (card.pageLength == 0 || card.pagesPerCluster == 0)
                return false;
            const auto first = card.entries(card.rootCluster, 1);
            if (first.empty())
                return false;
            bool any = false;
            for (const auto &e : card.entries(card.rootCluster, first[0].length))
            {
                if (!(e.mode & 0x8000) || !(e.mode & 0x20) || e.name == "." || e.name == ".." || regionOf(e.name) == Region::Unknown)
                    continue;
                Save save;
                save.directory = e.name;
                for (const auto &f : card.entries(e.cluster, e.length))
                    if ((f.mode & 0x8000) && !(f.mode & 0x20))
                        save.files.push_back({f.name, card.chain(f.cluster, f.length)});
                any |= keep(save, out);
            }
            return any;
        }

        bool loadFolder(const fs::path &dir, std::vector<Save> &out)
        {
            // A save folder itself, or a card folder holding save folders.
            std::error_code ec;
            if (regionOf(dir.filename().string()) != Region::Unknown)
            {
                Save save;
                save.directory = dir.filename().string();
                for (const auto &item : fs::directory_iterator(dir, ec))
                    if (item.is_regular_file())
                        save.files.push_back({item.path().filename().string(), readAll(item.path())});
                return keep(save, out);
            }
            bool any = false;
            for (const auto &item : fs::directory_iterator(dir, ec))
                if (item.is_directory() && regionOf(item.path().filename().string()) != Region::Unknown)
                    any |= loadFolder(item.path(), out);
            return any;
        }

        // sceMcStDateTime (the card's dates): reserved, seconds, minutes, hours, day, month, year.
        std::array<uint8_t, 8> now()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            const int year = tm.tm_year + 1900;
            return {0, static_cast<uint8_t>(tm.tm_sec), static_cast<uint8_t>(tm.tm_min), static_cast<uint8_t>(tm.tm_hour),
                    static_cast<uint8_t>(tm.tm_mday), static_cast<uint8_t>(tm.tm_mon + 1), static_cast<uint8_t>(year & 0xFF),
                    static_cast<uint8_t>(year >> 8)};
        }
    }

    Region regionOf(const std::string &directory)
    {
        if (directory.find("SLUS-20668") != std::string::npos)
            return Region::Ntsc;
        if (directory.find("SLES-52388") != std::string::npos)
            return Region::Pal;
        return Region::Unknown;
    }

    const char *regionName(Region region)
    {
        switch (region)
        {
        case Region::Ntsc:
            return "NTSC (USA, SLUS-20668)";
        case Region::Pal:
            return "PAL (Europe, SLES-52388)";
        default:
            return "unknown";
        }
    }

    std::vector<Save> load(const fs::path &path, std::string &error)
    {
        std::vector<Save> out;
        std::error_code ec;
        if (fs::is_directory(path, ec))
        {
            if (!loadFolder(path, out))
                error = "no Transformers save in this folder";
            return out;
        }
        const std::vector<uint8_t> d = readAll(path);
        bool ok = false;
        if (d.size() >= 12 && std::memcmp(d.data(), "Ps2PowerSave", 12) == 0)
            ok = loadMax(d, out);
        else if (d.size() >= 28 && std::memcmp(d.data(), "Sony PS2 Memory Card Format ", 28) == 0)
            ok = loadCard(d, out);
        else if (d.size() >= 4 && std::memcmp(d.data(), "\0VSP", 4) == 0)
            ok = loadPsv(d, out);
        else if (d.size() >= 1024 && (get16(d, 0) & 0x20))
            ok = loadPsu(d, out);
        else
            error = "not a save format this reads (.psu, .max, .psv, .ps2 card, or a save folder)";
        if (!ok && error.empty())
            error = "no Transformers save (BASLUS-20668 or BESLES-52388) in this file";
        return out;
    }

    Save convert(const Save &save, Region to)
    {
        Save out = save;
        const std::string name = to == Region::Pal ? kPalDirectory : kNtscDirectory;
        for (File &f : out.files)
            if (f.name == save.directory)
                f.name = name;
        out.directory = name;
        out.region = to;
        return out;
    }

    std::vector<std::string> slotDates(const Save &save)
    {
        // Each slot's ~Date block: the name, then (from 11 bytes after the '~') u32 year and u16
        // month, day, hour, minute, second; 1666 marks an empty slot.
        std::vector<std::string> out;
        for (const File &f : save.files)
        {
            if (f.name != save.directory)
                continue;
            const auto &d = f.data;
            static const char kDate[] = "~Date";
            for (size_t i = 0; i + 21 < d.size(); ++i)
            {
                if (std::memcmp(&d[i], kDate, 5) != 0 || d[i + 5] != 0)
                    continue;
                const uint32_t year = get32(d, i + 11);
                if (year < 2000 || year > 2200)
                    continue; // (an empty slot)
                char text[32];
                std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", year, get16(d, i + 15), get16(d, i + 17),
                              get16(d, i + 19), get16(d, i + 21));
                out.emplace_back(text);
            }
        }
        return out;
    }

    bool writeFolder(const Save &save, const fs::path &parent, std::string &error)
    {
        std::error_code ec;
        const fs::path dir = parent / save.directory;
        fs::create_directories(dir, ec);
        for (const File &f : save.files)
        {
            std::ofstream out(dir / f.name, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(f.data.data()), static_cast<std::streamsize>(f.data.size()));
            if (!out)
            {
                error = "could not write " + (dir / f.name).string();
                return false;
            }
        }
        return true;
    }

    bool writePsu(const Save &save, const fs::path &file, std::string &error)
    {
        const auto stamp = now();
        std::vector<uint8_t> out;
        auto entry = [&](uint16_t mode, uint32_t size, const std::string &name)
        {
            std::vector<uint8_t> e(512, 0);
            e[0] = static_cast<uint8_t>(mode);
            e[1] = static_cast<uint8_t>(mode >> 8);
            for (int i = 0; i < 4; ++i)
                e[4 + i] = static_cast<uint8_t>(size >> (8 * i));
            std::copy(stamp.begin(), stamp.end(), e.begin() + 8);    // created
            std::copy(stamp.begin(), stamp.end(), e.begin() + 0x18); // modified
            std::copy(name.begin(), name.begin() + static_cast<long>(std::min<size_t>(name.size(), 32)), e.begin() + 0x40);
            out.insert(out.end(), e.begin(), e.end());
        };
        entry(0x8427, static_cast<uint32_t>(save.files.size() + 2), save.directory);
        entry(0x8427, 0, ".");
        entry(0x8427, 0, "..");
        for (const File &f : save.files)
        {
            entry(0x8497, static_cast<uint32_t>(f.data.size()), f.name);
            out.insert(out.end(), f.data.begin(), f.data.end());
            out.resize(out.size() + (1024 - f.data.size() % 1024) % 1024, 0);
        }
        std::ofstream o(file, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!o)
        {
            error = "could not write " + file.string();
            return false;
        }
        return true;
    }
}
