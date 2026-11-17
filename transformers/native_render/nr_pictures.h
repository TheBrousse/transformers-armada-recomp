// The pictures between the game's frames (nr_pictures.cpp, in the core - nr_core S3b; N9 "Smooth pictures between frames"): a
// frame drawn once for each refresh of the screen its moments cover, the poses blended for each picture's moment, each picture
// handed to the window at its time. What it needs of the host - the settings' answers and the camera turn's - comes through
// nr::PicturesHost.
#pragma once

#include "nr_types.h"

#include <cstdint>
#include <string>

namespace nr
{
    // Set once before the first frame (a question left null: the answer is no / 0).
    struct PicturesHost
    {
        bool (*posePictures)() = nullptr;   // the setting "Smooth pictures between frames"
        bool (*nativePictures)() = nullptr; // the window shows the native renderer's pictures, one a frame
        int (*pictureCap)() = nullptr;      // the frame rate cap as a cap of pictures (0: none)
        bool (*vsyncOff)() = nullptr;       // the settings' VSync off (the presents at once)
        int (*frameCap)() = nullptr;        // the settings' frame rate cap (0: Unlimited)
        bool (*cameraCut)(const view_turn::FrameCamera &fc) = nullptr; // the camera turn: the game's own cut at this frame
        bool (*turnable)(const view_turn::FrameCamera &fc) = nullptr;  // the camera turn: this frame's camera can be turned
        bool (*turnEnabled)() = nullptr;                               // the camera turn is on
        bool (*turnPicturesWanted)() = nullptr;                        // pictures of the camera turn wanted
    };
    void setPicturesHost(const PicturesHost &host);

    // A frame drawn (the render thread): its pictures, or the one picture (or the shot asked for).
    void renderPictures(Frame &frame, const std::string &shot);
    // This frame's own work before its pictures (its grass), ms: counted in the pictures' time.
    void notePicturesPreMs(double ms);
    // A frame left out (nr_queue.cpp): the next frame's pictures span from its moment.
    void picturesFrameLeftOut(const Frame &frame);
}
