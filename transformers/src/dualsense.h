#pragma once
// What the game shows on a DualSense / DualShock 4 (TODO 209), sent to the pad through the runtime's requests
// (ps2_sony_pad.h: setLightbarRequest, setTriggerRequest; the device thread writes them).
//
// - The light bar follows the Autobot you play (PS2X_SONYPAD_LIGHTBAR=autobot, the default): Optimus red, Hot Shot
//   yellow, Red Alert blue. A chosen colour stays that colour. Off leaves the light to the pad.
// - TF_LIGHTBAR_ALERTS (on by default): while the HUD's health flashes (below 40 per cent, the game's own test,
//   taAutobotHealthComponent::Update 0x303260) the light blinks blue and red; when the Autobot dies it blinks his
//   colour about three times a second until he is back. By real time, so the same at any frame rate.

#include <cstdint>

namespace dualsense
{
    // Its settings (before the window loop starts: overrides.cpp, with the Controller tab's).
    void addSettings();
    // Once a game frame (overrides.cpp). inLevel: a taGame runs; playerPosition: the player actor's position field
    // (playerPositionAddress, 0 when none).
    void frame(const uint8_t *rdram, bool inLevel, uint32_t playerPosition);
}
