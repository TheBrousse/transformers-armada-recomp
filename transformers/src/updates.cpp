// Updates (TODO 104): updates.h has the long form.

#include "updates.h"

#include "guest_code.h"
#include "tf_version.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_settings.h"

#if defined(PS2X_ENABLE_DEBUG_UI)
#include "imgui.h"
#include "menu_style.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fs = std::filesystem;

namespace updates
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // ---------------------------------------------------------------------------------
        // Where things are.

        fs::path exeDir()
        {
            std::wstring buffer(32768, L'\0');
            const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            buffer.resize(n);
            return fs::path(buffer).parent_path();
        }

        // An installed copy, under git with update.ps1 at its top: a clone of the game's repository
        // (the game's folder itself: build.ps1 and transformers\ beside update.ps1), or a repository
        // that holds the game's folder as its port\ (port\ beside update.ps1).
        bool isRepo(const fs::path &dir)
        {
            std::error_code ec;
            if (!fs::exists(dir / "update.ps1", ec) || !fs::exists(dir / ".git", ec))
                return false;
            return fs::is_directory(dir / "port", ec) || (fs::exists(dir / "build.ps1", ec) && fs::is_directory(dir / "transformers", ec));
        }

        // The copy the game was built in (its exe is under out\build\... of the game's folder), or
        // TF_UPDATE_REPO's.
        const fs::path &repoRoot()
        {
            static const fs::path root = []() -> fs::path
            {
                if (const char *env = std::getenv("TF_UPDATE_REPO"); env && *env)
                    return isRepo(env) ? fs::path(env) : fs::path();
                fs::path dir = exeDir();
                for (int i = 0; i < 10 && !dir.empty(); ++i)
                {
                    if (isRepo(dir))
                        return dir;
                    if (dir == dir.parent_path())
                        break;
                    dir = dir.parent_path();
                }
                return {};
            }();
            return root;
        }

        // The game's folder (its own states\, settings.ini): the copy's port\ or the copy itself,
        // else the checkout the exe was built in, else the working folder.
        const fs::path &portRoot()
        {
            static const fs::path root = []() -> fs::path
            {
                std::error_code pec;
                if (!repoRoot().empty())
                    return fs::is_directory(repoRoot() / "port", pec) ? repoRoot() / "port" : repoRoot();
                std::error_code ec;
                fs::path dir = exeDir();
                for (int i = 0; i < 10 && !dir.empty(); ++i)
                {
                    if (fs::exists(dir / "build.ps1", ec) && fs::is_directory(dir / "transformers", ec))
                        return dir;
                    if (dir == dir.parent_path())
                        break;
                    dir = dir.parent_path();
                }
                return fs::current_path(ec);
            }();
            return root;
        }

        fs::path statesDir() { return portRoot() / "states"; }
        fs::path resumeState() { return statesDir() / "update_resume.ps2state"; }
        fs::path resumeMarker() { return statesDir() / "update_resume.json"; }
        fs::path resumeAttempt() { return statesDir() / "update_resume.attempt.json"; }
        fs::path updateResult() { return statesDir() / "update_result.txt"; } // (update.ps1 writes it)
        // The branch the player's own changes were committed to before an update (update.ps1 or
        // tfa-setup.exe write it: TODO 111), read and deleted at the next start.
        fs::path updateKept() { return statesDir() / "update_kept.txt"; }

        bool installed() { return std::strcmp(TF_VERSION_KIND, "install") == 0 || std::getenv("TF_UPDATE_REPO") != nullptr; }

        std::string shortHash(const std::string &h) { return h.substr(0, std::min<size_t>(h.size(), 7)); }

        std::string hex64(uint64_t v)
        {
            char text[24];
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
            return text;
        }

        std::wstring widen(const std::string &s)
        {
            if (s.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        void removeQuietly(const fs::path &p)
        {
            std::error_code ec;
            fs::remove(p, ec);
        }

        // ---------------------------------------------------------------------------------
        // Environments and processes.

        std::vector<std::wstring> g_launchEnvironment;

        std::vector<std::wstring> currentEnvironment()
        {
            std::vector<std::wstring> out;
            if (LPWCH block = GetEnvironmentStringsW())
            {
                for (const wchar_t *p = block; *p; p += wcslen(p) + 1)
                    out.emplace_back(p);
                FreeEnvironmentStringsW(block);
            }
            return out;
        }

        std::wstring nameOf(const std::wstring &variable)
        {
            return variable.substr(0, variable.find(L'=', 1)); // ("=C:=C:\..." names start with '=')
        }

        void setVariable(std::vector<std::wstring> &env, const std::wstring &name, const std::wstring &value)
        {
            env.erase(std::remove_if(env.begin(), env.end(), [&](const std::wstring &v)
                                     { return _wcsicmp(nameOf(v).c_str(), name.c_str()) == 0; }),
                      env.end());
            if (!value.empty())
                env.push_back(name + L"=" + value);
        }

        std::wstring environmentBlock(std::vector<std::wstring> env)
        {
            std::sort(env.begin(), env.end(), [](const std::wstring &a, const std::wstring &b)
                      { return CompareStringOrdinal(nameOf(a).c_str(), -1, nameOf(b).c_str(), -1, TRUE) == CSTR_LESS_THAN; });
            std::wstring block;
            for (const std::wstring &v : env)
            {
                block += v;
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }

        // What the updater (and the game it starts again) gets: the environment the game was started
        // with, without what would make the next start load or save something of its own.
        std::vector<std::wstring> childEnvironment()
        {
            std::vector<std::wstring> env = g_launchEnvironment.empty() ? currentEnvironment() : g_launchEnvironment;
            for (const wchar_t *name : {L"PS2X_LOAD_STATE", L"PS2X_LOAD_STATE_TICK", L"PS2X_RESTART_STATE", L"PS2X_SAVE_STATE",
                                        L"PS2X_SAVE_STATE_TICK", L"PS2X_SETTINGS_RESTART", L"TF_UPDATE_TEST", L"TF_RESUME_FAILED"})
                setVariable(env, name, L"");
            return env;
        }

        std::wstring quoted(const fs::path &p) { return L"\"" + p.wstring() + L"\""; }

        struct Ran
        {
            bool started = false;
            bool timedOut = false;
            DWORD code = 0;
            std::string out; // stdout and stderr together
        };

        // A program run hidden, its output read, killed after `timeout`.
        Ran runHidden(const std::wstring &commandLine, const std::vector<std::wstring> &environment, std::chrono::seconds timeout)
        {
            Ran r;
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            HANDLE readEnd = nullptr, writeEnd = nullptr;
            if (!CreatePipe(&readEnd, &writeEnd, &sa, 0))
                return r;
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdInput = nullptr;
            si.hStdOutput = writeEnd;
            si.hStdError = writeEnd;
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
            command.push_back(L'\0');
            std::wstring block = environmentBlock(environment);
            if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                block.data(), nullptr, &si, &pi))
            {
                CloseHandle(readEnd);
                CloseHandle(writeEnd);
                return r;
            }
            r.started = true;
            CloseHandle(writeEnd);
            CloseHandle(pi.hThread);
            const auto until = Clock::now() + timeout;
            char buffer[4096];
            for (;;)
            {
                DWORD available = 0;
                if (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) && available > 0)
                {
                    DWORD got = 0;
                    if (ReadFile(readEnd, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) && got > 0)
                        r.out.append(buffer, got);
                    continue;
                }
                if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0)
                {
                    // (what it wrote last)
                    DWORD got = 0;
                    while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) && available > 0 &&
                           ReadFile(readEnd, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) && got > 0)
                        r.out.append(buffer, got);
                    break;
                }
                if (Clock::now() > until)
                {
                    TerminateProcess(pi.hProcess, 1);
                    WaitForSingleObject(pi.hProcess, 2000);
                    r.timedOut = true;
                    break;
                }
            }
            GetExitCodeProcess(pi.hProcess, &r.code);
            CloseHandle(pi.hProcess);
            CloseHandle(readEnd);
            return r;
        }

        std::string trimmed(std::string s)
        {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            size_t start = 0;
            while (start < s.size() && (s[start] == ' ' || s[start] == '\n' || s[start] == '\r'))
                ++start;
            return s.substr(start);
        }

        std::string lastLine(const std::string &s)
        {
            const std::string t = trimmed(s);
            const size_t at = t.find_last_of('\n');
            return trimmed(at == std::string::npos ? t : t.substr(at + 1));
        }

        // git in the copy, hidden, never asking for anything.
        Ran git(const std::string &args, std::chrono::seconds timeout = std::chrono::seconds(60))
        {
            std::vector<std::wstring> env = currentEnvironment();
            setVariable(env, L"GIT_TERMINAL_PROMPT", L"0");
            setVariable(env, L"GCM_INTERACTIVE", L"never");
            setVariable(env, L"GIT_ASKPASS", L"");
            setVariable(env, L"SSH_ASKPASS", L"");
            if (!std::getenv("GIT_SSH_COMMAND"))
                setVariable(env, L"GIT_SSH_COMMAND", L"ssh -o BatchMode=yes");
            return runHidden(L"git -C " + quoted(repoRoot()) + L" " + widen(args), env, timeout);
        }

        // A version's number (TODO 111): "0.1.3" from the VERSION file a commit of the copy has
        // (VERSION in the game's folder); "" for a commit from before the file, or one that does not
        // read as a number.
        bool isVersionName(const std::string &v)
        {
            int dots = 0;
            bool digit = false;
            for (char ch : v)
            {
                if (ch == '.')
                {
                    if (!digit)
                        return false;
                    ++dots;
                    digit = false;
                }
                else if (ch >= '0' && ch <= '9')
                    digit = true;
                else
                    return false;
            }
            return dots == 2 && digit;
        }

        std::string versionAt(const std::string &commit)
        {
            if (commit.empty() || repoRoot().empty())
                return {};
            // (the game's folder in the repository: "" for a clone of the game's own, "port/" for one holding it)
            const std::string folder = portRoot() == repoRoot() ? "" : "port/";
            Ran r = git("show " + commit + ":" + folder + "VERSION");
            const std::string v = trimmed(r.out);
            return r.code == 0 && isVersionName(v) ? v : std::string();
        }

        // "v0.1.3" when there is a number, else the commit's short code.
        std::string displayName(const std::string &name, const std::string &commit)
        {
            return name.empty() ? shortHash(commit) : "v" + name;
        }

        // The version bump's own commit ("version 0.1.3", sync_home.ps1): bookkeeping, not a change.
        bool isVersionBump(const std::string &subject)
        {
            return subject.rfind("version ", 0) == 0 && isVersionName(subject.substr(8));
        }

        // A "key": "value" pair's value in a small JSON file (.setup.json, the resume marker).
        std::string field(const std::string &text, const char *key)
        {
            const std::string k = std::string("\"") + key + "\"";
            size_t at = text.find(k);
            if (at == std::string::npos)
                return {};
            at = text.find('"', text.find(':', at + k.size()) + 1);
            const size_t end = at == std::string::npos ? std::string::npos : text.find('"', at + 1);
            return end == std::string::npos ? std::string() : text.substr(at + 1, end - at - 1);
        }

        // ---------------------------------------------------------------------------------
        // The check.

        enum class Status
        {
            NotChecked,
            Checking,
            UpToDate,
            Available,
            Failed,
            NotInstalled, // a development checkout, or no copy of the repository found
        };

        struct Check
        {
            Status status = Status::NotChecked;
            std::string branch;
            std::string latest;               // origin/<branch>'s commit
            std::string latestName;           // its number (port/VERSION), "" before the file
            int count = 0;                    // commits the build does not have
            std::vector<std::string> changes; // "hash subject", newest first
            std::string error;
            std::time_t at = 0;
        };

        std::mutex g_checkLock;
        Check g_check;
        std::condition_variable g_wake;
        std::atomic<bool> g_checkNow{false};
        std::atomic<bool> g_auto{true};

        Check runCheck()
        {
            Check c;
            c.at = std::time(nullptr);
            if (!installed() || repoRoot().empty())
            {
                c.status = Status::NotInstalled;
                return c;
            }
            Ran r = git("rev-parse --abbrev-ref HEAD");
            if (!r.started)
            {
                c.status = Status::Failed;
                c.error = "Git was not found";
                return c;
            }
            c.branch = trimmed(r.out);
            if (r.code != 0 || c.branch.empty() || c.branch == "HEAD")
                c.branch = "main";
            r = git("fetch --quiet --no-tags origin " + c.branch, std::chrono::seconds(120));
            if (r.timedOut || r.code != 0)
            {
                c.status = Status::Failed;
                const std::string line = lastLine(r.out);
                if (r.timedOut)
                    c.error = "no answer from the repository (network?)";
                else if (line.find("could not read Username") != std::string::npos || line.find("Authentication") != std::string::npos ||
                         line.find("terminal prompts disabled") != std::string::npos)
                    c.error = "the repository asked for a sign-in: run update.ps1 once in the install folder (it asks), then check again";
                else
                    c.error = line.empty() ? "git fetch failed" : line;
                return c;
            }
            r = git("rev-parse --verify origin/" + c.branch);
            c.latest = trimmed(r.out);
            if (r.code != 0 || c.latest.empty())
            {
                c.status = Status::Failed;
                c.error = "the repository has no branch " + c.branch;
                return c;
            }
            c.latestName = versionAt(c.latest);
            std::string built = TF_VERSION_COMMIT;
            bool known = !built.empty() && git("cat-file -e " + built + "^{commit}").code == 0;
            // An update that needed no build (documentation only) leaves the exe at an older commit:
            // the copy is up to date as of the commit update.ps1 / setup.ps1 recorded (.setup.json's
            // SetUpAt), when that one comes after the build's.
            {
                std::ifstream in(repoRoot() / ".setup.json");
                std::stringstream text;
                text << in.rdbuf();
                const std::string setUpAt = field(text.str(), "SetUpAt");
                if (known && !setUpAt.empty() && setUpAt != built && git("merge-base --is-ancestor " + built + " " + setUpAt).code == 0)
                    built = setUpAt;
            }
            // (a build of origin's commit or of one after it - local commits - has nothing to get)
            if (built == c.latest || (known && git("merge-base --is-ancestor " + c.latest + " " + built).code == 0))
            {
                c.status = Status::UpToDate;
                return c;
            }
            c.status = Status::Available;
            const std::string range = known ? built + ".." + c.latest : c.latest;
            r = git("rev-list --count " + range);
            c.count = known ? std::max(1, std::atoi(r.out.c_str())) : 0;
            r = git("log --no-merges -n 40 --format=%h%x09%s " + range);
            std::istringstream lines(r.out);
            std::string line;
            while (std::getline(lines, line) && c.changes.size() < 15)
            {
                line = trimmed(line);
                const size_t tab = line.find('\t');
                if (tab == std::string::npos)
                    continue;
                const std::string subject = line.substr(tab + 1);
                if (subject.rfind("port: sync from the working copy", 0) == 0 || isVersionBump(subject)) // (bookkeeping)
                    continue;
                c.changes.push_back(line.substr(0, tab) + "  " + subject);
            }
            return c;
        }

        void checkerLoop()
        {
            const char *firstEnv = std::getenv("TF_UPDATE_CHECK_AT");
            auto next = Clock::now() + std::chrono::seconds(firstEnv && *firstEnv ? std::atoi(firstEnv) : 20);
            for (;;)
            {
                bool asked = false;
                {
                    std::unique_lock lock(g_checkLock);
                    g_wake.wait_until(lock, next, [] { return g_checkNow.load(); });
                    asked = g_checkNow.exchange(false);
                    if (!asked)
                    {
                        if (Clock::now() < next)
                            continue;
                        next = Clock::now() + std::chrono::hours(4); // (the automatic check's time)
                        if (!g_auto.load())
                            continue;
                    }
                    g_check.status = Status::Checking;
                }
                Check result = runCheck();
                std::string what = result.status == Status::UpToDate       ? "up to date"
                                   : result.status == Status::Available    ? "a new version is there, " + displayName(result.latestName, result.latest) + " (" +
                                                                              shortHash(result.latest) + ", " + std::to_string(result.count) + " commits)"
                                   : result.status == Status::NotInstalled ? "not an installed copy (no check)"
                                                                           : "could not check: " + result.error;
                std::fprintf(stderr, "[updates] %s: %s\n", asked ? "checked (asked)" : "checked", what.c_str());
                {
                    std::lock_guard lock(g_checkLock);
                    g_check = std::move(result);
                }
                next = Clock::now() + std::chrono::hours(4);
            }
        }

        void startChecker()
        {
            static std::once_flag once;
            std::call_once(once, []
                           { std::thread(checkerLoop).detach(); });
        }

        Check checkNowState()
        {
            std::lock_guard lock(g_checkLock);
            return g_check;
        }

        // ---------------------------------------------------------------------------------
        // The resume marker: a few "key": "value" pairs.

        struct Marker
        {
            std::string commit; // the build that made the state
            std::string layout; // its guest_code::layoutKey
            std::string made;
        };

        bool writeMarker(const fs::path &path, const Marker &m)
        {
            std::ofstream out(path, std::ios::trunc);
            out << "{\n  \"version\": \"1\",\n  \"state\": \"" << resumeState().filename().string() << "\",\n  \"commit\": \"" << m.commit
                << "\",\n  \"layout\": \"" << m.layout << "\",\n  \"made\": \"" << m.made << "\"\n}\n";
            return static_cast<bool>(out);
        }

        bool readMarker(const fs::path &path, Marker &m)
        {
            std::ifstream in(path);
            if (!in)
                return false;
            std::stringstream text;
            text << in.rdbuf();
            if (field(text.str(), "version") != "1")
                return false;
            m.commit = field(text.str(), "commit");
            m.layout = field(text.str(), "layout");
            m.made = field(text.str(), "made");
            return true;
        }

        // ---------------------------------------------------------------------------------
        // The update's steps and the start's messages (the window thread, but for the message).

        enum class Phase
        {
            None,
            Saving,   // the moment being saved
            Quitting, // the updater started: the game closes
            Failed,
        };
        Phase g_phase = Phase::None;
        bool g_phaseResume = true;
        uint64_t g_savedBefore = 0u, g_failedBefore = 0u;
        Clock::time_point g_savingSince;
        std::string g_phaseMessage;
        std::atomic<int> g_request{0}; // 1: update and resume, 2: update without resuming

        std::mutex g_messageLock;
        std::string g_startMessage;
        bool g_resumeRequested = false;

        void setStartMessage(const std::string &m)
        {
            std::lock_guard lock(g_messageLock);
            g_startMessage = m;
            std::fprintf(stderr, "[updates] %s\n", m.c_str());
        }

        std::string nowText()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm);
            return text;
        }

        // The updater, started detached: tfa-setup.exe in the install folder (a window with the steps),
        // else update.ps1 in a console window of its own. It waits for this process to end.
        bool launchUpdater(std::string &error)
        {
            const fs::path repo = repoRoot();
            if (repo.empty())
            {
                error = "this is not an installed copy of the game (no repository to update)";
                return false;
            }
            const DWORD pid = GetCurrentProcessId();
            const bool hidden = [] { const char *e = std::getenv("TF_UPDATE_HIDDEN"); return e && *e == '1'; }();
            std::error_code ec;
            const fs::path setup = repo.parent_path() / "tfa-setup.exe";
            std::wstring command;
            if (!hidden && fs::exists(setup, ec))
                command = quoted(setup) + L" --folder " + quoted(repo.parent_path()) + L" --update --play --wait-pid " + std::to_wstring(pid);
            else
                // (RemoteSigned: the copy's own scripts came with git, so they carry no Mark of the Web and run)
                command = L"powershell.exe -NoProfile -ExecutionPolicy RemoteSigned" + std::wstring(hidden ? L" -NonInteractive" : L"") +
                          L" -File " + quoted(repo / "update.ps1") + L" -WaitForPid " + std::to_wstring(pid) + L" -Play -StashLocalChanges";
            std::vector<wchar_t> line(command.begin(), command.end());
            line.push_back(L'\0');
            std::wstring block = environmentBlock(childEnvironment());
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | (hidden ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE);
            // (out of the game's job, when it is in one that allows it: the updater outlives the game)
            BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, block.data(),
                                     repo.wstring().c_str(), &si, &pi);
            if (!ok)
                ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, flags, block.data(), repo.wstring().c_str(), &si, &pi);
            if (!ok)
            {
                error = "Windows did not start the updater (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            std::fprintf(stderr, "[updates] the updater started: %ls\n", command.c_str());
            return true;
        }

        void clearResumeFiles()
        {
            removeQuietly(resumeMarker());
            removeQuietly(resumeAttempt());
            removeQuietly(resumeState());
        }
    }

    // ---------------------------------------------------------------------------------------------

    void noteLaunchEnvironment()
    {
        g_launchEnvironment = currentEnvironment();
    }

    void addSettings()
    {
        std::fprintf(stderr, "[updates] this version: %s\n", versionText().c_str());
        ps2_settings::Setting s;
        s.key = "TF_UPDATE_CHECK";
        s.section = "Updates"; // (the About page: mod_menu.cpp)
        s.label = "Check for updates automatically";
        s.help = "At start and every few hours, the game asks the repository whether a newer version is there (in the "
                 "background, never while you wait). A small notice says so; About shows what changed and updates.";
        s.choices = {{"1", "On"}, {"0", "Off"}};
        s.defaultValue = "1";
        s.apply = [](const std::string &value)
        {
            g_auto.store(value != "0");
            if (value != "0")
                g_wake.notify_all();
            return true;
        };
        ps2_settings::add(s);
        g_auto.store(ps2_settings::value("TF_UPDATE_CHECK") != "0");
        startChecker();
    }

    void beginResume(PS2Runtime &runtime)
    {
        std::error_code ec;
        std::string message;
        // What update.ps1 left: "ok <commit>" or "failed <why>".
        if (std::ifstream in(updateResult()); in)
        {
            std::string word, rest;
            in >> word;
            std::getline(in, rest);
            rest = trimmed(rest);
            if (word == "ok")
            {
                // (the number of the commit updated to; this build's own when it is that commit)
                std::string name = versionAt(rest);
                if (name.empty() && !rest.empty() && std::string(TF_VERSION_COMMIT).rfind(rest, 0) == 0)
                    name = TF_VERSION_NAME;
                message = "Updated to version " + displayName(name, rest) + ".";
            }
            else if (word == "failed")
                message = "The update did not finish: " + rest + " (update.log in the install folder). This is the version you had.";
            in.close();
            removeQuietly(updateResult());
        }
        if (std::ifstream in(updateKept()); in)
        {
            std::string branch;
            std::getline(in, branch);
            branch = trimmed(branch);
            in.close();
            removeQuietly(updateKept());
            if (!branch.empty())
                message += (message.empty() ? "" : " ") + std::string("Your own changes were kept on the branch ") + branch + "; nothing was lost.";
        }
        if (const char *failed = std::getenv("TF_RESUME_FAILED"); failed && *failed == '1')
        {
            message += (message.empty() ? "" : " ") +
                       std::string("The game could not go on from where you were (the new version cannot read that moment), so it "
                                   "started normally: your memory card saves are all there.");
        }
        else if (fs::exists(resumeAttempt(), ec))
        {
            // (a start that was to resume ended before it could: not tried again)
            message += (message.empty() ? "" : " ") +
                       std::string("The last start could not go on from where you were; this one starts normally: your memory "
                                   "card saves are all there.");
            clearResumeFiles();
        }
        Marker m;
        if (fs::exists(resumeMarker(), ec))
        {
            const bool off = [] { const char *e = std::getenv("TF_RESUME"); return e && *e == '0'; }();
            const std::string layout = hex64(guest_code::layoutKey());
            if (off)
                std::fprintf(stderr, "[updates] TF_RESUME=0: the resume left by the update is not loaded\n");
            else if (!readMarker(resumeMarker(), m) || !fs::exists(resumeState(), ec))
            {
                message += (message.empty() ? "" : " ") + std::string("The moment saved before the update is missing: the game starts normally.");
                clearResumeFiles();
            }
            else if (m.layout != layout)
            {
                std::fprintf(stderr, "[updates] the saved moment's added-code layout %s is not this build's %s\n", m.layout.c_str(),
                             layout.c_str());
                message += (message.empty() ? "" : " ") +
                           std::string("This version changed how the game's mods hook in, so it cannot go on from where you were: it "
                                       "starts normally (your memory card saves are all there).");
                clearResumeFiles();
            }
            else
            {
                fs::rename(resumeMarker(), resumeAttempt(), ec);
                if (ec)
                    removeQuietly(resumeMarker());
                runtime.eeScheduler().requestLoadState(resumeState());
                g_resumeRequested = true;
                std::fprintf(stderr, "[updates] resuming from %s (made %s by build %s)\n", resumeState().string().c_str(), m.made.c_str(),
                             shortHash(m.commit).c_str());
            }
        }
        if (!message.empty())
            setStartMessage(message);
    }

    bool afterRun(PS2Runtime &runtime)
    {
        std::error_code ec;
        if (!g_resumeRequested || runtime.eeScheduler().loadedStateCount() != 0u || !fs::exists(resumeAttempt(), ec))
            return false;
        // The runtime stopped at the load (it says why in the log): the game once more, normally.
        clearResumeFiles();
        std::vector<std::wstring> env = childEnvironment();
        setVariable(env, L"TF_RESUME_FAILED", L"1");
        std::wstring block = environmentBlock(env);
        const std::wstring commandLine = GetCommandLineW();
        std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
        command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        std::fprintf(stderr, "[updates] the saved moment did not load: the game starts again, normally\n");
        std::fflush(stdout);
        std::fflush(stderr);
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT, block.data(), nullptr, &startup,
                            &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    void frame(PS2Runtime &runtime)
    {
        EeScheduler &ee = runtime.eeScheduler();
        if (g_resumeRequested && ee.loadedStateCount() != 0u)
        {
            g_resumeRequested = false;
            clearResumeFiles();
            std::string message;
            {
                std::lock_guard lock(g_messageLock);
                message = g_startMessage;
            }
            setStartMessage((message.empty() ? "" : message + " ") + "You are back where you were.");
        }

        // TF_UPDATE_TEST=<vblank> (tests): "Update now" at that tick.
        static const uint64_t testAt = std::getenv("TF_UPDATE_TEST") ? std::strtoull(std::getenv("TF_UPDATE_TEST"), nullptr, 10) : 0u;
        static bool tested = false;
        if (testAt != 0u && !tested && ee.currentVSyncTick() >= testAt)
        {
            tested = true;
            std::fprintf(stderr, "[updates] TF_UPDATE_TEST: update now (vblank %llu)\n", static_cast<unsigned long long>(ee.currentVSyncTick()));
            g_request.store(1);
        }

        if (const int request = g_request.exchange(0); request != 0 && g_phase != Phase::Saving && g_phase != Phase::Quitting)
        {
            g_phaseMessage.clear();
            g_phaseResume = request == 1;
            clearResumeFiles();
            if (g_phaseResume)
            {
                std::error_code ec;
                fs::create_directories(statesDir(), ec);
                g_savedBefore = ee.savedStateCount();
                g_failedBefore = ee.failedStateSaveCount();
                g_savingSince = Clock::now();
                g_phase = Phase::Saving;
                ee.requestSaveState(resumeState());
                std::fprintf(stderr, "[updates] saving the moment before updating: %s\n", resumeState().string().c_str());
            }
            else
            {
                std::string error;
                if (launchUpdater(error))
                {
                    g_phase = Phase::Quitting;
                    runtime.requestStop();
                }
                else
                {
                    g_phase = Phase::Failed;
                    g_phaseMessage = "Could not update: " + error + ".";
                }
            }
        }

        if (g_phase == Phase::Saving)
        {
            if (ee.savedStateCount() != g_savedBefore)
            {
                Marker m{TF_VERSION_COMMIT, hex64(guest_code::layoutKey()), nowText()};
                std::string error;
                if (!writeMarker(resumeMarker(), m))
                    error = "cannot write " + resumeMarker().string();
                else if (launchUpdater(error))
                {
                    g_phase = Phase::Quitting;
                    std::fprintf(stderr, "[updates] the moment is saved: the game closes for the update\n");
                    runtime.requestStop();
                    return;
                }
                clearResumeFiles();
                g_phase = Phase::Failed;
                g_phaseMessage = "Could not update: " + error + ".";
            }
            else if (ee.failedStateSaveCount() != g_failedBefore || Clock::now() - g_savingSince > std::chrono::seconds(20))
            {
                clearResumeFiles();
                g_phase = Phase::Failed;
                g_phaseMessage = "The game cannot be saved at this moment (a movie or a loading screen): try again in a moment, or "
                                 "update without coming back here (you go on from your memory card save).";
            }
        }
    }

    bool updateAvailable()
    {
        return checkNowState().status == Status::Available;
    }

    std::string noticeText()
    {
        const Check c = checkNowState();
        if (c.status != Status::Available)
            return {};
        const std::string name = c.latestName.empty() ? std::string() : ": v" + c.latestName;
        return c.count > 0 ? "Update available" + name + " (" + std::to_string(c.count) + (c.count == 1 ? " change" : " changes") + ") - F1, About"
                           : "Update available" + name + " - F1, About";
    }

    std::string startMessage()
    {
        std::lock_guard lock(g_messageLock);
        return g_startMessage;
    }

    std::string versionText()
    {
        const std::string commit = TF_VERSION_COMMIT;
        const std::string name = TF_VERSION_NAME;
        const bool dev = !installed();
        if (commit.empty() && name.empty())
            return "unknown (built without git)";
        // "v0.1.3 (2026-11-01)"; a development build "v0.1.3 development build (2026-11-01, 1a2b3c4)";
        // before the VERSION file (TODO 111) the commit's code as before: "1a2b3c4 (2026-10-31)".
        std::string text = name.empty() ? shortHash(commit) : "v" + name + (dev ? " development build" : "");
        std::string details = TF_VERSION_DATE;
        if (!name.empty() && dev && !commit.empty())
            details += (details.empty() ? "" : ", ") + shortHash(commit);
        if (!details.empty())
            text += " (" + details + ")";
        if (TF_VERSION_LOCAL)
            text += " + changes of its own";
        return text;
    }

    void drawAbout(PS2Runtime &runtime, const std::function<void()> &settings)
    {
#if defined(PS2X_ENABLE_DEBUG_UI)
        (void)runtime;
        menu_style::heading("This version");
        ImGui::TextUnformatted(("Transformers PC version " + versionText()).c_str());
        if (installed() && !repoRoot().empty())
            ImGui::TextDisabled("Branch %s, commit %s, installed in %s", *TF_VERSION_BRANCH ? TF_VERSION_BRANCH : "?",
                                *TF_VERSION_COMMIT ? shortHash(TF_VERSION_COMMIT).c_str() : "?", repoRoot().string().c_str());
        else
            ImGui::TextDisabled("A development build (%s%s): it is not updated from here, git is.", *TF_VERSION_BRANCH ? "branch " : "",
                                TF_VERSION_BRANCH);
        if (const std::string m = startMessage(); !m.empty())
            ImGui::TextWrapped("%s", m.c_str());

        menu_style::heading("Updates");
        if (!installed() || repoRoot().empty())
        {
            ImGui::TextDisabled("Updates come with an install made by tfa-setup.exe or setup.ps1.");
            return;
        }
        if (settings)
            settings(); // (Check for updates automatically)
        const Check c = checkNowState();
        char when[32] = "";
        if (c.at != 0)
        {
            std::tm tm{};
            localtime_s(&tm, &c.at);
            std::strftime(when, sizeof(when), "%H:%M", &tm);
        }
        switch (c.status)
        {
        case Status::NotChecked:
            ImGui::TextDisabled("Not checked yet.");
            break;
        case Status::Checking:
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(menu_style::colour("amber")), "Checking...");
            break;
        case Status::UpToDate:
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(menu_style::colour("blue")), "Up to date (checked at %s).", when);
            break;
        case Status::Available:
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(menu_style::colour("amber")), "A new version is available: %s%s (checked at %s).",
                               displayName(c.latestName, c.latest).c_str(),
                               c.count > 0 ? (", " + std::to_string(c.count) + (c.count == 1 ? " change" : " changes")).c_str() : "", when);
            if (!c.latestName.empty())
                ImGui::TextDisabled("(commit %s)", shortHash(c.latest).c_str());
            break;
        case Status::Failed:
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(menu_style::colour("red")), "Could not check (at %s): %s", when, c.error.c_str());
            break;
        case Status::NotInstalled:
            break;
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        const bool busy = c.status == Status::Checking || g_phase == Phase::Saving || g_phase == Phase::Quitting;
        ImGui::BeginDisabled(busy);
        if (menu_style::button("Check now"))
        {
            g_checkNow.store(true);
            g_wake.notify_all();
        }
        if (c.status == Status::Available)
        {
            ImGui::SameLine();
            if (menu_style::button("Update now", true))
                g_request.store(1);
            if (ImGui::IsItemHovered())
                menu_style::setHelp("The game saves this moment, closes, updates (it is rebuilt on this PC: a few minutes) and starts "
                                    "again right here. Your mods, their choices and your settings stay as they are.");
            if (g_phase == Phase::Failed && g_phaseResume)
            {
                ImGui::SameLine();
                if (menu_style::button("Update without coming back here"))
                    g_request.store(2);
            }
        }
        ImGui::EndDisabled();
        if (g_phase == Phase::Saving)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(menu_style::colour("amber")), "Saving this moment...");
        else if (g_phase == Phase::Quitting)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(menu_style::colour("amber")), "The game closes for the update and comes back by itself.");
        else if (g_phase == Phase::Failed)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(menu_style::colour("red")), "%s", g_phaseMessage.c_str());
        if (c.status == Status::Available && !c.changes.empty())
        {
            menu_style::heading("What is new");
            for (const std::string &line : c.changes)
                ImGui::TextWrapped("%s", line.c_str());
            if (c.count > static_cast<int>(c.changes.size()))
                ImGui::TextDisabled("(and more: %d commits in all)", c.count);
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::TextDisabled("Updating builds the new version on this PC (a few minutes to a quarter of an hour, more when the "
                            "recompiler changed). Mods\\, the mods' on / off choices and settings.ini are kept.");
#else
        (void)runtime;
        (void)settings;
#endif
    }
}
