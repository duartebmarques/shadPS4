// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

#include "common/types.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/gr2_fsr3/fsr3_inputs.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class GraphicsPipeline;
class Rasterizer;
} // namespace Vulkan

namespace Gr2Fsr3 {

/// Program hashes (Shader::Info::pgm_hash) of the Gravity Rush Remastered passes the module uses.
namespace GrrHash {
constexpr u64 ExposureCs = 0x275e3114; ///< First GPU work of every 3D frame.
constexpr u64 CameraVs = 0xdbb08a76;   ///< GfxMaterial_G_BG_Main_vso, constant buffer 0 = camera.
constexpr u64 CompositeVs = 0x4dccac15;
/// The GfxRenderPassComposite pixel shaders, one per Composite variant.
constexpr std::array<u64, 32> CompositePs{
    0x10b60698, 0x1784ed3b, 0x1955dfe9, 0x22251b56, 0x2f887d9f, 0x32f2d5eb, 0x3610ea85, 0x3e7be685,
    0x40de8a89, 0x414383c9, 0x4146e510, 0x4b793409, 0x52ff72de, 0x7246d172, 0x74456ebb, 0x7e6b1092,
    0x869c2c38, 0x885e858a, 0x9a4b146c, 0xb041b1f2, 0xb663a197, 0xb732a9ff, 0xba16c10f, 0xd179c883,
    0xdba6dd74, 0xe542cedb, 0xe775c523, 0xea49c496, 0xebe95e8d, 0xedb2ffe3, 0xefbcc99a, 0xf2a0fefe,
};
constexpr u64 CopyLumaPs = 0x8c2537d0;
constexpr u64 FxaaPs = 0x634c42fb;
constexpr u64 MotionBlurPs = 0x2f948ea9;
} // namespace GrrHash

/// Stencil bit the game writes for the character categories.
constexpr u32 GrrCharacterStencil = 0x80;

/// Distinct keys, at most N. Insert returns true for a key that is new while there is room; a
/// full set takes no more keys and stops searching.
template <typename T, std::size_t N>
class BoundedSet {
public:
    bool Insert(const T& key) {
        // A predicate search: the MSVC STL fails to compile ranges::contains for some struct keys.
        const auto same = [&key](const T& held) { return held == key; };
        if (size == N || std::ranges::any_of(Keys(), same)) {
            return false;
        }
        keys[size++] = key;
        return true;
    }

    std::span<const T> Keys() const {
        return {keys.data(), size};
    }

private:
    std::array<T, N> keys{};
    std::size_t size{};
};

/// Row-major 4x4 matrix for row vectors: a point maps as p * M.
using GrrMatrix = std::array<std::array<double, 4>, 4>;

/// Viewport 0 of a draw with its enable bits applied: window = offset + scale * NDC.
struct GrrViewport {
    float x_scale{};
    float x_offset{};
    float y_scale{};
    float y_offset{};
    float z_scale{};
    float z_offset{};
    vk::Extent2D size{};
};

/// The camera of a frame, from constant buffer 0 of its first camera draw.
struct GrrCamera {
    GrrMatrix view{};
    GrrMatrix view_proj{};
    GrrMatrix projection{};           ///< view^-1 * view_proj.
    std::array<double, 3> position{}; ///< World position of the camera.
    GrrViewport viewport{};
    CameraParams params{};
    bool valid{};
};

/// How far the camera moved between two frames.
struct GrrCameraDelta {
    double translation{};
    double rotation_degrees{};
    double fov_delta{};
    bool cut{}; ///< Further than a camera cut threshold.
};

/// Columns x, y and w of the matrices that take an unjittered render pixel of the current frame,
/// with its depth, to the previous frame's render pixel times w. The sky columns leave out the
/// camera translation.
struct GrrReprojection {
    std::array<std::array<float, 4>, 3> scene{};
    std::array<std::array<float, 4>, 3> sky{};
};

/// The scene depth-stencil image of a frame, latched at its camera draw.
struct GrrSceneDepth {
    AmdGpu::DepthBuffer buffer{};
    AmdGpu::DepthView view{};
    AmdGpu::DepthControl control{};
    VAddr htile_address{};
    AmdGpu::CbDbExtent hint{};
    vk::Extent2D extent{}; ///< Viewport size of the camera draw.
    bool valid{};
};

/// The bindings of an FXAA draw (pixel shader 0x634c42fb).
struct GrrFxaaDraw {
    ImageBinding color;           ///< t0, written by CopyLuma or the Composite.
    AmdGpu::ColorBuffer target{}; ///< The back buffer, or the 2D screen capture of menus.
    AmdGpu::CbDbExtent target_hint{};
    std::array<u8, 4> components{}; ///< Source component of each target component.
    vk::Extent2D render_size{};     ///< Extent of t0.
    vk::Extent2D output_size{};     ///< Viewport size.
};

/// Images of one FXAA replacement. All are in VK_IMAGE_LAYOUT_GENERAL once the runtime barriers
/// are flushed.
struct GrrImages {
    GuestImage color;   ///< Sampled view of t0 in its T# format.
    GuestImage depth;   ///< The scene depth-stencil image; depth-aspect view.
    GuestImage stencil; ///< The same image; stencil-aspect view, null without a stencil plane.
    GuestImage output;  ///< R8G8B8A8_UNORM storage view of the target.
};

/// Program hashes of the stages of a draw, 0 for an absent stage.
struct GrrDrawHashes {
    u64 vs{};
    u64 gs{};
    u64 ps{};
};

/// Program hashes of the vertex, geometry and pixel stages of a pipeline.
GrrDrawHashes HashesOf(const Vulkan::GraphicsPipeline& pipeline);

/// Whether a draw is a Composite draw: its pixel shader is a Composite variant, or its vertex
/// shader is the Composite one. A patch of the vertex shader's texel offsets changes its hash.
bool IsComposite(const GrrDrawHashes& hashes);

/// Compares a camera with the one of the previous frame.
GrrCameraDelta CompareCameras(const GrrCamera& current, const GrrCamera& previous);

/// The reprojection from the current to the previous camera, both seen through the viewport of
/// the current one. Empty when a matrix is singular or a value is not finite.
std::optional<GrrReprojection> Reprojection(const GrrCamera& current, const GrrCamera& previous);

/// Follows a Gravity Rush Remastered frame up to its FXAA draw and resolves the images of that
/// draw. GPU command thread only.
class GrrCapture {
public:
    GrrCapture();
    ~GrrCapture();

    GrrCapture(const GrrCapture&) = delete;
    GrrCapture& operator=(const GrrCapture&) = delete;

    /// Forgets the camera, the scene depth and the CopyLuma and Composite targets of the frame. The
    /// scene depth address stays for the jitter of the next frame.
    void StartFrame();

    /// Latches the camera and the scene depth at the first qualifying camera draw of the frame,
    /// and the targets of the CopyLuma and Composite draws.
    void OnDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                Vulkan::Rasterizer& rasterizer, const GrrDrawHashes& hashes);

    /// Copies the bindings of an FXAA draw. Returns false, logging once per cause, unless they are
    /// one RGBA8 sRGB input and an unblended RGBA8 sRGB target that the viewport fills, with a
    /// scissor around the viewport and no depth or stencil test.
    bool IdentifyFxaa(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                      GrrFxaaDraw& draw);

    /// Whether a CopyLuma draw of the frame wrote the non-zero address.
    bool CopyLumaWrote(VAddr address) const;

    /// Whether the last Composite draw of the frame wrote the non-zero address.
    bool CompositeWrote(VAddr address) const;

    const GrrCamera& Camera() const;
    const GrrSceneDepth& SceneDepth() const;

    /// Depth address of the last scene depth, kept across frames; 0 before the first.
    VAddr SceneDepthAddress() const;

    /**
     * Resolves the images of the FXAA draw through the texture cache and queues their transitions
     * to VK_IMAGE_LAYOUT_GENERAL for compute access on the runtime, with the bookkeeping of a
     * render target for the target. The caller ends rendering before and flushes the runtime
     * barriers after. Returns false, logging once per cause, when an image cannot be used.
     */
    bool Acquire(const GrrFxaaDraw& draw, const GrrSceneDepth& scene,
                 Vulkan::Rasterizer& rasterizer, GrrImages& images);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gr2Fsr3
