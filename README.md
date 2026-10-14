<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# shadPS4 — inFAMOUS Second Son on Steam Deck

A personal shadPS4 fork tuned to run **inFAMOUS Second Son (CUSA00004)** on a Steam Deck OLED.
Emulator core only, no GUI. Builds come from CI on every push; tagged versions are on the
[Releases](https://github.com/duartebmarques/shadPS4/releases) page.

## What's different
- **Two-stage draw pipeline** (`draw_pipe`): PM4 decoding and Vulkan command recording run on separate threads.
- **In-place guest memory** (`inplace_guest_memory` 1–3): honest fences, plus guest memory in dma-buf chunks that the GPU uses directly.
- **GPU-time timestamps**: guest GPU timestamps count GPU execution time, which fixes the "refocus" flicker.
- **Input**: `motion_shake` (graffiti) and `touchpad_two_finger` (car push).
- Texture-cache GC, stream copy lanes, and more. See [`docs/infamous-deck-handoff.md`](docs/infamous-deck-handoff.md).

Result: about 12–25 fps on the base fork, about 29 fps now (open-world scene, 1280x720 render
patch, GPU at 1600 MHz).

## Setup
Tested only on Steam Deck (SteamOS, AMD GPU, RADV). Windows and macOS builds are untested.

1. From [Releases](https://github.com/duartebmarques/shadPS4/releases), download the zip for
   your platform: `-linux` (Steam Deck), `-win64` or `-macos`. Unzip it into its own folder. It
   holds the emulator, a `user` folder, `LICENSE` and this README. With a CI artifact instead,
   copy [`dist/user/`](dist/user) next to the executable as a folder named `user`.
2. Dump the game (CUSA00004) and its 1.07 update. Put them side by side:
   `<games>/CUSA00004/` and `<games>/CUSA00004-UPDATE/` (or `-patch`).
3. The emulator uses `./user` from the directory it starts in, so start it from the unzipped
   folder. Without `./user` it falls back to `~/.local/share/shadPS4` (Linux),
   `%APPDATA%\shadPS4` (Windows) or `~/Library/Application Support/shadPS4` (macOS), and the
   settings below are not used. `user` contains:
   - `custom_configs/CUSA00004.json`: the tuned settings. The code defaults keep the fork's
     features off, so without this file the game runs at about base-fork speed.
   - `input_config/default.ini`: the controller map, plus `motion_shake = home` (simulates a
     controller shake for graffiti) and `touchpad_two_finger = t` (car push).
   - `patches/shadPS4/files.json`: the patch index for the next step.
4. Patches: put [`InfamousSecondSon.xml`](https://github.com/shadps4-emu/ps4_cheats/blob/main/PATCHES/InfamousSecondSon.xml)
   in `user/patches/shadPS4/` and set `isEnabled="true"` on "Skip intro movie" and
   "Render resolution 1280x720".
5. In Steam Input, map two back buttons to the keyboard keys Home and T.
6. Run `./Shadps4-sdl.AppImage <games>/CUSA00004/eboot.bin` (`shadPS4.exe` on Windows,
   `./shadps4` on macOS). As a Steam shortcut: target the executable, put the `eboot.bin` path
   in launch options, and set "Start In" to the unzipped folder. Set the GPU clock to 1600 MHz in
   the Quick Access performance menu. At the default clock, heavy scenes can flicker: the game
   switches a post-processing pass on and off when its GPU frame time is near its limit. The numbers above use the in-game frame rate "Unlocked".
   On the first run with a new `user` folder a "Save Migration" box appears; pick "Do nothing".

Platform notes:
- `inplace_guest_memory` 2 and 3 need Linux and an AMD GPU whose driver exports dma-bufs (RADV).
  Elsewhere they fall back to mode 1 (honest fences only).
- `draw_pipe` and `async_submit` (on in the shipped config) are untested on Windows and macOS.
  If the game misbehaves there, set them to `false` in `user/custom_configs/CUSA00004.json`.

## Build
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```
See [`CLAUDE.md`](CLAUDE.md) for the exact toolchain (Ubuntu 24.04 distrobox, clang-19).

To cut a release, push a tag: `git tag v1.0 && git push origin v1.0`. CI builds Linux, Windows
and macOS and publishes one zip per platform (executable, `user/`, `LICENSE`, `README.md`).

## Credits
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the emulator ([website](https://shadps4.net/)).
- GR2fork "EXODUS" by junminlee2004 ([Ko-fi](https://ko-fi.com/junminlee2004)): the base of this fork.
- [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) ("bbport"): source of the in-place memory and two-stage pipeline designs.

Developed with AI assistance (Claude Code).

## License
[GPL-2.0](LICENSE)
