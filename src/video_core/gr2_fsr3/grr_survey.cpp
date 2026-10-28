// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/ranges.h>
#include <magic_enum/magic_enum.hpp>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/gr2_fsr3/fsr3_jitter.h"
#include "video_core/gr2_fsr3/grr_survey.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Gr2Fsr3 {

namespace {

constexpr u32 CensusDraws = 3000;
constexpr u32 MaxOrderLogs = 8;
// The draws into the scene depth are surveyed for about five minutes at 60 fps, which bounds the
// cost of searching their distinct values.
constexpr u64 SurveyFrames = 18000;

// Passes whose order within a frame is logged.
enum class Event : u8 {
    Exposure,
    Camera,
    Composite,
    CopyLuma,
    Fxaa,
};
constexpr std::array<std::string_view, 5> EventNames{
    "exposure", "camera", "composite", "CopyLuma", "FXAA",
};
static_assert(EventNames.size() == magic_enum::enum_count<Event>());

// The events of a frame, repeats in a row merged. Entries past count stay value-initialized, so
// the defaulted compare compares the sequences.
struct EventList {
    std::array<Event, 12> events{};
    u32 count{};

    void Add(Event event) {
        if (count < events.size() && (count == 0 || events[count - 1] != event)) {
            events[count++] = event;
        }
    }

    bool operator==(const EventList&) const = default;
};

struct StencilWrite {
    u64 vs{};
    u64 gs{};
    u64 ps{};
    u8 ref{};
    AmdGpu::CompareFunc func{};
    u8 write_mask{};
    AmdGpu::StencilFunc pass_op{};

    bool operator==(const StencilWrite&) const = default;
};

} // namespace

struct GrrSurvey::Impl {
    void Census(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Regs& regs,
                const GrrDrawHashes& hashes);
    void LogComposite(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Regs& regs,
                      const GrrDrawHashes& hashes);
    void LogSceneDepthDraw(const AmdGpu::Liverpool& liverpool, const GrrDrawHashes& hashes,
                           bool depth_tested);
    void FinishFrame();

    // The census starts at the first depth-tested draw, so it covers 3D frames rather than the
    // boot screens.
    bool census_started{};
    u32 census_draws{};
    u32 camera_draws{};
    u32 copyluma_draws{};
    u32 fxaa_draws{};
    u32 composite_draws{};
    u32 motion_blur_draws{};
    u32 exposure_dispatches{};
    BoundedSet<u64, 16> small_rgba8_ps;

    u64 frame{};
    EventList events{};
    EventList logged_events{};
    u32 order_logs{};
    bool fxaa_drawn{};
    VAddr fxaa_target{};
    u32 after_draws{};
    u32 after_target_draws{};
    u32 after_depth_tested{};
    BoundedSet<u64, 16> after_vs;
    bool logged_after{};
    bool warned_after{};

    BoundedSet<u64, 64> taken_vs;
    BoundedSet<u64, 32> left_vs;
    BoundedSet<StencilWrite, 32> stencil_writes;
    BoundedSet<std::array<u64, 2>, 16> composites;
    BoundedSet<VAddr, 6> fxaa_targets;
    bool logged_motion_blur{};
};

void GrrSurvey::Impl::Census(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Regs& regs,
                             const GrrDrawHashes& hashes) {
    const auto& db = regs.depth_buffer;
    const bool depth_tested = regs.depth_control.depth_enable && db.DepthValid() &&
                              db.z_info.format == AmdGpu::DepthBuffer::ZFormat::Z32Float;
    census_started = census_started || depth_tested;
    if (!census_started || census_draws == CensusDraws) {
        return;
    }
    camera_draws += hashes.vs == GrrHash::CameraVs;
    copyluma_draws += hashes.ps == GrrHash::CopyLumaPs;
    fxaa_draws += hashes.ps == GrrHash::FxaaPs;
    composite_draws += IsComposite(hashes);
    motion_blur_draws += hashes.ps == GrrHash::MotionBlurPs;
    if (pipeline.GetGraphicsKey().mrt_mask == 1 &&
        regs.color_buffers[0].GetDataFmt() == AmdGpu::DataFormat::Format8_8_8_8 &&
        regs.num_indices <= 6) {
        small_rgba8_ps.Insert(hashes.ps);
    }
    if (++census_draws < CensusDraws) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "GRR FSR: in the first {} draws from the first depth-tested draw: camera draws {}, "
             "CopyLuma {}, FXAA {}, composite {}, motion blur {}, exposure dispatches {}",
             CensusDraws, camera_draws, copyluma_draws, fxaa_draws, composite_draws,
             motion_blur_draws, exposure_dispatches);
    if (fxaa_draws == 0 || camera_draws == 0) {
        LOG_WARNING(Render_Vulkan,
                    "GRR FSR: pixel shaders of single-target RGBA8 draws of at most 6 indices: "
                    "[{:#x}]",
                    fmt::join(small_rgba8_ps.Keys(), ", "));
    }
}

void GrrSurvey::Impl::LogComposite(const Vulkan::GraphicsPipeline& pipeline,
                                   const AmdGpu::Regs& regs, const GrrDrawHashes& hashes) {
    const auto& target = regs.color_buffers[0];
    if (!composites.Insert({hashes.ps, target.Address()})) {
        return;
    }
    std::array<VAddr, 4> reads{};
    std::size_t num_reads = 0;
    const auto stages = pipeline.GetStages();
    if (const Shader::Info* ps = stages[static_cast<u32>(Shader::SwStage::Fragment)]) {
        for (const Shader::ImageResource& res : ps->images) {
            if (!res.is_written && num_reads < reads.size()) {
                reads[num_reads++] = res.GetSharp(*ps).Address();
            }
        }
    }
    LOG_INFO(Render_Vulkan, "GRR FSR: composite PS {:#x} -> {:#x} {} {} swap {} reads [{:#x}]",
             hashes.ps, target.Address(), target.GetDataFmt(), target.GetNumberFmt(),
             magic_enum::enum_name(target.info.comp_swap),
             fmt::join(std::span{reads.data(), num_reads}, ", "));
}

void GrrSurvey::Impl::LogSceneDepthDraw(const AmdGpu::Liverpool& liverpool,
                                        const GrrDrawHashes& hashes, bool depth_tested) {
    const auto& regs = liverpool.regs;
    const auto& control = regs.depth_control;
    if (depth_tested) {
        if (taken_vs.Insert(hashes.vs)) {
            LOG_INFO(Render_Vulkan,
                     "GRR FSR: jitter predicate takes VS {:#x} PS {:#x} depth func {}", hashes.vs,
                     hashes.ps, magic_enum::enum_name(control.depth_func));
        }
    } else if (left_vs.Insert(hashes.vs)) {
        LOG_INFO(Render_Vulkan,
                 "GRR FSR: jitter predicate leaves out VS {:#x} PS {:#x}: depth test {} func {} "
                 "clip disabled {}",
                 hashes.vs, hashes.ps, control.depth_enable != 0,
                 magic_enum::enum_name(control.depth_func), regs.IsClipDisabled());
    }

    const auto& front = regs.stencil_ref_front;
    const auto& ops = regs.stencil_control;
    const AmdGpu::StencilFunc pass_op = ops.stencil_zpass_front;
    if (!control.stencil_enable || !regs.depth_buffer.StencilValid() ||
        front.stencil_write_mask == 0 || pass_op == AmdGpu::StencilFunc::Keep) {
        return;
    }
    const StencilWrite write{
        .vs = hashes.vs,
        .gs = hashes.gs,
        .ps = hashes.ps,
        .ref = front.stencil_test_val,
        .func = control.stencil_ref_func,
        .write_mask = front.stencil_write_mask,
        .pass_op = pass_op,
    };
    if (stencil_writes.Insert(write)) {
        LOG_INFO(Render_Vulkan,
                 "GRR FSR: stencil write ref {:#x} mask {:#x} write mask {:#x} op value {:#x} "
                 "func {} ops {}/{}/{} by VS {:#x} GS {:#x} PS {:#x}",
                 front.stencil_test_val, front.stencil_mask, front.stencil_write_mask,
                 front.stencil_op_val, magic_enum::enum_name(write.func),
                 magic_enum::enum_name(ops.stencil_fail_front), magic_enum::enum_name(pass_op),
                 magic_enum::enum_name(ops.stencil_zfail_front), hashes.vs, hashes.gs, hashes.ps);
    }
}

void GrrSurvey::Impl::FinishFrame() {
    if ((fxaa_drawn || order_logs > 0) && order_logs < MaxOrderLogs && events != logged_events) {
        ++order_logs;
        logged_events = events;
        std::string order;
        for (u32 i = 0; i < events.count; ++i) {
            order.append(i == 0 ? "" : ", ")
                .append(EventNames[static_cast<std::size_t>(events.events[i])]);
        }
        LOG_INFO(Render_Vulkan, "GRR FSR: frame {} order: {}, {} draws after FXAA", frame, order,
                 after_draws);
    }
    const bool first = fxaa_drawn && !logged_after;
    const bool warn = fxaa_drawn && after_depth_tested > 0 && !warned_after;
    if (first || warn) {
        logged_after = true;
        warned_after = warned_after || warn;
        LOG_GENERIC(Render_Vulkan, warn ? Common::Log::Level::Warning : Common::Log::Level::Info,
                    "GRR FSR: after the FXAA draw: {} draws, {} into the FXAA target, {} "
                    "depth-tested against the scene depth, vertex shaders [{:#x}]",
                    after_draws, after_target_draws, after_depth_tested,
                    fmt::join(after_vs.Keys(), ", "));
    }
    events = {};
    fxaa_drawn = false;
    after_draws = 0;
    after_target_draws = 0;
    after_depth_tested = 0;
    after_vs = {};
}

GrrSurvey::GrrSurvey() : impl{std::make_unique<Impl>()} {}

GrrSurvey::~GrrSurvey() = default;

void GrrSurvey::OnDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                       const GrrDrawHashes& hashes, VAddr scene_depth) {
    Impl& state = *impl;
    const auto& regs = liverpool.regs;
    state.Census(pipeline, regs, hashes);
    if (hashes.vs == GrrHash::CameraVs) {
        state.events.Add(Event::Camera);
    }
    if (IsComposite(hashes)) {
        state.events.Add(Event::Composite);
        state.LogComposite(pipeline, regs, hashes);
    }
    if (hashes.ps == GrrHash::CopyLumaPs) {
        state.events.Add(Event::CopyLuma);
    }
    const auto& db = regs.depth_buffer;
    const bool scene_target = db.DepthValid() && db.DepthAddress() == scene_depth;
    const bool depth_tested = Jitter::GrrEligible(liverpool, scene_depth);
    if (hashes.ps == GrrHash::FxaaPs) {
        state.events.Add(Event::Fxaa);
        state.fxaa_drawn = true;
        state.fxaa_target = regs.color_buffers[0].Address();
    } else if (state.fxaa_drawn) {
        ++state.after_draws;
        state.after_target_draws += regs.color_buffers[0].Address() == state.fxaa_target;
        state.after_depth_tested += depth_tested;
        state.after_vs.Insert(hashes.vs);
    }
    if (scene_target && state.frame < SurveyFrames) {
        state.LogSceneDepthDraw(liverpool, hashes, depth_tested);
    }
    if (hashes.ps == GrrHash::MotionBlurPs && !std::exchange(state.logged_motion_blur, true)) {
        LOG_INFO(Render_Vulkan, "GRR FSR: the motion blur pass {:#x} runs (frame {})", hashes.ps,
                 state.frame);
    }
}

void GrrSurvey::OnFxaa(const GrrFxaaDraw& draw, bool recognized, const GrrSceneDepth& scene,
                       Vulkan::Rasterizer& rasterizer) {
    Impl& state = *impl;
    if (!recognized || !state.fxaa_targets.Insert(draw.target.Address())) {
        return;
    }
    // The descs the replacement looks the images up with; every image is read after the last
    // lookup.
    VideoCore::TextureCache& texture_cache = rasterizer.GetTextureCache();
    VideoCore::TextureCache::ImageDesc color_desc{draw.color.tsharp, draw.color.resource};
    VideoCore::TextureCache::ImageDesc target_desc{draw.target, draw.target_hint};
    const VideoCore::ImageId color_id = texture_cache.FindImage(color_desc);
    const VideoCore::ImageId target_id = texture_cache.FindImage(target_desc);
    std::string depth_text = "none";
    if (scene.valid) {
        VideoCore::TextureCache::ImageDesc depth_desc{scene.buffer, scene.view, scene.control,
                                                      scene.htile_address, scene.hint};
        const VideoCore::Image& depth = texture_cache.GetImage(texture_cache.FindImage(depth_desc));
        depth_text = fmt::format(
            "{} {}x{} stencil {}", vk::to_string(depth.backing->image.image_ci.format),
            depth.info.size.width, depth.info.size.height, depth.info.props.has_stencil != 0);
    }
    const VideoCore::Image& color = texture_cache.GetImage(color_id);
    const VideoCore::Image& target = texture_cache.GetImage(target_id);
    const vk::ImageCreateInfo& color_ci = color.backing->image.image_ci;
    const vk::ImageCreateInfo& target_ci = target.backing->image.image_ci;
    LOG_INFO(Render_Vulkan,
             "GRR FSR: FXAA images: input {} {}x{} layers {} levels {} usage {}, target {} {}x{} "
             "layers {} usage {} storage {}, scene depth {}",
             vk::to_string(color_ci.format), color.info.size.width, color.info.size.height,
             color_ci.arrayLayers, color_ci.mipLevels, vk::to_string(color_ci.usage),
             vk::to_string(target_ci.format), target.info.size.width, target.info.size.height,
             target_ci.arrayLayers, vk::to_string(target_ci.usage),
             static_cast<bool>(target_ci.usage & vk::ImageUsageFlagBits::eStorage), depth_text);
}

void GrrSurvey::OnFrameStart(bool exposure) {
    Impl& state = *impl;
    state.FinishFrame();
    ++state.frame;
    if (exposure) {
        state.events.Add(Event::Exposure);
        state.exposure_dispatches += state.census_started && state.census_draws < CensusDraws;
    }
}

} // namespace Gr2Fsr3
