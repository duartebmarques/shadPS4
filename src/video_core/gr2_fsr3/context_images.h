// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "common/types.h"
#include "video_core/gr2_fsr3/fsr4_context.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
} // namespace Vulkan

namespace Gr2Fsr3 {

/// Formats of the context images, in the order of Fsr4Images.
constexpr std::array<vk::Format, 4> ContextImageFormats{
    vk::Format::eR16G16B16A16Sfloat, vk::Format::eR16G16Sfloat, vk::Format::eR32Sfloat,
    vk::Format::eR16G16B16A16Sfloat};

/// The four images of an FSR 4 or DLSS context: the inputs at the render size, the output at the
/// output size.
struct ContextImages {
    struct Owned {
        VideoCore::UniqueImage image{};
        vk::UniqueImageView view{};
    };

    std::array<Owned, ContextImageFormats.size()> images{}; ///< In the order of Fsr4Images.
    Fsr4Images handles{};
    u64 bytes{}; ///< Device memory of the four images.
};

/// GPU command thread or a worker. Creates the images, naming each "<name_prefix> colour" and so
/// on, or returns empty with `notice` set to "cannot create the <name> view: <result>".
std::optional<ContextImages> CreateContextImages(const Vulkan::Instance& instance,
                                                 vk::Extent2D render_size, vk::Extent2D output_size,
                                                 std::string_view name_prefix, std::string& notice);

} // namespace Gr2Fsr3
