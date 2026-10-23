// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/types.h"

// Sub-pixel viewport jitter of the scene draws. A plan armed at one Antialias dispatch takes effect
// after the next Composite draw, so only the scene draws of the next frame are jittered. The window
// opens at the first draw into the scene depth-stencil image and closes at the next Antialias
// dispatch. GPU command thread only.
namespace Gr2Fsr3::Jitter {

/// Offset in render pixels: +x right, +y down.
struct Offset {
    float x{};
    float y{};
};

/// Jitter for the scene draws of the next frame.
struct FramePlan {
    bool enabled{};
    Offset offset{};
    float lod_bias{};              ///< Added to the samplers of jittered draws; 0 adds nothing.
    VAddr scene_stencil_address{}; ///< Stencil plane of the scene depth-stencil image.
};

/// What the draws of a frame received, collected until the Antialias dispatch.
struct FrameReport {
    bool window_opened{};
    bool extent_hint{}; ///< The DB extent hint was valid when the window opened.
    Offset applied{};   ///< The plan offset when at least one draw was jittered, else zero.
    u32 eligible_draws{};
    u32 jittered_draws{};
    u32 indirect_draws{};
    u32 fullscreen_draws{};
    u32 num_vertex_hashes{};
    std::array<u64, 16> vertex_hashes{}; ///< Distinct vertex shaders of the jittered draws.
};

/// Length of the FSR 3.1 jitter sequence for a render and output width.
s32 PhaseCount(u32 render_width, u32 output_width);

/// FSR 3.1 jitter offset of a frame index: Halton(2, 3) of (index % phase_count) + 1, minus 0.5.
Offset PhaseOffset(s32 index, s32 phase_count);

/// Sets the device maxSamplerLodBias, which bounds the negative LOD bias of jittered draws.
void SetMaxLodBias(float max_lod_bias);

/// Replaces the plan; it takes effect after the next Composite draw.
void BeginFrame(const FramePlan& plan);

/// Closes the window and returns what the draws since the last call received.
FrameReport EndFrame();

} // namespace Gr2Fsr3::Jitter
