// The save-state and recording manager (state_manager.h, TODO 156).

#include "state_manager.h"

#include "menu_style.h"
#include "playrec.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_savestate.h"

#include "imgui.h"
#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>

namespace fs = std::filesystem;

namespace state_manager
{
    namespace
    {
        double now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // ---- the files ----------------------------------------------------------------------------------
        struct Item
        {
            fs::path path;
            std::string name;  // shown (the file's or folder's name without its extension)
            std::time_t when = 0;
            uint64_t bytes = 0u;
            std::string detail; // a recording's length and start
            bool quick = false; // the F6 / F9 slot
        };

        std::time_t modified(const fs::path &p)
        {
#if defined(_WIN32)
            struct _stat64 st{};
            return _wstat64(p.wstring().c_str(), &st) == 0 ? static_cast<std::time_t>(st.st_mtime) : 0;
#else
            struct stat st{};
            return stat(p.string().c_str(), &st) == 0 ? st.st_mtime : 0;
#endif
        }

        std::string timeText(std::time_t t, const char *format)
        {
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[64];
            std::strftime(buf, sizeof(buf), format, &tm);
            return buf;
        }

        std::string sizeText(uint64_t bytes)
        {
            char buf[32];
            if (bytes >= 1024u * 1024u)
                std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
            else
                std::snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>((bytes + 1023u) / 1024u));
            return buf;
        }

        // A name as a file name: letters, digits, spaces and - _ . ( ) ' kept, the rest made '_', 60 at most.
        std::string fileName(const std::string &name)
        {
            std::string out;
            for (const char c : name)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                out += (std::isalnum(u) || std::strchr(" -_.()'", c)) && u >= 32u ? c : '_';
                if (out.size() >= 60u)
                    break;
            }
            while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
                out.pop_back();
            while (!out.empty() && out.front() == ' ')
                out.erase(out.begin());
            return out;
        }

        // A path not taken yet: `<stem><ext>`, else `<stem> (2)<ext>`, ...
        fs::path freePath(const fs::path &folder, const std::string &stem, const std::string &ext)
        {
            std::error_code ec;
            fs::path p = folder / (stem + ext);
            for (int n = 2; fs::exists(p, ec) && n < 1000; ++n)
                p = folder / (stem + " (" + std::to_string(n) + ")" + ext);
            return p;
        }

        fs::path quickPath() { return fs::absolute(ps2_savestate::quickStatePath()); }
        // (beside the quick state; the quick state already in a states folder - run.ps1's: that one)
        fs::path statesFolder()
        {
            const fs::path folder = quickPath().parent_path();
            return folder.filename() == "states" ? folder : folder / "states";
        }

        std::vector<Item> scanStates()
        {
            std::vector<Item> items;
            std::error_code ec;
            if (const fs::path q = quickPath(); fs::is_regular_file(q, ec))
                items.push_back({q, "Quick state (F6 saves, F9 loads)", modified(q), static_cast<uint64_t>(fs::file_size(q, ec)), {}, true});
            std::vector<Item> named;
            for (const auto &e : fs::directory_iterator(statesFolder(), ec))
                if (e.is_regular_file(ec) && e.path().extension() == ".ps2state")
                    named.push_back({e.path(), e.path().stem().string(), modified(e.path()), static_cast<uint64_t>(e.file_size(ec)), {}, false});
            std::sort(named.begin(), named.end(), [](const Item &a, const Item &b) { return a.when > b.when; });
            items.insert(items.end(), named.begin(), named.end());
            return items;
        }

        std::vector<Item> scanRecordings()
        {
            std::vector<Item> items;
            std::error_code ec;
            for (const auto &e : fs::directory_iterator(fs::path(playrec::recordingsFolder()), ec))
            {
                if (!e.is_directory(ec) || e.path().extension() != ".tfrec")
                    continue;
                Item it{e.path(), e.path().stem().string(), modified(e.path() / "header.txt"), 0u, {}, false};
                std::string seconds, start, frames;
                std::ifstream header(e.path() / "header.txt");
                for (std::string line; std::getline(header, line);)
                {
                    if (line.rfind("seconds=", 0) == 0)
                        seconds = line.substr(8);
                    else if (line.rfind("start=", 0) == 0)
                        start = line.substr(6);
                    else if (line.rfind("frames=", 0) == 0)
                        frames = line.substr(7);
                }
                if (!seconds.empty())
                {
                    const double s = std::atof(seconds.c_str());
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
                    it.detail = std::string(buf) + (start == "boot" ? ", from the start" : "") + (frames.empty() ? "" : ", " + frames + " frames");
                }
                else
                    it.detail = "not finished";
                for (const auto &f : fs::recursive_directory_iterator(e.path(), ec))
                    if (f.is_regular_file(ec))
                        it.bytes += static_cast<uint64_t>(f.file_size(ec));
                if (it.when == 0)
                    it.when = modified(e.path());
                items.push_back(std::move(it));
            }
            std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.when > b.when; });
            return items;
        }

        // ---- a list's state (one for the states, one for the recordings) --------------------------------
        struct List
        {
            std::vector<Item> items;
            double scannedAt = -1.0;
            int renaming = -1;          // the row being renamed (its name in `rename`)
            bool focusRename = false;   // (its field takes the keyboard once)
            char rename[96] = {};
            int confirmDelete = -1;     // the row whose Delete was clicked once
            double confirmUntil = 0.0;
            std::string status;         // the last action's result
            double statusAt = 0.0;
            bool statusBad = false;
        };
        List g_states, g_recordings;

        void say(List &l, const std::string &text, bool bad = false)
        {
            l.status = text;
            l.statusAt = now();
            l.statusBad = bad;
            std::fprintf(stderr, "[states] %s\n", text.c_str());
        }

        void showStatus(const List &l)
        {
            if (l.status.empty() || now() - l.statusAt > 12.0)
                return;
            if (l.statusBad)
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", l.status.c_str());
            else
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", l.status.c_str());
        }

        // A request to the scheduler, waited for: its counters at the request, the answer when one moves.
        struct Pending
        {
            enum Kind
            {
                None,
                Save,
                Load
            } kind = None;
            uint64_t done = 0u, failed = 0u;
            std::string name;
            double since = 0.0;
        };
        Pending g_pending;

        void checkPending(PS2Runtime &runtime)
        {
            if (g_pending.kind == Pending::None)
                return;
            const EeScheduler &ee = runtime.eeScheduler();
            if (g_pending.kind == Pending::Save)
            {
                if (ee.savedStateCount() != g_pending.done)
                {
                    say(g_states, "Saved: " + g_pending.name);
                    g_pending.kind = Pending::None;
                    g_states.scannedAt = -1.0;
                }
                else if (ee.failedStateSaveCount() != g_pending.failed)
                {
                    say(g_states, "Not saved: the game cannot be saved at this moment (a loading screen?) - try again", true);
                    g_pending.kind = Pending::None;
                }
            }
            else
            {
                if (ee.loadedStateCount() != g_pending.done)
                {
                    say(g_states, "Loaded: " + g_pending.name);
                    g_pending.kind = Pending::None;
                }
                else if (ee.refusedStateLoadCount() != g_pending.failed)
                {
                    say(g_states, "Not loaded: " + ee.lastRefusedStateLoad(), true);
                    g_pending.kind = Pending::None;
                }
            }
            if (g_pending.kind != Pending::None && now() - g_pending.since > 20.0)
            {
                say(g_states, "No answer from the game for " + g_pending.name + " (paused in a movie?)", true);
                g_pending.kind = Pending::None;
            }
        }

        char g_newName[96] = {};

        // The actions (the buttons' and the test aid's).
        bool renameItem(List &l, const Item &it, const std::string &name, bool folder)
        {
            std::error_code ec;
            const std::string stem = fileName(name);
            if (stem.empty())
            {
                say(l, "A name with a letter or a digit, please", true);
                return false;
            }
            const std::string ext = folder ? ".tfrec" : ".ps2state";
            if (it.quick)
            {
                // (the quick state kept under a name: copied into the states folder, F6's slot left as it is)
                fs::create_directories(statesFolder(), ec);
                const fs::path to = freePath(statesFolder(), stem, ext);
                fs::copy_file(it.path, to, ec);
                say(l, ec ? "Not copied: " + ec.message() : "Kept as: " + to.stem().string(), static_cast<bool>(ec));
                return true;
            }
            if (stem == it.path.stem().string())
                return false;
            const fs::path to = freePath(it.path.parent_path(), stem, ext);
            fs::rename(it.path, to, ec);
            say(l, ec ? "Not renamed: " + ec.message() : "Renamed to: " + to.stem().string(), static_cast<bool>(ec));
            return true;
        }

        bool deleteItem(List &l, const Item &it, bool folder)
        {
            std::error_code ec;
            if (folder)
                fs::remove_all(it.path, ec);
            else
                fs::remove(it.path, ec);
            say(l, ec ? "Not deleted: " + ec.message() : "Deleted: " + it.name, static_cast<bool>(ec));
            return true;
        }

        bool saveNamed(EeScheduler &ee, const std::string &name)
        {
            if (g_pending.kind != Pending::None)
                return false;
            std::string stem = fileName(name);
            if (stem.empty())
                stem = "state " + timeText(std::time(nullptr), "%Y-%m-%d %H.%M.%S");
            std::error_code ec;
            fs::create_directories(statesFolder(), ec);
            const fs::path to = freePath(statesFolder(), stem, ".ps2state");
            g_pending = {Pending::Save, ee.savedStateCount(), ee.failedStateSaveCount(), to.stem().string(), now()};
            ee.requestSaveState(to);
            return true;
        }

        bool loadItem(EeScheduler &ee, const Item &it)
        {
            if (g_pending.kind != Pending::None)
                return false;
            g_pending = {Pending::Load, ee.loadedStateCount(), ee.refusedStateLoadCount(), it.name, now()};
            ee.requestLoadState(it.path);
            return true;
        }

        // The rename / delete buttons of a row (and the rename field when it is being renamed). Returns true when the
        // list changed (rescan).
        bool rowActions(List &l, int i, bool folder, const char *renameLabel = "Rename")
        {
            Item &it = l.items[static_cast<size_t>(i)];
            if (l.renaming == i)
            {
                ImGui::SetNextItemWidth(360.0f);
                if (l.focusRename)
                {
                    ImGui::SetKeyboardFocusHere();
                    l.focusRename = false;
                }
                const bool enter = ImGui::InputText("##rename", l.rename, sizeof(l.rename), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                const bool ok = menu_style::smallButton("OK") || enter;
                ImGui::SameLine();
                if (menu_style::smallButton("Cancel"))
                {
                    l.renaming = -1;
                    return false;
                }
                if (!ok)
                    return false;
                l.renaming = -1;
                return renameItem(l, it, l.rename, folder);
            }
            ImGui::SameLine();
            if (menu_style::smallButton(renameLabel))
            {
                l.renaming = i;
                l.focusRename = true;
                std::snprintf(l.rename, sizeof(l.rename), "%s", it.quick ? ("state " + timeText(it.when, "%Y-%m-%d %H.%M")).c_str() : it.name.c_str());
                l.confirmDelete = -1;
            }
            ImGui::SameLine();
            const bool confirming = l.confirmDelete == i && ImGui::GetTime() < l.confirmUntil;
            if (menu_style::smallButton(confirming ? "Sure? Delete" : "Delete"))
            {
                if (!confirming)
                {
                    l.confirmDelete = i;
                    l.confirmUntil = ImGui::GetTime() + 4.0;
                }
                else
                {
                    l.confirmDelete = -1;
                    return deleteItem(l, it, folder);
                }
            }
            return false;
        }

        void rescanIfDue(List &l, std::vector<Item> (*scan)())
        {
            if (l.scannedAt < 0.0 || ImGui::GetTime() - l.scannedAt > 3.0)
            {
                // (not while a row is being renamed or confirmed: the rows keep their places)
                if (l.renaming < 0 && !(l.confirmDelete >= 0 && ImGui::GetTime() < l.confirmUntil))
                {
                    l.items = scan();
                    l.scannedAt = ImGui::GetTime();
                }
            }
        }

        void (*g_close)() = nullptr;
    }

    void frame(PS2Runtime &runtime)
    {
        checkPending(runtime);
        // (TF_STATES_TEST="<vblank>:<action>[:<name>[:<name>]]|...", a test aid: the page's actions at those VBlanks -
        // save:<name>, load:<name> (a state's shown name, "quick" the quick one, "latest" the newest of the list), rename:<name>:<new>, delete:<name>,
        // record (the Start button), recrename:<name>:<new>, recdelete:<name>; each result logged as [states] ...)
        static std::vector<std::vector<std::string>> steps = []()
        {
            std::vector<std::vector<std::string>> v;
            if (const char *e = std::getenv("TF_STATES_TEST"))
            {
                std::string all(e), item;
                for (size_t at = 0; at <= all.size(); ++at)
                    if (at == all.size() || all[at] == '|')
                    {
                        std::vector<std::string> parts;
                        size_t from = 0;
                        for (size_t k = 0; k <= item.size(); ++k)
                            if (k == item.size() || item[k] == ':')
                            {
                                parts.push_back(item.substr(from, k - from));
                                from = k + 1;
                            }
                        if (parts.size() >= 2)
                            v.push_back(parts);
                        item.clear();
                    }
                    else
                        item += all[at];
            }
            return v;
        }();
        if (steps.empty() || g_pending.kind != Pending::None)
            return;
        EeScheduler &ee = runtime.eeScheduler();
        const std::vector<std::string> &s = steps.front();
        if (ee.currentVSyncTick() < std::strtoull(s[0].c_str(), nullptr, 10))
            return;
        const std::string what = s[1], a = s.size() > 2 ? s[2] : std::string(), b = s.size() > 3 ? s[3] : std::string();
        std::fprintf(stderr, "[states] TF_STATES_TEST: %s %s %s (vblank %llu)\n", what.c_str(), a.c_str(), b.c_str(),
                     static_cast<unsigned long long>(ee.currentVSyncTick()));
        const bool rec = what.rfind("rec", 0) == 0 && what != "record";
        List &l = rec ? g_recordings : g_states;
        l.items = rec ? scanRecordings() : scanStates();
        const Item *found = nullptr;
        for (const Item &it : l.items)
            if (it.name == a || (a == "quick" && it.quick) || (a == "latest" && !found && !it.quick))
                found = &it;
        if (what == "save")
            saveNamed(ee, a);
        else if (what == "record")
            playrec::startWhenMenuClosed();
        else if (!found)
            say(l, "TF_STATES_TEST: no " + a, true);
        else if (what == "load")
            loadItem(ee, *found);
        else if (what == "rename" || what == "recrename")
            renameItem(l, *found, b, rec);
        else if (what == "delete" || what == "recdelete")
            deleteItem(l, *found, rec);
        l.scannedAt = -1.0;
        steps.erase(steps.begin());
    }

    void drawStates(PS2Runtime &runtime, void (*close)())
    {
        g_close = close;
        rescanIfDue(g_states, scanStates);
        EeScheduler &ee = runtime.eeScheduler();

        // A new one, under a name.
        if (g_newName[0] == '\0')
            std::snprintf(g_newName, sizeof(g_newName), "state %s", timeText(std::time(nullptr), "%Y-%m-%d %H.%M.%S").c_str());
        ImGui::TextUnformatted("New state:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(440.0f);
        // (Tests, TF_MENU_TYPE_TEST - mod_menu.cpp: the box focused once, and each change of its text logged)
        static const bool typeTest = std::getenv("TF_MENU_TYPE_TEST") != nullptr;
        static bool typeFocused = false;
        if (typeTest && !typeFocused)
        {
            typeFocused = true;
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputText("##newstate", g_newName, sizeof(g_newName)) && typeTest)
            std::fprintf(stderr, "[state manager] the new state's name now: \"%s\"\n", g_newName);
        if (ImGui::IsItemHovered())
            menu_style::setHelp("Its name - what it is for (\"134 sky bar\"). Saved in the states folder.");
        ImGui::SameLine();
        const bool busy = g_pending.kind != Pending::None;
        if (menu_style::smallButton(busy ? "Waiting..." : "Save now") && !busy && saveNamed(ee, g_newName))
            g_newName[0] = '\0'; // (the next one named afresh)
        if (ImGui::IsItemHovered())
            menu_style::setHelp("Saves the game as it is now (behind this menu) under that name. F6 saves the quick state instead.");
        showStatus(g_states);

        if (g_states.items.empty())
            ImGui::TextDisabled("No save states yet.");
        for (int i = 0; i < static_cast<int>(g_states.items.size()); ++i)
        {
            const Item &it = g_states.items[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            if (it.quick)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", it.name.c_str());
            else
                ImGui::TextUnformatted(it.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s, %s", timeText(it.when, "%d %b %H:%M").c_str(), sizeText(it.bytes).c_str());
            ImGui::SameLine();
            if (menu_style::smallButton("Load") && loadItem(ee, it) && g_close)
                g_close();
            if (ImGui::IsItemHovered())
                menu_style::setHelp("The game goes on from this state (the menu closes). A state made by an older build may be refused.");
            const bool changed = rowActions(g_states, i, false, it.quick ? "Keep as..." : "Rename");
            ImGui::PopID();
            if (changed)
            {
                g_states.scannedAt = -1.0;
                break;
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        if (menu_style::smallButton("Open the states folder"))
        {
            std::error_code ec;
            fs::create_directories(statesFolder(), ec);
            OpenURL(statesFolder().string().c_str());
        }
    }

    void drawRecordings(void (*close)())
    {
        rescanIfDue(g_recordings, scanRecordings);
        if (playrec::recording())
            ImGui::TextUnformatted("Recording now - F2 stops it.");
        else if (playrec::canStartRecording())
        {
            if (menu_style::button(playrec::startPending() ? "Starting when the menu closes..." : "Start recording  (F2)", true))
            {
                playrec::startWhenMenuClosed();
                if (close)
                    close();
            }
            if (ImGui::IsItemHovered())
                menu_style::setHelp("Closes the menu and records your play from here (a save state is made at the start). F2 stops it; "
                                    "opening this menu, photo mode or loading a state ends it too.");
        }
        else if (playrec::replaying())
            ImGui::TextDisabled("A recording is being played back.");
        else
            ImGui::TextWrapped("Play recording is off: turn it on in Gameplay (\"Play recording (F2)\"); it takes effect from the next "
                               "start.");
        showStatus(g_recordings);

        if (g_recordings.items.empty())
            ImGui::TextDisabled("No recordings yet (%s).", playrec::recordingsFolder().c_str());
        for (int i = 0; i < static_cast<int>(g_recordings.items.size()); ++i)
        {
            const Item &it = g_recordings.items[static_cast<size_t>(i)];
            ImGui::PushID(1000 + i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(it.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s, %s, %s", timeText(it.when, "%d %b %H:%M").c_str(), it.detail.c_str(), sizeText(it.bytes).c_str());
            const bool changed = rowActions(g_recordings, i, true);
            ImGui::PopID();
            if (changed)
            {
                g_recordings.scannedAt = -1.0;
                break;
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        if (menu_style::smallButton("Open the recordings folder"))
        {
            std::error_code ec;
            fs::create_directories(fs::path(playrec::recordingsFolder()), ec);
            OpenURL(playrec::recordingsFolder().c_str());
        }
    }
}
