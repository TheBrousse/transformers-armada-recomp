// What the game shows on a DualSense / DualShock 4 (dualsense.h; TODO 209, 210).

#include "dualsense.h"

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
        // The player actor (the camera's target: its position at +0x620) holds its taTransformerContext at +0x7C,
        // whose +0x18 is the Autobot (taTransformerActor::GetAutobotID 0x28A740; the Autobot enum: 0 Optimus,
        // 1 Hot Shot, 2 Red Alert - the save's order, docs/SAVEGAME).
        constexpr uint32_t kPositionField = 0x620u, kActorContext = 0x7Cu, kContextAutobot = 0x18u;

        constexpr uint32_t kDeathBlinkMs = 167u; // about three times a second (on, off)
        constexpr uint32_t kLowBlinkMs = 400u;   // blue, red: at the HUD's flash below 40 per cent
        constexpr uint32_t kVeryLowBlinkMs = 200u; // ... and twice as fast below 25 per cent, as its alarm hurries

        std::atomic<bool> g_alerts{true};
        int g_autobot = -1; // the last one seen (kept in the HQ and the menus)

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

        int autobotOf(const uint8_t *rdram, uint32_t playerPosition)
        {
            if (playerPosition < kPositionField)
                return -1;
            uint32_t context = 0u, word = 0u;
            if (!readWord(rdram, playerPosition - kPositionField + kActorContext, context) ||
                !readWord(rdram, context + (kContextAutobot & ~3u), word))
                return -1;
            const uint32_t id = (kContextAutobot & 2u) ? (word >> 16) : (word & 0xFFFFu); // (little endian)
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
    }

    void frame(const uint8_t *rdram, bool inLevel, uint32_t playerPosition)
    {
        if (!rdram)
            return;
        if (inLevel)
            if (const int autobot = autobotOf(rdram, playerPosition); autobot >= 0 && autobot != g_autobot)
            {
                g_autobot = autobot;
                static const char *kNames[3] = {"Optimus Prime", "Hot Shot", "Red Alert"};
                std::fprintf(stderr, "[dualsense] playing %s (Autobot %d)\n", kNames[autobot], autobot);
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
        ps2x::sonypad::setLightbarRequest(request);
    }
}
