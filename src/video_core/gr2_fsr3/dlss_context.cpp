// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "common/logging/log.h"
#include "video_core/gr2_fsr3/context_images.h"
#include "video_core/gr2_fsr3/dlss_context.h"
#include "video_core/gr2_fsr3/fsr3_context.h"
#include "video_core/gr2_fsr3/fsr_worker.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/skipcache/skipcache.h"

namespace Gr2Fsr3 {

namespace {

using Clock = std::chrono::steady_clock;

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

// The four images take 16 bytes per render pixel and 8 per output pixel; NGX's own memory is
// estimated at 64 bytes per output pixel.
u64 EstimateBytes(const DlssKey& key) {
    const u64 render_pixels = u64{key.render_size.width} * key.render_size.height;
    const u64 output_pixels = u64{key.output_size.width} * key.output_size.height;
    return 16 * render_pixels + 72 * output_pixels;
}

bool Contains(const DlssRange& range, vk::Extent2D size) {
    return size.width >= range.min.width && size.width <= range.max.width &&
           size.height >= range.min.height && size.height <= range.max.height;
}

std::string Extent(vk::Extent2D size) {
    return fmt::format("{}x{}", size.width, size.height);
}

} // namespace

struct DlssContext::Impl {
    // A context the GPU may still use, released after it passes `tick`.
    struct Retired {
        ContextImages images;
        u64 tick{};
        u64 generation{};
        bool evaluated{};
    };

    explicit Impl(const Vulkan::Instance& instance) : instance{instance} {}

    ContextState Update(Vulkan::Scheduler& scheduler, const std::optional<DlssKey>& wanted,
                        bool may_allocate, u64 extra_bytes);
    bool RecordDispatch(Vulkan::Scheduler& scheduler, const Fsr4Dispatch& dispatch);
    void Create(const DlssKey& key, u64 extra_bytes);
    void Fail(const DlssKey& key, std::string text);
    void Retire(Vulkan::Scheduler& scheduler);
    void ReleaseRetired(Vulkan::Scheduler& scheduler);
    bool TakeRecordedFailure(Vulkan::Scheduler& scheduler);

    const Vulkan::Instance& instance;
    std::optional<ContextImages> current;
    DlssKey current_key{};
    std::optional<Retired> retired;
    std::optional<DlssKey> failed_key;
    std::string notice; ///< Why failed_key failed.
    bool key_wanted{};
    bool waiting_for_fsr{};
    bool evaluated{}; ///< An evaluation of the current context was recorded.
    u64 generation{};
    std::atomic<u64> feature_bytes{};
    // Releases recorded on the record thread that have yet to run. A creation starts at zero.
    std::atomic<u32> destroying{};
    RecordedFailure recorded_failure;
};

ContextState DlssContext::Impl::Update(Vulkan::Scheduler& scheduler,
                                       const std::optional<DlssKey>& wanted, bool may_allocate,
                                       u64 extra_bytes) {
    key_wanted = wanted.has_value();
    if (!DlssRunning()) {
        return wanted ? ContextState::Unsupported : ContextState::Idle;
    }
    TakeRecordedFailure(scheduler);
    ReleaseRetired(scheduler);
    // A changed key, or DLSS turned off and on again, retries.
    if (failed_key != wanted) {
        failed_key.reset();
        notice.clear();
    }
    if (current && wanted != current_key) {
        Retire(scheduler);
    }
    // A failed key keeps its reason while FSR runs in its place.
    waiting_for_fsr = wanted && !current && !failed_key && !may_allocate;
    if (wanted && !current && !failed_key && may_allocate && !retired && destroying.load() == 0) {
        Create(*wanted, extra_bytes);
    }
    if (!wanted) {
        return ContextState::Idle;
    }
    if (current) {
        return ContextState::Ready;
    }
    return failed_key ? ContextState::Failed : ContextState::Creating;
}

void DlssContext::Impl::Create(const DlssKey& key, u64 extra_bytes) {
    // No DLSS call runs on another thread now: nothing is current or retired and every recorded
    // release has run.
    std::string why;
    const std::optional<DlssRange> range =
        DlssOptimalRange(key.mode, key.preset, key.output_size, why);
    if (!range) {
        Fail(key, std::move(why));
        return;
    }
    LOG_INFO(Render_Vulkan, "GR2 DLSS: {} for {}: optimal {}, accepted {} to {}",
             DlssModeName(key.mode), Extent(key.output_size), Extent(range->optimal),
             Extent(range->min), Extent(range->max));
    if (!Contains(*range, key.render_size)) {
        Fail(key, fmt::format("DLSS {} accepts render sizes {} to {}, the game renders {}",
                              DlssModeName(key.mode), Extent(range->min), Extent(range->max),
                              Extent(key.render_size)));
        return;
    }
    const u64 needed = EstimateBytes(key) + extra_bytes + 64_MB;
    const u64 available = AvailableDeviceMemory(instance);
    if (needed > available) {
        Fail(key, fmt::format("DLSS needs about {} MiB of GPU memory, {} MiB available",
                              needed / 1_MB, available / 1_MB));
        return;
    }
    std::optional<ContextImages> images =
        CreateContextImages(instance, key.render_size, key.output_size, "GR2 DLSS", why);
    if (!images) {
        Fail(key, fmt::format("DLSS {}", why));
        return;
    }
    if (why = DlssSetOptions(key.mode, key.preset, key.output_size); !why.empty()) {
        Fail(key, std::move(why));
        return;
    }
    current = std::move(images);
    current_key = key;
    ++generation;
    evaluated = false;
    feature_bytes.store(0);
    LOG_INFO(Render_Vulkan, "GR2 DLSS: context {} ready, {}, preset {}, {} MiB of images",
             FormatSizes(key.render_size, key.output_size), DlssModeName(key.mode),
             magic_enum::enum_name(key.preset), current->bytes / 1_MB);
}

void DlssContext::Impl::Fail(const DlssKey& key, std::string text) {
    LOG_ERROR(Render_Vulkan, "GR2 DLSS: {}", text);
    failed_key = key;
    notice = std::move(text);
}

bool DlssContext::Impl::RecordDispatch(Vulkan::Scheduler& scheduler, const Fsr4Dispatch& dispatch) {
    const Fsr4Images& images = current->handles;
    // Streamline's own barrier before NGX carries no source access for GENERAL, so this one makes
    // the writes of the inputs pass available; Streamline's then makes them visible to NGX. The
    // output is written from scratch.
    const vk::MemoryBarrier2 inputs_written{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    const vk::ImageMemoryBarrier2 output_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images.output.image,
        .subresourceRange = ColorRange,
    };
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &inputs_written,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &output_barrier,
    });
    const DlssEvaluateInfo info{
        .images = images,
        .render_size = dispatch.render_size,
        .output_size = dispatch.output_size,
        .jitter_x = dispatch.jitter_x,
        .jitter_y = dispatch.jitter_y,
        // Streamline multiplies the scale by the render size again.
        .motion_vector_scale_x =
            dispatch.motion_vector_scale_x / static_cast<float>(dispatch.render_size.width),
        .motion_vector_scale_y =
            dispatch.motion_vector_scale_y / static_cast<float>(dispatch.render_size.height),
        .reset = dispatch.reset,
    };
    const bool first = !std::exchange(evaluated, true);
    scheduler.Record([info, first, generation = generation, failure = &recorded_failure,
                      bytes = &feature_bytes](vk::CommandBuffer cmdbuf) {
        if (failure->HasFailed(generation)) {
            return;
        }
        if (first) {
            LOG_INFO(Render_Vulkan, "GR2 DLSS: creating feature");
        }
        const Clock::time_point start = Clock::now();
        if (std::string error = DlssEvaluate(cmdbuf, info); !error.empty()) {
            failure->Report(generation, std::move(error));
            return;
        }
        if (first) {
            const u64 ngx = DlssFeatureBytes();
            bytes->store(ngx, std::memory_order_release);
            LOG_INFO(Render_Vulkan, "GR2 DLSS: feature created in {:.0f} ms, {} MiB of NGX memory",
                     std::chrono::duration<float, std::milli>(Clock::now() - start).count(),
                     ngx / 1_MB);
        }
    });
    // NGX changes command buffer state that the host must restore, so the graphics pipeline,
    // descriptors, constants and dynamic state are recorded again. The caller does the same for
    // the compute bind point after every mode.
    auto& skipcache = VideoCore::Skipcache::Framework::Instance();
    skipcache.BumpForeignPipelineGen(0);
    skipcache.BumpForeignPushGen(0);
    scheduler.GetDynamicState().Invalidate();
    return !TakeRecordedFailure(scheduler);
}

void DlssContext::Impl::Retire(Vulkan::Scheduler& scheduler) {
    const u64 tick = scheduler.CurrentTick();
    LOG_INFO(Render_Vulkan, "GR2 DLSS: context {} retired, released after GPU tick {}",
             FormatSizes(current_key.render_size, current_key.output_size), tick);
    retired = Retired{
        .images = std::move(*current),
        .tick = tick,
        .generation = generation,
        .evaluated = evaluated,
    };
    current.reset();
    evaluated = false;
    feature_bytes.store(0);
}

void DlssContext::Impl::ReleaseRetired(Vulkan::Scheduler& scheduler) {
    if (!retired || !scheduler.IsFree(retired->tick)) {
        return;
    }
    // Every recorded call of the retired context has run, so a late failure is final.
    if (const std::optional<std::string> message = recorded_failure.Take(retired->generation)) {
        LOG_ERROR(Render_Vulkan, "GR2 DLSS: {}", *message);
    }
    // slFreeResources releases the NGX feature at once; the GPU has finished its evaluations.
    if (retired->evaluated) {
        ++destroying;
        scheduler.Record([destroying = &destroying](vk::CommandBuffer) {
            DlssFree();
            --*destroying;
        });
    }
    retired.reset();
}

bool DlssContext::Impl::TakeRecordedFailure(Vulkan::Scheduler& scheduler) {
    if (!current) {
        return false;
    }
    std::optional<std::string> message = recorded_failure.Take(generation);
    if (!message) {
        return false;
    }
    LOG_ERROR(Render_Vulkan, "GR2 DLSS: context {}: {}",
              FormatSizes(current_key.render_size, current_key.output_size), *message);
    failed_key = current_key;
    notice = std::move(*message);
    Retire(scheduler);
    return true;
}

DlssKey DlssKeyFor(vk::Extent2D render_size, vk::Extent2D output_size, DlssPreset preset) {
    const float ratio = RoundedWidthRatio(render_size, output_size);
    const DlssMode mode = render_size == output_size ? DlssMode::Dlaa
                          : ratio >= 2.99f           ? DlssMode::UltraPerformance
                          : ratio >= 1.99f           ? DlssMode::Performance
                          : ratio >= 1.69f           ? DlssMode::Balanced
                                                     : DlssMode::Quality;
    return {.render_size = render_size, .output_size = output_size, .mode = mode, .preset = preset};
}

const char* DlssModeName(DlssMode mode) {
    switch (mode) {
    case DlssMode::Performance:
        return "Performance";
    case DlssMode::Balanced:
        return "Balanced";
    case DlssMode::Quality:
        return "Quality";
    case DlssMode::UltraPerformance:
        return "Ultra Performance";
    case DlssMode::Dlaa:
        return "DLAA";
    }
    return "";
}

DlssContext::DlssContext(const Vulkan::Instance& instance)
    : impl{std::make_unique<Impl>(instance)} {}

DlssContext::~DlssContext() = default;

ContextState DlssContext::Update(Vulkan::Scheduler& scheduler, const std::optional<DlssKey>& wanted,
                                 bool may_allocate, u64 extra_bytes) {
    return impl->Update(scheduler, wanted, may_allocate, extra_bytes);
}

bool DlssContext::HoldsMemory() const {
    return impl->current || impl->retired || impl->destroying.load() > 0;
}

u64 DlssContext::Generation() const {
    return impl->generation;
}

u64 DlssContext::MemoryBytes() const {
    if (!impl->current) {
        return 0;
    }
    return impl->current->bytes + impl->feature_bytes.load(std::memory_order_acquire);
}

std::string DlssContext::Notice() const {
    const Impl& state = *impl;
    if (!state.key_wanted) {
        return {};
    }
    if (!DlssRunning()) {
        return DlssBootNotice();
    }
    if (state.waiting_for_fsr) {
        return "DLSS: waiting for FSR to release its GPU memory";
    }
    return state.notice;
}

const Fsr4Images& DlssContext::Images() const {
    return impl->current->handles;
}

bool DlssContext::RecordDispatch(Vulkan::Scheduler& scheduler, const Fsr4Dispatch& dispatch) {
    return impl->RecordDispatch(scheduler, dispatch);
}

} // namespace Gr2Fsr3
