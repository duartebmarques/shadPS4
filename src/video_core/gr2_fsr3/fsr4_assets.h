// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/gr2_fsr3/fsr4_device.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Gr2Fsr3 {

/// What the device offers the files of a model set.
struct Fsr4DeviceLimits {
    Fsr4DeviceFeatures features{};
    u32 subgroup_operations{}; ///< VkSubgroupFeatureFlags supported in the compute stage.
    u64 max_storage_buffer_range{};
    u64 max_allocation_size{};
};

/// Shader modules of a model set: 0 pre-pass, 1-12 model passes, 13 post-pass.
constexpr std::size_t Fsr4ShaderCount = 14;

/// The files of one FSR 4 v07 model set, read and checked.
struct Fsr4Assets {
    std::array<std::vector<u32>, Fsr4ShaderCount> shaders;
    std::array<std::string, Fsr4ShaderCount> entry_points;
    std::vector<u8> initializer;
    std::vector<u8> pre_weights;
};

/// The assets, or the first problem as a notice; the summary is for the log.
struct Fsr4LoadResult {
    std::optional<Fsr4Assets> assets;
    std::string notice;
    std::string summary;
};

/**
 * Reads the model set of `preset` (GR2_FSR4_PRESET_*) for `output_size` from `folder` and checks
 * every file: present and complete, the model data sizes, SPIR-V with one GLCompute entry point,
 * capabilities and extensions the device enables, the v07 descriptor layout, and a scratch buffer
 * the device can bind. Any thread.
 */
Fsr4LoadResult LoadFsr4Assets(const std::filesystem::path& folder, u32 preset,
                              vk::Extent2D output_size, const Fsr4DeviceLimits& device);

} // namespace Gr2Fsr3
