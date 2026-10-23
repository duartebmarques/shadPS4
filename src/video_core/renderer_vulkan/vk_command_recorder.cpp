// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void CommandRecorder::pipelineBarrier2(const vk::DependencyInfo& dependency_info) const {
    scheduler->RawCommandBuffer().pipelineBarrier2(dependency_info);
}

void CommandRecorder::bindPipeline(vk::PipelineBindPoint pipeline_bind_point,
                                   vk::Pipeline pipeline) const {
    scheduler->RawCommandBuffer().bindPipeline(pipeline_bind_point, pipeline);
}

void CommandRecorder::bindDescriptorSets(
    vk::PipelineBindPoint pipeline_bind_point, vk::PipelineLayout layout, u32 first_set,
    vk::ArrayProxy<const vk::DescriptorSet> const& descriptor_sets,
    vk::ArrayProxy<const u32> const& dynamic_offsets) const {
    scheduler->RawCommandBuffer().bindDescriptorSets(pipeline_bind_point, layout, first_set,
                                                     descriptor_sets, dynamic_offsets);
}

void CommandRecorder::pushDescriptorSetKHR(
    vk::PipelineBindPoint pipeline_bind_point, vk::PipelineLayout layout, u32 set,
    vk::ArrayProxy<const vk::WriteDescriptorSet> const& descriptor_writes) const {
    scheduler->RawCommandBuffer().pushDescriptorSetKHR(pipeline_bind_point, layout, set,
                                                       descriptor_writes);
}

void CommandRecorder::pushDescriptorSet2(
    const vk::PushDescriptorSetInfo& push_descriptor_set_info) const {
    scheduler->RawCommandBuffer().pushDescriptorSet2(push_descriptor_set_info);
}

void CommandRecorder::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stage_flags,
                                    u32 offset, u32 size, const void* values) const {
    scheduler->RawCommandBuffer().pushConstants(layout, stage_flags, offset, size, values);
}

void CommandRecorder::pushConstants2(const vk::PushConstantsInfo& push_constants_info) const {
    scheduler->RawCommandBuffer().pushConstants2(push_constants_info);
}

void CommandRecorder::bindVertexBuffers(u32 first_binding, u32 binding_count,
                                        const vk::Buffer* buffers,
                                        const vk::DeviceSize* offsets) const {
    scheduler->RawCommandBuffer().bindVertexBuffers(first_binding, binding_count, buffers, offsets);
}

void CommandRecorder::bindVertexBuffers2(u32 first_binding, u32 binding_count,
                                         const vk::Buffer* buffers, const vk::DeviceSize* offsets,
                                         const vk::DeviceSize* sizes,
                                         const vk::DeviceSize* strides) const {
    scheduler->RawCommandBuffer().bindVertexBuffers2(first_binding, binding_count, buffers, offsets,
                                                     sizes, strides);
}

void CommandRecorder::bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset,
                                      vk::IndexType index_type) const {
    scheduler->RawCommandBuffer().bindIndexBuffer(buffer, offset, index_type);
}

void CommandRecorder::bindIndexBuffer2KHR(vk::Buffer buffer, vk::DeviceSize offset,
                                          vk::DeviceSize size, vk::IndexType index_type) const {
    scheduler->RawCommandBuffer().bindIndexBuffer2KHR(buffer, offset, size, index_type);
}

void CommandRecorder::setVertexInputEXT(
    vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> const& binding_descriptions,
    vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> const& attribute_descriptions)
    const {
    scheduler->RawCommandBuffer().setVertexInputEXT(binding_descriptions, attribute_descriptions);
}

void CommandRecorder::setViewport(u32 first_viewport,
                                  vk::ArrayProxy<const vk::Viewport> const& viewports) const {
    scheduler->RawCommandBuffer().setViewport(first_viewport, viewports);
}

void CommandRecorder::setScissor(u32 first_scissor,
                                 vk::ArrayProxy<const vk::Rect2D> const& scissors) const {
    scheduler->RawCommandBuffer().setScissor(first_scissor, scissors);
}

void CommandRecorder::setViewportWithCount(
    vk::ArrayProxy<const vk::Viewport> const& viewports) const {
    scheduler->RawCommandBuffer().setViewportWithCount(viewports);
}

void CommandRecorder::setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> const& scissors) const {
    scheduler->RawCommandBuffer().setScissorWithCount(scissors);
}

void CommandRecorder::setDepthTestEnable(vk::Bool32 depth_test_enable) const {
    scheduler->RawCommandBuffer().setDepthTestEnable(depth_test_enable);
}

void CommandRecorder::setDepthWriteEnable(vk::Bool32 depth_write_enable) const {
    scheduler->RawCommandBuffer().setDepthWriteEnable(depth_write_enable);
}

void CommandRecorder::setDepthCompareOp(vk::CompareOp depth_compare_op) const {
    scheduler->RawCommandBuffer().setDepthCompareOp(depth_compare_op);
}

void CommandRecorder::setDepthBoundsTestEnable(vk::Bool32 depth_bounds_test_enable) const {
    scheduler->RawCommandBuffer().setDepthBoundsTestEnable(depth_bounds_test_enable);
}

void CommandRecorder::setDepthBounds(float min_depth_bounds, float max_depth_bounds) const {
    scheduler->RawCommandBuffer().setDepthBounds(min_depth_bounds, max_depth_bounds);
}

void CommandRecorder::setDepthBiasEnable(vk::Bool32 depth_bias_enable) const {
    scheduler->RawCommandBuffer().setDepthBiasEnable(depth_bias_enable);
}

void CommandRecorder::setDepthBias(float depth_bias_constant_factor, float depth_bias_clamp,
                                   float depth_bias_slope_factor) const {
    scheduler->RawCommandBuffer().setDepthBias(depth_bias_constant_factor, depth_bias_clamp,
                                               depth_bias_slope_factor);
}

void CommandRecorder::setStencilTestEnable(vk::Bool32 stencil_test_enable) const {
    scheduler->RawCommandBuffer().setStencilTestEnable(stencil_test_enable);
}

void CommandRecorder::setStencilOp(vk::StencilFaceFlags face_mask, vk::StencilOp fail_op,
                                   vk::StencilOp pass_op, vk::StencilOp depth_fail_op,
                                   vk::CompareOp compare_op) const {
    scheduler->RawCommandBuffer().setStencilOp(face_mask, fail_op, pass_op, depth_fail_op,
                                               compare_op);
}

void CommandRecorder::setStencilReference(vk::StencilFaceFlags face_mask, u32 reference) const {
    scheduler->RawCommandBuffer().setStencilReference(face_mask, reference);
}

void CommandRecorder::setStencilWriteMask(vk::StencilFaceFlags face_mask, u32 write_mask) const {
    scheduler->RawCommandBuffer().setStencilWriteMask(face_mask, write_mask);
}

void CommandRecorder::setStencilCompareMask(vk::StencilFaceFlags face_mask,
                                            u32 compare_mask) const {
    scheduler->RawCommandBuffer().setStencilCompareMask(face_mask, compare_mask);
}

void CommandRecorder::setPrimitiveRestartEnable(vk::Bool32 primitive_restart_enable) const {
    scheduler->RawCommandBuffer().setPrimitiveRestartEnable(primitive_restart_enable);
}

void CommandRecorder::setRasterizerDiscardEnable(vk::Bool32 rasterizer_discard_enable) const {
    scheduler->RawCommandBuffer().setRasterizerDiscardEnable(rasterizer_discard_enable);
}

void CommandRecorder::setCullMode(vk::CullModeFlags cull_mode) const {
    scheduler->RawCommandBuffer().setCullMode(cull_mode);
}

void CommandRecorder::setFrontFace(vk::FrontFace front_face) const {
    scheduler->RawCommandBuffer().setFrontFace(front_face);
}

void CommandRecorder::setBlendConstants(const float blend_constants[4]) const {
    scheduler->RawCommandBuffer().setBlendConstants(blend_constants);
}

void CommandRecorder::setColorWriteMaskEXT(
    u32 first_attachment,
    vk::ArrayProxy<const vk::ColorComponentFlags> const& color_write_masks) const {
    scheduler->RawCommandBuffer().setColorWriteMaskEXT(first_attachment, color_write_masks);
}

void CommandRecorder::setLineWidth(float line_width) const {
    scheduler->RawCommandBuffer().setLineWidth(line_width);
}

void CommandRecorder::setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags aspect_mask) const {
    scheduler->RawCommandBuffer().setAttachmentFeedbackLoopEnableEXT(aspect_mask);
}

void CommandRecorder::beginRendering(const vk::RenderingInfo& rendering_info) const {
    scheduler->RawCommandBuffer().beginRendering(rendering_info);
}

void CommandRecorder::endRendering() const {
    scheduler->RawCommandBuffer().endRendering();
}

void CommandRecorder::draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                           u32 first_instance) const {
    scheduler->RawCommandBuffer().draw(vertex_count, instance_count, first_vertex, first_instance);
}

void CommandRecorder::drawIndexed(u32 index_count, u32 instance_count, u32 first_index,
                                  s32 vertex_offset, u32 first_instance) const {
    scheduler->RawCommandBuffer().drawIndexed(index_count, instance_count, first_index,
                                              vertex_offset, first_instance);
}

void CommandRecorder::drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                                   u32 stride) const {
    scheduler->RawCommandBuffer().drawIndirect(buffer, offset, draw_count, stride);
}

void CommandRecorder::drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                                          u32 stride) const {
    scheduler->RawCommandBuffer().drawIndexedIndirect(buffer, offset, draw_count, stride);
}

void CommandRecorder::drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset,
                                        vk::Buffer count_buffer, vk::DeviceSize count_buffer_offset,
                                        u32 max_draw_count, u32 stride) const {
    scheduler->RawCommandBuffer().drawIndirectCount(buffer, offset, count_buffer,
                                                    count_buffer_offset, max_draw_count, stride);
}

void CommandRecorder::drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset,
                                               vk::Buffer count_buffer,
                                               vk::DeviceSize count_buffer_offset,
                                               u32 max_draw_count, u32 stride) const {
    scheduler->RawCommandBuffer().drawIndexedIndirectCount(
        buffer, offset, count_buffer, count_buffer_offset, max_draw_count, stride);
}

void CommandRecorder::dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) const {
    scheduler->RawCommandBuffer().dispatch(group_count_x, group_count_y, group_count_z);
}

void CommandRecorder::dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const {
    scheduler->RawCommandBuffer().dispatchIndirect(buffer, offset);
}

void CommandRecorder::copyBuffer(vk::Buffer src_buffer, vk::Buffer dst_buffer,
                                 vk::ArrayProxy<const vk::BufferCopy> const& regions) const {
    scheduler->RawCommandBuffer().copyBuffer(src_buffer, dst_buffer, regions);
}

void CommandRecorder::copyImage(vk::Image src_image, vk::ImageLayout src_image_layout,
                                vk::Image dst_image, vk::ImageLayout dst_image_layout,
                                vk::ArrayProxy<const vk::ImageCopy> const& regions) const {
    scheduler->RawCommandBuffer().copyImage(src_image, src_image_layout, dst_image,
                                            dst_image_layout, regions);
}

void CommandRecorder::copyBufferToImage(
    vk::Buffer src_buffer, vk::Image dst_image, vk::ImageLayout dst_image_layout,
    vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
    scheduler->RawCommandBuffer().copyBufferToImage(src_buffer, dst_image, dst_image_layout,
                                                    regions);
}

void CommandRecorder::copyImageToBuffer(
    vk::Image src_image, vk::ImageLayout src_image_layout, vk::Buffer dst_buffer,
    vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
    scheduler->RawCommandBuffer().copyImageToBuffer(src_image, src_image_layout, dst_buffer,
                                                    regions);
}

void CommandRecorder::resolveImage(vk::Image src_image, vk::ImageLayout src_image_layout,
                                   vk::Image dst_image, vk::ImageLayout dst_image_layout,
                                   vk::ArrayProxy<const vk::ImageResolve> const& regions) const {
    scheduler->RawCommandBuffer().resolveImage(src_image, src_image_layout, dst_image,
                                               dst_image_layout, regions);
}

void CommandRecorder::fillBuffer(vk::Buffer dst_buffer, vk::DeviceSize dst_offset,
                                 vk::DeviceSize size, u32 data) const {
    scheduler->RawCommandBuffer().fillBuffer(dst_buffer, dst_offset, size, data);
}

void CommandRecorder::updateBuffer(vk::Buffer dst_buffer, vk::DeviceSize dst_offset,
                                   vk::DeviceSize data_size, const void* data) const {
    scheduler->RawCommandBuffer().updateBuffer(dst_buffer, dst_offset, data_size, data);
}

void CommandRecorder::clearColorImage(
    vk::Image image, vk::ImageLayout image_layout, const vk::ClearColorValue& color,
    vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const {
    scheduler->RawCommandBuffer().clearColorImage(image, image_layout, color, ranges);
}

void CommandRecorder::beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label_info) const {
    scheduler->RawCommandBuffer().beginDebugUtilsLabelEXT(label_info);
}

void CommandRecorder::insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label_info) const {
    scheduler->RawCommandBuffer().insertDebugUtilsLabelEXT(label_info);
}

void CommandRecorder::endDebugUtilsLabelEXT() const {
    scheduler->RawCommandBuffer().endDebugUtilsLabelEXT();
}

} // namespace Vulkan
