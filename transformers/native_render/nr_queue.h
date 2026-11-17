// The frame queue (nr_queue.cpp, in the core - nr_core S3a): the frames the game hands over, read on one thread and drawn on
// another (TODO 161 a), the room the game waits for, the frames left out when the drawing is behind or over the cap, the
// drawing's times. What a frame's read and draw ARE stays the host's (the recomp: the display list read from guest memory, the
// grass - nr_thread.cpp; the pictures between frames: nr_pictures.cpp); it comes in through nr::FrameQueueHost.
#pragma once

#include "nr_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nr
{
    // A frame on its way through the queue.
    struct Job
    {
        Frame frame;
        std::string shot;
        double readMs = 0.0; // (TODO 161 a: its list read, the read thread)
        int64_t inputNs = 0, submitNs = 0, readStartNs = 0, readEndNs = 0; // (TODO 169: TF_NATIVE_LATLOG's steps, steady clock)
    };

    // What the queue needs of its host. Set once before the first frame (null: that part does nothing / gives 0).
    struct FrameQueueHost
    {
        void (*read)(Job &job) = nullptr;        // a frame's list read (the read thread)
        void (*draw)(Job &job) = nullptr;        // a frame drawn (the render thread)
        int (*pictureCap)() = nullptr;           // the frame rate cap as a cap of pictures (0: none)
        int64_t (*inputNs)() = nullptr;          // (TF_NATIVE_LATLOG) the moment of the last pad read, steady clock
    };
    void setFrameQueueHost(const FrameQueueHost &host);

    // A finished frame into the queue (the game's thread): waits while there is no room for it; the threads start with the first.
    void queueFrame(Frame &&frame, const std::string &shot);
    // (TODO 406) Right before a state is loaded, on the game's thread: the frames being read from guest memory read to the end
    // (the host's before-load hook; nr_install.h declares it for the recomp).
    void finishReadsBeforeLoad();
}
