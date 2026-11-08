// The decompilation layer's registry: puts the decompiled functions in place of the
// recompiled ones, and checks them against those (TF_DECOMP_VERIFY).

#include "port_ext.h"
#include "decomp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>


namespace decomp
{
    namespace
    {
        thread_local Guest *t_guest = nullptr;

        std::vector<Function> &registry()
        {
            static std::vector<Function> functions;
            return functions;
        }

        struct Installed
        {
            const Function *function = nullptr;
            PS2Runtime::RecompiledFunction original = nullptr;
            uint64_t calls = 0u;
            uint64_t differing = 0u;
            uint64_t unchecked = 0u;
        };
        std::unordered_map<uint32_t, Installed> g_installed; // by entry address

        // TF_DECOMP_OFF=ClockSeconds,0x25de90: names or addresses left to the recompiled code.
        bool switchedOff(const Function &function, const char *list)
        {
            if (!list)
                return false;
            const std::string text = list;
            size_t start = 0u;
            while (start <= text.size())
            {
                const size_t end = std::min(text.find(',', start), text.size());
                const std::string item = text.substr(start, end - start);
                if (!item.empty())
                {
                    if (item == function.name)
                        return true;
                    char *rest = nullptr;
                    const unsigned long address = std::strtoul(item.c_str(), &rest, 16);
                    if (rest && *rest == '\0' && address == function.address)
                        return true;
                }
                start = end + 1u;
            }
            return false;
        }

        void writeBack(uint32_t address, uint32_t size, uint64_t value)
        {
            Guest &g = guest();
            if (size == 1u)
                g.write<uint8_t>(address, static_cast<uint8_t>(value));
            else if (size == 2u)
                g.write<uint16_t>(address, static_cast<uint16_t>(value));
            else if (size == 4u)
                g.write<uint32_t>(address, static_cast<uint32_t>(value));
            else
                g.write<uint64_t>(address, value);
        }

        uint64_t readBack(uint32_t address, uint32_t size)
        {
            Guest &g = guest();
            if (size == 1u)
                return g.read<uint8_t>(address);
            if (size == 2u)
                return g.read<uint16_t>(address);
            if (size == 4u)
                return g.read<uint32_t>(address);
            return g.read<uint64_t>(address);
        }

        bool sameVector(__m128i a, __m128i b)
        {
            return _mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) == 0xFFFF;
        }

        uint32_t floatBits(float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        // What the caller can see after the call: the result, the callee-saved registers, the
        // FPU's flags (not the compare condition, which no caller reads), and what the
        // decompiled function wrote.
        std::string compare(const R5900Context &native, const R5900Context &original, const WriteLog &log, Result result)
        {
            std::string out;
            char text[160];
            if (native.pc != original.pc)
            {
                std::snprintf(text, sizeof(text), " pc %08x (original %08x)", native.pc, original.pc);
                out += text;
            }
            if (result == Result::Integer && GPR_U64((&native), 2) != GPR_U64((&original), 2))
            {
                std::snprintf(text, sizeof(text), " v0 %016llx (original %016llx)",
                              static_cast<unsigned long long>(GPR_U64((&native), 2)),
                              static_cast<unsigned long long>(GPR_U64((&original), 2)));
                out += text;
            }
            static const struct
            {
                int reg;
                const char *name;
            } kSaved[] = {{16, "s0"}, {17, "s1"}, {18, "s2"}, {19, "s3"}, {20, "s4"}, {21, "s5"}, {22, "s6"},
                          {23, "s7"}, {28, "gp"}, {29, "sp"}, {30, "fp"}, {31, "ra"}};
            for (const auto &saved : kSaved)
                if (!sameVector(native.r[saved.reg], original.r[saved.reg]))
                {
                    std::snprintf(text, sizeof(text), " %s differs", saved.name);
                    out += text;
                }
            for (int f = 0; f < 32; ++f)
            {
                if ((f == 0 && result != Result::Float) || (f != 0 && f < 20))
                    continue; // (the result and the callee-saved f20-f31)
                if (floatBits(native.f[f]) != floatBits(original.f[f]))
                {
                    std::snprintf(text, sizeof(text), " f%d %g (%08x; original %g, %08x)", f, native.f[f], floatBits(native.f[f]),
                                  original.f[f], floatBits(original.f[f]));
                    out += text;
                }
            }
            constexpr uint32_t kCondition = 0x800000u;
            if ((native.fcr31 & ~kCondition) != (original.fcr31 & ~kCondition))
            {
                std::snprintf(text, sizeof(text), " fcr31 %08x (original %08x)", native.fcr31, original.fcr31);
                out += text;
            }
            std::map<uint32_t, const WriteRecord *> last; // (the last write to each place)
            for (const WriteRecord &write : log.writes)
                last[write.address] = &write;
            for (const auto &[address, write] : last)
            {
                const uint64_t now = readBack(address, write->size);
                if (now != write->after)
                {
                    std::snprintf(text, sizeof(text), " [%08x] %llx (original %llx)", address,
                                  static_cast<unsigned long long>(write->after), static_cast<unsigned long long>(now));
                    out += text;
                }
            }
            return out;
        }

        // TF_DECOMP_VERIFY: runs the decompiled function with the PC port's rules off and its
        // writes logged, takes the writes back, runs the original and compares; then puts the
        // places it wrote back as they were and runs the decompiled function for real, so the
        // game goes on as it does without the checker.
        void checkedEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto found = g_installed.find(ctx->pc);
            if (found == g_installed.end())
            {
                std::fprintf(stderr, "[decomp:verify] no function at %08x\n", ctx->pc);
                return;
            }
            Installed &installed = found->second;
            ++installed.calls;
            const R5900Context before = *ctx;
            WriteLog log;
            t_writeLog = &log;
            t_consoleRules = true;
            installed.function->entry(rdram, ctx, runtime);
            t_consoleRules = false;
            t_writeLog = nullptr;
            const R5900Context native = *ctx;
            const auto undo = [&]()
            {
                GuestScope scope(rdram, ctx, runtime);
                for (auto write = log.writes.rbegin(); write != log.writes.rend(); ++write)
                    writeBack(write->address, write->size, write->before);
            };
            undo();
            *ctx = before;
            installed.original(rdram, ctx, runtime);
            std::string differences;
            if (!log.touchedHardware)
            {
                GuestScope scope(rdram, ctx, runtime);
                differences = compare(native, *ctx, log, installed.function->result);
            }
            undo();
            *ctx = before;
            installed.function->entry(rdram, ctx, runtime);
            if (log.touchedHardware)
            {
                ++installed.unchecked;
                return;
            }
            if (!differences.empty())
            {
                ++installed.differing;
                if (installed.differing <= 20u)
                    std::fprintf(stderr, "[decomp:verify] %s (%08x) call %llu differs:%s\n", installed.function->name,
                                 installed.function->address, static_cast<unsigned long long>(installed.calls),
                                 differences.c_str());
            }
            const uint64_t calls = installed.calls;
            if (calls == 1u || calls == 10u || calls == 100u || calls == 1000u || calls == 10000u || calls == 100000u ||
                calls == 1000000u)
                std::fprintf(stderr, "[decomp:verify] %s: %llu calls checked, %llu differ%s\n", installed.function->name,
                             static_cast<unsigned long long>(calls), static_cast<unsigned long long>(installed.differing),
                             installed.unchecked ? " (some touched hardware: not checked)" : "");
        }
    }

    Guest &guest()
    {
        return *t_guest;
    }

    GuestScope::GuestScope(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) : m_guest(rdram, ctx, runtime), m_outer(t_guest)
    {
        t_guest = &m_guest;
    }

    GuestScope::~GuestScope()
    {
        t_guest = m_outer;
    }

    bool Guest::backEdge(uint32_t loopAddress) const
    {
        m_ctx->pc = loopAddress;
        return m_runtime->eeCheckpointDue(); // (32 cycles, as at every back edge of the recompiled code)
    }

    bool Guest::call(uint32_t target, uint32_t returnAddress) const
    {
        SET_GPR_U32(m_ctx, 31, returnAddress);
        m_ctx->pc = target;
        return m_runtime->dispatchGuestBranch(m_rdram, m_ctx, target, returnAddress - 8u, returnAddress,
                                              PS2Runtime::GuestBranchKind::DirectCall, "decomp");
    }

    void Guest::storeQuad(uint32_t address, int reg) const
    {
        uint8_t *rdram = m_rdram; // (the macro's names)
        R5900Context *ctx = m_ctx;
        PS2Runtime *runtime = m_runtime;
        WRITE128(address, GPR_VEC(ctx, reg));
    }

    void Guest::loadQuad(uint32_t address, int reg) const
    {
        uint8_t *rdram = m_rdram;
        R5900Context *ctx = m_ctx;
        PS2Runtime *runtime = m_runtime;
        SET_GPR_VEC(ctx, reg, READ128(address));
    }

    Registration::Registration(const Function &function)
    {
        registry().push_back(function);
    }

    PS2Runtime::RecompiledFunction original(uint32_t address)
    {
        const auto found = g_installed.find(address);
        return found == g_installed.end() ? nullptr : found->second.original;
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("TF_DECOMP"); env && std::strcmp(env, "0") == 0)
        {
            std::fprintf(stderr, "[decomp] off (TF_DECOMP=0): the recompiled code runs throughout\n");
            return;
        }
        ext::addDecompFunctions(); // (an extension's functions into the registry: port_ext.h)
        const char *off = std::getenv("TF_DECOMP_OFF");
        const char *verifyEnv = std::getenv("TF_DECOMP_VERIFY");
        const bool verify = verifyEnv && *verifyEnv && std::strcmp(verifyEnv, "0") != 0;
        const bool list = std::getenv("TF_DECOMP_LIST") != nullptr;

        std::vector<Function> &functions = registry();
        std::sort(functions.begin(), functions.end(), [](const Function &a, const Function &b) { return a.address < b.address; });
        std::map<std::string, int> perModule;
        int placed = 0, left = 0, checked = 0, kept = 0;
        for (const Function &function : functions)
        {
            if (switchedOff(function, off))
            {
                ++left;
                continue;
            }
            // (exact entries only: lookupFunction answers any other address with a stub that reports
            // a missing function - it would become the "original" a fallback or a check runs)
            const PS2Runtime::RecompiledFunction recompiled =
                runtime.hasFunction(function.address) ? runtime.lookupFunction(function.address) : nullptr;
            if (!recompiled)
            {
                std::fprintf(stderr, "[decomp] %s: nothing at %08x to replace\n", function.name, function.address);
                continue;
            }
            if (function.expected && recompiled != function.expected && recompiled != function.entry)
            {
                // (the port replaces this function already - its fixes, mods, native sound... are
                // installed before decomp::install: that one stays)
                ++kept;
                if (list)
                    std::fprintf(stderr, "[decomp]   %08x %-28s %s: the port's own replacement there stays\n", function.address,
                                 function.name, function.module);
                continue;
            }
            Installed &installed = g_installed[function.address];
            installed.function = &function;
            installed.original = recompiled;
            const bool check = verify && function.checkable;
            runtime.registerFunction(function.address, check ? &checkedEntry : function.entry);
            for (size_t i = 0; i < function.resumeCount; ++i)
                runtime.registerFunction(function.resumeAddresses[i], function.entry);
            ++placed;
            checked += check ? 1 : 0;
            ++perModule[function.module];
            if (list)
                std::fprintf(stderr, "[decomp]   %08x %-28s %s%s\n", function.address, function.name, function.module,
                             check ? " (checked)" : "");
        }
        std::string modules;
        for (const auto &[module, count] : perModule)
            modules += (modules.empty() ? "" : ", ") + module + " " + std::to_string(count);
        std::fprintf(stderr, "[decomp] %d functions in place of the recompiled ones (%s)%s", placed, modules.c_str(),
                     left ? "" : "\n");
        if (left)
            std::fprintf(stderr, ", %d left to the recompiled code (TF_DECOMP_OFF)\n", left);
        if (kept)
            std::fprintf(stderr, "[decomp] %d left to the port's own replacements at their addresses\n", kept);
        if (verify)
            std::fprintf(stderr, "[decomp] TF_DECOMP_VERIFY: %d of them run beside their originals and are compared\n", checked);
    }
}
