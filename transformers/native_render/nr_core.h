// The native renderer's core (nr_core): the part that builds and runs without the PS2 runtime (no ps2_memory, ee_scheduler,
// gs/*, vu1, ps2_settings, raylib, view_turn, ps2_host_input, no guest addresses). Lane B's spec: runs\handover_2026-11-15\
// NR_CORE_SPEC.md. The recomp keeps its glue (nr_capture, nr_vif, nr_gif, the GS replay in nr_frame, ...) on top of it.
//
// S0 (this file): the build gate. The core's sources are compiled as the library nr_core (CMake: native_render/nr_core.cmake)
// with an include path that cannot see src/, decomp/ or the runtime, so a stray include fails at compile time. What the
// core needs from the outside comes in through small hooks the glue sets at start-up, declared here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nr_core
{
    // The threads the core starts say what they are; the PC layer decides where they run (the recomp: PS2X_THREAD_PLACEMENT,
    // ps2_thread_placement). Values mirror ps2_thread_placement::Role (Windows laptops' placement, TODO 290).
    enum class ThreadRole : int
    {
        Ee = 0,
        Vif1,
        GsFeed,
        RenderRead,
        Render,
        Helper, // HD texture uploads, sound mixer, movie decoder
        Count
    };

    using ThreadPlacer = void (*)(ThreadRole);

    // What the texture conversion (nr_texture.cpp) needs of its host: a main-memory pointer (guest RAM) and the GS upload skip's
    // bookkeeping (PS2Recomp's gsupload: the checks and the work that complete a texture's pages before they are read). A host
    // with neither leaves the hooks null: reads of guest memory give nothing, the checks see nothing stale.
    struct TextureHost
    {
        // The pointer to [addr] in guest RAM (null: not mapped). `ram` is the caller's base pointer, as the host gave it.
        const uint8_t *(*guestPtr)(const uint8_t *ram, uint32_t addr) = nullptr;
        // gsupload::ensureWhole(first, last): the uploads a skip left out that reach into the bytes written first.
        void (*ensureWhole)(uint64_t first, uint64_t last) = nullptr;
        // gsupload::mode() == Check
        bool (*checkMode)() = nullptr;
        // gsupload::staleAt(address, topByte)
        bool (*staleAt)(uint32_t address, bool topByte) = nullptr;
        // gsupload::reportRead(what, tbp, psm, staleBytes, bytes)
        void (*reportRead)(const char *what, uint32_t tbp, uint32_t psm, uint32_t staleBytes, uint32_t bytes) = nullptr;
    };
    void setTextureHost(const TextureHost &host);
    const TextureHost &textureHost();

    // What the HD texture pack (nr_hires.cpp) needs of its host: a PNG / image file as RGBA8 (the PC layer's image library),
    // the prompt-style questions (which controller / keyboard the game's button pictures should follow) and the developer's own
    // asset tree (the private build's Extracted folder, looked in last). Left null: no images load, the console's prompts show,
    // no developer tree.
    struct HiresHost
    {
        // An image as RGBA8 (R in the low byte); false when it does not load.
        bool (*loadImage)(const char *path, uint32_t &width, uint32_t &height, std::vector<uint32_t> &pixels) = nullptr;
        bool (*keyboardPromptsShown)() = nullptr; // key_text::symbolsAsKeys(ps2_host_input::keyboardPromptsShown())
        bool (*lastPadIsPlayStation)() = nullptr; // ps2_host_input::lastPadStyle() == PlayStation
        bool (*keyboardPrompts)() = nullptr;      // ps2_host_input::keyboardPrompts()
        bool (*xbox360Pads)() = nullptr;          // ps2_host_input::xbox360Pads()
        const char *(*applicationDirectory)() = nullptr; // the folder of the executable (raylib's GetApplicationDirectory)
        std::string devTree;                      // the developer's asset tree (empty: none)
    };
    void setHiresHost(const HiresHost &host);
    const HiresHost &hiresHost();

    // Set once by the host before the core starts a thread (null: no placement).
    void setThreadPlacer(ThreadPlacer placer);
    // From the thread itself, as it starts.
    void placeThisThread(ThreadRole role);

    // ---- S2a: opening the core and its settings (NR_CORE_SPEC.md 2), a frame's in and out (3, 4), the window's events (8) ----
    // The graphics API: the default is Direct3D 12 where the build has it (else none: the host draws); Vulkan is in development.
    enum class Api : int
    {
        Default = 0,
        Vulkan = 1
    };
    // The core's settings, a plain struct in place of the recomp's ps2_settings. A field left at -1 is not changed.
    struct Settings
    {
        int api = -1;            // Api: open() only (chosen once); -1 = the TF_GFX_API environment variable, as before
        int antiAliasing = -1;   // the 3D's edges: 0 off, 1 FXAA, 2 SMAA 1x
        int supersampling = -1;  // 1: the 3D drawn at 1.5 times the window's size each way and averaged down
        int speedupsForced = -1; // the picture-keeping speed-ups forced off (0) or on (1) - a testing aid (TODO 94)
        int windowLines = -1;    // the picture drawn at the window's own lines (Windows)
        int windowWidth = -1, windowHeight = -1; // (Linux: the window's size)
        int sharedPictures = -1; // 1: the pictures handed to the window as shared GPU textures, 0: through the CPU
    };
    // Chooses the API (once: the first call or the first use of the backend decides) and starts preparing the device, its
    // shaders and pipelines on a thread of their own; then the settings.
    void open(const Settings &settings);
    // The settings' fields that are not -1, through the backend (any thread; from the next frame).
    void applySettings(const Settings &settings);
    // A frame as the host hands it over (S3 on: the core's frame queue) and its picture as it comes out (the swap chain's side,
    // S5: the core presenting its own window). Declared here so the hosts can be written against them.
    struct FrameInfo
    {
        uint64_t tick = 0;                          // the frame's moment (the recomp: its VBlank)
        uint32_t pictureWidth = 0, pictureHeight = 0; // the picture's size
    };
    struct FrameEnd
    {
        uint32_t pictureTarget = 0;   // the target the frame's picture is
        int displayRect[4] = {};      // x, y, width, height of it shown
        float aspect = 0.0f;          // the shape it is shown at (width / height)
    };
    // The window's events to the PC layer (resize, focus, close); the core reads no input itself. Left null: not told.
    struct WindowEvents
    {
        void (*resized)(int width, int height) = nullptr;
        void (*focus)(bool focused) = nullptr;
        void (*closed)() = nullptr;
    };
    void setWindowEvents(const WindowEvents &events);
    const WindowEvents &windowEvents();

    // (S2b) What the pacing log (nr_pacelog.cpp) needs of its host: the screen's refresh timing (the latest refresh and the
    // period, steady-clock nanoseconds; false: not known), whether the refresh is variable, the view turn's own figures for the
    // log line and each picture shown to it (N9), the hand-over's gate holds and cap drops since the last line (TODO 336).
    // Left null: those parts of the line are empty or zero.
    struct PresentHost
    {
        bool (*screenTiming)(int64_t &refreshNs, int64_t &periodNs) = nullptr;
        bool (*screenVariable)() = nullptr;
        void (*appendTurnPacing)(char *text, size_t size) = nullptr;
        void (*noteShownPicture)(uint64_t turnId, int64_t shownNs, int64_t dueNs) = nullptr;
        void (*takeGateHolds)(unsigned &pictures, double &heldMs, double &slicesMs) = nullptr;
        unsigned (*takeCapDrops)() = nullptr;
    };
    void setPresentHost(const PresentHost &host);
    const PresentHost &presentHost();
}
