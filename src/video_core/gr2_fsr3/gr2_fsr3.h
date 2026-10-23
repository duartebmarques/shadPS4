// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace AmdGpu {
struct ComputeProgram;
struct Liverpool;
struct Sampler;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {
class GraphicsPipeline;
class Rasterizer;
} // namespace Vulkan

// FSR 3.1 and FSR 4 (INT8, experimental) temporal upscaling for Gravity Rush 2. Every function runs
// on the GPU command thread except DrawMenu, which runs on the ImGui thread. For other titles a
// call costs one load and a branch.
namespace Gr2Fsr3 {

#ifdef ENABLE_GR2_FSR3

/// Bit of the dynamic-state memo flags that marks a jittered draw.
constexpr u32 DynFlagJittered = 1u << 3;

namespace Detail {

enum class TitleState : u8 {
    Unresolved,
    Inactive,
    Active,
};

extern TitleState g_title_state;
extern bool g_draw_jittered;
extern float g_jitter_x;
extern float g_jitter_y;
extern float g_lod_bias;

bool ResolveTitle();
bool OnDispatchActive(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                      Vulkan::Rasterizer& rasterizer);
void OnDrawActive(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                  bool is_indirect);
void AdjustSamplerActive(AmdGpu::Sampler& ssharp, const Shader::Info& stage);

inline bool IsActive() {
    if (g_title_state == TitleState::Unresolved) [[unlikely]] {
        return ResolveTitle();
    }
    return g_title_state == TitleState::Active;
}

} // namespace Detail

/// Observes or replaces a direct compute dispatch. Returns true when the guest dispatch is skipped.
inline bool OnDispatch(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                       Vulkan::Rasterizer& rasterizer) {
    return Detail::IsActive() && Detail::OnDispatchActive(info, cs_program, rasterizer);
}

/// Decides whether the draw being set up receives the sub-pixel jitter of the frame.
inline void OnDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                   bool is_indirect) {
    if (Detail::IsActive()) {
        Detail::OnDrawActive(pipeline, liverpool, is_indirect);
    }
}

/// Moves the origin of a clip-enabled viewport of a jittered draw by the jitter.
inline void JitterViewport(float& x, float& y) {
    if (Detail::g_draw_jittered) {
        x += Detail::g_jitter_x;
        y += Detail::g_jitter_y;
    }
}

/// Memo flag bits of the draw, which key the dynamic-state memos on whether it is jittered.
inline u32 DynFlagBits() {
    return Detail::g_draw_jittered ? DynFlagJittered : 0u;
}

/// Adds the upscale LOD bias to a sampler of a jittered draw.
inline void AdjustSampler(AmdGpu::Sampler& ssharp, const Shader::Info& stage) {
    if (Detail::g_draw_jittered && Detail::g_lod_bias != 0.0f) {
        Detail::AdjustSamplerActive(ssharp, stage);
    }
}

/// Draws the "FSR 3.1 (GR2)" submenu of the Display menu.
void DrawMenu();

#else

inline bool OnDispatch(const Shader::Info&, const AmdGpu::ComputeProgram&, Vulkan::Rasterizer&) {
    return false;
}

inline void OnDraw(const Vulkan::GraphicsPipeline&, const AmdGpu::Liverpool&, bool) {}

inline void JitterViewport(float&, float&) {}

inline u32 DynFlagBits() {
    return 0u;
}

inline void AdjustSampler(AmdGpu::Sampler&, const Shader::Info&) {}

inline void DrawMenu() {}

#endif

} // namespace Gr2Fsr3
