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

## Install (Steam Deck)
You need your own dump of the game (CUSA00004) and its 1.07 update, either extracted folders or
the `.pkg` files.

1. In Desktop Mode, download `shadps4-<version>-linux.zip` from
   [Releases](https://github.com/duartebmarques/shadPS4/releases) and extract it.
2. Run `install.sh` from the extracted folder (double-click it, or run `./install.sh` in a terminal).
3. Go back to Gaming Mode and start **inFAMOUS Second Son** from your library.

The installer:
- installs into `~/Applications/shadPS4-infamous`, with the tuned settings and controller map;
- finds the game, its update and DLC in your home folder or on the SD card. When it finds only
  `.pkg` files, it extracts them with a `pkg_extractor*.AppImage` it finds (or `--extractor FILE`);
- downloads the [patch file](https://github.com/shadps4-emu/ps4_cheats/blob/main/PATCHES/InfamousSecondSon.xml)
  and turns on "Skip intro movie" and "Render resolution 1280x720";
- copies your saves from an earlier shadPS4 setup, if any (it never changes the originals);
- adds the Steam shortcut. Steam has to be closed for this, so the installer offers to close it
  and start it again.

Run `./install.sh --help` for the options, such as `--game DIR` when the search does not find the game.

Set these once in Gaming Mode:
- **Controller settings** for the game: map two back buttons to the keyboard keys **Home** (shakes
  the controller, for graffiti) and **T** (two-finger touchpad press, for the car push).
- **Performance** (optional): a manual GPU clock of 1600 MHz. At the default clock, heavy scenes
  can flicker. The fps numbers above use the in-game frame rate "Unlocked".

**Update:** run `~/Applications/shadPS4-infamous/install.sh --update`. It keeps your saves,
controller map and patch choices, and installs the release's tuned settings. If you changed those
settings, your old copy is kept as `user/custom_configs/CUSA00004.json.bak`.

<details>
<summary>Manual setup</summary>

- Unzip the release for your platform. Start the emulator from that folder, because it uses the
  `user` folder in the directory it starts in. Without one, it falls back to
  `~/.local/share/shadPS4` or `%APPDATA%\shadPS4`, and the tuned settings are not used.
- Put the update next to the game as `<game folder>-UPDATE` (or `-patch`). Put DLC folders in
  `user/addcont/CUSA00004/`.
- Put the patch file in `user/patches/shadPS4/` and set `isEnabled="true"` on the two patches
  above.
- Run `Shadps4-sdl.AppImage <game folder>/eboot.bin`.
- Releases are Linux only. Other platforms need a build from source and are untested:
  `inplace_guest_memory` 2 and 3 need Linux and an AMD GPU with RADV (elsewhere they fall back to
  mode 1), and `draw_pipe` / `async_submit` may need `false` in `user/custom_configs/CUSA00004.json`.
</details>

## Build
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```
See [`CLAUDE.md`](CLAUDE.md) for the exact toolchain (Ubuntu 24.04 distrobox, clang-19).

To cut a release, push a tag: `git tag v1.0 && git push origin v1.0`. CI builds Linux and
publishes a zip (AppImage, `user/`, `LICENSE`, `README.md`, `install.sh`). To try a CI build, pass its AppImage to the installer with `--appimage FILE`.

## Credits
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the emulator ([website](https://shadps4.net/)).
- GR2fork "EXODUS" by junminlee2004 ([Ko-fi](https://ko-fi.com/junminlee2004)): the base of this fork.
- [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) ("bbport"): source of the in-place memory and two-stage pipeline designs.

Developed with AI assistance (Claude Code).

## License
[GPL-2.0](LICENSE)
