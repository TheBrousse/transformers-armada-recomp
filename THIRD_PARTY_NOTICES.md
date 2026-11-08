# Third-party notices

This project's own code is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)).
It uses the components below. Each keeps its own licence.

## Built into the game

| Component | Author | Licence | How it is used |
|---|---|---|---|
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) | ran-j | GPL-3.0 | The recompiler and runtime (git submodule, with this port's patches) |
| [raylib](https://github.com/raysan5/raylib) | Ramon Santamaria | zlib | Window, input and graphics base (fetched by PS2Recomp) |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut | MIT | The settings menu (fetched) |
| [rlImGui](https://github.com/raylib-extras/rlImGui) | Jeffery Myers | zlib | ImGui on raylib (fetched) |
| [xbyak](https://github.com/herumi/xbyak) | MITSUNARI Shigeo | BSD-3-Clause | Code generation at run time (fetched) |
| [Opus](https://opus-codec.org/) | Xiph.Org Foundation and others | BSD-3-Clause | The DualSense speaker's sound over Bluetooth (fetched by a PS2Recomp patch) |
| [fmt](https://github.com/fmtlib/fmt) | Victor Zverovich | MIT | Text formatting (fetched) |
| [toml11](https://github.com/ToruNiina/toml11) | Toru Niina | MIT | Config files (fetched) |
| [sse2neon](https://github.com/DLTcollab/sse2neon) | DLTcollab | MIT | SIMD helpers (fetched) |
| [xxHash](https://github.com/Cyan4973/xxHash) | Yann Collet | BSD-2-Clause | Fast hashing (in a PS2Recomp patch) |
| [bc7enc](https://github.com/richgel999/bc7enc) | Rich Geldreich | MIT or public domain | Texture compression (`transformers/native_render/ext/bc7enc`) |
| [SMAA](https://github.com/iryoku/smaa) | Jorge Jimenez and others | MIT-style (see its `LICENSE.txt`) | Anti-aliasing (`transformers/native_render/ext/smaa`) |
| [FFmpeg](https://ffmpeg.org/) | The FFmpeg developers | LGPL-2.1 or later | Movie playback. Shared libraries (DLLs) downloaded during the build and placed beside the game. Source: <https://ffmpeg.org/download.html> |

## Build tools

| Component | Author | Licence | How it is used |
|---|---|---|---|
| [ELFIO](https://github.com/serge1/ELFIO) | Serge Lamikhov-Center | MIT | Reading the game's program (recompiler) |
| [rabbitizer](https://github.com/Decompollaborate/rabbitizer) | Decompollaborate | MIT | MIPS instruction decoding (recompiler) |
| [libdwarf](https://github.com/davea42/libdwarf-code) | David Anderson and others | LGPL-2.1 (library) | Debug information (recompiler) |
| [nlohmann/json](https://github.com/nlohmann/json) | Niels Lohmann | MIT | JSON (analyzer) |

## Made on your PC by the setup

| Component | Author | Licence | How it is used |
|---|---|---|---|
| [Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan) | Xintao Wang | MIT | Upscaling the textures from your disc (downloaded by `tools/make_hd_textures.ps1`) |
| [Real-ESRGAN models](https://github.com/xinntao/Real-ESRGAN) | Xintao Wang | BSD-3-Clause | The upscaling models |
| [TeX Gyre Adventor](https://www.gust.org.pl/projects/e-foundry/tex-gyre) | GUST e-foundry | GUST Font License | Redrawing the game's fonts sharp (`tools/hd_textures/typeface`) |

## Setup program

| Component | Licence |
|---|---|
| [egui / eframe](https://github.com/emilk/egui) | MIT or Apache-2.0 |
| [rfd](https://github.com/PolyMeilex/rfd) | MIT |

## Fonts from Windows

The settings menu uses the Bahnschrift font that comes with Windows. It is read from your Windows installation
and is not included here.

## The game

*Transformers* is a trademark of Hasbro. The game is copyright its owners (Hasbro, Atari, Melbourne House). No part
of the game is included in this repository or in the setup program. It is built from the player's own disc.
