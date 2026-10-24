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
#include "video_core/gr2_fsr3/fsr4_context.h"
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

constexpr std::array<std::string_view, 5> ModeNames{"stock", "fsr", "fallback", "debug", "fsr4"};
constexpr std::array<std::string_view, 3> KindNames{"native", "upscale", "other"};

constexpr u64 PeriodicLogFrames = 300;
constexpr size_t MaxLoggedSizes = 16;
constexpr auto ResetGap = std::chrono::milliseconds{100};
constexpr float MaxFrameTimeMs = 200.0f;
constexpr float DefaultFrameTimeMs = 16.6f;
constexpr float CoverageOffset = 2.0f;

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
    if (mode == Mode::Fsr || mode == Mode::Fsr4) {
        return {
            .color = true,
            .velocity = true,
            .stencil = (mode == Mode::Fsr && frame.masks) || frame.camera_motion ||
                       frame.settings.fxaa == Fxaa::Characters,
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

// The jitter and motion-vector scale that FSR 3.1 and FSR 4 receive.
struct MotionTerms {
    float jitter_x{};
    float jitter_y{};
    float motion_vector_scale_x{};
    float motion_vector_scale_y{};
};

MotionTerms MotionTermsOf(const Frame& frame) {
    const u32 test = frame.settings.test;
    const vk::Extent2D render = frame.sizes.render;
    const float jitter_sign = (test & TestBit::JitterSignFlip) != 0 ? -1.0f : 1.0f;
    const bool repeated = frame.camera.repeated && (test & TestBit::RepeatZeroingOff) == 0;
    const float mv_scale_y = (test & TestBit::MotionVectorYFlip) != 0 ? 0.5f : -0.5f;
    return {
        .jitter_x = jitter_sign * frame.report.applied.x,
        .jitter_y = jitter_sign * frame.report.applied.y,
        .motion_vector_scale_x = repeated ? 0.0f : 0.5f * static_cast<float>(render.width),
        .motion_vector_scale_y = repeated ? 0.0f : mv_scale_y * static_cast<float>(render.height),
    };
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

CameraMotionParams CameraMotionOf(const Frame& frame, const FrameImages& images,
                                  const VelocityCapture& capture) {
    CameraMotionParams params{
        .motion_vectors = images.velocity.view,
        .depth = images.depth_copy.view,
        .stencil = images.stencil.view,
        .render_size = frame.sizes.render,
    };
    std::memcpy(params.matrix.data(), capture.cb.data(), sizeof(params.matrix));
    std::memcpy(params.inverse_view.data(), capture.cb.data() + params.matrix.size(),
                sizeof(params.inverse_view));
    return params;
}

StoreParams StoreOf(const Frame& frame, const FrameImages& images, const Fsr4Image& source) {
    const float sharpness = static_cast<float>(frame.settings.sharpness) / 100.0f;
    return {
        .destination = images.output.view,
        .output_size = frame.sizes.output,
        .rcas_stops = sharpness > 0.0f ? 2.0f * (1.0f - sharpness) : -1.0f,
        .linear_input = frame.settings.color_space == ColorSpace::Linear,
        .source = source,
        .fxaa = frame.settings.fxaa,
        .stencil = images.stencil.view,
        .render_size = frame.sizes.render,
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
    explicit Module(const Vulkan::Instance& instance)
        : instance{instance}, context{instance}, fsr4{instance} {
        Jitter::SetMaxLodBias(instance.MaxSamplerLodBias());
    }

    bool OnDispatch(size_t tracked, const Shader::Info& info,
                    const AmdGpu::ComputeProgram& cs_program, Vulkan::Rasterizer& rasterizer);

private:
    bool Antialias(const Shader::Info& info, Vulkan::Rasterizer& rasterizer);
    Mode Record(Mode mode, const Frame& frame, Vulkan::Rasterizer& rasterizer);
    Mode RecordFsr(const Vulkan::CommandRecorder& cmdbuf, const Frame& frame,
                   const FrameImages& images, Vulkan::Scheduler& scheduler);
    Mode RecordFsr4(const Vulkan::CommandRecorder& cmdbuf, const Frame& frame,
                    const FrameImages& images, Vulkan::Scheduler& scheduler);
    void ArmJitter(Vulkan::Rasterizer& rasterizer, const Jitter::FramePlan& plan);
    void Publish(Mode mode, const Frame& frame, float interval_ms);

    const Vulkan::Instance& instance;
    Inputs inputs;
    Context context;
    Fsr4Context fsr4;
    std::unique_ptr<Passes> passes;
    bool passes_failed{};
    ContextState context_state{ContextState::Idle};
    ContextState fsr4_state{ContextState::Idle};
    std::optional<Fsr4Key> fsr4_key;
    bool fsr4_passes_failed{};
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
    bool last_frame_fsr4{};
    u64 context_generation{};
    u64 fsr4_generation{};
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
    bool logged_fsr4_rejected{};
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
        last_frame_fsr4 = false;
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
    // A predicate search: the MSVC STL vectorizes ranges::contains for trivially comparable
    // elements and fails to compile it for this 16-byte type.
    const auto same_sizes = [&frame](const SizePair& sizes) { return sizes == frame.sizes; };
    if (logged_sizes.size() < MaxLoggedSizes && !std::ranges::any_of(logged_sizes, same_sizes)) {
        logged_sizes.push_back(frame.sizes);
        LOG_INFO(Render_Vulkan, "GR2 FSR3: Antialias {}x{} -> {}x{} ({}){}",
                 frame.sizes.render.width, frame.sizes.render.height, frame.sizes.output.width,
                 frame.sizes.output.height, KindNames[static_cast<size_t>(frame.kind)],
                 session_frame ? "" : ", not the first sizes: FSR stays off for these frames");
    }
    frame.camera = inputs.EvaluateCamera(frame.capture_current);

    const bool coverage = (settings.test & TestBit::CoverageJitter) != 0;
    const bool session_fsr = !coverage && !cbresolve && KindOf(*session_sizes) != Kind::Other;
    fsr4_key.reset();
    if (settings.fsr4_enable && session_fsr && !fsr4_passes_failed) {
        fsr4_key = Fsr4KeyFor(session_sizes->render, session_sizes->output, settings.fsr4_model);
    }
    fsr4_state = fsr4.Update(
        rasterizer.GetScheduler(), fsr4_key, !context.HoldsMemory(),
        Passes::EstimateFsr4Bytes(session_sizes->render, settings.character_camera_motion));
    // FSR 3.1 holds no context while FSR 4 holds one or its files have passed their checks; while
    // the files load, a context FSR 3.1 already has keeps running.
    const bool fsr3_blocked =
        fsr4.HoldsMemory() ||
        (fsr4_state == ContextState::Loading && (fsr4.AssetsReady() || !context.HoldsMemory()));
    std::optional<ContextKey> wanted;
    if (settings.enable && session_fsr && !fsr3_blocked) {
        wanted = ContextKey{
            .max_render_size = session_sizes->render,
            .max_output_size = session_sizes->output,
            .fp16 = context.IsFp16Supported() && !settings.fp32,
            .debug_checking = settings.validate,
        };
    }
    context_state =
        context.Update(rasterizer.GetScheduler(), wanted,
                       Passes::EstimateBytes(session_sizes->render, session_sizes->output,
                                             settings.character_camera_motion));

    frame.masks = settings.reactive_characters > 0 || settings.reactive_effects > 0 ||
                  settings.reactive_outline > 0 || settings.tc_characters > 0 ||
                  (settings.test & TestBit::CharacterMaskHistory) != 0;
    frame.camera_motion =
        settings.character_camera_motion && frame.capture_current && capture.cb_valid;
    // The copy needs this frame's Velocity capture. FSR barriers only the depth aspect of the main
    // depth-stencil image, which is valid only with separateDepthStencilLayouts.
    frame.depth_main = context.IsSeparateDepthStencilLayoutsSupported() &&
                       (settings.depth_source == DepthSource::Main || !frame.capture_current);
    // History restarts on a new context, after a frame without this upscaler, after a gap, at a
    // camera cut, and when the colour space, the test bits or the depth source change.
    const auto restarts = [&](u64 current_generation, u64 seen_generation, bool last_frame) {
        return current_generation != seen_generation || !last_frame ||
               frame.now - last_fsr_time > ResetGap ||
               (frame.camera.cut && (settings.test & TestBit::CutDetectionOff) == 0) ||
               settings.color_space != last_fsr_settings.color_space ||
               settings.test != last_fsr_settings.test || frame.depth_main != last_fsr_depth_main;
    };
    const u64 generation = context.Generation();
    frame.reset = restarts(generation, context_generation, last_frame_fsr);

    const bool fsr4_ready = fsr4_state == ContextState::Ready && session_frame;
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
    } else if (fsr4_ready) {
        mode = Mode::Fsr4;
    } else if (fsr_ready && has_depth) {
        mode = Mode::Fsr;
    } else if (frame.kind == Kind::Native) {
        mode = Mode::Stock;
    }
    if (mode == Mode::Fsr4) {
        // The inputs pass samples either depth image, so FSR 4 reads the main depth without
        // separate depth/stencil layouts.
        frame.depth_main = settings.depth_source == DepthSource::Main || !frame.capture_current;
        frame.reset = restarts(fsr4.Generation(), fsr4_generation, last_frame_fsr4);
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
    if (mode == Mode::Fsr4 && !passes->InitFsr4()) {
        // From the next frame no FSR 4 context is wanted, and FSR 3.1 can take over.
        fsr4_passes_failed = true;
        LOG_ERROR(Render_Vulkan, "GR2 FSR4: the FSR 4 inputs pass is unavailable, FSR 4 stays off");
        mode = frame.kind == Kind::Native ? Mode::Stock : Mode::Fallback;
    }

    const Mode executed = Record(mode, frame, rasterizer);

    Jitter::FramePlan plan{.scene_stencil_address = frame.bindings.stencil.tsharp.Address()};
    if (executed == Mode::Fsr || executed == Mode::Fsr4) {
        const bool fsr4_frame = executed == Mode::Fsr4;
        const s32 phase_count =
            fsr4_frame ? Jitter::Fsr4PhaseCount(frame.sizes.render.width, frame.sizes.output.width)
                       : Jitter::PhaseCount(frame.sizes.render.width, frame.sizes.output.width);
        const u64 executed_generation = fsr4_frame ? fsr4.Generation() : generation;
        u64& seen_generation = fsr4_frame ? fsr4_generation : context_generation;
        if (executed_generation != seen_generation) {
            phase_index = 0;
        }
        phase_index = (phase_index + 1) % phase_count;
        last_fsr_time = frame.now;
        last_fsr_settings = settings;
        last_fsr_depth_main = frame.depth_main;
        seen_generation = executed_generation;
        ++fsr_dispatches;
        if ((settings.test & TestBit::JitterOff) == 0) {
            plan.enabled = true;
            plan.offset = Jitter::PhaseOffset(phase_index, phase_count);
            if (frame.kind == Kind::Upscale && settings.lod_bias) {
                const float ratio = static_cast<float>(frame.sizes.render.width) /
                                    static_cast<float>(frame.sizes.output.width);
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
    ArmJitter(rasterizer, plan);
    Publish(executed, frame, interval_ms);
    if (log_this) {
        inputs.LogFrame(FrameLog{
            .frame = frame_index,
            .mode = executed,
            .kind = frame.kind,
            .context = context_state,
            .fsr4_context = fsr4_state,
            .render_size = frame.sizes.render,
            .output_size = frame.sizes.output,
            .interval_ms = interval_ms,
            .reset = (executed == Mode::Fsr || executed == Mode::Fsr4) && frame.reset,
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
    // A wait for a free FSR 4 frame slot can submit the open command buffer, so it comes first.
    if (mode == Mode::Fsr4 && !fsr4.PrepareFrame(rasterizer)) {
        if (frame.kind == Kind::Native) {
            return Mode::Stock;
        }
        mode = Mode::Fallback;
    }
    scheduler.EndRendering();
    FrameImages images{};
    bool acquired = inputs.Acquire(frame.bindings, RequestFor(mode, frame), rasterizer, images);
    if (!acquired && (mode == Mode::Fsr || mode == Mode::Fsr4) && frame.kind != Kind::Native) {
        mode = Mode::Fallback;
        acquired = inputs.Acquire(frame.bindings, RequestFor(mode, frame), rasterizer, images);
    }
    // Flushed even when Acquire fails: the tracked layouts include every queued transition. A
    // stock frame then transitions its own bindings again.
    rasterizer.GetRuntime().FlushBarriers();
    if (!acquired) {
        return Mode::Stock;
    }

    rasterizer.ScopeMarkerBegin(mode == Mode::Fsr4 ? "Host/FSR4" : "Host/FSR3");
    const Vulkan::CommandRecorder cmdbuf = scheduler.CommandBuffer();
    if (mode == Mode::Fsr) {
        mode = RecordFsr(cmdbuf, frame, images, scheduler);
    } else if (mode == Mode::Fsr4) {
        mode = RecordFsr4(cmdbuf, frame, images, scheduler);
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

Mode Module::RecordFsr(const Vulkan::CommandRecorder& cmdbuf, const Frame& frame,
                       const FrameImages& images, Vulkan::Scheduler& scheduler) {
    const Settings::Values& settings = frame.settings;
    const auto& [render, output] = frame.sizes;
    passes->Resize(scheduler, render, output, settings.character_camera_motion, false);
    if (frame.reset) {
        passes->ResetHistory();
    }
    if (frame.masks) {
        passes->RecordMask(cmdbuf, MasksOf(frame, images));
    }
    if (frame.camera_motion) {
        passes->RecordCameraMotion(cmdbuf, CameraMotionOf(frame, images, inputs.Velocity()));
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
    const MotionTerms motion = MotionTermsOf(frame);
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
    passes->RecordStore(cmdbuf, StoreOf(frame, images, {}));
    return Mode::Fsr;
}

Mode Module::RecordFsr4(const Vulkan::CommandRecorder& cmdbuf, const Frame& frame,
                        const FrameImages& images, Vulkan::Scheduler& scheduler) {
    const Settings::Values& settings = frame.settings;
    const auto& [render, output] = frame.sizes;
    passes->ResizeFsr4(scheduler, render, settings.character_camera_motion);
    if (frame.camera_motion) {
        passes->RecordCameraMotion(cmdbuf, CameraMotionOf(frame, images, inputs.Velocity()));
    }
    // A copy: a rejected dispatch retires the context together with its images.
    const Fsr4Images targets = fsr4.Images();
    const GuestImage& depth = frame.depth_main ? images.depth_main : images.depth_copy;
    passes->RecordFsr4Inputs(
        cmdbuf, Fsr4InputsParams{
                    .color = images.color.view,
                    .motion_vectors = images.velocity.view,
                    .depth = depth.view,
                    .targets = targets,
                    .render_size = render,
                    .near_plane = frame.camera.params.near_plane,
                    .far_plane = frame.camera.params.far_plane,
                    .camera_motion = frame.camera_motion,
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
        .output_size = output,
        .reset = frame.reset,
    };
    if (!fsr4.RecordDispatch(scheduler, dispatch)) {
        const bool native = frame.kind == Kind::Native;
        if (!logged_fsr4_rejected) {
            logged_fsr4_rejected = true;
            LOG_ERROR(Render_Vulkan,
                      "GR2 FSR4: FSR 4 rejected the dispatch, frame uses the {} pass",
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
        return Mode::Fsr4;
    }
    passes->RecordStore(cmdbuf, StoreOf(frame, images, targets.output));
    return Mode::Fsr4;
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
        .fsr4_context = fsr4_state,
        .fsr4_bytes = fsr4.MemoryBytes(),
        .fsr4_model = fsr4_key ? Fsr4PresetName(fsr4_key->preset) : "",
        .fsr4_tier = fsr4_key ? Fsr4TierName(fsr4_key->max_output_size) : "",
        .fsr4_notice = fsr4.Notice(),
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

TitleState ResolveTitle() {
    if (Common::IsGravityRush2()) {
        g_title_state = TitleState::Gr2;
        LOG_INFO(Render_Vulkan, "GR2 FSR3: active for {}",
                 Common::ElfInfo::Instance().GameSerial());
    } else if (Common::IsGravityRushRemastered()) {
        g_title_state = TitleState::Grr;
        LOG_INFO(Render_Vulkan, "GRR FSR: active for {}", Common::ElfInfo::Instance().GameSerial());
    } else {
        g_title_state = TitleState::Inactive;
    }
    return g_title_state;
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
