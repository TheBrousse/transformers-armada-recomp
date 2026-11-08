#include "runtime/ps2_host_input.h"
#include "nr_prompt_style.h"
#include <cassert>
#include <iostream>
int main()
{
    using namespace ps2_host_input;
    unsigned cases = 0;
    for (const char *name : {"Sony DualSense", "PS4 Controller", "DualShock 4", "Wireless Controller",
                             "wireless controller", "DualSense Wireless Controller", "PS5", "PLAYSTATION(R)3 Controller"})
    {
        assert(padStyleForName(name) == PadStyle::PlayStation);
        std::cout << "PASS: " << name << " => PlayStation\n";
        ++cases;
    }
    for (const char *name : {static_cast<const char *>(nullptr), "", "Xbox 360 Controller", "Xbox Wireless Controller",
                             "Xbox One Wireless Controller", "Xbox Series X Controller", "XBOX WIRELESS CONTROLLER",
                             "XInput Wireless Controller", "X-Box Controller", "360 Controller", "Series X Controller",
                             "Steam Virtual Gamepad", "Steam Deck", "Steam Controller", "Legion Go Controller",
                             "Legion Go 2 Wireless Controller", "ASUS ROG Ally", "MSI Claw", "AYANEO Controller",
                             "One XPlayer Controller", "ONEXPLAYER Wireless Controller", "Generic USB Gamepad",
                             "Generic Wireless Controller", "Xbox Sony Wireless Controller", "Steam Virtual DualSense"})
    {
        assert(padStyleForName(name) == PadStyle::Xbox);
        std::cout << "PASS: " << (name ? name : "<null>") << " => Xbox\n";
        ++cases;
    }
    for (bool sony : {false, true})
    {
        assert(nr::useXboxPrompts("auto", sony) == !sony);
        assert(nr::useXboxPrompts("", sony) == !sony);
        assert(nr::useXboxPrompts("xbox", sony));
        assert(!nr::useXboxPrompts("ps", sony));
        // (the overlay's PlayStation / PS2 rows: auto with key prompts off, or the PS2's symbols, keeps PlayStation)
        assert(!nr::useXboxPrompts("auto", sony, false, false));
        assert(!nr::useXboxPrompts("auto", sony, true, true));
        assert(nr::useXboxPrompts("xbox", sony, false, true));
        assert(!nr::useXboxPrompts("ps", sony, true, false));
        assert(nr::useXboxPrompts(nr::kPadPromptsAuto, sony, true, false) == !sony);
        std::cout << "PASS: auto/default/forced styles, Sony=" << sony << "\n";
    }
    std::cout << "PASS: " << cases << " controller names and 18 style-selection checks\n";
}
