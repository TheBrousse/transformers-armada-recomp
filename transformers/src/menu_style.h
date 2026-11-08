#pragma once
// The menu's look (menu_style.cpp): a modern game menu in Transformers colours (gunmetal panels,
// Autobot red, energon blue), and the widgets the mod menu is built from.
//
// Fonts, read from the fonts folder beside the game (fonts\ in the working folder, then next to
// the program) when they are there: "Transformers Movie.ttf" for the title, "Nulshock Bd.otf"
// for headings and tabs, "Bahnschrift.ttf" (also from Windows' own fonts) for the text. ImGui's
// own font stands in for any that is missing.

#include <string>
#include <vector>

struct ImFont;

namespace menu_style
{
    struct Fonts
    {
        ImFont *title = nullptr;
        ImFont *heading = nullptr;
        ImFont *text = nullptr;
    };

    // Once ImGui runs, before its frame (rlImGuiBegin): the fonts and the style.
    void prepare();
    const Fonts &fonts();
    // The text size for the window's height now, in pixels.
    float textSize();
    void applyStyle();

    // Colours (ImU32, as the draw list takes them).
    unsigned int colour(const char *name); // "red", "red_dark", "blue", "amber", "text", "dim", "panel", "line"

    // A tab of the side bar: true when clicked. `selected` draws it lit.
    bool sideTab(const char *label, bool selected, float width);
    // A section's heading in the page.
    void heading(const char *text);
    // A row: its label on the left, a choice on the right as < value > (the value opens the whole
    // list). Returns the index chosen, or -1. `badge`: a word after the label ("RESTART").
    int choiceRow(const char *id, const std::string &label, const std::vector<std::string> &choices, int current,
                  bool enabled, const char *badge, const std::string &help);
    // An on / off row.
    bool toggleRow(const char *id, const std::string &label, bool &on, const std::string &help);
    // A button in the menu's colours: primary is red, the others outlined.
    bool button(const char *label, bool primary = false);
    // A small one, for lists and tables.
    bool smallButton(const char *label);
    // A status chip: a word in a coloured frame ("blue", "red", "dim", "amber").
    void chip(const char *text, const char *colourName);
    // The help shown in the footer: what the pointer is on (set by the rows this frame).
    void setHelp(const std::string &help);
    const std::string &help();
    void clearHelp();
}
