#pragma once
// The controller prompt style (PR #11 by TPepperoni666): which button labels the native renderer's font
// atlases show for a gamepad. Kept free of the renderer and the window for tests (tests/prompt_style_tests.cpp).
#include <atomic>
#include <string>

namespace nr
{
    // TF_PAD_PROMPTS: auto (the default) follows the gamepad used last; ps and xbox force a style.
    enum PadPrompts : int
    {
        kPadPromptsAuto = 0,
        kPadPromptsPlayStation = 1,
        kPadPromptsXbox = 2
    };
    inline int padPromptsFrom(const std::string &setting)
    {
        return setting == "xbox" ? kPadPromptsXbox : setting == "ps" ? kPadPromptsPlayStation : kPadPromptsAuto;
    }
    // The setting as chosen: written by its apply (crisp_text.cpp, the window thread), read per draw (any thread).
    inline std::atomic<int> g_padPrompts{kPadPromptsAuto};

    // Xbox labels in place of the button symbols. Auto picks them for a gamepad that is not Sony's, and only
    // while the prompts follow the device (TF_KEY_PROMPTS on: the overlay's "Match device") with the PS5-style
    // symbols: the overlay's "PlayStation" (key prompts off) and "PS2 (original)" keep the PlayStation buttons,
    // as an older settings.ini without TF_PAD_PROMPTS had them.
    inline bool useXboxPrompts(int mode, bool sony, bool followDevice, bool ps2Symbols)
    {
        if (mode == kPadPromptsXbox)
            return true;
        if (mode == kPadPromptsPlayStation)
            return false;
        return followDevice && !ps2Symbols && !sony;
    }
    inline bool useXboxPrompts(const std::string &setting, bool sony, bool followDevice = true, bool ps2Symbols = false)
    {
        return useXboxPrompts(padPromptsFrom(setting), sony, followDevice, ps2Symbols);
    }
}
