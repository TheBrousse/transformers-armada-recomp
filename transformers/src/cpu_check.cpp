// The processor check at the game's start (cpucheck): the game needs SSE4.1 (2008 or newer).
//
// The runtime uses SSE4.1 instructions unconditionally (MSVC takes the intrinsics without a flag), so on a processor
// without it the game would end at its first such instruction with no message. This runs among the libraries' static
// initialisers (init_seg(lib): before the game's own objects and main), tests the processor with cpuid (bit 19 of
// ECX for leaf 1), and when SSE4.1 is missing shows one plain message and ends the program. Nothing in this file uses
// those instructions: it is compiled as every other file (no /arch, x86-64's baseline), and the only call before the
// test is cpuid itself. Not on other processors (an ARM build has no such instructions to need).

#if defined(_M_X64) || defined(__x86_64__)

#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#if defined(_WIN32)
extern "C" __declspec(dllimport) int __stdcall MessageBoxW(void *window, const wchar_t *text, const wchar_t *caption, unsigned type);
#endif

namespace
{
    bool hasSse41()
    {
#if defined(_MSC_VER)
        int regs[4] = {0, 0, 0, 0};
        __cpuid(regs, 0);
        if (regs[0] < 1)
            return false;
        __cpuid(regs, 1);
        return (regs[2] & (1 << 19)) != 0;
#else
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return false;
        return (c & (1u << 19)) != 0;
#endif
    }

    struct CpuCheck
    {
        CpuCheck()
        {
            // (TF_CPU_CHECK_TEST=1, tests: the message and the end as on a processor without it)
            const bool test = std::getenv("TF_CPU_CHECK_TEST") != nullptr;
            if (hasSse41() && !test)
                return;
            static const char kMessage[] = "This game needs a processor with SSE4.1 (2008 or newer).";
            std::fprintf(stderr, "[cpu] %s\n", kMessage);
#if defined(_WIN32)
            MessageBoxW(nullptr, L"This game needs a processor with SSE4.1 (2008 or newer).", L"Transformers", 0x10u /* MB_ICONERROR */);
#endif
            std::_Exit(1);
        }
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4073) // (initializers put in the library initialization area: on purpose)
#pragma init_seg(lib)
#pragma warning(pop)
#endif
    // (GCC / Clang: the highest priority a program may use, before the game's other static objects)
#if defined(__GNUC__) && !defined(_MSC_VER)
    __attribute__((init_priority(101)))
#endif
    CpuCheck g_cpuCheck;
}

#endif
