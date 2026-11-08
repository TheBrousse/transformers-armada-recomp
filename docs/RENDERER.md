# A renderer of our own

*Started 2026-10-04 (night). Plan and findings; see [STATUS.md](STATUS.md) for progress.*

## Why

Today the game's pictures are made the way the console makes them, emulated:

- the EE builds a display list every frame and sends it over VIF1 (about 5 MB in the
  dropship area);
- VIF1 unpacks vertex data into VU1 memory, and VU1 microcode transforms, lights and clips
  it and sends the result to the GS as GIF packets (the JIT runs it at 1.7 ns an instruction
  pair);
- the GS front end turns the packets into primitives, and the GPU renderer draws them with
  the GS's pixel pipeline emulated in a fragment shader ([RUNTIME.md](RUNTIME.md#on-the-gpu)).

In real time at 60 on the walk from the dropship (2026-10-04, night) that is 2.7 cores busy:
the VIF1/VU1 thread 69%, the GS thread 67%, the EE 61%, the GS front end 41%. Only the EE's
share is the game; the other 1.8 cores emulate the PS2's graphics hardware, and flat out the
VIF1/VU1 thread is what each frame waits for. A renderer of our own does that work on the
graphics card: the vertex work from the game's own vertex data, and in the end the pixel
work with the GPU's own depth test and blending.

It does not have to come all at once. The plan below replaces the vertex work first, batch
kind by batch kind, behind a switch, while everything else keeps going through the emulation.

## How the game draws

### The microcode is a dispatcher and a library of routines

All eight VU1 code images start with the same dispatcher (`0x0000`-`0x0108`):

- `MSCAL 0` sets `vi13` = data `q4.x` (how many routines) and `vi14` = 1020 (a stack); every
  batch after it is an `MSCNT` that resumes at `0x00f8` and goes round to `0x0010`.
- At `0x0010` it reads the batch header at `ITOP`: `x` & `0x7FFF` = vertex count (`vi10`),
  `y` = batch type (cleared once read). Types 1 to 4 first convert the vertex data in place
  (below); every type then runs the routine table: for each entry from data `q5` on, `jalr`
  to `entry.x * 8` with `vi11` = `entry.y` (the routine's data block), the input buffer
  (`vi01`) at `ITOP + 1`.
- A routine ends with `jr vi15` and usually an `XGKICK` in its delay slot.

The batch types are integer-to-float conversions of the vertex data (the usual trick: values
stored in a float's mantissa with a bias, the bias taken off as a float subtraction; the bias
is the header's `z`):

| Type | Entry | Vertex layout | Converted in place |
|---|---|---|---|
| 1 | `0x170` | 2 qwords | `xyz` of the second, its `w` less (bias + 1) / 1024 |
| 2 | `0x220` | 3 qwords | `xyz` of the third |
| 3 | `0x2c0` | 3 qwords | `xyzw` of the first (four vertices at a time; the qword the loop runs over past the end saved and put back) |
| 4 | `0x388` | 3 qwords | as 3, more (not read yet) |
| other | `0x00a8` | as the routines expect | nothing |

The code images share a core: everything up to `0x1AC0` is the same in seven of the eight
images (dispatcher, routines `0x0480`, `0x1380`, `0x1410`, the clipping subroutines). From
`0x1AC0` on, each image holds its own overlay routines at the same few addresses (`0x1ac0`,
`0x1d10`, `0x21f0`, `0x2800`... are different code in different images). Image
`47dd78e8f5335846` has a different core.

### What the routines cost

`PS2X_VU1_ROUTINES=1` reports the VU1 work by code image, batch type and routine table
(2026-10-04, flat out at 60; share of VU1 instruction pairs, both entry points added):

| Image | Type | Routines (address/data block) | Start of Amazon Basin | Dropship walk | Pairs a vertex |
|---|---|---|---|---|---|
| `2011a15b85aa5a94` | 3 | `1410/q35 0480/qd` | 14.7% | 25.5% | 25-45 |
| `422f0062aa2ca315` | 4 | `1d10/q35 2800/q40 0480/qd` | 31.3% | 16.0% | 85 |
| `bec5738323d4afd8` | 3 | `2150/q35 1410/q36 0480/qd` | 11.3% | 9.5% | 28-41 |
| `47dd78e8f5335846` | 3 | `1380/q35` | 14.1% | 9.0% | 280-840 |
| `bec5738323d4afd8` | 3 | `1ac0/q35` | 4.0% | 7.9% | 240-260 |
| `bec5738323d4afd8` | 1 | `22a0/q35` | 2.9% | 9.3% | 57 |
| `2011a15b85aa5a94` | 3 | `21b0/q35 1410/q3a 0480/qd` | 11.8% | 4.0% | 49-59 |
| `422f0062aa2ca315` | 4 | `21f0/q35 1410/q3c 0480/qd` | - | 6.3% | 62 |
| `e1472a8c96f62e62` | 3 | `1df0/q35 2cc0/q3a 0480/qd` | - | 3.0% | 76 |
| `2011a15b85aa5a94` | 3 | `3660/q35 1410/q37 0480/qd` | 3.0% | 1.8% | 100-210 |

Batches are about 20-27 vertices. Routine `1410` is in the tables of 40-47% of the work,
`0480` in about three quarters of it.

### Routines read so far

- **`0x1410`, transform and kick.** A vertex is three qwords: ST, colour, position `xyz`. The
  position goes through two matrices - `vf01`-`vf04`, field A's screen transform (set by
  `MSCAL 0x450` from data `q0`-`q3`), and data block `q0`-`q3`, field B's - then `Q = 1/w`,
  `xyz * Q` to 12.4 fixed point (`FTOI4`), `ST * Q`; the drawing-kick bit of each vertex
  comes from a bit mask in the word before the output (`-1(vi04)`, shifted one place a
  vertex). Field A is written over the input, field B after it; the data block's `q8`-`q10`
  and `q12`-`q14` are the two copies' GIF tags and register writes (`SCISSOR` etc.); one
  `XGKICK` sends both. Software-pipelined: a vertex every 16 pairs, both fields.
- **`0x0480`, clipping, called by `0x1410`.** `0x1410` reads `q21.x`: not above 0, it draws
  the batch and adds one to the dispatcher's routine index (`vi12`), so the routine after it
  in the table, always `0x0480`, does not run. Above 0 it puts the drawing-kick masks into
  the positions' `w` (`ISW`), writes both fields' register writes after the input, and calls
  `0x0480` twice - field A, then, with the matrices swapped about (field B's into
  `vf01`-`vf04` and `q0`-`q3`, its clipping matrix into `0x0480`'s data block), field B - and
  swaps them back. `0x0480` draws the strip after the input (field A at `h+3n+1`, field B
  after it), every vertex through the field's matrix and through the clipping matrix
  (`0x0480`'s `q0`-`q3`, `CLIP` against `w`: the guard band and the near plane). A triangle
  with a vertex outside is taken out of the strip (bit 15 of its last vertex's `XYZF2` w);
  unless all three are outside one plane it is clipped - Sutherland-Hodgman against `-z`,
  `+z`, `-x`, `+x`, `-y`, `+y` in turn, between buffers at `q117` and `q147`, a new point at
  `o + (i - o) * |d_o / (d_i - d_o)|` from the outside vertex - and drawn as a packet of its
  own (`0x0f50`): first the field's register writes again (from `q21.w`), then the polygon
  from `0x0480`'s `q9` (the triangle's own vertices through the field's matrix as in the
  strip, the new ones through `0x0480`'s `q4`-`q7` from their clipping positions; `XYZF2`'s
  w the last new point's alpha). Around the clipper `0x1180` saves `vf17`-`vf31` and
  `vi02`-`vi15` on the stack (`vi14`, from `q1020` down) and `0x1290` restores them.
  The same holds for `0x2800` below.
- **`0x2150`** (image `bec5738323d4afd8`, before `0x1410`): every vertex's colour times the
  data block's q0, at most 255, back to integers - four vertices at a time, so it runs past
  the last one (into slots `0x1410` writes later, whose `w` its packet carries).
- **`0x21b0`** (images `2011a15b85aa5a94` and `25cef3e84459f32b`, before `0x1410`): ST x and y
  from the position by the rows x and y of the data block's q0-q3 (environment-mapped
  coordinates); the colour the data block's q4 as integers, with bit 7 of the vertex's
  alpha.
- **`0x1d10` then `0x2800`** (images `422f0062aa2ca315` and `c06ad47f0d18db35`, batch type 4:
  ST and normal as integers, position): environment-mapped, lit meshes - the look of the
  Transformers' metal. Four copies of the strip go out, each with its tags: context 1 the
  reflection, context 2 the base texture, each for field A then B (field B's matrix is
  `0x2800`'s data block q0-q3; context 2's `SCISSOR`/`XYOFFSET` are context 1's register
  writes with the register's number plus one). Per vertex, with V the view vector (the eye,
  `0x1d10`'s q4, less the position, through `RSQRT`):
  - reflection: R = 2 (N.V) N - V through the environment matrix (q0-q2), normalized
    (`ERLENG`); sphere-map coordinates 0.5 + (Rx, Ry) * 0.5 / (Rz + 1), and q = Rz + 1, each
    times the field's Q; colour 255 * q8.x * ((1 - N.V) * q8.y) in all three channels;
  - base: the vertex's ST times Q; colour per channel 128 * (q7 * q6) * ((q10 * max(N.L2, 0) +
    q6 * max(N.L1, 0)) * q5.w + q9.w), at most 255 - two directional lights, L1 = q5,
    L2 = q9;
  - alpha `0x2800`'s q16.w * 128, the drawing-kick bits from the mask after the vertices.
- **`0x1ac0`** (image `bec5738323d4afd8`, batch type 3, alone in the table): one mesh drawn at
  several places (grass, debris). Its data block holds field A's and field B's register
  writes (`q0`-`q5`), what makes field B's matrix from field A's (`q6`-`q9`, each row of the
  instance's matrix times it), the guard band (`q10`, `q11`), the number of instances
  (`q12.x`) and per instance a colour and field A's matrix (`q13`-`q17` + 5 per instance).
  A packet of both fields per instance, from two buffers after the input in turn; the
  colour the vertex's times the instance's. A vertex does not start a triangle when the mask
  says so or when it or one of the two before it is outside the guard band (`CLIP` of
  `q11 * w + q10 * the position`), per field; field B's history is left in `vi14`, the
  dispatcher's stack pointer.
- **`0x1ac0` / `0x1c20`** (images `422f0062aa2ca315`, `c06ad47f0d18db35` / `6b20c90260be78a4`,
  the same code, before `0x1410` in batches of type 4): one directional light and an ambient
  term - the colour, in place of the normal, `q2 * q1 * (max(N.L, 0) + L.w)` at most 255,
  the alpha `q2.w * 128` (L the data block's `q0`).
- **`0x3660`** (images `2011a15b85aa5a94`, `25cef3e84459f32b`, before `0x1410`): the alpha
  of each vertex its distance from a plane (`q0`), from 0 to 128; the alphas of the last
  three vertices kept as a running sum (the oldest taken off, the newest added, each
  rounded), and a vertex whose sum's addition sets the MAC zero flag gets its drawing-kick
  bit (`FMAND`): a triangle faded out entirely is not drawn.
- **`0x13b0`** (the shared core, before `0x1410`): every vertex's colour the data block's
  `q0`, four at a time.
- **Batch types.** The dispatcher converts types 1 to 4 (`0x170`, `0x220`, `0x2c0`, `0x388`)
  and clears the type; any other type (`0x8004`: four-vertex quads, always clipped) has no
  conversion. Type 1 (`0x22a0`'s sprites) is a vertex of two qwords, colour and position,
  the position less the header's z (w less `(1 + z) / 1024`).
- **`0x22a0`** (image `bec5738323d4afd8`, type 1, alone): a sprite per vertex, for each field.
  The centre is the position through the field's matrix times Q = 1 / max(w, 0), x and y
  from 1000 to 3000, z at most 1.6e7; the corners the centre less `q15 * w * Q` and plus
  `q18 * w * Q` (each at most `q10.zw`, w the position's own), their ST `q11` and `q14`
  times Q; the second corner is not drawn when the field's w is negative - read as the MAC
  sign flag of the transform's last step. (With `q15.xy` both 0 the routine takes another
  path, with ST not times Q.)
- **`0x1df0`** (images `2011a15b85aa5a94`, `25cef3e84459f32b`, type 3, before that image's own
  `0x22a0`, a `0x1410` with a second set of coordinates): a texture projected onto the mesh.
  ST x and y become the position through `q0`-`q3`, ST's w and the position's w the
  projector's coordinates (`q4`-`q7`), the colour `q8` with the alpha `q8.w` times bit 7 of
  the vertex's; a triangle all outside one side of the box (`CLIP` of `2 (t - 0.5)`: x, y,
  and -z) is culled. With nothing left it sends an empty packet (`q1023`) and ends the
  table - which on the dropship walk is nearly always the case.
- **`0x21f0`** (image `422f0062aa2ca315`, type 4, before `0x1410`): two directional lights
  and a highlight, worked out in the mesh's own space - since the draw distance went to 2x
  (the eco forest's trees as meshes twice as far) the batch kind VU1 ran most. Per batch it
  inverts field A's 3x3 matrix (cofactors of `vf01`-`vf03`, over the determinant) and takes
  the lights (`q5` and `q1`, normalized with `RSQRT`) and the eye (`q0`, less `vf04` first)
  into the mesh's space. Per vertex, in place of the normal: the colour `q3 * q2 * ((q6 *
  max(N.L1, 0) + q2 * max(N.L2, 0)) * q1.w + q5.w) * 128` from 0 to 255, the alpha `q4.w *
  max(R.L2, 0)^8` with `R = 2 (N.V) N - V` (V to the eye, `ERLENG`). Its loop does all but the
  last three vertices, then those three: a batch of fewer than three is left to the microcode.
- **`0x1df0` then `0x2cc0`** (image `e1472a8c96f62e62`, type 3: ST, colour, position): water
  (the river). The height of each vertex is moved by two waves, each the cubic `s - s^3` of a
  phase `s = 2 frac(p + 100000) - 1` (the fraction as `x - ITOF0(FTOI0(x))`), `p` = the
  position's x and y by the wave's (`q2`, `q3`; plus its w) times its speed (`q1.y`, `q1.w`),
  times the wave's height (`q2.z`, `q3.z`). The normal comes from the slopes - the same cubic a
  quarter of a period on (`-0.75`) - by the waves' x and y times `q1.x`, its z `q1.z`. The
  direction to the eye (`q0`, from the unmoved position, `ERLENG`) reflected about it gives
  sphere-map coordinates `0.5 + R * 0.5 / (Rz + 1)`. Four copies of the strip go out with the
  lit meshes' tags: context 1 the ST moved by `q4.xy` (the current) with the vertex's colour,
  context 2 the sphere map with the colour times `q21`, each for field A and B (field B's
  matrix `0x2cc0`'s `q0`-`q3`).
- **`0x1ac0`** (images `6b20c90260be78a4`, `e1472a8c96f62e62`, `2011a15b85aa5a94` - the same
  bytes; not the lighting `0x1ac0` of 422f and c06a): the texture coordinates through a 2x3
  matrix, `q0 * s + q1 * t + q2` (in the captures the identity and an offset that moves from
  frame to frame: scrolling textures); z and w kept. Four vertices at a time and a group more,
  so 1 to 7 slots past the last vertex are written too (the masks' qword saved and put back);
  it leaves VF16-VF19 holding the ST of the four slots after its last group - which 0x0480's
  register save puts on the stack when the batch clips (read by an agent with forced clipping
  on the trace: the same held, unnoticed, for `0x13b0`'s VF26).
- **`0x1c20`** (images e147 and 2011; image 6b20's `0x1c20` is the lighting one): every vertex's
  colour, all four lanes, times `q0` plus `q1`, from 0 to 255 (in the captures 1 and 0).
  Pipelined three vertices deep: a count of 1 or 2 writes three vertices, 3 or 4 writes four.
- **Image 2011's `0x22a0`** (after `0x1df0`, when something is left to draw): `0x1410` but for
  field B's texture coordinates - field A (over the input) has `0x1df0`'s coordinates in the
  mesh's box times its Q, field B the projector's (`0 + position.w`, `0 + ST.w`, 1) times its
  Q; tags and packet as `0x1410`'s. Clipping (`q21.x` above 0) is `0x1410`'s clipping path
  with three differences: each position's w (the projector's x) is kept after field B's tags
  before the masks go into it; between the two runs of `0x0480` every ST becomes the
  projector's coordinates (with one vertex, a second one too); and `0x0480` is called from
  `0x2730` and `0x28f8`. `0x1df0` leaves in VF17 and VF18 the last vertex's ST and colour, which
  the clipper's stack saves. (Every drawing batch captured clips; the drawing path was checked
  on 6,152 variants made from them against the trace. `0x1df0` shifts its second mask word at
  least once, so with 32 vertices vertex 16's bit falls off and the others land a place higher
  - the earlier native culled case had them a place lower, unseen as no batch had 32.)
- **`0x3cc0`** (image `1a707099ecafadff`, alone, type 0): a flat strip in one colour into a 128 x
  128 scissor (a shadow or mask texture by the look of it): a vertex is one qword, through the
  data block's q0-q3 (no divide) to 12.4, in place; the tags (the block's q4-q7) before it.
- **`0x2cf0`** (image `8673b3b4fc9a8b0c`; the same bytes in 2011, alone; the header `0x8000 | n` in
  every word): `0x22a0`'s sprites for field A only, the texture whole (ST the data block's q1
  and q4 as they are). Its path for `q0.x` zero (quads, from `0x3168`) has not been seen.
- **The set-up programs** (the shared core): `MSCAL 0` reads how many routines the table has
  (`q4.x`: vi13) and puts the dispatcher's stack at 1020 (vi14); `MSCAL 0x450` loads field A's
  screen transform (`q0`-`q3`: vf01-vf04). Two and four pairs, some 60,000 runs a second.
- **Sprites of type `0x8001`**: the same `0x22a0` (in image `6b20c90260be78a4` the same code at
  `0x2740`) on vertices the dispatcher leaves as they are - one sprite a batch.
- **`0x1380`** (image `47dd78e8f5335846`, which has a core of its own): grass. Each strip
  triangle that is not culled (all three corners outside one plane in both fields) or masked
  is drawn as tufts: for each of its corners a number of packets from that corner's alpha,
  the triangle's area and its distance (`DIV`, the whole part: packets of eight blades from a
  table at the data block's `q24`, each entry two blades; the fraction above 0.1: one more,
  its blades shorter by it); more than eight packets for a corner, and the triangle is split
  in four and each part drawn the same way (`0x20a8` calls itself, its state on VU1's stack
  at `vi14`). A blade is the point the table gives on the triangle and that point plus the
  up vector (`q2`), both through the field's projection, colours interpolated with 2^23
  added (the integer lands in the float's low bits); two packet buffers in turn. Not
  rewritten by hand: recompiled whole (below).

### How the routines are read

The microcode is software-pipelined: a vertex's work is spread over three or four turns of a
loop, mixed with its neighbours', and Q and P are read on purpose before the next result
lands. `tools\vu1_trace.py` runs a captured batch (`PS2X_VU1_CAPTURE`, which saves the code
too) on a Python model of the fast engine - the same timing (stalls, Q and P latencies, the
branch's view of a VI written just before, the MAC, status and clip flags four cycles after
the instruction that sets them, `FCSET`/`FSSET`) and VU1's arithmetic, done exactly with rationals and rounded
toward zero - with, beside every value, the expression it was computed from. The run must
leave data memory as VU1 did and send the same packets, word for word (it does on every
batch tried); then `--same` shows whether every vertex is computed alike, `--spec <copies>`
prints every word the batch writes as one formula per copy, slot and lane, and `--kick <k>`
packet k as it was sent (routines that draw many packets from the same buffers). Where the
control flow depends on the data (the clipper), a model of the routine in Python is checked
against the trace on every captured batch first - memory, scratch buffers and stack
included - and the C++ written from the model. `PS2X_VU1_NATIVE=check` then holds the
native code to the microcode.

### Microcode recompiled

Software-pipelined, branchy routines are long to read by hand. But a VU1 program run in
program order does what VU1 does, but for a few reads: VU1's interlocks stall a read of a VF
or VI register until the latest write to it has landed, so those always see program order;
what its pipeline lets lag behind are Q (`DIV`, `SQRT`, `RSQRT`), P (the EFU), the clip flags
and the MAC and status flags, which a pair reads as they are when it issues - on purpose, in
the game's software-pipelined loops (a `MULq` reading the `DIV` before the latest, an
`FCGET` reading the judgement from two `CLIP`s back) - plus a branch's view of a VI written
by the pair just before it.

- `tools\vu1_lags.py` runs captured batches on `tools\vu1_trace.py` (which now numbers every
  producer and notes which one's result each read sees) and lists, per pair, how many
  producers behind program order each such read is. Where every site always has one lag, the
  program can be rewritten in program order.
- `tools\vu1_recompile.py check` runs captured batches that way - the trace's instruction
  semantics, every result landing at once, the lagged reads taking the producer that many
  back - and compares data memory and packets with VU1's.
- `tools\vu1_recompile.py emit` writes the program, from its entry points, as one C++
  function: every pair a block of SSE code in program order, Q, P and the flags kept as
  short histories read that many back, the flags worked out only for the instructions whose
  flags are read, the branch's VI view kept, the dispatcher's and the subroutines' jumps as
  a switch. The whole program is recompiled - dispatcher, conversions, routines - so a batch
  runs from its start pc to its end exactly as the microcode does.

Image `47dd78e8f5335846` (the grass, `0x1380`) is the first: 607 pairs, every timed read with
one lag over its captured batches, the program-order run identical to VU1's on all of them,
and the generated `transformers\src\vu1_grass47dd.inc` identical to the microcode over
every batch the check saw.

### What the scene does not feed back

The game reads back one buffer a frame (page 384, the sun's glare test), and nothing in the
display list draws there: what the scene's draws produce never reaches the game's logic.
The VU1 counter at `0x1100C0B0` is read and dropped. So drawing the scene differently cannot
change how the game plays - only how long a frame takes, which the game's clock already
follows (see [NOTES.md](NOTES.md#draw-distance)).

## Done so far

- **Tools.** `PS2X_VU1_ROUTINES=1` (the table above); `PS2X_VU1_CAPTURE=<routine
  table>[@<image>][#<type>][:<n>]` saves batches - registers and data memory before and after, the packets VU1 sent - for
  `tools\vu1_batch.py` (reads one) and `tools\vu1_routines.py` (reference implementations in
  Python, VU1's rounding included, compared with the packets); `tools\vu1_liveness.py`
  (which registers a program run has to leave for the runs after it).
- **What a batch leaves behind.** With calls followed as calls, the only registers a batch
  reads from before it are the dispatcher's `vi13` (routines in the table) and `vi14` (its
  stack) and field A's matrix `vf01`-`vf04`; the clip flags only as old bits shifted out,
  and `vf17`-`vf31` only in a subroutine (`0x1180`) that saves them on the stack before
  using them and puts them back. So a run done some other way has to send the same packets
  and leave the same data memory, not the same scratch registers.
- **Native batches.** `ps2vu1native` (`runtime/ps2_vu1_native.h`) lets a game do a program run
  itself: the engine offers every run to the hook, in VU1's float mode, and the hook either
  does it (writes data memory as the microcode would, sends the packets, sets the end pc) or
  leaves it to the microcode. `PS2X_VU1_NATIVE=check` runs the microcode as usual and the
  hook on copies, and compares packets, all of data memory, the end pc and the registers a
  later run reads (`vi13`, `vi14`, `vf01`-`vf04`); `PS2X_VU1_NATIVE=0` turns it off. The
  game's hook is `transformers\src\vu1_native.cpp`.
- **Routine `0x1410` natively** (2026-10-04, night): batches of type 3 with the table
  `1410/... 0480/...` where `0x1410` draws (`q21.x` not above 0), in any image with the
  shared core (its bytes are checked once per image): the type 3 conversion, both fields'
  transforms, Q, STQ, 12.4 fixed point, the drawing-kick bits, the tags - in SSE with
  VU1's rounding (toward zero) and operand clamps, a product and a sum rounded separately.
  Checked over 1.5 million batches (start of Amazon Basin and the dropship walk): packets,
  data memory and end pc identical every time; all 22 frames of both renderers identical.
  Flat out at 144: the start of the level 143.9 to 147.7 fps, the dropship walk 89.1 to
  95.5. (Built without its helpers inlined - the game's sources build with `/Ob1` - it was
  no faster than the JIT: they are `__forceinline` now.)
- **`0x2150` and `0x21b0` before `0x1410`, and the lit meshes** (`0x1d10`, `0x2800`; four
  vertices at a time, one an SSE lane): checked identical over 0.7 million batches at the
  start of the level and 1.7 million on the dropship walk, all 22 frames identical. Flat out
  at 144 (base: native runs off, run in turn): the start of Amazon Basin 140.1 to 159.0 fps,
  the dropship walk 85.6 to 100.4. The start of the level is now the EE's alone: it no
  longer waits for VIF1 at all (`PS2X_MTVU_STATS`: wait to settle 0.00 ms) and is 97% busy.
  In real time at 60 on the walk 2.61 cores are busy (2.70 before), the VIF1/VU1 thread
  54% (69%); the GS front end (53%) and the GS thread (59%) are now as big.
- **The grass, recompiled** (2026-10-05, later): image `47dd78e8f5335846` run by its
  generated program; identical over 11 million batches on seven save states, all 22 frames
  identical. The microcode left on the dropship walk 4.1 to 0.9 million pairs a second; in
  real time at 60 the VIF1/VU1 thread 36% (42%), the grass's code 1.4% of it where the JIT
  took about 7%, the whole process 2.38 cores (2.48).
- **The sprites (`0x22a0`) and the projection (`0x1df0`, when it culls everything)**
  (2026-10-05, later): exact over 6.8 million more batches; the microcode left on the
  dropship walk 5.7 to 4.1 million pairs a second, `0x1380` 77% of it. In real time at 60
  the VIF1/VU1 thread 42% (45%), the JIT's code 10% of it.
- **The instances, the lights, the clipping path, `0x3660` and `0x13b0`** (2026-10-05): checked
  identical over 12 million batches on ten save states (packets, data memory, end pc and
  now the live registers too), all 22 frames identical. The microcode left on the dropship
  walk at 60 went from 26 to 5.7 million instruction pairs a second: `0x1380` (image
  `47dd78e8f5335846`) more than half of it, the two `0x22a0`s (sprites) a third. Flat out at
  144 the walk 87.4 to 103.8 fps (native runs off and on) - the EE's own work is what holds
  it there now (`PS2X_MTVU_STATS`: VIF1 0.03-0.19 ms behind a frame of 7-8 ms); in real time
  at 60 2.48 cores (2.61), the VIF1/VU1 thread 45% (54%), the JIT's code 15% of it.

- **The rest of the library** (2026-10-06, stage 1 of the plan below): `0x21f0`, the water,
  the sprites of type `0x8001`, the two `0x1ac0`s and `0x1c20` (texture matrix, colour scale),
  image 2011's `0x22a0` with `0x1df0` drawing and clipping, `0x3cc0`, `0x2cf0` and the two set-up
  programs - three of them read by agents from captured batches (a Python model checked
  against every capture and against the trace on made-up batches, then C++ in this file's
  style). Every batch kind seen now runs natively: on the dropship walk VU1's microcode went
  from 54 million instruction pairs a second (flat out, the forest at twice the distance) to
  none. `PS2X_VU1_NATIVE=check` found nothing over about 45 million batches on the fifteen
  level save states; all 22 frames identical at scale 1 and 3. On the way two latent slips in
  the earlier native code came out, both where no captured batch went: `0x13b0` leaves its
  colour in VF26, which the clipper's stack saves (now left in the state), and the 32-vertex
  masks above. `0x21f0`'s batches that clip stay the microcode's (it leaves its own values in
  VF17 and VF18 for the clipper's stack).
- **Stage 2, `0x1410`'s drawing as meshes** (2026-10-06, night; on by default when the GPU
  renderer draws alone, `PS2X_GPU_MESHES=0` turns it off): the batch goes to the GS as a packet
  of its own and the GPU makes the vertices in a compute pass - in VU1's arithmetic, so the
  vertices are the CPU's bit for bit (below, "Stage 2 in detail"). `PS2X_GPU_MESHES=3` (every
  vertex made on the CPU as well and compared) found none different over 660,000; the GPU-only
  frames with meshes and without are identical on all five cases at scale 1 and 3. In real time
  at 4K, 60 frames and 120 pictures near the drop ship, meshes off and on in the same build:
  3.04 to 2.62 cores busy - the GS front end 68 to 38%, the VIF1 thread 47 to 41%, the
  renderer's thread 75 to 66% (with its texture pack lookup remembered, which matters once
  every field prepares a draw state: [RUNTIME.md](RUNTIME.md#on-the-gpu)). Flat out nothing
  changes (the EE's own work sets the pace there).
- **Stage 3 begun: the scene through the GPU's own depth test and blending** (2026-10-06,
  night; `PS2X_GPU_NATIVE`, on by default since 2026-10-08 with the user's OK): flat out at 7x5
  in 16:9 the GPU's time a picture near the drop ship 6.31 to 3.81 ms, near the Mini-Con 6.39 to
  3.93 - under half of 8.3 ms, the budget of 120 pictures a second - and the frame 13.4 to 11.4
  and 13.5 to 10.7 ms (the CPU's now). 99.8% of the draws go the new way; against the emulated
  pictures (22 frames at scale 3) a mean difference of 0.02 a channel once the GS's rounding was
  matched (0.40 before), the 99th percentile at most 1, SSIM 0.9999 - closer than the software
  and the GPU renderer are to each other; the player's path the same (`TF_FRAME_LOG`, 2,401
  frames each of the dropship walk and the Mini-Con fight) (below, "Stage 3 in detail").
- **Stage 4: moment pictures** (2026-10-07; `TF_MOMENTS=1`, with the native path; off until
  the user has seen them): the frame drawn at the screen's refreshes - each refresh a picture of
  its own, the moment it shows between the frame before's pose and this frame's - and the window
  showing them by the refresh, not the VBlank. Even: during a steady turn every step 10.4-10.9 px
  across a frame and from one frame to the next (moments fixed at quarters); in real time on a
  170 Hz screen at 5x3, 324 of 325 refreshes a new picture made for that refresh, within 0.03 ms.
  At t = 1 the moment is field A's picture to 99.98% of the pixels; the player's path the same
  (2,401 frames each, moments off and on); the default path's 22 frames identical. At 4K (7x5)
  the GPU's time is the limit: about 4-4.6 ms a moment, so a 144 Hz screen (2.4 moments a frame)
  needs about 13-14 ms of the frame's 16.7 - the moments' cap keeps the game at full speed and
  shows fewer moments where it falls behind (below, "Stage 4 in detail").
- **The warp gates' loading screen** (2026-10-08): a microcode image of its own (copied into
  VU1 by the EE, `memcpy` to `0x11008000`) that the EE starts at every VBlank by writing CMSAR1,
  run by the JIT like any image the native hook does not know - tunnel layers, about 100,000
  points and the bar, sent by XGKICK. It draws two 640 x 224 buffers (pages 0 and 70) in the SDK's
  frame mode: the presenters show such a display as half its `DH` in buffer rows, and the level's
  fields buffer, second-field and moment settings (block 0) stand aside while it is up. Above
  scale 1 a fill's edge on a whole coordinate covers whole GS-pixel blocks
  (`PS2X_GPU_SPRITE_EDGES`): the SDK's clears from 0 to 640 left a line at the top. GPU against
  software renderer: mean 2.3 of 255 (its 64 x 64 textures magnified across the screen). Details:
  NOTES.md ("Frame loop and display") and RUNTIME.md.

## The plan

The user's brief (2026-10-06, branch `native-renderer`): draw the game with the graphics card's
own pipeline instead of emulating the PS2's graphics hardware, and keep everything about the
simulation as it is - fixed 60 steps, the game's own interpolation, physics untouched; only how
pictures are made changes. The targets: 4K at 120 everywhere with no in-between pictures left
out, sharper pictures, and later evenly paced pictures at any refresh rate (144, 165, 240 Hz).
Every stage behind a switch, the emulated path kept as the fallback and the reference; exact
steps may be on by default, the others stay off until the user has compared them and agreed.

Why it is safe: what the scene draws never reaches the game's logic (above, "What the scene
does not feed back").

0. **The in-between as a cap** (done 2026-10-05/06, the safety net whatever comes later): a
   frame whose two field pictures the GPU cannot draw in time leaves its field B out and shows
   field A at both VBlanks; decided on the GPU thread at the flip
   ([RUNTIME.md](RUNTIME.md#on-the-gpu)).
1. **Map the routine library** (now): for each routine in the table above, in order of cost,
   its input layout, data block, the math and the packets it writes, in this document, with
   a reference implementation checked against captured batches - and then the routine done
   natively on the CPU, bit for bit (as `0x1410` is). That is exact, so it can be on by
   default as soon as the check finds nothing, and it is already a speed-up: VU1 emulated
   costs about four times what the same arithmetic costs written out. Done: `0x1410` and its
   clipping path (`0x0480`), `0x2150`, `0x21b0`, `0x1d10` with `0x2800`, both `0x1ac0`s,
   `0x1c20`, `0x3660`, `0x13b0`, the sprites `0x22a0` (and `0x2740`, type `0x8001`), `0x1df0`
   when it culls everything, image `47dd78e8f5335846` recompiled whole (the grass), `0x21f0`
   (two lights and a highlight), the water (`0x1df0` with `0x2cc0`), the texture matrix and
   colour scale (`0x1ac0`, `0x1c20`), image 2011's `0x22a0` drawing and clipping, `0x3cc0`,
   `0x2cf0` and the set-up programs: done (2026-10-06) - no microcode is left on the dropship
   walk. Kinds not seen yet run on the microcode as before.
2. **Native batches through the GPU renderer.** For a batch kind whose routines are all
   known, the VIF1 thread hands the GS thread the batch's vertex data, matrices and draw
   settings instead of running VU1; the GPU computes the vertices in a vertex shader and
   draws them through the existing pixel pipeline, in the same place of the command stream.
   Since the in-between pictures (2026-10-05) field B is drawn too - the frame's second
   picture, the moving parts halfway - so a mesh goes out as its two fields (and its two
   contexts where it has them), as the microcode sends it. Expected to differ from the
   emulated pictures by a fraction of a pixel (the GPU's float arithmetic is not VU1's), so
   behind a switch; in the end the compute pass does VU1's arithmetic exactly, so the first
   kind (`0x1410`) is on by default. What it saves: the VU1 work of those batches, their
   packets (about 700 MB a second were copied to the GS front end), and the front end's work
   on their primitives.
3. **The pixel side**: draw with the GPU's own depth test, blending and multisampling
   instead of the interlocked emulated pixel pipeline (the interlock is about a third of the
   scene's cost, 20 ns a primitive whatever its size): the GS blend modes, the alpha test,
   DATE and FBMSK mapped (alpha 0x80 is 1.0); the frame's effect passes rewritten as passes of
   our own - the pre-pass into a 256-wide region of the next display buffer (C32 and C16), the
   glow and blur buffers at block 14848 and the 128 x 128 ping-pong at 0/256, the depth buffer
   read as colour (Z16), the 136-draw pass on block 0 with texture 16382, the field copies
   (`PS2X_GS_TARGET_LOG` lists them) - and every picture drawn at the screen's full
   resolution rather than as a field of 224 lines (twice the detail down).
4. **Pictures at any moment** (begun 2026-10-07: "Stage 4 in detail" below). First meant as the
   engine's render layer decompiled (the `vl*` traversal, for each object's transform of the
   frame before and of this one); read first instead, and not needed: every batch already
   carries two transforms, field A's and field B's, and with the game's in-between factor at 0
   field B is the frame before's pose - so the GPU draws any moment between the two, a picture
   for each refresh of the screen (144, 165, 170, 240 Hz), the game's simulation and its
   traversal as they are.

### Stage 2 in detail

Where a frame's CPU time goes now (16:9, flat out at scale 1 near the drop ship, 12 ms a frame,
2026-10-06): the GPU renderer's own thread 90% busy - converting the GS front end's vertices
for the GPU (`appendPrimitives`) 37% of it; the GS front end 75% - decoding the GIF packets and
assembling primitives (`processGIFPacket`, `vertexKick`) about half of it; the VIF1 thread 59%.
For a batch drawn natively all of that is work done three times over on the same vertices:
the native routine writes them into VU memory as a packet, the front end decodes the packet into
vertices one register at a time, the renderer turns those into its own vertices. Stage 2 sends
the batch itself instead:

1. **The mesh as a packet of its own.** For a batch kind the GPU can draw, the native code sends
   one packet in place of the routine's, through the same PATH1 and the GIF arbiter, so it
   reaches the GS front end in the same place of the stream: a marker (a GIF tag of the
   disabled kind, no data, with a magic word where its registers would be - nothing the game
   sends), then the batch: its vertices as uploaded (converted), what its routines computed per
   vertex on the CPU (the lighting pre-passes stay there: cheap and exact), both fields'
   matrices, their tags and register writes, the drawing-kick masks.
2. **The front end** takes the tags and register writes through its usual path (SCISSOR,
   XYOFFSET, PRIM: the GS state stays as the packet would leave it), and hands the renderer the
   mesh with the draw state of each field instead of decoding vertices.
3. **The renderer** keeps room in its vertex list for the mesh's triangles (the masks give how
   many) and a compute pass fills it on the GPU before the batch is drawn: each corner through
   the field's matrix, Q = 1/w, x and y to 12.4 and back as the GS front end has them, Z, STQ, the
   colour, the fog - what `appendPrimitives` would have written. The batch's draws stay one call
   and in order; the pixel side is untouched.

In steps, behind `PS2X_GPU_MESHES` ([DEBUGGING.md](DEBUGGING.md)): first the plumbing with the
expansion done on the CPU (`1`), which draws the same pictures bit for bit (it proves the order
and the state); then the expansion on the GPU (`2`). That was expected not to be exact - a
coordinate a sixteenth of a pixel off now and then - but VU1's rounding toward zero can be had
on a GPU: a product rounded to nearest plus its exact error in double precision (a float times
a float is exact in a double; the AMD driver did not fuse `fma`, `precise` or not), a sum's
error by TwoSum (kept from being reassociated by XORing in a uniform zero), a reciprocal checked
by its residual in double, then one step toward zero where the error says so. `3` makes every
vertex on the CPU as well, reads the GPU's back and compares: none different over 660,000, so
`2` is the default. Both are done for `0x1410`'s drawing (type 3 or 4, any pre-passes on the
CPU first); batches that clip stay packets (the GPU could clip them itself, as the
microcode's guard band does).

What is left, by vertices (`TF_VU1_NATIVE_STATS=1`, 2 s in real time at 4K near the drop ship,
16:9, the forest at twice the distance):

| Way | Batches | Vertices |
|---|---:|---:|
| `0x1410` as meshes | 381,697 | 9,804,382 |
| instances (`0x1ac0`: one mesh at up to 64 places, a guard band per vertex) | 12,528 | 1,898,448 |
| lit meshes (`0x1d10`, `0x2800`: four copies) | 56,023 | 1,115,015 |
| sprites (`0x22a0`) | 145,815 | 1,073,739 |
| clipped (`0x0480`) | 20,991 | 347,092 |
| flat strips, projected, field sprites, water | 9,281 | 186,746 |
| grass (47dd, its own program) | 3,976 | - |

Near the Mini-Con the water is 259,000 and `0x21f0`'s clipping batches (still the microcode's)
141,000. Each of the other kinds would save less than a tenth of a core: on the front end about
60 ns a vertex, on the renderer's thread somewhat less. The GPU's side is now the bigger gap:
5.8 ms a picture at 7x5 (draws 5.0, field pictures 0.73) against 8.3 at 120 pictures a second -
70% of the budget where the brief asks about half, which is stage 3's.

Done when: the drop ship and the first Mini-Con at 4K hold 120 pictures a second with no
in-between left out and the GPU under about half its budget; the cores spent emulating the
graphics are substantially fewer; the native pictures match the emulated ones within an agreed
tolerance and are sharper (full-resolution fields, MSAA); the player's path is the same as
with the emulated renderer (`TF_FRAME_LOG` / `TF_GOTO` logs; fights can come out differently
through the COUNT-based clock - timing, not physics); stage 4: an even 144 Hz, the view's
step checked every picture.

Open questions to settle on the way: what the second context's copy draws (a second texture
layer, per [NOTES.md](NOTES.md#rendering-notes)); whether the clipping routines' output
matters beyond guard-band clipping (the GPU clips by itself); how the EE builds the routine
tables (per object, per material?), which decides where native batches can be recognized
most cheaply; for stage 4, where the engine keeps each object's transform of the frame before
and of this one, and what the motion interpolators (factor `0x6CD6F0`) already give.

### Stage 3 in detail

The emulated pixel pipeline reads and writes the frame and depth buffers as R32UI images inside
an ordered pixel interlock: the GS's arithmetic to the bit, and about a third of the scene's
cost (20 ns a primitive whatever its size, [RUNTIME.md](RUNTIME.md#on-the-gpu)). The native
path (`PS2X_GPU_NATIVE=1`) draws a batch with the GPU's own depth test and blending instead.

What a frame asks of it (two frames logged with `PS2X_GPU_DRAWLOG`, near the drop ship and
the Mini-Con, `drawlog_summary.py` / `state_runs.py` in the session's scratch): the scene is
frame `0/8` (C24, and C32 for some draws) with Z24 at 7168 or 3584, about 210,000 primitives;
98% test depth GEQUAL; the blends are opaque (`(Cs-0)*128`, 52%), alpha (`(Cs-Cd)*As+Cd`, 38%)
and additive (`(Cs-0)*As+Cd`, 10%), plus some `(Cs-Cd)*FIX+Cd` sprites and 22,000 C32 sprites
blending by destination alpha (`(0-Cs)*Ad+Cs`, alpha masked) after a pass that writes alpha
only; the alpha test only ever keeps nothing where it fails. Done the GPU's way that is about
220 changes of depth state a frame - not 18,000, the draw states logged - and three runs (52
primitives: DATE, AFAIL) that are not.

How it is drawn:

- **Colour in the buffer's own bytes.** A target's texture is an immutable R32UI texture; an
  RGBA8 view of it (`glTextureView`: the same 32-bit view class) is the native batch's colour
  attachment, and its bytes are the GS's CT32 word (R the low one). So nothing is converted:
  every emulated pass, field picture, write-back, read-back and the display read what the
  native batches drew, and the stored alpha is the GS's own byte.
- **Depth in a depth texture of its own** (D32F: the GS's Z over 2^24 is exact there; through
  NDC -1 to 1 and back too, without `glClipControl`, which raylib's GL 4.3 loader lacks), kept
  with the depth target: unpacked from the R32UI texture when a native batch needs it and the
  texture is newer, packed back (the value bits; a Z24 word's top byte stays) before anything
  else uses the texture - an emulated batch with that depth buffer, a texture read of it, a
  write-back to local memory, growing, eviction, loading stale cells.
- **One blend for every mode.** `(A - B) * C + D` is a sum of the source and the destination,
  each with a factor; with C known in the shader the blend `ONE, SRC1_COLOR` (dual source) does
  them all: output 0 carries the source's part, output 1 the destination's factor, and alpha
  the same way (`ONE, SRC1_ALPHA`: the alpha stored, or kept for C24 and a masked alpha byte).
  The frame mask, a byte at a time, is a factor of 1 for the destination. C is As, FIX, or Ad -
  0x80 on a C24 frame, and on a C32 frame read from a copy of the frame made as the batch is
  drawn (a batch with such draws writes no alpha, so it is the alpha there). Draws are left to
  the emulated path where a factor would leave 0 to 1 (-C, 1 + C), for DATE, for a failed alpha
  test that writes something, for a frame mask not of whole bytes, for 16-bit frames and field
  pictures; a batch is native or not as a whole.
- **Runs.** A native batch is still one batch: its vertices in order, drawn in runs of one depth
  test and depth write (`glDrawArrays` over each run's vertices); no repeats folded.

The rounding: the GS's `((A - B) * C >> 7) + D` is floor(Cs * ks + Cd * kd); the GPU's blend
rounds it to nearest. Where Cd's factor is 0 or 1 (opaque, additive, C of 0 or 0x80) the shader
works the source's part out in the GS's own integers, so those are exact; where it is a fraction
(the alpha blends), the source's part less 127/256 makes round-to-nearest the GS's floor (the
fraction is a multiple of 1/128), and for a source too dark to take that off, the rest comes off
the destination's factor. Dithering, on by default in GL, is off for the native draws. (An agent
brute-forced all 81 blend forms over every Cs, Cd and C for this - the session's scratch,
`blend_agent\`.) What is left that differs: a factor above 1 (alpha above 0x80) is 1, depth is
interpolated in floating point, a dark source in an alpha blend can still be one off.
`tools\check_native.ps1` measures it (`tools\native_compare.py`): over the 22 frames of
`check_frames` at scale 3 a mean difference of 0.02 a channel (0.40 with the GPU's own rounding),
the 99th percentile at most 1, no pixel off by more than 16 but for a few on edges, PSNR 63 dB,
SSIM 0.9999 - two correct renderers (the software and the GPU one at scale 1) differ by 0.07 and
0.9995.

Speed (flat out, 16:9, 7x5; native off and on): near the drop ship 13.43 to 11.38 ms a frame,
the GPU 6.31 to 3.81 ms a picture (draws 5.09 to 2.38); near the Mini-Con 13.54 to 10.73, 6.39 to
3.93. With the GS's rounding matched 3.88 and 4.12 (the shader's few more instructions, and the
run-to-run spread). A frame flat out is the CPU's now: the EE thread (the game's code, the IOP)
and the GPU renderer's own thread (vertices converted, textures copied into its picture of
local memory, the GL driver) both near 90% busy. Packing and unpacking the depth cost about 0.2 ms a picture (the depth buffer read as
colour, Z16, is what needs it most). In real time the field pictures looked slower with native
on - the GPU idles more and clocks down; flat out they are the same.

A level frame, pass by pass (read from the two logged frames by an agent; its tables are in the
session's scratch, `passes_agent\passes.md`): both are the same 41 passes in the same order, the
scene 99.2% of the primitives. This frame's depth buffer is block 7168 (D) while 3584 is shown;
the next frame swaps them.

- P0 clears the scene's colour (frame `0/8`, C24) and depth (Z24 at D), 512 x 448. P1-P4 draw
  two shadow textures: A (blob shadows, C32 256 x 256) over D's rows 0-127, B (the player's
  shadow, 128 x 128 at block 14848); a C16 copy puts each one's green into the top byte of its
  words. P5 clears field A's rows again (P1 wrote over D's).
- P6 and P8 set the scene's alpha to 0 (C32, FBMSK 00ffffff); their TEST is 0x3: ZTE 0 with ZTST
  0. Both of the runtime's renderers took ZTST 0 as NEVER whatever ZTE said, so they drew nothing;
  the GS with its depth test off (ZTE 0) passes every pixel, and PCSX2 draws them. Drawn since
  2026-10-08 (below, "ZTE 0").
- P7 characters, P9 the world (opaque and cut-out, then decals), P11 the sky; P10 the shadow
  receivers: the terrain drawn again (C32, depth tested, no Z written) reading A from the depth
  buffer's top byte and B as T8H, writing the shadow's strength into the frame's alpha; P12 the
  22,000-26,000 grass sprites, `(0-Cs)*Ad+Cs` with the alpha masked - the grass darkened by the
  shadow on the ground under it; P13 foliage billboards and two-layer transparent meshes.
- P14-P15 the depth fog: a Z16 view of D copies depth bits 8-15 into each word's top byte, and a
  full-screen strip reads them as T8H through a CLUT (the fog's ramp) over the pixels at or beyond
  its Z. The only pass that reads the depth: done natively from the native depth it would need no
  depth copy at all (the agent checked the shuffle against the GS's address tables: the texel at
  (x, y) is bits 8-15 of the Z24 there, for every pixel).
- P16 a full-screen tint, P17 tree foliage (alpha-blended with Z written) and effects, P18 a
  256 x 256 C16S copy of the scene that nothing reads, P19 a small object at the screen's centre,
  P20 the field copies into D (now the display buffer), then the glow (a 128 x 128 ping-pong at
  blocks 0/256, added back) and the HUD. (The glow pass by pass - shrink, threshold, warp, pyramid
  to 8 x 8, channel shifts, enlargements, vignette, three additions - and what the native renderer
  needed for it: transformers/native_render/README.md, stage 3, "The glow". The GPU renderer's glow
  differs from the software renderer's, which is the GS's: compare with `PS2X_GPU_MIRROR=1`.)

For the native path: no depth needs unpacking for the scene (it starts from P0's clear), and one
pack at most (before P14) - the page-level rules here still unpack after P1 and P14 and pack at
P14 and P20 (about 0.2 ms a picture at 7x5).

Pictures at the screen's full resolution (`PS2X_GPU_FULL_FIELDS=1`, 2026-10-06; off until
compared). The game draws each field of a frame at 224 rows - field A into the top half of the
scene buffer, field B, half a row lower (for an interlaced TV), into the bottom half - and the
field copies stretch each over the 448 rows of its picture, field B's sampled half a row lower
so that both line up (the copies' texture coordinates: field A rows 0.31 to 224.31, field B 224.81
to 448.81 over display rows 0.44 to 448.44). Each picture had 224 rows of detail, edges stepped
two picture rows at a time, and since the two fields sample the scene half a row apart, thin
edges (branches, leaves against the sky) came out differently in the two pictures - shown in
turn 120 times a second they shimmer, which the user saw as trees doubled. With the switch the
scene buffer (the full-width colour buffer at the block of `ps2_host_display::secondFieldDraws`)
and the depth buffers drawn with it are kept at twice the scale down (`Target::sx`, `sy`; the
buffer is the one the game's port names with `ps2_host_display::setFieldsBuffer` whenever the
in-between pictures are on, cap or not; every
buffer's own scale in its loads, write-backs, copies, depth copies, its use as a texture -
`uSourceScale` - and the display), so each field has as many GL rows as its picture: the copies
read it one to one, and the two pictures sample the same rows. Cost flat out at 7x5: 3.9 to 4.6
ms a picture near the drop ship, 4.1 to 4.9 near the Mini-Con (the scene's draws and the depth
copies are twice the pixels).

Edges smoothed (`PS2X_GPU_AA=1`, off until compared): the picture, as it is made, goes through a
pass of the FXAA kind (`kFxaa`) - where the luminance changes, the edge's direction, its two ends
found along it, the pixel moved across by how far it is from the nearer end, and a little of the
neighbours' average for a pixel alone. For every edge, the alpha-tested foliage's too, which
multisampling would not touch. 0.2 ms a picture at 7x5.

The depth copies a row of pages at a time (2026-10-06): a depth buffer's native copy and its
texture are newer or the same per row of cells (`Target::depthRows`), so a write-back packs only
the rows of the pages it writes, and an unpack only the rows loaded since. A frame packed the
depth buffer four times in full - for the shadow texture's 256 x 256 in its top 128 rows (twice:
drawn, then read by the receivers), a texture read from local memory, and the depth fog - and
now packs those top rows alone but for the fog: write-backs 0.48 to 0.20 ms a picture at 7x5 with
full fields. With full fields and the native path: 4.1 ms a picture near the drop ship. The fog
still needs the whole depth buffer packed and unpacked (it reads it, then writes its top bytes
through a Z16 view).

The native pixel shader made lean (2026-10-08, later). Read through Radeon GPU Analyzer (AMD's
offline shader compiler; `tools/rga_stats.py` builds each of the renderer's programs as
`gs_gpu.cpp` links it and prints, per shader, its registers, instructions, loads from memory and
the waits for them), the native pixel shader was about 830 instructions for a wave of 64 pixels,
a typical draw running some 600 of them, and it fetched the draw's registers from its entry one
by one - 23 loads, the wave waiting for each before the branch it decides. Timed in variants
(flat out at 7x5, three moments a frame near the Mini-Con; the scene's draws 5.1 ms a picture):
an empty shader 1.6 ms, the same with a discard in it 2.3 (a discard puts the depth test after
the shader), the replacement texture sampled as well 2.35 - the rest was the shader's own
arithmetic. By RGA's count it used 23 vector registers, so every wave the hardware holds was
there: neither the registers nor the loads (taking them out alone changed nothing) but the
instructions each pixel ran. Now:

- **The vertex shader works out what the pixels need** from the draw's entry, once a vertex, and
  hands it over flat: the packed switches and values (`vState`), the scissor in the target's
  pixels, the blend's factors (`vBlend`), the bytes the frame mask keeps (`vKeep`), a
  replacement's coordinates over their divisor (`vPack`).
- **The GS's integer arithmetic in floating point**: exact, its values whole numbers below 2^24
  and `>> 7` a floor of a division by 128 - no 32-bit integer multiplies, a quarter of the rate.
- **Buffers read through the texture unit**: a buffer of 32-bit pixels read as C32 or C24 and
  clamped - the field copies, the glow - is sampled through an RGBA8 view of it with the unit's
  filtering, one sample rather than four loads and the shader's own filter
  (`PS2X_GPU_NATIVE_VIEWS=0`; the unit's weights and rounding, not the GS's).
- **The glow folded in the field pictures** as the emulated path folds it: its four passes drawn
  once, the source's part added four times (the native shader takes the entry's repeat count).

The scene's draws 5.1 to 3.7 ms a picture, the field pictures 0.57 (0.74 read the emulated way;
twice that before the glow was folded); with full fields near the Mini-Con three moments a frame
17.0 to 14.0 ms of the GPU (stage 4's table below). Against the emulated renderer (`check_native.ps1`, the 22 frames at
scale 3) a mean difference of 0.03 a channel (0.02 before), the 99th percentile at most 3, SSIM
0.9999; the default pictures identical. Tried and not kept: the depth test before the shader for
the runs it is right for (no draw that can discard, or no Z written) - nothing measurable, since
every run that writes Z has a draw whose alpha test can fail (the world's cut-outs, the grass, the
foliage), and runs cut at those draws were four times as many draw calls, slower; testing every run
first (wrong for the alpha-tested ones) would save about 0.9 ms a picture, what the test after
the shader costs. The draw's registers read once a wave for the whole shader (a loop of
`readFirstInvocationARB` around it) was slower on this driver than each pixel's own; and a
replacement's sampler picked once a wave the same way (the slot as the wave's first pixel's, the
others round again) drew the wrong texture on this driver where a wave held two draws - specks
and slivers of another part's texture along the edges between parts, which the user saw on the
player (2026-10-08): taken out, each pixel picks its sampler again.

The depth fog as a pass of our own (2026-10-08, after the user's play at 4K; `PS2X_GPU_NATIVE_FOG=0`
leaves it as it was). The user saw a rough white line round rocks at some distance from them,
gone from close by. The fog (P15) draws over the pixels at or beyond its strip's Z, its amount
from the depth's bits 8-15 that the shuffle (P14) puts in each word's top byte, and both went
through local memory a GS pixel at a time: at 7x5 with full fields one value for 70 GL pixels.
In moment pictures the fog's index was the depth in the middle of its GS pixel (`kMomentFog`), and
in the in-between pictures the shuffle's trip through local memory left the native depth itself a
GS pixel a value (reloaded after it), which P15's depth test then used - either way the part of a
rock's edge in a block whose middle was the fogged distance took the distance's fog. Now:

- The fog's read of the depth buffer's top bytes (T8H at the depth buffer's own block and width:
  the rule the moments had, `momentFogSlot`) reads, in a native batch too, a fog index made from
  the batch's native depth a GL pixel at a time (`nativeFogIndex`, `kMomentFog` at a scale of 1;
  the moments' layers likewise, the GS pixel's middle only with the testing aid
  `PS2X_MOMENT_BLOCKY`); the buffer it would have read is neither loaded nor made current.
- The shuffle - a depth buffer drawn through a depth format's view while it reads its own buffer,
  from two bases (7168 and 8960 for the buffer at 7168) - is left out: only the fog read what it
  wrote, and the frame's depth bits are not its.

The rock's edge is clean in both kinds of picture (the emulated renderer keeps the console's
look). The depth goes through local memory 2 times a frame instead of 3.5 and comes back once
instead of twice (what is left: the shadow textures kept in its top rows); flat out at 7x5 the
in-between pictures 4.6 to 4.7 ms a picture near the drop ship and the Mini-Con - 55-56% of the
120-a-second budget, which the user found good enough (2026-10-08).

ZTE 0 (2026-10-08, the user's call). P6 and P8 now draw: with ZTE 0 (TEST bit 16, the depth test
off) a draw's every pixel passes and it writes no Z, as the GS does and PCSX2 has it ("write is
blocked then") - set once where a draw's state is made (`GS::buildDrawState`: ZTST ALWAYS and
the Z mask on), so both renderers, the front end and the native path see it alike. The scene's
alpha is now 0 before the characters and the world, as on the console, and the grass's shadow
(`(0-Cs)*Ad+Cs`) no longer reads a mask left from before: 0.15-0.34% of a level frame's pixels
differ, brighter in the grass. The golden frame sets were made again (`runs\golden`,
`golden_mtvu0`, `golden_gpuonly_s1`, `golden_gpuonly_s3`).

Multisampling (2026-10-08; `PS2X_GPU_MSAA=2`, `4` or `8`, 4x the default since the user's OK the same night; first for moment pictures):
each moment layer is drawn into multisampled colour and depth (renderbuffers, `MomentLayer::msFbo`)
and resolved before what reads it - the copy of its alpha for blends by Ad, the fog's index (the
depth: one sample), the copy into the picture (the colour: the samples' average). Geometry's edges
smoothed (alpha-tested edges - grass, leaves - are a texture's: FXAA, `PS2X_GPU_AA`, smooths those).
On the user's driver a blit into the RGBA8 view of the layer's R32UI texture wrote other values
(every colour wrong): the colour is resolved into a plain RGBA8 texture and its bytes copied. Cost
flat out at 7x5, three moments a frame near the Mini-Con: 13.5 ms of the GPU a frame without, 16.6
with 4x (the scene's draws 3.7 to 4.7 ms a moment), 29.1 with 8x - so 4x at 4K on a 144 Hz screen
(about 80% of its frames), and easily at 1440p. The in-between pictures' scene buffer is a GS
buffer that emulated batches, loads and write-backs share, so a multisampled copy of it has to be
resolved before each of those and filled again after - done the same night (below).

The scene multisampled (2026-10-08, night, the user's go-ahead; the same `PS2X_GPU_MSAA`, 4x by
default since): the native batches with a depth buffer - the scene of the frame's two pictures - draw
into multisampled copies of the frame and its depth buffer (renderbuffers, `Target::ms`), filled
from the buffers where something else wrote them since and resolved into them before anything
else uses them: on the way to the Mini-Con two fills and about eight resolves a frame (the frame
read by the effect passes, write-backs, the depth fog, the alpha copy). The colour is the samples'
average. The depth is where it went wrong: a GPU's is a sample's, not the pixel's middle, and the
Drop Zones room showed what handing back one sample does. The warp gate's tunnel is one mesh drawn
three times with GEQUAL - opaque, added, subtracted - and the subtracted pass, `(0 - Cs) * FIX + Cd`,
is the emulated path's (the GPU's blend cannot take the source away), so it tests against the depth
handed back; where sample 0 lay nearer than the pixel's middle - half of the cone's slopes - it
failed, and the tunnel's dark middle was gone on a wedge with straight edges (1.31 a channel off
plain native in that frame). Now a batch that tests against a multisampled depth buffer outside the
multisampled path has it handed back as the farthest of each pixel's samples (`kMsaaDepthResolve`,
from a copy of the samples): every depth test the game makes is GEQUAL, and the farthest sample is
never nearer than the pixel's middle within a surface, nor where two of a mesh's triangles meet
(the samples' depth a pixel with one sample handed back still speckled the tunnel's middle along
its triangles' edges). The rest - the fog, write-backs, the buffer read as a texture - takes a
blit's sample: on the way to the Mini-Con no batch needed the farthest, in the room six a frame.
Writing the GS's Z a pixel into every sample (`PS2X_GPU_MSAA_PIXEL_DEPTH=1`) keeps a mesh drawn
again equal on every sample whatever comes between, but turns the GPU's early depth test off for
the scene: 4x at 7x5 with full fields, flat out with RuneLite on a third of the GPU, 15.7 ms of the
GPU a frame against 12.4 (10.3 without multisampling; 49, 60 and 75 frames a second). Against plain
native at scale 3 (`check_native.ps1`'s 22 frames): 0.35 a channel (SSIM 0.994), flat areas 0.04%
more than 16 off and edges 1.4% - geometry's edges smoothed; 0.54 before the fix, the room 0.09.

Stage 3 done (2026-10-08, night, the user's OK): the scene through the GPU's own depth test and
blending and full-resolution fields, both on by default (`PS2X_GPU_NATIVE=0` and
`PS2X_GPU_FULL_FIELDS=0` give the exact pictures the golden sets are of; `check_frames.ps1` and
`check_native.ps1`'s reference pin them), the field pictures and the depth fog as native passes,
edges smoothed (`PS2X_GPU_AA`, and `PS2X_GPU_MSAA` for the scene and moment pictures), ZTE 0; native
pictures against the emulated within a mean difference of 0.03 a channel (SSIM 0.9999) but for the
fog's edges: with the native depth fog 0.21 (SSIM 0.992, `check_native.ps1` at scale 3), the
difference all where the emulated pictures have the distance's fog on a nearer edge - the outlines
round the dropship, the robots and the rocks, and the grass blades against the fogged ground
(`PS2X_GPU_NATIVE_FOG=0`: 0.03 again). The player's path the same with native, moments and 4x
multisampling as without (a11 and a12, 2,400 ticks), and with the new defaults and the scene
multisampled as with the exact pictures (a11 and a12, 2,400 ticks: no difference at all).

### Stage 4 in detail

Every VU1 batch carries both fields: field A's matrix (`vf01`-`vf04`, the frame's pose) and
field B's (q0-q3 of the routine's data block: `MV x M11 x S_B`, where M11 is the motion from this
frame to the in-between moment and `S_B` the screen transform with the in-between field of view).
The governor's in-between factor (`0x6CD6F0`, `decomp/timing/governor.cpp`) is the only thing
that places field B: 0.5 as the console has it, halfway. With `TF_MOMENTS=1` it is 0, and field B
is the frame before's pose - camera, field of view, every motion interpolator and bone - while
nothing about the simulation changes (only `0x20D3A0` and `0x20DBC0` read it, both drawing).
Checked per mesh (`TF_MOMENTS_CHECK=1`, in `transformers/src/vu1_native.cpp`): field B against the
field A the same mesh had the frame before - the median relative difference 3e-8, 99.4% of
meshes within 1e-4, 99.9% within 1e-3 (the rest: rounding of the longer product, and the few
moving parts the game's own in-between treats otherwise); with the factor at 0.5 none the same.
The player's path is the same with it on and off (`TF_FRAME_LOG`, 2,401 frames each of the
dropship walk and the Mini-Con fight).

So each frame holds two moments, t = 0 (field B) and t = 1 (field A), and any moment between is
the GPU's to draw:

- **Which moments.** At each flip the renderer takes the screen's refreshes while the next frame
  is up (`pickMoments`: the window's screen's refresh times, `ps2_host_display::screenTiming` -
  its vertical blanks and its mode's period; the frame's time from the VBlank schedule,
  `vblankDue`): from its flip, a little later - 5 ms, `PS2X_MOMENT_OFFSET` - for two VBlanks. On a
  144 Hz screen 2 or 3 a frame (t = k * 5/12 repeating every 5 frames), on 170 Hz 2 or 3 (2.83 on
  average), on 240 Hz 4. Each is a picture made for one refresh, and shown on it. With variable
  refresh (FreeSync) the screen refreshes for the pictures: they go at its mode's rate, the fastest
  it may (until 2026-10-08 the period was measured, and with FreeSync it measured the game's own
  pictures, 72 a second, and the moments fell to one a frame).
- **Drawn in the stream.** The scene's textures pass through local memory as it is drawn, so a
  frame cannot be drawn again later: each batch into the buffer of both fields is drawn into
  each moment's layer as it is flushed (`MomentLayer`: colour - an R32UI texture drawn through an
  RGBA8 view, as the buffers are - and a depth texture, field A's 224 rows at the buffer's scale,
  full fields included). The buffer itself is not drawn at all (`PS2X_MOMENT_MAIN=1` draws it
  too, for checks).
- **The moments' vertices.** The batch's runs of vertices (`momentSegments`, one per draw entry)
  are paired: field A's run of a draw followed by field B's run of a draw whose entry is the same
  but for the scissor's rows (`prepareMoments`). A pair with as many vertices is mixed vertex by
  vertex on the GPU at each moment (`kMomentLerp`: (1 - t) * B + t * A for the window position,
  depth, colour, texture coordinates and fog, B moved into field A's rows by the difference of
  their XYOFFSETs); a pair that differs (a clipped batch) takes the nearer field's run; a run of
  field A alone goes to the moments in the frame's second half, field B's alone to the first
  (with an entry of its own for field A's rows); a draw over both fields (the clears, the fog,
  the tint) to every moment. Meshes made on the GPU (`0x1410`) are mixed the same way from the
  two fields' vertices `kMeshExpand` made, so what the game draws over the same geometry again
  (the shadow receivers, testing depth for equality) stays on it. With per-primitive culling off
  for these batches, 99% of the runs pair up and mix (near the drop ship: 1.07 million pairs, 7,000
  that differ, 3,400 of A alone and of B alone, 360 over both, every 2 s). A vertex moves along the
  straight line between its two places on the screen - under a pixel from the true path at 4K in
  a fast turn (the chord against the arc, and the perspective: a few percent of a step).
- **Fields that draw different pieces** (2026-10-08, after the user saw the player's shadow
  flicker): a run that cannot be paired falls back to the nearer field, and where the geometry
  under a decal is mixed and the decal is not (or the other way round) the two part at moments in
  between - the shadow receivers, which test depth for equality against the ground, vanished at
  t = 0.5 and 0.75 under a walking player. Every place a field's pieces could differ from the
  other's is now made to agree in moments mode:
  - *The clippers* (`0x1410`'s `0x0480` and image 2011's projected draw) clip at the screen's
    edges, each field at its own: with moment pictures a triangle is clipped once, in its own
    barycentric coordinates, against both fields' planes (`ClipPath::planJoint`), the x and y
    planes taken six times as far out (`kJointReach`: cutting one field at the other's screen
    edge took away a strip of what it shows in a turn), and each field's pass draws that polygon
    through its own matrices (`drawJointPolygon`); a triangle across a screen edge goes whole into
    the strip and the GPU's scissor cuts it. `TF_MOMENTS_JOINT=0` turns it off.
  - *The GS front end* left out a primitive wholly outside its field's scissor, and the renderer a
    sprite of no size: not for the buffer of both fields while it is drawn for moments.
  - *Runs whose counts still differ* (the grass, whose program leaves out a blade past the screen's
    edge in one field and not the other; the instances, culled by a guard band close to the screen,
    per field) are matched primitive by primitive (`alignRuns`: in order, by colour, texture
    coordinates and nearness), the matched ones mixed, what one field alone has drawn from that
    field - off the screen, or left out of field B by the game too.
  - *A clipped batch's pieces* come field by field (its polygons, then its strip): a run of field
    A's pieces is paired with the run of field B's after it.

  Near the drop ship every 2 s: 910,000 pairs mixed, 7,800 matched primitive by primitive, 580
  runs of grass left on the nearer field (from 7,000 pairs that differed and 2,200 runs alone);
  the shadow shows at every moment.
- **The fog and multisampling** (2026-10-08): each moment's fog index is made from its own depth a
  GL pixel at a time (it was a GS pixel's middle: a rough light line round rocks before the
  fogged distance, which the user saw), and with `PS2X_GPU_MSAA` the layers are multisampled
  (stage 3 above).
- **Each moment drawn** into its layer as the native path draws (`drawMoments`): the batch's draw
  entries, the layer's depth, Ad from a copy of the layer's colour, runs of one depth test and
  write. Transformers' depth fog reads the depth buffer's top bytes as T8H after a Z16 shuffle of
  local memory; each moment reads its own depth there instead (`kMomentFog`: bits 8-15 of the
  moment's Z24, a GS pixel at a time as the depth comes back from local memory) - the source
  slot whose buffer is the depth buffer's block at its own width (the shadow texture kept in the
  same top bytes, 256 wide, is not it).
- **The pictures.** The display buffer's field pictures grow by one for each moment
  (`Target::shadow[2 + m]`): the copy of field A into it (SCANMSK 3) makes each moment's picture
  from the moment's layer, what goes over both field pictures after (the glow, the HUD) goes over
  each, field B's copy draws nothing, and what reads the buffer (the glow's source) reads the last
  moment's picture (`readPicture`). At the frame's flip all of them are composed (smoothed with
  `PS2X_GPU_AA`) and published, each with the refresh it is for (`Published::showsNs`); the VBlank
  after publishes nothing.
- **The window** shows a picture each refresh instead of each VBlank while the game draws moments
  (`ps2_host_display::waitForRefresh`: the refresh's picture handed over 3 ms before it,
  `PS2X_MOMENT_LEAD`, or later while there is still time; `ps2_gpu::pictureFor`: the newest picture
  made for that refresh or one before it). The FPS counter counts the new pictures the window
  shows. The game's VBlanks keep their own schedule.
- **A cap**, as the in-between has one: the GPU behind twice within about 20 frames - the GS
  thread waiting more than 4 ms for it before a frame's pictures - and the frames after draw a
  moment less (down to one, the last moments kept); a second without a wait, one more again. A
  refresh without a moment of its own shows the one before. The game never slows down for them.

Checks:

- **t = 1 is field A** (`PS2X_MOMENT_FIXED=1`, flat out, against the in-between's field A
  picture): 97.1% of the pixels identical, and 99.98% with the moment's depth made a GS pixel at a
  time at the fog (`PS2X_MOMENT_BLOCKY=1`) - before the joint clipping; with it 97.9%, a mean
  difference of 0.018 (triangles across a screen edge drawn whole rather than clipped there: their
  colour and texture interpolated a little differently near the edges). The 3% are the scene's own depth buffer: the fog's
  shuffle sends it through local memory, a value a GS pixel, and the fog's coverage and the
  foliage drawn after it are tested against that blocky depth (3x6 GL pixels a GS pixel at 7x5
  with full fields) - 99.2% of the pixels that differ are where a pixel's depth differs from its
  GS pixel's sample. The moments keep their depth whole, so their fog and foliage edges are the
  sharper ones (perhaps part of the "pixelated rock edges" the user saw). The rest: the field
  copy's last row, which on the console reads half a row of field B.
- **Even.** Moments fixed at quarters, flat out, a steady turn: every step 10.4-10.9 px on the
  left of the screen and 11.9-12.6 on the right, inside a frame and from one frame to the next.
  In real time on the 170 Hz screen at 5x3 (`PS2X_PRESENT_STATS=1`, the `[present] moments:` line):
  340 refreshes every 2 s, 324-340 of them a new picture, the picture shown the one made for that
  refresh within 0.02-0.04 ms; made about 11 ms before its refresh. The FPS counter reads
  161-170. At 7x5 - 4K's pixels - near the Mini-Con on the same screen, with the native shader
  made lean (2026-10-08, later): after the first 4 s, 337-340 of every 340 refreshes a new
  picture, the one made for that refresh (within 0.01 ms on average), no moment ever taken off by
  the cap; the GPU 12.2-13.2 ms a frame for 2.83 moments (RuneLite and a wallpaper program on the
  GPU too).
- **Only the drawing changed**: the player's path as above; the default path's 22 frames
  identical at scale 1 (both renderers) and 3 (the GPU alone).

Cost (flat out, 16:9, native path, full fields; the GPU's time a frame from its timer queries,
`GPU ms a frame`): near the Mini-Con at 7x5 the in-between's two pictures 13.6 ms, two moments
12.4, three 17.0, four 22.7; near the drop ship 11.9 / 11.2 / 15.2 / 20.4. At 5x3 near the Mini-Con
6.2 / 5.7 / 8.3 / 10.8. About 4-4.6 ms a moment at 7x5, most of it the scene's pixels - the grass
(1 ms a moment), the foliage and the world - and 0.37 ms its picture (the copy, the glow, the HUD,
drawn natively since: `PS2X_GPU_NATIVE_FIELDS=0` draws the field pictures the emulated way). With
the native shader made lean (stage 3 above, 2026-10-08): near the Mini-Con 10.3 / 9.8 / 14.0 /
18.5 ms, near the drop ship 8.9 / 8.7 / 12.6 / 16.1 - each moment more 3.7-4.3 ms at 7x5. So a 144 Hz
screen at 4K (2.4 moments a frame) needs about 11.5 ms of the 16.7 near the Mini-Con (13-14 before);
the cap takes moments off where the GPU has less (other programs on it, the GPU clocking down); at
1440p it has room to spare.

The cap (2026-10-08, after the user's first play at 4K: on the way to the Mini-Con the game fell to
30-50 frames a second). The scene there was heavier than the benchmarks' - 250,000 primitives a
frame, 7.8 ms of the GPU a moment - so 2.9 moments a frame wanted 22 ms of each 16.7, and the cap
never saw it: it struck when this thread waited for the GPU two presentations back, and the
driver held the thread back first (inside GL calls), the moments handed over late - 119 of 195
after their refresh - while every one was still drawn. Now a moment handed over after its
refresh strikes as well (`ps2_host_display::momentsMissed`), nothing strikes in the first second
of moments (a level starting) or the ten frames after one is taken off (the backlog going down),
and one taken off again within two seconds of being put back doubles the wait before the next try
(one to four seconds). At 10x7 - far too much for three moments - the game keeps 60 frames a
second with one moment a frame (two tried now and then); at 7x5 near the Mini-Con it keeps all
170 a second.

The cap, the game's frame-rate logic and the texture pack (2026-10-08, the user's second play: 4K
through Sunshine's virtual screen at 144 Hz, full fields, 2x multisampling, the texture pack,
RuneLite beside it on the GPU - no 120 near the Mini-Con, "lagging"). Three things together:
- The game's `adaptFrameRate` took the in-between pictures off - and with them the moments - when a
  level's frames came late for half a second, and for the rest of the level once a try again failed
  twice within 5 s; and a try again always failed, judged after half a second while the cap's first
  second of moments is not judged. Now with moments it lets them go only once the cap
  (`ps2_host_display::momentCap`) is down to one moment a frame and the frames are still late over
  2 s, judges a try again after 2 s, and never keeps them off for the level.
- The cap went up only after a run of frames without a strike, and a strike comes now and then at
  any cap - a moment a few ms late, the GS thread waiting for the GPU while it is merely ahead - so
  once a freeze or a level's first frames had taken it down to one it stayed there: 60 pictures a
  second where the GPU had room for 170. Now the calm starts again only when a moment is taken off,
  none is counted while there are no moments (they come back at the cap they went at), and the
  first two seconds of moments are not judged (a level's textures coming in). And behind is judged
  as shares over about 20 frames - three frames in ten waited for more than 4 ms, or a fifth of the
  moments handed over late - not strike by strike: a hitch (a few frames late together) or a short
  wait now and then (RuneLite on the GPU) is not helped by a moment less, and counted one by one
  they took the cap 170, 120, 60 a second by turns where the user walked into the Mini-Con's
  clearing (4 of 340 moments late, waits of 3-5 ms in a fifth of the frames, the GPU at 12 ms of
  16.7). Refreshes the window does not get to draw count as late moments too
  (`ps2_host_display::noteMomentShown`): with the shares alone the cap stayed at four where the GPU,
  RuneLite on it, was truly short - the moments made in time, but the window's drawing queued
  behind them, 185 of 340 refreshes drawn, 85-93 new pictures a second and uneven. Now, RuneLite on
  a fifth of the GPU: 114-142 pictures a second, up to 155-168 where there is room, never 60, the
  game at 59-60 frames a second.
- The texture pack's reader thread held one lock round every file it read and took it straight
  back, so the GS thread, needing a texture the reader had not got to, waited behind file after
  file: freezes of 0.1-0.7 s after a state loaded or where textures were new (`PS2X_GPU_STALL_LOG`
  found them: `primitives took 646 ms`, none without the pack). Now a texture is read by whoever
  comes to it first and nobody waits for another file than its own (and a file is read in one go).
Real time on the way from the drop ship to the Mini-Con at 7x5 with the user's settings, on the
170 Hz screen: before, the in-between pictures taken off in two runs of four (in the user's own play for the rest of the level), freezes of 0.6-1.1 s, the game at 45 frames a second for 2 s; after, with RuneLite on 8-16% of the GPU, the in-between pictures never taken off, no freeze longer than 83 ms, 120-170 pictures a second - down to 60 for a few seconds where the GPU is short, and back up - and the game at 54-60 frames a second.

Latency: a moment is shown one frame and 5 ms after the frame's flip at t = 0, two VBlanks later
at t = 1 - about 8 ms more than the in-between's field B on average, the price of showing the
frame before's pose first (`PS2X_MOMENT_OFFSET=<ms>` moves it).

Finished (2026-10-08, night, the user's "continue with phase 4 until it's fully done"):

- **A setting.** The settings' Frame rate offers the screen's refresh beside 120 and 60 ("The
  screen's refresh (144 a second)": `TF_FPS_LOCK=refresh`), switched while playing at the next
  frame as 120 and 60 are - the governor's in-between factor, the clippers and the renderer's
  moments follow `tf::port::momentPictures()`, which is the setting now rather than `TF_MOMENTS`
  read once. F4 goes from it to 120. Switched every few seconds while playing - 120, the refresh,
  60, the refresh, 120 - the pictures a second were 120, 163-170, 60, 170, 120 on the 170 Hz screen,
  the game at 60 throughout.
- **Perspective in the mix.** A draw with an STQ texture has Q = 1/w at each vertex; its moment is
  mixed by `t * Q_B / ((1 - t) * Q_A + t * Q_B)` instead of `t` for the place on the screen, the
  depth and the texture's S, T, Q - the projection of the vertex's straight path in the scene -
  and by `t` for its colours and fog (`kMomentLerp`; the fields themselves at 0 and 1, so t = 1
  is field A as before). In a steady turn at quarters the steps on the right of the screen, where
  the depth changes most, went from 11.92-12.62 px to 11.97-12.21, on the left from 10.48-10.92 to
  10.54-10.89; what is left there is the chord against the turn's arc (3% of a step). Off by default
  since the user saw parts of the player's model jump (2026-10-08, the same night): only STQ draws
  have the Q, and a piece beside one without it - the chest and the shoulder pad - is mixed along the
  screen's straight line, so at the moments halfway the two part along their shared edges (a dark
  wedge at t = 0.5, none at 0.25 or 0.75; none at all with every piece on the straight line).
  `PS2X_MOMENT_PERSPECTIVE=1` turns it on.
- **The water's clipping batches** (2026-10-08, the same night): the river right below the camera
  clips across the screen's bottom edge, and those batches ran the microcode's clipper, which cuts
  each field at its own edges - pieces the renderer cannot pair (the nearer field's, one field's
  alone), so holes opened in the river at the moments between while the view turned. With moment
  pictures such a batch is drawn whole by the native water routine when all of it lies in front of
  the eye and inside the GS's coordinates in both fields (`waterWhole`; `TF_MOMENTS_WATER=0` off).
  A batch that does not fit whole - the river right below the camera as it rises in a jump, part
  of it behind the eye (the user, 2026-10-08, at the bridge: "look around with the camera and jump
  you get the glitching in the water") - is drawn by the native routine with its triangles
  clipped alike in both fields (`drawWaterJoint`): a triangle not inside both fields' planes is
  left out of the four strips and, unless all of it is outside one plane in a field, clipped once
  in its own barycentric coordinates against both fields' clipping planes (field A's matrix in
  `0x0480`'s data block, field B's in `0x2cc0`'s q4-q7, where the microcode's clipping path finds
  them; x and y `kJointReach` times as far out) and drawn as a fan in each of the four copies under
  the clipper's own polygon tags (`q1022`; `q1020` for context 2, which the microcode's clipping
  path swaps in and back). The polygon clipping is `ClipPath::clipBoth`, shared with `planJoint`.
  From the user's `jump` save, turning and jumping: the water's runs that did not pair went from
  56 (field A alone, B alone, the nearer field's) to none.
- **What the game does not carry.** Field B is the frame before's pose for what the game moves
  with its interpolators; the rest it draws where it is now, seen from the frame before's view.
  Measured run by run (`PS2X_MOMENT_CONTINUITY`: each packet run's field B against the field A the
  frame before had, found by its kind and its first vertex): 99% of runs in the Mini-Con fight
  are where the frame before left them; what is not is mostly the particles and the trails of
  shots and missiles (small additive quads and strips, no Z written: 20-30 a frame, most moving
  less than a pixel a frame, some 100), and blades of grass and leaves that come and go with the
  view or change their detail. Those moved 60 times a second and jumped at every frame's start.
  Now such a run starts from where the frame before left it (`carryFrom`): a kind found moved
  within 32 GS pixels is carried, its runs' field A vertices kept for the next frame, and a run of
  it takes the nearest one's - by all its vertices, when no other is near. Kinds of more than 128
  runs a frame (the grass: its blades sway less than a pixel a frame, come and go, and look alike)
  are left alone. The moments at t = 0 and 1 compared across frames (each frame's t = 0 against the
  frame before's t = 1, where only what is not carried differs): in the fight 0.98% of the pixels
  jumped, now 0.05% - the shots' trails gone from the difference; in a turn the same with it as
  without (0.258% and 0.257%: grass and leaves coming into view). Kept in arrays reused frame to
  frame, it costs nothing measurable flat out (72.5 frames a second at 7x5 with three moments
  against 72.7); the first version, which kept every batch's vertices, cost 10%.
- **The HUD** is drawn once a frame over each moment's picture: it changes with the game's frames.
  Its pieces stay where they are; what follows the world - the lock-on reticle, the shots - is
  drawn in the scene and moves at the moments.
- **Without a texture pack** a moment cost about three times as much (one moment, 11 ms of the GPU
  at 7x5, where two to three fit with the pack): the native shader read each texel of a texture
  from local memory - its swizzle, its CLUT, four of them a pixel for the GS's bilinear - and every
  page the game wrote ended a batch (13,000 batches every 2 s against 3,400). The texture cache
  (`PS2X_GPU_TEXTURE_CACHE`, the settings' Graphics tab, on by default since 2026-10-08) decodes such a texture
  once into an RGBA8 texture - its texels as the GS reads them - found again by PCSX2's hash of its
  memory and the colours it takes from the CLUT (the game streams its textures through local
  memory every frame: 128 textures on the way to the Mini-Con, decoded in its first seconds), and
  the native path samples it as a pack's replacement, with the GS's bilinear or nearest (not the
  pack's anisotropy, which changed which of the leaves' pixels passed their alpha test). Without a
  pack, flat out at 7x5: the in-between's two pictures 22.2 to 10.4 ms of the GPU a frame (35 to 80
  frames a second); moments in real time on the way to the Mini-Con 25-61 pictures a second at one
  moment a frame to 159-170 at 2.83, the game at 60. Against reading local memory at scale 3
  (`check_native.ps1`'s 22 frames): 0.11 a channel, SSIM 0.9992, p99 2.

(The scene's cost at 4K, above: with a pack the textures are never decoded per fragment - its
replacements are sampled by the texture unit, and memory textures are 2% of what is drawn - and the
depth test cannot go before the shader where Z is written.)

Stage 4, as of 2026-10-08, night: done - moment pictures behind the Frame rate setting
(`TF_FPS_LOCK=refresh`), the texture cache behind its own; the player's path the same with
moments as without, the default pictures the golden sets. The user played them at 144 (FreeSync
off) with 4x multisampling and said to make them the default: the screen's refresh, the texture
cache and 4x multisampling are the defaults since (test runs keep 120 without the two).
