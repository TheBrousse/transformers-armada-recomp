// The game's words for the sticks said for the keyboard (TODO 223): key_text.h.

#include "key_text.h"

#include "mod_menu.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace key_text
{
    namespace
    {
        // One string of the table that names a stick: its CodeWarrior std::string (word 0 = capacity << 1 | 1, +4 the
        // size, +8 the pointer to the characters; measured on the loaded table) and its text as the game has it.
        struct Entry
        {
            uint32_t object = 0, chars = 0;
            std::string original;
        };

        std::mutex g_mutex;
        std::vector<Entry> g_entries; // (found by the search; the game's thread applies them)
        std::atomic<bool> g_searching{false};
        bool g_searched = false;
        bool g_shown = false;                // the keyboard texts in place
        std::string g_shownLayout;           // (the layout they were made from: a change makes them again)
        std::chrono::steady_clock::time_point g_lastSearch{}, g_lastCheck{};
        int g_searches = 0;

        uint32_t word(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + a, 4);
            return v;
        }
        void setWord(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + a, &v, 4); }

        bool enabled()
        {
            static const bool on = !(std::getenv("TF_KEY_TEXT") && *std::getenv("TF_KEY_TEXT") == '0');
            return on;
        }

        // ---- The keys' names, as the prompts' key caps show them (make_crisp_fonts.py: shown_key, KEY_SHORT) ----
        std::map<std::string, std::vector<std::string>> layout(const std::string &text)
        {
            std::map<std::string, std::vector<std::string>> out;
            size_t at = 0;
            while (at < text.size())
            {
                size_t end = text.find(';', at);
                if (end == std::string::npos)
                    end = text.size();
                const std::string part = text.substr(at, end - at);
                at = end + 1;
                const size_t eq = part.find('=');
                if (eq == std::string::npos)
                    continue;
                std::vector<std::string> keys;
                size_t k = eq + 1;
                while (k <= part.size())
                {
                    size_t c = part.find(',', k);
                    if (c == std::string::npos)
                        c = part.size();
                    if (c > k)
                        keys.push_back(part.substr(k, c - k));
                    k = c + 1;
                }
                out[part.substr(0, eq)] = keys;
            }
            return out;
        }

        std::string keyWord(const std::string &key)
        {
            static const std::map<std::string, std::string> shortNames = {
                {"ESCAPE", "ESC"}, {"BACKSPACE", "BKSP"}, {"INSERT", "INS"}, {"DELETE", "DEL"}, {"PAGE_UP", "PGUP"},
                {"PAGE_DOWN", "PGDN"}, {"LEFT_SHIFT", "SHIFT"}, {"RIGHT_SHIFT", "SHIFT"}, {"LEFT_CONTROL", "CTRL"},
                {"RIGHT_CONTROL", "CTRL"}, {"LEFT_ALT", "ALT"}, {"RIGHT_ALT", "ALT"}, {"CAPS_LOCK", "CAPS"},
                {"MOUSE_LEFT", "left click"}, {"MOUSE_RIGHT", "right click"}, {"MOUSE_MIDDLE", "middle click"},
                {"MOUSE_SIDE", "side mouse button"}, {"MOUSE_EXTRA", "extra mouse button"}};
            const auto it = shortNames.find(key);
            if (it != shortNames.end())
                return it->second;
            if (key.rfind("KP_", 0) == 0)
                return "keypad " + key.substr(3);
            std::string s = key;
            std::replace(s.begin(), s.end(), '_', ' ');
            return s;
        }

        std::string shownKey(const std::map<std::string, std::vector<std::string>> &keys, const char *button)
        {
            const auto it = keys.find(button);
            if (it == keys.end() || it->second.empty())
                return {};
            const std::vector<std::string> &k = it->second;
            static const char *const modifiers[] = {"LEFT_SHIFT", "RIGHT_SHIFT", "LEFT_CONTROL", "RIGHT_CONTROL", "LEFT_ALT", "RIGHT_ALT"};
            if (std::find_if(std::begin(modifiers), std::end(modifiers), [&](const char *m) { return k[0] == m; }) != std::end(modifiers))
                for (const std::string &m : k)
                    if (m.rfind("MOUSE_", 0) == 0)
                        return keyWord(m);
            return keyWord(k[0]);
        }

        void replaceAll(std::string &s, const std::string &from, const std::string &to)
        {
            for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
                s.replace(at, from.size(), to);
        }

        // The keyboard's wording of a string (the stick phrases first, the longest first; then the buttons named in
        // the same strings). The same text when nothing in it changes.
        std::string keyboardText(const std::string &original, const std::map<std::string, std::vector<std::string>> &keys)
        {
            const std::string up = shownKey(keys, "lup"), left = shownKey(keys, "lleft"), down = shownKey(keys, "ldown"),
                              right = shownKey(keys, "lright");
            const std::string move = up + " " + left + " " + down + " " + right;
            std::string s = original;
            replaceAll(s, "Left analog stick - Steering", left + " " + right + " - Steering");
            replaceAll(s, "pull back on the left analog stick", "hold " + down);
            replaceAll(s, "Use the left analog stick", "Use " + move);
            replaceAll(s, "use the left analog stick", "use " + move);
            replaceAll(s, "the left analog stick", move);
            replaceAll(s, "the right analog stick", "the mouse");
            replaceAll(s, "Left analog stick", move);
            replaceAll(s, "Right analog stick", "Mouse");
            replaceAll(s, "left analog stick", move);
            replaceAll(s, "right analog stick", "the mouse");
            static const std::pair<const char *, const char *> buttons[] = {
                {"L1", "l1"}, {"R1", "r1"}, {"L2", "l2"}, {"R2", "r2"}, {"L3", "l3"}, {"R3", "r3"}, {"X", "cross"}, {"START", "start"}};
            for (const auto &[name, button] : buttons)
            {
                const std::string key = shownKey(keys, button);
                if (key.empty())
                    continue;
                replaceAll(s, std::string("the ") + name + " button", key);
                replaceAll(s, std::string(name) + " button", key);
            }
            return s;
        }

        // ---- The search: the strings that name a stick, and the std::strings that hold them ----
        void search(const uint8_t *rdram)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const uint32_t from = 0x00100000u, to = PS2_RAM_SIZE;
            static const char *const marks[] = {"analog stick", "R3 button", "L3 button"};
            std::vector<uint32_t> starts;
            for (const char *m : marks)
            {
                const size_t n = std::strlen(m);
                const char *ram = reinterpret_cast<const char *>(rdram);
                const char *begin = ram + from, *end = ram + to;
                const std::boyer_moore_horspool_searcher finder(m, m + n);
                for (const char *at = begin;;)
                {
                    const char *hit = std::search(at, end, finder);
                    if (hit == end)
                        break;
                    uint32_t a = static_cast<uint32_t>(hit - ram);
                    uint32_t s = a;
                    while (s > from && rdram[s - 1] != 0 && a - s < 4096u)
                        --s;
                    if (std::find(starts.begin(), starts.end(), s) == starts.end())
                        starts.push_back(s);
                    at = hit + n;
                }
            }
            std::vector<Entry> found;
            if (!starts.empty())
            {
                const auto [lo, hi] = std::minmax_element(starts.begin(), starts.end());
                const uint32_t min = *lo, max = *hi;
                for (uint32_t p = from + 8u; p + 4u <= to; p += 4u)
                {
                    const uint32_t v = word(rdram, p);
                    if (v < min || v > max || std::find(starts.begin(), starts.end(), v) == starts.end())
                        continue;
                    const uint32_t object = p - 8u, flags = word(rdram, object), size = word(rdram, object + 4u);
                    // (a long-form std::string: an odd first word, its capacity at least the size, the text that long)
                    if (!(flags & 1u) || (flags >> 1) < size || size == 0u || size > 4096u || v + size >= to ||
                        rdram[v + size] != 0 || std::strlen(reinterpret_cast<const char *>(rdram + v)) != size)
                        continue;
                    Entry e;
                    e.object = object;
                    e.chars = v;
                    e.original.assign(reinterpret_cast<const char *>(rdram + v), size);
                    found.push_back(std::move(e));
                }
            }
            std::fprintf(stderr, "[key text] the sticks' strings: %zu found (%zu texts), %.0f ms\n", found.size(), starts.size(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            std::lock_guard<std::mutex> lock(g_mutex);
            g_entries = std::move(found);
            g_searched = true;
        }

        // Each string as `keyboard` says (its keyboard text, or the original back), where it is still the game's.
        // False: a string is not where it was found (another table: a state of another session) - search again.
        bool apply(uint8_t *rdram, bool keyboard, const std::string &layoutText)
        {
            const auto keys = layout(ps2_host_input::defaultKeyBindings() + ";" + layoutText);
            static bool toldLayout = std::getenv("TF_KEY_TEXT_LOG") == nullptr;
            if (!toldLayout)
            {
                toldLayout = true;
                std::fprintf(stderr, "[key text] layout: %s\n", layoutText.c_str());
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            bool allThere = true;
            int changed = 0;
            for (const Entry &e : g_entries)
            {
                if (word(rdram, e.object + 8u) != e.chars)
                {
                    allThere = false;
                    continue;
                }
                const std::string kb = keyboardText(e.original, keys);
                const std::string &want = keyboard && kb.size() <= e.original.size() ? kb : e.original;
                const uint32_t size = word(rdram, e.object + 4u);
                const std::string now(reinterpret_cast<const char *>(rdram + e.chars), std::min<uint32_t>(size, 4096u));
                if (now == want)
                    continue;
                if (now != e.original && now != kb)
                {
                    // (a text that is neither: an old layout's - written over only where it is no longer than the original)
                    if (size > e.original.size())
                    {
                        allThere = false;
                        continue;
                    }
                }
                static int told = 24;
                if (told > 0)
                {
                    --told;
                    std::fprintf(stderr, "[key text] \"%s\" -> \"%s\"\n", now.c_str(), want.c_str());
                }
                std::memset(rdram + e.chars, 0, e.original.size() + 1u);
                std::memcpy(rdram + e.chars, want.data(), want.size());
                setWord(rdram, e.object + 4u, static_cast<uint32_t>(want.size()));
                ++changed;
            }
            if (changed)
                std::fprintf(stderr, "[key text] %d strings for the %s\n", changed, keyboard ? "keyboard" : "controller");
            return allThere;
        }
    }

    void frame(uint8_t *rdram)
    {
        if (!enabled())
            return;
        const auto now = std::chrono::steady_clock::now();
        const bool keyboard = ps2_host_input::keyboardPromptsShown();
        // (Nothing to do until the keyboard is used: the controller's words are the game's own.)
        if (!g_searched && !g_searching.load())
        {
            if (!keyboard || (g_searches > 0 && now - g_lastSearch < std::chrono::seconds(20)) || g_searches >= 12)
                return;
            ++g_searches;
            g_lastSearch = now;
            g_searching = true;
            std::thread([rdram] {
                search(rdram);
                g_searching = false;
            }).detach();
            return;
        }
        if (g_searching.load())
            return;
        bool empty;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            empty = g_entries.empty();
        }
        if (empty)
        {
            // (the table not loaded yet when the keyboard was first used: looked for again a while later)
            if (keyboard && g_searches < 12 && now - g_lastSearch >= std::chrono::seconds(20))
                g_searched = false;
            return;
        }
        const std::string layoutText = mod_menu::playerKeyLayout();
        // On a change of device or layout at once; else checked once a second (a state loaded puts back what it held).
        if (keyboard == g_shown && layoutText == g_shownLayout && now - g_lastCheck < std::chrono::seconds(1))
            return;
        g_lastCheck = now;
        if (!apply(rdram, keyboard, layoutText) && g_searches < 12 && now - g_lastSearch >= std::chrono::seconds(20))
            g_searched = false; // (search again)
        g_shown = keyboard;
        g_shownLayout = layoutText;
    }
}
