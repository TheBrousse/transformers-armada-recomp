#pragma once
// HD movies: upscaled versions of the game's movies, shown over its picture while it plays them
// (hd_movies.cpp has the long form). TF_HD_MOVIES=<folder> ("movies_hd"), TF_FFMPEG=<ffmpeg.exe>.

#include <string>

class PS2Runtime;

namespace hd_movies
{
    // From the movie stream's creation and destruction (overrides.cpp, the game thread).
    void movieStarted(PS2Runtime &runtime, const std::string &name);
    void movieEnded();
    // Once a window frame, over the game's picture (main.cpp).
    void draw(PS2Runtime &runtime);
    bool playing();
    // The movie's name under FMV without its extension ("CG/EP01/N1_1"), and its upscaled file.
    std::string movieKey(std::string name);
    std::string fileFor(const std::string &key);
}
