// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Gr2Fsr3 {

/// Features the Vulkan device was created with that FSR 4 v07 shaders can need.
struct Fsr4DeviceFeatures {
    bool shader_float16{};
    bool shader_float64{};
    bool shader_int8{};
    bool shader_int16{};
    bool shader_int64{};
    bool shader_integer_dot_product{};
    bool storage_buffer_8bit{};
    bool uniform_storage_buffer_8bit{};
    bool storage_buffer_16bit{};
    bool uniform_storage_buffer_16bit{};
    bool image_gather_extended{};
    bool storage_image_extended_formats{};
    bool storage_image_read_without_format{};
    bool storage_image_write_without_format{};
    bool compute_shader_derivatives{}; ///< VK_KHR_compute_shader_derivatives is enabled.
    bool derivative_group_linear{};
    bool derivative_group_quads{};
};

#ifdef ENABLE_GR2_FSR3

void ExtendDevice(vk::PhysicalDevice physical_device, vk::DeviceCreateInfo& create_info,
                  vk::PhysicalDeviceFeatures2& features, vk::PhysicalDeviceVulkan11Features& vk11,
                  vk::PhysicalDeviceVulkan12Features& vk12,
                  vk::PhysicalDeviceVulkan13Features& vk13);

/**
 * For Gravity Rush 2 with fsr4Enable set, enables the supported features and the
 * VK_KHR_compute_shader_derivatives extension that FSR 4 v07 shaders can declare, and records the
 * features the device is created with. Call after the last change to the chain, right before the
 * device is created.
 */
template <typename... Chain>
void ExtendDevice(vk::PhysicalDevice physical_device, vk::StructureChain<Chain...>& chain) {
    ExtendDevice(physical_device, chain.get(), chain.template get<vk::PhysicalDeviceFeatures2>(),
                 chain.template get<vk::PhysicalDeviceVulkan11Features>(),
                 chain.template get<vk::PhysicalDeviceVulkan12Features>(),
                 chain.template get<vk::PhysicalDeviceVulkan13Features>());
}

/// The features ExtendDevice recorded; empty unless it ran for Gravity Rush 2 with fsr4Enable set.
std::optional<Fsr4DeviceFeatures> Fsr4EnabledFeatures();

#else

template <typename... Chain>
void ExtendDevice(vk::PhysicalDevice, vk::StructureChain<Chain...>&) {}

#endif

} // namespace Gr2Fsr3
