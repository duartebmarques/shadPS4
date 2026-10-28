// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <utility>

#include <fmt/format.h>

#include "video_core/gr2_fsr3/context_images.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace Gr2Fsr3 {

namespace {

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

// Indexed as ContextImageFormats; the output is the last image.
constexpr std::array<std::string_view, 4> ImageNames{"colour", "motion vectors", "depth", "output"};
constexpr std::size_t OutputIndex = 3;

} // namespace

std::optional<ContextImages> CreateContextImages(const Vulkan::Instance& instance,
                                                 vk::Extent2D render_size, vk::Extent2D output_size,
                                                 std::string_view name_prefix,
                                                 std::string& notice) {
    const vk::Device device = instance.GetDevice();
    ContextImages created{};
    for (std::size_t i = 0; i < ContextImageFormats.size(); ++i) {
        const vk::Format format = ContextImageFormats[i];
        const vk::Extent2D extent = i == OutputIndex ? output_size : render_size;
        const std::string name = fmt::format("{} {}", name_prefix, ImageNames[i]);
        ContextImages::Owned& owned = created.images[i];
        owned.image = VideoCore::UniqueImage{device, instance.GetAllocator()};
        owned.image.Create({
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        auto [view_result, view] = device.createImageViewUnique({
            .image = owned.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = ColorRange,
        });
        if (view_result != vk::Result::eSuccess) {
            notice = fmt::format("cannot create the {} view: {}", name, vk::to_string(view_result));
            return std::nullopt;
        }
        owned.view = std::move(view);
        Vulkan::SetObjectName(device, static_cast<vk::Image>(owned.image), name);
        Vulkan::SetObjectName(device, *owned.view, name);
        created.bytes += owned.image.size_bytes;
    }
    const auto handle = [&created](std::size_t index) {
        const ContextImages::Owned& owned = created.images[index];
        return Fsr4Image{.image = owned.image, .view = *owned.view};
    };
    created.handles = Fsr4Images{
        .color = handle(0),
        .motion_vectors = handle(1),
        .depth = handle(2),
        .output = handle(OutputIndex),
    };
    return created;
}

} // namespace Gr2Fsr3
