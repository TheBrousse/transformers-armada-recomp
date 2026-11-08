# What's new

## 0.3.1

**Fixed**
- The frame rate no longer stays at 60 in levels and the HQ when the frame rate limit is set higher (for example 144
  or 165). The "Frame rate limit" setting now always decides.

**New**
- The settings menu (F1) works with a controller: open it with the PS button (on Xbox, hold View and Menu), move with
  the D-pad, Cross (A) to choose, Circle (B) to go back.
- DualSense and DualShock 4: the light bar shows your Autobot's colour (Optimus red, Hot Shot yellow, Red Alert blue).
- DualSense: adaptive triggers for weapon Mini-Cons (missiles click, cannons click harder, the sniper rifle resists
  steadily), off in car mode. The shot fires at the click. "Trigger strength" in F1 > Controls.
- DualSense: the controller's speaker plays the transform sound, enemies spotting you, the low-health warning and HQ
  calls (Bluetooth or cable). "Surround" plays all sound from both the PC and the controller, with a delay setting
  for Bluetooth. Both are in F1 > Controls.

## 0.3.0

**Fixed**
- Linux and Steam Deck (Proton): the screen no longer turns pink after the Atari logo. The game now shows its picture
  under Proton (checked on Linux with GE-Proton). Building the game still needs Windows.

**New**
- DualSense and DualShock 4: the PS button opens and closes the settings menu (F1). On an Xbox controller, hold View
  and Menu together.
- DualSense and DualShock 4: the light bar blinks blue and red while your health is low, and blinks while your Autobot
  is destroyed. It can be turned off in F1 > Controls ("Light bar warnings").

## 0.2.9

**New**
- Once the game is built, starting the setup again starts the game straight away, so a game launcher (for example
  Quiver) can use the setup as its Play button. Hold Shift while starting it to open the setup's window instead.

**Fixed**
- Emulated renderer: a single garbled frame (red and green bands) that sometimes showed a second or two after
  loading a save state.

## 0.2.8

**Fixed**
- The setup no longer stops at "Disc and game" with "detected dubious ownership" when the game is installed on a
  drive formatted as exFAT or FAT32 (many external drives and memory cards).

## 0.2.7

**New**
- The game can be started straight from its .exe, for example as a non-Steam game in Steam (Steam Deck with Proton).
  It finds its own folder and makes its settings file on the first start, like Play.bat. Building the game still
  needs Windows.

## 0.2.6

**Fixed**
- The setup no longer stops at "Tools" with "pip install cmake failed" when another program (for example Inkscape)
  has put its own Python on your PC. It now uses a full Python install, or installs one.

## 0.2.5

**Fixed**
- The setup no longer stops at "Tools" with "no newer package versions are available" when Git is already
  installed but Windows can't find it. It now finds and uses the Git you have.
- Native renderer: the HQ warp gate's tunnel no longer flickers dark every few frames. It pulses smoothly, like on
  the PS2.

## 0.2.4

**Fixed**
- With the Emulated renderer (Graphics > Renderer), your Autobot's shadow on the ground was missing. It's back.
- The setup program now builds and publishes itself automatically again.

**Changed**
- The setup program now carries proper Windows file details and avoids things antivirus programs tend to dislike,
  so it should be flagged far less often.

## 0.2.3 (first public release)

**New**
- Settings menu (F1) tidied up: presets at the top of Graphics (Low, Medium, High, Ultra), related options merged,
  and the fine details under "Advanced".
- "Trees: Full 3D at any distance" option: no more flat trees popping in the distance (heavy; Ultra turns it on).
- Xbox controllers vibrate, and the button prompts follow your controller (PlayStation or Xbox symbols, or your
  keyboard keys).
- Modern stick response: a light touch walks, running starts at about a third of the stick.
- The game's .exe now has its own icon, made from your disc.

**Fixed**
- Ragdolls behave as on the PS2 at every frame rate: Megatron's kick throws you into the lava again instead of
  kicking you over and over, and falls no longer spin in the air.
- Human mode: enemies now aim at your size instead of shooting over your head.
- First-person view: walking, running and jumping work like in third person.
- The crosshair is no longer cut in half by the ground.
- Movies no longer stutter when the game is in the background (alt-tab).
- Less input delay at 30, 60 and 90 fps frame caps.
- A busy PC (a video render, a big download) no longer turns the main menu black; the game just runs slower.
- The HQ's Options screen no longer flashes pixelated blocks during its camera move.
- The Emulated renderer at very high settings no longer crashes on graphics cards with less memory.
- Antarctica's music plays again when you replay the level after finishing it (an original PS2 bug).
- Decepticlones dropped by a dropship no longer hang frozen in the air after a cutscene (an original PS2 bug).
