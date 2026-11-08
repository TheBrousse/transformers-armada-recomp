#pragma once
// HD movies: an upscaled movie decoded to raw RGBA frames by an ffmpeg process (hd_movies_decoder.cpp).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hd_movies_decoder
{
    struct Frame
    {
        double time = 0.0; // seconds from the movie's start
        std::vector<uint32_t> rgba; // width x height RGBA8 texels (one word each: a texture's own layout - TODO 159 moves it in)
    };

    // The movie's width at that height and its frame rate (ffprobe beside TF_FFMPEG).
    bool probe(const std::string &file, int height, int &width, double &fps);

    // One movie decoding: its own ffmpeg process and reader thread (TODO 159: the HQ's level previews have one of their
    // own, beside the full-window HD movies' - nr_movie.cpp).
    class Decoder
    {
    public:
        Decoder();
        ~Decoder();
        Decoder(const Decoder &) = delete;
        Decoder &operator=(const Decoder &) = delete;
        // Decoding from the start, at that size; frames are taken as their time comes.
        bool start(const std::string &file, int width, int height, double fps);
        void stop();
        // The newest frame whose time has come (false: none new since).
        bool take(double now, Frame &frame);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // The full-window HD movies' decoder (hd_movies.cpp).
    bool start(const std::string &file, int width, int height, double fps);
    void stop();
    bool take(double now, Frame &frame);
}
