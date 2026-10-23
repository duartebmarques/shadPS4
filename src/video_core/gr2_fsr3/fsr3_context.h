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
class Scheduler;
} // namespace Vulkan

namespace Gr2Fsr3 {

/// Parameters an FSR context is created with.
struct ContextKey {
    vk::Extent2D max_render_size{};
    vk::Extent2D max_output_size{};
    bool fp16{};
    bool debug_checking{};

    bool operator==(const ContextKey&) const = default;
};

/**
 * An image as FSR receives it, in VK_IMAGE_LAYOUT_GENERAL. Read images are declared
 * FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ and written ones UNORDERED_ACCESS; FSR returns each
 * image in the declared state.
 */
struct FsrImage {
    vk::Image image{};
    vk::Format format{vk::Format::eUndefined};
    vk::Extent2D extent{};
    vk::ImageUsageFlags usage{};
    vk::ImageAspectFlags aspect{vk::ImageAspectFlagBits::eColor};
    bool written{};
};

/// One FSR dispatch. A null reactive or transparency image is passed as absent.
struct FsrDispatch {
    FsrImage color;
    FsrImage depth;
    FsrImage motion_vectors;
    FsrImage reactive;
    FsrImage transparency;
    FsrImage output;
    float jitter_x{};
    float jitter_y{};
    float motion_vector_scale_x{};
    float motion_vector_scale_y{};
    vk::Extent2D render_size{};
    vk::Extent2D output_size{};
    float frame_time_ms{};
    CameraParams camera{};
    bool reset{};
    u64 frame_id{};
};

/**
 * Owns the FidelityFX FSR 3.1 upscaler context. Contexts are created and destroyed on a worker
 * thread, one operation at a time; a context the GPU may still use is destroyed only after the GPU
 * passes the tick that was current when it was retired.
 */
class Context {
public:
    explicit Context(const Vulkan::Instance& instance);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /// Whether the device meets the FSR 3.1 requirements.
    bool IsSupported() const;

    /// Whether the FP16 shader permutations can be used.
    bool IsFp16Supported() const;

    /// Whether the depth aspect of a depth-stencil image can be transitioned on its own.
    bool IsSeparateDepthStencilLayoutsSupported() const;

    /**
     * GPU command thread. Moves towards a ready context for `wanted`, or towards none when it is
     * empty: starts a creation, takes a finished one, and retires a context that is not wanted.
     * `extra_bytes` is the memory the caller allocates for the same sizes, counted by the budget
     * check before a creation starts. Returns the state of the context for `wanted`.
     */
    ContextState Update(Vulkan::Scheduler& scheduler, const std::optional<ContextKey>& wanted,
                        u64 extra_bytes);

    /// Whether a context exists, is being created, or awaits destruction.
    bool HoldsMemory() const;

    /// Number of contexts that have become ready.
    u64 Generation() const;

    /// Device memory of the ready context as FSR reports it, or 0.
    u64 MemoryBytes() const;

    /// The last failure, or an empty string.
    std::string Notice() const;

    /**
     * GPU command thread. Records one dispatch of the ready context into cmdbuf, outside dynamic
     * rendering. Returns false when FSR rejects the dispatch. The validation issues of a rejected
     * dispatch are logged once per distinct set; with `log_issues` every dispatch is validated
     * before it is recorded.
     */
    bool RecordDispatch(vk::CommandBuffer cmdbuf, const FsrDispatch& dispatch, bool log_issues);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Device memory available for new allocations.
u64 AvailableDeviceMemory(const Vulkan::Instance& instance);

/// The render and output sizes as "WxH -> WxH".
std::string FormatSizes(vk::Extent2D render_size, vk::Extent2D output_size);

} // namespace Gr2Fsr3
