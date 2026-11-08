# Frame rate: what depends on it

*2026-10-04. A survey of the executable made by an analysis agent from the disassembly and the
generated code (read-only), for running levels at rates other than 30 and 60 - 120 and up.
Not every finding has been checked by a run yet: what has is marked in docs/NOTES.md ("Frame
rates", "the governor"). The decompiled frame timing is in
[transformers/decomp/timing](../transformers/decomp/timing/timing.h). Since the same day the
rate is a setting (`TF_FRAME_RATE`): P0 and P1 of section 7 are done, see "Done" there.*

Static survey of SLUS_206.68 (read-only: disassembly, the generated C++ as ground truth, the
call graph and vtables). "Census" means the `TF_CALLERS=32e910` log in `runs/census30.err`
(Drop Zones state at 30, then Amazon Basin loading and running; counts per 60 frames). Class
names come from the vtables that hold a function (or the nearest caller that is in one).


## 0. How time reaches the simulation (the short version)

- **The level is fixed-step.** `taGame`'s begin (`0x294950`) stores `FramePeriod()` at
  `taGame+0x5C` and sets the clock's step from it. Every frame `taGame`'s update (`0x292DE0`)
  passes `+0x5C` times a time scale to the subsystems as `f12`: `f22` = period x `+0x64` (most
  systems), `f21` = period x `+0x60` (the cars `0x3231D0`, `0x307B90`), `f20` = period x `+0x6C`
  (the sound listener `0x2804C0`), and `+0x5C` unscaled to the objects at `+0xA8`/`+0xAC`
  (`0x35B8B0`/`0x35B400`) and `0x5309E0`. The scales are products of a 4x3 table at `+0x70`
  (the melee state writes 0.3 there: slow motion; `+0x90..+0x98` are 1.15 or 1). 18 more
  functions read `+0x5C` x a scale themselves through the `taGame` pointer `gp-0x3E38`
  (`0x6CF2B8`): `taCreatureAI+0xB0` (`0x2AD200`), `taTransformerCreature+0x3C` (`0x349910`),
  the ragdoll state (`0x477EA0`), `taSnowEffect`, `taSnowClusterMotion`, `taMeleeComboCommand`,
  `taPowerlinkSequenceScript`, the sidekick's powerlink states, `taCybertronTowerManager`,
  `taGuerrillaMeleeUp`, `taLauncherImpactDarkEnergon`, `0x374330`/`0x375CC0` (many reads). (The
  camera scripts read only the scale `+0x64`, to compare it with 0.5.)
- **Nothing in the simulation reads a measured frame time.** The frame start (`0x25BE20`)
  computes VBlanks taken x VBlank length into time slot 7 (`0x6F851C`), and the EE clock's
  seconds since the last frame into `0x6CF054`; neither is read (section 4).
- So the simulation's rate is the rate `FramePeriod()` gave when the level began, and the
  real frame rate must match it: that is why a level keeps one rate from begin to end.
- Most game code is written in seconds (timers decremented by `dt`, the clock in seconds,
  speeds x `dt`). What depends on the rate is (a) code that does a fixed amount **per call**
  (blends by a share, damping by a factor, increments by a constant, counters), (b) values
  captured once at the begin or at an event, (c) the pacing by VBlanks, and (d) code tuned at
  1/30 s although it takes `dt` (the cars).

## 1. `FramePeriod` (`0x32E910`): 29 call sites in 22 functions

Census in a level: `0x284684` 1/frame, `0x315F7C` 1, `0x315F98` 1, `0x35C8E4` 3, `0x3F87C4`
2; at level load `0x294998`, `0x2C6238`, `0x2ECA24` once, `0x513E54` once, `0x516A74` 3 times;
later `0x517874` once; in the menus `0x284684` and `0x333650` 1/frame. The others were not
reached in that run (events, other levels).

| Call (return) | Function [class / reached from] | What it does with the period | When | Scales with the period? |
|---|---|---|---|---|
| `0x28467C` (`0x284684`) | `0x284670`, `j` from `0x2E6410` [`shSystemsPS2+0x8`] | `0x279DA0(systems+8, period)`: the input manager's update. Five rumble sources' timers `-= period`. Runs once per VBlank count (`GetTimeSlot(6)` vs `+0x540`). Rumble "baseline": a 300-frame history of the summed strength, the minimum of the last 90 frames, smoothed by 0.1 a frame (`gp-0x7DF0`); motor = 500 x min(1, excess) | every frame, menus and levels | timers yes; the 300/90-frame windows and the 0.1 a frame no |
| `0x294990` (`0x294998`) | `0x294950` [`taGame+0xC`, begin] | `taGame+0x5C` = period; clock step (`0x41E820`) | once, level begin | defines the level's `dt` |
| `0x2C6230` (`0x2C6238`) | `0x2C61A0` <- `0x2936F0` <- begin | physics gravity as a velocity change per step = (0, 0, 29.4 x period) -> `0x6EC690` (`0x1B5570`); read only by `0x1C06A0` (through `0x1B5560`), which takes it, scaled, off a body's velocity | once, level load | yes (captured) |
| `0x2ECA1C` (`0x2ECA24`) | `0x2ECA10` <- `0x292060` <- `0x2936F0` | `0x22D5A0`: particle-emitter period `gp-0x5958` (`0x6CD798`); also rewritten every frame by `0x230100` with `taGame`'s `f22`; read by `0x22C0F0`, `0x22C460`, `0x22D5C0` [`vlxParticleEmitterBase/Ballistic+0x2C`] | once (then every frame via `0x230100`) | yes |
| `0x315F74`, `0x315F90` (`0x315F7C`, `0x315F98`) | `0x315F30` <- `taGame` update (`a0` = the object at `taGame+0x14`) | `0x318100(sm, period)` for the two game-flow state machines (`0x316A60` -> `gp-0x3B38`, `0x3166A0` -> `gp-0x3B34`): time in state `+0x44 += period`, the state's `+0x48` update(period) | every frame | yes |
| `0x31F7D8` | `0x31F6D0` <- `0x319C70` (car reset: `taTransformerStateCar+0xC/+0x14`, `taTransformerCreature+0x3C/+0xB0`, `taStatementCommand/Keyframe`, melee grab) | car physics `+0x34` = period, `+0x38` = 1/period (the constructor `0x3200E0` sets 1/60 and 60). The wheel/suspension/tyre code under `0x31EF90` uses `+0x34`, not the frame time given to `0x3231D0`: `0x323AA0` -> `0x3236E0` -> `0x1C5630` (tyre friction impulse over `+0x34`), `0x323FF0` (wheel spin), `0x31EF50`, `0x31CD30`, ... | at each reset (transforming into the car) | captured at the reset; at true 60 it is 1/59.94 while the 60 fix steps the car by 1/30 (open question 1) |
| `0x333648` (`0x333650`) | `0x333630` [`taUIPreGame+0x18`] | the menus' `dt` for about 8 subsystem calls | every frame, menus | yes |
| `0x356B1C` | `0x356170` [`taIncidentalObjectPillar/Building+0x38`; `0x5927F0` Bunker] | crumble timer `+0x188 -= period`, below 0 -> state 4 | every frame while crumbling | yes |
| `0x35C8DC` (`0x35C8E4`) | `0x35C8D0` [`+0xC` of `taFootstepAttachment`, `taSnowTreads(2)Attachment`, `taSnowFootstepAttachment`, `taOrientedDecalSet`, `ta{Metal,Rock,Dirt,Surface}ImpactAttachment`] | age `+0x28 += period` | every frame per attachment | yes |
| `0x3CBFA4` | `0x3CBF90` [`taWeaponDamageAttribute_DirectContinuous+0xC`] | damage this frame = rate `+4` x period (x `+8` when `f12` != 0) | every frame while it hits | yes |
| `0x3F87BC` (`0x3F87C4`) | `0x3F8790` <- `0x369D60` [`taEffectSceneNode+0x14`] | `gp-0x431C` (`0x6CEDD4`) = period, read by the particle contexts (`vlxParticleContext_MotionCompensatedTransform+0x10/+0x24`, `_PhysicsBuffer(RandomTime)+0x10/+0x14`, `_RibbonTrail+0x24`, `0x24E960`, `0x24FC50`) | twice a frame | yes |
| `0x4605D8` | `0x45FF50` [`taDecepticloneTurn+0x28`] | turn finished when time-to-target < 2 x period | every frame while turning | "two frames" (shorter at higher rates; harmless) |
| `0x4CE300` | `0x4CE2D0` [`taUIIntro+0x18`] | `+0x28 += period/10`, wrapped to [0,1) (10 s cycle) | every frame, intro | yes |
| `0x50B4E8`, `0x50B844` | `0x50B400` [script command table `0x61ED00`], `0x50B830` <- `0x513FD0` <- `taScreenInGPause+0x50` | cinema skip/end: timer `+0` = period, state `+8` = 3 | on the event | "one frame" |
| `0x513E4C` (`0x513E54`) | `0x513DD0` [table `0x61EDF0`] | `0x5142B0`: request a cinema that starts after 3 x period (cutscene manager `+0x78`) | per request (once at load) | "three frames" |
| `0x516A6C` (`0x516A74`), `0x51786C` (`0x517874`) | `0x5168A0` [table `0x61EE00`], `0x517860` <- `0x50CDC0` | cinema player `0x517890(obj, period)`: time `+8 += period`, runs keyframes | at a cinema's start/skip | yes |
| `0x51A758` | `0x51A740` [`taIncidentalUpdateLogic_BuildingRubble+0x14`] | `0x40FB50(+0x4C, period)` when the detail scale `0x6CD974` > 0.8 | every frame while rubble moves | yes |
| `0x533098`, `0x5331F0`, `0x533368`, `0x5333F4`, `0x533530`, `0x53362C`, `0x533694` | `0x533010` [`taCreatureEffectSwitcher+0xC`] | effect timers `-= period`; but the texture scroll (`+0xC/+0x10` `+=` `+4/+8`, wrapped) and an LCG flicker step **per frame** | every frame | timers yes; UV scroll and flicker no |
| `0x560580` | `0x560260` [`taLauncherProjectile_DropMine+0x4C`] | displacement = velocity x period | every frame of flight | yes |
| `0x5CDD44` | `0x5CD6F0` <- `taAnimationControllerDismember+0x28`, `taArtilleryManager` (`0x4B6060`), `taLauncherImpactDropMine/LimpetMine+0xC` | `v_z += -19.6 x period`; position `+= v x period` (debris, shells, mines) | every frame per piece | yes (velocity first, then position, as in the player's jump: arcs differ by g dt t/2 between rates) |

## 2. `TargetFrameRate` (`0x32E8B0`, 10 sites) and `TvFieldRate` (`0x32E840`, 1 site)

| Call | Function [class] | What it does | When | Notes |
|---|---|---|---|---|
| `0x2842B8` | `0x2842B0` [`shSystems/shSystemsPS2+0x14`] -> `0x281290` -> `0x1EABF0` | `gp-0x5BF8` = rate, only when the systems singleton (`gp-0x3E48`) is made, at boot (half rate not set yet: 59.94). Read by `0x2801D0` (3D sound listener and emitters, from `0x2804C0` every frame): velocity = position change a frame x rate x 0.5 (for the 3D sound, presumably Doppler) | once, boot | assumes frames of two fields: right at 30, half the speed at 60, a quarter at 120 |
| `0x284534` | `0x284490` <- `0x2E6310` (`shSystemsPS2`) | governor target slot 0 (`0x202FF0`) | once | |
| `0x29532C`, `0x295348`; `TvFieldRate` `0x295324` | `0x295310` (the main loop, `taGame+0x14` and the other modes') | `f20` = TvFieldRate / TargetFrameRate = VBlanks a frame (2 at 30, 1 at 60), worked out once when the loop starts and waited for only without betwixtMode (`overrides.cpp` forces 1 at 60); governor target slot 0 | each mode's start | TvFieldRate 119.88 / target 119.88 gives 1; TvFieldRate left at 59.94 with a 119.88 target gives 0.5, truncated: no wait |
| `0x319AAC` | `0x3199E0` <- `0x2936F0` <- `taGame` begin | `gp-0x3B00` = rate; the in-level cinema movie (`0x5142B0` -> `0x319520` -> `0x274D20`) is made with it (`fmvStreamPS2+0xD0`). `0x275C8C`: at exactly 50 or 60 the movie alternates fields, otherwise (59.94) frames of two fields (`0x6CF010` = 1) | once, level begin | movies set the frame-rate flag themselves |
| `0x4D04A4`, `0x4D04F4`, `0x4D0C18`, `0x4D0E04`, `0x4D0F00` | `0x4D0480`, `0x4D04D0`, `0x4D0BD0` (TV mode option: `0x32E890` PAL60 byte, display mode `0x32E770`/`0x276E70`) | governor target slot 0 after switching | on the option | menu only |

## 3. The level's clock (`0x41E8F0` make, `0x41E820` set step, `0x41E840` advance; pointer `gp-0x384C` = `0x6CF8A4`, also `taGame+0xB8`)

| Function | Reads | When | Use | Scales? |
|---|---|---|---|---|
| `0x41E8F0` <- `0x294988` (`taGame` begin) | makes it (time 0, scale 1) | once | | |
| `0x41E820` <- `0x2949A0` | step = `taGame+0x5C` | once | | |
| `0x41E840` <- `0x29300C` (`taGame` update, midway: before the cars and the physics step) | time = time-at-step + frames-since-step x step x scale | every frame | | |
| `0x41E8A0` <- `0x2948C0` (`taGame` end) | clears the pointer | once | | |
| `0x41E880(clock, n)`: time + n x step x scale | `+4`, `+8`, `+0xC` | 31 sites, **all with n = 0** ("now") | absolute time in seconds | yes |
| `0x41E6E0` (time since a stamp), `0x41E720`/`0x41E750`/`0x41E780` (set a stamp) | `+4`, `+8`, `+0xC` via `gp-0x384C` | per use; all pass 0 frames ahead except `0x3BD458` (now - 3 s) | timers in seconds | yes |

Nothing else reads the clock's fields (the `gp-0x384C` loads are only in these helpers). Users
of "now": AI (`taCreatureAI+0xF8/+0xFC`, `taCreatureActivationBasic`, `taSquadMemberSendMessage`,
`taAIGoal_Melee`, `taAIGoal_AttackOpponent2`, `taAIGoal_Kamikaze2`, `taCreatureHeavyGrunt`),
weapons (`taWeaponDirectCollisionHandler`, nine `taMiniconAbility*` cooldowns), animated
materials (`taMaterialAssignment_OceanWater`, `_FlowingWater`, `_WarpGate`, `_HeatHaze`),
effects (`taMist`, `taRain`, `taBirdiesFlock`, `taDisplayList_CreatureEffects`), the sidekick
(`Idle`, `FollowPathfinding`). All rate-independent as long as the step equals the real
period, which holds while a level keeps the rate it began at.

## 4. The frame start (`0x25BE20`): the measured step and the VBlank count

| What | Where it goes | Who reads it |
|---|---|---|
| EE seconds at the frame start (`ClockSeconds`) | `gp-0x4094` (`0x6CF05C`) | `0x25BE20` itself (stats record -> `0x262260`, the timing graph) |
| VBlank count after `WaitVBlank` | `gp-0x4214` (`0x6CEEDC`) | nobody |
| time spent waiting | `gp-0x40A0` (`0x6CF050`) | `0x25BE20`'s stats record only |
| EE seconds since the last frame | `gp-0x409C` (`0x6CF054`) | nobody |
| VBlanks since the last frame (ring of 20 at `0x70F4B0`) x VBlank length (`0x6CEF68`) | **time slot 7** (`0x6F851C`) via `0x203010(7)` | nobody: `GetTimeSlot` (`0x203030`) has 11 callers, all asking for slot 6 |
| previous frame's VBlank | `lastFlipVBlank` `gp-0x40D8` | `WaitVBlank` |

Slot 6 is the VBlank count as a float. Its 11 readers are **once-per-VBlank guards**:
`0x1971E0` (input library: "same VBlank" flag `gp-0x4924`, read by `0x198780`), `0x279DA0`
(input manager, above), `0x319170` (`taGame` update -> `0x274A80`: the list of movie streams,
whose update `fmvStreamPS2+0x10` = `0x275A00` runs once per VBlank; `0x274A80` is also called
from `0x333630` menus and `0x4CE2D0` intro), `0x4CE2D0`,
`0x537270`/`0x537D80` (a random number held for one VBlank), `0x275A00` (movie player). If a
frame ever took less than a VBlank, these would skip their work on the second frame.

## 5. The in-between factor (`0x6CD6F0`), the frame-rate flag (`0x6CF010`) and the governor's other outputs

| Variable | Written by | Read by | Use |
|---|---|---|---|
| `0x6CD6F0` in-between factor | governor `0x25BDA0`/`0x25BDA8`/`0x25BDBC`: 0.5 with its request for 30 (or when `gp-0x4078` is set), else 1.0 | `0x20D3A0` (motion interpolators' transform blend: `vlSceneNodeMotionInterpolator`, `taAnimationNodeMotionInterpolator`, through `0x218E20`, which alternates `+0x38`/`+0x34` by a pass counter `+0x60`), `0x20DBC0` <- `0x20D150` (blends a float `+0x1A0` -> `+0x1A4` the same way) | drawing only; one slot per view field (`+0x20C` of the view: 0/1, stride `0x220`), so the scheme holds two pictures per frame |
| `0x6CF010` frames of two fields | governor `0x25BD68`/`0x25BD84` (= its request for 30); graphics set-up `0x25EEA0`; mode begins `0x2949E8` (`taGame`), `0x3348AC` (`0x334810`), `0x4CE9E8` (`0x4CE9B0`) set 1 with betwixtMode; movie player `0x275CC0`/`0x275D04` (1); saved/restored around movies (`0x275870`/`0x2757E0`, with `nextFrameField`) | `WaitVBlank` (`0x25DEA8`, `0x25DF24`) only | pacing |
| `0x6CD984` governor's request for 30 | governor | governor; `0x244E10` (vtable `0x6B3B80`, renderer: without the request, a minimum `gp-0x5908` for some amount) | render-only |
| `0x6CD974` detail scale | governor (lowers it 0.005 a frame at 60) | `0x369640`, `0x3CA310`, `0x412C10`/`0x412DB0`, `0x429510` (trees), `0x430190`, `0x457AF0`, `0x4B6060`, `0x51A740` (rubble), `0x58CDB0`, `0x58E010` | quality, not time; pinned to 1 by `overrides.cpp` |
| `0x6CD8DC` betwixtMode | settings | 30 reads (and 3 writes): the renderer set-up, `0x20CF2C` (views), `0x2953B4` (main loop wait), the mode begins, `0x2C5340`, `0x2F54C0`/`0x2F56C0` (writes too), `0x34C110`, `0x34C390`, `0x407CD0` (4 reads), `0x409B90`, `0x4BA7F0` (`taDisplayListScreenEffectBehaviour`) | not followed here |

## 6. Per-frame amounts outside the existing fixes

### 6a. The 0.2 (`gp-0x75A8`) and 0.3 (`gp-0x7AA8`) constants outside `0x460000-0x480000`

- **0.2**: 40 sites, 18 functions outside the range, none a per-frame blend: impact effect
  parameters (`0x1E1BB0` from `taLauncherImpact{DarkEnergon,DropMine,LimpetMine,Cannon,
  Scrambler}+0x28`, `taFlakBurstProjectile+0x28`), initial values (`0x2D67E0`, `0x321880`,
  `taWeaponLogic2_HomingMissile`), the artillery bounce (`0x4B5790`: velocity x 0.2, an event),
  offsets (`taTransformerActor+0x24`), `0x3229E0` (from the truck, `+8 = x + 0.2`).
- **0.3**: 115 sites, 54 functions outside the range. Per-frame blends: `0x2D0B50`
  `taThirdPersonCameraScriptThree+0xC` (`gp-0x3C6C` rises toward 0.7 by 0.3 a frame and falls by
  0.5 a second), `0x2CD670` `taFirstPersonCameraScript+0xC` (`+4`), `0x2CBBB0`
  `taWarpOutSequenceScript+0x10` (`gp-0x3C6C`). The rest: comparisons (sidekick states, AI goals,
  launcher weapon), blend times passed to the animation (`0x2D8CA0` from `taTransformerStateFlight`,
  `taTransformerStateTractorBeam`, `taSidekickStateFall`), values stored once (static
  initialisers `0x67C000-0x6A9300`, `taRocketLauncherWeapon`), the melee state's slow motion
  (`0x474F50` writes 0.3 into `taGame+0x70..`: a time scale, fine).

### 6b. Blends `x += (t - x) * c` with a constant `c`, found by a scan of all code

(Linear symbolic scan of every function; stack temporaries and 0.5 midpoints left out.)

| Function [class] | Field, share | Covered at 60? |
|---|---|---|
| `0x471620` `taTransformerStateFall(+Base)+0x8` | controller `+0x1C8` -> 0.75 by 0.1 (`gp-0x748C`); `+0x1C8` is read by the walk (`0x476180`, `0x476100`) and `0x47D9A0` | **no** |
| `0x47A440` `taTransformerStateTransformToVehicle+0x8`, `0x47B010` `...ToRobot+0x8` | `+0x1F4`, `+0x58` by 0.1 | **no** |
| `0x47EA10` `taTransformerStateGlideFall+0x8` | `+0x118` by 0.1 | **no** |
| `0x47F0D0` `taTransformerStatePoweredGlide+0x8` | `+0x100` by 0.1 (3 sites) | **no** |
| `0x47FF90` <- `taTransformerStateFlight` (`0x480830`) | `+0x34` by 0.1 | **no** |
| `0x469510` `taTransformerMotionShaderGlidePack+0x10` | `+0xB0..+0xB8` by 0.1 (`gp-0x7594`) | **no** |
| `0x46C6F0`, `0x46CA80` `taTransformerMotionShaderFootslip` | `+0x94`, `+0x88/+0x8C` by 0.1 | **no** |
| `0x2CA0B0` <- `0x2CA9D0` <- `taCamera+0x34` | camera distance `+0x31C` moves toward its target by 0.06 a frame (`gp-0x72D8`), never past a limit (`f2`, set at once when a flag is up) | no |
| `0x2D0B50`, `0x2CD670`, `0x2CBBB0` | camera scripts, 0.3 (6a) | no |
| `0x2CE0A0` `taGlideCameraScript+0xC` | `gp-0x3C6C` by 0.2 (`gp-0x7388`) | no |
| `0x2CD9D0` `taJetPackCameraScript+0xC` | `+0xE8` by 0.05 | no |
| `0x52A440` <- `taSidekickStateFall` | `gp-0x34C4` by 0.2 | no |
| `0x4C5830` <- `taGame` update | `ecoPromotionAlphaDistance` instanceNear0/1 (`0x61DFD0/4`) by 0.1: tree fade distances | no |
| `0x279DA0` input manager | rumble baseline by 0.1 | no |
| `0x247020` <- `0x20EC80` | `+0x1000..+0x100C` by 0.35 (renderer) | no |
| `0x31A660`, `0x323D60` (car, per step) | `+0x18C` by 0.05; wheel `+0xC0/+0xC4` by 1/6 | yes (the car steps at 30) |
| `0x59C840` `taScreenExtrasArtViewer` | menu | n/a |

`gp-0x748C` (0.1) has 63 loads in 46 functions and `gp-0x7594` (0.1) 23 in 16; only the ones
above came out as blends, the rest are other uses or blends the linear scan cannot see.

### 6c. Damping `x *= c` each call

`0x1B3AE0` <- physics step `0x1B5E50` (each body and step: an orientation `+0x90..` nudged by 0.1
of the body's and renormalised, and sums `+0x80..+0x88` that leak x 0.9091 (`gp-0x7614`) a step:
smoothing, probably for the rest test), `0x4E2120` `taSnowClusterMotion+0xC` (velocity
x 0.97), `0x3CF750` `taWeaponLogic2_Vortex+0x10` (`+0x68` x 0.9), `0x3D36A0`
`taWeaponLogic2_TwistRocket+0x18` (`+0x34` x 0.75), `0x39D520` `taAIGoal_PatrolShieldArm+0x54`
(`+0x40` x 0.75), `0x494F90`/`0x495390` (`+0xD8` x 0.9), `0x426280` <- `taActorDropShip`
(x 0.995), `0x4316C0` (x 0.95), `0x438EB0` (x 0.8), `0x3ECA40`, `0x2F0C00` (x 0.5), menu screens
`0x335E00` (x 0.95). The cars' body velocity and spin x car `+0x30` each step (`0x3231D0`) are
covered by stepping the car at 30.

### 6d. Increments by a constant each call

Sound: `0x37DEF0` (<- `0x37E060` <- listener update `0x2804C0`, `+0x10` +- 0.007),
`taAmbienceUnPhasePL2` (`0x37FEE0`, 0.0003), `taAreaRandomSequence` (`0x3804C0`, 0.0005),
`taAreaRandomSound` (`0x381050`, 0.0005), `taWindGenerator` (`0x381A80`, 0.005): fades and phases
a frame. Visual: `vlShader_Drift+0xC` (`0x26ABE0`, `gp-0x3FD4 += 0.07`), the creature effect UV
scroll (section 1), `0x483FC0` (`+0x164` +- 5 degrees a frame), `taRibbonBeamProjectile+0x10`
(`+0x40 += 0.45`). Gameplay: `taTransformerStateCar+0x8` (`0x46F0D0`, `+0x11C += 0.01`),
`taActorBossStarscream+0x14` (`+0x1EC/+0x1F0 += 0.1`), `taActorHeavyGrunt+0x14` (0.1),
`taSearchCommand_Pivot+0xC` (0.1), `taAutobotHealthComponent+0xC` (-0.1), `0x423BA0`,
`0x489E50`, `0x498280` (`+0x54 += 0.01`). The scan's other hits are angle wraps (2 pi), menu
layout and `gp-0x5958` (the emitter period, a variable).

### 6e. Hard-coded periods

- 1/30 at `gp-0x65BC`, `gp-0x795C`, `gp-0x7CF4` (never written): durations in seconds -
  animation blend times of one 30th (`0x4FCFA0` -> `0x2D5290` from `taActorBossMegatron`,
  `taAIGoal_SargeFleeExplosion`, squad goals, `0x478500` ragdoll, `0x411AA0` dismember), clamps
  "length - 1/30" (`0x2E5D30`, `0x2E5E20`, `0x2E5EE0`), and `0x4A3C60` converting data counted
  in 30ths to seconds at boot. Time-based: harmless.
- 1/60 (`gp-0x7F6C`): the `anAnimation*` curve sampling step (data), and the car's default
  period (`0x3200E0`). 2/30 (`gp-0x7700`): a ramp in the car code (`0x31F410`).

### 6f. Integer counters and frame counts

733 `lw / addiu +-1 / sw` updates of object or global fields in `0x280000-0x5E0000`; the ones
compared with a constant right after are event counters (attempts, cycles), except the HUD's
4-frame cycle (`0x2FC848` `taHUDComponent+0x20/+0x24`, `gp-0x3B64`). The level's frame counter
`taGame+0xA0` (+1 a frame at `0x292F64`) is exposed as a `vlShaderContextSource<int>`
(`taGame+0xA4`, vtable `0x6B4A80`): anything a shader animates by it counts frames (not
traced). The game keeps nearly all its timers as float seconds.

## 7. What has to change to run the simulation at rates other than 30/60 (e.g. 120)

In priority order. R is the level's rate.

**Done (2026-10-04).** P0 and P1: the rates scaled to R in `rates.cpp` (`59.94 x R / 60`; at 60
as before), VBlanks every 1/R s (`EeScheduler::setVBlankPeriod`, the console's while a movie
plays), the cycle scale 30/R, the governor's and main loop's rules for any R, the 3D sound's
frame rate (`0x6CD4F8` = twice the level's), the car reset's period kept at 30 (`FramePeriod`
asked from `0x31F7E0`), every fix of `overrides.cpp` written for R (shares `1 - (1 - s)^(30/R)`,
air steering `0.4 x 30/R`, the jump's `g dt (R/30 - 1)/2`, contacts by a credit of 30/R, cars
stepped every 30th of a second with the part of a frame for rates that are not multiples of
30, the VBlank counts scaled), and a save state's level converted to the rate set (period,
clock step, gravity a step, movie rate). Measured at 120 against 30: the walk and the truck the
same curves (shifted by the shorter input latency), the jump the same height. Above 60 the rate
adapts to the host (R, R/2, ...: `adaptFrameRate`). P2 is done too, but for the counters and
the data-driven amounts listed in docs/NOTES.md ("The rest of what counts frames"): 69
instruction patches (`config/overrides.toml`) found and checked site by site by an analysis
agent ([FRAME_RATE_SITES.md](FRAME_RATE_SITES.md): what each site does, the sites rejected -
the twist rocket's and the shield arm's factors run per event, three 0.01s are a search
loop's step - and what needs a hook).

**P0 - the rate and the pacing (nothing else works without these)**

1. **One rate per level, chosen at `taGame`'s begin**, as 60 is now: `TvFieldRate`,
   `TargetFrameRate` and `FramePeriod` (`rates.cpp`, decompiled) return the R values while that
   level runs (`port::levelAt60()` becomes a level rate). Everything captured at the begin then
   follows: `taGame+0x5C` (every subsystem's `dt`), the clock's step, the physics gravity per
   step (`0x6EC690`), the emitter period, the in-level movie rate (`gp-0x3B00`), cinema delays,
   and the cars' period at each reset. A state made at one rate still has to be played at it.
2. **VBlanks at R in a level.** A frame cannot be shorter than a VBlank: `WaitVBlank`
   (`0x25DE90`) waits for the next one and the main loop (`0x2953CC`) for `f20` of them.
   `EeScheduler`'s VBlank is a `constexpr` 16,667 us (`kVBlankPeriod`, `kVBlankPeriodCycles`) and
   `ps2_host_display` presents one picture per VBlank; both need a level-time rate. Keep on
   59.94 what counts VBlanks as real time: the MPEG picture clock (`MPEG.cpp`, "2 fields" a
   picture - in-level cinema movies exist, `0x5142B0` -> `0x319520`), the CD stream production
   (`CD.cpp`, per VSync tick), and check the IOP's own VBlank cadence (`iop_emulator_const.h`,
   59.94, separate). Main loop `f20` must be 1 (as `mainLoopSpin` does at 60). The alternative,
   60 Hz VBlanks with frames that do not wait, breaks the 11 once-per-VBlank guards (section 4:
   input read, rumble timers and the per-VBlank list would run every other frame) and the
   one-picture-per-VBlank presentation.
3. **The governor generalised**: target `0x6CE980` = R, no request for 30, the detail scale at
   1, the hold at 30 lifted, in-between factor 1.0 (`applyFrameRate`). The EE's cycle scale
   (0.5 at 60) has to shrink with R, and the host has to fit a frame in 1/R s: at 60 the
   heaviest stretch is about 13 ms of game code a frame, so true 120 is not reachable there on
   this machine.

**P1 - the existing 60 fixes written for any R** (all are "x2" today)

4. Shares (`0x6CD2A0..0x6CD2B0`, the model's turn share `+0x1F0` via `0x47D6C0`):
   `1 - (1 - s)^(30/R)` (sqrt is the R = 60 case). At 120: 0.1 -> 0.0260, 0.2 -> 0.0543,
   0.3 -> 0.0853, 0.03 -> 0.0076, 0.07 -> 0.0180. Air steering: 0.4 x 30/R.
5. Jump arc (`0x470F70` hook): add `g x (1/30 - dt)/2` at take-off; the current `g dt/2` equals
   it only at 60 (at 120 it must be 3x larger).
6. Sliding contacts (`0x47C720`/`0x47C500`): "every other contact keeps the speed" is the R = 60
   case of "cut the speed once per 1/30 s"; it was found by measurement and has to be measured
   again at R.
7. Cars (`0x3231D0`/`0x3230D0` hooks): step every R/30 frames with that many frames' time and
   move the bodies on in between; and set car `+0x34/+0x38` (written by `0x31F7D8` at every
   reset) to the step actually used (open question 1). The bodies moved on are for the picture
   only: the world's physics step (`0x2C60D0` -> `0x1B5E50`, every frame right after the cars'
   step, every rigid body's contacts) gets them put back where the last car step left them
   (TODO 154, 2026-11-05: on the moved-on body a car driving into a wall at 240 was met 0.2-0.5
   units inside it and thrown up its face, pitching - the back wheels in the air; `TF_CAR_WORLD=0`
   as before).
8. The hooks measure time in VBlank ticks (`airborneUpdate`: not in the air for 4 VBlanks;
   `speedFromVelocity`: within 2 VBlanks): scale them with R.
9. Rates that are not multiples of 30 (144, 165) cannot use "every n-th frame": the cars and
   contacts need a time accumulator instead.

**P2 - frame-counting code this survey found, not covered (visible at 60 already)**

10. The player's other states with 0.1 a frame (6b): fall, transform to car/robot, glide fall,
    powered glide, flight, and the glide-pack and foot-slip motion shaders. Same treatment as
    the 0.2/0.3 shares (patch the loads of `gp-0x748C`/`gp-0x7594` in those functions to a
    rate-dependent word, after checking each with `TF_FRAME_LOG`).
11. Cameras (6b): third person (0.3), first person (0.3), glide (0.2), jet pack (0.05),
    warp-out (0.3), and the boom recovery in `taCamera` (0.06).
12. Physics smoothing `0x1B3AE0` (0.1 and x 0.9091 a step): it reacts twice as fast at 60
    (what it decides was not followed).
13. Gameplay increments/damping (6c, 6d): boss Starscream, heavy grunt, search pivot, weapon
    vortex and twist rocket, patrol shield arm, the car state's `+0x11C`, the drop ship.
14. Cosmetic: creature effect UV scroll and flicker (`0x533010`), shader drift, snow damping,
    ribbon beam, tree fade distances (`0x4C5830`), HUD 4-frame cycle, the frame-counter shader
    source, ambience fades and wind phases, rumble windows (`0x279DA0`), Doppler (`0x2801D0`:
    multiply by R instead of 59.94 x 0.5).

**P3 - no change needed** (they take the period when they run): the every-frame
`FramePeriod` readers in section 1 marked "yes", the clock users (3), the particle periods,
the cinema player, everything that integrates with `dt`.

**Alternative for 120 pictures without a 120 Hz simulation.** The console already draws two
moments per frame (betwixtMode: field B blended halfway by `0x6CD6F0`, field A current). With
VBlanks at 119.88 in a level, the rate table at 119.88 and the half-rate byte and betwixtMode
left as the game sets them, the frame lasts two VBlanks: the simulation runs at 59.94 (the
true-60 simulation, with the P1 fixes keyed on the period instead of the flag) and every
VBlank shows a picture: 120 a second. EE and VU1 work stay as at 60 (the second pass is drawn
at 60 too, scissored to a pixel); the GS/GPU fills twice as much (the earlier two-field patch
ran at 35-38 fps at 4K, so this needs 1440p-class scales). Only scene nodes with motion
interpolators move between frames; the HUD and some effects move at 60. It stops at two
pictures a frame (two view slots in the interpolators).
Done on 2026-10-05 as the default, "in-between pictures" ([NOTES.md](NOTES.md#frame-rates));
at 4K the GPU renderer needed a third less time a frame first ([RUNTIME.md](RUNTIME.md#on-the-gpu)).

## 8. Open questions

1. **The cars' own period at true 60.** `0x31F7D8` sets car `+0x34` = the period when a car
   is reset; the tyre friction (`0x3236E0` -> `0x1C5630`) and wheel code use `+0x34`. Answered
   2026-10-04: the truck on the measured route is never reset (`TF_TRACE_FUNCS=31f6d0`): it
   keeps its constructor's 1/60 at every rate, as on the console, and the drift at 4 s (95.1 /
   92.9) is the route meeting an obstacle (each run ends where its hit takes it). A reset at the
   PC rate now gets the period of 30 (`rates.cpp`).
2. Are the 0.1 player-state blends visible at 60 (falling, gliding, flying, transforming)?
   Only walk, jump and truck were logged.
3. What does the `taGame+0xA0` frame counter drive as a shader source?
4. In-level cinema movies at true 60: the movie streams are updated once per VBlank
   (`0x274A80`, one `sceMpegGetPicture` each) and the movie sets the frame-rate flag to two
   fields, while `applyFrameRate` keeps the governor's request at 0 and the governor rewrites the
   flag every frame. Does a movie in a level play at the right speed at 60? At 120 Hz VBlanks
   the movie update and the MPEG picture clock must stay on 59.94.
5. Does anything on the IOP side (pad, sound driver) assume the EE's VBlank is 59.94 when the
   EE's VBlank is raised in a level? (The IOP keeps its own VBlank; the pad is read once a
   VBlank of the EE's; the disc stream's production counts the EE's VBlanks - at 120 the disc
   reads twice as fast, which only shortens waits.)
6. `0x244E10` (renderer, reads the governor's request) and the `betwixtMode` readers in
   gameplay code (`0x2F54C0`/`0x2F56C0`, `0x407CD0`, `0x34C390`) were not followed.
