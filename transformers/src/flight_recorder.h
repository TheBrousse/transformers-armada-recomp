#pragma once
// (TODO 405) The flight recorder, TF_FLIGHT_RECORDER=1 (off by default; for playtests): the last ~20 s of the game kept
// in memory, one entry a game frame (nothing written while nothing happens), and written to runs\flight_<time>.log when
// a detector fires (the camera's boom jumping out, the camera's collision hitting and missing by turns, the eye leaping
// for one frame, a burst of stand-in textures or of texture uses put off) or when the player presses F10 - which also
// saves a state of that moment (runs\flight_<time>.ps2state). flight_recorder.cpp.

#include <cstdint>

class PS2Runtime;

namespace flight_recorder
{
    // At start-up (game thread): reads TF_FLIGHT_RECORDER and its knobs, makes the rings.
    void install(PS2Runtime &runtime);
    bool enabled();

    // Game thread, from the camera's boom (0x2CA0B0's wrapper, overrides.cpp): this frame's camera.
    struct Camera
    {
        float eye[3], target[3];
        float boomBefore, boomGame, boom; // (+0x31C before, as the game left it, as shown - after the release)
        float wanted, sweep;              // (the scripts' eye and the first sweep's point, from the target)
        uint32_t sceneHits, modelHits;    // (the sweep's snapshot: scene triangles and models hit)
        uint8_t arrived329, cut32A, held;
    };
    void camera(const Camera &c);
    // Game thread: the camera collision's sweep radius and field of view (for the dump's header).
    void noteRadius(float radius);
    void noteFieldOfView(float degrees);
    // Game thread: the ground under the camera's eye as the game's own check found it (0x2C9F10's probe: the height of
    // the surface below; `found` false = none within its reach), and how far the check lifted the eye (0: not at all).
    void noteGround(bool found, float groundZ, float lifted);
    // Any thread (view_turn, each picture): the eye a picture shows after its turn (z) and the turn (degrees).
    void notePicture(float eyeZ, float yawDegrees, float pitchDegrees);
    // Game thread, the camera hook: the mouse's travel taken this frame (pixels).
    void noteMouse(float x, float y);
    // Game thread, every frame (frameKick): closes the frame's entry and runs the detectors.
    void frame(uint8_t *rdram, PS2Runtime &runtime, uint64_t tick, const float player[3]);
    // Render thread, each frame drawn: the native renderer's counts for it and a signature of what it drew.
    void renderFrame(uint64_t tick, uint32_t newTextures, uint32_t putOff, uint32_t standIns, uint32_t draws, uint64_t signature);
    // Window thread, every window frame: the F10 key.
    void windowFrame();
}
