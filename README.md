<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# inFAMOUS Second Son on Steam Deck

A shadPS4 fork tuned to play **inFAMOUS Second Son** on the Steam Deck: about 30 fps (the game's
cap), working graffiti shake and trackpads, and one installer that sets up everything, including
the Steam shortcut.

## Install (3 steps, about 5 minutes)

**What you need**
- A Steam Deck (made and tested on the OLED) connected to the internet.
- Your own copy of the game, dumped from your PS4: **CUSA00004** plus update **1.07** (DLC
  optional), as extracted folders or `.pkg` files. Copy it anywhere in your home folder or on the
  SD card, for example `~/Games`. This project does not provide game files.

**1. Switch to Desktop Mode.** Press the Steam button > Power > Switch to Desktop.

**2. Run the installer.** Open **Konsole** (app menu > System > Konsole), paste this line
(right-click > Paste) and press Enter:

```bash
bash <(curl -fsSL https://github.com/duartebmarques/shadPS4/releases/latest/download/install.sh)
```

It asks a few questions; press Enter to accept each one. It downloads the emulator, finds the
game, its update and DLC (and extracts `.pkg` files if that's what you have), turns on the
patches, and adds **inFAMOUS Second Son** to Steam. Steam closes for a moment and starts again.

**3. Play.** Double-click **Return to Gaming Mode** on the desktop, then open Library >
Non-Steam > **inFAMOUS Second Son**.

<details>
<summary>No internet, or you'd rather not paste commands?</summary>

Download `shadps4-<version>-linux.zip` from
[Releases](https://github.com/duartebmarques/shadPS4/releases), extract it (right-click >
Extract > Extract archive here), and double-click `install.sh` (choose *Execute* if asked).
The patch file still needs internet; without it the game runs, just slower.
</details>

## Set once in Gaming Mode (recommended)

While the game runs, press the **...** button > **Performance** (battery icon):
- turn on **Use per-game profile** and **Advanced view**;
- turn on **Manual GPU clock** and set it to **1600 MHz**: heavy fights need it;
- turn the **Frame limit off**: the emulator caps the game at 30 fps itself, with less input lag.

## Good to know
- **First start:** the emulator builds shaders while you play, so the first minutes stutter. It
  gets smooth and stays smooth on later starts.
- **Controls:** the Deck's buttons work as a PS4 controller. Shake the Deck to shake the controller
  (graffiti spray cans). The trackpads act as the PS4 touchpad (swipes, the two-finger press for the
  car push). On a pad without motion, map the keyboard keys **Home** (shake) and **T**
  (two-finger press) to spare buttons in the game's controller settings.
- **Saves** are in `~/Applications/shadPS4-infamous/user/home/1000/savedata/`. Saves from an
  earlier shadPS4 setup are copied there on install (the originals are left alone).
- **Update:** in Desktop Mode, run `~/Applications/shadPS4-infamous/install.sh --update`. Your
  saves, controls and patch choices are kept.
- **Uninstall:** delete `~/Applications/shadPS4-infamous` (back up the saves first) and remove the
  shortcut in Steam (game > gear > Manage > Remove non-Steam game).

## Troubleshooting
- **"CUSA00004 not found":** the search looks in your home folder and on the SD card, up to six
  folders deep. Point it at the folder that has `eboot.bin`:
  `~/Applications/shadPS4-infamous/install.sh --game "/path/to/CUSA00004"`.
- **"no update found":** put the 1.07 update folder next to the game as `CUSA00004-UPDATE` (or
  `-patch`), or let the installer link it when it asks.
- **The shortcut is missing:** run the installer again with Steam open in Desktop Mode, or add
  `~/Applications/shadPS4-infamous/inFAMOUS Second Son.sh` as a non-Steam game.
- **Low fps in fights:** check the 1600 MHz GPU clock above. Turn off other overlays and frame
  limiters.
- **Reporting a problem:** include `~/Applications/shadPS4-infamous/user/log/shad_log.txt`.

All installer options: `~/Applications/shadPS4-infamous/install.sh --help`.

## What's different from shadPS4
- **Two-stage draw pipeline** (`draw_pipe`): PM4 decoding and Vulkan command recording run on separate threads.
- **In-place guest memory** (`inplace_guest_memory` 1–3): honest fences, plus guest memory in dma-buf chunks that the GPU uses directly.
- **GPU-time timestamps**: guest GPU timestamps count GPU execution time, which fixes the "refocus" flicker.
- **Present thread and frame cap** (`frame_cap`): steady 30 fps with about a third of the input lag.
- **Background pipeline compiles** for every-frame passes, and a log-based barrier tracker.
- **Steam Deck input**: the Deck's own motion sensor (shake past the sensor's limit) and trackpads
  feed the emulated controller.
- Texture-cache GC, stream copy lanes, and more. See [`docs/infamous-deck-handoff.md`](docs/infamous-deck-handoff.md).

Result: about 12–25 fps on the base fork, about 30 fps now (open-world scene, 1280x720 render
patch, GPU at 1600 MHz).

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

To cut a release, push a tag: `git tag v1.0 && git push origin v1.0`. CI publishes a zip
(the PGO AppImage, `user/`, `LICENSE`, `README.md`, `install.sh`), `install.sh` on its own (the
one-line install), and the notes from `dist/RELEASE.md`. To try a CI build, pass its AppImage to the installer with `--appimage FILE`.

## Credits
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the emulator ([website](https://shadps4.net/)).
- GR2fork "EXODUS" by junminlee2004 ([Ko-fi](https://ko-fi.com/junminlee2004)): the base of this fork.
- [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) ("bbport"): source of the in-place memory and two-stage pipeline designs.

Developed with AI assistance (Claude Code).

## License
[GPL-2.0](LICENSE)
