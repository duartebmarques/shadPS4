// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include "common/types.h"
#include "video_core/gr2_fsr3/fsr3_context.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace Gr2Fsr3 {

/// Inputs of the mask pass. The values are fractions in [0, 1].
struct MaskParams {
    vk::ImageView stencil{};
    vk::Extent2D render_size{};
    float reactive_characters{};
    float reactive_effects{};
    float reactive_outline{};
    float tc_characters{};
    bool character_history{}; ///< Pixels the characters covered in the previous frame count too.
};

/// Inputs of the camera-motion pass, which gives stencil 0x20 pixels the motion of the camera.
struct CameraMotionParams {
    vk::ImageView motion_vectors{};
    vk::ImageView depth{};
    vk::ImageView stencil{};
    vk::Extent2D render_size{};
    std::array<float, 16> matrix{};      ///< Velocity constant buffer dwords 0-15.
    std::array<float, 4> inverse_view{}; ///< Velocity constant buffer dwords 16-19.
};

/// Inputs of the store pass, which writes the FSR output into the guest history image.
struct StoreParams {
    vk::ImageView destination{};
    vk::Extent2D output_size{};
    float rcas_stops{-1.0f}; ///< RCAS strength in stops, 0 being the strongest; negative skips it.
    bool linear_input{};     ///< FSR read linear colour, so the store pass encodes to sRGB.
};

/// Inputs of the fallback pass, which scales the colour input into the guest history image.
struct FallbackParams {
    vk::ImageView source{};
    vk::Extent2D source_size{};
    vk::ImageView destination{};
    vk::Extent2D destination_size{};
    bool easu{};
};

/// Inputs of the debug pass. A view the selected view does not read may be null.
struct DebugParams {
    DebugView view{DebugView::Off};
    vk::ImageView color{};
    vk::ImageView motion_vectors{};
    vk::ImageView stencil{};
    vk::ImageView depth_copy{};
    vk::ImageView depth_main{};
    vk::ImageView destination{};
    vk::Extent2D source_size{};
    vk::Extent2D destination_size{};
    CameraParams camera{};
    float motion_vector_scale_x{};
    float motion_vector_scale_y{};
};

/**
 * The host compute passes around the FSR dispatch and the host images they own. Every Record
 * function records into the command buffer it is given, outside dynamic rendering, and leaves the
 * guest images it writes in VK_IMAGE_LAYOUT_GENERAL. GPU command thread only.
 */
class Passes {
public:
    explicit Passes(const Vulkan::Instance& instance);
    ~Passes();

    Passes(const Passes&) = delete;
    Passes& operator=(const Passes&) = delete;

    /// Builds the descriptor set layouts, pipelines and sampler. Returns false on any failure.
    bool Init();

    /// Sizes the host images. An image whose size changes is destroyed once the GPU passes the
    /// current tick.
    void Resize(Vulkan::Scheduler& scheduler, vk::Extent2D render_size, vk::Extent2D output_size,
                bool camera_motion, bool debug_history);

    /// The previous-frame character mask and the reprojection history start empty again.
    void ResetHistory();

    void RecordMask(vk::CommandBuffer cmdbuf, const MaskParams& params);
    void RecordCameraMotion(vk::CommandBuffer cmdbuf, const CameraMotionParams& params);

    /// Makes the mask and camera-motion writes of the frame visible to FSR and readies the output.
    void PrepareFsr(vk::CommandBuffer cmdbuf);

    void RecordStore(vk::CommandBuffer cmdbuf, const StoreParams& params);
    void RecordFallback(vk::CommandBuffer cmdbuf, const FallbackParams& params);
    void RecordDebug(vk::CommandBuffer cmdbuf, const DebugParams& params);

    FsrImage ReactiveMask() const;
    FsrImage TransparencyMask() const;
    FsrImage CameraMotionVectors() const;
    FsrImage Output() const;

    /// Device memory of the host images for these sizes.
    static u64 EstimateBytes(vk::Extent2D render_size, vk::Extent2D output_size,
                             bool camera_motion);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gr2Fsr3
