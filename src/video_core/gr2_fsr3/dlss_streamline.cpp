// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Win32 surface functions of the default dispatcher exist only with this platform defined.
#define VK_USE_PLATFORM_WIN32_KHR

// sl_security.h has non-inline definitions, so only this file includes it.
#include <sl_security.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <magic_enum/magic_enum.hpp>

#include "common/error.h"
#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "video_core/gr2_fsr3/context_images.h"
#include "video_core/gr2_fsr3/dlss_streamline.h"
#include "video_core/gr2_fsr3/fsr3_settings.h"
#include "video_core/renderer_vulkan/vk_common.h"

// <vulkan/vulkan.h> must first be included through vk_common.h, which configures it.
#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>

namespace Gr2Fsr3 {

namespace {

using Clock = std::chrono::steady_clock;

static_assert(static_cast<u32>(DlssMode::Performance) ==
              static_cast<u32>(sl::DLSSMode::eMaxPerformance));
static_assert(static_cast<u32>(DlssMode::Balanced) == static_cast<u32>(sl::DLSSMode::eBalanced));
static_assert(static_cast<u32>(DlssMode::Quality) == static_cast<u32>(sl::DLSSMode::eMaxQuality));
static_assert(static_cast<u32>(DlssMode::UltraPerformance) ==
              static_cast<u32>(sl::DLSSMode::eUltraPerformance));
static_assert(static_cast<u32>(DlssMode::Dlaa) == static_cast<u32>(sl::DLSSMode::eDLAA));

// The files of Streamline 2.14.16 that DLSS Super Resolution needs, from bin/x64 of the SDK.
constexpr std::array<std::wstring_view, 4> RequiredFiles{L"sl.interposer.dll", L"sl.common.dll",
                                                         L"sl.dlss.dll", L"nvngx_dlss.dll"};

// With an engine version and a project ID, NGX needs no application ID from NVIDIA.
constexpr const char* EngineVersion = "GR2fork-EXODUS";
constexpr const char* ProjectId = "23459be6-f898-4aad-9dbc-18fbdd716555";

// Indexed by DlssPreset.
constexpr std::array Presets{sl::DLSSPreset::eDefault, sl::DLSSPreset::ePresetJ,
                             sl::DLSSPreset::ePresetK, sl::DLSSPreset::ePresetL,
                             sl::DLSSPreset::ePresetM};
static_assert(Presets.size() == magic_enum::enum_count<DlssPreset>());

// The import library is not linked: the core functions come from GetProcAddress and the DLSS
// functions from slGetFeatureFunction.
struct SlApi {
    SL_FUN_DECL(slInit);
    SL_FUN_DECL(slIsFeatureSupported);
    SL_FUN_DECL(slGetFeatureRequirements);
    SL_FUN_DECL(slGetFeatureVersion);
    SL_FUN_DECL(slSetVulkanInfo);
    SL_FUN_DECL(slGetFeatureFunction);
    SL_FUN_DECL(slGetNewFrameToken);
    SL_FUN_DECL(slSetConstants);
    SL_FUN_DECL(slEvaluateFeature);
    SL_FUN_DECL(slFreeResources);
    SL_FUN_DECL(slDLSSGetOptimalSettings);
    SL_FUN_DECL(slDLSSGetState);
    SL_FUN_DECL(slDLSSSetOptions);
};

// Streamline's proxies of the hooks sl_hooks.h marks mandatory for Vulkan.
struct SlProxies {
    PFN_vkQueuePresentKHR queue_present{};
    PFN_vkCreateSwapchainKHR create_swapchain{};
    PFN_vkDestroySwapchainKHR destroy_swapchain{};
    PFN_vkGetSwapchainImagesKHR get_swapchain_images{};
    PFN_vkAcquireNextImageKHR acquire_next_image{};
    PFN_vkDeviceWaitIdle device_wait_idle{};
    PFN_vkCreateWin32SurfaceKHR create_win32_surface{};
    PFN_vkDestroySurfaceKHR destroy_surface{};
};

// Each boot step runs only after the previous one succeeded.
enum class Stage : u8 { Off, Initialized, DeviceExtended, Running };

SlApi g_api{};
SlProxies g_proxies{};
sl::FeatureRequirements g_requirements{};
std::array<u8, VK_LUID_SIZE> g_luid{};
// The device create info points at these until vkCreateDevice returns.
std::vector<const char*> g_device_extensions;
// Written on the main thread before the stage is stored.
std::string g_notice;
std::atomic<Stage> g_stage{Stage::Off};

std::string_view ResultName(sl::Result result) {
    return magic_enum::enum_name(result);
}

void Disable(std::string notice) {
    LOG_WARNING(Render_Vulkan, "GR2 DLSS: off: {}", notice);
    g_notice = std::move(notice);
    g_stage.store(Stage::Off, std::memory_order_release);
}

// Streamline ends the messages it formats with a newline.
void ForwardLog(sl::LogType type, const char* message) {
    std::string_view text{message};
    if (text.ends_with('\n')) {
        text.remove_suffix(1);
    }
    switch (type) {
    case sl::LogType::eError:
        LOG_ERROR(Render_Vulkan, "GR2 DLSS: SL: {}", text);
        break;
    case sl::LogType::eWarn:
        LOG_WARNING(Render_Vulkan, "GR2 DLSS: SL: {}", text);
        break;
    default:
        LOG_INFO(Render_Vulkan, "GR2 DLSS: SL: {}", text);
        break;
    }
}

// Returns the first export the library lacks, or an empty string.
std::string ResolveExports(HMODULE module) {
    std::string missing;
    const auto resolve = [&](const char* name, auto& function) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
            GetProcAddress(module, name));
        if (!function && missing.empty()) {
            missing = name;
        }
    };
    resolve("slInit", g_api.slInit);
    resolve("slIsFeatureSupported", g_api.slIsFeatureSupported);
    resolve("slGetFeatureRequirements", g_api.slGetFeatureRequirements);
    resolve("slGetFeatureVersion", g_api.slGetFeatureVersion);
    resolve("slSetVulkanInfo", g_api.slSetVulkanInfo);
    resolve("slGetFeatureFunction", g_api.slGetFeatureFunction);
    resolve("slGetNewFrameToken", g_api.slGetNewFrameToken);
    resolve("slSetConstants", g_api.slSetConstants);
    resolve("slEvaluateFeature", g_api.slEvaluateFeature);
    resolve("slFreeResources", g_api.slFreeResources);
    resolve("vkQueuePresentKHR", g_proxies.queue_present);
    resolve("vkCreateSwapchainKHR", g_proxies.create_swapchain);
    resolve("vkDestroySwapchainKHR", g_proxies.destroy_swapchain);
    resolve("vkGetSwapchainImagesKHR", g_proxies.get_swapchain_images);
    resolve("vkAcquireNextImageKHR", g_proxies.acquire_next_image);
    resolve("vkDeviceWaitIdle", g_proxies.device_wait_idle);
    resolve("vkCreateWin32SurfaceKHR", g_proxies.create_win32_surface);
    resolve("vkDestroySurfaceKHR", g_proxies.destroy_surface);
    return missing;
}

// The names the properties do not list, comma-separated.
template <typename Properties>
std::string Unavailable(std::span<const char* const> names, const Properties& properties) {
    std::string missing;
    for (const char* name : names) {
        const bool listed = std::ranges::any_of(properties, [name](const auto& property) {
            return std::strcmp(property.extensionName, name) == 0;
        });
        if (!listed) {
            missing.append(missing.empty() ? "" : ", ").append(name);
        }
    }
    return missing;
}

// Appends the names the list lacks, as Streamline's own instance and device proxies do.
void AppendMissing(std::vector<const char*>& list, std::span<const char* const> names) {
    for (const char* name : names) {
        const bool present = std::ranges::any_of(
            list, [name](const char* entry) { return std::strcmp(entry, name) == 0; });
        if (!present) {
            list.push_back(name);
        }
    }
}

sl::AdapterInfo AdapterOf(vk::PhysicalDevice physical_device) {
    sl::AdapterInfo adapter{};
    // Without a LUID, slIsFeatureSupported skips the adapter check.
    adapter.deviceLUID = g_luid.data();
    adapter.deviceLUIDSizeInBytes = VK_LUID_SIZE;
    adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
    return adapter;
}

sl::DLSSOptions OptionsOf(DlssMode mode, DlssPreset preset, vk::Extent2D output_size) {
    const sl::DLSSPreset value = Presets[static_cast<std::size_t>(preset)];
    sl::DLSSOptions options{};
    options.mode = static_cast<sl::DLSSMode>(mode);
    options.outputWidth = output_size.width;
    options.outputHeight = output_size.height;
    // The colour is sRGB-encoded LDR; the default creates the feature for HDR colour.
    options.colorBuffersHDR = sl::Boolean::eFalse;
    options.dlaaPreset = value;
    options.qualityPreset = value;
    options.balancedPreset = value;
    options.performancePreset = value;
    options.ultraPerformancePreset = value;
    options.ultraQualityPreset = value;
    return options;
}

sl::Resource ResourceOf(const Fsr4Image& image, vk::Format format, vk::Extent2D extent) {
    sl::Resource resource{sl::ResourceType::eTex2d, static_cast<VkImage>(image.image), nullptr,
                          static_cast<VkImageView>(image.view), VK_IMAGE_LAYOUT_GENERAL};
    resource.width = extent.width;
    resource.height = extent.height;
    resource.nativeFormat = static_cast<u32>(format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return resource;
}

} // namespace

void ExtendInstance(std::vector<const char*>& extensions) {
    const bool gr2 = Common::IsGravityRush2();
    if (!gr2 && !Common::IsGravityRushRemastered()) {
        return;
    }
    const Settings::Values settings = Settings::Snapshot();
    if (!(gr2 ? settings.dlss_enable : settings.grr_dlss_enable)) {
        return;
    }
    const std::filesystem::path folder = Settings::DlssDir();
    const std::string folder_text = Common::FS::PathToUTF8String(folder);
    std::string missing;
    for (const std::wstring_view file : RequiredFiles) {
        if (!std::filesystem::exists(folder / file)) {
            missing.append(missing.empty() ? "" : ", ")
                .append(Common::FS::PathToUTF8String(std::filesystem::path{file}));
        }
    }
    if (!missing.empty()) {
        Disable(fmt::format("DLSS needs {} in {}", missing, folder_text));
        return;
    }
    const std::filesystem::path interposer = folder / RequiredFiles[0];
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        Disable(fmt::format("DLSS: sl.interposer.dll in {} is not signed by NVIDIA", folder_text));
        return;
    }
    const HMODULE module = LoadLibraryW(interposer.c_str());
    if (!module) {
        Disable(fmt::format("DLSS: sl.interposer.dll did not load: {}", Common::GetLastErrorMsg()));
        return;
    }
    if (const std::string name = ResolveExports(module); !name.empty()) {
        Disable(fmt::format("DLSS: sl.interposer.dll lacks {}", name));
        return;
    }

    const std::wstring plugin_path = folder.wstring();
    const wchar_t* plugin_paths[] = {plugin_path.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences preferences{};
    preferences.logLevel = settings.log ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = 1;
    preferences.logMessageCallback = ForwardLog;
    preferences.flags =
        sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = EngineVersion;
    preferences.projectId = ProjectId;
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    if (const sl::Result result = g_api.slInit(preferences, sl::kSDKVersion);
        result != sl::Result::eOk) {
        Disable(fmt::format("DLSS: Streamline did not start: {}", ResultName(result)));
        return;
    }
    if (const sl::Result result = g_api.slGetFeatureRequirements(sl::kFeatureDLSS, g_requirements);
        result != sl::Result::eOk) {
        Disable(fmt::format("DLSS: Streamline gave no requirements: {}", ResultName(result)));
        return;
    }
    // Streamline keeps these names until slShutdown, which is never called.
    const std::span<const char* const> names{g_requirements.vkInstanceExtensions,
                                             g_requirements.vkNumInstanceExtensions};
    const auto [properties_result, properties] = vk::enumerateInstanceExtensionProperties();
    if (const std::string lacking = Unavailable(names, properties); !lacking.empty()) {
        Disable(fmt::format("DLSS: the Vulkan instance lacks {}", lacking));
        return;
    }
    AppendMissing(extensions, names);
    LOG_INFO(Render_Vulkan,
             "GR2 DLSS: Streamline loaded from {}, signed by NVIDIA; instance extensions: {}",
             folder_text, fmt::join(names, ", "));
    g_stage.store(Stage::Initialized, std::memory_order_release);
}

void ExtendDeviceForDlss(vk::PhysicalDevice physical_device, vk::DeviceCreateInfo& create_info,
                         vk::PhysicalDeviceVulkan12Features& vk12) {
    if (g_stage.load(std::memory_order_acquire) != Stage::Initialized) {
        return;
    }
    const auto properties =
        physical_device
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
    const std::string_view device_name =
        properties.get<vk::PhysicalDeviceProperties2>().properties.deviceName.data();
    std::ranges::copy(properties.get<vk::PhysicalDeviceIDProperties>().deviceLUID, g_luid.begin());
    if (const sl::Result result =
            g_api.slIsFeatureSupported(sl::kFeatureDLSS, AdapterOf(physical_device));
        result != sl::Result::eOk) {
        Disable(fmt::format("DLSS is unavailable on {}: {}", device_name, ResultName(result)));
        return;
    }
    const std::span<const char* const> names{g_requirements.vkDeviceExtensions,
                                             g_requirements.vkNumDeviceExtensions};
    const auto [extensions_result, available] =
        physical_device.enumerateDeviceExtensionProperties();
    if (const std::string lacking = Unavailable(names, available); !lacking.empty()) {
        Disable(fmt::format("DLSS: {} lacks {}", device_name, lacking));
        return;
    }
    g_device_extensions.assign(create_info.ppEnabledExtensionNames,
                               create_info.ppEnabledExtensionNames +
                                   create_info.enabledExtensionCount);
    AppendMissing(g_device_extensions, names);
    // NGX can ask for both buffer device address extensions, which a device must not enable
    // together; Streamline's own device proxy drops the EXT one.
    const auto named = [](std::string_view wanted) {
        return [wanted](const char* name) { return wanted == name; };
    };
    if (std::ranges::any_of(g_device_extensions,
                            named(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))) {
        std::erase_if(g_device_extensions, named(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));
    }
    create_info.enabledExtensionCount = static_cast<u32>(g_device_extensions.size());
    create_info.ppEnabledExtensionNames = g_device_extensions.data();

    // The Vulkan 1.2 features merge into the chained struct as Streamline's device proxy does.
    const VkPhysicalDeviceVulkan12Features required = sl::getVkPhysicalDeviceVulkan12Features(
        g_requirements.vkNumFeatures12, g_requirements.vkFeatures12);
    const vk::PhysicalDeviceVulkan12Features supported =
        physical_device
            .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>()
            .get<vk::PhysicalDeviceVulkan12Features>();
    sl::getMergedSupportedVkPhysicalDeviceVulkanFeatures(
        reinterpret_cast<VkBaseOutStructure*>(&vk12),
        reinterpret_cast<const VkBaseOutStructure*>(&required),
        reinterpret_cast<const VkBaseOutStructure*>(&supported));
    LOG_INFO(
        Render_Vulkan,
        "GR2 DLSS: device extensions: {}; 1.2 features: {}; 1.3 features: {}; extra "
        "queues: graphics {}, compute {}",
        fmt::join(names, ", "),
        fmt::join(std::span{g_requirements.vkFeatures12, g_requirements.vkNumFeatures12}, ", "),
        g_requirements.vkNumFeatures13, g_requirements.vkNumGraphicsQueuesRequired,
        g_requirements.vkNumComputeQueuesRequired);
    g_stage.store(Stage::DeviceExtended, std::memory_order_release);
}

void OnDeviceCreated(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                     u32 queue_family_index) {
    if (g_stage.load(std::memory_order_acquire) != Stage::DeviceExtended) {
        return;
    }
    sl::VulkanInfo info{};
    info.device = static_cast<VkDevice>(device);
    info.instance = static_cast<VkInstance>(instance);
    info.physicalDevice = static_cast<VkPhysicalDevice>(physical_device);
    // DLSS creates no queue; the indices name the first one after the emulator's queue.
    info.graphicsQueueFamily = queue_family_index;
    info.graphicsQueueIndex = 1;
    info.computeQueueFamily = queue_family_index;
    info.computeQueueIndex = 1;
    LOG_INFO(Render_Vulkan, "GR2 DLSS: starting NGX");
    const Clock::time_point start = Clock::now();
    // A plugin that fails to start is unloaded without an error here; the second support check
    // reports it.
    if (const sl::Result result = g_api.slSetVulkanInfo(info); result != sl::Result::eOk) {
        Disable(fmt::format("DLSS: Streamline did not accept the device: {}", ResultName(result)));
        return;
    }
    if (const sl::Result result =
            g_api.slIsFeatureSupported(sl::kFeatureDLSS, AdapterOf(physical_device));
        result != sl::Result::eOk) {
        Disable(fmt::format("DLSS: NGX did not start: {}", ResultName(result)));
        return;
    }
    std::string lacking;
    const auto feature_function = [&lacking](const char* name, auto& function) {
        void* address{};
        g_api.slGetFeatureFunction(sl::kFeatureDLSS, name, address);
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
        if (!function && lacking.empty()) {
            lacking = name;
        }
    };
    feature_function("slDLSSGetOptimalSettings", g_api.slDLSSGetOptimalSettings);
    feature_function("slDLSSGetState", g_api.slDLSSGetState);
    feature_function("slDLSSSetOptions", g_api.slDLSSSetOptions);
    if (!lacking.empty()) {
        Disable(fmt::format("DLSS: sl.dlss.dll lacks {}", lacking));
        return;
    }
    // Without a version the boot line shows 0.0.0.
    sl::FeatureVersion version{};
    g_api.slGetFeatureVersion(sl::kFeatureDLSS, version);

    // The dispatcher loaded its device functions just before and does not load them again.
    auto& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
    dispatcher.vkQueuePresentKHR = g_proxies.queue_present;
    dispatcher.vkCreateSwapchainKHR = g_proxies.create_swapchain;
    dispatcher.vkDestroySwapchainKHR = g_proxies.destroy_swapchain;
    dispatcher.vkGetSwapchainImagesKHR = g_proxies.get_swapchain_images;
    dispatcher.vkAcquireNextImageKHR = g_proxies.acquire_next_image;
    dispatcher.vkDeviceWaitIdle = g_proxies.device_wait_idle;
    dispatcher.vkCreateWin32SurfaceKHR = g_proxies.create_win32_surface;
    dispatcher.vkDestroySurfaceKHR = g_proxies.destroy_surface;
    g_stage.store(Stage::Running, std::memory_order_release);
    LOG_INFO(Render_Vulkan,
             "GR2 DLSS: NGX ready in {:.0f} ms, sl.dlss {}, NGX {}; presentation goes through "
             "Streamline",
             std::chrono::duration<float, std::milli>(Clock::now() - start).count(),
             version.versionSL.toStr(), version.versionNGX.toStr());
}

bool DlssRunning() {
    return g_stage.load(std::memory_order_acquire) == Stage::Running;
}

std::string DlssBootNotice() {
    return g_notice.empty() ? "DLSS needs a restart: it was off when the game started" : g_notice;
}

std::optional<DlssRange> DlssOptimalRange(DlssMode mode, DlssPreset preset,
                                          vk::Extent2D output_size, std::string& notice) {
    sl::DLSSOptimalSettings settings{};
    const sl::Result result =
        g_api.slDLSSGetOptimalSettings(OptionsOf(mode, preset, output_size), settings);
    if (result == sl::Result::eErrorInvalidParameter) {
        notice = fmt::format("DLSS {} for {}x{}: mode not available", DlssModeName(mode),
                             output_size.width, output_size.height);
        return std::nullopt;
    }
    if (result != sl::Result::eOk) {
        notice =
            fmt::format("DLSS {}: no optimal settings: {}", DlssModeName(mode), ResultName(result));
        return std::nullopt;
    }
    return DlssRange{
        .optimal = {settings.optimalRenderWidth, settings.optimalRenderHeight},
        .min = {settings.renderWidthMin, settings.renderHeightMin},
        .max = {settings.renderWidthMax, settings.renderHeightMax},
    };
}

std::string DlssSetOptions(DlssMode mode, DlssPreset preset, vk::Extent2D output_size) {
    const sl::Result result =
        g_api.slDLSSSetOptions(sl::ViewportHandle{0u}, OptionsOf(mode, preset, output_size));
    if (result != sl::Result::eOk) {
        return fmt::format("DLSS options rejected: {}", ResultName(result));
    }
    return {};
}

std::string DlssEvaluate(vk::CommandBuffer cmdbuf, const DlssEvaluateInfo& info) {
    sl::FrameToken* frame{};
    if (const sl::Result result = g_api.slGetNewFrameToken(frame, nullptr);
        result != sl::Result::eOk) {
        return fmt::format("DLSS has no frame token: {}", ResultName(result));
    }
    const sl::ViewportHandle viewport{0u};
    sl::Constants constants{};
    constants.jitterOffset = sl::float2{info.jitter_x, info.jitter_y};
    constants.mvecScale = sl::float2{info.motion_vector_scale_x, info.motion_vector_scale_y};
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = info.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (const sl::Result result = g_api.slSetConstants(constants, *frame, viewport);
        result != sl::Result::eOk) {
        return fmt::format("DLSS rejected the constants: {}", ResultName(result));
    }
    const Fsr4Images& images = info.images;
    std::array resources{
        ResourceOf(images.color, ContextImageFormats[0], info.render_size),
        ResourceOf(images.motion_vectors, ContextImageFormats[1], info.render_size),
        ResourceOf(images.depth, ContextImageFormats[2], info.render_size),
        ResourceOf(images.output, ContextImageFormats[3], info.output_size),
    };
    constexpr sl::ResourceLifecycle Lifecycle = sl::ResourceLifecycle::eValidUntilEvaluate;
    std::array tags{
        sl::ResourceTag{&resources[0], sl::kBufferTypeScalingInputColor, Lifecycle},
        sl::ResourceTag{&resources[1], sl::kBufferTypeMotionVectors, Lifecycle},
        sl::ResourceTag{&resources[2], sl::kBufferTypeDepth, Lifecycle},
        sl::ResourceTag{&resources[3], sl::kBufferTypeScalingOutputColor, Lifecycle},
    };
    std::array<const sl::BaseStructure*, 5> inputs{&viewport, &tags[0], &tags[1], &tags[2],
                                                   &tags[3]};
    const sl::Result result = g_api.slEvaluateFeature(sl::kFeatureDLSS, *frame, inputs.data(),
                                                      static_cast<u32>(inputs.size()),
                                                      static_cast<VkCommandBuffer>(cmdbuf));
    // Streamline returns this warning after a completed evaluation.
    if (result == sl::Result::eWarnOutOfVRAM) {
        return "DLSS: VRAM budget exceeded (Streamline warning)";
    }
    if (result != sl::Result::eOk) {
        return fmt::format("DLSS rejected the evaluation: {}", ResultName(result));
    }
    return {};
}

u64 DlssFeatureBytes() {
    sl::DLSSState state{};
    if (g_api.slDLSSGetState(sl::ViewportHandle{0u}, state) != sl::Result::eOk) {
        return 0;
    }
    return state.estimatedVRAMUsageInBytes;
}

void DlssFree() {
    if (const sl::Result result = g_api.slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle{0u});
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan, "GR2 DLSS: Streamline did not free the feature: {}",
                    ResultName(result));
    }
}

} // namespace Gr2Fsr3
