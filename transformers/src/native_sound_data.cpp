// The native sound server's data (native_sound_data.h).
//
// Formats (decomp Claude's Sirens report §3; the remaster's Tools/audio/tonebank_to_wav.py, which
// decodes them byte-exactly):
//   .TLB  text FISH: ~ToneLibrary { Datafile "x.vac" ~Tone { Name Offset SampleRate Loop RootNote }
//         ~Instrument { Name Tone ~Region { ToneName RootNote NoteRange* VelocityRange* }
//         ~AmplitudeEnvelope { ~ADSR { DelayTime AttackTime DecayTime SustainLevel ReleaseTime } } } }
//   .VAC  headerless SPU ADPCM; a tone's Offset is a byte offset into it.
//   .TLA  binary FISH of the same tree, the ADPCM embedded in ~SoundData (u32 size, then the data).
//         A node: '~' type '\0', a flag byte (1: u32 length + name follow), u32 size (counting
//         itself), then the payload, in which child nodes follow the node's own fields.
//         ~ToneLibrary: u32 version, u32 tones, u32 length + Datafile name.
//         ~Tone: u32 version, u32 length + name, u32 offset, u32 rate, u8 loop, u8 root note.
//         ~Instrument: u32 version, u32 length + name, u32 length + tone, ...; children ~Region
//         (u32 version, u32 length + tone name, u32 root note, note begin, note end, velocity
//         begin, velocity end) and ~AmplitudeEnvelope > ~ADSR (5 floats).
//   .ALB_T text FISH: ~Album { Name ~Tune { Name ~Part { Filename RepeatCount } } }.
//   PS-ADPCM: block = (filter << 4 | shift), flags, 14 bytes of 4-bit samples; filters
//   (0,0) (60,0) (115,-52) (98,-55) (122,-60) >> 6; flags bit 0 = end, bit 1 = repeat, bit 2 =
//   loop start.

#include "native_sound_data.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

namespace native_sound::data
{
    namespace
    {
        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
        uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        float lef(const uint8_t *p)
        {
            const uint32_t v = le32(p);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }

        // ---- text FISH ----------------------------------------------------------------
        struct TextNode
        {
            std::string type;
            std::map<std::string, std::string> values;
            std::vector<TextNode> children;
            std::string get(const std::string &key, const std::string &def = "") const
            {
                auto it = values.find(key);
                return it == values.end() ? def : it->second;
            }
            int geti(const std::string &key, int def = 0) const
            {
                auto it = values.find(key);
                return it == values.end() ? def : std::atoi(it->second.c_str());
            }
            float getf(const std::string &key, float def = 0.0f) const
            {
                auto it = values.find(key);
                return it == values.end() ? def : static_cast<float>(std::atof(it->second.c_str()));
            }
        };

        // Tokens: ~Type, Key:, "string", {, }, bare words; // comments to the end of the line.
        std::vector<std::string> tokens(const std::string &text)
        {
            std::vector<std::string> out;
            size_t i = 0;
            while (i < text.size())
            {
                const char c = text[i];
                if (std::isspace(static_cast<unsigned char>(c)))
                {
                    ++i;
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
                {
                    while (i < text.size() && text[i] != '\n')
                        ++i;
                    continue;
                }
                if (c == '{' || c == '}')
                {
                    out.emplace_back(1, c);
                    ++i;
                    continue;
                }
                if (c == '"')
                {
                    size_t j = i + 1;
                    while (j < text.size() && text[j] != '"')
                        ++j;
                    out.push_back("\"" + text.substr(i + 1, j - i - 1));
                    i = j + 1;
                    continue;
                }
                size_t j = i;
                while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) && text[j] != '{' && text[j] != '}' && text[j] != '"')
                    ++j;
                out.push_back(text.substr(i, j - i));
                i = j;
            }
            return out;
        }

        void parseBlock(const std::vector<std::string> &t, size_t &pos, TextNode &node)
        {
            if (pos < t.size() && t[pos] == "{")
                ++pos;
            while (pos < t.size() && t[pos] != "}")
            {
                const std::string &tok = t[pos];
                if (tok.size() > 1 && tok[0] == '~')
                {
                    ++pos;
                    TextNode child;
                    child.type = tok.substr(1);
                    if (pos < t.size() && t[pos] == "{")
                        parseBlock(t, pos, child);
                    node.children.push_back(std::move(child));
                }
                else if (!tok.empty() && tok.back() == ':')
                {
                    const std::string key = tok.substr(0, tok.size() - 1);
                    ++pos;
                    std::string value = pos < t.size() ? t[pos] : "";
                    if (!value.empty() && value[0] == '"')
                        value.erase(0, 1);
                    node.values[key] = value;
                    ++pos;
                }
                else
                    ++pos;
            }
            if (pos < t.size())
                ++pos;
        }

        bool parseText(const std::vector<uint8_t> &bytes, TextNode &root)
        {
            const std::vector<std::string> t = tokens(std::string(bytes.begin(), bytes.end()));
            size_t pos = 0;
            while (pos < t.size() && !(t[pos].size() > 1 && t[pos][0] == '~'))
                ++pos;
            if (pos >= t.size())
                return false;
            root.type = t[pos].substr(1);
            ++pos;
            parseBlock(t, pos, root);
            return true;
        }

        // ---- binary FISH ----------------------------------------------------------------
        struct BinNode
        {
            std::string type;
            size_t start = 0, end = 0; // the payload
            std::vector<BinNode> children;
        };

        bool header(const std::vector<uint8_t> &d, size_t pos, size_t limit, BinNode &node, size_t &payload)
        {
            if (pos + 3 > limit || d[pos] != '~')
                return false;
            size_t e = pos + 1;
            while (e < limit && e < pos + 48 && d[e] != 0)
                ++e;
            if (e >= limit || d[e] != 0 || e == pos + 1)
                return false;
            for (size_t i = pos + 1; i < e; ++i)
                if (!(std::isalnum(d[i]) || d[i] == '_'))
                    return false;
            size_t p = e + 1;
            if (p >= limit)
                return false;
            const uint8_t flag = d[p++];
            if (flag == 1)
            {
                if (p + 4 > limit)
                    return false;
                const uint32_t n = le32(&d[p]);
                if (n > 256 || p + 4 + n > limit)
                    return false;
                p += 4 + n;
            }
            else if (flag != 0)
                return false;
            if (p + 4 > limit)
                return false;
            const uint32_t size = le32(&d[p]);
            if (size < 4 || p + size > limit)
                return false;
            node.type.assign(reinterpret_cast<const char *>(&d[pos + 1]), e - pos - 1);
            node.start = p + 4;
            node.end = p + size;
            payload = p + 4;
            return true;
        }

        void children(const std::vector<uint8_t> &d, size_t start, size_t end, std::vector<BinNode> &out, int depth)
        {
            for (size_t pos = start; pos < end;)
            {
                BinNode n;
                size_t payload = 0;
                if (d[pos] == '~' && header(d, pos, end, n, payload))
                {
                    if (depth < 8 && n.type != "SoundData")
                        children(d, payload, n.end, n.children, depth + 1);
                    pos = n.end;
                    out.push_back(std::move(n));
                    continue;
                }
                ++pos;
            }
        }

        std::string named(const std::vector<uint8_t> &d, size_t &p) // u32 length + bytes
        {
            if (p + 4 > d.size())
                return "";
            const uint32_t n = le32(&d[p]);
            p += 4;
            if (p + n > d.size())
                return "";
            std::string s(reinterpret_cast<const char *>(&d[p]), n);
            p += n;
            return s;
        }

        // ---- tones ---------------------------------------------------------------------
        // Decodes a tone's ADPCM up to the block with the end flag (included). The loop start is
        // the last block with the loop-start flag (else the start).
        void decodeTone(const uint8_t *body, size_t size, Tone &tone)
        {
            int32_t h1 = 0, h2 = 0;
            const size_t blocks = size / 16u;
            tone.pcm.reserve(blocks * 28u);
            tone.loopStart = 0;
            for (size_t b = 0; b < blocks; ++b)
            {
                int16_t samples[28];
                const uint8_t flags = decodeBlock(body + b * 16u, samples, h1, h2);
                if ((flags & 4u) != 0u)
                    tone.loopStart = static_cast<uint32_t>(tone.pcm.size());
                tone.pcm.insert(tone.pcm.end(), samples, samples + 28);
                if ((flags & 1u) != 0u)
                {
                    if ((flags & 2u) != 0u)
                        tone.loop = true; // end + repeat: the data loops
                    break;
                }
            }
        }

        void sliceTones(std::vector<Tone> &tones, std::vector<std::pair<uint32_t, size_t>> &order, const uint8_t *audio, size_t audioSize)
        {
            std::sort(order.begin(), order.end());
            for (size_t i = 0; i < order.size(); ++i)
            {
                const uint32_t start = order[i].first;
                const uint32_t end = i + 1 < order.size() ? order[i + 1].first : static_cast<uint32_t>(audioSize);
                if (start < audioSize && end > start)
                    decodeTone(audio + start, std::min<size_t>(end, audioSize) - start, tones[order[i].second]);
            }
        }

        bool loadTlb(const Archive &archive, const std::string &path, const std::vector<uint8_t> &text, ToneLibrary &out)
        {
            TextNode root;
            if (!parseText(text, root) || root.type != "ToneLibrary")
                return false;
            std::string dir = normalise(path);
            dir = dir.substr(0, dir.find_last_of('/') + 1);
            std::vector<uint8_t> vac;
            if (!archive.read(dir + root.get("Datafile"), vac))
                return false;
            std::vector<std::pair<uint32_t, size_t>> order;
            for (const TextNode &c : root.children)
            {
                if (c.type == "Tone")
                {
                    Tone t;
                    t.name = c.get("Name");
                    t.sampleRate = static_cast<uint32_t>(c.geti("SampleRate", 22050));
                    t.rootNote = c.geti("RootNote", 60);
                    t.loop = c.get("Loop") == "true";
                    order.emplace_back(static_cast<uint32_t>(c.geti("Offset")), out.tones.size());
                    out.tones.push_back(std::move(t));
                }
                else if (c.type == "Instrument")
                {
                    Instrument in;
                    in.name = c.get("Name");
                    in.toneName = c.get("Tone");
                    for (const TextNode &s : c.children)
                    {
                        if (s.type == "Region")
                        {
                            Region r;
                            r.toneName = s.get("ToneName");
                            r.rootNote = s.geti("RootNote", 60);
                            r.noteBegin = s.geti("NoteRangeBegin", 0);
                            r.noteEnd = s.geti("NoteRangeEnd", 127);
                            r.velocityBegin = s.geti("VelocityRangeBegin", 0);
                            r.velocityEnd = s.geti("VelocityRangeEnd", 127);
                            in.regions.push_back(r);
                        }
                        else if (s.type == "AmplitudeEnvelope")
                            for (const TextNode &a : s.children)
                                if (a.type == "ADSR")
                                {
                                    in.envelope.delay = a.getf("DelayTime");
                                    in.envelope.attack = a.getf("AttackTime");
                                    in.envelope.decay = a.getf("DecayTime");
                                    in.envelope.sustain = a.getf("SustainLevel", 1.0f);
                                    in.envelope.release = a.getf("ReleaseTime");
                                    in.envelope.given = true;
                                }
                    }
                    out.instruments.push_back(std::move(in));
                }
            }
            sliceTones(out.tones, order, vac.data(), vac.size());
            return true;
        }

        bool loadTla(const std::vector<uint8_t> &d, ToneLibrary &out)
        {
            if (d.size() < 8 || std::memcmp(d.data(), "FISH", 4) != 0)
                return false;
            std::vector<BinNode> top;
            children(d, 4, d.size(), top, 0);
            const BinNode *lib = nullptr;
            for (const BinNode &n : top)
                if (n.type == "ToneLibrary")
                    lib = &n;
            if (!lib)
                return false;
            const BinNode *sound = nullptr;
            for (const BinNode &n : lib->children)
                if (n.type == "SoundData")
                    sound = &n;
            if (!sound || sound->start + 4 > sound->end)
                return false;
            const size_t audioBase = sound->start + 4;
            const size_t audioSize = sound->end - audioBase;
            std::vector<std::pair<uint32_t, size_t>> order;
            for (const BinNode &n : lib->children)
            {
                if (n.type == "Tone")
                {
                    size_t p = n.start + 4; // (version)
                    Tone t;
                    t.name = named(d, p);
                    if (p + 10 > d.size())
                        continue;
                    const uint32_t offset = le32(&d[p]);
                    t.sampleRate = le32(&d[p + 4]);
                    t.loop = d[p + 8] != 0;
                    t.rootNote = d[p + 9];
                    order.emplace_back(offset, out.tones.size());
                    out.tones.push_back(std::move(t));
                }
                else if (n.type == "Instrument")
                {
                    size_t p = n.start + 4;
                    Instrument in;
                    in.name = named(d, p);
                    in.toneName = named(d, p);
                    for (const BinNode &c : n.children)
                    {
                        if (c.type == "Region")
                        {
                            size_t q = c.start + 4;
                            Region r;
                            r.toneName = named(d, q);
                            if (q + 20 <= d.size())
                            {
                                r.rootNote = static_cast<int>(le32(&d[q]));
                                r.noteBegin = static_cast<int>(le32(&d[q + 4]));
                                r.noteEnd = static_cast<int>(le32(&d[q + 8]));
                                r.velocityBegin = static_cast<int>(le32(&d[q + 12]));
                                r.velocityEnd = static_cast<int>(le32(&d[q + 16]));
                            }
                            in.regions.push_back(r);
                        }
                        else if (c.type == "AmplitudeEnvelope")
                            for (const BinNode &a : c.children)
                                if (a.type == "ADSR" && a.start + 20 <= d.size())
                                {
                                    in.envelope.delay = lef(&d[a.start]);
                                    in.envelope.attack = lef(&d[a.start + 4]);
                                    in.envelope.decay = lef(&d[a.start + 8]);
                                    in.envelope.sustain = lef(&d[a.start + 12]);
                                    in.envelope.release = lef(&d[a.start + 16]);
                                    in.envelope.given = true;
                                }
                    }
                    out.instruments.push_back(std::move(in));
                }
            }
            sliceTones(out.tones, order, d.data() + audioBase, audioSize);
            return true;
        }
    }

    std::string normalise(std::string name)
    {
        for (char &c : name)
        {
            if (c == '\\')
                c = '/';
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        while (name.rfind("./", 0) == 0)
            name.erase(0, 2);
        while (!name.empty() && name[0] == '/')
            name.erase(0, 1);
        return name;
    }

    bool Archive::open(const std::filesystem::path &path)
    {
        m_entries.clear();
        m_path = path;
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        // The central directory: records "PK\1\2" from offset 0.
        std::vector<uint8_t> head(1u << 20);
        f.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(f.gcount()));
        size_t p = 0;
        while (p + 46 <= head.size() && std::memcmp(&head[p], "PK\x01\x02", 4) == 0)
        {
            const uint16_t method = le16(&head[p + 10]);
            const uint32_t size = le32(&head[p + 20]);
            const uint16_t nl = le16(&head[p + 28]), el = le16(&head[p + 30]), cl = le16(&head[p + 32]);
            const uint32_t local = le32(&head[p + 42]);
            if (p + 46 + nl > head.size())
                break;
            const std::string name(reinterpret_cast<const char *>(&head[p + 46]), nl);
            Entry e;
            e.offset = local;
            e.size = size;
            e.method = method;
            m_entries[normalise(name)] = e;
            p += 46u + nl + el + cl;
        }
        return !m_entries.empty();
    }

    bool Archive::openFolder(const std::filesystem::path &folder)
    {
        m_entries.clear();
        std::error_code ec;
        m_folder = std::filesystem::is_directory(folder, ec) ? folder : std::filesystem::path();
        return !m_folder.empty();
    }

    bool Archive::read(const std::string &name, std::vector<uint8_t> &out) const
    {
        if (!m_folder.empty())
        {
            std::ifstream f(m_folder / std::filesystem::path(normalise(name)).make_preferred(), std::ios::binary); // (case-blind on Windows)
            if (!f)
                return false;
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return true;
        }
        auto it = m_entries.find(normalise(name));
        if (it == m_entries.end() || it->second.method != 0)
            return false; // (the sound archive is stored)
        std::ifstream f(m_path, std::ios::binary);
        uint8_t local[30];
        f.seekg(it->second.offset);
        if (!f.read(reinterpret_cast<char *>(local), 30) || std::memcmp(local, "PK\x03\x04", 4) != 0)
            return false;
        f.seekg(it->second.offset + 30u + le16(local + 26) + le16(local + 28));
        out.resize(it->second.size);
        return static_cast<bool>(f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size())));
    }

    const Tone *ToneLibrary::tone(const std::string &name) const
    {
        for (const Tone &t : tones)
            if (t.name == name)
                return &t;
        return nullptr;
    }

    const Instrument *ToneLibrary::instrument(const std::string &name) const
    {
        for (const Instrument &i : instruments)
            if (i.name == name)
                return &i;
        return nullptr;
    }

    bool loadToneLibrary(const Archive &archive, const std::string &path, ToneLibrary &out)
    {
        std::vector<uint8_t> bytes;
        if (!archive.read(path, bytes))
            return false;
        out.path = path;
        const std::string n = normalise(path);
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".TLA") == 0)
            return loadTla(bytes, out);
        return loadTlb(archive, path, bytes, out);
    }

    bool loadAlbum(const std::vector<uint8_t> &text, Album &out)
    {
        // (out starts empty: a stream re-opened with another album used to keep the first one's
        // tunes in front - every boss played the first boss's music, TODO 85)
        out = Album{};
        TextNode root;
        if (!parseText(text, root) || root.type != "Album")
            return false;
        out.name = root.get("Name");
        for (const TextNode &t : root.children)
        {
            if (t.type != "Tune")
                continue;
            Album::Tune tune;
            tune.name = t.get("Name");
            for (const TextNode &p : t.children)
                if (p.type == "Part")
                    tune.parts.push_back({p.get("Filename"), p.geti("RepeatCount", 1)});
            out.tunes.push_back(std::move(tune));
        }
        return true;
    }

    bool readStreamHeader(const uint8_t *h, size_t size, StreamHeader &out)
    {
        if (size < 0x28 || std::memcmp(h, "SL3\0MS2\0", 8) != 0)
            return false;
        out.channels = le32(h + 0x14);
        out.sampleRate = le32(h + 0x18);
        out.samples = le32(h + 0x1C);
        out.interleave = le32(h + 0x20);
        out.dataOffset = le32(h + 0x24);
        return out.channels >= 1 && out.channels <= 8 && out.sampleRate && out.interleave;
    }

    uint8_t decodeBlock(const uint8_t *block, int16_t *out, int32_t &h1, int32_t &h2)
    {
        static const int kPos[5] = {0, 60, 115, 98, 122};
        static const int kNeg[5] = {0, 0, -52, -55, -60};
        int filter = block[0] >> 4;
        const int shift = block[0] & 0x0F;
        if (filter > 4)
            filter = 0;
        for (int i = 0; i < 28; ++i)
        {
            const int nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 0x0F;
            int32_t s = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12));
            s >>= shift;
            const int32_t sample = s + ((kPos[filter] * h1 + kNeg[filter] * h2) >> 6);
            h2 = h1;
            h1 = sample;
            out[i] = static_cast<int16_t>(std::clamp<int32_t>(sample, -32768, 32767));
        }
        return block[1];
    }
}
