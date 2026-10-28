// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include <fmt/ranges.h>
#include <magic_enum/magic_enum.hpp>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/gr2_fsr3/fsr3_inputs.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Gr2Fsr3 {

namespace {

// 30 units is also the distance at which the game raises its own reset flag (FUN_013bf4d0).
constexpr float CutTranslation = 30.0f;
constexpr float CutRotationDegrees = 45.0f;
constexpr float CutFovDelta = 0.35f;
// Camera motion, in units or degrees, above which an unchanged constant buffer repeats a view.
constexpr float RepeatMotion = 1e-3f;
constexpr size_t MaxLoggedRenderSizes = 16;

void LogImages(const Shader::Info& info, Common::Log::Level level) {
    for (size_t index = 0; index < info.images.size(); ++index) {
        const Shader::ImageResource& res = info.images[index];
        const AmdGpu::Image tsharp = res.GetSharp(info);
        LOG_GENERIC(Render_Vulkan, level,
                    "GR2 FSR3:   image {}: {:#x} {} {} {}x{} levels {} offset {} written {}", index,
                    tsharp.Address(), tsharp.GetDataFmt(), tsharp.GetNumberFmt(),
                    static_cast<u32>(tsharp.width) + 1, static_cast<u32>(tsharp.height) + 1,
                    tsharp.NumLevels(), res.sharp_fetch.offsets[0], res.is_written);
    }
}

} // namespace

bool IsGuestReadable(Vulkan::Rasterizer& rasterizer, VAddr address, u64 size) {
    return rasterizer.IsMapped(address, size) &&
           !rasterizer.GetBufferCache().IsRegionGpuModified(address, size);
}

struct Inputs::Impl {
    VelocityCapture capture;
    decltype(VelocityCapture::cb) previous_cb{};
    bool previous_valid{};
    float previous_fov{};
    CameraParams last_params{};
    u32 equal_cb_frames{};
    std::vector<std::array<u32, 4>> logged_render_sizes;
    bool logged_capture{};
    bool logged_identify{};
    bool logged_camera{};
    bool logged_unbound{};
    bool logged_no_depth{};
    bool logged_unregistered{};
    bool logged_multisampled{};
    bool logged_subresource{};
    bool logged_not_storage{};
};

Inputs::Inputs() : impl{std::make_unique<Impl>()} {}

Inputs::~Inputs() = default;

void Inputs::CaptureVelocity(const Shader::Info& info, Vulkan::Rasterizer& rasterizer, u64 frame) {
    VelocityCapture capture{.frame = frame};
    bool unique = true;
    for (const Shader::ImageResource& res : info.images) {
        const AmdGpu::Image tsharp = res.GetSharp(info);
        if (tsharp.Address() == 0) {
            continue;
        }
        const AmdGpu::DataFormat data_format = tsharp.GetDataFmt();
        const AmdGpu::NumberFormat number_format = tsharp.GetNumberFmt();
        ImageBinding* slot = nullptr;
        if (res.is_written && data_format == AmdGpu::DataFormat::Format16_16 &&
            number_format == AmdGpu::NumberFormat::Float) {
            slot = &capture.velocity;
        } else if (!res.is_written && data_format == AmdGpu::DataFormat::Format32 &&
                   number_format == AmdGpu::NumberFormat::Float) {
            slot = &capture.depth;
        } else if (!res.is_written && data_format == AmdGpu::DataFormat::Format8 &&
                   number_format == AmdGpu::NumberFormat::Uint) {
            slot = &capture.stencil;
        }
        if (slot) {
            unique = unique && !slot->present;
            *slot = ImageBinding{tsharp, res, true};
        }
    }

    const auto cb_res = std::ranges::find_if(info.buffers, [](const Shader::BufferResource& res) {
        return res.buffer_type == Shader::BufferType::Guest && !res.is_written;
    });
    if (cb_res != info.buffers.end()) {
        const AmdGpu::Buffer buffer = cb_res->GetSharp(info);
        const VAddr address = buffer.base_address;
        if (buffer.GetSize() >= sizeof(capture.cb) &&
            IsGuestReadable(rasterizer, address, sizeof(capture.cb))) {
            std::memcpy(capture.cb.data(), reinterpret_cast<const void*>(address),
                        sizeof(capture.cb));
            capture.cb_valid = true;
        }
    }

    capture.valid = unique && capture.depth.present && capture.velocity.present;
    if ((!capture.valid || !capture.cb_valid) && !std::exchange(impl->logged_capture, true)) {
        LOG_WARNING(Render_Vulkan,
                    "GR2 FSR3: Velocity bindings incomplete: depth {}, velocity {}, stencil {}, "
                    "unique {}, constant buffer {}",
                    capture.depth.present, capture.velocity.present, capture.stencil.present,
                    unique, capture.cb_valid);
    }
    impl->capture = capture;
}

const VelocityCapture& Inputs::Velocity() const {
    return impl->capture;
}

bool Inputs::IdentifyAntialias(const Shader::Info& info, AntialiasBindings& bindings) {
    bindings = {};
    bool unique = true;
    for (const Shader::ImageResource& res : info.images) {
        const AmdGpu::Image tsharp = res.GetSharp(info);
        if (tsharp.Address() == 0) {
            continue;
        }
        const AmdGpu::DataFormat data_format = tsharp.GetDataFmt();
        const AmdGpu::NumberFormat number_format = tsharp.GetNumberFmt();
        ImageBinding* slot = nullptr;
        if (res.is_written) {
            slot = &bindings.output;
        } else if (data_format == AmdGpu::DataFormat::Format8 &&
                   number_format == AmdGpu::NumberFormat::Uint) {
            slot = &bindings.stencil;
        } else if (data_format == AmdGpu::DataFormat::Format16_16 &&
                   number_format == AmdGpu::NumberFormat::Float) {
            slot = &bindings.velocity;
        } else if (data_format == AmdGpu::DataFormat::Format8_8_8_8 &&
                   number_format == AmdGpu::NumberFormat::Srgb) {
            // The colour T# is passed in user SGPRs, the previous history in the resource table.
            slot = res.sharp_fetch.offsets[0] < Shader::NUM_USER_DATA_REGS ? &bindings.color
                                                                           : &bindings.history;
        }
        if (slot) {
            unique = unique && !slot->present;
            *slot = ImageBinding{tsharp, res, true};
        }
    }

    // An absent output keeps a zero T#, whose data format is FormatInvalid.
    const AmdGpu::NumberFormat output_format = bindings.output.tsharp.GetNumberFmt();
    const bool output_rgba8 =
        bindings.output.tsharp.GetDataFmt() == AmdGpu::DataFormat::Format8_8_8_8 &&
        (output_format == AmdGpu::NumberFormat::Srgb ||
         output_format == AmdGpu::NumberFormat::Unorm);
    const bool recognized = unique && output_rgba8 && bindings.color.present &&
                            bindings.velocity.present && bindings.stencil.present;
    if (!recognized && !std::exchange(impl->logged_identify, true)) {
        LOG_WARNING(Render_Vulkan,
                    "GR2 FSR3: Antialias bindings not recognized ({} images), the stock pass runs",
                    info.images.size());
        LogImages(info, Common::Log::Level::Warning);
    }
    return recognized;
}

vk::Extent2D Inputs::RenderSize(const AntialiasBindings& bindings, bool capture_is_current) {
    const vk::Extent2D color{bindings.color.Width(), bindings.color.Height()};
    const VelocityCapture& capture = impl->capture;
    if (!capture_is_current || !capture.cb_valid) {
        return color;
    }
    // Dwords 20-21 hold the width and height of u_f4TargetSize. The range checks also reject NaN
    // and infinities.
    const float target_width = std::bit_cast<float>(capture.cb[20]);
    const float target_height = std::bit_cast<float>(capture.cb[21]);
    const auto fits = [](float size, u32 limit) {
        return size >= 1.0f && size <= static_cast<float>(limit) && std::trunc(size) == size;
    };
    const bool target_fits = fits(target_width, color.width) && fits(target_height, color.height);
    const vk::Extent2D render =
        target_fits ? vk::Extent2D{static_cast<u32>(target_width), static_cast<u32>(target_height)}
                    : color;
    if (target_fits && render == color) {
        return render;
    }
    auto& logged = impl->logged_render_sizes;
    const std::array<u32, 4> key{capture.cb[20], capture.cb[21], color.width, color.height};
    if (logged.size() < MaxLoggedRenderSizes && !std::ranges::contains(logged, key)) {
        logged.push_back(key);
        LOG_WARNING(Render_Vulkan,
                    "GR2 FSR3: Velocity TargetSize {}x{} and colour input {}x{} differ, render "
                    "size {}x{}",
                    target_width, target_height, color.width, color.height, render.width,
                    render.height);
    }
    return render;
}

bool Inputs::Acquire(const AntialiasBindings& bindings, const AcquireRequest& request,
                     Vulkan::Rasterizer& rasterizer, FrameImages& images) {
    // Stencil and Depth resolve the stencil plane T# to the depth-stencil image it belongs to.
    enum class Plane : u8 {
        Color,
        Stencil,
        Depth,
    };
    struct Lookup {
        bool requested;
        const char* name;
        const ImageBinding* binding;
        GuestImage* image;
        Plane plane;
        VideoCore::TextureCache::ImageDesc desc{};
    };
    images = {};
    std::array<Lookup, 6> lookups{{
        {request.color, "colour", &bindings.color, &images.color, Plane::Color},
        {request.velocity, "velocity", &bindings.velocity, &images.velocity, Plane::Color},
        {request.stencil, "stencil", &bindings.stencil, &images.stencil, Plane::Stencil},
        {request.depth_copy, "depth copy", &impl->capture.depth, &images.depth_copy, Plane::Color},
        {request.depth_main, "scene depth", &bindings.stencil, &images.depth_main, Plane::Depth},
        {request.output, "output", &bindings.output, &images.output, Plane::Color},
    }};
    VideoCore::TextureCache& texture_cache = rasterizer.GetTextureCache();

    const auto find_images = [&] {
        for (Lookup& lookup : lookups) {
            if (!lookup.requested) {
                continue;
            }
            const ImageBinding& binding = *lookup.binding;
            if (!binding.present || !rasterizer.IsMapped(binding.tsharp.Address(), 1)) {
                if (!std::exchange(impl->logged_unbound, true)) {
                    LOG_WARNING(Render_Vulkan, "GR2 FSR3: {} image is not bound to mapped memory",
                                lookup.name);
                }
                return false;
            }
            lookup.desc = VideoCore::TextureCache::ImageDesc{binding.tsharp, binding.resource};
            VideoCore::ImageId id = texture_cache.FindImage(lookup.desc);
            if (lookup.plane != Plane::Color) {
                id = texture_cache.GetAssociatedDepth(texture_cache.GetImage(id));
                if (!id) {
                    if (!std::exchange(impl->logged_no_depth, true)) {
                        LOG_WARNING(Render_Vulkan,
                                    "GR2 FSR3: stencil plane {:#x} has no depth image",
                                    binding.tsharp.Address());
                    }
                    return false;
                }
            }
            lookup.image->id = id;
        }
        return true;
    };
    const auto all_registered = [&] {
        return std::ranges::all_of(lookups, [&](const Lookup& lookup) {
            return !lookup.requested || True(texture_cache.GetImage(lookup.image->id).flags &
                                             VideoCore::ImageFlagBits::Registered);
        });
    };
    if (!find_images()) {
        return false;
    }
    // A lookup can replace an image an earlier lookup returned (expansion, overlap merge). The
    // replaced image stays allocated until the GPU passes the current tick.
    if (!all_registered()) {
        if (!find_images()) {
            return false;
        }
        if (!all_registered()) {
            if (!std::exchange(impl->logged_unregistered, true)) {
                LOG_WARNING(Render_Vulkan,
                            "GR2 FSR3: input images keep replacing each other in the texture "
                            "cache");
            }
            return false;
        }
    }

    // An R32 view format selects the depth aspect of a depth-stencil image.
    VideoCore::ImageViewInfo depth_view{};
    depth_view.format = vk::Format::eR32Sfloat;
    Vulkan::Runtime& runtime = rasterizer.GetRuntime();
    for (Lookup& lookup : lookups) {
        if (!lookup.requested) {
            continue;
        }
        GuestImage& guest = *lookup.image;
        VideoCore::Image& image = texture_cache.GetImage(guest.id);
        const vk::ImageCreateInfo& image_ci = image.backing->image.image_ci;
        const bool is_storage = lookup.desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (image.info.num_samples != 1 || image.backing_num_samples != 1) {
            if (!std::exchange(impl->logged_multisampled, true)) {
                LOG_WARNING(Render_Vulkan, "GR2 FSR3: {} image has {} samples (backing {})",
                            lookup.name, image.info.num_samples, image.backing_num_samples);
            }
            return false;
        }
        // FSR reads level 0 and layer 0 of the backing image of each sampled input except the
        // stencil plane, through a 2D view that needs a single-layer image. The texture cache view
        // has to start there too; the depth view of the scene depth plane always does.
        const VideoCore::SubresourceBase& base = lookup.desc.view_info.range.base;
        const bool fsr_input = !is_storage && lookup.plane != Plane::Stencil;
        const bool at_base = lookup.plane == Plane::Depth || (base.level == 0 && base.layer == 0);
        if (fsr_input && (image_ci.arrayLayers != 1 || !at_base)) {
            if (!std::exchange(impl->logged_subresource, true)) {
                LOG_WARNING(Render_Vulkan,
                            "GR2 FSR3: {} image has {} layers, view base level {} layer {}",
                            lookup.name, image_ci.arrayLayers, base.level, base.layer);
            }
            return false;
        }
        if (is_storage && !(image_ci.usage & vk::ImageUsageFlagBits::eStorage)) {
            if (!std::exchange(impl->logged_not_storage, true)) {
                LOG_WARNING(Render_Vulkan, "GR2 FSR3: {} image {} has no storage usage",
                            lookup.name, vk::to_string(image.info.pixel_format));
            }
            return false;
        }
        guest.view = lookup.plane == Plane::Depth
                         ? image.FindViewHandle(depth_view)
                         : texture_cache.FindTexture(guest.id, lookup.desc);
        guest.image = &image;
        guest.format = image_ci.format;
        guest.usage = image_ci.usage;
        guest.extent = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(
            &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
            is_storage ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eShaderRead);
    }
    return true;
}

CameraState Inputs::EvaluateCamera(bool capture_is_current) {
    const VelocityCapture& capture = impl->capture;
    CameraState state{.params = impl->last_params};
    if (!capture_is_current || !capture.cb_valid) {
        return state;
    }
    const auto cb = [&capture](u32 index) { return std::bit_cast<float>(capture.cb[index]); };

    // Dwords 16-19 hold u_f4InverseView = (1/P00, 1/P11, (n-f)/2nf, (f+n)/2nf).
    const float near_plane = 1.0f / (cb(19) - cb(18));
    const float far_plane = 1.0f / (cb(18) + cb(19));
    const float vertical_fov = 2.0f * std::atan(cb(17));
    if (near_plane > 0.0f && far_plane > near_plane && std::isfinite(far_plane) &&
        vertical_fov > 0.0f && vertical_fov < std::numbers::pi_v<float>) {
        state.params = CameraParams{
            .near_plane = near_plane,
            .far_plane = far_plane,
            .vertical_fov = vertical_fov,
            .valid = true,
        };
    } else if (!std::exchange(impl->logged_camera, true)) {
        LOG_WARNING(Render_Vulkan,
                    "GR2 FSR3: camera near {} far {} vertical fov {} rejected, keeping near {} "
                    "far {} vertical fov {}",
                    near_plane, far_plane, vertical_fov, state.params.near_plane,
                    state.params.far_plane, state.params.vertical_fov);
    }

    // Dwords 0-15 hold M, row-major for row vectors, from the current view space to the previous
    // clip space. R = M x P^-1, with P^-1 rows (IV.x,0,0,0), (0,IV.y,0,0), (0,0,0,IV.z) and
    // (0,0,-1,IV.w), maps the current to the previous view space while the projection is unchanged.
    const float r00 = cb(0) * cb(16);
    const float r01 = cb(1) * cb(17);
    const float r02 = -cb(3);
    const float r10 = cb(4) * cb(16);
    const float r11 = cb(5) * cb(17);
    const float r12 = -cb(7);
    const float r20 = cb(8) * cb(16);
    const float r21 = cb(9) * cb(17);
    const float r22 = -cb(11);
    state.translation = std::hypot(cb(12) * cb(16), cb(13) * cb(17), -cb(15));
    // The sine from the antisymmetric part keeps the angle exact near 0 degrees, where the float
    // cosine from the trace resolves only about 0.02 degrees.
    const float sin_angle = 0.5f * std::hypot(r21 - r12, r02 - r20, r10 - r01);
    const float cos_angle = 0.5f * (r00 + r11 + r22 - 1.0f);
    state.rotation_degrees =
        std::atan2(sin_angle, cos_angle) * (180.0f / std::numbers::pi_v<float>);

    const bool same_cb = impl->previous_valid && capture.cb == impl->previous_cb;
    if (impl->previous_valid) {
        state.fov_delta = std::abs(state.params.vertical_fov - impl->previous_fov);
        state.cut = state.translation > CutTranslation ||
                    state.rotation_degrees > CutRotationDegrees || state.fov_delta > CutFovDelta;
    }
    state.repeated =
        same_cb && (state.translation > RepeatMotion || state.rotation_degrees > RepeatMotion);
    if (same_cb) {
        ++impl->equal_cb_frames;
    }
    impl->previous_cb = capture.cb;
    impl->previous_valid = true;
    impl->previous_fov = state.params.vertical_fov;
    impl->last_params = state.params;
    return state;
}

void Inputs::LogDispatch(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                         Vulkan::Rasterizer& rasterizer) {
    LOG_INFO(Render_Vulkan,
             "GR2 FSR3: dispatch {:#x} groups {}x{}x{} queue {} images {} buffers {}",
             info.pgm_hash, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
             static_cast<const void*>(&cs_program), info.images.size(), info.buffers.size());
    LogImages(info, Common::Log::Level::Info);
    for (size_t index = 0; index < info.buffers.size(); ++index) {
        const Shader::BufferResource& res = info.buffers[index];
        if (res.IsSpecial()) {
            LOG_INFO(Render_Vulkan, "GR2 FSR3:   buffer {}: {}", index,
                     magic_enum::enum_name(res.buffer_type));
            continue;
        }
        const AmdGpu::Buffer buffer = res.GetSharp(info);
        const VAddr address = buffer.base_address;
        std::array<u32, 24> dwords{};
        const u32 count = std::min(static_cast<u32>(dwords.size()),
                                   buffer.GetSize() / static_cast<u32>(sizeof(u32)));
        const bool readable = IsGuestReadable(rasterizer, address, count * sizeof(u32));
        if (readable) {
            std::memcpy(dwords.data(), reinterpret_cast<const void*>(address), count * sizeof(u32));
        }
        LOG_INFO(Render_Vulkan, "GR2 FSR3:   buffer {}: {:#x} size {} written {} dwords [{:08x}]",
                 index, address, buffer.GetSize(), res.is_written,
                 fmt::join(std::span<const u32>{dwords.data(), readable ? count : 0u}, " "));
    }
}

void Inputs::LogFrame(const FrameLog& log) {
    const CameraState& camera = log.camera;
    const Jitter::FrameReport& jitter = log.jitter;
    const u32 num_hashes =
        std::min(jitter.num_vertex_hashes, static_cast<u32>(jitter.vertex_hashes.size()));
    LOG_INFO(Render_Vulkan,
             "GR2 FSR3: frame {} mode {} kind {} context {} render {}x{} output {}x{} interval "
             "{:.2f} ms reset {} | camera near {} far {} vertical fov {} valid {} move {:.4f} "
             "turn {:.4f} deg fov delta {:.4f} cut {} repeated {} equal CBs {} | jitter window "
             "{} extent hint {} offset ({:.4f}, {:.4f}) draws eligible {} jittered {} indirect "
             "{} full-screen {} vertex shaders [{:#x}] | fsr4 {} | dlss {}",
             log.frame, magic_enum::enum_name(log.mode), magic_enum::enum_name(log.kind),
             magic_enum::enum_name(log.context), log.render_size.width, log.render_size.height,
             log.output_size.width, log.output_size.height, log.interval_ms, log.reset,
             camera.params.near_plane, camera.params.far_plane, camera.params.vertical_fov,
             camera.params.valid, camera.translation, camera.rotation_degrees, camera.fov_delta,
             camera.cut, camera.repeated, impl->equal_cb_frames, jitter.window_opened,
             jitter.extent_hint, jitter.applied.x, jitter.applied.y, jitter.eligible_draws,
             jitter.jittered_draws, jitter.indirect_draws, jitter.fullscreen_draws,
             fmt::join(std::span<const u64>{jitter.vertex_hashes.data(), num_hashes}, ", "),
             magic_enum::enum_name(log.fsr4_context), magic_enum::enum_name(log.dlss_context));
}

} // namespace Gr2Fsr3
