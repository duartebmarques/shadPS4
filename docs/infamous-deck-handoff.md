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

## 2026-10-10 evening: Game:Main is a spin-wait, not work (Claude Code session)
Tooling:
- Local builds: `shadps4-build` distrobox. `build/` is Release; `build-prof/` is RelWithDebInfo with frame pointers.
- `infamous-build ci|local|zip|rollback|status` swaps the AppImage the launcher runs.
- `infamous-profile [secs]` profiles a running game. perf works on the host unprivileged (user-space only). gdb/strace attach are blocked (ptrace_scope=1).

First profile, title/menu scene, profiling build, mode 3:
- **Thread CPU:**
  - Game:Main: 112% user, 0% sys, about 0 page faults/s. So there is no HLE/syscall/fault overhead.
  - GpuComm: 42%.
  - The 5 JobWorkers: about 9% each.
- **92.5% of Game:Main samples are in one guest loop at `eboot.bin+0x53b340` (patch 1.07 eboot):**
  - `0x53b2b0` is `Delay(us)`. Delays of 500 µs or less busy-spin on RDTSC. The scale is the TSC frequency / 1e6, computed once from `*(0x7f91f88)`. Longer delays jump to an import, probably `sceKernelUsleep`.
  - Its caller is a poll loop, `0x1695e0` (twin at `0x1695a0`):
    `while ((u32)g_slots[this->idx] < this->target) Delay(50);`
    It is called from the main loop at `0x29cd56` → `0x3ee470`.
  - `g_slots` is at `0xe17e80`: `{u64* slots; u32 free_head}`. It's a freelist pool of 8-byte slots, zeroed on allocation.
- **Hypothesis:** the slots are GPU-written labels (EOP/RELEASE_MEM). If so, Game:Main is waiting for GPU completion, and the real limit is the latency of the GPU pipeline: game submit → GpuComm decode → Vulkan submit → GPU → honest fence → label. That fits the GPU reading 100% busy at 2.2 W, with no thread saturated.
- **Unverified:** whether the slots are written by the GPU or by another CPU thread, and whether gameplay scenes show the same picture (this profile was the menu).

Next:
1. Profile a slow gameplay scene with `infamous-profile`.
2. Instrument liverpool label writes (EOP/RELEASE_MEM/WRITE_DATA destination addresses) and log the hits inside the slot array, to confirm what writes it.
3. A/B `inplace_guest_memory` 0 vs 1 at the same spot, comparing the spin share and FPS.
4. Measure the submit → label latency per frame.

### Label probe results (same evening, menu scene)
The probe is local and uncommitted. It's enabled with `SHAD_LABEL_PROBE=1` and lives in liverpool.cpp, vk_scheduler.cpp and vk_runtime.cpp.

**The slot pool is written by the GPU.**
- The pool is at `0x248000000`, in guest direct memory, and only 3 slots are used.
- The writers are RELEASE_MEM on the compute queue (about 350 hits/s) and EOP (about 17/s, one per frame).

**Label latency is all GPU-side backlog** (mode 3):
- decode → vkQueueSubmit: about 80 µs.
- submit → signal: 5–20 ms on average, up to about 45 ms.
- There are about 700 submits/s. The GPU queue depth at submit averages 3–4 ticks, with a maximum of about 17.

**Mode 1 is unusable**, with the same build and scene:
- the user saw graphical issues, and it felt worse;
- frame rate is lower (EOP 125–165 per 2 s versus 165–200 in mode 3);
- submit → signal is about 30 ms.
- Keep mode 3.

**Leading hypothesis: barrier drains.**
- Each frame has about 5,200 draws, about 1,900 `Runtime::FlushBarriers` pipeline barriers (about 16k/s), and about 600 image barriers.
- Every one of those barriers uses `dstStageMask = eAllCommands` and calls `EndRendering`, so each drains the GPU and breaks the render pass. That's a full GPU drain every 2–3 draws.
- This fits: GPU timeline full (backlog), only 2.2 W, insensitive to GPU clock, Game:Main spinning on labels.
- Next:
  - break down which callers trigger FlushBarriers (buffer write→read hazards versus image layout changes);
  - measure GPU time per submit with timestamp queries;
  - then make the barriers narrower (precise dst stages, skip barriers that aren't needed) without game-specific hacks.

### Correction: the "menu" runs were gameplay
The unattended runs load straight into gameplay because of the skip-intro patch. Delsin is idle on the Fish Cannery rooftop, with fire and particles in view (screenshots in `profiles/scene*.png`).

### Barrier attribution and the guest-sync experiment
**Where the barriers come from** (FlushBarriers callers, per 2 s):

| Caller | Barriers |
|---|---|
| DispatchDirect | 20k (63%) |
| Draw | 6.4k (mostly image transitions) |
| BeginRendering | 4.5k |

**Dispatch barrier reasons:** almost all are a buffer *write* overlapping an earlier access. `W` is about 14k and `GW` (GDS + write) about 8k. True RAW is only about 34.

**Did the game ask for a sync?** On the compute queue, about 25.5k dispatches per 2 s produced about 21.9k barriers *without* any game sync since the previous dispatch, versus about 330 after one. So about 98.5% of compute drains are emulator-inferred hazards that the game never requested. The sync events counted are CS_PARTIAL_FLUSH, the CACHE_FLUSH events, ACQUIRE_MEM and WAIT_REG_MEM.

**Experiment `SHAD_TRUST_GUEST_SYNC=1`** (local probe, not for commit): skip the drain when there was no game sync and the only hazard is a G/W write overlap.
- Barriers went from 38k to 14k per 2 s.
- Label latency went from 11 ms to 3.5 ms, and GPU queue depth from 4 to 2.6.
- No glitches were visible in the screenshot.
- **FPS was unchanged** (34–36 frames per 2 s).

### GPU timeline (timestamp queries per submission)

| | Baseline | Experiment |
|---|---|---|
| GPU busy, per 2 s | 1.59 s (80%) | 1.36 s (68%) |
| Idle gaps, per 2 s | 0.43 s | 0.65 s |
| Max submission span | 7.5 ms | 5.3 ms |

- Drains cost about 14% of GPU time, but removing them only turned busy time into idle time.
- Game:Main still spins about 92% in the same label poll (`0x1695e0`). The wait moved from the main-loop site to the frame-start site (`0x3edef0`, object `0x7d3cf10`), so it looks like a per-frame fence. The game builds frame N+1 only after the GPU has finished an earlier frame, so CPU build time and GPU time add up in series.
- That means about 39–47 ms of GPU execution per 720p frame, insensitive to the shader clock.

### GPU sysfs during gameplay
- sclk 1040 MHz; mclk and fclk at 800 MHz. 800 MHz is the top level on the OLED, so memory clock is not throttled.
- APU power about 15 W.
- VRAM 949 of 1024 MiB used; GTT about 5.8 GB.

### Next
- Find what fills the 39–47 ms of GPU time per frame. Planned: per-draw/dispatch bottom-of-pipe timestamps aggregated by pipeline hash. The alternative is an RGP capture via `MESA_VK_TRACE=rgp`.
- Suspects: about 750 tiny compute dispatches per frame (launch- or latency-bound), and memory placement (guest memory in snooped GTT versus a full 1 GiB VRAM carve-out).

### GPU time by work type (SHAD_GPU_TIME probe, idle gameplay, mode 3)
Measured per 4 s (about 67 frames) by incremental-completion timestamps around each render pass and each dispatch. Total is about 44 ms of GPU per frame (about 23 FPS worth), against about 58 ms frames.

| Work | ms per frame | Notes |
|---|---|---|
| **2048×2048 depth + 1 color (shadow map)** | **~15** | About 2,700 draws per frame, split over 21 render passes per frame; about 5.6 µs per draw |
| 1280×720, 1 color, no depth (post) | ~6.4 | About 17 fullscreen passes per frame |
| 1280×720 G-buffer (5 color + depth) | ~4.8 | About 600 draws per frame |
| 1280×720, 1 color + depth | ~1.8 | 89 passes per frame, about 2 draws each |
| Compute | ~9.6 | About 790 dispatches per frame, 161 shaders, the largest about 1.5 ms per frame |

**Conclusions so far**
- The frame is GPU-bound (about 44 ms per frame), and the GPU time is in many small draws, render-pass restarts and drains, not shader math. That is consistent with the GPU clock not mattering.
- The CPU side (Game:Main) only waits.

**Candidate emulator-level work** (no game-specific hacks):
1. Follow guest sync semantics for compute WAW/WAR hazards instead of inferring a drain per dispatch. This saves about 14% of GPU time, but needs proper tracking of access origin, so that emulator uploads/copies keep their barriers.
2. Reduce render-pass restarts in the shadow pass: why 21 per frame? Find what interrupts them (image transitions, dispatches or barriers between draws).
3. Lower the per-draw GPU cost: pipeline and descriptor changes per draw, and dynamic state.

The probe code is in `claude/diagnostic-probes.patch` (local); apply it with `git apply` to reproduce. The env vars are `SHAD_LABEL_PROBE`, `SHAD_TRUST_GUEST_SYNC` and `SHAD_GPU_TIME`.

### Shadow pass splits: ruled out as the cost
- The 2048² shadow pass starts about 21 times per frame. About a third are resumes of identical targets.
- What ends a pass, per 4 s:
  - 938 × the next draw needs a different render state;
  - 458 × an emulator submit mid-pass (the `flush_draw_interval` / `ring_drain_flush_draws` early flushes);
  - about 17 × copies and dispatches.
- A/B of the early-flush settings:

| Setting | Frames / 2 s | GPU busy / 2 s | Shadow passes / 4 s | Shadow GPU time / 4 s |
|---|---|---|---|---|
| Current (384 / 64) | 33.6 | 1624 ms | 1411 | 1016 ms |
| Ring drain off | 33.5 | 1629 ms | 1397 | 999 ms |
| Both off | 33.0 | 1592 ms | 958 | 994 ms |

- Conclusion: the pass splits cost about 2%. Keep the current settings.
- The shadow cost is per draw: about 2,700 draws per frame at about 5.6 µs of GPU each.
- Next suspects: per-draw GPU-side overhead (pipeline switches, descriptor/push-constant churn, vertex fetch done in the shader) and context rolls.

### Texture flicker (user-reported, 2026-10-10)
- Symptom: a brief "refocus" on the building window frames (middle and left of the Fish Cannery view), every 2–10 s, sometimes lasting about 0.2 s.
- Seen during runs of today's profiling builds. Whether the official 3834b34 build also has it is not yet confirmed: a 40 s x11grab capture of the CI build showed nothing, but the capture may simply have missed it.
- The GC hypothesis (GPU-modified textures freed before their async download lands) was **not** supported. Only one non-GPU-modified 128×128/64×64 pair thrashes, every 10–25 s; GPU-modified frees only happen at load. Probe: `claude/gc-probe.patch`.

### `guest_sync_barriers` (committed locally as 9103c21, not pushed)
- New GPU setting `guest_sync_barriers` (default off). A guest shader write that overlaps only accesses made by guest draws/dispatches since the last guest sync packet (partial flushes, cache flushes, ACQUIRE_MEM, waits, EOP/EOS signals) gets no barrier. RAW and every emulator transfer keep their barriers.
- Timed FPS, same scene, Release build, 6 windows of 300 frames: **off 17.14, on 17.80 (+3.9%)**. About 41k syncs and 405k relaxed writes per 300 frames.
- Not yet enabled in the user's config: waiting for the user to see it running without problems.
- FPS method: `infamous-fps <label> <binary> [key=value...]` (`~/.local/bin`, recovered from the old session) temporarily overrode config.json keys, ran unattended, timed the per-300-frame telemetry, and restored the config (`profiles/config.json.orig` is the backup).

### Per-draw shadow timing (SHAD_GPU_TIME, probe in `claude/shadow-draw-probe.patch`)
- 2048² shadow pass: about 1040 ms of GPU per 4 s; timestamps around each draw add up to about 690 ms over about 188k draws (about 3.7 µs each).
- Top pipelines by GPU time per 4 s: 186 ms (27.7k draws), 163 ms (64k draws), 123 ms (only 272 draws, so heavy draws), 51 ms, 47 ms. The cost is spread over a few pipelines with many small draws.

### APU clocks and power (gameplay, Release build)
- slowPPT cap 15 W (firmware max 29 W). Measured total 12–15 W.
- **All 8 CPU threads at 3.5 GHz (max)** even though Game:Main only spins and the workers are light.
- GPU at 1040 MHz, 99–100% busy, not clocking higher.
- This seems to contradict the earlier "1600 MHz = same FPS" note: if the GPU is the bottleneck and 99% busy, a real 1600 MHz clock should help, unless that test never actually reached 1600 MHz under load.
- **Pending user action:** set Steam ⚡ Performance → Manual GPU Clock Control → 1600 MHz, then rerun the timed FPS test and check in sysfs (`freq1_input`) that the GPU really holds 1600 MHz. The spinning CPU threads may be eating the power budget; a spin that yields (e.g. `pause`/usleep) could free power for the GPU.

### Working tree state at 2026-10-10 ~22:05
- Uncommitted: the shadow-draw probe in `vk_scheduler.*` and `vk_rasterizer.cpp` (diagnostic only, never commit). Saved as `claude/shadow-draw-probe.patch`; `git checkout -- src` drops it.
- Installed game build: CI 3834b34 (`infamous-build status`). Local builds: `build/` = Release with 9103c21, `build-prof/` = probe build.

### GPU clock lock from Desktop Mode (2026-10-10 ~22:15)
- Game Mode's Manual GPU Clock is not available in Desktop Mode, but `/usr/bin/steamos-polkit-helpers/steamos-priv-write <sysfs path> <value>` works without a password (polkit allow_active=yes). It is the same helper Game Mode uses. Claude Code's auto mode blocks Claude from calling it, so the user runs it with `!`.
- Lock: `P=/sys/class/drm/card0/device; W=/usr/bin/steamos-polkit-helpers/steamos-priv-write; $W $P/power_dpm_force_performance_level manual; $W $P/pp_od_clk_voltage "s 0 1600"; $W $P/pp_od_clk_voltage "s 1 1600"; $W $P/pp_od_clk_voltage c`
- **Revert:** `$W $P/pp_od_clk_voltage r; $W $P/pp_od_clk_voltage c; $W $P/power_dpm_force_performance_level auto` (a reboot also resets it).
- Status: waiting for the user to run the lock; then rerun `infamous-fps` off/on with `build/shadps4` and check `freq1_input`.
- Wrapper written: `~/.local/bin/deck-gpu-clock lock <200-1600> | auto | status` (only touches the GPU clock, via steamos-priv-write). Claude cannot chmod it or allow it for itself (auto mode blocks self-granted permissions); the user has to run `chmod +x` and add a Bash allow rule for it.
- `deck-gpu-clock` is approved and works autonomously (user ran chmod and allowed it). Always `deck-gpu-clock auto` after a test.

### Result: GPU clock barely matters (1600 vs 1040 MHz, 2026-10-10 ~22:30)
`infamous-fps`, Release build `build/` (9103c21), same scene. Sampler log: `profiles/clock-1600.txt`.

| GPU clock | guest_sync_barriers off | on |
|---|---|---|
| auto (sits at 1040) | 17.14 | 17.80 |
| locked 1600 | 17.93 (+4.6%) | 17.80 (+0%) |

- The clock really held 1600 MHz under load (avg sclk 1600), with total power about 14.8 W, right at the 15 W cap. GPU busy was still about 99–100%.
- A 54% higher shader clock gives at most about 4.6% FPS, and nothing on top of guest_sync_barriers. So the GPU is "busy" but not shader-bound: it is stalled on memory/fabric (mclk is already at its 800 MHz max on shared LPDDR5) or on serialization (drains, tiny draws, idle waits that count as busy).
- The earlier note "1600 = same FPS" holds. Raising the clock is not the lever; cutting memory traffic and GPU-side serialization is.
- Next candidates: memory traffic per frame (shadow map 2048² D32 + color, guest memory in GTT versus VRAM, tiling/compression of render targets), and whether the always-3.5 GHz spinning CPU threads steal power and fabric bandwidth from the GPU (try a yielding spin or a lower CPU clock cap and measure).

### 30 fps push (2026-10-10 ~22:40): game-side patches
- The user asked for 30 fps fast. Levers: the game's own patches in `~/.local/share/shadPS4/patches/shadPS4/InfamousSecondSon.xml` (backup at `profiles/InfamousSecondSon.xml.orig`).
- Enabled the motion blur, DOF and depth blur off switches; added a disabled "Render resolution 960x540" variant of the 720p patch (only the 1280/720 values changed).
- Results (Release 9103c21, guest_sync_barriers on, GPU 1600): post-FX off 17.99 fps (vs 17.80); post-FX off + 960x540 18.01 fps. **Resolution and post-FX don't move FPS**, so the ~18 fps cap is serial/latency-bound, not GPU pixel work. Patch file left at 720p with the post-FX-off patches enabled. Paused at the user's request.
- **Post-FX patches removed fire** (user report): the motion blur patch also skips an "unidentified post chain", which was evidently fire. Restored the original patch file. Don't enable those patches.
- inplace_guest_memory 0: 14.37 fps (vs 17.80 in mode 3). In-place memory is a net win; snooped GTT is not the main cost.
- `infamous-fps` shortened at the user's request: 1 warm-up + 3 windows of 300 frames (about 90 s plus load).
- Feedback-loop usage off (DCC hypothesis): 17.53, rejected. CPU profile of GpuComm (53%, flat) and GpuSche (66%, mostly memcpy): emulator CPU side is not the limit.
- **Work is now organized in `agent/` (local, git-excluded): STATUS.md, ideas/BACKLOG.md, experiments/NNN-*.md, results.csv, TOOLS.md.** Read `agent/STATUS.md` first when resuming.

### Critical path and GPU counters (2026-10-10 ~23:40; details in local `agent/experiments/007–010`)
- Frame trace: decode→vkQueueSubmit <1 ms; submit→GPU done ~18–20 ms average. GPU queue backlog is the chain's limit.
- HW counters (VK_KHR_performance_query): GPU active ~72%, VALU busy ~22%, DRAM read 11.7 GB/s (peak 89). GPU stalls, not work.
- Microbenchmark: snooped GTT, WC GTT and VRAM carve-out read identically on the Deck APU (89 GB/s streaming, 8 GB/s scattered). In-place guest memory is not the cost.
- ACO stats: no spills, median 40 VGPRs; occupancy not the issue.
- Next: front-end/fixed-function stalls — barrier model, render-pass breaks, per-draw state. Diagnostics patch: `claude/diagnostics-2.patch`.

### Latency-bound GPU (2026-10-11 ~00:15; local details in `agent/experiments/011–013`)
- Front-end timestamp trace + barrier reduction A/B: barriers are where the GPU waits, not what costs. Cutting ~35% of image barriers gave no fps.
- gpu_metrics: GFX 2.2 W "busy" vs CPU 6–7 W of a 12 W socket; 1600 MHz triggers PPT throttling. GPU work is latency-bound (DRAM traffic in the scattered-access regime).
- No GS emulation (only 13 tess pairs). Next: instruction-level profiling (RGP/SQTT) to find which work is latency-bound.
- Known visual issues parked: window-frame "refocus" flicker; particle/debris ghosting.
- 2026-10-11: play build is now local eddff94 with `guest_sync_barriers` on (17.75 fps verified). Decode-time fences with in-place memory → device lost (rejected).
- Image↔buffer transfers (bbport's Deck bottleneck): ~10 async downloads/frame (27 MB), ~1% of GPU time, zero CP waits. Not our bottleneck. Patch: `claude/copy-stats.patch`.
