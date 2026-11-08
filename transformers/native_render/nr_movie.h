// Roadmap N6: the movies' pictures drawn by the native renderer straight from the decoder (nr_movie.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

class PS2Runtime;

namespace nr
{
    struct Texture;
    struct TextureKey;

    namespace movie
    {
        // From nr::install (the native renderer on): the decoder's pictures handed over (TF_NATIVE_MOVIES).
        void install(PS2Runtime &runtime);

        // nr_capture.cpp convertKey (game thread): whether the level whose texels are at `data` (the level's data
        // pointer) of the vlTexturePS2 `texture` holds a movie's picture drawn from the decoder; `generation` then
        // names that picture (never 0, a new one for each picture).
        bool holds(uint32_t texture, uint32_t data, uint64_t &generation);
        // The picture of that level as a texture of the key's size (the picture's own texels at the top left, its
        // last row and column held past them - the texture's power-of-two size); null if it is gone. With
        // TF_NATIVE_MOVIES=check also compared with the conversion of the game's copy (rdram).
        std::shared_ptr<const Texture> texture(const uint8_t *rdram, const TextureKey &key, uint32_t data, uint64_t id);
        // nr_texture.cpp rememberLevel: the game's copy of a picture drawn from the decoder is not kept (never
        // written: TF_NATIVE_MOVIES=1).
        bool skipsSnapshot(uint32_t data);
        // nr_capture.cpp's vlTexturePS2 destructor: the movie whose texture it was has ended (the game keeps its sceMpeg
        // object from one movie to the next) - its picture no longer drawn, the movie's log line written.
        void textureGone(uint32_t texture);
        // (TF_NATIVE_MOVIELOG, the old path) The time a movie picture's conversion from the game's copy took.
        void oldConversion(uint32_t data, double ms);
        // (TODO 159) The HQ's level preview now playing (overrides.cpp, the game thread, at its stream's creation): its
        // upscaled file (tools/upscale_previews.py; empty: none - the game's pictures as they are), drawn picture for
        // picture in place of the game's; and its stream's end.
        void previewStarted(const std::string &key, const std::string &hdFile);
        void previewEnded();
    }
}
