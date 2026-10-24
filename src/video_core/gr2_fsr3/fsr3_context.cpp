// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/gr2_fsr3/fsr3_context.h"
#include "video_core/gr2_fsr3/fsr_worker.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// <vulkan/vulkan.h> must first be included through vk_common.h, which configures it.
#include <ffx_vk_portable.h>

static_assert(FFX_VK_PORTABLE_ABI_VERSION == 2u,
              "The FSR 3.1 calls in this file follow portable ABI version 2");

namespace Gr2Fsr3 {

namespace {

const char* ResultName(FfxVkPortableResult result) {
    switch (result) {
    case FFX_VK_PORTABLE_OK:
        return "success";
    case FFX_VK_PORTABLE_ERROR_INVALID_POINTER:
        return "invalid pointer";
    case FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE:
        return "invalid struct size";
    case FFX_VK_PORTABLE_ERROR_VULKAN:
        return "Vulkan error";
    case FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY:
        return "out of memory";
    case FFX_VK_PORTABLE_ERROR_UNSUPPORTED:
        return "unsupported";
    case FFX_VK_PORTABLE_ERROR_BACKEND:
        return "backend error";
    }
    return "unknown result";
}

std::string IssueNames(u64 issues) {
    if (issues == FFX_VK_PORTABLE_VALIDATION_NONE) {
        return ffxVkPortableValidationIssueName(FFX_VK_PORTABLE_VALIDATION_NONE);
    }
    std::string names;
    for (; issues != 0; issues &= issues - 1) {
        if (!names.empty()) {
            names += ", ";
        }
        names += ffxVkPortableValidationIssueName(u64{1} << std::countr_zero(issues));
    }
    return names;
}

FfxVkPortableImage Describe(const FsrImage& image) {
    FfxVkPortableImage description{};
    description.structSize = sizeof(description);
    if (!image.image) {
        return description;
    }
    description.image = static_cast<VkImage>(image.image);
    description.format = static_cast<VkFormat>(image.format);
    description.extent = {image.extent.width, image.extent.height};
    description.mipCount = 1;
    description.arrayLayers = 1;
    description.usage = static_cast<VkImageUsageFlags>(image.usage);
    description.aspect = static_cast<VkImageAspectFlags>(image.aspect);
    description.state = image.written ? FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS
                                      : FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ;
    return description;
}

} // namespace

struct Context::Impl {
    struct Created {
        FfxVkPortableUpscaleContext* context{};
        FfxVkPortableResult result{FFX_VK_PORTABLE_OK};
        FfxVkPortableUpscaleCreateInfo create{};
        u64 issues{};
        u64 memory_bytes{};
    };

    explicit Impl(const Vulkan::Instance& instance);

    ContextState Update(Vulkan::Scheduler& scheduler, const std::optional<ContextKey>& wanted,
                        u64 extra_bytes);
    bool RecordDispatch(Vulkan::Scheduler& scheduler, const FsrDispatch& dispatch);
    Created Create(const ContextKey& key) const;
    void EnqueueDestroy(FfxVkPortableUpscaleContext* context);

    const Vulkan::Instance& instance;
    FfxVkPortableDeviceCapabilities caps{};
    bool supported{};
    bool fp16{};
    bool separate_layouts{};

    FfxVkPortableUpscaleContext* current{};
    ContextKey current_key{};
    FfxVkPortableUpscaleCreateInfo current_create{};
    u64 generation{};
    u64 memory_bytes{};
    std::future<Created> pending;
    ContextKey pending_key{};
    std::optional<ContextKey> failed_key;
    // Contexts the worker has yet to destroy. A creation starts only when there are none, so at
    // most one context holds device memory.
    std::atomic<u32> destroying{};
    std::string notice;
    std::vector<u64> reported_issues;

    // FidelityFX creates contexts through one process-global device copy. Creations and
    // destructions run on this worker, one at a time.
    Worker worker{"shadPS4:Gr2Fsr3Worker"};
};

Context::Impl::Impl(const Vulkan::Instance& instance) : instance{instance} {
    const vk::PhysicalDevice physical_device = instance.GetPhysicalDevice();
    caps.structSize = sizeof(caps);
    supported = ffxVkPortableQueryDeviceCapabilities(static_cast<VkPhysicalDevice>(physical_device),
                                                     &caps) == FFX_VK_PORTABLE_OK &&
                caps.fsr3ComputePrerequisites == VK_TRUE;
    fp16 = instance.IsShaderFloat16Supported() && instance.IsShaderInt16Supported();
    const auto features =
        physical_device
            .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
    separate_layouts =
        features.get<vk::PhysicalDeviceVulkan12Features>().separateDepthStencilLayouts == VK_TRUE;
    const bool write_without_format =
        features.get().features.shaderStorageImageWriteWithoutFormat == VK_TRUE;

    const auto yes_no = [](bool value) { return value ? "yes" : "no"; };
    LOG_INFO(Render_Vulkan,
             "GR2 FSR3: FSR 3.1 {}: vendor {:#x}, subgroup size {} ({}-{}), compute queue {}, "
             "subgroup operations {}, storage formats {}, write without format {}, fp16 {}, "
             "synchronization2 {}, separate depth/stencil layouts {}",
             supported ? "supported" : "unsupported", caps.vendorId, caps.subgroupSize,
             caps.minSubgroupSize, caps.maxSubgroupSize, yes_no(caps.hasComputeQueue),
             yes_no(caps.subgroupOperations), yes_no(caps.requiredFsr3StorageFormats),
             yes_no(write_without_format), yes_no(fp16), yes_no(caps.synchronization2),
             yes_no(separate_layouts));
}

ContextState Context::Impl::Update(Vulkan::Scheduler& scheduler,
                                   const std::optional<ContextKey>& wanted, u64 extra_bytes) {
    if (!supported) {
        return ContextState::Unsupported;
    }
    if (IsReady(pending)) {
        const Created created = pending.get();
        if (created.result != FFX_VK_PORTABLE_OK) {
            failed_key = pending_key;
            notice = fmt::format("FSR 3.1 context creation failed: {}", ResultName(created.result));
            LOG_ERROR(Render_Vulkan,
                      "GR2 FSR3: context {} creation failed: {}, validation issues: {}",
                      FormatSizes(pending_key.max_render_size, pending_key.max_output_size),
                      ResultName(created.result), IssueNames(created.issues));
        } else if (wanted == pending_key) {
            current = created.context;
            current_key = pending_key;
            current_create = created.create;
            memory_bytes = created.memory_bytes;
            ++generation;
            notice.clear();
            LOG_INFO(Render_Vulkan, "GR2 FSR3: context {} ready, {} MiB, fp16 {}",
                     FormatSizes(current_key.max_render_size, current_key.max_output_size),
                     memory_bytes / 1_MB, current_key.fp16);
        } else {
            ++destroying;
            EnqueueDestroy(created.context);
        }
    }
    if (failed_key != wanted) {
        failed_key.reset();
    }
    if (current && wanted != current_key) {
        LOG_INFO(Render_Vulkan, "GR2 FSR3: context {} retired, destroyed after GPU tick {}",
                 FormatSizes(current_key.max_render_size, current_key.max_output_size),
                 scheduler.CurrentTick());
        ++destroying;
        scheduler.DeferOperation(
            [this, context = std::exchange(current, nullptr)] { EnqueueDestroy(context); });
        memory_bytes = 0;
    }
    if (wanted && !current && !pending.valid() && !failed_key && destroying.load() == 0) {
        // FSR 3.1 allocates about 42 bytes per render pixel and 17 per output pixel; the margin
        // covers its buffers and the error of the estimate.
        const u64 render_pixels =
            u64{wanted->max_render_size.width} * wanted->max_render_size.height;
        const u64 output_pixels =
            u64{wanted->max_output_size.width} * wanted->max_output_size.height;
        const u64 needed = 42 * render_pixels + 17 * output_pixels + extra_bytes + 64_MB;
        const u64 available = AvailableDeviceMemory(instance);
        const std::string sizes = FormatSizes(wanted->max_render_size, wanted->max_output_size);
        if (needed > available) {
            failed_key = wanted;
            notice = fmt::format("FSR 3.1 needs {} MiB of GPU memory, {} MiB available",
                                 needed / 1_MB, available / 1_MB);
            LOG_ERROR(Render_Vulkan,
                      "GR2 FSR3: context {} needs about {} MiB of GPU memory, {} MiB available",
                      sizes, needed / 1_MB, available / 1_MB);
        } else {
            LOG_INFO(Render_Vulkan, "GR2 FSR3: creating context {}, fp16 {}, debug checking {}",
                     sizes, wanted->fp16, wanted->debug_checking);
            pending_key = *wanted;
            std::packaged_task<Created()> task{[this, key = *wanted] { return Create(key); }};
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
    return failed_key ? ContextState::Failed : ContextState::Creating;
}

bool Context::Impl::RecordDispatch(Vulkan::Scheduler& scheduler, const FsrDispatch& dispatch) {
    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    // Validation rejects a null command buffer; the recorded call passes the one it records into.
    info.commandBuffer = reinterpret_cast<VkCommandBuffer>(std::uintptr_t{1});
    info.color = Describe(dispatch.color);
    info.depth = Describe(dispatch.depth);
    info.motionVectors = Describe(dispatch.motion_vectors);
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask = Describe(dispatch.reactive);
    info.transparencyAndCompositionMask = Describe(dispatch.transparency);
    info.output = Describe(dispatch.output);
    info.jitterOffset = {dispatch.jitter_x, dispatch.jitter_y};
    info.motionVectorScale = {dispatch.motion_vector_scale_x, dispatch.motion_vector_scale_y};
    info.renderSize = {dispatch.render_size.width, dispatch.render_size.height};
    info.outputSize = {dispatch.output_size.width, dispatch.output_size.height};
    info.frameTimeMilliseconds = dispatch.frame_time_ms;
    info.preExposure = 1.0f;
    info.cameraNear = dispatch.camera.near_plane;
    info.cameraFar = dispatch.camera.far_plane;
    info.cameraVerticalFovRadians = dispatch.camera.vertical_fov;
    info.viewSpaceToMeters = 1.0f;
    info.sharpness = 0.0f;
    info.enableSharpening = VK_FALSE;
    info.reset = dispatch.reset ? VK_TRUE : VK_FALSE;
    info.frameId = dispatch.frame_id;

    // The library validates every dispatch the same way and rejects it on any issue.
    const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&current_create, &info);
    if (issues != FFX_VK_PORTABLE_VALIDATION_NONE) {
        const FfxVkPortableResult result = FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;
        notice = fmt::format("FSR 3.1 rejected the dispatch: {}", ResultName(result));
        if (!std::ranges::contains(reported_issues, issues)) {
            reported_issues.push_back(issues);
            LOG_ERROR(Render_Vulkan, "GR2 FSR3: dispatch {} rejected: {}, validation issues: {}",
                      FormatSizes(dispatch.render_size, dispatch.output_size), ResultName(result),
                      IssueNames(issues));
        }
        return false;
    }
    scheduler.Record([context = current, info](vk::CommandBuffer cmdbuf) {
        FfxVkPortableUpscaleDispatchInfo recorded = info;
        recorded.commandBuffer = static_cast<VkCommandBuffer>(cmdbuf);
        const FfxVkPortableResult result =
            ffxVkPortableUpscaleContextRecordDispatch(context, &recorded);
        ASSERT_MSG(result == FFX_VK_PORTABLE_OK, "GR2 FSR3: validated dispatch rejected: {}",
                   ResultName(result));
    });
    return true;
}

Context::Impl::Created Context::Impl::Create(const ContextKey& key) const {
    // The device enables synchronization2 and shaderStorageImageWriteWithoutFormat whenever they
    // are supported. The other optional features are reported as disabled, which only restricts
    // what the backend uses.
    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = static_cast<VkInstance>(instance.GetInstance());
    device_info.physicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice());
    device_info.device = static_cast<VkDevice>(instance.GetDevice());
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = static_cast<VkQueue>(instance.GetGraphicsQueue());
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = key.fp16 ? VK_TRUE : VK_FALSE;
    device_info.synchronization2Enabled = caps.synchronization2;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    Created created{};
    created.create.structSize = sizeof(created.create);
    if (key.debug_checking) {
        created.create.flags = FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING;
    }
    created.create.maxRenderSize = {key.max_render_size.width, key.max_render_size.height};
    created.create.maxOutputSize = {key.max_output_size.width, key.max_output_size.height};
    created.issues = ffxVkPortableValidateUpscaleCreateInfo(&created.create);
    if (created.issues != FFX_VK_PORTABLE_VALIDATION_NONE) {
        created.result = FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT;
        return created;
    }
    created.result =
        ffxVkPortableUpscaleContextCreate(&device_info, &created.create, &created.context);
    if (created.result == FFX_VK_PORTABLE_OK) {
        FfxVkPortableMemoryUsage usage{};
        usage.structSize = sizeof(usage);
        ffxVkPortableUpscaleContextGetMemoryUsage(created.context, &usage);
        created.memory_bytes = usage.totalUsageInBytes;
    }
    return created;
}

void Context::Impl::EnqueueDestroy(FfxVkPortableUpscaleContext* context) {
    worker.Enqueue([this, context] {
        const FfxVkPortableResult result = ffxVkPortableUpscaleContextDestroy(context);
        if (result != FFX_VK_PORTABLE_OK) {
            LOG_ERROR(Render_Vulkan, "GR2 FSR3: context destruction failed: {}",
                      ResultName(result));
        }
        --destroying;
    });
}

Context::Context(const Vulkan::Instance& instance) : impl{std::make_unique<Impl>(instance)} {}

Context::~Context() = default;

bool Context::IsSupported() const {
    return impl->supported;
}

bool Context::IsFp16Supported() const {
    return impl->fp16;
}

bool Context::IsSeparateDepthStencilLayoutsSupported() const {
    return impl->separate_layouts;
}

ContextState Context::Update(Vulkan::Scheduler& scheduler, const std::optional<ContextKey>& wanted,
                             u64 extra_bytes) {
    return impl->Update(scheduler, wanted, extra_bytes);
}

bool Context::HoldsMemory() const {
    return impl->current || impl->pending.valid() || impl->destroying.load() > 0;
}

u64 Context::Generation() const {
    return impl->generation;
}

u64 Context::MemoryBytes() const {
    return impl->memory_bytes;
}

std::string Context::Notice() const {
    return impl->notice;
}

bool Context::RecordDispatch(Vulkan::Scheduler& scheduler, const FsrDispatch& dispatch) {
    return impl->RecordDispatch(scheduler, dispatch);
}

u64 AvailableDeviceMemory(const Vulkan::Instance& instance) {
    if (instance.IsIntegrated() && instance.CanReportMemoryUsage()) {
        // GetTotalMemoryBudget() of an integrated GPU sets part of the shared memory aside for the
        // system; the current heap budgets of the driver give the headroom there.
        vk::PhysicalDeviceMemoryBudgetPropertiesEXT heaps{};
        vk::PhysicalDeviceMemoryProperties2 properties{.pNext = &heaps};
        instance.GetPhysicalDevice().getMemoryProperties2(&properties);
        u64 available = 0;
        for (u32 i = 0; i < properties.memoryProperties.memoryHeapCount; ++i) {
            available += heaps.heapBudget[i] - std::min(heaps.heapUsage[i], heaps.heapBudget[i]);
        }
        return available;
    }
    const u64 budget = instance.GetTotalMemoryBudget();
    const u64 used = instance.CanReportMemoryUsage() ? instance.GetDeviceMemoryUsage() : 0;
    return budget > used ? budget - used : 0;
}

std::string FormatSizes(vk::Extent2D render_size, vk::Extent2D output_size) {
    return fmt::format("{}x{} -> {}x{}", render_size.width, render_size.height, output_size.width,
                       output_size.height);
}

} // namespace Gr2Fsr3
