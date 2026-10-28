// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ffx_vk_fsr4_v07.h>
#include <ffx_vk_fsr4_v07_assets.h>
#include <ffx_vk_fsr4_v07_schedule.h>

#include "video_core/gr2_fsr3/fsr4_v07_bridge.h"

_Static_assert(FFX_FSR4_VK_PASS_COUNT == 16, "The backend takes 14 network passes, RCAS and SPD");
_Static_assert(FFX_FSR4_MODEL_PASS_COUNT + 2u == GR2_FSR4_SHADER_COUNT,
               "The network is a pre-pass, the model passes and a post-pass");
_Static_assert(GR2_FSR4_SHADER_COUNT + 2u == GR2_FSR4_FILE_COUNT,
               "A model set is its shaders, the initializer and the pre-pass weights");
_Static_assert(FFX_FSR4_V07_INITIALIZER_BYTES == GR2_FSR4_INITIALIZER_BYTES,
               "Initializer size of the provider");
_Static_assert(FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES == GR2_FSR4_PRE_WEIGHTS_BYTES,
               "Pre-pass weights size of the provider");
_Static_assert(FFX_FSR4_V07_ASSET_PATH_MAX == GR2_FSR4_NAME_MAX, "File name size of the provider");
_Static_assert(FFX_FSR4_MODEL_PRESET_DRS == GR2_FSR4_PRESET_DRS, "Preset values of the provider");
_Static_assert(FFX_API_RETURN_ERROR_PARAMETER == GR2_FSR4_API_ERROR_PARAMETER,
               "Result codes of the provider");

struct Gr2Fsr4Context {
    FfxInterface iface; // The backend; iface.device points into scratch.
    ffxContext provider;
    void* scratch;
    uint64_t memory_bytes;
    // The backend keeps the entry point pointers of the shaders it was created with.
    char entry_points[GR2_FSR4_SHADER_COUNT][GR2_FSR4_ENTRY_POINT_MAX];
};

static Gr2Fsr4Status MakeStatus(uint32_t stage, int32_t vk_result, uint32_t api_result) {
    const Gr2Fsr4Status status = {stage, vk_result, api_result};
    return status;
}

static FfxApiResource Resource(const Gr2Fsr4Image* image, uint32_t format, uint32_t state) {
    FfxApiResource resource;
    memset(&resource, 0, sizeof(resource));
    resource.resource = (void*)image->view;
    resource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = format;
    resource.description.width = image->width;
    resource.description.height = image->height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.state = state;
    return resource;
}

// The image stays in VK_IMAGE_LAYOUT_GENERAL. `access` is the last compute access before the
// dispatch; the dispatch leaves the image readable by compute shaders.
static VkResult SetImageState(Gr2Fsr4Context* context, const Gr2Fsr4Image* image,
                              VkAccessFlags access) {
    FfxFsr4VkExternalImageState state;
    memset(&state, 0, sizeof(state));
    state.structSize = sizeof(state);
    state.image = image->image;
    state.view = image->view;
    state.layout = VK_IMAGE_LAYOUT_GENERAL;
    state.stageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    state.accessMask = access;
    state.restoreLayout = VK_IMAGE_LAYOUT_GENERAL;
    state.restoreStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    state.restoreAccessMask = VK_ACCESS_SHADER_READ_BIT;
    return ffxFsr4VkSetExternalImageState(&context->iface, &state);
}

const char* Gr2Fsr4PresetName(uint32_t preset) {
    return ffxFsr4ModelPresetName((FfxFsr4ModelPreset)preset);
}

const char* Gr2Fsr4TierName(uint32_t output_width, uint32_t output_height) {
    return ffxFsr4V07SelectAssetTier(output_width, output_height);
}

bool Gr2Fsr4GetAssetNames(uint32_t preset, uint32_t output_width, uint32_t output_height,
                          Gr2Fsr4AssetNames* names) {
    FfxFsr4V07AssetSet set;
    if (!ffxFsr4V07BuildAssetSet((FfxFsr4ModelPreset)preset, output_width, output_height, &set)) {
        return false;
    }
    memset(names, 0, sizeof(*names));
    memcpy(names->tier, set.tier, sizeof(set.tier));
    memcpy(names->files[0], set.pre, sizeof(set.pre));
    for (uint32_t pass = 0; pass < FFX_FSR4_MODEL_PASS_COUNT; ++pass) {
        memcpy(names->files[pass + 1u], set.model[pass], sizeof(set.model[pass]));
    }
    memcpy(names->files[13], set.post, sizeof(set.post));
    memcpy(names->files[14], set.initializer, sizeof(set.initializer));
    memcpy(names->files[15], set.prePassWeights, sizeof(set.prePassWeights));
    return true;
}

uint64_t Gr2Fsr4ScratchBytes(uint32_t output_width, uint32_t output_height) {
    return ffxFsr4GetDot4ScratchSize(output_width, output_height);
}

int32_t Gr2Fsr4ValidateShader(const Gr2Fsr4Shader* shader, uint32_t pass) {
    const FfxFsr4VkShaderBlob blob = {shader->words, shader->size_bytes, shader->entry_point};
    return ffxFsr4VkValidateShaderLayout(&blob, pass);
}

Gr2Fsr4Status Gr2Fsr4Create(const Gr2Fsr4CreateInfo* info, Gr2Fsr4Context** context) {
    *context = NULL;
    const size_t scratch_size = ffxFsr4VkGetScratchMemorySize();
    Gr2Fsr4Context* ctx = (Gr2Fsr4Context*)calloc(1, sizeof(*ctx));
    void* scratch = ctx ? calloc(1, scratch_size) : NULL;
    if (!scratch) {
        free(ctx);
        return MakeStatus(GR2_FSR4_STAGE_HOST_MEMORY, 0, 0);
    }
    ctx->scratch = scratch;

    FfxFsr4VkCreateInfo create;
    memset(&create, 0, sizeof(create));
    create.device = info->device;
    create.physicalDevice = info->physical_device;
    // RCAS and SPD stay absent: the module sharpens in its store pass and auto exposure is off.
    for (uint32_t pass = 0; pass < GR2_FSR4_SHADER_COUNT; ++pass) {
        snprintf(ctx->entry_points[pass], GR2_FSR4_ENTRY_POINT_MAX, "%s",
                 info->shaders[pass].entry_point);
        create.shaders[pass].spirv = info->shaders[pass].words;
        create.shaders[pass].sizeBytes = info->shaders[pass].size_bytes;
        create.shaders[pass].entryPoint = ctx->entry_points[pass];
    }
    create.modelInitializer = info->initializer;
    create.modelInitializerSize = info->initializer_size;
    create.prePassWeights = info->pre_weights;
    create.prePassWeightsSize = info->pre_weights_size;
    create.scratchBuffer = scratch;
    create.scratchBufferSize = scratch_size;
    const VkResult backend = ffxFsr4VkCreateContext(&create, &ctx->iface);
    if (backend != VK_SUCCESS) {
        free(scratch);
        free(ctx);
        return MakeStatus(GR2_FSR4_STAGE_BACKEND, (int32_t)backend, 0);
    }

    ffxCreateContextDescUpscale desc;
    memset(&desc, 0, sizeof(desc));
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.maxRenderSize.width = info->max_render_width;
    desc.maxRenderSize.height = info->max_render_height;
    desc.maxUpscaleSize.width = info->max_output_width;
    desc.maxUpscaleSize.height = info->max_output_height;
    // The provider copies the interface during creation.
    ffxFsr4V07SetBackendInterface(&ctx->iface);
    const ffxReturnCode_t provider = ffxFsr4V07CreateContext(&ctx->provider, &desc.header, NULL);
    ffxFsr4V07SetBackendInterface(NULL);
    if (provider != FFX_API_RETURN_OK) {
        ffxFsr4VkDestroyContext((FfxFsr4VkContext*)ctx->iface.device);
        free(scratch);
        free(ctx);
        return MakeStatus(GR2_FSR4_STAGE_PROVIDER, 0, provider);
    }

    FfxApiEffectMemoryUsage usage;
    memset(&usage, 0, sizeof(usage));
    ctx->iface.fpGetEffectGpuMemoryUsage(&ctx->iface, 0, &usage);
    ctx->memory_bytes = usage.totalUsageInBytes;
    *context = ctx;
    return MakeStatus(GR2_FSR4_STAGE_NONE, 0, 0);
}

uint64_t Gr2Fsr4MemoryBytes(const Gr2Fsr4Context* context) {
    return context->memory_bytes;
}

Gr2Fsr4Status Gr2Fsr4Dispatch(Gr2Fsr4Context* context, const Gr2Fsr4DispatchInfo* info) {
    VkResult result = ffxFsr4VkBeginFrame(&context->iface, info->frame_id);
    if (result != VK_SUCCESS) {
        return MakeStatus(GR2_FSR4_STAGE_BEGIN_FRAME, (int32_t)result, 0);
    }
    const struct {
        const Gr2Fsr4Image* image;
        VkAccessFlags access;
    } images[] = {
        {&info->color, VK_ACCESS_SHADER_WRITE_BIT},
        {&info->depth, VK_ACCESS_SHADER_WRITE_BIT},
        {&info->motion_vectors, VK_ACCESS_SHADER_WRITE_BIT},
        {&info->output, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT},
    };
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); ++i) {
        result = SetImageState(context, images[i].image, images[i].access);
        if (result != VK_SUCCESS) {
            return MakeStatus(GR2_FSR4_STAGE_IMAGE_STATE, (int32_t)result, 0);
        }
    }

    // Sharpening, auto exposure and every flag stay off; the provider reads none of the masks.
    ffxDispatchDescUpscale desc;
    memset(&desc, 0, sizeof(desc));
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = (void*)info->command_buffer;
    desc.color = Resource(&info->color, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                          FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.depth =
        Resource(&info->depth, FFX_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.motionVectors = Resource(&info->motion_vectors, FFX_SURFACE_FORMAT_R16G16_FLOAT,
                                  FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.output = Resource(&info->output, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                           FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.jitterOffset.x = info->jitter_x;
    desc.jitterOffset.y = info->jitter_y;
    desc.motionVectorScale.x = info->motion_vector_scale_x;
    desc.motionVectorScale.y = info->motion_vector_scale_y;
    desc.renderSize.width = info->render_width;
    desc.renderSize.height = info->render_height;
    desc.upscaleSize.width = info->output_width;
    desc.upscaleSize.height = info->output_height;
    desc.preExposure = 1.0f;
    desc.reset = info->reset != 0u;
    const ffxReturnCode_t code = ffxFsr4V07Dispatch(&context->provider, &desc.header);
    return code == FFX_API_RETURN_OK ? MakeStatus(GR2_FSR4_STAGE_NONE, 0, 0)
                                     : MakeStatus(GR2_FSR4_STAGE_DISPATCH, 0, code);
}

int32_t Gr2Fsr4Retire(Gr2Fsr4Context* context, uint64_t completed_frame_id) {
    return ffxFsr4VkRetireFrame(&context->iface, completed_frame_id);
}

void Gr2Fsr4Destroy(Gr2Fsr4Context* context) {
    ffxFsr4V07DestroyContext(&context->provider, NULL);
    ffxFsr4VkDestroyContext((FfxFsr4VkContext*)context->iface.device);
    free(context->scratch);
    free(context);
}
