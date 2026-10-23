// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// C interface to the experimental source-v07 FSR 4 INT8/DOT4 Vulkan provider. Only
// fsr4_v07_bridge.c includes the provider headers, whose type names collide with the FidelityFX
// SDK headers of the FSR 3.1 libraries. A C++ includer includes vk_common.h first.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GR2_FSR4_SHADER_COUNT 14u // 0 pre-pass, 1-12 model passes, 13 post-pass
#define GR2_FSR4_FILE_COUNT 16u   // The shaders, the initializer and the pre-pass weights.
#define GR2_FSR4_NAME_MAX 96u
#define GR2_FSR4_ENTRY_POINT_MAX 128u
#define GR2_FSR4_INITIALIZER_BYTES 89216u
#define GR2_FSR4_PRE_WEIGHTS_BYTES 1024u
#define GR2_FSR4_FRAMES_IN_FLIGHT 3u

#define GR2_FSR4_PRESET_NATIVE 0u
#define GR2_FSR4_PRESET_QUALITY 1u
#define GR2_FSR4_PRESET_BALANCED 2u
#define GR2_FSR4_PRESET_PERFORMANCE 3u
#define GR2_FSR4_PRESET_ULTRA_PERFORMANCE 4u
#define GR2_FSR4_PRESET_DRS 5u
#define GR2_FSR4_PRESET_COUNT 6u

// Stage of a failed call.
#define GR2_FSR4_STAGE_NONE 0u
#define GR2_FSR4_STAGE_HOST_MEMORY 1u
#define GR2_FSR4_STAGE_BACKEND 2u  // Backend context: device checks, shaders, ring, pools.
#define GR2_FSR4_STAGE_PROVIDER 3u // Provider context: pipelines, internal resources.
#define GR2_FSR4_STAGE_BEGIN_FRAME 4u
#define GR2_FSR4_STAGE_IMAGE_STATE 5u
#define GR2_FSR4_STAGE_DISPATCH 6u
#define GR2_FSR4_STAGE_COUNT 7u

/// Provider result code of a rejected parameter.
#define GR2_FSR4_API_ERROR_PARAMETER 6u

typedef struct Gr2Fsr4Context Gr2Fsr4Context;

/// File names of one model set: 0-13 the shaders, 14 the initializer, 15 the pre-pass weights.
typedef struct Gr2Fsr4AssetNames {
    char tier[8];
    char files[GR2_FSR4_FILE_COUNT][GR2_FSR4_NAME_MAX];
} Gr2Fsr4AssetNames;

typedef struct Gr2Fsr4Shader {
    const uint32_t* words;
    size_t size_bytes;
    const char* entry_point; ///< At most GR2_FSR4_ENTRY_POINT_MAX - 1 bytes; creation copies it.
} Gr2Fsr4Shader;

typedef struct Gr2Fsr4CreateInfo {
    VkDevice device;
    VkPhysicalDevice physical_device;
    Gr2Fsr4Shader shaders[GR2_FSR4_SHADER_COUNT];
    const void* initializer;
    size_t initializer_size;
    const void* pre_weights;
    size_t pre_weights_size;
    uint32_t max_render_width;
    uint32_t max_render_height;
    uint32_t max_output_width;
    uint32_t max_output_height;
} Gr2Fsr4CreateInfo;

/// A colour image with one mip and one layer, in VK_IMAGE_LAYOUT_GENERAL.
typedef struct Gr2Fsr4Image {
    VkImage image;
    VkImageView view;
    uint32_t width;
    uint32_t height;
} Gr2Fsr4Image;

/**
 * One dispatch. The colour (R16G16B16A16_SFLOAT), depth (R32_SFLOAT) and motion vector
 * (R16G16_SFLOAT) images were last written by a compute shader; the output
 * (R16G16B16A16_SFLOAT) was made writable by a compute barrier. All four are left readable by
 * compute shaders.
 */
typedef struct Gr2Fsr4DispatchInfo {
    VkCommandBuffer command_buffer;
    uint64_t frame_id;
    Gr2Fsr4Image color;
    Gr2Fsr4Image depth;
    Gr2Fsr4Image motion_vectors;
    Gr2Fsr4Image output;
    float jitter_x;
    float jitter_y;
    float motion_vector_scale_x;
    float motion_vector_scale_y;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t reset;
} Gr2Fsr4DispatchInfo;

/// Result of a call. vk_result is the VkResult of a failed backend call and api_result the code of
/// a failed provider call; both are 0 otherwise.
typedef struct Gr2Fsr4Status {
    uint32_t stage; ///< GR2_FSR4_STAGE_*
    int32_t vk_result;
    uint32_t api_result;
} Gr2Fsr4Status;

/// Name of a GR2_FSR4_PRESET_* value, as the file names use it.
const char* Gr2Fsr4PresetName(uint32_t preset);

/// Resolution tier of the model set for an output extent, or NULL above 7680x4320.
const char* Gr2Fsr4TierName(uint32_t output_width, uint32_t output_height);

/// Fills the file names of a model set. Returns false above 7680x4320.
bool Gr2Fsr4GetAssetNames(uint32_t preset, uint32_t output_width, uint32_t output_height,
                          Gr2Fsr4AssetNames* names);

/// Size of the activation storage buffer for an output extent, or 0 above 7680x4320.
uint64_t Gr2Fsr4ScratchBytes(uint32_t output_width, uint32_t output_height);

/// VkResult of checking a shader against the descriptor layout of pass `pass` (0-13).
int32_t Gr2Fsr4ValidateShader(const Gr2Fsr4Shader* shader, uint32_t pass);

/// Creates a context. Thread safe as long as one creation runs at a time.
Gr2Fsr4Status Gr2Fsr4Create(const Gr2Fsr4CreateInfo* info, Gr2Fsr4Context** context);

/// Device memory of the provider's resources.
uint64_t Gr2Fsr4MemoryBytes(const Gr2Fsr4Context* context);

/**
 * Records one dispatch into info->command_buffer, outside dynamic rendering. At most
 * GR2_FSR4_FRAMES_IN_FLIGHT dispatches may be unretired. After a failure the context accepts
 * no further dispatch.
 */
Gr2Fsr4Status Gr2Fsr4Dispatch(Gr2Fsr4Context* context, const Gr2Fsr4DispatchInfo* info);

/// Releases the per-frame resources of every dispatch up to completed_frame_id, whose command
/// buffers the GPU has finished. Returns a VkResult.
int32_t Gr2Fsr4Retire(Gr2Fsr4Context* context, uint64_t completed_frame_id);

/// Destroys a context whose command buffers the GPU has finished.
void Gr2Fsr4Destroy(Gr2Fsr4Context* context);

#ifdef __cplusplus
}
#endif
