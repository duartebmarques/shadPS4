// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <utility>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/gr2_fsr3/fsr3_jitter.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/gr2_fsr3/gr2_fsr3.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Gr2Fsr3 {

namespace Detail {
bool g_draw_jittered{};
float g_jitter_x{};
float g_jitter_y{};
float g_lod_bias{};
} // namespace Detail

namespace {

Jitter::FramePlan g_pending{};
bool g_has_pending{};
Jitter::FramePlan g_plan{};
bool g_armed{};
bool g_window_open{};
bool g_window_logged{};
u32 g_scene_extent_hint{};
u32 g_scene_pitch{};
u32 g_scene_height{};
Jitter::FrameReport g_report{};
s32 g_min_lod_bias{-8192};

float Halton(s32 index, s32 base) {
    float scale = 1.0f;
    float result = 0.0f;
    for (s32 i = index; i > 0; i /= base) {
        scale /= static_cast<float>(base);
        result += scale * static_cast<float>(i % base);
    }
    return result;
}

void AddVertexHash(u64 vs_hash) {
    auto& hashes = g_report.vertex_hashes;
    const u32 count = g_report.num_vertex_hashes;
    if (count < hashes.size() &&
        !std::ranges::contains(hashes.begin(), hashes.begin() + count, vs_hash)) {
        hashes[count] = vs_hash;
        ++g_report.num_vertex_hashes;
    }
}

} // namespace

namespace Jitter {

s32 PhaseCount(u32 render_width, u32 output_width) {
    const float ratio = static_cast<float>(output_width) / static_cast<float>(render_width);
    return static_cast<s32>(8.0f * std::pow(ratio, 2.0f));
}

s32 Fsr4PhaseCount(u32 render_width, u32 output_width) {
    const float ratio = static_cast<float>(output_width) / static_cast<float>(render_width);
    return std::clamp(static_cast<s32>(std::ceil(8.0f * ratio * ratio)), 1, 256);
}

Offset PhaseOffset(s32 index, s32 phase_count) {
    const s32 i = (index % phase_count) + 1;
    return {.x = Halton(i, 2) - 0.5f, .y = Halton(i, 3) - 0.5f};
}

void SetMaxLodBias(float max_lod_bias) {
    g_min_lod_bias = -static_cast<s32>(std::min(max_lod_bias * 256.0f, 8192.0f));
}

void BeginFrame(const FramePlan& plan) {
    g_pending = plan;
    g_has_pending = true;
}

FrameReport EndFrame() {
    g_window_open = false;
    g_armed = false;
    Detail::g_draw_jittered = false;
    FrameReport report = std::exchange(g_report, {});
    if (report.jittered_draws > 0) {
        report.applied = g_plan.offset;
    }
    return report;
}

bool StartFrame() {
    g_plan = std::exchange(g_has_pending, false) ? g_pending : FramePlan{};
    g_armed = g_plan.enabled;
    g_window_open = false;
    g_report = {};
    Detail::g_jitter_x = g_plan.offset.x;
    Detail::g_jitter_y = g_plan.offset.y;
    Detail::g_lod_bias = g_plan.lod_bias;
    return g_armed;
}

bool GrrEligible(const AmdGpu::Liverpool& liverpool, VAddr scene_depth) {
    const auto& regs = liverpool.regs;
    const auto& db = regs.depth_buffer;
    return db.DepthValid() && db.DepthAddress() == scene_depth && regs.depth_control.depth_enable &&
           regs.depth_control.depth_func != AmdGpu::CompareFunc::Always && !regs.IsClipDisabled();
}

void OnGrrDraw(const AmdGpu::Liverpool& liverpool, u64 vertex_hash, bool is_indirect) {
    Detail::g_draw_jittered = false;
    if (!g_armed || !GrrEligible(liverpool, g_plan.scene_depth_address)) {
        return;
    }
    if (!g_window_open) {
        g_window_open = true;
        g_report.window_opened = true;
        if (!std::exchange(g_window_logged, true)) {
            LOG_INFO(Render_Vulkan, "GRR FSR: jitter window opens at depth {:#x}",
                     g_plan.scene_depth_address);
        }
    }
    ++g_report.eligible_draws;
    g_report.indirect_draws += is_indirect;
    Detail::g_draw_jittered = true;
    ++g_report.jittered_draws;
    AddVertexHash(vertex_hash);
}

} // namespace Jitter

namespace Detail {

void OnDrawActive(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                  bool is_indirect) {
    g_draw_jittered = false;
    const auto stages = pipeline.GetStages();
    if (g_has_pending) {
        const Shader::Info* ps = stages[static_cast<u32>(Shader::SwStage::Fragment)];
        if (ps && (ps->pgm_hash == Hash::CompositePs || ps->pgm_hash == Hash::CompositeLensPs)) {
            g_plan = g_pending;
            g_has_pending = false;
            g_armed = g_plan.enabled;
            g_window_open = false;
            g_jitter_x = g_plan.offset.x;
            g_jitter_y = g_plan.offset.y;
            g_lod_bias = g_plan.lod_bias;
            return;
        }
    }
    if (!g_armed) {
        return;
    }
    const auto& db = liverpool.regs.depth_buffer;
    if (!db.DepthValid() || db.z_info.format != AmdGpu::DepthBuffer::ZFormat::Z32Float ||
        db.NumSamples() != 1) {
        return;
    }
    const auto& hint = liverpool.last_db_extent;
    if (!g_window_open) {
        if (!db.StencilValid() || db.StencilAddress() != g_plan.scene_stencil_address) {
            return;
        }
        g_window_open = true;
        g_report.window_opened = true;
        g_report.extent_hint = hint.Valid();
        g_scene_extent_hint = hint.raw;
        g_scene_pitch = db.Pitch();
        g_scene_height = db.Height();
        if (!g_window_logged) {
            g_window_logged = true;
            LOG_INFO(Render_Vulkan,
                     "GR2 FSR3: jitter window opens at stencil {:#x}: pitch {}, height {}, "
                     "extent hint {} ({}x{})",
                     g_plan.scene_stencil_address, g_scene_pitch, g_scene_height, hint.Valid(),
                     hint.width, hint.height);
        }
    }
    // Depth images are sized by the DB extent hint when present, else by pitch and height.
    const bool scene_extent =
        hint.raw == g_scene_extent_hint &&
        (hint.Valid() || (db.Pitch() == g_scene_pitch && db.Height() == g_scene_height));
    if (!scene_extent || liverpool.regs.IsClipDisabled()) {
        return;
    }
    ++g_report.eligible_draws;
    g_report.indirect_draws += is_indirect;
    const Shader::Info* vs = stages[static_cast<u32>(Shader::SwStage::Vertex)];
    const u64 vs_hash = vs ? vs->pgm_hash : 0;
    if (vs_hash == Hash::FullscreenVs) {
        ++g_report.fullscreen_draws;
        return;
    }
    g_draw_jittered = true;
    ++g_report.jittered_draws;
    AddVertexHash(vs_hash);
}

void AdjustSamplerActive(AmdGpu::Sampler& ssharp, const Shader::Info& stage) {
    // A dispatch sees the flag of the last draw; only graphics stages take the bias.
    if (stage.sw_stage == Shader::SwStage::Compute) {
        return;
    }
    // The S# LOD bias is a 14-bit two's complement value in 1/256 steps. Vulkan bounds its
    // magnitude by maxSamplerLodBias, which the sampler applies to positive biases only.
    const auto bias = static_cast<s32>(std::lround((ssharp.LodBias() + g_lod_bias) * 256.0f));
    ssharp.lod_bias.Assign(static_cast<u64>(std::clamp(bias, g_min_lod_bias, 8191)));
}

} // namespace Detail

} // namespace Gr2Fsr3
