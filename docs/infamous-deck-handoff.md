# inFAMOUS Second Son (CUSA00004) on Steam Deck OLED — handoff

Game: CUSA00004 with its update in a `CUSA00004-patch` folder, dumped from the user's own copy.
Device: Steam Deck OLED (SteamOS, gamescope, RADV, 16 GB shared RAM, GTT total 8 GiB, VRAM carve-out 1 GiB).

## Branch and builds
- Fork: github.com/duartebmarques/shadPS4, branch `exodus-uma-shake`. Base: GR2fork EXODUS (0.19.1 WIP), which already includes upstream up to #5327.
- CI runs on push, Linux only. The artifact is `shadps4-linux-sdl-<date>-<sha>` (AppImage).
- Latest build: `3834b34`.
- Our commits on top of EXODUS:
  - `motion_shake` input binding: fixes the graffiti soft-lock. Code in `src/input/controller.*` and `input_handler.*`.
  - Texture-cache GC with 20 s idle eviction, plus 64 MiB VMA blocks (ed51855).
  - Guest memory in place, ported from bbport (deadinside28/bloodborne_pc):
    - aa74561: honest fences, plus dma-buf chunks bound in place.
    - 8d23028: CP writes (WRITE_DATA, DMA) done on the GPU in stream order.
    - 3834b34: small read-only buffers read in place, guarded against CP CPU writes.
  - Upstream #5340, analog smoothing fix (6824469).
- Code map:
  - `src/video_core/renderer_vulkan/vk_guest_memory.*`: chunk allocator.
  - `src/core/address_space.cpp`: `InstallChunks`, `MapChunked`.
  - `src/core/memory.cpp`: `DirectPhysOf`, `TryWriteLabel`, `InstallGuestMemoryChunks`.
  - `src/video_core/buffer_cache/buffer_cache.cpp`: `EnsureResident` in-place runs, `TryInPlaceRead`, `NoteCpWrite`.
  - `src/video_core/amdgpu/liverpool.cpp`: `QueueHonestSignal`, `PendingLabelSatisfies`, `WriteData`.
  - `src/video_core/renderer_vulkan/vk_rasterizer.cpp`: `WriteDataOnGpu`, Fill/CopyBuffer in-place routing, telemetry.
- Local builds needed compat fixes for clang 18 (`ranges::to`, `constexpr`, `<optional>`). These are not committed; CI uses clang 19.

## Settings
In `~/.local/share/shadPS4/config.json`, section `"GPU"`:
- `inplace_guest_memory` = 3. Values:
  - 0: off.
  - 1: honest fences only.
  - 2: guest memory in place.
  - 3: 2, plus small reads in place.
- Readbacks Relaxed and readback linear images on. These fixed the dark image.
- Keep these at their defaults: `vblank_frequency` 60, `stream_copy_lane_threads` 4, `stream_copy_idle_us` 0. Changing them didn't help or made things worse.
- The community 720p patch is on. It has no effect on FPS.

Input, in `~/.local/share/shadPS4/input_config/CUSA00004.ini` (or `default.ini`):
- `motion_shake` is bound to a keyboard key, with a Deck back button mapped to that key in Steam Input.
- Add `touchpad_two_finger = t` for the Reggie car push, which needs two fingers on both ends of the touchpad plus a click. Map a back button to keyboard T in Steam Input.

## Measurements (latest)
- **FPS:** 15–20. It was 12–25 before this work.
- **Mode 3:** removed all stream copies (`LANE jobs=0`, `small_inplace=15.3M` per 300 frames). CPU idle went from 17% to 56%, but FPS rose only about 1.
- **GPU clock:** Decky PowerTools had been blocking Steam's GPU clock setting. With it uninstalled, a locked 1600 MHz gives the same FPS as 1040 MHz.
- **Resolution and vblank:** the 720p patch made no difference, and neither did vblank 120. So FPS is not limited by GPU throughput.
- **Threads now:**
  - Game:Main is at about 95% of one core.
  - Its 5 JobWork threads are at about 9%.
  - Two emulator threads, one of them GpuComm, are at about 57%.
  - `gpu_busy_percent` reads 100% while GPU power is only about 2.2 W. That suggests the GPU is mostly waiting, not working.
- **Memory:** about 3–4.8 GB available and about 0.5 GB of swap, so memory is no longer thrashing.

## Open question / next step
Find out what Game:Main spends its 95% on:
- the game's own work;
- emulator HLE overhead (faults, syscalls);
- spin-waiting on GPU labels, which honest fences now delay until the GPU really finishes.

Ways to measure:
- `perf top -t <tid>` or `perf record`. On SteamOS this needs distrobox, or `steamos-readonly disable` plus pacman.
- utime/stime split from `/proc/<pid>/task/<tid>/stat`.
- Guest hotspots can be mapped onto the `CUSA00004-patch` `eboot.bin`.
