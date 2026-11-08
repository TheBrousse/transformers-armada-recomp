// What the game shows on a DualSense / DualShock 4 (dualsense.h; TODO 209, 210).

#include "dualsense.h"

#include "camera_mode.h"
#include "mod_menu.h"
#include "pad_speaker.h"

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_sony_pad.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dualsense
{
    namespace
    {
        // The player's energy: a record in the game's static data (0x34F2D0's owner + 0xC0, as TF_GOD reads it,
        // overrides.cpp): what is left and the full value. The owner's word 0 is the current episode (0x350D30).
        constexpr uint32_t kEpisodeNow = 0x00716EF0u;
        constexpr uint32_t kEnergy = 0x00716FB4u, kEnergyFull = 0x00716FBCu;
        // taAutobotHealthComponent::Update (0x303260): the HUD's health flashes below this fraction (0.4), and on
        // Cybertron (episode 7) shows this value as the fraction instead of the player's energy.
        constexpr uint32_t kFlashBelow = 0x006CBD10u, kCybertronHealth = 0x006CE0F0u;
        constexpr uint32_t kCybertron = 7u;
        // The Autobot you play: the episode's taMetaGameAutobotData (that same record, 0x716FB0: GetAutobot 0x3506E0
        // reads its first halfword; the HQ's character select sets it, SetAutobot 0x3506F0, and a level starts with
        // it): 0 Optimus Prime, 1 Hot Shot, 2 Red Alert (the save's order). The same in the HQ, the menus and a level.
        // (Not the player actor's taTransformerContext +0x18, the first try: it read 0 for Hot Shot too.)
        constexpr uint32_t kAutobotData = 0x00716FB0u;

        constexpr uint32_t kDeathBlinkMs = 167u; // about three times a second (on, off)
        constexpr uint32_t kLowBlinkMs = 400u;   // blue, red: at the HUD's flash below 40 per cent
        constexpr uint32_t kVeryLowBlinkMs = 200u; // ... and twice as fast below 25 per cent, as its alarm hurries

        std::atomic<bool> g_alerts{true};
        std::atomic<bool> g_triggers{true};
        int g_autobot = -1; // the last one seen (kept in the HQ and the menus)

        // (TODO 211) The player actor's Mini-Cons by slot (taTransformerActor::minicons, +0x34, GetMinicon 0x2867D0;
        // METAGAME.TDB's Slot): 0 the primary weapon (R1), 1 the secondary weapon (R2), 2 movement (L1), 3 an ability
        // (L2). Only the two on the triggers can have an adaptive-trigger effect. Each Mini-Con is a taMiniconAbility
        // class: known by its vtable (the decomp's config/rtti_classes.json).
        constexpr uint32_t kActorMinicons = 0x34u;
        constexpr int kSlotR2 = 1, kSlotL2 = 3;
        enum class Feel : uint8_t
        {
            None,
            Missile, // a weapon-style click near the end of the pull (our earlier launcher's "weapon" effect)
            Cannon,  // the same, harder and earlier: a heavy shot
            Laser,   // steady resistance all the way (our earlier launcher's "medium")
        };
        struct WeaponClass
        {
            uint32_t vtable;
            Feel feel;
            const char *name;
        };
        constexpr WeaponClass kWeapons[] = {
            {0x006BCE50u, Feel::Missile, "Homing Missile"},
            {0x006BCEB0u, Feel::Missile, "Heavy Homing Missile"},
            {0x006BCD30u, Feel::Missile, "Cluster Rocket"},
            {0x006BCD90u, Feel::Missile, "Heavy Cluster Rocket"},
            {0x006BCFD0u, Feel::Cannon, "Vortex Cannon"},
            {0x006BD0E0u, Feel::Cannon, "Dark Energon Blaster"},
            {0x006BD4F0u, Feel::Cannon, "Super Heavy Cannon"},
            {0x006BCC70u, Feel::Cannon, "Ultimate Cannon"},
            {0x006BCF10u, Feel::Laser, "Sniper Rifle"},
            {0x006BD320u, Feel::Laser, "Heavy Ribbon Beam"},
            {0x006BD2C0u, Feel::Laser, "Super Heavy Ribbon Beam"},
            {0x006BD3E0u, Feel::Laser, "Electric Arc Gun"},
            {0x006BD380u, Feel::Laser, "Heavy Electric Arc Gun"},
        };

        bool readWord(const uint8_t *rdram, uint32_t address, uint32_t &out);

        // The player's taTransformerActor, as taPlayerManager::GetFirstPlayer (0x2D4EF0) and the engine sound
        // (taEngineSoundImpl) find it: the manager (0x6CF498) lists its players from +8 (the list's end: +4); a
        // player's creature is at +8, the creature's actor at +0x54. 0 when there is none (the HQ, a cutscene).
        constexpr uint32_t kPlayerManager = 0x006CF498u;
        uint32_t playerActor(const uint8_t *rdram)
        {
            uint32_t manager = 0u, node = 0u, player = 0u, creature = 0u, actor = 0u;
            if (!readWord(rdram, kPlayerManager, manager) || !manager || !readWord(rdram, manager + 8u, node) ||
                node == manager + 4u || !readWord(rdram, node + 8u, player) || !readWord(rdram, player + 8u, creature) ||
                !readWord(rdram, creature + 0x54u, actor))
                return 0u;
            return actor;
        }

        const WeaponClass *weaponIn(const uint8_t *rdram, uint32_t actor, int slot)
        {
            uint32_t ability = 0u, vtable = 0u;
            if (!readWord(rdram, actor + kActorMinicons + 4u * static_cast<uint32_t>(slot), ability) || ability == 0u ||
                !readWord(rdram, ability, vtable))
                return nullptr;
            for (const WeaponClass &w : kWeapons)
                if (w.vtable == vtable)
                    return &w;
            return nullptr;
        }

        // The 11 bytes of a trigger's effect (the mode, then its parameters), from our earlier launcher's pad code.
        // TF_TRIGGER_STRENGTH: light (the default), medium, strong - the effects' force.
        std::atomic<int> g_strength{0};

        // The 11 bytes of a trigger's effect (the mode, then its parameters), and the pull (0..255, as the pad
        // reports it) at which the game is to see the trigger pressed: the effect's break point, so the shot comes
        // with the click (0: as usual). Your playtest (2026-11-08): the first set (break near 0x90-0xB0, full force)
        // fired long before the click and was far too stiff.
        void effectOf(Feel feel, uint8_t out[11], uint8_t &pressAt)
        {
            std::memset(out, 0, 11);
            pressAt = 0u;
            const int strength = g_strength.load(std::memory_order_relaxed);
            switch (feel)
            {
            case Feel::Missile: // weapon: resistance from 0x40 to 0x70 of the pull (a quarter to under half), then it snaps
            {
                static const uint8_t kForce[3] = {0x50u, 0x90u, 0xD0u};
                out[0] = 0x02u;
                out[1] = 0x40u;
                out[2] = 0x70u;
                out[3] = kForce[strength];
                pressAt = 0x70u;
                break;
            }
            case Feel::Cannon: // weapon, a longer section to half the pull and harder: a heavier shot
            {
                static const uint8_t kForce[3] = {0x70u, 0xB0u, 0xFFu};
                out[0] = 0x02u;
                out[1] = 0x48u;
                out[2] = 0x80u;
                out[3] = kForce[strength];
                pressAt = 0x80u;
                break;
            }
            case Feel::Laser: // continuous resistance from the start of the pull, light (beams fire as usual)
            {
                static const uint8_t kForce[3] = {0x28u, 0x50u, 0x80u};
                out[0] = 0x01u;
                out[1] = 0x00u;
                out[2] = kForce[strength];
                break;
            }
            case Feel::None:
                break;
            }
        }

        bool readWord(const uint8_t *rdram, uint32_t address, uint32_t &out)
        {
            if (address < 0x00100000u || address >= PS2_RAM_SIZE - 4u || (address & 3u))
                return false;
            const uint8_t *p = getConstMemPtr(rdram, address);
            if (!p)
                return false;
            std::memcpy(&out, p, sizeof(out));
            return true;
        }

        bool readFloat(const uint8_t *rdram, uint32_t address, float &out)
        {
            uint32_t bits = 0u;
            if (!readWord(rdram, address, bits))
                return false;
            std::memcpy(&out, &bits, sizeof(out));
            return out == out; // (not a NaN)
        }

        int autobotNow(const uint8_t *rdram)
        {
            uint32_t word = 0u;
            if (!readWord(rdram, kAutobotData, word))
                return -1;
            const uint32_t id = word & 0xFFFFu; // (the first halfword, little endian)
            return id <= 2u ? static_cast<int>(id) : -1;
        }

        void colourOf(int autobot, uint8_t rgb[3])
        {
            static const uint8_t kColours[3][3] = {
                {0xDCu, 0x16u, 0x16u}, // Optimus Prime: red
                {0xF0u, 0xB4u, 0x00u}, // Hot Shot: yellow
                {0x1Eu, 0x50u, 0xDCu}, // Red Alert: blue
            };
            std::memcpy(rgb, kColours[autobot >= 0 && autobot <= 2 ? autobot : 0], 3);
        }

        // The light bar setting: "autobot" (the default, also when unset), "off", or a colour RRGGBB.
        bool baseColour(int autobot, uint8_t rgb[3])
        {
            std::string value = ps2_settings::value("PS2X_SONYPAD_LIGHTBAR");
            for (char &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (value == "off")
                return false;
            unsigned colour = 0u;
            if (value.size() == 6u && std::sscanf(value.c_str(), "%6x", &colour) == 1)
            {
                rgb[0] = static_cast<uint8_t>(colour >> 16);
                rgb[1] = static_cast<uint8_t>(colour >> 8);
                rgb[2] = static_cast<uint8_t>(colour);
                return true;
            }
            colourOf(autobot, rgb);
            return true;
        }
    }

    void addSettings()
    {
        pad_speaker::addSettings(); // (the controller's speaker: pad_speaker.h)
        if (const char *env = std::getenv("TF_LIGHTBAR_ALERTS"); env && *env)
            g_alerts.store(*env != '0');
        ps2_settings::Setting s;
        s.key = "TF_LIGHTBAR_ALERTS";
        s.section = "Controller";
        s.label = "Light bar warnings";
        s.order = 31;
        s.help = "On a DualSense or DualShock 4: the light blinks blue and red while your health is low, and blinks your "
                 "Autobot's colour when he is destroyed.";
        s.choices = {{"1", "On"}, {"0", "Off"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &value)
        {
            g_alerts.store(value != "0");
            return true;
        };
        ps2_settings::add(s);

        if (const char *env = std::getenv("TF_ADAPTIVE_TRIGGERS"); env && *env)
            g_triggers.store(*env != '0');
        ps2_settings::Setting t;
        t.key = "TF_ADAPTIVE_TRIGGERS";
        t.section = "Controller";
        t.label = "Adaptive triggers";
        t.order = 32;
        t.help = "On a DualSense: L2 or R2 pushes back while a weapon Mini-Con is on it. Missiles click, cannons kick "
                 "harder, beam weapons resist all the way. Off in car mode.";
        t.choices = {{"1", "On"}, {"0", "Off"}};
        t.defaultValue = "1";
        t.apply = [](const std::string &value)
        {
            g_triggers.store(value != "0");
            return true;
        };
        ps2_settings::add(t);

        const auto strengthOf = [](const std::string &v) { return v == "strong" ? 2 : v == "medium" ? 1 : 0; };
        if (const char *env = std::getenv("TF_TRIGGER_STRENGTH"); env && *env)
            g_strength.store(strengthOf(env));
        ps2_settings::Setting k;
        k.key = "TF_TRIGGER_STRENGTH";
        k.section = "Controller";
        k.label = "Trigger strength";
        k.order = 33;
        k.help = "How hard the adaptive triggers push back.";
        k.choices = {{"light", "Light"}, {"medium", "Medium"}, {"strong", "Strong"}};
        k.defaultValue = "light";
        k.apply = [strengthOf](const std::string &value)
        {
            g_strength.store(strengthOf(value));
            return true;
        };
        ps2_settings::add(k);
    }

    void frame(const uint8_t *rdram, bool inLevel, uint32_t playerPosition)
    {
        if (!rdram)
            return;
        if (const int autobot = autobotNow(rdram); autobot >= 0 && autobot != g_autobot)
        {
            g_autobot = autobot;
            static const char *kNames[3] = {"Optimus Prime", "Hot Shot", "Red Alert"};
            std::fprintf(stderr, "[dualsense] playing %s (Autobot %d, from the episode's Autobot record %08x, %s)\n",
                         kNames[autobot], autobot, kAutobotData, inLevel ? "a level or the HQ" : "the menus");
        }

        ps2x::sonypad::LightbarRequest request;
        uint8_t colour[3] = {0u, 0u, 0u};
        request.active = baseColour(g_autobot, colour);
        std::memcpy(request.a, colour, 3);
        std::memcpy(request.b, colour, 3);

        float energy = 0.0f, full = 0.0f;
        if (request.active && inLevel && g_alerts.load(std::memory_order_relaxed) && readFloat(rdram, kEnergy, energy) &&
            readFloat(rdram, kEnergyFull, full) && full > 0.0f)
        {
            uint32_t episode = 0u;
            readWord(rdram, kEpisodeNow, episode);
            float ratio = energy / full;
            if (episode == kCybertron)
                readFloat(rdram, kCybertronHealth, ratio);
            ratio = ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio;
            float flashBelow = 0.4f;
            readFloat(rdram, kFlashBelow, flashBelow);
            if (energy <= 0.0f)
            {
                // Destroyed: his colour on and off until he is back.
                std::memset(request.b, 0, 3);
                request.blinkMs = kDeathBlinkMs;
            }
            else if (ratio < flashBelow)
            {
                // The HUD's health flashes: blue and red.
                static const uint8_t kBlue[3] = {0x00u, 0x30u, 0xFFu}, kRed[3] = {0xFFu, 0x00u, 0x00u};
                std::memcpy(request.a, kBlue, 3);
                std::memcpy(request.b, kRed, 3);
                request.blinkMs = ratio < 0.25f ? kVeryLowBlinkMs : kLowBlinkMs;
            }
        }
        static bool wasLow = false, wasDead = false;
        const bool dead = request.blinkMs == kDeathBlinkMs, low = request.blinkMs != 0u && !dead;
        if (low != wasLow || dead != wasDead)
        {
            std::fprintf(stderr, "[dualsense] light bar: %s\n", dead ? "destroyed, blinking" : low ? "low health, blinking" : "steady");
            wasLow = low;
            wasDead = dead;
        }
        static ps2x::sonypad::LightbarRequest sent;
        if (request != sent)
        {
            if (!request.active)
                std::fprintf(stderr, "[dualsense] light bar request: the pad's own\n");
            else if (request.blinkMs)
                std::fprintf(stderr, "[dualsense] light bar request: %02X%02X%02X / %02X%02X%02X every %u ms\n", request.a[0],
                             request.a[1], request.a[2], request.b[0], request.b[1], request.b[2], request.blinkMs);
            else
                std::fprintf(stderr, "[dualsense] light bar request: %02X%02X%02X\n", request.a[0], request.a[1], request.a[2]);
            sent = request;
        }
        ps2x::sonypad::setLightbarRequest(request);

        // (TODO 211) The adaptive triggers: the weapon Mini-Con on L2 / R2 gives that trigger its feel - only on foot
        // in a level, while he is alive, with the menu closed.
        ps2x::sonypad::TriggerRequest triggers;
        const uint32_t actor = playerActor(rdram);
        (void)playerPosition;
        float left = 0.0f;
        const bool alive = !readFloat(rdram, kEnergy, left) || left > 0.0f;
        const WeaponClass *onL2 = nullptr, *onR2 = nullptr;
        if (g_triggers.load(std::memory_order_relaxed) && inLevel && actor && alive && !camera_mode::inVehicle() &&
            !mod_menu::isOpen())
        {
            onL2 = weaponIn(rdram, actor, kSlotL2);
            onR2 = weaponIn(rdram, actor, kSlotR2);
        }
        if (onL2 || onR2)
        {
            triggers.active = true;
            effectOf(onL2 ? onL2->feel : Feel::None, triggers.left, triggers.pressLeft);
            effectOf(onR2 ? onR2->feel : Feel::None, triggers.right, triggers.pressRight);
        }
        // TF_DUALSENSE_TRACE=1 (a debugging aid): each slot's Mini-Con class (its vtable) when it changes.
        static const bool trace = [] { const char *e = std::getenv("TF_DUALSENSE_TRACE"); return e && *e == '1'; }();
        if (trace && inLevel && actor)
        {
            static uint32_t was[4] = {1u, 1u, 1u, 1u};
            for (uint32_t slot = 0u; slot < 4u; ++slot)
            {
                uint32_t ability = 0u, vtable = 0u;
                if (readWord(rdram, actor + kActorMinicons + 4u * slot, ability) && ability)
                    readWord(rdram, ability, vtable);
                if (vtable != was[slot])
                {
                    std::fprintf(stderr, "[dualsense] slot %u: Mini-Con %08x vtable %08x\n", slot, ability, vtable);
                    was[slot] = vtable;
                }
            }
        }
        static const WeaponClass *wasL2 = nullptr, *wasR2 = nullptr;
        if (onL2 != wasL2 || onR2 != wasR2)
        {
            std::fprintf(stderr, "[dualsense] adaptive triggers: L2 %s, R2 %s\n", onL2 ? onL2->name : "none",
                         onR2 ? onR2->name : "none");
            wasL2 = onL2;
            wasR2 = onR2;
        }
        ps2x::sonypad::setTriggerRequest(triggers);
    }
}
