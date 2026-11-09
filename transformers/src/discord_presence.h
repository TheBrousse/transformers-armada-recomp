#pragma once
// Discord status (Rich Presence): what the game shows on the player's Discord profile - the level (or the HQ, the
// main menu, a film), the Autobot played and how long. No SDK: the local Discord client's IPC pipe
// (\\.\pipe\discord-ipc-0..9), on a background thread that never makes the game wait; nothing happens while Discord
// is not running (it tries again every 30 seconds). On by default; F1 > Other > Discord status switches it.
//
// The Discord application: kApplicationId in discord_presence.cpp (one place), or TF_DISCORD_APP_ID=<id> in the
// environment or settings.ini. Its Rich Presence art assets are named "logo" (large) and "optimus", "hotshot",
// "redalert" (small, the Autobot); an extension's character (port_ext.h presenceCharacter) brings its own key.
// TF_DISCORD_PIPE=<name> (tests): that pipe instead of discord-ipc-0..9 (\\.\pipe\<name>).

#include <cstdint>

namespace discord_presence
{
    // Its setting (TF_DISCORD, kept in settings.ini) and the worker thread.
    void addSettings();
    // Once a game frame (overrides.cpp): where the player is. Cheap: only compares and hands the result over.
    void frame(const uint8_t *rdram, bool inLevel, bool inHq, bool moviePlaying);
    // At the game's end: the status cleared and the pipe closed (it waits at most 1.5 s for that).
    void shutdown();
}
