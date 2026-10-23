// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <optional>
#include <string_view>
#include <vector>

#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "video_core/gr2_fsr3/fsr3_settings.h"
#include "video_core/gr2_fsr3/fsr4_device.h"

namespace Gr2Fsr3 {

namespace {

// The device create info points at these until vkCreateDevice returns.
vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR g_derivatives{};
std::vector<const char*> g_extensions;

Fsr4DeviceFeatures g_features{};
std::atomic<bool> g_recorded{};

} // namespace

void ExtendDevice(vk::PhysicalDevice physical_device, vk::DeviceCreateInfo& create_info,
                  vk::PhysicalDeviceFeatures2& features, vk::PhysicalDeviceVulkan11Features& vk11,
                  vk::PhysicalDeviceVulkan12Features& vk12,
                  vk::PhysicalDeviceVulkan13Features& vk13) {
    if (!Common::IsGravityRush2() || !Settings::Snapshot().fsr4_enable) {
        return;
    }
    const auto supported =
        physical_device
            .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                          vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>();
    features.features.shaderStorageImageReadWithoutFormat =
        supported.get().features.shaderStorageImageReadWithoutFormat;
    vk11.uniformAndStorageBuffer16BitAccess =
        supported.get<vk::PhysicalDeviceVulkan11Features>().uniformAndStorageBuffer16BitAccess;
    vk12.uniformAndStorageBuffer8BitAccess =
        supported.get<vk::PhysicalDeviceVulkan12Features>().uniformAndStorageBuffer8BitAccess;
    // With a Vulkan 1.3 feature struct in the chain, integer dot product is enabled there.
    vk13.shaderIntegerDotProduct =
        supported.get<vk::PhysicalDeviceVulkan13Features>().shaderIntegerDotProduct;

    // The emulator's extension list lacks this extension, so it is appended. Its feature struct
    // may be queried only when the device lists the extension.
    const auto [extensions_result, extensions] =
        physical_device.enumerateDeviceExtensionProperties();
    const bool has_derivatives =
        extensions_result == vk::Result::eSuccess &&
        std::ranges::any_of(extensions, [](const vk::ExtensionProperties& extension) {
            return std::string_view{extension.extensionName.data()} ==
                   VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;
        });
    bool derivatives_enabled = false;
    if (has_derivatives) {
        const auto derivatives =
            physical_device
                .getFeatures2<vk::PhysicalDeviceFeatures2,
                              vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>()
                .get<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>();
        derivatives_enabled = derivatives.computeDerivativeGroupLinear == VK_TRUE;
        if (derivatives_enabled) {
            g_derivatives = vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR{
                .pNext = features.pNext,
                .computeDerivativeGroupQuads = derivatives.computeDerivativeGroupQuads,
                .computeDerivativeGroupLinear = VK_TRUE,
            };
            features.pNext = &g_derivatives;
            g_extensions.assign(create_info.ppEnabledExtensionNames,
                                create_info.ppEnabledExtensionNames +
                                    create_info.enabledExtensionCount);
            g_extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
            create_info.enabledExtensionCount = static_cast<u32>(g_extensions.size());
            create_info.ppEnabledExtensionNames = g_extensions.data();
        }
    }

    const vk::PhysicalDeviceFeatures& core = features.features;
    g_features = Fsr4DeviceFeatures{
        .shader_float16 = vk12.shaderFloat16 == VK_TRUE,
        .shader_float64 = core.shaderFloat64 == VK_TRUE,
        .shader_int8 = vk12.shaderInt8 == VK_TRUE,
        .shader_int16 = core.shaderInt16 == VK_TRUE,
        .shader_int64 = core.shaderInt64 == VK_TRUE,
        .shader_integer_dot_product = vk13.shaderIntegerDotProduct == VK_TRUE,
        .storage_buffer_8bit = vk12.storageBuffer8BitAccess == VK_TRUE,
        .uniform_storage_buffer_8bit = vk12.uniformAndStorageBuffer8BitAccess == VK_TRUE,
        .storage_buffer_16bit = vk11.storageBuffer16BitAccess == VK_TRUE,
        .uniform_storage_buffer_16bit = vk11.uniformAndStorageBuffer16BitAccess == VK_TRUE,
        .image_gather_extended = core.shaderImageGatherExtended == VK_TRUE,
        .storage_image_extended_formats = core.shaderStorageImageExtendedFormats == VK_TRUE,
        .storage_image_read_without_format = core.shaderStorageImageReadWithoutFormat == VK_TRUE,
        .storage_image_write_without_format = core.shaderStorageImageWriteWithoutFormat == VK_TRUE,
        .compute_shader_derivatives = derivatives_enabled,
        .derivative_group_linear = derivatives_enabled,
        .derivative_group_quads =
            derivatives_enabled && g_derivatives.computeDerivativeGroupQuads == VK_TRUE,
    };
    g_recorded.store(true, std::memory_order_release);

    const Fsr4DeviceFeatures& enabled = g_features;
    const auto yes_no = [](bool value) { return value ? "yes" : "no"; };
    LOG_INFO(Render_Vulkan,
             "GR2 FSR4: device for FSR 4 (INT8, experimental): integer dot product {}, int8 {}, "
             "fp16 {}, int16 {}, 8-bit uniform storage {}, 16-bit uniform storage {}, read "
             "without format {}, compute shader derivatives {} (linear {}, quads {})",
             yes_no(enabled.shader_integer_dot_product), yes_no(enabled.shader_int8),
             yes_no(enabled.shader_float16), yes_no(enabled.shader_int16),
             yes_no(enabled.uniform_storage_buffer_8bit),
             yes_no(enabled.uniform_storage_buffer_16bit),
             yes_no(enabled.storage_image_read_without_format),
             yes_no(enabled.compute_shader_derivatives), yes_no(enabled.derivative_group_linear),
             yes_no(enabled.derivative_group_quads));
}

std::optional<Fsr4DeviceFeatures> Fsr4EnabledFeatures() {
    if (!g_recorded.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return g_features;
}

} // namespace Gr2Fsr3
