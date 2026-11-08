#pragma once
// The decompilation layer: functions of the game rewritten as C++, each in place of its
// recompiled translation (README.md in this folder has the long form).
//
// - A decompiled function is registered at its address in the executable; recompiled
//   callers reach it through the function table as before, and it reaches the game's data
//   through Var, Array and Object below, which go through the runtime's memory map
//   (main memory, scratchpad, hardware registers) and can be logged for verification.
// - Arguments come in the EE's registers as GCC's EABI passes them: integers and pointers in
//   a0-a3 and t0-t3, floats in f12-f19, each kind counted on its own. Results go to v0 or
//   f0. bind<&fn>() reads them from the function's C++ signature.
// - Floats are f32: the EE FPU's arithmetic (operands and results clamped to the largest
//   float, no infinities or NaNs; see ps2_runtime_macros.h), so a decompiled function
//   computes what the recompiled one did, bit for bit.
// - Timing: the recompiled code charges the EE clock 8 cycles for a call and 32 for a
//   loop's back edge, and stops at those points when the scheduler has something due (it
//   returns up the host stack and is entered again at the address it stopped at). A
//   function with neither is exact as plain C++. A loop that waits for something yields
//   at its back edge as the original did (Guest::backEdge), with its state in the guest's
//   registers, and is registered at the loop's address too. A function that calls other
//   code calls through Guest::call with the original's return addresses and is registered
//   at each of them, its locals where the original kept them (timing/main_loop.cpp).

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <tuple>
#include <type_traits>
#include <vector>

namespace decomp
{
    // ---------------------------------------------------------------------------------
    // The guest while decompiled code runs

    struct WriteRecord
    {
        uint32_t address;
        uint32_t size;
        uint64_t before;
        uint64_t after;
    };

    // While a function is checked against its original (TF_DECOMP_VERIFY): the writes it made.
    struct WriteLog
    {
        std::vector<WriteRecord> writes;
        bool touchedHardware = false; // a write to a register or the scratchpad: left out, the call not checked
    };
    inline thread_local WriteLog *t_writeLog = nullptr;

    // True while the checker runs a decompiled function to compare it with the original: the
    // PC port's rules (the lines marked "PC:") give the console's answers then, so that what
    // is compared is the game's own logic. (src/port.h's functions look at it.)
    inline thread_local bool t_consoleRules = false;
    inline bool consoleRules() { return t_consoleRules; }

    class Guest
    {
    public:
        Guest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) : m_rdram(rdram), m_ctx(ctx), m_runtime(runtime) {}

        uint8_t *rdram() const { return m_rdram; }
        R5900Context *ctx() const { return m_ctx; }
        PS2Runtime *runtime() const { return m_runtime; }

        // Memory, through the runtime's map like the recompiled loads and stores.
        template <class T>
        T read(uint32_t address) const;
        template <class T>
        void write(uint32_t address, T value) const;

        // Registers (for functions that keep their state there across a yield).
        uint32_t pc() const { return m_ctx->pc; }
        uint32_t gpr(int reg) const { return GPR_U32(m_ctx, reg); }
        void setGpr(int reg, uint32_t value) const { SET_GPR_S32(m_ctx, reg, value); } // (sign-extended, as lw)
        uint64_t gpr64(int reg) const { return GPR_U64(m_ctx, reg); }
        void setGpr64(int reg, uint64_t value) const { SET_GPR_U64(m_ctx, reg, value); }
        void copyGpr(int to, int from) const { SET_GPR_U64(m_ctx, to, GPR_U64(m_ctx, from)); } // move (daddu to, from, zero)
        uint32_t cop0Count() const { return m_ctx->cop0_count; }

        // The 128-bit saves and loads of prologues and epilogues (sq, lq).
        void storeQuad(uint32_t address, int reg) const;
        void loadQuad(uint32_t address, int reg) const;

        // A loop's back edge: charges the 32 cycles the recompiled code charges there. True
        // when the scheduler has something due: the caller returns at once, with the pc set to
        // the loop's address, and is entered there again when the guest resumes.
        bool backEdge(uint32_t loopAddress) const;
        // A call into the game (jal, jalr), from a function that is entered again at
        // `returnAddress` (registered there): sets ra, charges the 8 cycles of a call and runs
        // the callee - recompiled or decompiled. True when it has returned; false when it
        // stopped at a checkpoint (before it started, or inside): the caller returns at once,
        // and is entered at `returnAddress` when the callee is done.
        bool call(uint32_t target, uint32_t returnAddress) const;
        // Return to the caller (jr ra).
        void returnToCaller() const { m_ctx->pc = GPR_U32(m_ctx, 31); }

    private:
        uint8_t *m_rdram;
        R5900Context *m_ctx;
        PS2Runtime *m_runtime;
    };

    // The guest of the decompiled function that is running (set by the bindings).
    Guest &guest();

    class GuestScope
    {
    public:
        GuestScope(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
        ~GuestScope();
        GuestScope(const GuestScope &) = delete;
        GuestScope &operator=(const GuestScope &) = delete;

    private:
        Guest m_guest;
        Guest *m_outer;
    };

    // ---------------------------------------------------------------------------------
    // Floats with the EE FPU's arithmetic

    struct f32
    {
        float v = 0.0f;

        f32() = default;
        constexpr f32(float value) : v(value) {}

        static f32 fromBits(uint32_t bits)
        {
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return f32(value);
        }
        uint32_t bits() const
        {
            uint32_t value;
            std::memcpy(&value, &v, sizeof(value));
            return value;
        }
        static f32 fromInt(int32_t value) { return f32(FPU_CVT_S_W(value)); } // cvt.s.w
        int32_t toInt() const { return ps2_fpu_cvt_w_s(v); }                // cvt.w.s: truncates, clamped

        friend f32 operator+(f32 a, f32 b) { return f32(ps2_fpu_add(a.v, b.v)); }
        friend f32 operator-(f32 a, f32 b) { return f32(ps2_fpu_sub(a.v, b.v)); }
        friend f32 operator*(f32 a, f32 b) { return f32(ps2_fpu_mul(a.v, b.v)); }
        friend f32 operator/(f32 a, f32 b); // (div.s also sets FCR31's flags)
        f32 operator-() const { return f32(-v); }

        // c.olt.s, c.ole.s, c.eq.s: a denormal compares as zero.
        friend bool operator<(f32 a, f32 b) { return ps2_fpu_compare_key(a.v) < ps2_fpu_compare_key(b.v); }
        friend bool operator<=(f32 a, f32 b) { return ps2_fpu_compare_key(a.v) <= ps2_fpu_compare_key(b.v); }
        friend bool operator>(f32 a, f32 b) { return b < a; }
        friend bool operator>=(f32 a, f32 b) { return b <= a; }
        friend bool operator==(f32 a, f32 b) { return ps2_fpu_compare_key(a.v) == ps2_fpu_compare_key(b.v); }
        friend bool operator!=(f32 a, f32 b) { return !(a == b); }
    };
    static_assert(sizeof(f32) == 4);

    inline f32 operator/(f32 a, f32 b) { return f32(ps2_fpu_div(guest().ctx()->fcr31, a.v, b.v)); }
    inline f32 abs(f32 a) { return f32(FPU_ABS_S(a.v)); }
    inline f32 min(f32 a, f32 b) { return f32(ps2_fpu_min(a.v, b.v)); } // min.s (EE)
    inline f32 max(f32 a, f32 b) { return f32(ps2_fpu_max(a.v, b.v)); } // max.s (EE)
    inline f32 sqrt(f32 a) { return f32(ps2_fpu_sqrt(guest().ctx()->fcr31, a.v)); }

    // ---------------------------------------------------------------------------------
    // The game's data

    template <class T>
    T Guest::read(uint32_t address) const
    {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
        uint8_t *rdram = m_rdram; // (the macros' names)
        R5900Context *ctx = m_ctx;
        PS2Runtime *runtime = m_runtime;
        if constexpr (std::is_same_v<T, f32> || std::is_same_v<T, float>)
            return T(f32::fromBits(READ32(address)).v);
        else if constexpr (sizeof(T) == 1)
            return static_cast<T>(READ8(address));
        else if constexpr (sizeof(T) == 2)
            return static_cast<T>(READ16(address));
        else if constexpr (sizeof(T) == 4)
            return static_cast<T>(READ32(address));
        else
            return static_cast<T>(READ64(address));
    }

    template <class T>
    void Guest::write(uint32_t address, T value) const
    {
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
        uint64_t bits = 0u;
        if constexpr (std::is_same_v<T, f32> || std::is_same_v<T, float>)
            bits = f32(value).bits();
        else if constexpr (sizeof(T) == 8)
            bits = static_cast<uint64_t>(value);
        else
            bits = static_cast<uint64_t>(value) & ((1ull << (8 * sizeof(T))) - 1ull);
        if (WriteLog *log = t_writeLog)
        {
            if (PS2Runtime::isSpecialAddress(address))
            {
                log->touchedHardware = true; // (checked runs leave hardware alone)
                return;
            }
            uint64_t before = 0u;
            if constexpr (sizeof(T) == 1)
                before = read<uint8_t>(address);
            else if constexpr (sizeof(T) == 2)
                before = read<uint16_t>(address);
            else if constexpr (sizeof(T) == 4)
                before = read<uint32_t>(address);
            else
                before = read<uint64_t>(address);
            log->writes.push_back({address, static_cast<uint32_t>(sizeof(T)), before, bits});
        }
        uint8_t *rdram = m_rdram;
        R5900Context *ctx = m_ctx;
        PS2Runtime *runtime = m_runtime;
        if constexpr (sizeof(T) == 1)
            WRITE8(address, static_cast<uint8_t>(bits));
        else if constexpr (sizeof(T) == 2)
            WRITE16(address, static_cast<uint16_t>(bits));
        else if constexpr (sizeof(T) == 4)
            WRITE32(address, static_cast<uint32_t>(bits));
        else
            WRITE64(address, bits);
    }

    // A value of the game's at a fixed address (its globals: .data, .sdata, .bss).
    template <class T>
    struct Var
    {
        uint32_t address;

        operator T() const { return guest().read<T>(address); }
        const Var &operator=(T value) const
        {
            guest().write<T>(address, value);
            return *this;
        }
        const Var &operator=(const Var &other) const { return *this = static_cast<T>(other); }
    };

    // A value somewhere in guest memory (a field, an element).
    template <class T>
    struct Ref
    {
        uint32_t address;

        operator T() const { return guest().read<T>(address); }
        const Ref &operator=(T value) const
        {
            guest().write<T>(address, value);
            return *this;
        }
        const Ref &operator=(const Ref &other) const { return *this = static_cast<T>(other); }
    };

    // An array of the game's at a fixed address.
    template <class T>
    struct Array
    {
        uint32_t address;

        Ref<T> operator[](int32_t index) const { return Ref<T>{address + static_cast<uint32_t>(index) * static_cast<uint32_t>(sizeof(T))}; }
    };

    // A local of a resumable function, kept in the guest register the original kept it in: a
    // callee-saved one survives the calls and a stop inside one. GprVar is a 32-bit value
    // (sign-extended, as lw and the 32-bit operations leave it), FprVar a float register.
    struct GprVar
    {
        int reg;

        operator int32_t() const { return static_cast<int32_t>(guest().gpr(reg)); }
        const GprVar &operator=(int32_t value) const
        {
            guest().setGpr(reg, static_cast<uint32_t>(value));
            return *this;
        }
    };

    struct FprVar
    {
        int reg;

        operator f32() const { return f32(guest().ctx()->f[reg]); }
        const FprVar &operator=(f32 value) const
        {
            guest().ctx()->f[reg] = value.v;
            return *this;
        }
    };

    // The method in `slot` of an object's vtable (lw t9, 0(object); lw t9, slot(t9)).
    inline uint32_t method(uint32_t object, uint32_t slot)
    {
        return guest().read<uint32_t>(guest().read<uint32_t>(object) + slot);
    }

    // An object of the game's in guest memory, by its address: derived types name its fields.
    struct Object
    {
        uint32_t address = 0u;

        explicit operator bool() const { return address != 0u; }

    protected:
        template <class T>
        Ref<T> field(uint32_t offset) const { return Ref<T>{address + offset}; }
    };

    // ---------------------------------------------------------------------------------
    // Binding a C++ function to the EE's calling convention

    namespace detail
    {
        template <class T>
        inline constexpr bool isFloat = std::is_same_v<T, f32> || std::is_same_v<T, float>;
        template <class T>
        inline constexpr bool isObject = std::is_base_of_v<Object, T>;

        class Arguments
        {
        public:
            explicit Arguments(R5900Context *ctx) : m_ctx(ctx) {}

            template <class T>
            T next()
            {
                if constexpr (isFloat<T>)
                    return T(m_ctx->f[12 + m_floats++]);
                else
                {
                    const int reg = 4 + m_ints++; // a0-a3, then t0-t3
                    if constexpr (isObject<T>)
                    {
                        T object;
                        object.address = GPR_U32(m_ctx, reg);
                        return object;
                    }
                    else if constexpr (sizeof(T) <= 4)
                        return static_cast<T>(GPR_U32(m_ctx, reg));
                    else
                        return static_cast<T>(GPR_U64(m_ctx, reg));
                }
            }

        private:
            R5900Context *m_ctx;
            int m_ints = 0;
            int m_floats = 0;
        };

        template <class R>
        void setResult(R5900Context *ctx, R result)
        {
            if constexpr (isFloat<R>)
                ctx->f[0] = f32(result).v;
            else if constexpr (isObject<R>)
                SET_GPR_S32(ctx, 2, result.address);
            else if constexpr (sizeof(R) <= 4)
                SET_GPR_S32(ctx, 2, static_cast<int32_t>(result)); // (32-bit values are kept sign-extended)
            else
                SET_GPR_U64(ctx, 2, static_cast<uint64_t>(result));
        }

        template <class F>
        struct Signature;
        template <class R, class... A>
        struct Signature<R (*)(A...)>
        {
            using Result = R;
            using Args = std::tuple<A...>;
        };

        template <auto Fn, class R, class... A>
        void call(R5900Context *ctx, std::tuple<A...> *)
        {
            Arguments arguments(ctx);
            std::tuple<A...> values{arguments.template next<A>()...}; // (braces: read in order)
            if constexpr (std::is_void_v<R>)
                std::apply(Fn, values);
            else
                setResult<R>(ctx, std::apply(Fn, values));
        }
    }

    // The function table entry for a decompiled function: reads its arguments from the
    // registers, calls it, sets the result and returns to the caller.
    template <auto Fn>
    void bind(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        GuestScope scope(rdram, ctx, runtime);
        using S = detail::Signature<decltype(Fn)>;
        detail::call<Fn, typename S::Result>(ctx, static_cast<typename S::Args *>(nullptr));
        ctx->pc = GPR_U32(ctx, 31);
    }

    // The entry for a function that handles the registers itself (Guest&): one that yields.
    template <void (*Fn)(Guest &)>
    void bindRaw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        GuestScope scope(rdram, ctx, runtime);
        Fn(guest());
    }

    // ---------------------------------------------------------------------------------
    // Registration

    // What a function hands back, for the checker: the register that holds the result (the
    // others of v0 and f0 hold whatever the original left there, which no caller reads).
    enum class Result : uint8_t
    {
        None,
        Integer, // v0
        Float,   // f0
    };

    template <auto Fn>
    constexpr Result resultOf()
    {
        using R = typename detail::Signature<decltype(Fn)>::Result;
        if constexpr (std::is_void_v<R>)
            return Result::None;
        else if constexpr (detail::isFloat<R>)
            return Result::Float;
        else
            return Result::Integer;
    }

    struct Function
    {
        const char *name;
        const char *module;
        uint32_t address;
        PS2Runtime::RecompiledFunction entry;
        const uint32_t *resumeAddresses; // where it is entered again after a yield
        size_t resumeCount;
        bool checkable; // no yield, no hardware: TF_DECOMP_VERIFY runs both
        Result result;
        // (another replacement's) the recompiled function expected at the address: when the table
        // holds another one there - the port's own replacement (a fix, a mod, native sound...) -
        // that one stays. nullptr: replace whatever is there (the hand-written functions).
        PS2Runtime::RecompiledFunction expected = nullptr;
    };

    struct Registration
    {
        explicit Registration(const Function &function);
    };

    // Puts the decompiled functions in place of the recompiled ones (unless TF_DECOMP=0 or
    // TF_DECOMP_OFF names them); with TF_DECOMP_VERIFY=1 every call of a checkable one also
    // runs the original and compares.
    void install(PS2Runtime &runtime);
    // The recompiled translation of a function that is replaced (null if it is not).
    PS2Runtime::RecompiledFunction original(uint32_t address);
}

// DECOMP_FUNCTION(0x108780, ClockSeconds, "timing") - a function with a plain signature.
#define DECOMP_FUNCTION(address, fn, module)                                                        \
    static const ::decomp::Registration decompRegistration_##fn{                                    \
        ::decomp::Function{#fn, module, address, &::decomp::bind<&fn>, nullptr, 0u, true, ::decomp::resultOf<&fn>()}}

// In a resumable function written as `switch (g.pc()) { case <entry>: ... }`: a call that can
// stop inside the callee. Execution goes on below it either way - at once when the callee
// returns, or when the function is entered again at `returnAddress` (the original's return
// address, registered with DECOMP_YIELDING). Nothing may be declared between two of these in
// the same scope (put such code in braces): locals live in the guest's registers or frame.
#define DECOMP_CALL(g, target, returnAddress)                                                     \
    if (!(g).call((target), (returnAddress)))                                                       \
        return;                                                                                     \
    [[fallthrough]];                                                                                \
    case (returnAddress):;

// DECOMP_YIELDING(0x25DE90, WaitVBlank, "timing", 0x25DEE8) - a function that yields at the
// given addresses (void fn(Guest &), entered there again).
#define DECOMP_YIELDING(address, fn, module, ...)                                                   \
    static const uint32_t decompResume_##fn[] = {__VA_ARGS__};                                     \
    static const ::decomp::Registration decompRegistration_##fn{                                    \
        ::decomp::Function{#fn, module, address, &::decomp::bindRaw<&fn>, decompResume_##fn,         \
                           std::size(decompResume_##fn), false, ::decomp::Result::None}}
