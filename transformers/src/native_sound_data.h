#pragma once
// The sound data the native sound server reads itself (native_sound_data.cpp): the disc's
// front-directory zips (SOUNDE.ZIP: the tone libraries and the music albums), tone libraries
// (.TLB text + .VAC, .TLA binary FISH), albums (.ALB_T) and .MS stream headers, and PS-ADPCM.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace native_sound::data
{
    // A disc archive: PKZIP with the central directory first. Entries by upper-case name.
    class Archive
    {
    public:
        bool open(const std::filesystem::path &path);
        // A folder of loose files in the archive's place (a mod's files the port reads itself).
        bool openFolder(const std::filesystem::path &folder);
        bool read(const std::string &name, std::vector<uint8_t> &out) const; // (any case, '/' or '\\')
        bool isOpen() const { return !m_entries.empty() || !m_folder.empty(); }

    private:
        struct Entry
        {
            uint32_t offset = 0, size = 0;
            uint16_t method = 0;
        };
        std::filesystem::path m_path, m_folder;
        std::map<std::string, Entry> m_entries;
    };

    std::string normalise(std::string name); // upper case, '/' separators, no leading "./" or "/"

    // An amplitude envelope in seconds (an instrument's ~ADSR).
    struct Envelope
    {
        float delay = 0.0f, attack = 0.0f, decay = 0.0f, sustain = 1.0f, release = 0.0f;
        bool given = false;
    };

    // A tone: mono PCM decoded from its SPU ADPCM, with its loop.
    struct Tone
    {
        std::string name;
        uint32_t sampleRate = 44100;
        int rootNote = 60;
        bool loop = false;
        std::vector<int16_t> pcm;
        uint32_t loopStart = 0; // sample index the loop goes back to (when looping)
    };

    struct Region
    {
        std::string toneName;
        int rootNote = 60, noteBegin = 0, noteEnd = 127, velocityBegin = 0, velocityEnd = 127;
    };

    struct Instrument
    {
        std::string name, toneName;
        std::vector<Region> regions;
        Envelope envelope;
    };

    struct ToneLibrary
    {
        std::string path;
        std::vector<Tone> tones;
        std::vector<Instrument> instruments;
        const Tone *tone(const std::string &name) const;
        const Instrument *instrument(const std::string &name) const;
    };

    // Loads a .TLB (with its .VAC beside it) or a .TLA from the archive. False on a failure.
    bool loadToneLibrary(const Archive &archive, const std::string &path, ToneLibrary &out);

    struct Album
    {
        struct Part
        {
            std::string file;
            int repeatCount = 1; // 9999 = for ever
        };
        struct Tune
        {
            std::string name;
            std::vector<Part> parts;
        };
        std::string name;
        std::vector<Tune> tunes;
    };
    bool loadAlbum(const std::vector<uint8_t> &text, Album &out);

    // An .MS stream file's header (SL3\0MS2\0).
    struct StreamHeader
    {
        uint32_t channels = 0, sampleRate = 0, samples = 0, interleave = 0, dataOffset = 0;
    };
    bool readStreamHeader(const uint8_t *header, size_t size, StreamHeader &out);

    // PS-ADPCM, one 16-byte block to 28 samples, with the two-sample history (the unclamped
    // values, as the hardware keeps them). Returns the block's flag byte.
    uint8_t decodeBlock(const uint8_t *block, int16_t *out, int32_t &history1, int32_t &history2);
}
