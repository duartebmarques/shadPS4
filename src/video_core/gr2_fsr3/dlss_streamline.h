// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/gr2_fsr3/fsr4_context.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Gr2Fsr3 {

/// DLSS quality modes, numbered as sl::DLSSMode.
enum class DlssMode : u32 {
    Performance = 1,
    Balanced = 2,
    Quality = 3,
    UltraPerformance = 4,
    Dlaa = 6,
};

/// Name of a quality mode.
const char* DlssModeName(DlssMode mode);

/// The render sizes a DLSS mode accepts for an output size.
struct DlssRange {
    vk::Extent2D optimal{};
    vk::Extent2D min{};
    vk::Extent2D max{};
};

/// One DLSS evaluation of the images of a context. Trivially copyable, for Scheduler::Record.
struct DlssEvaluateInfo {
    Fsr4Images images;
    vk::Extent2D render_size{};
    vk::Extent2D output_size{};
    float jitter_x{}; ///< Render pixels, as the viewport jitter.
    float jitter_y{};
    float motion_vector_scale_x{}; ///< Times the render width, pixels.
    float motion_vector_scale_y{}; ///< Times the render height, pixels.
    bool reset{};
};

#if defined(ENABLE_GR2_FSR3) && defined(_WIN32)

/**
 * Main thread, while the instance extensions are chosen. For Gravity Rush 2 with dlssEnable or
 * Gravity Rush Remastered with grrDlssEnable set, loads Streamline from Settings::DlssDir(),
 * initializes it and appends the instance extensions DLSS needs when the instance offers all.
 */
void ExtendInstance(std::vector<const char*>& extensions);

/// Main thread, right before the device is created: appends the device extensions and enables
/// the Vulkan 1.2 features DLSS needs when the physical device supports DLSS and all of them.
void ExtendDeviceForDlss(vk::PhysicalDevice physical_device, vk::DeviceCreateInfo& create_info,
                         vk::PhysicalDeviceVulkan12Features& vk12);

/// Main thread, right after the device and the default dispatcher are set up: hands the device to
/// Streamline, which starts NGX, and routes the presentation functions through Streamline.
void OnDeviceCreated(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                     u32 queue_family_index);

/// Whether Streamline runs DLSS on the device. Any thread after the device is created.
bool DlssRunning();

/// Why DLSS does not run.
std::string DlssBootNotice();

/// GPU command thread, while no other DLSS call runs: the render sizes the mode accepts, or empty
/// with `notice` set.
std::optional<DlssRange> DlssOptimalRange(DlssMode mode, DlssPreset preset,
                                          vk::Extent2D output_size, std::string& notice);

/// GPU command thread, while no other DLSS call runs: sets the options of viewport 0. Returns
/// why they were rejected, or an empty string.
std::string DlssSetOptions(DlssMode mode, DlssPreset preset, vk::Extent2D output_size);

/// Record thread. Evaluates DLSS into info.images.output; returns why it failed, or an empty
/// string. The images are in VK_IMAGE_LAYOUT_GENERAL and the inputs are written.
std::string DlssEvaluate(vk::CommandBuffer cmdbuf, const DlssEvaluateInfo& info);

/// Record thread, after an evaluation succeeded: NGX's estimate of its device memory.
u64 DlssFeatureBytes();

/// Record thread, after the GPU finished every evaluation: releases the NGX feature.
void DlssFree();

#else

inline void ExtendInstance(std::vector<const char*>&) {}
inline void ExtendDeviceForDlss(vk::PhysicalDevice, vk::DeviceCreateInfo&,
                                vk::PhysicalDeviceVulkan12Features&) {}
inline void OnDeviceCreated(vk::Instance, vk::PhysicalDevice, vk::Device, u32) {}
inline bool DlssRunning() {
    return false;
}
inline std::string DlssBootNotice() {
    return "DLSS needs Windows";
}
inline std::optional<DlssRange> DlssOptimalRange(DlssMode, DlssPreset, vk::Extent2D, std::string&) {
    return std::nullopt;
}
inline std::string DlssSetOptions(DlssMode, DlssPreset, vk::Extent2D) {
    return "DLSS needs Windows";
}
inline std::string DlssEvaluate(vk::CommandBuffer, const DlssEvaluateInfo&) {
    return "DLSS needs Windows";
}
inline u64 DlssFeatureBytes() {
    return 0;
}
inline void DlssFree() {}

#endif

} // namespace Gr2Fsr3
