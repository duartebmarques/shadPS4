// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include "common/types.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/gr2_fsr3/fsr3_jitter.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace AmdGpu {
struct ComputeProgram;
}

namespace Shader {
struct Info;
}

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {
struct Image;
}

namespace Gr2Fsr3 {

/// An image binding of a guest dispatch, copied out of its Shader::Info.
struct ImageBinding {
    AmdGpu::Image tsharp{};
    Shader::ImageResource resource{};
    bool present{};

    u32 Width() const {
        return static_cast<u32>(tsharp.width) + 1;
    }

    u32 Height() const {
        return static_cast<u32>(tsharp.height) + 1;
    }
};

/// The bindings of the Antialias dispatch (compute shader 0x6f705679).
struct AntialiasBindings {
    ImageBinding color;    ///< t0, the tonemapped scene colour (RGBA8 sRGB, user data).
    ImageBinding history;  ///< t1, the previous history image (RGBA8 sRGB, resource table).
    ImageBinding velocity; ///< t2, RG16F motion vectors.
    ImageBinding stencil;  ///< t3, the R8 UINT stencil plane of the scene depth.
    ImageBinding output;   ///< u0, the current history image.
};

/// The Velocity dispatch (compute shader 0xaafa61e8) of a frame.
struct VelocityCapture {
    ImageBinding depth;       ///< t0, the R32F device-depth copy.
    ImageBinding stencil;     ///< t1
    ImageBinding velocity;    ///< u0
    std::array<u32, 24> cb{}; ///< Constant buffer 0: matrix, u_f4InverseView, u_f4TargetSize.
    bool cb_valid{};
    u64 frame{}; ///< Antialias dispatches handled before the capture.
    bool valid{};
};

/// Camera of the frame, derived from the Velocity constant buffer.
struct CameraState {
    CameraParams params{};
    bool cut{};      ///< The camera moved or turned further than a camera cut threshold.
    bool repeated{}; ///< The constant buffer equals the previous one while the camera moves.
    float translation{};
    float rotation_degrees{};
    float fov_delta{};
};

/// A guest image resolved through the texture cache for one dispatch.
struct GuestImage {
    VideoCore::ImageId id{};
    VideoCore::Image* image{};
    vk::ImageView view{};
    vk::Format format{vk::Format::eUndefined}; ///< Format of the backing VkImage.
    vk::ImageUsageFlags usage{};               ///< Usage of the backing VkImage.
    vk::Extent2D extent{};
};

/// The images Acquire resolves.
struct AcquireRequest {
    bool color{};
    bool velocity{};
    bool stencil{};
    bool depth_copy{};
    bool depth_main{};
    bool output{};
};

/// Images of one Antialias replacement. All are in VK_IMAGE_LAYOUT_GENERAL once the runtime
/// barriers are flushed.
struct FrameImages {
    GuestImage color;      ///< Sampled view in the T# format.
    GuestImage velocity;   ///< Sampled view.
    GuestImage stencil;    ///< The scene depth-stencil image; stencil-aspect view.
    GuestImage depth_copy; ///< Sampled view of the device-depth copy.
    GuestImage depth_main; ///< The scene depth-stencil image; depth-aspect view.
    GuestImage output;     ///< Storage view.
};

/// Summary of one Antialias dispatch for the log.
struct FrameLog {
    u64 frame{};
    Mode mode{Mode::Stock};
    Kind kind{Kind::Native};
    ContextState context{ContextState::Idle};
    ContextState fsr4_context{ContextState::Idle};
    vk::Extent2D render_size{};
    vk::Extent2D output_size{};
    float interval_ms{};
    bool reset{};
    CameraState camera{};
    Jitter::FrameReport jitter{};
};

/// Finds the guest resources of the passes the module reads. GPU command thread only.
class Inputs {
public:
    Inputs();
    ~Inputs();

    Inputs(const Inputs&) = delete;
    Inputs& operator=(const Inputs&) = delete;

    /// Copies the bindings and the constant buffer of a Velocity dispatch.
    void CaptureVelocity(const Shader::Info& info, Vulkan::Rasterizer& rasterizer, u64 frame);

    /// The last Velocity capture.
    const VelocityCapture& Velocity() const;

    /// Sorts the Antialias bindings by format and binding class. Returns false, logging once,
    /// unless they hold one colour, velocity, stencil and RGBA8 output image, at most one history.
    bool IdentifyAntialias(const Shader::Info& info, AntialiasBindings& bindings);

    /// Render size of the frame: u_f4TargetSize of a current capture when it fits the colour
    /// input, else the colour input extent. A disagreement is logged once.
    vk::Extent2D RenderSize(const AntialiasBindings& bindings, bool capture_is_current);

    /**
     * Resolves the requested images through the texture cache and queues their transitions to
     * VK_IMAGE_LAYOUT_GENERAL for compute access on the runtime. The caller ends rendering before
     * and flushes the runtime barriers after. Returns false, logging once per cause, when a
     * requested image cannot be resolved.
     */
    bool Acquire(const AntialiasBindings& bindings, const AcquireRequest& request,
                 Vulkan::Rasterizer& rasterizer, FrameImages& images);

    /// Derives the camera of the frame and compares it with the previous frame. Call once per
    /// Antialias dispatch; without a current capture the last parameters are kept.
    CameraState EvaluateCamera(bool capture_is_current);

    /// Logs every image and buffer binding of a dispatch.
    void LogDispatch(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                     Vulkan::Rasterizer& rasterizer);

    /// Logs the summary of an Antialias dispatch.
    void LogFrame(const FrameLog& log);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gr2Fsr3
