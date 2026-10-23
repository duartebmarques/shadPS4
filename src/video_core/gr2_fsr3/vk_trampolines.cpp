// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <type_traits>

#include "video_core/renderer_vulkan/vk_common.h"

// The FidelityFX libraries call these Vulkan commands directly, and the emulator links no Vulkan
// loader. Each definition forwards to the dynamic dispatcher, which is initialized before any
// FidelityFX call.
static_assert(std::is_same_v<std::remove_cvref_t<decltype(VULKAN_HPP_DEFAULT_DISPATCHER)>,
                             vk::detail::DispatchLoaderDynamic>);

// Hidden visibility keeps the names out of the dynamic symbol table, so the loader always binds
// its own entry points.
#ifdef _WIN32
#define GR2_FSR3_TRAMPOLINE extern "C"
#else
#define GR2_FSR3_TRAMPOLINE extern "C" __attribute__((visibility("hidden")))
#endif

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* allocate_info,
                 const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory(device, allocate_info, allocator, memory);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image,
                                                                     VkDeviceMemory memory,
                                                                     VkDeviceSize memory_offset) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkBindImageMemory(device, image, memory, memory_offset);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* create_info,
               const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateBuffer(device, create_info, allocator, buffer);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo* create_info,
              const VkAllocationCallbacks* allocator, VkImage* image) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImage(device, create_info, allocator, image);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage(device, image, allocator);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical_device, const char* layer_name,
                                     uint32_t* property_count, VkExtensionProperties* properties) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(
        physical_device, layer_name, property_count, properties);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(device, memory, allocator);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                                 const char* name) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, name);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements* memory_requirements) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageMemoryRequirements(device, image, memory_requirements);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures* features) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures(physical_device, features);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2* features) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(physical_device, features);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format, VkFormatProperties* format_properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties(physical_device, format,
                                                                      format_properties);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical_device, VkPhysicalDeviceMemoryProperties* memory_properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(physical_device,
                                                                      memory_properties);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties* properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties(physical_device, properties);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2* properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties2(physical_device, properties);
}

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical_device, uint32_t* queue_family_property_count,
    VkQueueFamilyProperties* queue_family_properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, queue_family_property_count, queue_family_properties);
}
