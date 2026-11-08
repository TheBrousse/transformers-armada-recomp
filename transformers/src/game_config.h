#pragma once
// The game's own config settings in the settings panel (F5): the Cheats and Debug tabs.
//
// The game keeps about 120 named settings (god mode, which creatures spawn, cutscenes, the
// HUD, ...) in utGameConfig, loaded at start from CONFIGS/0_DEFS.TMD. The developers changed
// them with a debug menu that retail no longer has; the code that reads them is still there.
// These tabs set them: the chosen value is written into the game's table of settings every
// frame it differs (on the game thread, between frames), so a setting left at "Game's own"
// is never touched. The game reads most of them when something is created (god mode when the
// Autobot is spawned, the creatures when a level loads), so a change shows from the next
// level on.
//
// The table (retail SLUS-20668): a vector of clData* at 0x715598 (begin) / 0x715594 (count),
// filled by utGameConfig::Startup (0x277CC0); each entry's label is a pointer to a std::string at +4, and
// its value - a bool's byte, an enum's index, an integer, a float - is at +0x14 (where the
// typed getters return it: GetBoolean 0x277B90 and its neighbours, through vtable slots +0x38
// enum, +0x3C integer, +0x44 float, +0x48 bool, +0x4C string). TF_CONFIG_DUMP=1 logs every
// entry once the table is filled (label, vtable, value): checked against CONFIGS/0_DEFS.TMD.

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace game_config
{
    // The tabs' settings: before the window loop starts.
    void addSettings();

    // A setting of the game's config table added by an extension (cheat_rows.cpp: the Cheats rows and the developers'
    // Debug rows). Call before addSettings (from a static initialiser): rows come after the core's, in the order added.
    struct Row
    {
        const char *label;   // the setting's name in 0_DEFS.TMD
        const char *section; // the panel's tab
        const char *title;
        const char *help;
        bool isEnum;                      // false: a bool
        std::vector<const char *> values; // an enum's values in the file's order (stored as the index)
        int order;
    };
    void addRow(const Row &row);

    // The same settings for the mod menu (F1). Choosing one there is the same as in F5 (kept in
    // settings.ini, in effect at the game's next frame).
    struct Choice
    {
        std::string value; // "" = the game's own
        std::string label;
    };
    struct View
    {
        std::string section, title, help;
        std::vector<Choice> choices;
        std::string chosen;
    };
    std::vector<View> views();
    void choose(size_t index, const std::string &value);
    // Once a rendered frame, on the game thread: puts the chosen values in place.
    void frame(uint8_t *rdram);
    // Where a setting's value is (a bool's byte, ...), by its label in 0_DEFS.TMD; 0 until frame has
    // found the table. On the game thread.
    uint32_t valueAddress(const char *label);
    // (TODO 228) The HUD shown or not, at once: the game reads showHUD only when it makes the HUD (taPlayerHUD's
    // constructor 0x2FBBC0 keeps it at the HUD's +0x74, which the HUD's draw 0x2FB190 checks: every part - the bars,
    // the Mini-Con ring and its icons, the insignia, the HQ's calls, the context messages - is one of its components).
    // So a change of the setting, or photo mode, showed only from the next level. The draw is wrapped: the HUD's own
    // flag is set from the setting (or photo mode's hidden HUD) as it draws; "Game's own" leaves it as the game made
    // it. installHud before the game runs; hudOff: the setting says Off (the crosshair goes too, as in photo mode).
    void installHud(PS2Runtime &runtime);
    bool hudOff();
    // A start movie the "When the game starts" setting skips (start_screens.h, TODO 110; it replaced the
    // "Intro screens" switch). The game's own runUIIntro is never written: it skips the title's whole
    // flow - the profile is never set up and the first level stops the game (TODO 80).
    bool skipIntroMovie(const std::string &movieKey);
}
