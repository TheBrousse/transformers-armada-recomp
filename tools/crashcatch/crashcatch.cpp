// crashcatch - runs a program as its debugger and, when it fails fast (0xC0000409: a stack cookie, CFG, or a library's
// own abort() - the test PC's NVIDIA OpenGL driver ends the game that way, TODO 163) or crashes, writes every thread's
// stack (symbolised from the program's PDB and the modules' exports) and a minidump before it goes. Those ends never reach
// the program's own crash handler (profiler.cpp's crashFilter): a fast fail goes straight to the kernel - but a debugger
// is told first.
//
//     crashcatch.exe <dump folder> <program> [arguments...]
//
// The program runs in the current folder with the current environment. Output: <dump folder>\crashcatch.txt (the
// stacks) and <dump folder>\crashcatch.dmp. The exit code is the program's. Built by tools\crashcatch\build.ps1.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace
{
    FILE *g_out = stderr;
    std::map<DWORD, HANDLE> g_threads;
    HANDLE g_process = nullptr;

    void printStack(DWORD id, HANDLE thread)
    {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &c))
        {
            std::fprintf(g_out, "thread %lu: no context\n", id);
            return;
        }
        STACKFRAME64 f{};
        f.AddrPC.Offset = c.Rip;
        f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = c.Rbp;
        f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = c.Rsp;
        f.AddrStack.Mode = AddrModeFlat;
        std::fprintf(g_out, "thread %lu:\n", id);
        for (int i = 0; i < 48; ++i)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_process, thread, &f, &c, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                f.AddrPC.Offset == 0)
                break;
            const DWORD64 a = f.AddrPC.Offset;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
            auto *s = reinterpret_cast<SYMBOL_INFO *>(buffer);
            s->SizeOfStruct = sizeof(SYMBOL_INFO);
            s->MaxNameLen = 511;
            DWORD64 displacement = 0;
            char module[MAX_PATH] = "?";
            IMAGEHLP_MODULE64 mi{};
            mi.SizeOfStruct = sizeof(mi);
            if (SymGetModuleInfo64(g_process, a, &mi))
                std::snprintf(module, sizeof(module), "%s", mi.ModuleName);
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            char where[400] = "";
            if (SymGetLineFromAddr64(g_process, a, &lineDisp, &line))
                std::snprintf(where, sizeof(where), " (%s:%lu)", line.FileName, line.LineNumber);
            if (SymFromAddr(g_process, a, &displacement, s))
                std::fprintf(g_out, "  #%02d %s!%s+0x%llx%s\n", i, module, s->Name, static_cast<unsigned long long>(displacement), where);
            else
                std::fprintf(g_out, "  #%02d %s+0x%llx\n", i, module, static_cast<unsigned long long>(a - mi.BaseOfImage));
        }
    }

    void report(const DEBUG_EVENT &ev, const char *dumpDir)
    {
        const EXCEPTION_RECORD &r = ev.u.Exception.ExceptionRecord;
        std::fprintf(g_out, "exception %08lx at %p on thread %lu (%s chance), parameter 0 %llx\n", r.ExceptionCode, r.ExceptionAddress, ev.dwThreadId,
                     ev.u.Exception.dwFirstChance ? "first" : "second", r.NumberParameters ? static_cast<unsigned long long>(r.ExceptionInformation[0]) : 0ull);
        SymRefreshModuleList(g_process);
        if (auto it = g_threads.find(ev.dwThreadId); it != g_threads.end())
            printStack(it->first, it->second);
        for (auto &[id, h] : g_threads)
            if (id != ev.dwThreadId)
                printStack(id, h);
        std::fflush(g_out);
        // The minidump: the threads, their stacks and the memory they point at.
        const std::string path = std::string(dumpDir) + "\\crashcatch.dmp";
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            if (auto it = g_threads.find(ev.dwThreadId); it != g_threads.end())
                GetThreadContext(it->second, &c);
            EXCEPTION_RECORD record = r;
            EXCEPTION_POINTERS pointers{&record, &c};
            MINIDUMP_EXCEPTION_INFORMATION info{ev.dwThreadId, &pointers, FALSE};
            const BOOL ok = MiniDumpWriteDump(g_process, ev.dwProcessId, file,
                                              static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                                                         MiniDumpScanMemory | MiniDumpWithUnloadedModules),
                                              &info, nullptr, nullptr);
            CloseHandle(file);
            std::fprintf(g_out, "minidump %s: %s\n", path.c_str(), ok ? "written" : "failed");
        }
        std::fflush(g_out);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "crashcatch.exe <dump folder> <program> [arguments...]\n");
        return 2;
    }
    const char *dumpDir = argv[1];
    CreateDirectoryA(dumpDir, nullptr);
    const std::string outPath = std::string(dumpDir) + "\\crashcatch.txt";
    if (FILE *f = std::fopen(outPath.c_str(), "w"))
        g_out = f;
    std::string cmd = "\"" + std::string(argv[2]) + "\"";
    for (int i = 3; i < argc; ++i)
        cmd += " \"" + std::string(argv[i]) + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(argv[2], cmd.data(), nullptr, nullptr, TRUE, DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &si, &pi))
    {
        std::fprintf(g_out, "cannot start %s (%lu)\n", argv[2], GetLastError());
        return 2;
    }
    g_process = pi.hProcess;
    DWORD exitCode = 0;
    bool reported = false, symbols = false;
    for (;;)
    {
        DEBUG_EVENT ev{};
        if (!WaitForDebugEvent(&ev, INFINITE))
            break;
        DWORD status = DBG_CONTINUE;
        switch (ev.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            g_threads[ev.dwThreadId] = ev.u.CreateProcessInfo.hThread;
            if (ev.u.CreateProcessInfo.hFile)
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_IGNORE_CVREC);
            symbols = SymInitialize(g_process, nullptr, FALSE) != FALSE;
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            g_threads[ev.dwThreadId] = ev.u.CreateThread.hThread;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            g_threads.erase(ev.dwThreadId);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (symbols)
            {
                char name[MAX_PATH] = "";
                if (GetFinalPathNameByHandleA(ev.u.LoadDll.hFile, name, MAX_PATH, FILE_NAME_NORMALIZED))
                    SymLoadModuleEx(g_process, ev.u.LoadDll.hFile, name[0] == '\\' && name[1] == '\\' && name[2] == '?' ? name + 4 : name, nullptr,
                                    reinterpret_cast<DWORD64>(ev.u.LoadDll.lpBaseOfDll), 0, nullptr, 0);
            }
            if (ev.u.LoadDll.hFile)
                CloseHandle(ev.u.LoadDll.hFile);
            break;
        case EXCEPTION_DEBUG_EVENT:
        {
            const DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
            const bool fatal = code == 0xC0000409u || code == 0xC0000374u || !ev.u.Exception.dwFirstChance;
            if (code == EXCEPTION_BREAKPOINT && !reported)
                break; // (the loader's initial breakpoint)
            if (fatal && !reported)
            {
                reported = true;
                report(ev, dumpDir);
            }
            status = DBG_EXCEPTION_NOT_HANDLED;
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exitCode = ev.u.ExitProcess.dwExitCode;
            std::fprintf(g_out, "exited %08lx\n", exitCode);
            std::fflush(g_out);
            ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
            return static_cast<int>(exitCode);
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
    }
    return static_cast<int>(exitCode);
}
