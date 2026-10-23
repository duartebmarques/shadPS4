// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <type_traits>

#include "video_core/renderer_vulkan/vk_common.h"

// The FidelityFX libraries and the FSR 4 v07 provider call these Vulkan commands directly, and the
// emulator links no Vulkan loader. Each definition forwards to the dynamic dispatcher, which is
// initialized before any FidelityFX call, and is checked against its PFN_ type because
// VK_NO_PROTOTYPES hides the prototypes.
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
vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* allocate_info,
                         VkDescriptorSet* descriptor_sets) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateDescriptorSets(device, allocate_info,
                                                                  descriptor_sets);
}
static_assert(std::is_same_v<decltype(&vkAllocateDescriptorSets), PFN_vkAllocateDescriptorSets>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* allocate_info,
                 const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory(device, allocate_info, allocator, memory);
}
static_assert(std::is_same_v<decltype(&vkAllocateMemory), PFN_vkAllocateMemory>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device,
                                                                      VkBuffer buffer,
                                                                      VkDeviceMemory memory,
                                                                      VkDeviceSize memory_offset) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkBindBufferMemory(device, buffer, memory, memory_offset);
}
static_assert(std::is_same_v<decltype(&vkBindBufferMemory), PFN_vkBindBufferMemory>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image,
                                                                     VkDeviceMemory memory,
                                                                     VkDeviceSize memory_offset) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkBindImageMemory(device, image, memory, memory_offset);
}
static_assert(std::is_same_v<decltype(&vkBindImageMemory), PFN_vkBindImageMemory>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkCmdBindDescriptorSets(VkCommandBuffer command_buffer, VkPipelineBindPoint pipeline_bind_point,
                        VkPipelineLayout layout, uint32_t first_set, uint32_t descriptor_set_count,
                        const VkDescriptorSet* descriptor_sets, uint32_t dynamic_offset_count,
                        const uint32_t* dynamic_offsets) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindDescriptorSets(
        command_buffer, pipeline_bind_point, layout, first_set, descriptor_set_count,
        descriptor_sets, dynamic_offset_count, dynamic_offsets);
}
static_assert(std::is_same_v<decltype(&vkCmdBindDescriptorSets), PFN_vkCmdBindDescriptorSets>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
    VkCommandBuffer command_buffer, VkPipelineBindPoint pipeline_bind_point, VkPipeline pipeline) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindPipeline(command_buffer, pipeline_bind_point, pipeline);
}
static_assert(std::is_same_v<decltype(&vkCmdBindPipeline), PFN_vkCmdBindPipeline>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(
    VkCommandBuffer command_buffer, VkImage image, VkImageLayout image_layout,
    const VkClearColorValue* color, uint32_t range_count, const VkImageSubresourceRange* ranges) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdClearColorImage(command_buffer, image, image_layout, color,
                                                       range_count, ranges);
}
static_assert(std::is_same_v<decltype(&vkCmdClearColorImage), PFN_vkCmdClearColorImage>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer command_buffer,
                                                               VkBuffer src_buffer,
                                                               VkBuffer dst_buffer,
                                                               uint32_t region_count,
                                                               const VkBufferCopy* regions) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer,
                                                  region_count, regions);
}
static_assert(std::is_same_v<decltype(&vkCmdCopyBuffer), PFN_vkCmdCopyBuffer>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer command_buffer,
                                                             uint32_t group_count_x,
                                                             uint32_t group_count_y,
                                                             uint32_t group_count_z) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDispatch(command_buffer, group_count_x, group_count_y,
                                                group_count_z);
}
static_assert(std::is_same_v<decltype(&vkCmdDispatch), PFN_vkCmdDispatch>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer command_buffer,
                                                               VkBuffer dst_buffer,
                                                               VkDeviceSize dst_offset,
                                                               VkDeviceSize size, uint32_t data) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdFillBuffer(command_buffer, dst_buffer, dst_offset, size,
                                                  data);
}
static_assert(std::is_same_v<decltype(&vkCmdFillBuffer), PFN_vkCmdFillBuffer>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer command_buffer, VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask, VkDependencyFlags dependency_flags,
    uint32_t memory_barrier_count, const VkMemoryBarrier* memory_barriers,
    uint32_t buffer_memory_barrier_count, const VkBufferMemoryBarrier* buffer_memory_barriers,
    uint32_t image_memory_barrier_count, const VkImageMemoryBarrier* image_memory_barriers) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(
        command_buffer, src_stage_mask, dst_stage_mask, dependency_flags, memory_barrier_count,
        memory_barriers, buffer_memory_barrier_count, buffer_memory_barriers,
        image_memory_barrier_count, image_memory_barriers);
}
static_assert(std::is_same_v<decltype(&vkCmdPipelineBarrier), PFN_vkCmdPipelineBarrier>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* create_info,
               const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateBuffer(device, create_info, allocator, buffer);
}
static_assert(std::is_same_v<decltype(&vkCreateBuffer), PFN_vkCreateBuffer>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipeline_cache, uint32_t create_info_count,
    const VkComputePipelineCreateInfo* create_infos, const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateComputePipelines(
        device, pipeline_cache, create_info_count, create_infos, allocator, pipelines);
}
static_assert(std::is_same_v<decltype(&vkCreateComputePipelines), PFN_vkCreateComputePipelines>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* create_info,
                       const VkAllocationCallbacks* allocator, VkDescriptorPool* descriptor_pool) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDescriptorPool(device, create_info, allocator,
                                                                descriptor_pool);
}
static_assert(std::is_same_v<decltype(&vkCreateDescriptorPool), PFN_vkCreateDescriptorPool>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDescriptorSetLayout* set_layout) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDescriptorSetLayout(device, create_info, allocator,
                                                                     set_layout);
}
static_assert(
    std::is_same_v<decltype(&vkCreateDescriptorSetLayout), PFN_vkCreateDescriptorSetLayout>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo* create_info,
              const VkAllocationCallbacks* allocator, VkImage* image) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImage(device, create_info, allocator, image);
}
static_assert(std::is_same_v<decltype(&vkCreateImage), PFN_vkCreateImage>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* create_info,
                  const VkAllocationCallbacks* allocator, VkImageView* view) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImageView(device, create_info, allocator, view);
}
static_assert(std::is_same_v<decltype(&vkCreateImageView), PFN_vkCreateImageView>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* create_info,
                       const VkAllocationCallbacks* allocator, VkPipelineLayout* pipeline_layout) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreatePipelineLayout(device, create_info, allocator,
                                                                pipeline_layout);
}
static_assert(std::is_same_v<decltype(&vkCreatePipelineLayout), PFN_vkCreatePipelineLayout>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* create_info,
                const VkAllocationCallbacks* allocator, VkSampler* sampler) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSampler(device, create_info, allocator, sampler);
}
static_assert(std::is_same_v<decltype(&vkCreateSampler), PFN_vkCreateSampler>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* create_info,
                     const VkAllocationCallbacks* allocator, VkShaderModule* shader_module) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule(device, create_info, allocator,
                                                              shader_module);
}
static_assert(std::is_same_v<decltype(&vkCreateShaderModule), PFN_vkCreateShaderModule>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyBuffer(device, buffer, allocator);
}
static_assert(std::is_same_v<decltype(&vkDestroyBuffer), PFN_vkDestroyBuffer>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(
    VkDevice device, VkDescriptorPool descriptor_pool, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDescriptorPool(device, descriptor_pool, allocator);
}
static_assert(std::is_same_v<decltype(&vkDestroyDescriptorPool), PFN_vkDestroyDescriptorPool>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout descriptor_set_layout,
                             const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDescriptorSetLayout(device, descriptor_set_layout,
                                                               allocator);
}
static_assert(
    std::is_same_v<decltype(&vkDestroyDescriptorSetLayout), PFN_vkDestroyDescriptorSetLayout>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage(device, image, allocator);
}
static_assert(std::is_same_v<decltype(&vkDestroyImage), PFN_vkDestroyImage>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
    VkDevice device, VkImageView image_view, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImageView(device, image_view, allocator);
}
static_assert(std::is_same_v<decltype(&vkDestroyImageView), PFN_vkDestroyImageView>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyPipeline(device, pipeline, allocator);
}
static_assert(std::is_same_v<decltype(&vkDestroyPipeline), PFN_vkDestroyPipeline>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipeline_layout, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyPipelineLayout(device, pipeline_layout, allocator);
}
static_assert(std::is_same_v<decltype(&vkDestroyPipelineLayout), PFN_vkDestroyPipelineLayout>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySampler(device, sampler, allocator);
}
static_assert(std::is_same_v<decltype(&vkDestroySampler), PFN_vkDestroySampler>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
    VkDevice device, VkShaderModule shader_module, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyShaderModule(device, shader_module, allocator);
}
static_assert(std::is_same_v<decltype(&vkDestroyShaderModule), PFN_vkDestroyShaderModule>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical_device, const char* layer_name,
                                     uint32_t* property_count, VkExtensionProperties* properties) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateDeviceExtensionProperties(
        physical_device, layer_name, property_count, properties);
}
static_assert(std::is_same_v<decltype(&vkEnumerateDeviceExtensionProperties),
                             PFN_vkEnumerateDeviceExtensionProperties>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(device, memory, allocator);
}
static_assert(std::is_same_v<decltype(&vkFreeMemory), PFN_vkFreeMemory>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements* memory_requirements) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferMemoryRequirements(device, buffer,
                                                                memory_requirements);
}
static_assert(
    std::is_same_v<decltype(&vkGetBufferMemoryRequirements), PFN_vkGetBufferMemoryRequirements>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                                                 const char* name) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, name);
}
static_assert(std::is_same_v<decltype(&vkGetDeviceProcAddr), PFN_vkGetDeviceProcAddr>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements* memory_requirements) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageMemoryRequirements(device, image, memory_requirements);
}
static_assert(
    std::is_same_v<decltype(&vkGetImageMemoryRequirements), PFN_vkGetImageMemoryRequirements>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures* features) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures(physical_device, features);
}
static_assert(
    std::is_same_v<decltype(&vkGetPhysicalDeviceFeatures), PFN_vkGetPhysicalDeviceFeatures>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2* features) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(physical_device, features);
}
static_assert(
    std::is_same_v<decltype(&vkGetPhysicalDeviceFeatures2), PFN_vkGetPhysicalDeviceFeatures2>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format, VkFormatProperties* format_properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties(physical_device, format,
                                                                      format_properties);
}
static_assert(std::is_same_v<decltype(&vkGetPhysicalDeviceFormatProperties),
                             PFN_vkGetPhysicalDeviceFormatProperties>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical_device, VkPhysicalDeviceMemoryProperties* memory_properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(physical_device,
                                                                      memory_properties);
}
static_assert(std::is_same_v<decltype(&vkGetPhysicalDeviceMemoryProperties),
                             PFN_vkGetPhysicalDeviceMemoryProperties>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties* properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties(physical_device, properties);
}
static_assert(
    std::is_same_v<decltype(&vkGetPhysicalDeviceProperties), PFN_vkGetPhysicalDeviceProperties>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceProperties2* properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties2(physical_device, properties);
}
static_assert(
    std::is_same_v<decltype(&vkGetPhysicalDeviceProperties2), PFN_vkGetPhysicalDeviceProperties2>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical_device, uint32_t* queue_family_property_count,
    VkQueueFamilyProperties* queue_family_properties) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, queue_family_property_count, queue_family_properties);
}
static_assert(std::is_same_v<decltype(&vkGetPhysicalDeviceQueueFamilyProperties),
                             PFN_vkGetPhysicalDeviceQueueFamilyProperties>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
            VkMemoryMapFlags flags, void** data) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkMapMemory(device, memory, offset, size, flags, data);
}
static_assert(std::is_same_v<decltype(&vkMapMemory), PFN_vkMapMemory>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(
    VkDevice device, VkDescriptorPool descriptor_pool, VkDescriptorPoolResetFlags flags) {
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkResetDescriptorPool(device, descriptor_pool, flags);
}
static_assert(std::is_same_v<decltype(&vkResetDescriptorPool), PFN_vkResetDescriptorPool>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device,
                                                             VkDeviceMemory memory) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkUnmapMemory(device, memory);
}
static_assert(std::is_same_v<decltype(&vkUnmapMemory), PFN_vkUnmapMemory>);

GR2_FSR3_TRAMPOLINE VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(
    VkDevice device, uint32_t descriptor_write_count, const VkWriteDescriptorSet* descriptor_writes,
    uint32_t descriptor_copy_count, const VkCopyDescriptorSet* descriptor_copies) {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkUpdateDescriptorSets(device, descriptor_write_count,
                                                         descriptor_writes, descriptor_copy_count,
                                                         descriptor_copies);
}
static_assert(std::is_same_v<decltype(&vkUpdateDescriptorSets), PFN_vkUpdateDescriptorSets>);
