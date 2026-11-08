# What's new

## 0.3.8

**Fixed**
- HD textures on graphics cards that make them black (some NVIDIA GTX 16 series): the HD texture maker now checks
  every image and tries safer ways on such cards. Existing HD sets are checked once after this update; broken images
  are made again (or, if the card cannot make them, the game's own textures are used).
- The desktop shortcut is now made by the update too (it did not appear for installs updated from inside the game).

**New**
- Controller buttons can be changed: F1 > Controls > Controller buttons, with a "Triggers fire" preset (swaps the
  bumpers and triggers) and "Reset to default". The adaptive triggers follow your layout.
- F1 > Graphics: an "HD textures: On / Off" setting. The "Texture pack" settings only show with the Emulated
  renderer, which they belong to.

## 0.3.7

**Fixed**
- Hot Shot's red parts (and other dark textures) no longer turn black or navy on some NVIDIA cards (GTX 16 series).
  The HD texture maker can produce broken images on those cards; the game now notices a black, colour-missing or
  colour-swapped HD image and uses the game's own texture instead. A repair for the HD set itself comes next.

## 0.3.6

**Fixed**
- HUD Off (F1 > Gameplay) now hides the whole HUD at once, with no restart: the bars, the Mini-Con ring, the insignia,
  the messages and the crosshair. Photo mode hides it too.
- Recon mode on the keyboard: the right mouse button goes in, zooms in two steps, then goes out. Quick clicks are no
  longer missed, and B no longer does recon (you can still bind a key to it in F1 > Controls).
- First person: the car now uses its normal car camera instead of a broken view from inside the car. First person
  comes back when you transform back into the robot.

## 0.3.5

**New**
- Playing with keyboard and mouse, the game's help texts name your keys: "W A S D - Move/Strafe" and "Mouse -
  Look/Aim" instead of the analog sticks, also for steering, recon mode and the Glide Mini-Con. With a controller the
  original words come back.
- Existing installs also get the desktop shortcut, with their next update.

## 0.3.4

**New**
- You can play while the HD textures are being made: the setup has a "Play now" button during that step. The
  textures you see first (menus, the HQ, your Autobots, the first levels) are made first, and new ones appear in the
  game while you play. Making them pauses while the game runs and carries on when you close it, unless you tick
  "Keep making HD textures while I play".
- A "Fast" choice for the HD textures: about 9 minutes instead of about an hour on a GTX 1080, a little less sharp
  on the ground. The setup suggests Fast or Best for your graphics card.
- If the game is started before the HD textures are finished, a small window finishes them in the background.
- F1 > Graphics: "Open texture folder", and "Make HD textures" for anyone who skipped them. F1 shows how many are made.

**Fixed**
- "Sharp text" now says so when its fonts have not been made yet, instead of quietly showing the old text.

## 0.3.3

**Fixed**
- HQ with Hot Shot in a 4:3 picture: the character select showed jumbled letters where his car pictures belong, and
  the QUIT station showed a big grey panel after leaving Options. Both show the right pictures now.

## 0.3.2

**Fixed**
- Closing the setup before it finished and starting it again no longer opens the game. The setup opens instead,
  with a "Finish the setup" button that carries on where it stopped.

**New**
- The setup puts a "Transformers Armada" shortcut on your desktop after the first install (it starts the game).

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
