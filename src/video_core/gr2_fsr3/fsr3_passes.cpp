// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "common/logging/log.h"
#include "video_core/gr2_fsr3/fsr3_passes.h"
#include "video_core/gr2_fsr3/shaders/fsr3_debug_comp.h"
#include "video_core/gr2_fsr3/shaders/fsr3_fallback_comp.h"
#include "video_core/gr2_fsr3/shaders/fsr3_mask_comp.h"
#include "video_core/gr2_fsr3/shaders/fsr3_mvcopy_comp.h"
#include "video_core/gr2_fsr3/shaders/fsr3_store_comp.h"
#include "video_core/gr2_fsr3/shaders/fsr4_inputs_comp.h"
#include "video_core/gr2_fsr3/shaders/grr_camera_motion_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

namespace Gr2Fsr3 {

namespace {

constexpr s32 OutlineRadius = 2;

constexpr u32 MaskUseHistory = 1u << 0;
constexpr u32 MaskHistoryValid = 1u << 1;
constexpr u32 StoreRcas = 1u << 0;
constexpr u32 StoreLinearInput = 1u << 1;
constexpr u32 StoreFxaa = 1u << 2;
constexpr u32 StoreFxaaCharacters = 1u << 3;
constexpr u32 StoreAlphaOne = 1u << 4;
constexpr u32 FallbackEasu = 1u << 0;
constexpr u32 FallbackAlphaOne = 1u << 1;
constexpr u32 DebugHistoryValid = 1u << 0;
constexpr u32 DebugDepthCopy = 1u << 1;
constexpr u32 DebugDepthMain = 1u << 2;
constexpr u32 Fsr4InputsEncodeSrgb = 1u << 0;
constexpr u32 Fsr4InputsDepthInverted = 1u << 1;
constexpr u32 Fsr4InputsDepthLinear = 1u << 2;
constexpr u32 Fsr4InputsZeroMotion = 1u << 3;

// fsr3_debug.comp switches on the DebugView values 1 to 5.
static_assert(static_cast<u32>(DebugView::MotionVectors) == 1 &&
              static_cast<u32>(DebugView::Reprojection) == 5);

constexpr vk::ImageUsageFlags HostImageUsage =
    vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr vk::DescriptorType SampledImage = vk::DescriptorType::eSampledImage;
constexpr vk::DescriptorType StorageImage = vk::DescriptorType::eStorageImage;
constexpr vk::DescriptorType SamplerBinding = vk::DescriptorType::eSampler;

// Descriptor set 0 of each shader, in binding order.
constexpr std::array MaskBindings{SampledImage, StorageImage, StorageImage, StorageImage};
constexpr std::array CameraMotionBindings{SampledImage, SampledImage, SampledImage, StorageImage};
constexpr std::array StoreBindings{SampledImage, SamplerBinding, SampledImage, StorageImage};
constexpr std::array FallbackBindings{SampledImage, SamplerBinding, StorageImage};
constexpr std::array DebugBindings{SampledImage, SampledImage,   SampledImage, SampledImage,
                                   SampledImage, SampledImage,   SampledImage, SampledImage,
                                   StorageImage, SamplerBinding, StorageImage};
constexpr std::array Fsr4InputsBindings{SampledImage, SampledImage, SampledImage,
                                        StorageImage, StorageImage, StorageImage};
constexpr std::array GrrCameraMotionBindings{SampledImage, StorageImage, StorageImage};

// Push constant blocks of the shaders.
struct MaskConstants {
    std::array<u32, 2> size{};
    float reactive_characters{};
    float reactive_effects{};
    float reactive_outline{};
    float tc_characters{};
    u32 flags{};
    s32 outline_radius{};
    u32 character_stencil{};
    u32 geometry_stencil{};
    u32 temporal_stencil{};
};
static_assert(sizeof(MaskConstants) == 44);
static_assert(offsetof(MaskConstants, reactive_characters) == 8);
static_assert(offsetof(MaskConstants, flags) == 24);
static_assert(offsetof(MaskConstants, outline_radius) == 28);
static_assert(offsetof(MaskConstants, character_stencil) == 32);
static_assert(offsetof(MaskConstants, geometry_stencil) == 36);
static_assert(offsetof(MaskConstants, temporal_stencil) == 40);

struct CameraMotionConstants {
    std::array<float, 16> matrix{};
    std::array<float, 4> inverse_view{};
    std::array<u32, 2> size{};
    std::array<float, 2> inv_size{};
};
static_assert(sizeof(CameraMotionConstants) == 96);
static_assert(offsetof(CameraMotionConstants, inverse_view) == 64);
static_assert(offsetof(CameraMotionConstants, size) == 80);
static_assert(offsetof(CameraMotionConstants, inv_size) == 88);

struct GrrCameraMotionConstants {
    std::array<std::array<float, 4>, 3> scene{};
    std::array<std::array<float, 4>, 3> sky{};
    std::array<float, 2> jitter{};
    std::array<u32, 2> size{};
};
static_assert(sizeof(GrrCameraMotionConstants) == 112);
static_assert(offsetof(GrrCameraMotionConstants, sky) == 48);
static_assert(offsetof(GrrCameraMotionConstants, jitter) == 96);
static_assert(offsetof(GrrCameraMotionConstants, size) == 104);

struct StoreConstants {
    std::array<u32, 2> size{};
    float rcas_stops{};
    u32 flags{};
    std::array<u32, 2> render_size{};
    u32 character_stencil{};
    u32 components{};
};
static_assert(sizeof(StoreConstants) == 32);
static_assert(offsetof(StoreConstants, rcas_stops) == 8);
static_assert(offsetof(StoreConstants, flags) == 12);
static_assert(offsetof(StoreConstants, render_size) == 16);
static_assert(offsetof(StoreConstants, character_stencil) == 24);
static_assert(offsetof(StoreConstants, components) == 28);

struct FallbackConstants {
    std::array<u32, 2> destination_size{};
    std::array<u32, 2> source_size{};
    u32 flags{};
    u32 components{};
};
static_assert(sizeof(FallbackConstants) == 24);
static_assert(offsetof(FallbackConstants, source_size) == 8);
static_assert(offsetof(FallbackConstants, flags) == 16);
static_assert(offsetof(FallbackConstants, components) == 20);

struct DebugConstants {
    std::array<u32, 2> destination_size{};
    std::array<u32, 2> source_size{};
    u32 view{};
    u32 flags{};
    float near_plane{};
    float far_plane{};
    std::array<float, 2> motion_vector_scale{};
    std::array<u32, 3> stencil_bits{};
    u32 components{};
};
static_assert(sizeof(DebugConstants) == 56);
static_assert(offsetof(DebugConstants, view) == 16);
static_assert(offsetof(DebugConstants, flags) == 20);
static_assert(offsetof(DebugConstants, near_plane) == 24);
static_assert(offsetof(DebugConstants, far_plane) == 28);
static_assert(offsetof(DebugConstants, motion_vector_scale) == 32);
static_assert(offsetof(DebugConstants, stencil_bits) == 40);
static_assert(offsetof(DebugConstants, components) == 52);

struct Fsr4InputsConstants {
    std::array<u32, 2> size{};
    u32 flags{};
    float near_plane{};
    float far_plane{};
};
static_assert(sizeof(Fsr4InputsConstants) == 20);
static_assert(offsetof(Fsr4InputsConstants, flags) == 8);
static_assert(offsetof(Fsr4InputsConstants, near_plane) == 12);
static_assert(offsetof(Fsr4InputsConstants, far_plane) == 16);

struct Pass {
    vk::UniqueDescriptorSetLayout set_layout{};
    vk::UniquePipelineLayout layout{};
    vk::UniquePipeline pipeline{};
};

/// A host image with one mip and one layer. An empty extent means no image.
struct HostImage {
    VideoCore::UniqueImage image{};
    vk::UniqueImageView view{};
    vk::Format format{vk::Format::eUndefined};
    vk::Extent2D extent{};
};

template <typename T>
bool Take(vk::ResultValue<T>&& created, T& target, const char* pass, const char* object) {
    if (created.result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2 FSR3: cannot create the {} {}: {}", pass, object,
                  vk::to_string(created.result));
        return false;
    }
    target = std::move(created.value);
    return true;
}

vk::DescriptorImageInfo ImageDescriptor(vk::ImageView view) {
    return {.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
}

// One byte per component, the order Fsr3Reorder of fsr3_color.glsl takes.
u32 PackComponents(const std::array<u8, 4>& components) {
    return u32{components[0]} | u32{components[1]} << 8 | u32{components[2]} << 16 |
           u32{components[3]} << 24;
}

FsrImage Describe(const HostImage& host_image, bool written) {
    return {
        .image = host_image.image,
        .format = host_image.format,
        .extent = host_image.extent,
        .usage = HostImageUsage,
        .written = written,
    };
}

// Every access to a host image is a compute shader access. The source scope covers the writes of
// earlier dispatches and, before an overwrite, the reads of the previous frame.
vk::ImageMemoryBarrier2 ComputeBarrier(vk::Image image, vk::ImageLayout old_layout,
                                       vk::AccessFlags2 dst_access) {
    return {
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

/// The image is written again from scratch; its contents are discarded.
vk::ImageMemoryBarrier2 OverwriteBarrier(vk::Image image, vk::AccessFlags2 dst_access) {
    return ComputeBarrier(image, vk::ImageLayout::eUndefined, dst_access);
}

/// The image keeps what a compute shader wrote into it.
vk::ImageMemoryBarrier2 AfterWriteBarrier(vk::Image image, vk::AccessFlags2 dst_access) {
    return ComputeBarrier(image, vk::ImageLayout::eGeneral, dst_access);
}

void Barrier(const Vulkan::CommandRecorder& cmdbuf,
             std::span<const vk::ImageMemoryBarrier2> barriers) {
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

/// Binds the pass, pushes one descriptor per binding and dispatches 8x8 groups over `size`.
template <std::size_t N, typename Constants>
void Record(const Vulkan::CommandRecorder& cmdbuf, const Pass& pass,
            const std::array<vk::DescriptorType, N>& types,
            const std::array<vk::DescriptorImageInfo, N>& images, const Constants& constants,
            vk::Extent2D size) {
    std::array<vk::WriteDescriptorSet, N> writes{};
    for (u32 binding = 0; binding < N; ++binding) {
        writes[binding] = vk::WriteDescriptorSet{
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = types[binding],
            .pImageInfo = &images[binding],
        };
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pass.pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pass.layout, 0, writes);
    cmdbuf.pushConstants(*pass.layout, vk::ShaderStageFlagBits::eCompute, 0,
                         static_cast<u32>(sizeof(Constants)), &constants);
    cmdbuf.dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
}

} // namespace

struct Passes::Impl {
    explicit Impl(const Vulkan::Instance& instance)
        : device{instance.GetDevice()}, allocator{instance.GetAllocator()} {}

    template <typename Constants, std::size_t N>
    bool Build(Pass& pass, const std::array<vk::DescriptorType, N>& types,
               std::span<const u32> code, const char* name) {
        std::array<vk::DescriptorSetLayoutBinding, N> bindings{};
        for (u32 binding = 0; binding < N; ++binding) {
            bindings[binding] = vk::DescriptorSetLayoutBinding{
                .binding = binding,
                .descriptorType = types[binding],
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute,
            };
        }
        if (!Take(device.createDescriptorSetLayoutUnique({
                      .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor,
                      .bindingCount = static_cast<u32>(bindings.size()),
                      .pBindings = bindings.data(),
                  }),
                  pass.set_layout, name, "descriptor set layout")) {
            return false;
        }
        const vk::PushConstantRange push_constants{
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .offset = 0,
            .size = static_cast<u32>(sizeof(Constants)),
        };
        if (!Take(device.createPipelineLayoutUnique({
                      .setLayoutCount = 1,
                      .pSetLayouts = &pass.set_layout.get(),
                      .pushConstantRangeCount = 1,
                      .pPushConstantRanges = &push_constants,
                  }),
                  pass.layout, name, "pipeline layout")) {
            return false;
        }
        const vk::ShaderModule shader = Vulkan::CompileSPV(code, device);
        auto pipeline = device.createComputePipelineUnique(
            {}, {
                    .stage{
                        .stage = vk::ShaderStageFlagBits::eCompute,
                        .module = shader,
                        .pName = "main",
                    },
                    .layout = *pass.layout,
                });
        device.destroyShaderModule(shader);
        if (!Take(std::move(pipeline), pass.pipeline, name, "pipeline")) {
            return false;
        }
        Vulkan::SetObjectName(device, *pass.set_layout, "GR2 FSR3 {} set layout", name);
        Vulkan::SetObjectName(device, *pass.layout, "GR2 FSR3 {} pipeline layout", name);
        Vulkan::SetObjectName(device, *pass.pipeline, "GR2 FSR3 {} pipeline", name);
        return true;
    }

    /// Gives the image the extent, retiring the old image once the GPU passes the current tick.
    /// Returns true when the image was replaced.
    bool ResizeImage(Vulkan::Scheduler& scheduler, HostImage& host_image, vk::Format format,
                     vk::Extent2D extent, const char* name) {
        if (host_image.extent == extent) {
            return false;
        }
        if (host_image.image) {
            scheduler.DeferOperation([retired = std::exchange(host_image, HostImage{})] {});
        }
        if (extent.width == 0) {
            return true;
        }
        host_image.image = VideoCore::UniqueImage{device, allocator};
        host_image.image.Create({
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = HostImageUsage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        host_image.view =
            Vulkan::Check<"create a GR2 FSR3 image view">(device.createImageViewUnique({
                .image = host_image.image,
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .subresourceRange = ColorRange,
            }));
        host_image.format = format;
        host_image.extent = extent;
        Vulkan::SetObjectName(device, static_cast<vk::Image>(host_image.image), name);
        Vulkan::SetObjectName(device, *host_image.view, name);
        return true;
    }

    /// Gives every host image its extent; an empty extent releases the image.
    void ResizeImages(Vulkan::Scheduler& scheduler, vk::Extent2D mask_size,
                      vk::Extent2D output_size, vk::Extent2D camera_motion_size,
                      vk::Extent2D debug_size, vk::Extent2D depth_copy_size) {
        ResizeImage(scheduler, reactive, vk::Format::eR8Unorm, mask_size, "GR2 FSR3 reactive mask");
        ResizeImage(scheduler, tc, vk::Format::eR8Unorm, mask_size, "GR2 FSR3 transparency mask");
        if (ResizeImage(scheduler, character_history, vk::Format::eR8Unorm, mask_size,
                        "GR2 FSR3 character history")) {
            history_valid = false;
            character_history_initialized = false;
        }
        ResizeImage(scheduler, output, vk::Format::eR16G16B16A16Sfloat, output_size,
                    "GR2 FSR3 output");
        ResizeImage(scheduler, camera_mv, vk::Format::eR16G16Sfloat, camera_motion_size,
                    "GR2 FSR3 camera motion vectors");
        ResizeImage(scheduler, depth_copy, vk::Format::eR32Sfloat, depth_copy_size,
                    "GR2 FSR3 depth copy");
        for (HostImage& history : debug_history) {
            if (ResizeImage(scheduler, history, vk::Format::eR16G16B16A16Sfloat, debug_size,
                            "GR2 FSR3 debug history")) {
                debug_history_valid = false;
            }
        }
    }

    vk::DescriptorImageInfo SamplerDescriptor() const {
        return {.sampler = *linear_sampler};
    }

    vk::Device device{};
    VmaAllocator allocator{};
    vk::UniqueSampler linear_sampler{};
    Pass mask{};
    Pass camera_motion{};
    Pass store{};
    Pass fallback{};
    Pass debug{};
    Pass fsr4_inputs{};       ///< Built by InitFsr4.
    Pass grr_camera_motion{}; ///< Built by InitGrr.
    HostImage reactive{};
    HostImage tc{};
    HostImage character_history{};
    HostImage output{};
    HostImage camera_mv{};
    HostImage depth_copy{};
    std::array<HostImage, 2> debug_history{};
    bool history_valid{};
    bool character_history_initialized{};
    bool debug_history_valid{};
    u32 debug_index{};
    bool mask_written{};
    bool camera_written{};
    bool depth_written{};
};

Passes::Passes(const Vulkan::Instance& instance) : impl{std::make_unique<Impl>(instance)} {}

Passes::~Passes() = default;

bool Passes::Init() {
    Impl& state = *impl;
    if (!Take(state.device.createSamplerUnique({
                  .magFilter = vk::Filter::eLinear,
                  .minFilter = vk::Filter::eLinear,
                  .mipmapMode = vk::SamplerMipmapMode::eNearest,
                  .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                  .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                  .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                  .maxAnisotropy = 1.0f,
                  .minLod = -1000.0f,
                  .maxLod = 1000.0f,
              }),
              state.linear_sampler, "linear", "sampler")) {
        return false;
    }
    Vulkan::SetObjectName(state.device, *state.linear_sampler, "GR2 FSR3 linear sampler");
    return state.Build<MaskConstants>(state.mask, MaskBindings, FSR3_MASK_COMP, "mask") &&
           state.Build<CameraMotionConstants>(state.camera_motion, CameraMotionBindings,
                                              FSR3_MVCOPY_COMP, "camera motion") &&
           state.Build<StoreConstants>(state.store, StoreBindings, FSR3_STORE_COMP, "store") &&
           state.Build<FallbackConstants>(state.fallback, FallbackBindings, FSR3_FALLBACK_COMP,
                                          "fallback") &&
           state.Build<DebugConstants>(state.debug, DebugBindings, FSR3_DEBUG_COMP, "debug");
}

bool Passes::InitFsr4() {
    Impl& state = *impl;
    return state.fsr4_inputs.pipeline ||
           state.Build<Fsr4InputsConstants>(state.fsr4_inputs, Fsr4InputsBindings, FSR4_INPUTS_COMP,
                                            "FSR 4 inputs");
}

bool Passes::InitGrr() {
    Impl& state = *impl;
    return state.grr_camera_motion.pipeline ||
           state.Build<GrrCameraMotionConstants>(state.grr_camera_motion, GrrCameraMotionBindings,
                                                 GRR_CAMERA_MOTION_COMP, "GRR camera motion");
}

void Passes::Resize(Vulkan::Scheduler& scheduler, vk::Extent2D render_size,
                    vk::Extent2D output_size, bool camera_motion, bool debug_history,
                    bool depth_copy) {
    impl->ResizeImages(
        scheduler, render_size, output_size, camera_motion ? render_size : vk::Extent2D{},
        debug_history ? render_size : vk::Extent2D{}, depth_copy ? render_size : vk::Extent2D{});
}

void Passes::ResizeFsr4(Vulkan::Scheduler& scheduler, vk::Extent2D render_size, bool camera_motion,
                        bool depth_copy) {
    impl->ResizeImages(scheduler, {}, {}, camera_motion ? render_size : vk::Extent2D{}, {},
                       depth_copy ? render_size : vk::Extent2D{});
}

void Passes::ResetHistory() {
    impl->history_valid = false;
    impl->debug_history_valid = false;
}

void Passes::RecordMask(const Vulkan::CommandRecorder& cmdbuf, const MaskParams& params) {
    Impl& state = *impl;
    constexpr vk::AccessFlags2 read_write =
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
    const std::array barriers{
        OverwriteBarrier(state.reactive.image, vk::AccessFlagBits2::eShaderStorageWrite),
        OverwriteBarrier(state.tc.image, vk::AccessFlagBits2::eShaderStorageWrite),
        state.character_history_initialized
            ? AfterWriteBarrier(state.character_history.image, read_write)
            : OverwriteBarrier(state.character_history.image, read_write),
    };
    Barrier(cmdbuf, barriers);
    const MaskConstants constants{
        .size = {params.render_size.width, params.render_size.height},
        .reactive_characters = params.reactive_characters,
        .reactive_effects = params.reactive_effects,
        .reactive_outline = params.reactive_outline,
        .tc_characters = params.tc_characters,
        .flags = (params.character_history ? MaskUseHistory : 0u) |
                 (state.history_valid ? MaskHistoryValid : 0u),
        .outline_radius = OutlineRadius,
        .character_stencil = params.character_stencil,
        .geometry_stencil = params.geometry_stencil,
        .temporal_stencil = params.temporal_stencil,
    };
    Record(cmdbuf, state.mask, MaskBindings,
           std::array{ImageDescriptor(params.stencil), ImageDescriptor(*state.reactive.view),
                      ImageDescriptor(*state.tc.view),
                      ImageDescriptor(*state.character_history.view)},
           constants, params.render_size);
    state.mask_written = true;
    state.history_valid = true;
    state.character_history_initialized = true;
}

void Passes::RecordCameraMotion(const Vulkan::CommandRecorder& cmdbuf,
                                const CameraMotionParams& params) {
    Impl& state = *impl;
    const std::array barriers{
        OverwriteBarrier(state.camera_mv.image, vk::AccessFlagBits2::eShaderStorageWrite)};
    Barrier(cmdbuf, barriers);
    const vk::Extent2D size = params.render_size;
    const CameraMotionConstants constants{
        .matrix = params.matrix,
        .inverse_view = params.inverse_view,
        .size = {size.width, size.height},
        .inv_size = {1.0f / static_cast<float>(size.width), 1.0f / static_cast<float>(size.height)},
    };
    Record(cmdbuf, state.camera_motion, CameraMotionBindings,
           std::array{ImageDescriptor(params.motion_vectors), ImageDescriptor(params.depth),
                      ImageDescriptor(params.stencil), ImageDescriptor(*state.camera_mv.view)},
           constants, size);
    state.camera_written = true;
}

void Passes::RecordGrrCameraMotion(const Vulkan::CommandRecorder& cmdbuf,
                                   const GrrCameraMotionParams& params) {
    Impl& state = *impl;
    const std::array barriers{
        OverwriteBarrier(state.camera_mv.image, vk::AccessFlagBits2::eShaderStorageWrite),
        OverwriteBarrier(state.depth_copy.image, vk::AccessFlagBits2::eShaderStorageWrite),
    };
    Barrier(cmdbuf, barriers);
    const vk::Extent2D size = params.render_size;
    const GrrCameraMotionConstants constants{
        .scene = params.scene,
        .sky = params.sky,
        .jitter = {params.jitter_x, params.jitter_y},
        .size = {size.width, size.height},
    };
    Record(cmdbuf, state.grr_camera_motion, GrrCameraMotionBindings,
           std::array{ImageDescriptor(params.depth), ImageDescriptor(*state.camera_mv.view),
                      ImageDescriptor(*state.depth_copy.view)},
           constants, size);
    state.camera_written = true;
    state.depth_written = true;
}

void Passes::PrepareFsr(const Vulkan::CommandRecorder& cmdbuf) {
    Impl& state = *impl;
    std::array<vk::ImageMemoryBarrier2, 5> barriers{
        OverwriteBarrier(state.output.image, vk::AccessFlagBits2::eShaderRead |
                                                 vk::AccessFlagBits2::eShaderStorageWrite)};
    u32 count = 1;
    if (state.mask_written) {
        barriers[count++] =
            AfterWriteBarrier(state.reactive.image, vk::AccessFlagBits2::eShaderRead);
        barriers[count++] = AfterWriteBarrier(state.tc.image, vk::AccessFlagBits2::eShaderRead);
    }
    if (state.camera_written) {
        barriers[count++] =
            AfterWriteBarrier(state.camera_mv.image, vk::AccessFlagBits2::eShaderRead);
    }
    if (state.depth_written) {
        barriers[count++] =
            AfterWriteBarrier(state.depth_copy.image, vk::AccessFlagBits2::eShaderRead);
    }
    Barrier(cmdbuf, {barriers.data(), count});
    state.mask_written = false;
    state.camera_written = false;
    state.depth_written = false;
}

void Passes::RecordFsr4Inputs(const Vulkan::CommandRecorder& cmdbuf,
                              const Fsr4InputsParams& params) {
    Impl& state = *impl;
    const Fsr4Images& targets = params.targets;
    std::array<vk::ImageMemoryBarrier2, 5> barriers{
        OverwriteBarrier(targets.color.image, vk::AccessFlagBits2::eShaderStorageWrite),
        OverwriteBarrier(targets.motion_vectors.image, vk::AccessFlagBits2::eShaderStorageWrite),
        OverwriteBarrier(targets.depth.image, vk::AccessFlagBits2::eShaderStorageWrite),
    };
    u32 count = 3;
    if (state.camera_written) {
        barriers[count++] =
            AfterWriteBarrier(state.camera_mv.image, vk::AccessFlagBits2::eShaderRead);
    }
    if (state.depth_written) {
        barriers[count++] =
            AfterWriteBarrier(state.depth_copy.image, vk::AccessFlagBits2::eShaderRead);
    }
    Barrier(cmdbuf, {barriers.data(), count});
    state.camera_written = false;
    state.depth_written = false;
    const Fsr4InputsConstants constants{
        .size = {params.render_size.width, params.render_size.height},
        .flags = (params.encode_srgb ? Fsr4InputsEncodeSrgb : 0u) |
                 (params.depth_inverted ? Fsr4InputsDepthInverted : 0u) |
                 (params.depth_linear ? Fsr4InputsDepthLinear : 0u) |
                 (params.zero_motion ? Fsr4InputsZeroMotion : 0u),
        .near_plane = params.near_plane,
        .far_plane = params.far_plane,
    };
    Record(
        cmdbuf, state.fsr4_inputs, Fsr4InputsBindings,
        std::array{
            ImageDescriptor(params.color),
            ImageDescriptor(params.camera_motion ? *state.camera_mv.view : params.motion_vectors),
            ImageDescriptor(params.host_depth ? *state.depth_copy.view : params.depth),
            ImageDescriptor(targets.color.view),
            ImageDescriptor(targets.motion_vectors.view),
            ImageDescriptor(targets.depth.view),
        },
        constants, params.render_size);
}

void Passes::RecordStore(const Vulkan::CommandRecorder& cmdbuf, const StoreParams& params) {
    Impl& state = *impl;
    const Fsr4Image source =
        params.source.image ? params.source
                            : Fsr4Image{.image = state.output.image, .view = *state.output.view};
    const std::array barriers{AfterWriteBarrier(source.image, vk::AccessFlagBits2::eShaderRead)};
    Barrier(cmdbuf, barriers);
    const StoreConstants constants{
        .size = {params.output_size.width, params.output_size.height},
        .rcas_stops = params.rcas_stops,
        .flags = (params.rcas_stops >= 0.0f ? StoreRcas : 0u) |
                 (params.linear_input ? StoreLinearInput : 0u) |
                 (params.fxaa != Fxaa::Off ? StoreFxaa : 0u) |
                 (params.fxaa == Fxaa::Characters ? StoreFxaaCharacters : 0u) |
                 (params.alpha_one ? StoreAlphaOne : 0u),
        .render_size = {params.render_size.width, params.render_size.height},
        .character_stencil = params.character_stencil,
        .components = PackComponents(params.components),
    };
    // The stencil may be a null descriptor when FXAA does not read it.
    Record(cmdbuf, state.store, StoreBindings,
           std::array{ImageDescriptor(source.view), state.SamplerDescriptor(),
                      ImageDescriptor(params.stencil), ImageDescriptor(params.destination)},
           constants, params.output_size);
}

void Passes::RecordFallback(const Vulkan::CommandRecorder& cmdbuf, const FallbackParams& params) {
    Impl& state = *impl;
    const FallbackConstants constants{
        .destination_size = {params.destination_size.width, params.destination_size.height},
        .source_size = {params.source_size.width, params.source_size.height},
        .flags = (params.easu ? FallbackEasu : 0u) | (params.alpha_one ? FallbackAlphaOne : 0u),
        .components = PackComponents(params.components),
    };
    Record(cmdbuf, state.fallback, FallbackBindings,
           std::array{ImageDescriptor(params.source), state.SamplerDescriptor(),
                      ImageDescriptor(params.destination)},
           constants, params.destination_size);
}

void Passes::RecordDebug(const Vulkan::CommandRecorder& cmdbuf, const DebugParams& params) {
    Impl& state = *impl;
    const bool masks = params.view == DebugView::Masks;
    const bool reprojection = params.view == DebugView::Reprojection;
    std::array<vk::ImageMemoryBarrier2, 6> barriers{};
    u32 count = 0;
    if (state.mask_written) {
        barriers[count++] =
            AfterWriteBarrier(state.reactive.image, vk::AccessFlagBits2::eShaderRead);
        barriers[count++] = AfterWriteBarrier(state.tc.image, vk::AccessFlagBits2::eShaderRead);
    }
    if (state.camera_written) {
        barriers[count++] =
            AfterWriteBarrier(state.camera_mv.image, vk::AccessFlagBits2::eShaderRead);
    }
    if (state.depth_written) {
        barriers[count++] =
            AfterWriteBarrier(state.depth_copy.image, vk::AccessFlagBits2::eShaderRead);
    }
    state.mask_written = false;
    state.camera_written = false;
    state.depth_written = false;
    if (reprojection) {
        constexpr vk::AccessFlags2 read_write =
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderStorageWrite;
        for (const HostImage& history : state.debug_history) {
            barriers[count++] = state.debug_history_valid
                                    ? AfterWriteBarrier(history.image, read_write)
                                    : OverwriteBarrier(history.image, read_write);
        }
    }
    Barrier(cmdbuf, {barriers.data(), count});

    const HostImage& history_in = state.debug_history[state.debug_index];
    const HostImage& history_out = state.debug_history[state.debug_index ^ 1];
    const DebugConstants constants{
        .destination_size = {params.destination_size.width, params.destination_size.height},
        .source_size = {params.source_size.width, params.source_size.height},
        .view = static_cast<u32>(params.view),
        .flags = (state.debug_history_valid ? DebugHistoryValid : 0u) |
                 (params.depth_copy ? DebugDepthCopy : 0u) |
                 (params.depth_main ? DebugDepthMain : 0u),
        .near_plane = params.camera.near_plane,
        .far_plane = params.camera.far_plane,
        .motion_vector_scale = {params.motion_vector_scale_x, params.motion_vector_scale_y},
        .stencil_bits = params.stencil_bits,
        .components = PackComponents(params.components),
    };
    // Host images the selected view does not read are bound as null descriptors.
    Record(cmdbuf, state.debug, DebugBindings,
           std::array{
               ImageDescriptor(params.color),
               ImageDescriptor(params.motion_vectors),
               ImageDescriptor(params.stencil),
               ImageDescriptor(params.depth_copy),
               ImageDescriptor(params.depth_main),
               ImageDescriptor(masks ? *state.reactive.view : vk::ImageView{}),
               ImageDescriptor(masks ? *state.tc.view : vk::ImageView{}),
               ImageDescriptor(reprojection ? *history_in.view : vk::ImageView{}),
               ImageDescriptor(reprojection ? *history_out.view : vk::ImageView{}),
               state.SamplerDescriptor(),
               ImageDescriptor(params.destination),
           },
           constants, params.destination_size);
    if (reprojection) {
        state.debug_index ^= 1;
        state.debug_history_valid = true;
    }
}

FsrImage Passes::ReactiveMask() const {
    return Describe(impl->reactive, false);
}

FsrImage Passes::TransparencyMask() const {
    return Describe(impl->tc, false);
}

FsrImage Passes::CameraMotionVectors() const {
    return Describe(impl->camera_mv, false);
}

FsrImage Passes::DepthCopy() const {
    return Describe(impl->depth_copy, false);
}

FsrImage Passes::Output() const {
    return Describe(impl->output, true);
}

vk::ImageView Passes::CameraMotionView() const {
    return *impl->camera_mv.view;
}

vk::ImageView Passes::DepthCopyView() const {
    return *impl->depth_copy.view;
}

u64 Passes::EstimateBytes(vk::Extent2D render_size, vk::Extent2D output_size, bool camera_motion,
                          bool depth_copy) {
    const u64 render_pixels = u64{render_size.width} * render_size.height;
    const u64 output_pixels = u64{output_size.width} * output_size.height;
    return 3 * render_pixels + 8 * output_pixels + (camera_motion ? 4 * render_pixels : 0) +
           (depth_copy ? 4 * render_pixels : 0);
}

u64 Passes::EstimateFsr4Bytes(vk::Extent2D render_size, bool camera_motion, bool depth_copy) {
    const u64 render_pixels = u64{render_size.width} * render_size.height;
    return (camera_motion ? 4 * render_pixels : 0) + (depth_copy ? 4 * render_pixels : 0);
}

} // namespace Gr2Fsr3
