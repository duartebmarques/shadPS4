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

// FSR 3.1 and FSR 4 (INT8, experimental) temporal upscaling and anti-aliasing for Gravity Rush 2
// and Gravity Rush Remastered. Every function runs on the GPU command thread except DrawMenu and
// DrawDlssMenu, which run on the ImGui thread. For other titles a call costs one load and a few
// compares.
namespace Gr2Fsr3 {

#ifdef ENABLE_GR2_FSR3

/// Bit of the dynamic-state memo flags that marks a jittered draw.
constexpr u32 DynFlagJittered = 1u << 3;

namespace Detail {

enum class TitleState : u8 {
    Unresolved,
    Inactive,
    Gr2,
    Grr,
};

extern TitleState g_title_state;
extern bool g_draw_jittered;
extern float g_jitter_x;
extern float g_jitter_y;
extern float g_lod_bias;

TitleState ResolveTitle();
bool OnDispatchActive(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                      Vulkan::Rasterizer& rasterizer);
void OnDrawActive(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                  bool is_indirect);
void AdjustSamplerActive(AmdGpu::Sampler& ssharp, const Shader::Info& stage);
void OnGrrDispatch(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                   Vulkan::Rasterizer& rasterizer);
bool OnGrrDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
               Vulkan::Rasterizer& rasterizer, bool is_indirect);

inline TitleState Title() {
    if (g_title_state == TitleState::Unresolved) [[unlikely]] {
        return ResolveTitle();
    }
    return g_title_state;
}

} // namespace Detail

/// Observes or replaces a direct compute dispatch. Returns true when the guest dispatch is skipped.
inline bool OnDispatch(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                       Vulkan::Rasterizer& rasterizer) {
    switch (Detail::Title()) {
    case Detail::TitleState::Gr2:
        return Detail::OnDispatchActive(info, cs_program, rasterizer);
    case Detail::TitleState::Grr:
        Detail::OnGrrDispatch(info, cs_program, rasterizer);
        return false;
    default:
        return false;
    }
}

/// Decides whether the draw being set up receives the sub-pixel jitter of the frame, and records
/// the module's replacement of a draw it takes over. Returns true when the guest draw is skipped.
inline bool OnDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                   Vulkan::Rasterizer& rasterizer, bool is_indirect) {
    switch (Detail::Title()) {
    case Detail::TitleState::Gr2:
        Detail::OnDrawActive(pipeline, liverpool, is_indirect);
        return false;
    case Detail::TitleState::Grr:
        return Detail::OnGrrDraw(pipeline, liverpool, rasterizer, is_indirect);
    default:
        return false;
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

/// Draws the "FSR 3.1 (GR2)" or "FSR 3.1 (GRR)" submenu of the Display menu.
void DrawMenu();

/// Draws the "DLSS (GR2)" or "DLSS (GRR)" menu of the menu bar, on Windows only.
void DrawDlssMenu();

#else

inline bool OnDispatch(const Shader::Info&, const AmdGpu::ComputeProgram&, Vulkan::Rasterizer&) {
    return false;
}

inline bool OnDraw(const Vulkan::GraphicsPipeline&, const AmdGpu::Liverpool&, Vulkan::Rasterizer&,
                   bool) {
    return false;
}

inline void JitterViewport(float&, float&) {}

inline u32 DynFlagBits() {
    return 0u;
}

inline void AdjustSampler(AmdGpu::Sampler&, const Shader::Info&) {}

inline void DrawMenu() {}

inline void DrawDlssMenu() {}

#endif

} // namespace Gr2Fsr3
