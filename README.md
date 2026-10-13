<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# shadPS4 — inFAMOUS Second Son on Steam Deck

A personal shadPS4 fork tuned to run **inFAMOUS Second Son (CUSA00004)** on a Steam Deck OLED.
Emulator core only, no GUI. Linux builds come from CI on every push.

## What's different
- **Two-stage draw pipeline** (`draw_pipe`): PM4 decoding and Vulkan command recording run on separate threads.
- **In-place guest memory** (`inplace_guest_memory` 1–3): honest fences, plus guest memory in dma-buf chunks that the GPU uses directly.
- **GPU-time timestamps**: guest GPU timestamps count GPU execution time, which fixes the "refocus" flicker.
- **Input**: `motion_shake` (graffiti) and `touchpad_two_finger` (car push).
- Texture-cache GC, stream copy lanes, and more. See [`docs/infamous-deck-handoff.md`](docs/infamous-deck-handoff.md).

Result: about 12–25 fps on the base fork, about 29 fps now (open-world scene, GPU at 1600 MHz).

## Build
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```
See [`CLAUDE.md`](CLAUDE.md) for the exact toolchain (Ubuntu 24.04 distrobox, clang-19).

## Credits
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the emulator ([website](https://shadps4.net/)).
- GR2fork "EXODUS" by junminlee2004 ([Ko-fi](https://ko-fi.com/junminlee2004)): the base of this fork.
- [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) ("bbport"): source of the in-place memory and two-stage pipeline designs.

Developed with AI assistance (Claude Code).

## License
[GPL-2.0](LICENSE)
