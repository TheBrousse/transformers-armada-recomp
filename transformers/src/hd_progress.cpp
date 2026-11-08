// The HD textures while they are still being made (TODO 219): hd_progress.h.
// (Its own file: windows.h and raylib.h do not go in one translation unit.)

#include "hd_progress.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace nr
{
    std::filesystem::path hdTextureFolder(); // (nr_hires.cpp)
    bool crispFontsFound();                  // (nr_hires.cpp)
}

namespace hd_progress
{
    namespace
    {
        std::atomic<uint64_t> g_generation{0};
        std::mutex g_mutex;
        std::string g_line;
        fs::path g_dir;

        struct Status
        {
            bool ok = false;
            std::string state;
            long long done = 0, total = 0;
        };

        // A field of the status file (a flat JSON object the tools write): its text, quotes taken off.
        std::string field(const std::string &text, const char *name)
        {
            const std::string key = std::string("\"") + name + "\"";
            size_t at = text.find(key);
            if (at == std::string::npos)
                return {};
            at = text.find(':', at + key.size());
            if (at == std::string::npos)
                return {};
            ++at;
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
                ++at;
            if (at < text.size() && text[at] == '"')
            {
                const size_t end = text.find('"', at + 1);
                return end == std::string::npos ? std::string() : text.substr(at + 1, end - at - 1);
            }
            size_t end = at;
            while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\r' && text[end] != '\n')
                ++end;
            return text.substr(at, end - at);
        }

        Status readStatus(const fs::path &file)
        {
            Status s;
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return s;
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            s.state = field(text, "state");
            s.done = std::atoll(field(text, "done").c_str());
            s.total = std::atoll(field(text, "total").c_str());
            s.ok = !s.state.empty();
            return s;
        }

        void setLine(const Status &s)
        {
            std::string line;
            if (s.ok && s.state != "done" && s.total > 0)
            {
                char text[96];
                std::snprintf(text, sizeof text, "HD TEXTURES %lld OF %lld%s", s.done, s.total,
                              s.state == "paused" ? " (PAUSED WHILE YOU PLAY)" : "");
                line = text;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            g_line = std::move(line);
        }

#if defined(_WIN32)
        HANDLE g_running = INVALID_HANDLE_VALUE;
        DWORD g_ourMaker = 0; // (the window this game started: closed by the player, it is not started again)

        bool pidAlive(DWORD pid)
        {
            if (!pid)
                return false;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h)
                return false;
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            CloseHandle(h);
            return alive;
        }

        // Whether a maker holds hd_textures\.upscaling (its process id inside; deleted when it ends).
        bool makerRunning(const fs::path &dir, DWORD *pidOut = nullptr)
        {
            const fs::path lock = dir / ".upscaling";
            std::error_code ec;
            if (!fs::exists(lock, ec))
                return false;
            HANDLE h = CreateFileW(lock.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return true; // (there, and not to be read: held)
            char buf[32] = {};
            DWORD got = 0;
            ReadFile(h, buf, sizeof buf - 1, &got, nullptr);
            CloseHandle(h);
            const DWORD pid = static_cast<DWORD>(std::strtoul(buf, nullptr, 10));
            if (pidOut)
                *pidOut = pid;
            return pid ? pidAlive(pid) : true;
        }

        // The minimized window that carries on with the set (make_hd_textures.ps1 -Resume).
        void startMaker(const fs::path &game)
        {
            const fs::path script = game / "tools" / "make_hd_textures.ps1";
            std::error_code ec;
            if (!fs::exists(script, ec))
                return;
            std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File \"" + script.wstring() + L"\" -Resume";
            STARTUPINFOW si{};
            si.cb = sizeof si;
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWMINNOACTIVE;
            PROCESS_INFORMATION pi{};
            const std::wstring dir = game.wstring();
            // (out of the game's job, where it has one - a launcher's - so that it carries on after the game closes)
            for (const DWORD extra : {static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB), static_cast<DWORD>(0)})
            {
                std::wstring line = cmd;
                if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | BELOW_NORMAL_PRIORITY_CLASS | extra,
                                   nullptr, dir.c_str(), &si, &pi))
                {
                    g_ourMaker = pi.dwProcessId;
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    std::fprintf(stderr, "[hd textures] the set is not finished: a window carries on with it (it waits while the game runs)\n");
                    return;
                }
            }
            std::fprintf(stderr, "[hd textures] the window to carry on with the set could not be started (error %lu)\n", GetLastError());
        }
#endif

        fs::path findDir()
        {
            std::error_code ec;
            if (fs::is_directory(fs::path("hd_textures"), ec))
                return fs::absolute(fs::path("hd_textures"), ec);
            return {};
        }

        std::atomic<bool> g_watching{false};

        // (fromNow: the state the file holds now is not news - a set done already, checked again from the F1 menu)
        void watch(fs::path status, bool helper, bool fromNow)
        {
            fs::file_time_type last{};
            if (fromNow)
            {
                std::error_code ec0;
                last = fs::last_write_time(status, ec0);
            }
            // (A maker seen at work that went away while the set is unfinished - the setup's window closed during play:
            // the game's own window carries on, at most three times a run.)
            bool seenMaker = false;
            int relaunched = 0;
            for (unsigned n = 1;; ++n)
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
#if defined(_WIN32)
                if (helper && n % 6u == 0u)
                {
                    DWORD pid = 0;
                    if (makerRunning(g_dir, &pid))
                        seenMaker = pid != g_ourMaker; // (a window of our own closed: the player's choice)
                    else if (seenMaker && relaunched < 3)
                    {
                        seenMaker = false;
                        ++relaunched;
                        startMaker(g_dir.parent_path());
                    }
                }
#endif
                std::error_code ec;
                const fs::file_time_type t = fs::last_write_time(status, ec);
                if (ec || t == last)
                    continue;
                last = t;
                const Status s = readStatus(status);
                setLine(s);
                g_generation.fetch_add(1, std::memory_order_relaxed);
                if (s.ok && s.state == "done")
                {
                    std::fprintf(stderr, "[hd textures] the set is done (%lld images)\n", s.total);
                    g_watching = false;
                    return;
                }
            }
        }
    }

    namespace
    {
        // The sign that the game runs, for the upscaler (deleted by Windows when the game ends, a crash too).
        void holdRunningSign()
        {
#if defined(_WIN32)
            if (g_running != INVALID_HANDLE_VALUE)
                return;
            const fs::path running = g_dir / ".game_running";
            g_running = CreateFileW(running.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (g_running != INVALID_HANDLE_VALUE)
            {
                const std::string pid = std::to_string(GetCurrentProcessId());
                DWORD wrote = 0;
                WriteFile(g_running, pid.data(), static_cast<DWORD>(pid.size()), &wrote, nullptr);
                FlushFileBuffers(g_running);
            }
#endif
        }

        void startWatch(bool helper, bool fromNow)
        {
            if (g_watching.exchange(true))
                return;
            const fs::path status = g_dir / "upscale_status.json";
            std::thread([status, helper, fromNow] { watch(status, helper, fromNow); }).detach();
        }
    }

    void start()
    {
        g_dir = findDir();
        if (g_dir.empty())
            return;
        holdRunningSign();
        const fs::path status = g_dir / "upscale_status.json";
        const Status s = readStatus(status);
        if (!s.ok || s.state == "done")
            return; // (a finished set, or one made before the status file: nothing to do)
        setLine(s);
        std::fprintf(stderr, "[hd textures] being made: %lld of %lld (%s)\n", s.done, s.total, s.state.c_str());
        const char *off = std::getenv("TF_HD_HELPER");
        const bool helper = !(off && *off == '0');
#if defined(_WIN32)
        if (helper && !makerRunning(g_dir))
            startMaker(g_dir.parent_path());
#endif
        startWatch(helper, false);
    }

    std::string makeLabel()
    {
        std::error_code ec;
        const fs::path dir = g_dir.empty() ? fs::absolute(fs::path("hd_textures"), ec) : g_dir;
        const Status s = readStatus(dir / "upscale_status.json");
        const bool made = s.ok ? s.state == "done" : fs::is_directory(dir / "upscaled", ec);
        return made ? "Check HD textures" : "Make HD textures";
    }

    std::string makeHdTextures()
    {
#if defined(_WIN32)
        std::error_code ec;
        if (g_dir.empty())
        {
            fs::create_directories(fs::path("hd_textures"), ec);
            g_dir = fs::absolute(fs::path("hd_textures"), ec);
        }
        if (!fs::exists(g_dir.parent_path() / "tools" / "make_hd_textures.ps1", ec))
            return "Not here: tools\\make_hd_textures.ps1 is missing";
        holdRunningSign(); // (so that the upscale waits while the game runs)
        if (makerRunning(g_dir))
            return "Being made already, in its own window";
        startMaker(g_dir.parent_path());
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_line.empty())
                g_line = "HD TEXTURES: STARTING";
        }
        startWatch(true, true);
        std::ifstream o(g_dir / "hd_options.json", std::ios::binary);
        std::stringstream ss;
        ss << o.rdbuf();
        std::string t = ss.str();
        t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }), t.end());
        const bool whilePlaying = t.find("\"while_playing\":true") != std::string::npos;
        return std::string("Started in a small window.") + (whilePlaying ? "" : " It waits while the game runs.") +
               " The new ones show from the next start.";
#else
        return "Run tools/make_hd_textures.ps1";
#endif
    }

    uint64_t generation() { return g_generation.load(std::memory_order_relaxed); }

    std::string line()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_line;
    }

    bool crispFontsFound()
    {
        static std::chrono::steady_clock::time_point at{};
        static bool found = false;
        const auto now = std::chrono::steady_clock::now();
        if (at == std::chrono::steady_clock::time_point{} || now - at > std::chrono::seconds(2))
        {
            at = now;
            found = nr::crispFontsFound();
        }
        return found;
    }

    std::string openTextureFolder()
    {
        const fs::path folder = nr::hdTextureFolder();
        if (folder.empty())
            return "No texture pack yet";
        std::error_code ec;
        const fs::path dir = fs::absolute(folder, ec);
        fs::path open = fs::is_directory(dir, ec) ? dir : fs::path();
        if (open.empty() && fs::is_directory(dir.parent_path(), ec))
            open = dir.parent_path();
#if defined(_WIN32)
        if (!open.empty())
        {
            const std::wstring w = open.wstring();
            ShellExecuteW(nullptr, L"explore", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
#endif
        std::fprintf(stderr, "[hd textures] texture folder: %s%s\n", dir.string().c_str(), open == dir ? "" : " (not there yet)");
        return open == dir ? std::string() : "No texture pack yet";
    }
}
