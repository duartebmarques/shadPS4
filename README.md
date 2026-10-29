<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# inFAMOUS on Steam Deck

A shadPS4 fork tuned to play the **inFAMOUS** games on the Steam Deck, packed as one folder: no
installer needed. Pick a game, switch its patches (render resolution, skip intro) and play, all
with the Deck's controls.

| Game | Title id | Version | State on the Deck OLED |
|---|---|---|---|
| inFAMOUS Second Son | CUSA00004 (+ other regions) | update 1.07 | about 30 fps (the game's cap), graffiti shake and trackpads work |
| inFAMOUS First Light | CUSA00897 (+ other regions) | update 1.04 (1.00 works too) | **work in progress**: playable, about 30 fps most of the time, heavy fights can still dip to the low 20s |

More games are added through `games.json` (see [Adding a game](#adding-a-game)).

## Install (about 5 minutes)

**What you need**
- A Steam Deck (made and tested on the OLED) connected to the internet.
- Your own copy of the games, dumped from your PS4, as extracted folders or `.pkg` files, anywhere
  in your home folder or on the SD card (for example `~/Games`). This project does not provide
  game files.

**1. Get the app.** In Desktop Mode (Steam button > Power > Switch to Desktop), download
`shadps4-<version>-linux.zip` from [Releases](https://github.com/duartebmarques/shadPS4/releases)
and extract it where you like (right-click > Extract > Extract archive here). You get one folder,
`shadPS4-infamous`.

**2. Start it.** Double-click `shadPS4.sh` in that folder (choose *Execute* if asked). The first
start finds your games, their update and DLC, and turns on the Steam Deck patches. Then the game
picker opens: choose a game, then **Play**, **Patches** or **Add to Steam**.

**3. Add it to Steam** (for Gaming Mode): on the game's page choose **Add to Steam**. Steam closes
for a moment and starts again with the game in Library > Non-Steam, with its artwork; that
shortcut starts the game directly. (`./shadPS4.sh --add-to-steam` adds every game at once.)

Or steps 1 and 2 with one line in Konsole (installs into `~/Applications/shadPS4-infamous`):

```bash
bash <(curl -fsSL https://github.com/duartebmarques/shadPS4/releases/latest/download/install.sh)
```

## Set once in Gaming Mode (recommended)

While the game runs, press the **...** button > **Performance** (battery icon):
- turn on **Use per-game profile** and **Advanced view**;
- turn on **Manual GPU clock** and set it to **1600 MHz**: heavy fights need it;
- turn the **Frame limit off**: it adds input lag.

> [!IMPORTANT]
> **After every game start, turn Manual GPU clock off and on again.** A Steam bug
> ([SteamOS#2586](https://github.com/ValveSoftware/SteamOS/issues/2586)) drops the GPU lock when a
> game exits, and the next start does not restore it, although the Performance menu still shows
> 1600 MHz. Without the lock the GPU clock falls as low as 200 MHz and the game can drop to under
> 10 fps. Turning the toggle off and on applies the lock until the game is closed.

In the game: **Options > Video > Frame Rate > 30fps limit**. The emulator paces the game's own
limit so each frame is shown on time with little input lag. *Unlimited* runs as fast as the
machine allows (uneven on the Deck, about 34 fps).

## Good to know
- **First start:** the emulator builds shaders while you play, so the first minutes stutter. It
  gets smooth and stays smooth on later starts.
- **Controls:** the Deck's buttons work as a PS4 controller. Shake the Deck to shake the controller
  (graffiti spray cans). The trackpads act as the PS4 touchpad (swipes, the two-finger press for the
  car push). On a pad without motion, map the keyboard keys **Home** (shake) and **T**
  (two-finger press) to spare buttons in the game's controller settings.
- **Everything stays in the app folder:** saves in `user/home/1000/savedata/`, settings in
  `user/`. Saves from an earlier shadPS4 setup are copied there on the first start (the originals
  are left alone). Move or back up the folder as a whole.
- **Trophies** only unlock with your own PS4 trophy key (it cannot be shipped). Put it in
  `user/keys.json` as `{"TrophyKeySet": {"ReleaseTrophyKey": "<32 hex digits>"}}`. Without it the
  games play normally and trophies stay locked.
- **Patches:** in the game picker choose a game > **Patches**. The Deck defaults are the 1280x720
  render patch and skip intro; on a stronger PC turn the resolution patch off (or pick a higher
  one) for a sharper image.
- **Update:** run `./shadPS4.sh --update` in the app folder. Your saves, controls and patch choices
  are kept; the previous build stays in `app.prev`.
- **Uninstall:** delete the app folder (back up the saves first) and remove its shortcuts in Steam
  (game > gear > Manage > Remove non-Steam game).

## Troubleshooting
- **A game is missing from the picker:** the first start looks in your home folder and on SD cards,
  up to six folders deep, skipping hidden folders. Copy the game folder (the one with `eboot.bin`)
  or its `.pkg` files there and run `./shadPS4.sh --setup` in a terminal (it can extract pkg files).
- **No update found:** put the update folder next to the game as `<game folder>-UPDATE` (or
  `-patch`) and run `--setup` again.
- **Low fps in fights:** check the 1600 MHz GPU clock above and turn it off and on again after the
  game starts (Steam bug, see above). Turn off other overlays and frame limiters.
- **Uneven frame rate or aiming feels laggy:** set the game's Frame Rate option to *30fps limit*
  (Options > Video) and the Deck's frame limit off.
- **Reporting a problem:** include `user/log/shad_log.txt` from the app folder.

All options: `./shadPS4.sh --help`.

## Adding a game
1. Add it to `dist/games.json`: its title ids (all regions), the update it needs, its patch file in
   [ps4_cheats](https://github.com/shadps4-emu/ps4_cheats) and the patches to turn on by default.
2. Tuned settings go in `dist/user/custom_configs/<title id>.json`; patches the community file
   lacks (other game versions) go in `dist/user/patches/infamous-deck/` with its `files.json`.
3. Measure it with the bench tools (`agent/TOOLS.md`: `title-ab`, `game-run`, `pad-session`).

## What's different from shadPS4
- **Three-stage draw pipeline**: PM4 decoding, draw preparation (`draw_pipe`) and the Vulkan
  driver calls (`vk_record_thread`, from EXODUS) run on separate threads; new shaders compile in
  the background.
- **In-place guest memory** (`inplace_guest_memory` 1–3): honest fences, plus guest memory in dma-buf chunks that the GPU uses directly.
- **GPU-time timestamps**: guest GPU timestamps count GPU execution time, which fixes the "refocus" flicker.
- **Present thread and low-lag pacing**: the game's own 30 fps limit (or a `frame_cap`) is paced so
  each frame is done just before it is shown: steady 30 fps with less than half the input lag.
- **Flip lead** (`flip_lead_us`): a game that runs ahead of the emulated GPU is held after each
  frame just long enough, so the input it reads is not queued behind a frame of commands
  (pad-to-screen lag about 33 ms in First Light, 36 ms in Second Son, and a few % more fps).
- A log-based barrier tracker, and builds optimized with profiles from real play (PGO).
- **Steam Deck input**: the Deck's own motion sensor (shake past the sensor's limit) and trackpads
  feed the emulated controller.
- Texture-cache GC, stream copy lanes, and more. See [`docs/infamous-deck-handoff.md`](docs/infamous-deck-handoff.md).

Result: about 12–25 fps on the base fork, about 30 fps now (open-world scene, 1280x720 render
patch, GPU at 1600 MHz).

<details>
<summary>Manual setup</summary>

- `app/usr/bin/shadps4` is the emulator. Start it from the app folder, because it uses the
  `user` folder in the directory it starts in. Without one, it falls back to
  `~/.local/share/shadPS4` or `%APPDATA%\shadPS4`, and the tuned settings are not used.
  `--big-picture` opens the game picker; a game's `eboot.bin` as the argument starts it.
- Put the update next to the game as `<game folder>-UPDATE` (or `-patch`). Put DLC folders in
  `user/addcont/<title id>/`.
- Releases are Linux only. Other platforms need a build from source and are untested:
  `inplace_guest_memory` 2 and 3 need Linux and an AMD GPU with RADV (elsewhere they fall back to
  mode 1), and `draw_pipe` / `async_submit` may need `false` in `user/custom_configs/<title id>.json`.
</details>

## Build
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```
See [`CLAUDE.md`](CLAUDE.md) for the exact toolchain (Ubuntu 24.04 distrobox, clang-19).

To cut a release, push a tag: `git tag v1.0 && git push origin v1.0`. CI publishes a zip with the
`shadPS4-infamous` folder (the extracted PGO build in `app/`, `shadPS4.sh`, `games.json`, `tools/`,
the default user files in `dist-user/`), `install.sh` on its own (the one-line install), and the
notes from `dist/RELEASE.md`.

## Credits
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the emulator ([website](https://shadps4.net/)).
- GR2fork "EXODUS" by junminlee2004 ([Ko-fi](https://ko-fi.com/junminlee2004)): the base of this fork.
- [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) ("bbport"): source of the in-place memory and two-stage pipeline designs.

Developed with AI assistance (Claude Code).

## License
[GPL-2.0](LICENSE)
