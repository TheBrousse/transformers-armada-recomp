# The decompilation layer

The game runs as recompiled code: every function of the executable translated to C++
instruction by instruction (`work/generated`, not in the repository). That runs well but
cannot be read or changed. Here functions are **rewritten as C++ that reads like source
code**, each in place of its recompiled translation. The rest of the game keeps running as it
was, so the game works at every step, and what a PC port wants to change - the frame rate,
the resolution, the timing - becomes a change to plain code.

It is a functional decompilation, not a matching one: the aim is code that does what the
original did, checked against the original, not code that compiles back to the same PS2
instructions.

## How a function is replaced

```cpp
// 0x32E8B0: the frame rate the game aims at - the TV's field rate, or half of it.
f32 TargetFrameRate()
{
    f32 rate = frameRates[RateStandard()];
    if (halfRate != 0)
        rate = rate / 2.0f;
    return rate;
}

DECOMP_FUNCTION(0x32E8B0, TargetFrameRate, "timing");
```

`DECOMP_FUNCTION` registers the function at its address in the executable. At start-up
`decomp::install` (called from `applyTransformers`) puts it in the runtime's function table
in place of the recompiled one, so every recompiled caller reaches it as before.
`bind<&fn>` reads the arguments from the EE's registers as GCC's EABI passes them (integers
and pointers in a0-a3 and t0-t3, floats in f12-f19, each kind counted on its own) and puts the
result in v0 or f0.

## Conventions

- **The game's data** is reached through `Var<T>` (a global at a fixed address), `Array<T>`
  and `Object` types whose members name the fields (`GameClock::time()`), never host
  pointers. Reads and writes go through the runtime's memory map like the recompiled loads
  and stores (main memory, the scratchpad, hardware registers), and the checker can log them.
  Name a global after what it does and keep its address and its `gp` offset in the comment.
- **Floats are `f32`**: the EE FPU's arithmetic (operands and results clamped to the largest
  float, no infinities or NaNs, denormals compared as zero, FCR31's flags set by a division),
  through the same helpers as the recompiled code, so a decompiled function computes what the
  recompiled one did, bit for bit. Keep the original's order of operations.
- **The comment over a function** gives its address and says what it does in the game.
  Changes the PC port makes are marked `PC:` - everything else is the game's own logic.
- **Timing.** The recompiled code charges the EE clock 8 cycles for a call and 32 for a
  loop's back edge, and those are the only points where it can stop: when the scheduler has
  something due (a VBlank, a timer, the IOP) it returns up the host stack and the scheduler
  enters the function table again at the address where it stopped, after the event. All its
  state is in the guest's registers and memory, so it can go on from there. Hence:
  - a function with no calls and no loops is exactly as fast, to the cycle, as plain C++ (the
    caller's call charges the 8 cycles either way);
  - a loop that waits for something yields at its back edge as the original did
    (`Guest::backEdge`), keeps its state in the registers where the original kept it, and is
    registered at the loop's address too (`DECOMP_YIELDING`), where it is entered again;
  - a function that calls other code (recompiled or decompiled) can be stopped inside the
    callee, and the callee then returns to the scheduler, not to the C++ that called it. Such
    a function is written as a resumable one: it calls through `Guest::call(target,
    returnAddress)` with the original's return address, is registered at every one of them
    (`DECOMP_YIELDING`), keeps its locals where the original kept them (callee-saved registers,
    its own frame on the guest stack - the calls leave both alone), and picks up from
    `Guest::pc()` when it is entered at a return address. Written as one
    `switch (g.pc()) { case <entry>: ... }` around the whole body, with `DECOMP_CALL(g, target,
    returnAddress)` for each call (it puts the `case` label for the return address right after
    the call), it reads top to bottom like the original, loops included: a loop's back edge is
    `if (g.backEdge(top)) return;` with `case top:` at the top of its body.
    `timing/main_loop.cpp` is the example. Because all of its state is in guest registers and
    memory, save states made by either the recompiled or the decompiled function load in the
    other.
- The game's outcome depends on that timing (fights in particular: docs/NOTES.md, "Draw
  distance"), so a decompiled function that charges the clock differently changes the game's
  course without being wrong. The ones here charge exactly what the recompiled ones did.

## Checking a function against the original

- `TF_DECOMP_VERIFY=1`: every call of a decompiled function without yields runs it with the
  PC port's rules off (`decomp::consoleRules()`: the `port::` functions give the console's
  answers) and its writes logged, takes the writes back, runs the recompiled original, and
  compares what the caller can see: the result (v0 or f0, by the function's type), the
  callee-saved registers, FCR31 (but its compare bit), and every place the decompiled
  function wrote. Then it puts those places back and runs the decompiled function for real,
  so a checked run plays like any other. Differences are logged (`[decomp:verify] <name> ...
  differs: ...`), and a count at 1, 10, 100, ... calls.
- `tools\check_frames.ps1`: the 22 kept frames must stay identical.
- `TF_DECOMP=0` runs the recompiled code throughout; `TF_DECOMP_OFF=<name or hex address>,...`
  leaves single functions to it; `TF_DECOMP_LIST=1` lists what is in place. (The PC port's
  rules written into decompiled functions - the frame rate - go with them.)

## What is decompiled

| Module | Functions |
|---|---|
| [timing](timing/timing.h) | The EE's clock in seconds (`0x108780`), a level's clock (`0x41E8F0`, `0x41E820`, `0x41E840`), the TV's field rate, the target frame rate and the frame period (`0x32E840`, `0x32E8B0`, `0x32E910`), the half-rate setter (`0x32E8A0`), the governor's target slots (`0x202FF0`), the hold at 30 (`0x106710`, `0x202FE0`), the wait for the VBlank at the end of a frame (`0x25DE90`, yields at `0x25DEE8`), the main loop that runs a mode and paces its frames (`0x295310`), the governor that chooses frames of two fields or one and scales the detail to the load (`0x25B670`) |

The frame rate is a setting since 2026-10-04 (`TF_FRAME_RATE`, [docs/NOTES.md](../../docs/NOTES.md#frame-rates)):
in a level at the PC port's rate R the rates above are the console's scaled to it (59.94 x R /
60, the period one over that), the governor keeps one field and the full detail, and the main
loop waits one VBlank a frame (`port::levelRate()`); a car's reset keeps the period of 30
(`FramePeriod` asked from `0x31F7E0`).

Next for the frame timing: the render device's frame start with the display list's kick
(`0x25BE20`, `0x25D2B0`) and the frame end that waits for the list (`0x25D5D0`) - drafted by
an analysis agent, to be made resumable; and the frame-rate rules that `src/overrides.cpp`
still applies from the outside (the switch at a level's begin and end, the half rate and
betwixtMode each frame, the fixes for what counts frames,
[docs/FRAME_RATE.md](../../docs/FRAME_RATE.md)).
