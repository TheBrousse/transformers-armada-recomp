#pragma once
// The HD textures while they are still being made (TODO 219).
//
// tools\make_hd_textures.ps1 makes the HD set in batches, the textures seen first first, and keeps
// hd_textures\upscale_status.json up to date (its state - working, paused, upscaled, done - and how many
// of how many are made). The game can be played meanwhile:
//  - While the game runs it holds hd_textures\.game_running (its process id inside; Windows deletes the
//    file when the game ends, a crash too). The upscale waits while the file is there - the two share the
//    graphics card - unless hd_options.json says "while_playing": true (the setup's "Keep making HD textures
//    while I play"): then it carries on at the graphics card's lowest priority.
//  - A set left unfinished (the setup closed, the game started from Play.bat, its exe or Steam): at the start
//    the game opens a small minimized window that carries on with it (make_hd_textures.ps1 -Resume) - unless
//    one is making them already (hd_textures\.upscaling holds its process id). It waits while the game runs
//    and finishes after it closes; closing it is fine, the next start carries on.
//  - The renderer takes the images as they appear: a texture without its HD image yet is asked for again
//    when the set has grown (generation(); nr_capture.cpp), never decoded or looked at in the frame.
//  - The F1 menu shows "HD TEXTURES 640 OF 1900" (line()) until the set is done.
// A set without upscale_status.json (made before this, or without HD textures) is left alone.
// TF_HD_HELPER=0: no helper window started (tests).

#include <cstdint>
#include <string>

namespace hd_progress
{
    // At start, in the game's folder (after main's enterGameFolder): the lock, the helper, the watcher.
    void start();
    // Counts up each time the set has grown (the status file rewritten); 0 before.
    uint64_t generation();
    // The F1 menu's line, upper case ("" when there is nothing being made).
    std::string line();
    // F1 > Graphics, "Open texture folder": the folder the game loads the HD textures from (nr::hdTextureFolder: the
    // install's hd_textures\upscaled, or TF_HIRES_DIR) opened in Explorer. Returns a line to show under the button:
    // "" when it opened; "No texture pack yet" (its parent opened when that exists) when the folder is not there.
    std::string openTextureFolder();
    // F1 > Graphics, "Make HD textures" / "Check HD textures" (makeLabel: Check once a set is done): starts the same
    // small window the game starts for an unfinished set (make_hd_textures.ps1 -Resume: the whole step - the disc's
    // archives, the conversion, the fonts, the upscale - each part skipped when done), waiting while the game runs
    // unless "Keep making HD textures while I play" is on; the F1 line follows it. Returns a line for the menu.
    std::string makeLabel();
    std::string makeHdTextures();
    // F1 > Display, "Sharp text": whether its fonts were made (hd_textures\crisp_fonts). Checked at most every 2 s.
    bool crispFontsFound();
}
