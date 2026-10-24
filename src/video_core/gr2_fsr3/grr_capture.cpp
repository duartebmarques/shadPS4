// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/ranges.h>
#include <magic_enum/magic_enum.hpp>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/gr2_fsr3/grr_capture.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Gr2Fsr3 {

namespace {

// Constant buffer 0 of the camera vertex shader holds cb0m4View, cb0m4ViewProj,
// cb0m4ShadowViewProjection, cb0f3ViewPosition and cb0f4ShadowOffsetDir; the first 32 floats are
// the view and view-projection matrices.
constexpr u32 CameraConstantBufferBytes = 224;
constexpr std::size_t CameraFloats = 32;

constexpr double CutTranslation = 20.0;
constexpr double CutRotationDegrees = 45.0;
constexpr double CutFovDelta = 0.35;

constexpr u32 MaxCameraLogs = 16;

// Why a qualifying camera draw gives no camera.
enum class CameraCheck : u8 {
    Valid,
    NoConstantBuffer,
    NotReadable,
    NotFinite,
    NotAffine,
    NotRigid,
    ClipW,
    NotInvertible,
    Projection,
};
constexpr std::array<std::string_view, 9> CameraCheckNames{
    "valid",
    "no 224-byte constant buffer",
    "constant buffer not readable",
    "not finite",
    "view not affine",
    "view not rigid",
    "clip w is not -view z",
    "view not invertible",
    "projection",
};
static_assert(CameraCheckNames.size() == magic_enum::enum_count<CameraCheck>());

// The first check an FXAA draw fails.
enum class FxaaCheck : u8 {
    Recognized,
    InputCount,
    InputFormat,
    InputView,
    InputSwizzle,
    Target,
    Blend,
    ColorWrites,
    Viewport,
    Scissor,
    DepthStencil,
};
constexpr std::array<std::string_view, 11> FxaaCheckNames{
    "recognized",
    "not exactly one input image",
    "the input is not RGBA8 sRGB",
    "the input is not level 0 and layer 0 of a 2D image",
    "the input swizzle does not keep red, green and blue",
    "the target is not one single-sampled RGBA8 sRGB layer",
    "the target is blended or partly write-masked",
    "colour writes are disabled",
    "the viewport does not cover a whole number of pixels from the origin",
    "the scissor does not cover the viewport",
    "the draw is depth or stencil tested",
};
static_assert(FxaaCheckNames.size() == magic_enum::enum_count<FxaaCheck>());

// Why Acquire fails.
enum class Fault : u8 {
    Unmapped,
    Replaced,
    Samples,
    Layers,
    Format,
    Usage,
    Size,
};
constexpr std::array<std::string_view, 7> FaultNames{
    "is not in mapped memory",  "keeps being replaced in the texture cache",
    "is multisampled",          "has more than one layer",
    "has another format",       "has no storage usage",
    "is smaller than the draw",
};
static_assert(FaultNames.size() == magic_enum::enum_count<Fault>());

// What Acquire needs of an image.
struct Requirement {
    vk::Extent2D size{};
    bool rgba8{};   // An R8G8B8A8 sRGB or UNORM backing image.
    bool depth{};   // A depth image.
    bool storage{}; // Storage usage.
};

std::optional<Fault> Check(const VideoCore::Image& image, const Requirement& requirement) {
    const vk::ImageCreateInfo& image_ci = image.backing->image.image_ci;
    if (image.info.num_samples != 1 || image.backing_num_samples != 1) {
        return Fault::Samples;
    }
    if (image_ci.arrayLayers != 1) {
        return Fault::Layers;
    }
    const bool rgba8 = image_ci.format == vk::Format::eR8G8B8A8Srgb ||
                       image_ci.format == vk::Format::eR8G8B8A8Unorm;
    if ((requirement.rgba8 && !rgba8) || (requirement.depth && !image.info.props.is_depth)) {
        return Fault::Format;
    }
    if (requirement.storage && !(image_ci.usage & vk::ImageUsageFlagBits::eStorage)) {
        return Fault::Usage;
    }
    if (image.info.size.width < requirement.size.width ||
        image.info.size.height < requirement.size.height) {
        return Fault::Size;
    }
    return std::nullopt;
}

GrrMatrix Multiply(const GrrMatrix& a, const GrrMatrix& b) {
    GrrMatrix product{};
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            for (u32 k = 0; k < 4; ++k) {
                product[r][c] += a[r][k] * b[k][c];
            }
        }
    }
    return product;
}

// Gauss-Jordan elimination with partial pivoting. Fails when a pivot is below 1e-12 times the
// largest absolute entry.
bool Invert(const GrrMatrix& matrix, GrrMatrix& inverse) {
    std::array<std::array<double, 8>, 4> rows{};
    double largest = 0.0;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            rows[r][c] = matrix[r][c];
            largest = std::max(largest, std::abs(matrix[r][c]));
        }
        rows[r][4 + r] = 1.0;
    }
    for (u32 c = 0; c < 4; ++c) {
        u32 pivot = c;
        for (u32 r = c + 1; r < 4; ++r) {
            if (std::abs(rows[r][c]) > std::abs(rows[pivot][c])) {
                pivot = r;
            }
        }
        // The negated compare also rejects NaN.
        if (!(std::abs(rows[pivot][c]) > 1e-12 * largest)) {
            return false;
        }
        std::swap(rows[c], rows[pivot]);
        const double scale = 1.0 / rows[c][c];
        for (double& value : rows[c]) {
            value *= scale;
        }
        for (u32 r = 0; r < 4; ++r) {
            if (r == c) {
                continue;
            }
            const double factor = rows[r][c];
            for (u32 k = 0; k < 8; ++k) {
                rows[r][k] -= factor * rows[c][k];
            }
        }
    }
    for (u32 r = 0; r < 4; ++r) {
        std::copy_n(rows[r].begin() + 4, 4, inverse[r].begin());
    }
    return true;
}

// Viewport 0 with its enable bits applied. Its size stays empty unless it covers a whole number of
// pixels in each direction from the origin, as the full-target passes of the game set it.
GrrViewport ViewportOf(const AmdGpu::Regs& regs) {
    const auto& bounds = regs.viewports[0];
    const auto& enables = regs.viewport_control;
    GrrViewport viewport{
        .x_scale = enables.xscale_enable ? bounds.xscale : 1.0f,
        .x_offset = enables.xoffset_enable ? bounds.xoffset : 0.0f,
        .y_scale = enables.yscale_enable ? bounds.yscale : 1.0f,
        .y_offset = enables.yoffset_enable ? bounds.yoffset : 0.0f,
        .z_scale = enables.zscale_enable ? bounds.zscale : 1.0f,
        .z_offset = enables.zoffset_enable ? bounds.zoffset : 0.0f,
    };
    const float width = 2.0f * std::abs(viewport.x_scale);
    const float height = 2.0f * std::abs(viewport.y_scale);
    const auto whole = [](float size) {
        return size >= 1.0f && std::abs(size - std::round(size)) <= 0.01f;
    };
    const bool at_origin = std::abs(viewport.x_offset - std::abs(viewport.x_scale)) <= 0.5f &&
                           std::abs(viewport.y_offset - std::abs(viewport.y_scale)) <= 0.5f;
    if (at_origin && whole(width) && whole(height)) {
        viewport.size =
            vk::Extent2D{static_cast<u32>(std::round(width)), static_cast<u32>(std::round(height))};
    }
    return viewport;
}

// The scissor of viewport 0, which Rasterizer::UpdateViewportScissorState intersects from the
// screen, window, generic and viewport scissors.
AmdGpu::Scissor ScissorOf(const AmdGpu::Regs& regs) {
    const bool enable_offset = !regs.window_scissor.window_offset_disable;
    const s16 offset_x = enable_offset ? s16(regs.window_offset.window_x_offset) : s16{0};
    const s16 offset_y = enable_offset ? s16(regs.window_offset.window_y_offset) : s16{0};
    const auto& screen = regs.screen_scissor;
    const auto& window = regs.window_scissor;
    const auto& generic = regs.generic_scissor;
    AmdGpu::Scissor scissor{};
    scissor.top_left_x = std::max(
        {screen.top_left_x, s16(window.top_left_x + offset_x), s16(generic.top_left_x + offset_x)});
    scissor.top_left_y = std::max(
        {screen.top_left_y, s16(window.top_left_y + offset_y), s16(generic.top_left_y + offset_y)});
    scissor.bottom_right_x = std::min({screen.bottom_right_x, s16(window.bottom_right_x + offset_x),
                                       s16(generic.bottom_right_x + offset_x)});
    scissor.bottom_right_y = std::min({screen.bottom_right_y, s16(window.bottom_right_y + offset_y),
                                       s16(generic.bottom_right_y + offset_y)});
    if (regs.mode_control.vport_scissor_enable) {
        const auto& vport = regs.viewport_scissors[0];
        scissor.top_left_x = std::max(scissor.top_left_x, s16(vport.top_left_x));
        scissor.top_left_y = std::max(scissor.top_left_y, s16(vport.top_left_y));
        scissor.bottom_right_x =
            std::min(AmdGpu::Scissor::Clamp(scissor.bottom_right_x), vport.bottom_right_x);
        scissor.bottom_right_y =
            std::min(AmdGpu::Scissor::Clamp(scissor.bottom_right_y), vport.bottom_right_y);
    }
    return scissor;
}

// The source of each component of a mapping as a letter: 0, 1, r, g, b or a.
std::string SwizzleName(const AmdGpu::CompMapping& mapping) {
    constexpr std::string_view Letters = "01??rgba";
    std::string name;
    for (const AmdGpu::CompSwizzle swizzle : mapping.array) {
        name += Letters[static_cast<u32>(swizzle)];
    }
    return name;
}

CameraCheck MakeCamera(const std::array<float, CameraFloats>& cb, const GrrViewport& viewport,
                       AmdGpu::ClipSpace clip_space, GrrCamera& camera) {
    camera = GrrCamera{.viewport = viewport};
    if (!std::ranges::all_of(cb, [](float value) { return std::isfinite(value); })) {
        return CameraCheck::NotFinite;
    }
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            camera.view[r][c] = cb[r * 4 + c];
            camera.view_proj[r][c] = cb[16 + r * 4 + c];
        }
    }
    const GrrMatrix& view = camera.view;
    const GrrMatrix& view_proj = camera.view_proj;
    if (std::abs(view[0][3]) > 1e-6 || std::abs(view[1][3]) > 1e-6 || std::abs(view[2][3]) > 1e-6 ||
        std::abs(view[3][3] - 1.0) > 1e-6) {
        return CameraCheck::NotAffine;
    }
    for (u32 r = 0; r < 3; ++r) {
        if (std::abs(std::hypot(view[r][0], view[r][1], view[r][2]) - 1.0) > 1e-3) {
            return CameraCheck::NotRigid;
        }
    }
    // The projection maps view z to clip w = -z, so column 3 of the view-projection is the negated
    // column 2 of the view.
    for (u32 r = 0; r < 4; ++r) {
        if (std::abs(view_proj[r][3] + view[r][2]) > 1e-3 * (1.0 + std::abs(view[r][2]))) {
            return CameraCheck::ClipW;
        }
    }
    GrrMatrix inverse_view{};
    if (!Invert(view, inverse_view)) {
        return CameraCheck::NotInvertible;
    }
    camera.projection = Multiply(inverse_view, view_proj);
    camera.position = {inverse_view[3][0], inverse_view[3][1], inverse_view[3][2]};
    // P11 = cot(fov / 2). With clip z in [-w, w], P22 = (n + f) / (n - f) and P32 = 2nf / (n - f);
    // with clip z in [0, w], P22 = f / (n - f) and P32 = nf / (n - f).
    const GrrMatrix& projection = camera.projection;
    const bool minus_w_to_w = clip_space == AmdGpu::ClipSpace::MinusWToW;
    const double near_plane =
        projection[3][2] / (minus_w_to_w ? projection[2][2] - 1.0 : projection[2][2]);
    const double far_plane = projection[3][2] / (projection[2][2] + 1.0);
    const double vertical_fov = 2.0 * std::atan(1.0 / std::abs(projection[1][1]));
    camera.params = CameraParams{
        .near_plane = static_cast<float>(near_plane),
        .far_plane = static_cast<float>(far_plane),
        .vertical_fov = static_cast<float>(vertical_fov),
    };
    if (!(std::isfinite(far_plane) && near_plane > 0.0 && near_plane < far_plane &&
          vertical_fov > 0.0 && vertical_fov < std::numbers::pi && projection[0][0] > 0.0)) {
        return CameraCheck::Projection;
    }
    camera.params.valid = true;
    camera.valid = true;
    return CameraCheck::Valid;
}

} // namespace

GrrDrawHashes HashesOf(const Vulkan::GraphicsPipeline& pipeline) {
    const auto stages = pipeline.GetStages();
    const auto hash = [&stages](Shader::SwStage stage) {
        const Shader::Info* info = stages[static_cast<u32>(stage)];
        return info ? info->pgm_hash : u64{0};
    };
    return {
        .vs = hash(Shader::SwStage::Vertex),
        .gs = hash(Shader::SwStage::Geometry),
        .ps = hash(Shader::SwStage::Fragment),
    };
}

bool IsComposite(const GrrDrawHashes& hashes) {
    return hashes.vs == GrrHash::CompositeVs ||
           std::ranges::contains(GrrHash::CompositePs, hashes.ps);
}

GrrCameraDelta CompareCameras(const GrrCamera& current, const GrrCamera& previous) {
    GrrCameraDelta delta{
        .translation = std::hypot(current.position[0] - previous.position[0],
                                  current.position[1] - previous.position[1],
                                  current.position[2] - previous.position[2]),
        .fov_delta = std::abs(current.params.vertical_fov - previous.params.vertical_fov),
    };
    // Q = A^T B for the rotations A of the current and B of the previous view. The sine from the
    // antisymmetric part keeps the angle exact near 0 degrees.
    std::array<std::array<double, 3>, 3> q{};
    for (u32 r = 0; r < 3; ++r) {
        for (u32 c = 0; c < 3; ++c) {
            for (u32 k = 0; k < 3; ++k) {
                q[r][c] += current.view[k][r] * previous.view[k][c];
            }
        }
    }
    const double sine = 0.5 * std::hypot(q[2][1] - q[1][2], q[0][2] - q[2][0], q[1][0] - q[0][1]);
    const double cosine = 0.5 * (q[0][0] + q[1][1] + q[2][2] - 1.0);
    delta.rotation_degrees = std::atan2(sine, cosine) * (180.0 / std::numbers::pi);
    delta.cut = delta.translation > CutTranslation || delta.rotation_degrees > CutRotationDegrees ||
                delta.fov_delta > CutFovDelta;
    return delta;
}

std::optional<GrrReprojection> Reprojection(const GrrCamera& current, const GrrCamera& previous) {
    const GrrViewport& viewport = current.viewport;
    const double xs = viewport.x_scale;
    const double xo = viewport.x_offset;
    const double ys = viewport.y_scale;
    const double yo = viewport.y_offset;
    const double zs = viewport.z_scale;
    const double zo = viewport.z_offset;
    // Window to NDC and NDC to homogeneous window coordinates; one is the inverse of the other.
    const GrrMatrix to_ndc{{
        {1.0 / xs, 0.0, 0.0, 0.0},
        {0.0, 1.0 / ys, 0.0, 0.0},
        {0.0, 0.0, 1.0 / zs, 0.0},
        {-xo / xs, -yo / ys, -zo / zs, 1.0},
    }};
    const GrrMatrix to_window{{
        {xs, 0.0, 0.0, 0.0},
        {0.0, ys, 0.0, 0.0},
        {0.0, 0.0, zs, 0.0},
        {xo, yo, zo, 1.0},
    }};
    // Without the camera translation the sky follows the camera rotation only.
    const auto sky_view_proj = [](const GrrCamera& camera) {
        GrrMatrix rotation = camera.view;
        rotation[3] = {0.0, 0.0, 0.0, 1.0};
        return Multiply(rotation, camera.projection);
    };
    GrrMatrix scene_inverse{};
    GrrMatrix sky_inverse{};
    if (!Invert(current.view_proj, scene_inverse) || !Invert(sky_view_proj(current), sky_inverse)) {
        return std::nullopt;
    }
    // The world positions are large, so the two cameras cancel in double before the conversion.
    const GrrMatrix scene =
        Multiply(Multiply(Multiply(to_ndc, scene_inverse), previous.view_proj), to_window);
    const GrrMatrix sky =
        Multiply(Multiply(Multiply(to_ndc, sky_inverse), sky_view_proj(previous)), to_window);
    GrrReprojection reprojection{};
    constexpr std::array<u32, 3> Columns{0, 1, 3};
    for (u32 i = 0; i < Columns.size(); ++i) {
        for (u32 r = 0; r < 4; ++r) {
            reprojection.scene[i][r] = static_cast<float>(scene[r][Columns[i]]);
            reprojection.sky[i][r] = static_cast<float>(sky[r][Columns[i]]);
        }
    }
    const auto finite = [](const std::array<float, 4>& column) {
        return std::ranges::all_of(column, [](float value) { return std::isfinite(value); });
    };
    if (!std::ranges::all_of(reprojection.scene, finite) ||
        !std::ranges::all_of(reprojection.sky, finite)) {
        return std::nullopt;
    }
    return reprojection;
}

struct GrrCapture::Impl {
    void LatchCamera(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                     Vulkan::Rasterizer& rasterizer);
    CameraCheck ReadCamera(const Shader::Info& vs, Vulkan::Rasterizer& rasterizer,
                           const GrrViewport& viewport, AmdGpu::ClipSpace clip_space,
                           std::array<float, CameraFloats>& cb);
    void LogRejection(CameraCheck check, const Shader::Info& vs,
                      const std::array<float, CameraFloats>& cb) const;
    void LatchCopyLuma(const Vulkan::GraphicsPipeline& pipeline,
                       const AmdGpu::Liverpool& liverpool);

    GrrCamera camera{};
    GrrSceneDepth scene{};
    VAddr copyluma_target{};
    VAddr composite_target{};
    VAddr scene_depth_address{};
    BoundedSet<VAddr, 4> logged_camera_draws;
    BoundedSet<VAddr, 8> logged_camera_buffers;
    BoundedSet<std::array<VAddr, 2>, 8> logged_copyluma;
    BoundedSet<std::array<VAddr, 2>, 8> logged_fxaa;
    std::array<bool, CameraCheckNames.size()> logged_rejections{};
    std::array<bool, FxaaCheckNames.size()> logged_fxaa_checks{};
    std::array<bool, FaultNames.size()> logged_faults{};
    u32 camera_logs{};
    CameraParams logged_params{};
};

void GrrCapture::Impl::LatchCamera(const Vulkan::GraphicsPipeline& pipeline,
                                   const AmdGpu::Liverpool& liverpool,
                                   Vulkan::Rasterizer& rasterizer) {
    const auto& regs = liverpool.regs;
    const auto& color = regs.color_buffers[0];
    const auto& db = regs.depth_buffer;
    const u32 mrt_mask = pipeline.GetGraphicsKey().mrt_mask;
    const GrrViewport viewport = ViewportOf(regs);
    // Colour and depth images are sized by the extent hint when present, else by pitch and height.
    const auto extent = [](AmdGpu::CbDbExtent hint, u32 pitch, u32 height) {
        return hint.Valid() ? vk::Extent2D{hint.width, hint.height} : vk::Extent2D{pitch, height};
    };
    const vk::Extent2D color_extent =
        extent(liverpool.last_cb_extent[0], color.Pitch(), color.Height());
    const vk::Extent2D depth_extent = extent(liverpool.last_db_extent, db.Pitch(), db.Height());
    const auto covers = [&viewport](vk::Extent2D image) {
        return image.width >= viewport.size.width && image.height >= viewport.size.height;
    };
    const bool qualifies =
        mrt_mask == 1 && color && color.GetDataFmt() == AmdGpu::DataFormat::Format16_16_16_16 &&
        color.GetNumberFmt() == AmdGpu::NumberFormat::Float && db.DepthValid() &&
        db.z_info.format == AmdGpu::DepthBuffer::ZFormat::Z32Float && db.NumSamples() == 1 &&
        !regs.IsClipDisabled() && viewport.size.width != 0 && viewport.z_scale != 0.0f &&
        covers(color_extent) && covers(depth_extent);
    if (logged_camera_draws.Insert(db.DepthAddress())) {
        const auto& ctl = regs.viewport_control;
        const u32 enables = ctl.xscale_enable | ctl.xoffset_enable << 1 | ctl.yscale_enable << 2 |
                            ctl.yoffset_enable << 3 | ctl.zscale_enable << 4 |
                            ctl.zoffset_enable << 5;
        LOG_INFO(Render_Vulkan,
                 "GRR FSR: camera draw: mrt {:#x} colour {:#x} {} {} extent {}x{}, depth {:#x} {} "
                 "stencil {} samples {} extent {}x{}, viewport x {}/{} y {}/{} z {}/{} enables "
                 "{:#x} clip space {} clip disabled {}, scene {}x{}, qualifies {}",
                 mrt_mask, color.Address(), color.GetDataFmt(), color.GetNumberFmt(),
                 color_extent.width, color_extent.height, db.DepthAddress(),
                 magic_enum::enum_name(db.z_info.format),
                 magic_enum::enum_name(db.stencil_info.format), db.NumSamples(), depth_extent.width,
                 depth_extent.height, viewport.x_scale, viewport.x_offset, viewport.y_scale,
                 viewport.y_offset, viewport.z_scale, viewport.z_offset, enables,
                 magic_enum::enum_name(regs.clipper_control.clip_space), regs.IsClipDisabled(),
                 viewport.size.width, viewport.size.height, qualifies);
    }
    if (!qualifies) {
        return;
    }
    scene = GrrSceneDepth{
        .buffer = db,
        .view = regs.depth_view,
        .control = regs.depth_control,
        .htile_address = regs.depth_htile_data_base.GetAddress(),
        .hint = liverpool.last_db_extent,
        .extent = viewport.size,
        .valid = true,
    };
    scene_depth_address = db.DepthAddress();

    const Shader::Info& vs = pipeline.GetStage(Shader::SwStage::Vertex);
    std::array<float, CameraFloats> cb{};
    const CameraCheck check =
        ReadCamera(vs, rasterizer, viewport, regs.clipper_control.clip_space, cb);
    if (check != CameraCheck::Valid) {
        if (!std::exchange(logged_rejections[static_cast<std::size_t>(check)], true)) {
            LogRejection(check, vs, cb);
        }
        return;
    }
    const CameraParams& params = camera.params;
    const auto moved = [](float value, float logged) {
        return std::abs(value - logged) > 0.01f * std::abs(logged);
    };
    if (camera_logs < MaxCameraLogs &&
        (camera_logs == 0 || moved(params.near_plane, logged_params.near_plane) ||
         moved(params.far_plane, logged_params.far_plane) ||
         moved(params.vertical_fov, logged_params.vertical_fov))) {
        ++camera_logs;
        logged_params = params;
        LOG_INFO(Render_Vulkan,
                 "GRR FSR: camera near {} far {} vertical fov {} NDC offset terms {} {} position "
                 "({:.1f}, {:.1f}, {:.1f})",
                 params.near_plane, params.far_plane, params.vertical_fov, camera.projection[2][0],
                 camera.projection[2][1], camera.position[0], camera.position[1],
                 camera.position[2]);
    }
}

CameraCheck GrrCapture::Impl::ReadCamera(const Shader::Info& vs, Vulkan::Rasterizer& rasterizer,
                                         const GrrViewport& viewport, AmdGpu::ClipSpace clip_space,
                                         std::array<float, CameraFloats>& cb) {
    const auto is_camera = [&vs](const Shader::BufferResource& res) {
        return res.buffer_type == Shader::BufferType::Guest && !res.is_written &&
               res.GetSharp(vs).GetSize() == CameraConstantBufferBytes;
    };
    if (std::ranges::count_if(vs.buffers, is_camera) != 1) {
        return CameraCheck::NoConstantBuffer;
    }
    const VAddr address = std::ranges::find_if(vs.buffers, is_camera)->GetSharp(vs).base_address;
    if (!IsGuestReadable(rasterizer, address, sizeof(cb))) {
        return CameraCheck::NotReadable;
    }
    std::memcpy(cb.data(), reinterpret_cast<const void*>(address), sizeof(cb));
    if (logged_camera_buffers.Insert(address)) {
        LOG_INFO(Render_Vulkan, "GRR FSR: camera constant buffer at {:#x}", address);
    }
    return MakeCamera(cb, viewport, clip_space, camera);
}

void GrrCapture::Impl::LogRejection(CameraCheck check, const Shader::Info& vs,
                                    const std::array<float, CameraFloats>& cb) const {
    const std::string_view name = CameraCheckNames[static_cast<std::size_t>(check)];
    if (check == CameraCheck::NoConstantBuffer) {
        std::vector<u32> sizes;
        for (const Shader::BufferResource& res : vs.buffers) {
            sizes.push_back(res.IsSpecial() ? 0u : res.GetSharp(vs).GetSize());
        }
        LOG_WARNING(Render_Vulkan, "GRR FSR: camera rejected ({}, {} buffers of sizes [{}])", name,
                    sizes.size(), fmt::join(sizes, ", "));
        return;
    }
    if (check == CameraCheck::NotReadable) {
        LOG_WARNING(Render_Vulkan, "GRR FSR: camera rejected ({})", name);
        return;
    }
    const CameraParams& params = camera.params;
    const std::string reason =
        check == CameraCheck::Projection
            ? fmt::format("{} near {} far {} vertical fov {}", name, params.near_plane,
                          params.far_plane, params.vertical_fov)
            : std::string{name};
    LOG_WARNING(Render_Vulkan, "GRR FSR: camera rejected ({}): view [{}] view-projection [{}]",
                reason, fmt::join(std::span{cb}.first<16>(), " "),
                fmt::join(std::span{cb}.last<16>(), " "));
}

void GrrCapture::Impl::LatchCopyLuma(const Vulkan::GraphicsPipeline& pipeline,
                                     const AmdGpu::Liverpool& liverpool) {
    const auto& target = liverpool.regs.color_buffers[0];
    copyluma_target = target.Address();
    const Shader::Info& ps = pipeline.GetStage(Shader::SwStage::Fragment);
    AmdGpu::Image input{};
    for (const Shader::ImageResource& res : ps.images) {
        const AmdGpu::Image tsharp = res.GetSharp(ps);
        if (!res.is_written && tsharp.Address() != 0) {
            input = tsharp;
            break;
        }
    }
    if (logged_copyluma.Insert({input.Address(), copyluma_target})) {
        LOG_INFO(Render_Vulkan,
                 "GRR FSR: CopyLuma {:#x} {} {} {}x{} swizzle {} -> {:#x} {} {} swap {}",
                 input.Address(), input.GetDataFmt(), input.GetNumberFmt(),
                 static_cast<u32>(input.width) + 1, static_cast<u32>(input.height) + 1,
                 SwizzleName(input.DstSelect()), copyluma_target, target.GetDataFmt(),
                 target.GetNumberFmt(), magic_enum::enum_name(target.info.comp_swap));
    }
}

GrrCapture::GrrCapture() : impl{std::make_unique<Impl>()} {}

GrrCapture::~GrrCapture() = default;

void GrrCapture::StartFrame() {
    Impl& state = *impl;
    state.camera = {};
    state.scene = {};
    state.copyluma_target = 0;
    state.composite_target = 0;
}

void GrrCapture::OnDraw(const Vulkan::GraphicsPipeline& pipeline,
                        const AmdGpu::Liverpool& liverpool, Vulkan::Rasterizer& rasterizer,
                        const GrrDrawHashes& hashes) {
    if (hashes.ps == GrrHash::CopyLumaPs) {
        impl->LatchCopyLuma(pipeline, liverpool);
    } else if (IsComposite(hashes)) {
        impl->composite_target = liverpool.regs.color_buffers[0].Address();
    } else if (hashes.vs == GrrHash::CameraVs && !impl->scene.valid) {
        impl->LatchCamera(pipeline, liverpool, rasterizer);
    }
}

bool GrrCapture::IdentifyFxaa(const Vulkan::GraphicsPipeline& pipeline,
                              const AmdGpu::Liverpool& liverpool, GrrFxaaDraw& draw) {
    Impl& state = *impl;
    const auto& regs = liverpool.regs;
    const auto& key = pipeline.GetGraphicsKey();
    const Shader::Info& ps = pipeline.GetStage(Shader::SwStage::Fragment);
    draw = {};
    u32 inputs = 0;
    for (const Shader::ImageResource& res : ps.images) {
        const AmdGpu::Image tsharp = res.GetSharp(ps);
        if (!res.is_written && tsharp.Address() != 0) {
            ++inputs;
            draw.color = ImageBinding{tsharp, res, true};
        }
    }
    draw.target = regs.color_buffers[0];
    draw.target_hint = liverpool.last_cb_extent[0];
    // The recompiler exports component Map(i) of the shader output into target component i.
    for (u32 i = 0; i < draw.components.size(); ++i) {
        draw.components[i] = static_cast<u8>(key.color_buffers[0].swizzle.Map(i));
    }
    const GrrViewport viewport = ViewportOf(regs);
    const AmdGpu::Scissor scissor = ScissorOf(regs);
    draw.render_size = vk::Extent2D{draw.color.Width(), draw.color.Height()};
    draw.output_size = viewport.size;

    const AmdGpu::Image& input = draw.color.tsharp;
    const AmdGpu::ColorBuffer& target = draw.target;
    const AmdGpu::BlendControl& blend = regs.blend_control[0];
    // The depth and stencil tests as the rasterizer enables them.
    const bool depth_test = regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    const bool stencil_test = regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    const FxaaCheck check = [&] {
        const AmdGpu::CompMapping swizzle = input.DstSelect();
        if (inputs != 1) {
            return FxaaCheck::InputCount;
        }
        if (input.GetDataFmt() != AmdGpu::DataFormat::Format8_8_8_8 ||
            input.GetNumberFmt() != AmdGpu::NumberFormat::Srgb) {
            return FxaaCheck::InputFormat;
        }
        if (input.GetType() != AmdGpu::ImageType::Color2D || input.base_level != 0 ||
            input.base_array != 0) {
            return FxaaCheck::InputView;
        }
        if (swizzle.r != AmdGpu::CompSwizzle::Red || swizzle.g != AmdGpu::CompSwizzle::Green ||
            swizzle.b != AmdGpu::CompSwizzle::Blue) {
            return FxaaCheck::InputSwizzle;
        }
        if (key.mrt_mask != 1 || !target ||
            target.GetDataFmt() != AmdGpu::DataFormat::Format8_8_8_8 ||
            target.GetNumberFmt() != AmdGpu::NumberFormat::Srgb || target.NumSamples() != 1 ||
            target.BaseSlice() != 0 || target.NumSlices() != 1) {
            return FxaaCheck::Target;
        }
        if ((blend.enable && !target.info.blend_bypass) ||
            regs.color_target_mask.GetMask(0) != 0xfu) {
            return FxaaCheck::Blend;
        }
        // Disable passes the draw filter and binds no colour target.
        if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Normal) {
            return FxaaCheck::ColorWrites;
        }
        if (viewport.size.width == 0) {
            return FxaaCheck::Viewport;
        }
        if (scissor.top_left_x > 0 || scissor.top_left_y > 0 ||
            scissor.bottom_right_x < static_cast<s32>(viewport.size.width) ||
            scissor.bottom_right_y < static_cast<s32>(viewport.size.height)) {
            return FxaaCheck::Scissor;
        }
        if (depth_test || stencil_test) {
            return FxaaCheck::DepthStencil;
        }
        return FxaaCheck::Recognized;
    }();

    if (state.logged_fxaa.Insert({input.Address(), target.Address()})) {
        const std::string order{"rgba"[draw.components[0]], "rgba"[draw.components[1]],
                                "rgba"[draw.components[2]], "rgba"[draw.components[3]]};
        LOG_INFO(Render_Vulkan,
                 "GRR FSR: FXAA {:#x} {} {} {}x{} swizzle {} -> {:#x} {} {} swap {} order {} VS "
                 "{:#x} blend {} write mask {:#x} fast clear {} viewport {}x{} scissor ({}, {})-"
                 "({}, {}) depth test {} stencil test {}",
                 input.Address(), input.GetDataFmt(), input.GetNumberFmt(), draw.render_size.width,
                 draw.render_size.height, SwizzleName(input.DstSelect()), target.Address(),
                 target.GetDataFmt(), target.GetNumberFmt(),
                 magic_enum::enum_name(target.info.comp_swap), order, HashesOf(pipeline).vs,
                 blend.enable != 0, regs.color_target_mask.GetMask(0), target.info.fast_clear != 0,
                 viewport.size.width, viewport.size.height, scissor.top_left_x, scissor.top_left_y,
                 scissor.bottom_right_x, scissor.bottom_right_y, depth_test, stencil_test);
    }
    if (check == FxaaCheck::Recognized) {
        return true;
    }
    if (!std::exchange(state.logged_fxaa_checks[static_cast<std::size_t>(check)], true)) {
        LOG_WARNING(Render_Vulkan, "GRR FSR: FXAA bindings not recognized: {}",
                    FxaaCheckNames[static_cast<std::size_t>(check)]);
    }
    return false;
}

bool GrrCapture::CopyLumaWrote(VAddr address) const {
    return impl->copyluma_target == address;
}

bool GrrCapture::CompositeWrote(VAddr address) const {
    return impl->composite_target == address;
}

const GrrCamera& GrrCapture::Camera() const {
    return impl->camera;
}

const GrrSceneDepth& GrrCapture::SceneDepth() const {
    return impl->scene;
}

VAddr GrrCapture::SceneDepthAddress() const {
    return impl->scene_depth_address;
}

bool GrrCapture::Acquire(const GrrFxaaDraw& draw, const GrrSceneDepth& scene,
                         Vulkan::Rasterizer& rasterizer, GrrImages& images) {
    Impl& state = *impl;
    images = {};
    const auto fail = [&state](std::string_view name, Fault fault, const VideoCore::Image* image) {
        if (std::exchange(state.logged_faults[static_cast<std::size_t>(fault)], true)) {
            return false;
        }
        const std::string_view cause = FaultNames[static_cast<std::size_t>(fault)];
        if (!image) {
            LOG_WARNING(Render_Vulkan, "GRR FSR: {} image {}", name, cause);
            return false;
        }
        const vk::ImageCreateInfo& image_ci = image->backing->image.image_ci;
        LOG_WARNING(Render_Vulkan,
                    "GRR FSR: {} image {}: {} {}x{} layers {} samples {} (backing {}) usage {}",
                    name, cause, vk::to_string(image_ci.format), image->info.size.width,
                    image->info.size.height, image_ci.arrayLayers, image->info.num_samples,
                    image->backing_num_samples, vk::to_string(image_ci.usage));
        return false;
    };
    if (!rasterizer.IsMapped(draw.color.tsharp.Address(), 1)) {
        return fail("colour", Fault::Unmapped, nullptr);
    }
    if (!rasterizer.IsMapped(scene.buffer.DepthAddress(), 1)) {
        return fail("scene depth", Fault::Unmapped, nullptr);
    }
    if (!rasterizer.IsMapped(draw.target.Address(), 1)) {
        return fail("target", Fault::Unmapped, nullptr);
    }

    // The descs of the draws that bind the images.
    VideoCore::TextureCache& texture_cache = rasterizer.GetTextureCache();
    VideoCore::TextureCache::ImageDesc color_desc{draw.color.tsharp, draw.color.resource};
    VideoCore::TextureCache::ImageDesc depth_desc{scene.buffer, scene.view, scene.control,
                                                  scene.htile_address, scene.hint};
    VideoCore::TextureCache::ImageDesc target_desc{draw.target, draw.target_hint};
    const auto registered = [&texture_cache](VideoCore::ImageId id) {
        return True(texture_cache.GetImage(id).flags & VideoCore::ImageFlagBits::Registered);
    };
    const auto find_images = [&] {
        images.color.id = texture_cache.FindImage(color_desc);
        images.depth.id = texture_cache.FindImage(depth_desc);
        images.output.id = texture_cache.FindImage(target_desc);
        return registered(images.color.id) && registered(images.depth.id) &&
               registered(images.output.id);
    };
    // A lookup can replace an image an earlier lookup returned (expansion, overlap merge). The
    // replaced image stays allocated until the GPU passes the current tick.
    if (!find_images() && !find_images()) {
        return fail("an FXAA", Fault::Replaced, nullptr);
    }
    VideoCore::Image& color = texture_cache.GetImage(images.color.id);
    VideoCore::Image& depth = texture_cache.GetImage(images.depth.id);
    VideoCore::Image& target = texture_cache.GetImage(images.output.id);
    if (const auto fault = Check(color, {.size = draw.render_size, .rgba8 = true})) {
        return fail("colour", *fault, &color);
    }
    if (const auto fault = Check(depth, {.size = draw.render_size, .depth = true})) {
        return fail("scene depth", *fault, &depth);
    }
    if (const auto fault =
            Check(target, {.size = draw.output_size, .rgba8 = true, .storage = true})) {
        return fail("target", *fault, &target);
    }

    Vulkan::Runtime& runtime = rasterizer.GetRuntime();
    images.color.view = texture_cache.FindTexture(images.color.id, color_desc);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);

    // An R32 view format selects the depth aspect of a depth-stencil image, an R8 one its stencil.
    VideoCore::ImageViewInfo depth_view{};
    depth_view.format = vk::Format::eR32Sfloat;
    images.depth.view = depth.FindViewHandle(depth_view);
    vk::ImageView stencil_view{};
    if (depth.info.props.has_stencil) {
        VideoCore::ImageViewInfo stencil_info{};
        stencil_info.format = vk::Format::eR8Uint;
        stencil_view = depth.FindViewHandle(stencil_info);
    }
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);

    // The bookkeeping the replaced draw does for its target: the GPU-modified flag, the readback
    // queue and the fast-clear metadata.
    static_cast<void>(texture_cache.FindRenderTarget(images.output.id, target_desc));
    texture_cache.TouchMeta(draw.target.CmaskAddress(), 0, false);
    VideoCore::ImageViewInfo output_view{};
    output_view.format = vk::Format::eR8G8B8A8Unorm;
    output_view.range = target_desc.view_info.range;
    output_view.is_storage = true;
    images.output.view = target.FindViewHandle(output_view);
    runtime.Transit(&target, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderWrite);

    const auto describe = [](GuestImage& guest, VideoCore::Image& image) {
        const vk::ImageCreateInfo& image_ci = image.backing->image.image_ci;
        guest.image = &image;
        guest.format = image_ci.format;
        guest.usage = image_ci.usage;
        guest.extent = vk::Extent2D{image.info.size.width, image.info.size.height};
    };
    describe(images.color, color);
    describe(images.depth, depth);
    describe(images.output, target);
    if (stencil_view) {
        images.stencil = images.depth;
        images.stencil.view = stencil_view;
    }
    return true;
}

} // namespace Gr2Fsr3
