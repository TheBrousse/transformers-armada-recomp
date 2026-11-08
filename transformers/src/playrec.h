#pragma once
// The play recorder's hooks: the points where the game is given its input (the pad, the mouse look, the pause
// menu's selections, F4) and where the port decides by the PC's clock (the frame-rate ladder, the forest's reach).
// The private overlay's playrec.cpp records and replays them (play recordings, F2); without it playrec_none.cpp
// stands in: nothing is recorded and every channel gives the game what it was given.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class PS2Runtime;

namespace playrec
{
    // main.cpp: before settings.ini is read (a replay's environment), after it (recording armed: VIF1 on the
    // game's thread), once the memory card's folder is known (a boot recording copies it), and at the end.
    void prepareReplay();
    void prepareRecord();
    void beforeRun();
    void finish(const char *why);
    // The mods this start applied (mod_loader.h): kept in a recording, compared in a replay.
    void noteMods(const std::vector<std::string> &mods);
    // The settings panel's switch (TF_PLAY_RECORDING) - before the window loop starts.
    void addSettings();
    // The pad read's hook (scePadRead) and the runtime: from the game's overrides.
    void install(PS2Runtime &runtime);
    // The window thread, over the picture (between BeginDrawing and EndDrawing): the REC mark and the messages.
    void draw();

    bool recording(); // input being written now
    bool replaying(); // input being fed back now (from the replay's start to its end)
    bool armed();     // recording possible in this session (VIF1 on the game's thread)
    // (TODO 156) The F1 menu's Debug page: a recording can be started now (armed, none running or being made); its
    // Start button - the recording begins at the first game frame with the menu (and photo mode) closed, as F2 then;
    // and the folder F2's recordings go to (TF_PLAY_RECORD_DIR, "Recordings").
    bool canStartRecording();
    void startWhenMenuClosed();
    bool startPending();
    std::string recordingsFolder();

    // Once a game frame (the frame kick), before the frame's input: the check line of the frame, the key and
    // the stops, the replay's end.
    struct FrameFacts
    {
        uint64_t vsync = 0u;
        uint64_t gameTick = 0u;
        int rate = 0; // the level's frame rate (0: the console's 30)
        uint32_t seed = 0u;
        float camera[3] = {};
        float player[3] = {};
        float heading = 0.0f;
        bool photo = false;    // photo mode on
        bool menuOpen = false; // the F1 menu open
    };
    void frame(PS2Runtime *runtime, const FrameFacts &facts);

    // The channels (game thread). Each returns what the game is to get: in a replay the recorded value.
    bool mouseTravel(bool captured, float &dx, float &dy); // after the live reading (or none, replaying)
    int keyPresses(int key, int presses);
    float forestFit(float fit);
    void menuFrame(uint8_t *rdram);                     // the pause menu's frame, before its cursor
    void menuSelect(uint32_t address, uint32_t value);  // the cursor wrote the selection there
    // The frame-rate ladder: true while replaying - the ladder judges nothing then, and `apply` is called with
    // the recorded steps of this call (kind 0: the screen's refresh rate taken at a level's start, 1: the
    // in-between pictures on (value 1) or off, 2: a step to the rate `value`).
    bool rateCall(const std::function<void(int kind, int value)> &apply);
    void noteRate(int kind, int value);
    // The forest's reach as the session had it when a recording starts / its replay starts.
    void bindForest(float *fit);
}
