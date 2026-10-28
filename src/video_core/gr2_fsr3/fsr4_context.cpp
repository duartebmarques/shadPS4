// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "video_core/gr2_fsr3/context_images.h"
#include "video_core/gr2_fsr3/fsr3_context.h"
#include "video_core/gr2_fsr3/fsr3_settings.h"
#include "video_core/gr2_fsr3/fsr4_assets.h"
#include "video_core/gr2_fsr3/fsr4_context.h"
#include "video_core/gr2_fsr3/fsr4_device.h"
#include "video_core/gr2_fsr3/fsr_worker.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// <vulkan/vulkan.h> must first be included through vk_common.h, which configures it.
#include "video_core/gr2_fsr3/fsr4_v07_bridge.h"

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

// Indexed by GR2_FSR4_STAGE_*.
constexpr std::array<const char*, GR2_FSR4_STAGE_COUNT> StageNames{
    "none",        "out of host memory", "backend context", "provider context",
    "frame start", "image state",        "dispatch",
};

std::string StatusText(const Gr2Fsr4Status& status) {
    const char* stage = StageNames[status.stage];
    if (status.vk_result != 0) {
        return fmt::format("{}: {}", stage,
                           vk::to_string(static_cast<vk::Result>(status.vk_result)));
    }
    if (status.api_result != 0) {
        return fmt::format("{}: {}", stage,
                           status.api_result == GR2_FSR4_API_ERROR_PARAMETER ? "invalid parameter"
                                                                             : "error");
    }
    return stage;
}

// The provider allocates the scratch buffer, 32 bytes per output pixel of internal images and
// about 2 MiB of buffers; the images of the context take 16 bytes per render pixel and 8 per
// output pixel.
u64 EstimateBytes(const Fsr4Key& key) {
    const u64 render_pixels = u64{key.max_render_size.width} * key.max_render_size.height;
    const u64 output_pixels = u64{key.max_output_size.width} * key.max_output_size.height;
    return Gr2Fsr4ScratchBytes(key.max_output_size.width, key.max_output_size.height) +
           40 * output_pixels + 16 * render_pixels + 2_MB;
}

} // namespace

struct Fsr4Context::Impl {
    // A provider context and its images. The provider context is destroyed first.
    struct Resources {
        Resources() = default;
        ~Resources() {
            if (context) {
                Gr2Fsr4Destroy(context);
            }
        }

        Resources(const Resources&) = delete;
        Resources& operator=(const Resources&) = delete;

        Gr2Fsr4Context* context{};
        ContextImages images;
        u64 bytes{};
    };

    struct Created {
        std::unique_ptr<Resources> resources;
        std::string notice;
        float milliseconds{};
    };

    struct InFlight {
        u64 frame_id{};
        u64 tick{};
    };

    explicit Impl(const Vulkan::Instance& instance);

    ContextState Update(Vulkan::Scheduler& scheduler, const std::optional<Fsr4Key>& wanted,
                        bool may_allocate, u64 extra_bytes);
    bool PrepareFrame(Vulkan::Rasterizer& rasterizer);
    bool RecordDispatch(Vulkan::Scheduler& scheduler, const Fsr4Dispatch& dispatch);
    Created Create(const Fsr4Key& key, const Fsr4Assets& assets) const;
    void Retire(Vulkan::Scheduler& scheduler);
    bool TakeRecordedFailure(Vulkan::Scheduler& scheduler);
    void EnqueueDestroy(std::unique_ptr<Resources> resources);

    const Vulkan::Instance& instance;
    Fsr4DeviceLimits limits{};
    bool supported{};
    std::string unsupported_notice;

    // At most one of loading, loaded, pending and current is set.
    std::future<Fsr4LoadResult> loading;
    Fsr4Key loading_key{};
    std::optional<Fsr4Assets> loaded;
    Fsr4Key loaded_key{};
    std::future<Created> pending;
    Fsr4Key pending_key{};
    std::unique_ptr<Resources> current;
    Fsr4Key current_key{};
    Fsr4Images images{};
    std::optional<Fsr4Key> failed_key;
    std::string notice; ///< Why failed_key failed.
    bool key_wanted{};
    bool waiting_for_fsr3{};
    u64 generation{};
    u64 memory_bytes{};
    // Contexts the worker has yet to destroy. A creation starts only when there are none.
    std::atomic<u32> destroying{};
    std::deque<InFlight> frames;
    u64 next_frame_id{};
    bool logged_wait{};
    RecordedFailure recorded_failure;

    // The provider creates contexts through one process-global interface pointer. Loads,
    // creations and destructions run on this worker, one at a time.
    Worker worker{"shadPS4:Gr2Fsr4Worker"};
};

Fsr4Context::Impl::Impl(const Vulkan::Instance& instance) : instance{instance} {
    const std::optional<Fsr4DeviceFeatures> features = Fsr4EnabledFeatures();
    if (!features) {
        unsupported_notice = "FSR 4 needs a restart: it was off when the game started";
        return;
    }
    const vk::PhysicalDevice physical_device = instance.GetPhysicalDevice();
    const auto properties =
        physical_device
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties,
                            vk::PhysicalDeviceVulkan13Properties>();
    const auto& vk11 = properties.get<vk::PhysicalDeviceVulkan11Properties>();
    const bool compute_subgroups =
        static_cast<bool>(vk11.subgroupSupportedStages & vk::ShaderStageFlagBits::eCompute);
    limits = Fsr4DeviceLimits{
        .features = *features,
        .subgroup_operations =
            compute_subgroups ? static_cast<u32>(vk11.subgroupSupportedOperations) : 0u,
        .max_storage_buffer_range = properties.get().properties.limits.maxStorageBufferRange,
        .max_allocation_size = instance.MaxMemoryAllocationSize(),
    };

    // The backend checks these when it creates a context.
    std::string missing;
    const auto require = [&missing](bool present, std::string_view name) {
        if (!present) {
            missing.append(missing.empty() ? "" : ", ").append(name);
        }
    };
    require(features->shader_integer_dot_product, "integer dot product");
    require(features->shader_int8, "8-bit integers");
    require(features->shader_float16, "16-bit floats");
    require(features->shader_int16, "16-bit integers");
    require(features->storage_image_extended_formats, "extended storage image formats");
    require(features->storage_image_write_without_format, "storage image writes without format");
    require(features->compute_shader_derivatives && features->derivative_group_linear,
            "compute shader derivatives");
    constexpr vk::FormatFeatureFlags sampled_linear =
        vk::FormatFeatureFlagBits::eSampledImage |
        vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
    for (const vk::Format format :
         {vk::Format::eR16G16B16A16Sfloat, vk::Format::eR8G8B8A8Unorm, vk::Format::eR32Sfloat}) {
        const vk::FormatFeatureFlags optimal =
            physical_device.getFormatProperties(format).optimalTilingFeatures;
        require((optimal & sampled_linear) == sampled_linear,
                fmt::format("linear filtering of {}", vk::to_string(format)));
    }
    supported = missing.empty();
    if (!supported) {
        unsupported_notice = fmt::format("FSR 4 needs {}", missing);
    }
    const bool accelerated = properties.get<vk::PhysicalDeviceVulkan13Properties>()
                                 .integerDotProduct4x8BitPackedSignedAccelerated == VK_TRUE;
    LOG_INFO(Render_Vulkan,
             "GR2 FSR4: FSR 4 (INT8, experimental) {}{}, accelerated 4x8-bit packed signed dot "
             "product {}, maxStorageBufferRange {}",
             supported ? "supported" : "unsupported: ", missing, accelerated ? "yes" : "no",
             limits.max_storage_buffer_range);
}

ContextState Fsr4Context::Impl::Update(Vulkan::Scheduler& scheduler,
                                       const std::optional<Fsr4Key>& wanted, bool may_allocate,
                                       u64 extra_bytes) {
    key_wanted = wanted.has_value();
    if (!supported) {
        return wanted ? ContextState::Unsupported : ContextState::Idle;
    }
    TakeRecordedFailure(scheduler);
    if (IsReady(loading)) {
        Fsr4LoadResult result = loading.get();
        if (!result.assets) {
            failed_key = loading_key;
            notice = std::move(result.notice);
            LOG_ERROR(Render_Vulkan, "GR2 FSR4: {}", notice);
        } else if (wanted == loading_key) {
            loaded = std::move(result.assets);
            loaded_key = loading_key;
            LOG_INFO(Render_Vulkan, "GR2 FSR4: files ready: {}", result.summary);
        }
    }
    if (IsReady(pending)) {
        Created created = pending.get();
        if (!created.resources) {
            failed_key = pending_key;
            notice = std::move(created.notice);
            LOG_ERROR(Render_Vulkan, "GR2 FSR4: context {}: {}",
                      FormatSizes(pending_key.max_render_size, pending_key.max_output_size),
                      notice);
        } else if (wanted == pending_key) {
            current = std::move(created.resources);
            current_key = pending_key;
            images = current->images.handles;
            memory_bytes = current->bytes;
            ++generation;
            next_frame_id = 0;
            LOG_INFO(Render_Vulkan,
                     "GR2 FSR4: context {} ready, {} model, {} tier, {} MiB, created in {:.0f} ms",
                     FormatSizes(current_key.max_render_size, current_key.max_output_size),
                     Fsr4PresetName(current_key.preset), Fsr4TierName(current_key.max_output_size),
                     memory_bytes / 1_MB, created.milliseconds);
        } else {
            ++destroying;
            EnqueueDestroy(std::move(created.resources));
        }
    }
    // A changed key, or FSR 4 turned off and on again, retries.
    if (failed_key != wanted) {
        failed_key.reset();
        notice.clear();
    }
    if (loaded && wanted != loaded_key) {
        loaded.reset();
    }
    if (current && wanted != current_key) {
        Retire(scheduler);
    }

    if (wanted && !current && !pending.valid() && !loading.valid() && !loaded && !failed_key) {
        const std::filesystem::path folder = Settings::Fsr4AssetDir();
        LOG_INFO(Render_Vulkan, "GR2 FSR4: loading the {} model, {} tier, from {}",
                 Fsr4PresetName(wanted->preset), Fsr4TierName(wanted->max_output_size),
                 Common::FS::PathToUTF8String(folder));
        loading_key = *wanted;
        std::packaged_task<Fsr4LoadResult()> task{[folder, key = *wanted, device = limits] {
            return LoadFsr4Assets(folder, key.preset, key.max_output_size, device);
        }};
        loading = task.get_future();
        worker.Enqueue(std::move(task));
    }

    waiting_for_fsr3 = wanted && loaded && !may_allocate;
    if (wanted && loaded && may_allocate && destroying.load() == 0) {
        const u64 needed = EstimateBytes(*wanted) + extra_bytes + 64_MB;
        const u64 available = AvailableDeviceMemory(instance);
        const std::string sizes = FormatSizes(wanted->max_render_size, wanted->max_output_size);
        if (needed > available) {
            failed_key = wanted;
            notice = fmt::format("FSR 4 needs {} MiB of GPU memory, {} MiB available",
                                 needed / 1_MB, available / 1_MB);
            LOG_ERROR(Render_Vulkan,
                      "GR2 FSR4: context {} needs about {} MiB of GPU memory, {} MiB available",
                      sizes, needed / 1_MB, available / 1_MB);
            loaded.reset();
        } else {
            LOG_INFO(Render_Vulkan, "GR2 FSR4: creating context {}, {} model, {} tier", sizes,
                     Fsr4PresetName(wanted->preset), Fsr4TierName(wanted->max_output_size));
            pending_key = *wanted;
            std::packaged_task<Created()> task{
                [this, key = *wanted, assets = std::move(*loaded)] { return Create(key, assets); }};
            loaded.reset();
            pending = task.get_future();
            worker.Enqueue(std::move(task));
        }
    }

    if (!wanted) {
        return ContextState::Idle;
    }
    if (current) {
        return ContextState::Ready;
    }
    if (failed_key) {
        return ContextState::Failed;
    }
    return pending.valid() ? ContextState::Creating : ContextState::Loading;
}

bool Fsr4Context::Impl::PrepareFrame(Vulkan::Rasterizer& rasterizer) {
    Vulkan::Scheduler& scheduler = rasterizer.GetScheduler();
    std::optional<u64> newest;
    while (!frames.empty() && scheduler.IsFree(frames.front().tick)) {
        newest = frames.front().frame_id;
        frames.pop_front();
    }
    if (frames.size() == GR2_FSR4_FRAMES_IN_FLIGHT) {
        const InFlight oldest = frames.front();
        if (!std::exchange(logged_wait, true)) {
            LOG_WARNING(Render_Vulkan,
                        "GR2 FSR4: three FSR 4 frames in flight, waiting for GPU tick {}",
                        oldest.tick);
        }
        // The wait blocks on the GPU and can submit the open command buffer, so the guest-copy
        // hold goes first.
        rasterizer.DropCopyHoldForCommands();
        scheduler.Wait(oldest.tick);
        newest = oldest.frame_id;
        frames.pop_front();
    }
    if (newest) {
        scheduler.Record([context = current->context, completed = *newest, generation = generation,
                          failure = &recorded_failure](vk::CommandBuffer) {
            if (failure->HasFailed(generation)) {
                return;
            }
            const s32 result = Gr2Fsr4Retire(context, completed);
            if (result != VK_SUCCESS) {
                failure->Report(generation,
                                fmt::format("FSR 4 cannot release frame {}: {}", completed,
                                            vk::to_string(static_cast<vk::Result>(result))));
            }
        });
    }
    return !TakeRecordedFailure(scheduler);
}

bool Fsr4Context::Impl::RecordDispatch(Vulkan::Scheduler& scheduler, const Fsr4Dispatch& dispatch) {
    // The output is written from scratch. The source scope also covers the reads of the previous
    // frame.
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
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &output_barrier,
    });
    const auto image = [](const Fsr4Image& source, vk::Extent2D extent) {
        return Gr2Fsr4Image{
            .image = static_cast<VkImage>(source.image),
            .view = static_cast<VkImageView>(source.view),
            .width = extent.width,
            .height = extent.height,
        };
    };
    const vk::Extent2D render = current_key.max_render_size;
    const Gr2Fsr4DispatchInfo info{
        .frame_id = next_frame_id,
        .color = image(images.color, render),
        .depth = image(images.depth, render),
        .motion_vectors = image(images.motion_vectors, render),
        .output = image(images.output, current_key.max_output_size),
        .jitter_x = dispatch.jitter_x,
        .jitter_y = dispatch.jitter_y,
        .motion_vector_scale_x = dispatch.motion_vector_scale_x,
        .motion_vector_scale_y = dispatch.motion_vector_scale_y,
        .render_width = dispatch.render_size.width,
        .render_height = dispatch.render_size.height,
        .output_width = dispatch.output_size.width,
        .output_height = dispatch.output_size.height,
        .reset = dispatch.reset ? 1u : 0u,
    };
    scheduler.Record([context = current->context, info, generation = generation,
                      failure = &recorded_failure](vk::CommandBuffer cmdbuf) {
        if (failure->HasFailed(generation)) {
            return;
        }
        Gr2Fsr4DispatchInfo recorded = info;
        recorded.command_buffer = static_cast<VkCommandBuffer>(cmdbuf);
        const Gr2Fsr4Status status = Gr2Fsr4Dispatch(context, &recorded);
        if (status.stage != GR2_FSR4_STAGE_NONE) {
            failure->Report(generation,
                            fmt::format("FSR 4 rejected the dispatch: {}", StatusText(status)));
        }
    });
    if (TakeRecordedFailure(scheduler)) {
        return false;
    }
    frames.push_back({.frame_id = next_frame_id++, .tick = scheduler.CurrentTick()});
    return true;
}

Fsr4Context::Impl::Created Fsr4Context::Impl::Create(const Fsr4Key& key,
                                                     const Fsr4Assets& assets) const {
    const Clock::time_point start = Clock::now();
    Created created{};
    Gr2Fsr4CreateInfo info{
        .device = static_cast<VkDevice>(instance.GetDevice()),
        .physical_device = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice()),
        .initializer = assets.initializer.data(),
        .initializer_size = assets.initializer.size(),
        .pre_weights = assets.pre_weights.data(),
        .pre_weights_size = assets.pre_weights.size(),
        .max_render_width = key.max_render_size.width,
        .max_render_height = key.max_render_size.height,
        .max_output_width = key.max_output_size.width,
        .max_output_height = key.max_output_size.height,
    };
    for (std::size_t pass = 0; pass < Fsr4ShaderCount; ++pass) {
        info.shaders[pass] = Gr2Fsr4Shader{
            .words = assets.shaders[pass].data(),
            .size_bytes = assets.shaders[pass].size() * sizeof(u32),
            .entry_point = assets.entry_points[pass].c_str(),
        };
    }
    auto resources = std::make_unique<Resources>();
    const Gr2Fsr4Status status = Gr2Fsr4Create(&info, &resources->context);
    if (status.stage != GR2_FSR4_STAGE_NONE) {
        created.notice = fmt::format("FSR 4 context creation failed: {}", StatusText(status));
        return created;
    }

    std::string why;
    std::optional<ContextImages> images =
        CreateContextImages(instance, key.max_render_size, key.max_output_size, "GR2 FSR4", why);
    if (!images) {
        created.notice = fmt::format("FSR 4 {}", why);
        return created;
    }
    resources->images = std::move(*images);
    resources->bytes = Gr2Fsr4MemoryBytes(resources->context) + resources->images.bytes;
    created.resources = std::move(resources);
    created.milliseconds = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
    return created;
}

void Fsr4Context::Impl::Retire(Vulkan::Scheduler& scheduler) {
    const u64 tick = scheduler.CurrentTick();
    LOG_INFO(Render_Vulkan, "GR2 FSR4: context {} retired, destroyed after GPU tick {}",
             FormatSizes(current_key.max_render_size, current_key.max_output_size), tick);
    ++destroying;
    // With a record thread, the provider calls recorded up to this tick run there; waiting for
    // its submit of the tick orders them before the destruction on the worker. A failure they
    // report after the last TakeRecordedFailure is logged here.
    scheduler.DeferOperation([this, &scheduler, tick, generation = generation, key = current_key,
                              resources = std::move(current)]() mutable {
        scheduler.WaitSubmitted(tick);
        if (const std::optional<std::string> message = recorded_failure.Take(generation)) {
            LOG_ERROR(Render_Vulkan, "GR2 FSR4: context {}: {}",
                      FormatSizes(key.max_render_size, key.max_output_size), *message);
        }
        EnqueueDestroy(std::move(resources));
    });
    images = {};
    memory_bytes = 0;
    frames.clear();
}

bool Fsr4Context::Impl::TakeRecordedFailure(Vulkan::Scheduler& scheduler) {
    if (!current) {
        return false;
    }
    std::optional<std::string> message = recorded_failure.Take(generation);
    if (!message) {
        return false;
    }
    LOG_ERROR(Render_Vulkan, "GR2 FSR4: context {}: {}",
              FormatSizes(current_key.max_render_size, current_key.max_output_size), *message);
    failed_key = current_key;
    notice = std::move(*message);
    Retire(scheduler);
    return true;
}

void Fsr4Context::Impl::EnqueueDestroy(std::unique_ptr<Resources> resources) {
    worker.Enqueue([this, resources = std::move(resources)]() mutable {
        resources.reset();
        --destroying;
    });
}

Fsr4Key Fsr4KeyFor(vk::Extent2D render_size, vk::Extent2D output_size, Fsr4Model model) {
    // Every model but Auto is a preset, in the order of the presets.
    static_assert(static_cast<u32>(Fsr4Model::Drs) == GR2_FSR4_PRESET_COUNT);
    Fsr4Key key{.max_render_size = render_size, .max_output_size = output_size};
    if (model != Fsr4Model::Auto) {
        key.preset = static_cast<u32>(model) - 1;
        return key;
    }
    // The provider's thresholds on the width ratio.
    const float ratio = RoundedWidthRatio(render_size, output_size);
    key.preset = ratio >= 2.99f   ? GR2_FSR4_PRESET_ULTRA_PERFORMANCE
                 : ratio >= 1.99f ? GR2_FSR4_PRESET_PERFORMANCE
                 : ratio >= 1.69f ? GR2_FSR4_PRESET_BALANCED
                 : ratio >= 1.5f  ? GR2_FSR4_PRESET_QUALITY
                                  : GR2_FSR4_PRESET_NATIVE;
    return key;
}

const char* Fsr4PresetName(u32 preset) {
    return Gr2Fsr4PresetName(preset);
}

const char* Fsr4TierName(vk::Extent2D output_size) {
    const char* tier = Gr2Fsr4TierName(output_size.width, output_size.height);
    return tier ? tier : "";
}

Fsr4Context::Fsr4Context(const Vulkan::Instance& instance)
    : impl{std::make_unique<Impl>(instance)} {}

Fsr4Context::~Fsr4Context() = default;

ContextState Fsr4Context::Update(Vulkan::Scheduler& scheduler, const std::optional<Fsr4Key>& wanted,
                                 bool may_allocate, u64 extra_bytes) {
    return impl->Update(scheduler, wanted, may_allocate, extra_bytes);
}

bool Fsr4Context::HoldsMemory() const {
    return impl->current || impl->pending.valid() || impl->destroying.load() > 0;
}

bool Fsr4Context::AssetsReady() const {
    return impl->loaded.has_value();
}

u64 Fsr4Context::Generation() const {
    return impl->generation;
}

u64 Fsr4Context::MemoryBytes() const {
    return impl->memory_bytes;
}

std::string Fsr4Context::Notice() const {
    const Impl& state = *impl;
    if (!state.key_wanted) {
        return {};
    }
    if (!state.supported) {
        return state.unsupported_notice;
    }
    if (state.waiting_for_fsr3) {
        return "FSR 4: waiting for FSR 3.1 to release its GPU memory";
    }
    return state.notice;
}

const Fsr4Images& Fsr4Context::Images() const {
    return impl->images;
}

bool Fsr4Context::PrepareFrame(Vulkan::Rasterizer& rasterizer) {
    return impl->PrepareFrame(rasterizer);
}

bool Fsr4Context::RecordDispatch(Vulkan::Scheduler& scheduler, const Fsr4Dispatch& dispatch) {
    return impl->RecordDispatch(scheduler, dispatch);
}

} // namespace Gr2Fsr3
