// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "common/elf_info.h"
#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/gr2_fsr3/fsr3_context.h"
#include "video_core/gr2_fsr3/fsr3_inputs.h"
#include "video_core/gr2_fsr3/fsr3_jitter.h"
#include "video_core/gr2_fsr3/fsr3_passes.h"
#include "video_core/gr2_fsr3/fsr3_settings.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/gr2_fsr3/gr2_fsr3.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/skipcache/skipcache.h"
#include "video_core/texture_cache/image.h"

namespace Gr2Fsr3 {

namespace {

using Clock = std::chrono::steady_clock;

// The compute passes whose dispatches are logged; the first three also drive the module.
constexpr std::array TrackedHashes{
    Hash::Antialias,         Hash::Velocity, Hash::CbResolve,
    Hash::MotionBlurPrepare, Hash::Ssao0,    Hash::Ssao1,
    Hash::Contour,           Hash::Ibl0,     Hash::Ibl1,
};

constexpr std::array<std::string_view, 4> ModeNames{"stock", "fsr", "fallback", "debug"};
constexpr std::array<std::string_view, 3> KindNames{"native", "upscale", "other"};

constexpr u64 PeriodicLogFrames = 300;
constexpr size_t MaxLoggedSizes = 16;
constexpr auto ResetGap = std::chrono::milliseconds{100};
constexpr float MaxFrameTimeMs = 200.0f;
constexpr float DefaultFrameTimeMs = 16.6f;
constexpr float CoverageOffset = 2.0f;

struct SizePair {
    vk::Extent2D render{};
    vk::Extent2D output{};

    bool operator==(const SizePair&) const = default;
};

Kind KindOf(const SizePair& sizes) {
    if (sizes.render == sizes.output) {
        return Kind::Native;
    }
    if (sizes.output.width >= sizes.render.width && sizes.output.height >= sizes.render.height) {
        return Kind::Upscale;
    }
    return Kind::Other;
}

float Milliseconds(Clock::duration duration) {
    return std::chrono::duration<float, std::milli>(duration).count();
}

// What one Antialias dispatch decided before recording.
struct Frame {
    Settings::Values settings{};
    Clock::time_point now{};
    Jitter::FrameReport report{};
    AntialiasBindings bindings{};
    SizePair sizes{};
    Kind kind{Kind::Other};
    CameraState camera{};
    bool capture_current{};
    bool masks{};         // The mask pass runs.
    bool camera_motion{}; // The camera-motion pass runs.
    bool depth_main{};    // FSR reads the main depth instead of the copy.
    bool reset{};         // FSR history restarts if FSR runs.
};

AcquireRequest RequestFor(Mode mode, const Frame& frame) {
    if (mode == Mode::Fsr) {
        return {
            .color = true,
            .velocity = true,
            .stencil = frame.masks || frame.camera_motion,
            .depth_copy = !frame.depth_main || frame.camera_motion,
            .depth_main = frame.depth_main,
            .output = true,
        };
    }
    if (mode == Mode::Debug) {
        return {
            .color = true,
            .velocity = true,
            .stencil = true,
            .depth_copy = frame.capture_current,
            .depth_main = true,
            .output = true,
        };
    }
    return {.color = true, .output = true};
}

MaskParams MasksOf(const Frame& frame, const FrameImages& images) {
    const Settings::Values& settings = frame.settings;
    return {
        .stencil = images.stencil.view,
        .render_size = frame.sizes.render,
        .reactive_characters = static_cast<float>(settings.reactive_characters) / 100.0f,
        .reactive_effects = static_cast<float>(settings.reactive_effects) / 100.0f,
        .reactive_outline = static_cast<float>(settings.reactive_outline) / 100.0f,
        .tc_characters = static_cast<float>(settings.tc_characters) / 100.0f,
        .character_history = (settings.test & TestBit::CharacterMaskHistory) != 0,
    };
}

FallbackParams FallbackOf(const Frame& frame, const FrameImages& images, bool easu) {
    return {
        .source = images.color.view,
        .source_size = frame.sizes.render,
        .destination = images.output.view,
        .destination_size = frame.sizes.output,
        .easu = easu,
    };
}

FsrImage DeclareInput(const GuestImage& image, vk::Format format, vk::ImageAspectFlags aspect) {
    return {
        .image = image.image->GetImage(),
        .format = format,
        .extent = image.extent,
        .usage = image.usage,
        .aspect = aspect,
    };
}

class Module {
public:
    explicit Module(const Vulkan::Instance& instance) : instance{instance}, context{instance} {
        Jitter::SetMaxLodBias(instance.MaxSamplerLodBias());
    }

    bool OnDispatch(size_t tracked, const Shader::Info& info,
                    const AmdGpu::ComputeProgram& cs_program, Vulkan::Rasterizer& rasterizer);

private:
    bool Antialias(const Shader::Info& info, Vulkan::Rasterizer& rasterizer);
    Mode Record(Mode mode, const Frame& frame, Vulkan::Rasterizer& rasterizer);
    Mode RecordFsr(vk::CommandBuffer cmdbuf, const Frame& frame, const FrameImages& images,
                   Vulkan::Scheduler& scheduler);
    void ArmJitter(Vulkan::Rasterizer& rasterizer, const Jitter::FramePlan& plan);
    void Publish(Mode mode, const Frame& frame, float interval_ms);

    const Vulkan::Instance& instance;
    Inputs inputs;
    Context context;
    std::unique_ptr<Passes> passes;
    bool passes_failed{};
    ContextState context_state{ContextState::Idle};
    u64 aa_count{};
    u64 cbresolve_frame{~0ULL};
    bool log_frame{};
    std::array<bool, TrackedHashes.size()> logged_dispatch{};
    std::optional<SizePair> session_sizes;
    std::vector<SizePair> logged_sizes;
    Clock::time_point last_aa_time{};
    Clock::time_point last_fsr_time{};
    Settings::Values last_fsr_settings{};
    bool last_fsr_depth_main{};
    bool last_frame_fsr{};
    u64 context_generation{};
    s32 phase_index{};
    bool jitter_was_enabled{};
    u64 frame_id{};
    u64 fsr_dispatches{};
    Mode last_mode{Mode::Stock};
    u32 logged_mode_changes{};
    bool logged_cbresolve{};
    bool logged_no_depth{};
    bool logged_copy_unavailable{};
    bool logged_main_unavailable{};
    bool logged_color_format{};
    bool logged_rejected{};
};

// Created on the first tracked dispatch and never destroyed: static destruction can run after
// the Vulkan device is gone.
Module* g_module{};

bool Module::OnDispatch(size_t tracked, const Shader::Info& info,
                        const AmdGpu::ComputeProgram& cs_program, Vulkan::Rasterizer& rasterizer) {
    if (log_frame || !logged_dispatch[tracked]) {
        logged_dispatch[tracked] = true;
        inputs.LogDispatch(info, cs_program, rasterizer);
    }
    switch (info.pgm_hash) {
    case Hash::Antialias:
        return Antialias(info, rasterizer);
    case Hash::Velocity:
        inputs.CaptureVelocity(info, rasterizer, aa_count);
        break;
    case Hash::CbResolve:
        cbresolve_frame = aa_count;
        break;
    default:
        break;
    }
    return false;
}

bool Module::Antialias(const Shader::Info& info, Vulkan::Rasterizer& rasterizer) {
    Frame frame{};
    frame.settings = Settings::Snapshot();
    frame.now = Clock::now();
    const float interval_ms = aa_count == 0 ? 0.0f : Milliseconds(frame.now - last_aa_time);
    last_aa_time = frame.now;
    frame.report = Jitter::EndFrame();
    const VelocityCapture& capture = inputs.Velocity();
    frame.capture_current = capture.valid && capture.frame == aa_count;
    const bool cbresolve = cbresolve_frame == aa_count;
    const u64 frame_index = aa_count++;
    const bool log_this = log_frame;
    const Settings::Values& settings = frame.settings;
    log_frame = settings.log && (frame_index + 1) % PeriodicLogFrames == 0;

    if (!inputs.IdentifyAntialias(info, frame.bindings)) {
        ArmJitter(rasterizer, Jitter::FramePlan{});
        last_frame_fsr = false;
        Publish(Mode::Stock, frame, interval_ms);
        return false;
    }

    frame.sizes = {
        .render = inputs.RenderSize(frame.bindings, frame.capture_current),
        .output = {frame.bindings.output.Width(), frame.bindings.output.Height()},
    };
    frame.kind = KindOf(frame.sizes);
    if (!session_sizes) {
        session_sizes = frame.sizes;
    }
    // Frames of other sizes keep the context of the first sizes and take the non-FSR path of
    // their kind.
    const bool session_frame = frame.sizes == *session_sizes;
    if (logged_sizes.size() < MaxLoggedSizes && !std::ranges::contains(logged_sizes, frame.sizes)) {
        logged_sizes.push_back(frame.sizes);
        LOG_INFO(Render_Vulkan, "GR2 FSR3: Antialias {}x{} -> {}x{} ({}){}",
                 frame.sizes.render.width, frame.sizes.render.height, frame.sizes.output.width,
                 frame.sizes.output.height, KindNames[static_cast<size_t>(frame.kind)],
                 session_frame ? "" : ", not the first sizes: FSR stays off for these frames");
    }
    frame.camera = inputs.EvaluateCamera(frame.capture_current);

    const bool coverage = (settings.test & TestBit::CoverageJitter) != 0;
    const bool camera_motion_test = (settings.test & TestBit::CharacterCameraMotion) != 0;
    std::optional<ContextKey> wanted;
    if (settings.enable && !coverage && !cbresolve && KindOf(*session_sizes) != Kind::Other) {
        wanted = ContextKey{
            .max_render_size = session_sizes->render,
            .max_output_size = session_sizes->output,
            .fp16 = context.IsFp16Supported() && !settings.fp32,
            .debug_checking = settings.validate,
        };
    }
    context_state = context.Update(
        rasterizer.GetScheduler(), wanted,
        Passes::EstimateBytes(session_sizes->render, session_sizes->output, camera_motion_test));

    frame.masks = settings.reactive_characters > 0 || settings.reactive_effects > 0 ||
                  settings.reactive_outline > 0 || settings.tc_characters > 0 ||
                  (settings.test & TestBit::CharacterMaskHistory) != 0;
    frame.camera_motion = camera_motion_test && frame.capture_current && capture.cb_valid;
    // The copy needs this frame's Velocity capture. FSR barriers only the depth aspect of the main
    // depth-stencil image, which is valid only with separateDepthStencilLayouts.
    frame.depth_main = context.IsSeparateDepthStencilLayoutsSupported() &&
                       (settings.depth_source == DepthSource::Main || !frame.capture_current);
    const u64 generation = context.Generation();
    frame.reset = generation != context_generation || !last_frame_fsr ||
                  frame.now - last_fsr_time > ResetGap ||
                  (frame.camera.cut && (settings.test & TestBit::CutDetectionOff) == 0) ||
                  settings.color_space != last_fsr_settings.color_space ||
                  settings.test != last_fsr_settings.test ||
                  frame.depth_main != last_fsr_depth_main;

    const bool fsr_ready = context_state == ContextState::Ready && session_frame;
    const bool has_depth = frame.depth_main || frame.capture_current;
    if (fsr_ready && !has_depth && !logged_no_depth) {
        logged_no_depth = true;
        LOG_WARNING(Render_Vulkan, "GR2 FSR3: no depth input (no Velocity capture this frame and "
                                   "the main depth needs separateDepthStencilLayouts)");
    }
    Mode mode = Mode::Fallback;
    if (cbresolve) {
        mode = Mode::Stock;
        if (!logged_cbresolve) {
            logged_cbresolve = true;
            LOG_WARNING(Render_Vulkan, "GR2 FSR3: 4K output flag set (CbResolve runs); pin it "
                                       "with a resolution toggle");
        }
    } else if (settings.debug >= DebugView::MotionVectors &&
               settings.debug <= DebugView::Reprojection) {
        mode = Mode::Debug;
    } else if (fsr_ready && has_depth) {
        mode = Mode::Fsr;
    } else if (frame.kind == Kind::Native) {
        mode = Mode::Stock;
    }
    if (mode != Mode::Stock && !passes && !passes_failed) {
        passes = std::make_unique<Passes>(instance);
        if (!passes->Init()) {
            passes.reset();
            passes_failed = true;
            LOG_ERROR(Render_Vulkan,
                      "GR2 FSR3: host passes are unavailable; the Antialias pass stays stock");
        }
    }
    if (!passes) {
        mode = Mode::Stock;
    }

    const Mode executed = Record(mode, frame, rasterizer);

    Jitter::FramePlan plan{.scene_stencil_address = frame.bindings.stencil.tsharp.Address()};
    if (executed == Mode::Fsr) {
        const s32 phase_count =
            Jitter::PhaseCount(frame.sizes.render.width, frame.sizes.output.width);
        if (generation != context_generation) {
            phase_index = 0;
        }
        phase_index = (phase_index + 1) % phase_count;
        last_fsr_time = frame.now;
        last_fsr_settings = settings;
        last_fsr_depth_main = frame.depth_main;
        context_generation = generation;
        ++fsr_dispatches;
        if ((settings.test & TestBit::JitterOff) == 0) {
            plan.enabled = true;
            plan.offset = Jitter::PhaseOffset(phase_index, phase_count);
            if (frame.kind == Kind::Upscale && settings.lod_bias) {
                const float ratio = static_cast<float>(frame.sizes.render.width) /
                                    static_cast<float>(frame.sizes.output.width);
                const float extra =
                    ((settings.test & TestBit::LodBiasExtraHalf) != 0 ? 0.5f : 0.0f) +
                    ((settings.test & TestBit::LodBiasExtraOne) != 0 ? 1.0f : 0.0f);
                plan.lod_bias = std::log2(ratio) - extra;
            }
        }
    } else if (coverage && executed != Mode::Debug) {
        plan.enabled = true;
        plan.offset = (frame_index & 1) != 0 ? Jitter::Offset{CoverageOffset, CoverageOffset}
                                             : Jitter::Offset{-CoverageOffset, -CoverageOffset};
    }
    last_frame_fsr = executed == Mode::Fsr;
    ArmJitter(rasterizer, plan);
    Publish(executed, frame, interval_ms);
    if (log_this) {
        inputs.LogFrame(FrameLog{
            .frame = frame_index,
            .mode = executed,
            .kind = frame.kind,
            .context = context_state,
            .render_size = frame.sizes.render,
            .output_size = frame.sizes.output,
            .interval_ms = interval_ms,
            .reset = executed == Mode::Fsr && frame.reset,
            .camera = frame.camera,
            .jitter = frame.report,
        });
    }
    return executed != Mode::Stock;
}

Mode Module::Record(Mode mode, const Frame& frame, Vulkan::Rasterizer& rasterizer) {
    if (mode == Mode::Stock) {
        return mode;
    }
    auto& scheduler = rasterizer.GetScheduler();
    scheduler.EndRendering();
    FrameImages images{};
    bool acquired = inputs.Acquire(frame.bindings, RequestFor(mode, frame), rasterizer, images);
    if (!acquired && mode == Mode::Fsr && frame.kind != Kind::Native) {
        mode = Mode::Fallback;
        acquired = inputs.Acquire(frame.bindings, RequestFor(mode, frame), rasterizer, images);
    }
    // Flushed even when Acquire fails: the tracked layouts include every queued transition. A
    // stock frame then transitions its own bindings again.
    rasterizer.GetRuntime().FlushBarriers();
    if (!acquired) {
        return Mode::Stock;
    }

    rasterizer.ScopeMarkerBegin("Host/FSR3");
    const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
    if (mode == Mode::Fsr) {
        mode = RecordFsr(cmdbuf, frame, images, scheduler);
    } else if (mode == Mode::Fallback) {
        passes->RecordFallback(cmdbuf, FallbackOf(frame, images, frame.settings.fallback_easu));
    } else {
        const DebugView view = frame.settings.debug;
        const bool masks = view == DebugView::Masks;
        const bool reprojection = view == DebugView::Reprojection;
        // The debug pass writes the guest image, and only the Masks and Reprojection views read
        // host images, all at the render size.
        passes->Resize(scheduler, (masks || reprojection) ? frame.sizes.render : vk::Extent2D{}, {},
                       false, reprojection);
        if (masks) {
            passes->RecordMask(cmdbuf, MasksOf(frame, images));
        }
        passes->RecordDebug(
            cmdbuf,
            DebugParams{
                .view = view,
                .color = images.color.view,
                .motion_vectors = images.velocity.view,
                .stencil = images.stencil.view,
                .depth_copy = images.depth_copy.view,
                .depth_main = images.depth_main.view,
                .destination = images.output.view,
                .source_size = frame.sizes.render,
                .destination_size = frame.sizes.output,
                .camera = frame.camera.params,
                .motion_vector_scale_x = 0.5f * static_cast<float>(frame.sizes.render.width),
                .motion_vector_scale_y = -0.5f * static_cast<float>(frame.sizes.render.height),
            });
    }
    // FSR binds compute pipelines and descriptor sets; the passes push descriptors and constants.
    auto& skipcache = VideoCore::Skipcache::Framework::Instance();
    skipcache.BumpForeignPipelineGen(1);
    skipcache.BumpForeignPushGen(1);
    rasterizer.ScopeMarkerEnd();
    return mode;
}

Mode Module::RecordFsr(vk::CommandBuffer cmdbuf, const Frame& frame, const FrameImages& images,
                       Vulkan::Scheduler& scheduler) {
    const Settings::Values& settings = frame.settings;
    const auto& [render, output] = frame.sizes;
    const bool camera_motion_image = (settings.test & TestBit::CharacterCameraMotion) != 0;
    passes->Resize(scheduler, render, output, camera_motion_image, false);
    if (frame.reset) {
        passes->ResetHistory();
    }
    if (frame.masks) {
        passes->RecordMask(cmdbuf, MasksOf(frame, images));
    }
    if (frame.camera_motion) {
        CameraMotionParams params{
            .motion_vectors = images.velocity.view,
            .depth = images.depth_copy.view,
            .stencil = images.stencil.view,
            .render_size = render,
        };
        const auto& cb = inputs.Velocity().cb;
        std::memcpy(params.matrix.data(), cb.data(), sizeof(params.matrix));
        std::memcpy(params.inverse_view.data(), cb.data() + params.matrix.size(),
                    sizeof(params.inverse_view));
        passes->RecordCameraMotion(cmdbuf, params);
    }
    passes->PrepareFsr(cmdbuf);

    vk::Format color_format = images.color.format;
    if (color_format == vk::Format::eR8G8B8A8Srgb || color_format == vk::Format::eR8G8B8A8Unorm) {
        color_format = settings.color_space == ColorSpace::Linear ? vk::Format::eR8G8B8A8Srgb
                                                                  : vk::Format::eR8G8B8A8Unorm;
    } else if (!logged_color_format) {
        logged_color_format = true;
        LOG_WARNING(Render_Vulkan, "GR2 FSR3: colour input format {} is passed to FSR unchanged",
                    vk::to_string(color_format));
    }
    bool& logged_depth = frame.depth_main ? logged_copy_unavailable : logged_main_unavailable;
    if (frame.depth_main != (settings.depth_source == DepthSource::Main) && !logged_depth) {
        logged_depth = true;
        LOG_WARNING(Render_Vulkan, "GR2 FSR3: the {} depth is unavailable; FSR reads the {} depth",
                    frame.depth_main ? "copy" : "main", frame.depth_main ? "main" : "copy");
    }

    const GuestImage& depth = frame.depth_main ? images.depth_main : images.depth_copy;
    const float jitter_sign = (settings.test & TestBit::JitterSignFlip) != 0 ? -1.0f : 1.0f;
    const bool repeated = frame.camera.repeated && (settings.test & TestBit::RepeatZeroingOff) == 0;
    const float mv_scale_y = (settings.test & TestBit::MotionVectorYFlip) != 0 ? 0.5f : -0.5f;
    const float frame_time_ms = Milliseconds(frame.now - last_fsr_time);
    const FsrDispatch dispatch{
        .color = DeclareInput(images.color, color_format, vk::ImageAspectFlagBits::eColor),
        .depth = DeclareInput(depth, depth.format,
                              frame.depth_main ? vk::ImageAspectFlagBits::eDepth
                                               : vk::ImageAspectFlagBits::eColor),
        .motion_vectors = frame.camera_motion
                              ? passes->CameraMotionVectors()
                              : DeclareInput(images.velocity, images.velocity.format,
                                             vk::ImageAspectFlagBits::eColor),
        .reactive = frame.masks ? passes->ReactiveMask() : FsrImage{},
        .transparency = settings.tc_characters > 0 ? passes->TransparencyMask() : FsrImage{},
        .output = passes->Output(),
        .jitter_x = jitter_sign * frame.report.applied.x,
        .jitter_y = jitter_sign * frame.report.applied.y,
        .motion_vector_scale_x = repeated ? 0.0f : 0.5f * static_cast<float>(render.width),
        .motion_vector_scale_y = repeated ? 0.0f : mv_scale_y * static_cast<float>(render.height),
        .render_size = render,
        .output_size = output,
        .frame_time_ms = frame_time_ms > 0.0f && frame_time_ms <= MaxFrameTimeMs
                             ? frame_time_ms
                             : DefaultFrameTimeMs,
        .camera = frame.camera.params,
        .reset = frame.reset,
        .frame_id = frame_id++,
    };
    if (!context.RecordDispatch(cmdbuf, dispatch, settings.validate)) {
        const bool native = frame.kind == Kind::Native;
        if (!logged_rejected) {
            logged_rejected = true;
            LOG_ERROR(Render_Vulkan, "GR2 FSR3: FSR rejected the dispatch, frame uses the {} pass",
                      native ? "stock" : "fallback");
        }
        if (native) {
            return Mode::Stock;
        }
        passes->RecordFallback(cmdbuf, FallbackOf(frame, images, settings.fallback_easu));
        return Mode::Fallback;
    }
    if (settings.debug == DebugView::Bypass) {
        passes->RecordFallback(cmdbuf, FallbackOf(frame, images, false));
        return Mode::Fsr;
    }
    const float sharpness = static_cast<float>(settings.sharpness) / 100.0f;
    passes->RecordStore(cmdbuf,
                        StoreParams{
                            .destination = images.output.view,
                            .output_size = output,
                            .rcas_stops = sharpness > 0.0f ? 2.0f * (1.0f - sharpness) : -1.0f,
                            .linear_input = settings.color_space == ColorSpace::Linear,
                        });
    return Mode::Fsr;
}

void Module::ArmJitter(Vulkan::Rasterizer& rasterizer, const Jitter::FramePlan& plan) {
    Jitter::BeginFrame(plan);
    // The offset changes at the next Composite draw; invalidating keeps the dynamic-state memos
    // from replaying a viewport with the old offset.
    if (plan.enabled || jitter_was_enabled) {
        rasterizer.GetScheduler().GetDynamicState().Invalidate();
    }
    jitter_was_enabled = plan.enabled;
}

void Module::Publish(Mode mode, const Frame& frame, float interval_ms) {
    const auto& [render, output] = frame.sizes;
    Settings::PublishStatus(Settings::Status{
        .mode = mode,
        .kind = frame.kind,
        .context = context_state,
        .render_width = render.width,
        .render_height = render.height,
        .output_width = output.width,
        .output_height = output.height,
        .fsr_dispatches = fsr_dispatches,
        .frame_ms = interval_ms,
        .context_bytes = context.MemoryBytes(),
        .notice = context.Notice(),
    });
    // Each distinct change is logged once, so modes alternating per frame cannot flood the log.
    const u32 change =
        1u << (static_cast<u32>(last_mode) * ModeNames.size() + static_cast<u32>(mode));
    if (mode != last_mode && (logged_mode_changes & change) == 0) {
        logged_mode_changes |= change;
        LOG_INFO(Render_Vulkan, "GR2 FSR3: Antialias {}x{} -> {}x{}: mode {} -> {}", render.width,
                 render.height, output.width, output.height,
                 ModeNames[static_cast<size_t>(last_mode)], ModeNames[static_cast<size_t>(mode)]);
    }
    last_mode = mode;
}

} // namespace

namespace Detail {

TitleState g_title_state{TitleState::Unresolved};

bool ResolveTitle() {
    const bool active = Common::IsGravityRush2();
    g_title_state = active ? TitleState::Active : TitleState::Inactive;
    if (active) {
        LOG_INFO(Render_Vulkan, "GR2 FSR3: active for {}",
                 Common::ElfInfo::Instance().GameSerial());
    }
    return active;
}

bool OnDispatchActive(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                      Vulkan::Rasterizer& rasterizer) {
    const auto tracked = std::ranges::find(TrackedHashes, info.pgm_hash);
    if (tracked == TrackedHashes.end()) {
        return false;
    }
    if (!g_module) {
        g_module = new Module{rasterizer.GetRuntime().GetInstance()};
    }
    const auto index = static_cast<size_t>(std::distance(TrackedHashes.begin(), tracked));
    return g_module->OnDispatch(index, info, cs_program, rasterizer);
}

} // namespace Detail

} // namespace Gr2Fsr3
