// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <fmt/ranges.h>
#include <magic_enum/magic_enum.hpp>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/gr2_fsr3/fsr3_context.h"
#include "video_core/gr2_fsr3/fsr3_jitter.h"
#include "video_core/gr2_fsr3/fsr3_passes.h"
#include "video_core/gr2_fsr3/fsr3_settings.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/gr2_fsr3/fsr4_context.h"
#include "video_core/gr2_fsr3/gr2_fsr3.h"
#include "video_core/gr2_fsr3/grr_capture.h"
#include "video_core/gr2_fsr3/grr_survey.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/skipcache/skipcache.h"
#include "video_core/texture_cache/image.h"

namespace Gr2Fsr3 {

namespace {

using Clock = std::chrono::steady_clock;

constexpr u64 PeriodicLogFrames = 300;
constexpr auto ResetGap = std::chrono::milliseconds{100};
constexpr float MaxFrameTimeMs = 200.0f;
constexpr float DefaultFrameTimeMs = 16.6f;
constexpr float CoverageOffset = 2.0f;
constexpr u32 MaxLoggedCuts = 8;

// Why FSR does not run for an FXAA draw.
enum class StockReason : u8 {
    Bindings,
    NoCamera,
    CameraRejected,
    NoCopyLuma,
    NoComposite,
    Sizes,
    NoPreviousCamera,
    Images,
};
constexpr std::array<std::string_view, 8> StockReasonNames{
    "the FXAA bindings are not recognized",
    "no camera draw this frame",
    "the camera of this frame is rejected",
    "no CopyLuma draw wrote the FXAA input this frame",
    "no Composite draw wrote the FXAA input this frame",
    "the FXAA input differs from the scene in size or is larger than its target",
    "no camera in the previous frame",
    "the images cannot be used",
};
static_assert(StockReasonNames.size() == magic_enum::enum_count<StockReason>());

float Milliseconds(Clock::duration duration) {
    return std::chrono::duration<float, std::milli>(duration).count();
}

// What one FXAA draw decided before recording.
struct GrrFrame {
    Settings::Values settings{};
    Clock::time_point now{};
    Jitter::FrameReport report{};
    GrrFxaaDraw draw{};
    GrrSceneDepth scene{};
    GrrCamera camera{};
    GrrCameraDelta delta{};
    GrrReprojection reprojection{};
    Kind kind{Kind::Other};
    bool masks{}; // The mask pass runs.
    bool reset{}; // FSR history restarts if FSR runs.
};

// The jitter and motion-vector scale that FSR 3.1 and FSR 4 receive. The camera-motion pass
// writes render pixels with +y down.
struct MotionTerms {
    float jitter_x{};
    float jitter_y{};
    float motion_vector_scale_x{};
    float motion_vector_scale_y{};
};

MotionTerms MotionTermsOf(const GrrFrame& frame) {
    const u32 test = frame.settings.test;
    const float jitter_sign = (test & TestBit::JitterSignFlip) != 0 ? -1.0f : 1.0f;
    return {
        .jitter_x = jitter_sign * frame.report.applied.x,
        .jitter_y = jitter_sign * frame.report.applied.y,
        .motion_vector_scale_x = 1.0f,
        .motion_vector_scale_y = (test & TestBit::MotionVectorYFlip) != 0 ? -1.0f : 1.0f,
    };
}

// The camera-motion pass removes the jitter the draws had, whatever FSR is told.
GrrCameraMotionParams CameraMotionOf(const GrrFrame& frame, const GrrImages& images) {
    return {
        .depth = images.depth.view,
        .render_size = frame.draw.render_size,
        .scene = frame.reprojection.scene,
        .sky = frame.reprojection.sky,
        .jitter_x = frame.report.applied.x,
        .jitter_y = frame.report.applied.y,
    };
}

MaskParams MasksOf(const GrrFrame& frame, const GrrImages& images) {
    const Settings::Values& settings = frame.settings;
    return {
        .stencil = images.stencil.view,
        .render_size = frame.draw.render_size,
        .reactive_characters = static_cast<float>(settings.reactive_characters) / 100.0f,
        .tc_characters = static_cast<float>(settings.tc_characters) / 100.0f,
        .character_history = (settings.test & TestBit::CharacterMaskHistory) != 0,
        .character_stencil = GrrCharacterStencil,
        .geometry_stencil = 0,
        .temporal_stencil = 0,
    };
}

// The store writes what the FXAA pixel shader exports: colour in the component order of the
// target, alpha 1. It reads output, a null image for the FSR 3.1 output.
StoreParams StoreOf(const GrrFrame& frame, const GrrImages& images, const Fsr4Image& output) {
    const Settings::Values& settings = frame.settings;
    const float sharpness = static_cast<float>(settings.sharpness) / 100.0f;
    return {
        .destination = images.output.view,
        .output_size = frame.draw.output_size,
        .rcas_stops = sharpness > 0.0f ? 2.0f * (1.0f - sharpness) : -1.0f,
        .linear_input = settings.color_space == ColorSpace::Linear,
        .source = output,
        .fxaa = settings.fxaa,
        .stencil = images.stencil.view,
        .render_size = frame.draw.render_size,
        .character_stencil = GrrCharacterStencil,
        .components = frame.draw.components,
        .alpha_one = true,
    };
}

// The fallback writes what the FXAA pixel shader exports: colour in the component order of the
// target, alpha 1.
FallbackParams FallbackOf(const GrrFrame& frame, const GrrImages& images, bool easu) {
    return {
        .source = images.color.view,
        .source_size = frame.draw.render_size,
        .destination = images.output.view,
        .destination_size = frame.draw.output_size,
        .easu = easu,
        .components = frame.draw.components,
        .alpha_one = true,
    };
}

class GrrModule {
public:
    explicit GrrModule(const Vulkan::Instance& instance)
        : instance{instance}, context{instance}, fsr4{instance} {
        Jitter::SetMaxLodBias(instance.MaxSamplerLodBias());
    }

    void OnExposure(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                    Vulkan::Rasterizer& rasterizer);
    bool OnDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                Vulkan::Rasterizer& rasterizer, bool is_indirect);

private:
    void StartFrame(Vulkan::Rasterizer& rasterizer, bool exposure);
    bool OnFxaa(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                Vulkan::Rasterizer& rasterizer);
    Mode Record(Mode mode, const GrrFrame& frame, Vulkan::Rasterizer& rasterizer);
    Mode RecordFsr(const Vulkan::CommandRecorder& cmdbuf, const GrrFrame& frame,
                   const GrrImages& images, Vulkan::Scheduler& scheduler);
    Mode RecordFsr4(const Vulkan::CommandRecorder& cmdbuf, const GrrFrame& frame,
                    const GrrImages& images, Vulkan::Scheduler& scheduler);
    void RecordDebug(const Vulkan::CommandRecorder& cmdbuf, const GrrFrame& frame,
                     const GrrImages& images, Vulkan::Scheduler& scheduler);
    void LogStock(StockReason reason);
    void Publish(Mode mode, const GrrFrame& frame, float interval_ms);
    void LogFrame(u64 frame_index, Mode mode, const GrrFrame& frame, float interval_ms) const;

    const Vulkan::Instance& instance;
    GrrCapture capture;
    GrrSurvey survey;
    Context context;
    Fsr4Context fsr4;
    std::unique_ptr<Passes> passes;
    bool passes_failed{};
    bool grr_passes_failed{};
    bool fsr4_passes_failed{};
    ContextState context_state{ContextState::Idle};
    ContextState fsr4_state{ContextState::Idle};
    std::optional<Fsr4Key> fsr4_key;
    std::optional<SizePair> session_sizes;
    GrrCamera previous_camera{};
    bool exposure_seen{};
    bool awaiting_frame_start{};
    bool jitter_was_armed{};
    u64 trigger_count{};
    bool log_frame{};
    Clock::time_point last_trigger_time{};
    Clock::time_point last_fsr_time{};
    Settings::Values last_fsr_settings{};
    bool last_frame_fsr{};
    bool last_frame_fsr4{};
    u64 context_generation{};
    u64 fsr4_generation{};
    s32 phase_index{};
    u64 frame_id{};
    u64 fsr_dispatches{};
    Mode last_mode{Mode::Stock};
    u32 logged_mode_changes{};
    std::array<bool, StockReasonNames.size()> logged_stock{};
    bool logged_eligible{};
    bool logged_fallback_start{};
    u32 logged_cuts{};
    bool logged_rejected{};
    bool logged_fsr4_rejected{};
};

// Created on the first draw or exposure dispatch and never destroyed: static destruction can run
// after the Vulkan device is gone.
GrrModule* g_grr{};

GrrModule& Grr(Vulkan::Rasterizer& rasterizer) {
    if (!g_grr) {
        g_grr = new GrrModule{rasterizer.GetRuntime().GetInstance()};
    }
    return *g_grr;
}

void GrrModule::OnExposure(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                           Vulkan::Rasterizer& rasterizer) {
    if (!std::exchange(exposure_seen, true)) {
        LOG_INFO(Render_Vulkan,
                 "GRR FSR: frames start at the exposure dispatch {:#x}, {}x{}x{} groups",
                 info.pgm_hash, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    }
    StartFrame(rasterizer, true);
}

bool GrrModule::OnDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                       Vulkan::Rasterizer& rasterizer, bool is_indirect) {
    const GrrDrawHashes hashes = HashesOf(pipeline);
    // Without exposure dispatches a frame starts at the first draw into the scene depth after an
    // FXAA draw.
    const auto& db = liverpool.regs.depth_buffer;
    if (!exposure_seen && awaiting_frame_start && db.DepthValid() &&
        db.DepthAddress() == capture.SceneDepthAddress()) {
        if (!std::exchange(logged_fallback_start, true)) {
            LOG_WARNING(Render_Vulkan, "GRR FSR: no exposure dispatch seen; frames start at the "
                                       "first draw into the scene depth");
        }
        StartFrame(rasterizer, false);
    }
    Jitter::OnGrrDraw(liverpool, hashes.vs, is_indirect);
    survey.OnDraw(pipeline, liverpool, hashes, capture.SceneDepthAddress());
    if (hashes.ps == GrrHash::FxaaPs) {
        return OnFxaa(pipeline, liverpool, rasterizer);
    }
    capture.OnDraw(pipeline, liverpool, rasterizer, hashes);
    return false;
}

void GrrModule::StartFrame(Vulkan::Rasterizer& rasterizer, bool exposure) {
    const bool armed = Jitter::StartFrame();
    // The offset changes here; invalidating keeps the dynamic-state memos from replaying a
    // viewport with the old offset.
    if (armed || jitter_was_armed) {
        rasterizer.GetScheduler().GetDynamicState().Invalidate();
    }
    jitter_was_armed = armed;
    capture.StartFrame();
    survey.OnFrameStart(exposure);
    awaiting_frame_start = false;
}

bool GrrModule::OnFxaa(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                       Vulkan::Rasterizer& rasterizer) {
    awaiting_frame_start = true;
    GrrFrame frame{};
    frame.settings = Settings::Snapshot();
    frame.now = Clock::now();
    const float interval_ms =
        trigger_count == 0 ? 0.0f : Milliseconds(frame.now - last_trigger_time);
    last_trigger_time = frame.now;
    frame.report = Jitter::EndFrame();
    const u64 frame_index = trigger_count++;
    const bool log_this = log_frame;
    const Settings::Values& settings = frame.settings;
    log_frame = settings.log && (frame_index + 1) % PeriodicLogFrames == 0;

    frame.camera = capture.Camera();
    frame.scene = capture.SceneDepth();
    const bool recognized = capture.IdentifyFxaa(pipeline, liverpool, frame.draw);
    survey.OnFxaa(frame.draw, recognized, frame.scene, rasterizer);
    const vk::Extent2D render = frame.draw.render_size;
    const vk::Extent2D output = frame.draw.output_size;
    const SizePair sizes{.render = render, .output = output};
    if (recognized) {
        frame.kind = KindOf(sizes);
    }
    if (frame.camera.valid && previous_camera.valid) {
        frame.delta = CompareCameras(frame.camera, previous_camera);
        if (frame.delta.cut && logged_cuts < MaxLoggedCuts) {
            ++logged_cuts;
            LOG_INFO(
                Render_Vulkan,
                "GRR FSR: camera cut at frame {}: move {:.2f} turn {:.2f} deg fov delta {:.3f}",
                frame_index, frame.delta.translation, frame.delta.rotation_degrees,
                frame.delta.fov_delta);
        }
    }
    std::optional<GrrReprojection> reprojection;
    if (frame.camera.valid) {
        reprojection =
            Reprojection(frame.camera, previous_camera.valid ? previous_camera : frame.camera);
    }

    // A replaced frame has its input at the scene size and a target at least as large. The input
    // of a native frame is the CopyLuma copy of the Composite, that of an upscaled frame is the
    // Composite itself.
    const bool native = frame.kind == Kind::Native;
    const VAddr input = frame.draw.color.tsharp.Address();
    std::optional<StockReason> stock;
    if (!recognized) {
        stock = StockReason::Bindings;
    } else if (!reprojection) {
        stock = frame.scene.valid ? StockReason::CameraRejected : StockReason::NoCamera;
    } else if (frame.kind == Kind::Other || render != frame.scene.extent) {
        stock = StockReason::Sizes;
    } else if (native ? !capture.CopyLumaWrote(input) : !capture.CompositeWrote(input)) {
        stock = native ? StockReason::NoCopyLuma : StockReason::NoComposite;
    } else {
        frame.reprojection = *reprojection;
        if (!session_sizes) {
            session_sizes = sizes;
        }
        if (!std::exchange(logged_eligible, true)) {
            LOG_INFO(Render_Vulkan,
                     "GRR FSR: the FXAA draw can be replaced: {}x{} -> {}x{} ({}), target {:#x}",
                     render.width, render.height, output.width, output.height,
                     magic_enum::enum_name(frame.kind), frame.draw.target.Address());
        }
    }

    const bool coverage = (settings.test & TestBit::CoverageJitter) != 0;
    const SizePair session = session_sizes.value_or(SizePair{});
    const bool session_fsr = session_sizes.has_value() && !coverage;
    fsr4_key.reset();
    if (settings.grr_fsr4_enable && session_fsr && !fsr4_passes_failed) {
        fsr4_key = Fsr4KeyFor(session.render, session.output, settings.fsr4_model);
    }
    fsr4_state = fsr4.Update(rasterizer.GetScheduler(), fsr4_key, !context.HoldsMemory(),
                             Passes::EstimateFsr4Bytes(session.render, true, true));
    // FSR 3.1 holds no context while FSR 4 holds one or its files have passed their checks; while
    // the files load, a context FSR 3.1 already has keeps running.
    const bool fsr3_blocked =
        fsr4.HoldsMemory() ||
        (fsr4_state == ContextState::Loading && (fsr4.AssetsReady() || !context.HoldsMemory()));
    std::optional<ContextKey> wanted;
    if (settings.grr_enable && session_fsr && !fsr3_blocked) {
        wanted = ContextKey{
            .max_render_size = session.render,
            .max_output_size = session.output,
            .fp16 = context.IsFp16Supported() && !settings.fp32,
            .debug_checking = settings.validate,
        };
    }
    context_state =
        context.Update(rasterizer.GetScheduler(), wanted,
                       Passes::EstimateBytes(session.render, session.output, true, true));

    // Frames of other sizes keep the contexts of the first sizes and the stock FXAA. Without FSR
    // an upscaled frame takes the fallback pass, bilinear like the stock FXAA of these frames, or
    // EASU.
    const bool session_frame = session_sizes && sizes == *session_sizes;
    const Mode fallback =
        !stock && session_frame && frame.kind == Kind::Upscale ? Mode::Fallback : Mode::Stock;
    Mode mode = fallback;
    if (!stock) {
        if (settings.debug >= DebugView::MotionVectors &&
            settings.debug <= DebugView::Reprojection) {
            mode = Mode::Debug;
        } else if (fsr4_state == ContextState::Ready && session_frame) {
            mode = Mode::Fsr4;
        } else if (context_state == ContextState::Ready && session_frame) {
            mode = Mode::Fsr;
        }
    }
    if (mode != fallback && !previous_camera.valid) {
        stock = StockReason::NoPreviousCamera;
        mode = fallback;
    }
    if (stock) {
        LogStock(*stock);
    }
    frame.masks = settings.reactive_characters > 0 || settings.tc_characters > 0 ||
                  (settings.test & TestBit::CharacterMaskHistory) != 0;

    if (mode != Mode::Stock && !passes && !passes_failed) {
        passes = std::make_unique<Passes>(instance);
        if (!passes->Init()) {
            passes.reset();
            passes_failed = true;
            LOG_ERROR(Render_Vulkan, "GRR FSR: host passes are unavailable, FXAA stays stock");
        }
    }
    if (!passes) {
        mode = Mode::Stock;
    }
    if (mode != Mode::Stock && !grr_passes_failed && !passes->InitGrr()) {
        grr_passes_failed = true;
        LOG_ERROR(Render_Vulkan, "GRR FSR: the camera-motion pass is unavailable, FSR stays off");
    }
    if (grr_passes_failed) {
        mode = fallback;
    }
    if (mode == Mode::Fsr4 && !passes->InitFsr4()) {
        // From the next frame no FSR 4 context is wanted, and FSR 3.1 can take over.
        fsr4_passes_failed = true;
        LOG_ERROR(Render_Vulkan, "GRR FSR: the FSR 4 inputs pass is unavailable, FSR 4 stays off");
        mode = fallback;
    }

    // History restarts on a new context, after a frame without this upscaler, after a gap, at a
    // camera cut, and when the colour space or the test bits change.
    const auto restarts = [&](u64 current_generation, u64 seen_generation, bool last_frame) {
        return current_generation != seen_generation || !last_frame ||
               frame.now - last_fsr_time > ResetGap ||
               (frame.delta.cut && (settings.test & TestBit::CutDetectionOff) == 0) ||
               settings.color_space != last_fsr_settings.color_space ||
               settings.test != last_fsr_settings.test;
    };
    frame.reset = mode == Mode::Fsr4
                      ? restarts(fsr4.Generation(), fsr4_generation, last_frame_fsr4)
                      : restarts(context.Generation(), context_generation, last_frame_fsr);

    const Mode executed = Record(mode, frame, rasterizer);

    Jitter::FramePlan plan{.scene_depth_address = capture.SceneDepthAddress()};
    if (executed == Mode::Fsr || executed == Mode::Fsr4) {
        const bool fsr4_frame = executed == Mode::Fsr4;
        const s32 phase_count = fsr4_frame ? Jitter::Fsr4PhaseCount(render.width, output.width)
                                           : Jitter::PhaseCount(render.width, output.width);
        const u64 executed_generation = fsr4_frame ? fsr4.Generation() : context.Generation();
        u64& seen_generation = fsr4_frame ? fsr4_generation : context_generation;
        if (executed_generation != seen_generation) {
            phase_index = 0;
        }
        phase_index = (phase_index + 1) % phase_count;
        last_fsr_time = frame.now;
        last_fsr_settings = settings;
        seen_generation = executed_generation;
        ++fsr_dispatches;
        if ((settings.test & TestBit::JitterOff) == 0) {
            plan.enabled = true;
            plan.offset = Jitter::PhaseOffset(phase_index, phase_count);
            if (frame.kind == Kind::Upscale && settings.lod_bias) {
                const float ratio =
                    static_cast<float>(render.width) / static_cast<float>(output.width);
                plan.lod_bias =
                    std::log2(ratio) - 0.5f * static_cast<float>(settings.lod_bias_extra);
            }
        }
    } else if (coverage && executed != Mode::Debug) {
        plan.enabled = true;
        plan.offset = (frame_index & 1) != 0 ? Jitter::Offset{CoverageOffset, CoverageOffset}
                                             : Jitter::Offset{-CoverageOffset, -CoverageOffset};
    }
    last_frame_fsr = executed == Mode::Fsr;
    last_frame_fsr4 = executed == Mode::Fsr4;
    Jitter::BeginFrame(plan);
    previous_camera = frame.camera;
    capture.StartFrame();
    Publish(executed, frame, interval_ms);
    if (log_this) {
        LogFrame(frame_index, executed, frame, interval_ms);
    }
    return executed != Mode::Stock;
}

Mode GrrModule::Record(Mode mode, const GrrFrame& frame, Vulkan::Rasterizer& rasterizer) {
    if (mode == Mode::Stock) {
        return mode;
    }
    auto& scheduler = rasterizer.GetScheduler();
    // A wait for a free FSR 4 frame slot can submit the open command buffer, so it comes first.
    if (mode == Mode::Fsr4 && !fsr4.PrepareFrame(rasterizer)) {
        if (frame.kind == Kind::Native) {
            return Mode::Stock;
        }
        mode = Mode::Fallback;
    }
    scheduler.EndRendering();
    GrrImages images{};
    const bool acquired = capture.Acquire(frame.draw, frame.scene, rasterizer, images);
    // Flushed even when Acquire fails: the tracked layouts include every queued transition. A
    // stock frame then transitions the bindings of its draw again.
    rasterizer.GetRuntime().FlushBarriers();
    if (!acquired) {
        LogStock(StockReason::Images);
        return Mode::Stock;
    }

    rasterizer.ScopeMarkerBegin(mode == Mode::Fsr4 ? "Host/GRR FSR4" : "Host/GRR FSR3");
    const Vulkan::CommandRecorder cmdbuf = scheduler.CommandBuffer();
    if (mode == Mode::Fsr) {
        mode = RecordFsr(cmdbuf, frame, images, scheduler);
    } else if (mode == Mode::Fsr4) {
        mode = RecordFsr4(cmdbuf, frame, images, scheduler);
    } else if (mode == Mode::Fallback) {
        passes->RecordFallback(cmdbuf, FallbackOf(frame, images, frame.settings.fallback_easu));
    } else {
        RecordDebug(cmdbuf, frame, images, scheduler);
    }
    // FSR binds compute pipelines and descriptor sets; the passes push descriptors and constants.
    auto& skipcache = VideoCore::Skipcache::Framework::Instance();
    skipcache.BumpForeignPipelineGen(1);
    skipcache.BumpForeignPushGen(1);
    rasterizer.ScopeMarkerEnd();
    return mode;
}

Mode GrrModule::RecordFsr(const Vulkan::CommandRecorder& cmdbuf, const GrrFrame& frame,
                          const GrrImages& images, Vulkan::Scheduler& scheduler) {
    const Settings::Values& settings = frame.settings;
    const vk::Extent2D render = frame.draw.render_size;
    const vk::Extent2D output = frame.draw.output_size;
    passes->Resize(scheduler, render, output, true, false, true);
    if (frame.reset) {
        passes->ResetHistory();
    }
    passes->RecordGrrCameraMotion(cmdbuf, CameraMotionOf(frame, images));
    if (frame.masks) {
        passes->RecordMask(cmdbuf, MasksOf(frame, images));
    }
    passes->PrepareFsr(cmdbuf);

    const MotionTerms motion = MotionTermsOf(frame);
    const float frame_time_ms = Milliseconds(frame.now - last_fsr_time);
    const FsrDispatch dispatch{
        .color =
            {
                .image = images.color.image->GetImage(),
                .format = settings.color_space == ColorSpace::Linear ? vk::Format::eR8G8B8A8Srgb
                                                                     : vk::Format::eR8G8B8A8Unorm,
                .extent = images.color.extent,
                .usage = images.color.usage,
            },
        .depth = passes->DepthCopy(),
        .motion_vectors = passes->CameraMotionVectors(),
        .reactive = frame.masks ? passes->ReactiveMask() : FsrImage{},
        .transparency =
            frame.masks && settings.tc_characters > 0 ? passes->TransparencyMask() : FsrImage{},
        .output = passes->Output(),
        .jitter_x = motion.jitter_x,
        .jitter_y = motion.jitter_y,
        .motion_vector_scale_x = motion.motion_vector_scale_x,
        .motion_vector_scale_y = motion.motion_vector_scale_y,
        .render_size = render,
        .output_size = output,
        .frame_time_ms = frame_time_ms > 0.0f && frame_time_ms <= MaxFrameTimeMs
                             ? frame_time_ms
                             : DefaultFrameTimeMs,
        .camera = frame.camera.params,
        .reset = frame.reset,
        .frame_id = frame_id++,
    };
    if (!context.RecordDispatch(scheduler, dispatch)) {
        const bool native = frame.kind == Kind::Native;
        if (!std::exchange(logged_rejected, true)) {
            LOG_ERROR(Render_Vulkan, "GRR FSR: FSR rejected the dispatch, the frame uses the {}",
                      native ? "stock FXAA" : "fallback pass");
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
    passes->RecordStore(cmdbuf, StoreOf(frame, images, {}));
    return Mode::Fsr;
}

Mode GrrModule::RecordFsr4(const Vulkan::CommandRecorder& cmdbuf, const GrrFrame& frame,
                           const GrrImages& images, Vulkan::Scheduler& scheduler) {
    const Settings::Values& settings = frame.settings;
    const vk::Extent2D render = frame.draw.render_size;
    passes->ResizeFsr4(scheduler, render, true, true);
    passes->RecordGrrCameraMotion(cmdbuf, CameraMotionOf(frame, images));
    // A copy: a rejected dispatch retires the context together with its images.
    const Fsr4Images targets = fsr4.Images();
    passes->RecordFsr4Inputs(
        cmdbuf, Fsr4InputsParams{
                    .color = images.color.view,
                    .targets = targets,
                    .render_size = render,
                    .near_plane = frame.camera.params.near_plane,
                    .far_plane = frame.camera.params.far_plane,
                    .camera_motion = true,
                    .host_depth = true,
                    .encode_srgb = settings.color_space == ColorSpace::Gamma,
                    .depth_inverted = (settings.test & TestBit::Fsr4DepthInverted) != 0,
                    .depth_linear = (settings.test & TestBit::Fsr4DepthLinear) != 0,
                });

    const MotionTerms motion = MotionTermsOf(frame);
    const Fsr4Dispatch dispatch{
        .jitter_x = motion.jitter_x,
        .jitter_y = motion.jitter_y,
        .motion_vector_scale_x = motion.motion_vector_scale_x,
        .motion_vector_scale_y = motion.motion_vector_scale_y,
        .render_size = render,
        .output_size = frame.draw.output_size,
        .reset = frame.reset,
    };
    if (!fsr4.RecordDispatch(scheduler, dispatch)) {
        const bool native = frame.kind == Kind::Native;
        if (!std::exchange(logged_fsr4_rejected, true)) {
            LOG_ERROR(Render_Vulkan, "GRR FSR: FSR 4 rejected the dispatch, the frame uses the {}",
                      native ? "stock FXAA" : "fallback pass");
        }
        if (native) {
            return Mode::Stock;
        }
        passes->RecordFallback(cmdbuf, FallbackOf(frame, images, settings.fallback_easu));
        return Mode::Fallback;
    }
    if (settings.debug == DebugView::Bypass) {
        passes->RecordFallback(cmdbuf, FallbackOf(frame, images, false));
        return Mode::Fsr4;
    }
    passes->RecordStore(cmdbuf, StoreOf(frame, images, targets.output));
    return Mode::Fsr4;
}

void GrrModule::RecordDebug(const Vulkan::CommandRecorder& cmdbuf, const GrrFrame& frame,
                            const GrrImages& images, Vulkan::Scheduler& scheduler) {
    const DebugView view = frame.settings.debug;
    const vk::Extent2D render = frame.draw.render_size;
    // The debug pass writes the guest image. The camera-motion pass runs for every view, the mask
    // pass for the Masks view, and the Reprojection view keeps a history, all at the render size.
    passes->Resize(scheduler, render, {}, true, view == DebugView::Reprojection, true);
    passes->RecordGrrCameraMotion(cmdbuf, CameraMotionOf(frame, images));
    if (view == DebugView::Masks) {
        passes->RecordMask(cmdbuf, MasksOf(frame, images));
    }
    passes->RecordDebug(cmdbuf, DebugParams{
                                    .view = view,
                                    .color = images.color.view,
                                    .motion_vectors = passes->CameraMotionView(),
                                    .stencil = images.stencil.view,
                                    .depth_copy = passes->DepthCopyView(),
                                    .depth_main = images.depth.view,
                                    .destination = images.output.view,
                                    .source_size = render,
                                    .destination_size = frame.draw.output_size,
                                    .camera = frame.camera.params,
                                    .motion_vector_scale_x = 1.0f,
                                    .motion_vector_scale_y = 1.0f,
                                    .stencil_bits = {0x02, GrrCharacterStencil, 0x7d},
                                    .components = frame.draw.components,
                                });
}

void GrrModule::LogStock(StockReason reason) {
    const auto index = static_cast<std::size_t>(reason);
    if (!std::exchange(logged_stock[index], true)) {
        LOG_WARNING(Render_Vulkan, "GRR FSR: no FSR for the FXAA draw: {}",
                    StockReasonNames[index]);
    }
}

void GrrModule::Publish(Mode mode, const GrrFrame& frame, float interval_ms) {
    const vk::Extent2D render = frame.draw.render_size;
    const vk::Extent2D output = frame.draw.output_size;
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
        .fsr4_context = fsr4_state,
        .fsr4_bytes = fsr4.MemoryBytes(),
        .fsr4_model = fsr4_key ? Fsr4PresetName(fsr4_key->preset) : "",
        .fsr4_tier = fsr4_key ? Fsr4TierName(fsr4_key->max_output_size) : "",
        .fsr4_notice = fsr4.Notice(),
    });
    // Each distinct change is logged once, so modes alternating per frame cannot flood the log.
    constexpr u32 ModeCount = static_cast<u32>(magic_enum::enum_count<Mode>());
    const u32 change = 1u << (static_cast<u32>(last_mode) * ModeCount + static_cast<u32>(mode));
    if (mode != last_mode && (logged_mode_changes & change) == 0) {
        logged_mode_changes |= change;
        LOG_INFO(Render_Vulkan, "GRR FSR: FXAA {}x{} -> {}x{}: mode {} -> {}", render.width,
                 render.height, output.width, output.height, magic_enum::enum_name(last_mode),
                 magic_enum::enum_name(mode));
    }
    last_mode = mode;
}

void GrrModule::LogFrame(u64 frame_index, Mode mode, const GrrFrame& frame,
                         float interval_ms) const {
    const vk::Extent2D render = frame.draw.render_size;
    const vk::Extent2D output = frame.draw.output_size;
    const CameraParams& camera = frame.camera.params;
    const GrrCameraDelta& delta = frame.delta;
    const Jitter::FrameReport& jitter = frame.report;
    const u32 num_hashes =
        std::min(jitter.num_vertex_hashes, static_cast<u32>(jitter.vertex_hashes.size()));
    LOG_INFO(Render_Vulkan,
             "GRR FSR: frame {} mode {} kind {} context {} fsr4 {} render {}x{} output {}x{} "
             "interval {:.2f} ms reset {} | camera near {} far {} vertical fov {} valid {} move "
             "{:.4f} turn {:.4f} deg fov delta {:.4f} cut {} | jitter window {} offset ({:.4f}, "
             "{:.4f}) draws eligible {} jittered {} indirect {} vertex shaders [{:#x}]",
             frame_index, magic_enum::enum_name(mode), magic_enum::enum_name(frame.kind),
             magic_enum::enum_name(context_state), magic_enum::enum_name(fsr4_state), render.width,
             render.height, output.width, output.height, interval_ms,
             (mode == Mode::Fsr || mode == Mode::Fsr4) && frame.reset, camera.near_plane,
             camera.far_plane, camera.vertical_fov, camera.valid, delta.translation,
             delta.rotation_degrees, delta.fov_delta, delta.cut, jitter.window_opened,
             jitter.applied.x, jitter.applied.y, jitter.eligible_draws, jitter.jittered_draws,
             jitter.indirect_draws,
             fmt::join(std::span<const u64>{jitter.vertex_hashes.data(), num_hashes}, ", "));
}

} // namespace

namespace Detail {

void OnGrrDispatch(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                   Vulkan::Rasterizer& rasterizer) {
    if (info.pgm_hash == GrrHash::ExposureCs) {
        Grr(rasterizer).OnExposure(info, cs_program, rasterizer);
    }
}

bool OnGrrDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
               Vulkan::Rasterizer& rasterizer, bool is_indirect) {
    return Grr(rasterizer).OnDraw(pipeline, liverpool, rasterizer, is_indirect);
}

} // namespace Detail

} // namespace Gr2Fsr3
