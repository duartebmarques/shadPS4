// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "common/types.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Rasterizer;
class Scheduler;
} // namespace Vulkan

namespace Gr2Fsr3 {

/// A colour image with one mip and one layer.
struct Fsr4Image {
    vk::Image image{};
    vk::ImageView view{};
};

/// The images of an FSR 4 context, registered with the provider once: the inputs at the render
/// size, the output at the output size.
struct Fsr4Images {
    Fsr4Image color;          ///< R16G16B16A16_SFLOAT
    Fsr4Image motion_vectors; ///< R16G16_SFLOAT
    Fsr4Image depth;          ///< R32_SFLOAT
    Fsr4Image output;         ///< R16G16B16A16_SFLOAT
};

/// Parameters an FSR 4 context is created with.
struct Fsr4Key {
    vk::Extent2D max_render_size{};
    vk::Extent2D max_output_size{};
    u32 preset{}; ///< GR2_FSR4_PRESET_* of the bridge.

    bool operator==(const Fsr4Key&) const = default;
};

/// One FSR 4 dispatch; the inputs are in the images of the context.
struct Fsr4Dispatch {
    float jitter_x{};
    float jitter_y{};
    float motion_vector_scale_x{};
    float motion_vector_scale_y{};
    vk::Extent2D render_size{};
    vk::Extent2D output_size{};
    bool reset{};
};

/// The key for the session sizes and the fsr4Model choice.
Fsr4Key Fsr4KeyFor(vk::Extent2D render_size, vk::Extent2D output_size, Fsr4Model model);

/// Name of a preset.
const char* Fsr4PresetName(u32 preset);

/// Resolution tier of the model set for an output extent, empty above 7680x4320.
const char* Fsr4TierName(vk::Extent2D output_size);

/**
 * Owns the experimental FSR 4 v07 INT8 upscaler context. Files are read and contexts are created
 * and destroyed on a worker thread, one task at a time; a context the GPU may still use is
 * destroyed only after the GPU passes the tick that was current when it was retired.
 */
class Fsr4Context {
public:
    explicit Fsr4Context(const Vulkan::Instance& instance);
    ~Fsr4Context();

    Fsr4Context(const Fsr4Context&) = delete;
    Fsr4Context& operator=(const Fsr4Context&) = delete;

    /**
     * GPU command thread. Moves towards a ready context for `wanted`, or towards none when it is
     * empty: reads the files, creates the context once `may_allocate` holds and the budget check
     * passes, takes a finished creation and retires a context that is not wanted. `extra_bytes`
     * is the memory the caller allocates for the same sizes. Returns Idle when nothing is wanted.
     */
    ContextState Update(Vulkan::Scheduler& scheduler, const std::optional<Fsr4Key>& wanted,
                        bool may_allocate, u64 extra_bytes);

    /// Whether a context exists, is being created, or awaits destruction.
    bool HoldsMemory() const;

    /// Whether the files of the wanted key passed every check and wait for the creation.
    bool AssetsReady() const;

    /// Number of contexts that have become ready.
    u64 Generation() const;

    /// Device memory of the ready context and its images, or 0.
    u64 MemoryBytes() const;

    /// Why FSR 4 is unsupported, failed or waiting while a key is wanted, or an empty string.
    std::string Notice() const;

    /// The images of the ready context.
    const Fsr4Images& Images() const;

    /**
     * GPU command thread, before anything of the frame is recorded: retires the frames the GPU
     * has finished and waits for the oldest when three are in flight, which drops the guest-copy
     * hold and can submit the open command buffer. Returns false, retiring the context, when the
     * provider refuses.
     */
    bool PrepareFrame(Vulkan::Rasterizer& rasterizer);

    /**
     * GPU command thread. Records one dispatch of the ready context into cmdbuf, outside dynamic
     * rendering, after the input images were written. Returns false, retiring the context and
     * keeping its key failed, when FSR 4 rejects the dispatch.
     */
    bool RecordDispatch(Vulkan::Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                        const Fsr4Dispatch& dispatch);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gr2Fsr3
