// The play recorder's hooks (playrec.h) when the build has no recorder (the public build, no overlay): nothing is
// recorded or replayed, and every channel gives the game what it was given. The overlay's playrec.cpp is the recorder.

#if !defined(TF_OVERLAY)

#include "playrec.h"

namespace playrec
{
    void prepareReplay() {}
    void prepareRecord() {}
    void beforeRun() {}
    void finish(const char *) {}
    void noteMods(const std::vector<std::string> &) {}
    void addSettings() {}
    void install(PS2Runtime &) {}
    void draw() {}
    bool recording() { return false; }
    bool replaying() { return false; }
    bool armed() { return false; }
    bool canStartRecording() { return false; }
    void startWhenMenuClosed() {}
    bool startPending() { return false; }
    std::string recordingsFolder() { return {}; }
    void frame(PS2Runtime *, const FrameFacts &) {}
    bool mouseTravel(bool captured, float &, float &) { return captured; }
    int keyPresses(int, int presses) { return presses; }
    float forestFit(float fit) { return fit; }
    void menuFrame(uint8_t *) {}
    void menuSelect(uint32_t, uint32_t) {}
    bool rateCall(const std::function<void(int kind, int value)> &) { return false; }
    void noteRate(int, int) {}
    void bindForest(float *) {}
}

#endif
