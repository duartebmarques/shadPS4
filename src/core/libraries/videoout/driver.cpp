// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    present_pacing = EmulatorSettings.GetPresentPacing();
    if (const u32 cap = EmulatorSettings.GetFrameCap(); cap != 0) {
        cap_period_ns = 1'000'000'000 / cap;
    }
    if (present_pacing != 0) {
        swap_thread = std::jthread([&](std::stop_token token) { SwapThread(token); });
    }
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

namespace {
std::atomic<u64> g_lat_pad_us{0};
u64 g_lat_flush_us = 0;
std::mutex g_lat_mutex;
std::deque<std::pair<u64, u64>> g_lat_guest;
FILE* g_lat_file = [] {
    const char* path = std::getenv("SHAD_LAT_LOG");
    const char* home = std::getenv("HOME");
    const std::string fallback = std::string{home ? home : "."} + "/.local/share/shadPS4/log/lat-" +
                                 std::to_string(std::time(nullptr)) + ".txt";
    FILE* f = std::fopen(path ? path : fallback.c_str(), "w");
    if (f) {
        std::fputs("guest_flip_us pad_read_us present_start_us present_end_us queued\n", f);
    }
    return f;
}();
} // namespace

void LatNotePadRead() {
    if (g_lat_file) {
        g_lat_pad_us.store(Libraries::Kernel::sceKernelGetProcessTime(), std::memory_order_relaxed);
    }
}

void LatNoteGuestFlip() {
    if (g_lat_file) {
        std::scoped_lock lk{g_lat_mutex};
        g_lat_guest.emplace_back(Libraries::Kernel::sceKernelGetProcessTime(),
                                 g_lat_pad_us.load(std::memory_order_relaxed));
        if (g_lat_guest.size() > 64) {
            g_lat_guest.pop_front();
        }
    }
}

namespace {
std::unordered_map<Vulkan::Frame*, std::pair<u64, u64>> g_lat_frames;

void LatTagFrame(Vulkan::Frame* frame) {
    if (g_lat_file) {
        std::scoped_lock lk{g_lat_mutex};
        if (!g_lat_guest.empty()) {
            g_lat_frames[frame] = g_lat_guest.front();
            g_lat_guest.pop_front();
        }
    }
}

void LatDropFrame(Vulkan::Frame* frame) {
    if (g_lat_file) {
        std::scoped_lock lk{g_lat_mutex};
        g_lat_frames.erase(frame);
    }
}

void LatFramePresented(Vulkan::Frame* frame, u64 start) {
    if (!g_lat_file) {
        return;
    }
    const u64 end = Libraries::Kernel::sceKernelGetProcessTime();
    std::pair<u64, u64> guest{0, 0};
    size_t queued = 0;
    {
        std::scoped_lock lk{g_lat_mutex};
        if (const auto it = g_lat_frames.find(frame); it != g_lat_frames.end()) {
            guest = it->second;
            g_lat_frames.erase(it);
        }
        queued = g_lat_guest.size();
    }
    if (guest.first == 0) {
        return;
    }
    std::fprintf(
        g_lat_file, "%llu %llu %llu %llu %zu\n", static_cast<unsigned long long>(guest.first),
        static_cast<unsigned long long>(guest.second), static_cast<unsigned long long>(start),
        static_cast<unsigned long long>(end), queued);
    if (end - g_lat_flush_us > 1'000'000) {
        std::fflush(g_lat_file);
        g_lat_flush_us = end;
    }
}

s64 SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void VideoOutDriver::Flip(const Request& req, s64 vblank_ns) {
    if (req.index != -1) {
        LatTagFrame(req.frame);
    }
    if (present_pacing != 0) {
        QueueSwap({
            .kind = SwapKind::Game,
            .frame = req.frame,
            .hdr = req.port->is_hdr,
            .vblank_ns = vblank_ns,
            .not_before_ns = req.not_before_ns,
        });
    } else {
        // Update HDR status before presenting.
        presenter->SetHDR(req.port->is_hdr);

        // Present the frame.
        const u64 lat_start = g_lat_file ? Libraries::Kernel::sceKernelGetProcessTime() : 0;
        presenter->Present(req.frame);
        LatFramePresented(req.frame, lat_start);
    }

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    if (present_pacing != 0) {
        QueueSwap({.kind = SwapKind::Blank});
        return;
    }
    const auto empty_frame = presenter->PrepareBlankFrame(true);
    presenter->Present(empty_frame, false, false);
}

void VideoOutDriver::DrawLastFrame() {
    if (present_pacing != 0) {
        QueueSwap({.kind = SwapKind::Last});
        return;
    }
    const auto frame = presenter->PrepareLastFrame();
    if (frame != nullptr) {
        presenter->Present(frame, true);
    }
}

void VideoOutDriver::QueueSwap(const SwapRequest& req) {
    std::scoped_lock lk{swap_mutex};
    if (req.kind != SwapKind::Game) {
        // Redraws only fill time the swap thread would otherwise sit idle.
        if (swap_busy || !swap_requests.empty()) {
            return;
        }
    } else {
        // Frames whose time has come but are still waiting, because the compositor is holding the
        // previous present, are replaced by the newest one instead of queueing behind it.
        const s64 now = SteadyNs();
        std::erase_if(swap_requests, [&](const SwapRequest& queued) {
            if (queued.kind == SwapKind::Game && queued.not_before_ns > now) {
                return false;
            }
            if (queued.kind == SwapKind::Game) {
                LatDropFrame(queued.frame);
                presenter->DiscardFrame(queued.frame);
                ++swap_dropped;
            }
            return true;
        });
    }
    swap_requests.push_back(req);
    swap_cv.notify_one();
}

void VideoOutDriver::SwapThread(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:SwapThread");
    const bool phase_lock = present_pacing >= 2 && presenter->IsPresentWaitSupported();
    LOG_INFO(Lib_VideoOut, "Presenting on a separate thread, phase lock {}", phase_lock);
    while (true) {
        SwapRequest req;
        {
            std::unique_lock lk{swap_mutex};
            if (!swap_cv.wait(lk, token, [this] { return !swap_requests.empty(); })) {
                break;
            }
            req = swap_requests.front();
            swap_requests.pop_front();
            swap_busy = true;
        }
        const u64 lat_start = g_lat_file ? Libraries::Kernel::sceKernelGetProcessTime() : 0;
        switch (req.kind) {
        case SwapKind::Game:
            if (const s64 wait = req.not_before_ns - SteadyNs(); wait > 0) {
                std::this_thread::sleep_for(std::chrono::nanoseconds{wait});
            }
            presenter->SetHDR(req.hdr);
            presenter->Present(req.frame);
            LatFramePresented(req.frame, lat_start);
            break;
        case SwapKind::Blank:
            presenter->Present(presenter->PrepareBlankFrame(true), false, false);
            break;
        case SwapKind::Last:
            if (const auto frame = presenter->PrepareLastFrame(); frame != nullptr) {
                presenter->Present(frame, true);
            }
            break;
        }
        if (phase_lock && req.kind == SwapKind::Game && req.vblank_ns != 0 &&
            presenter->WaitForPresent(100'000'000)) {
            TrackDisplay(req.vblank_ns, SteadyNs());
        }
        std::scoped_lock lk{swap_mutex};
        swap_busy = false;
    }
    std::scoped_lock lk{swap_mutex};
    for (const auto& queued : swap_requests) {
        if (queued.kind == SwapKind::Game) {
            presenter->DiscardFrame(queued.frame);
        }
    }
}

void VideoOutDriver::TrackDisplay(s64 vblank_ns, s64 display_ns) {
    // A frame flipped on a vblank reaches the display on the first refresh it is ready for. The
    // delay is shortest when it is ready just before that refresh, and a refresh longer when it
    // just misses it. Walk the vblanks later until a frame misses: the frame before the miss
    // gives the shortest delay that still makes it. Then hold the delay a margin above that,
    // widening the margin after each miss and narrowing it slowly while none happen.
    constexpr s64 Ms = 1'000'000;
    const s64 delay = display_ns - vblank_ns;
    const s64 prev = std::exchange(last_display_delay_ns, delay);
    if (prev == 0) {
        display_margin_ns = 2 * Ms;
        last_miss_ns = display_ns;
        return;
    }
    if (delay > prev + 6 * Ms) {
        display_floor_ns = display_floor_ns == 0 ? prev : std::min(display_floor_ns, prev);
        display_margin_ns = std::min(display_margin_ns + Ms / 2, 6 * Ms);
        last_miss_ns = display_ns;
        ++display_misses;
    } else if (display_ns - last_miss_ns > 60'000 * Ms && display_margin_ns > 3 * Ms / 2) {
        display_margin_ns -= Ms / 4;
        last_miss_ns = display_ns;
    }
    const s64 error = display_floor_ns == 0 ? Ms : delay - (display_floor_ns + display_margin_ns);
    vblank_shift_ns.fetch_add(std::clamp(error / 4, -Ms / 2, Ms / 2), std::memory_order_relaxed);
    if (++display_samples % 300 == 0) {
        LOG_INFO(Lib_VideoOut,
                 "PHASELOCK delay={:.1f} floor={:.1f} margin={:.2f} misses={} dropped={} ms",
                 delay / 1e6, display_floor_ns / 1e6, display_margin_ns / 1e6, display_misses,
                 swap_dropped);
    }
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SetFlipRate(VideoOutPort* port, s32 rate) {
    const bool changed = port->flip_rate != rate;
    port->flip_rate = rate;
    if (changed) {
        LOG_INFO(Lib_VideoOut, "Flip rate {} (pacing {})", rate,
                 present_pacing != 0 && EmulatorSettings.GetFrameCap() == 0 ? "on" : "off");
    }
    if (!changed || present_pacing == 0 || EmulatorSettings.GetFrameCap() != 0) {
        return;
    }
    // The pacer shows frames at the game's flip rate, each just after it is done, instead of
    // queueing them for a later vblank.
    const s64 vblank_period_ns = 1'000'000'000 / EmulatorSettings.GetVblankFrequency();
    std::scoped_lock lk{cap_mutex};
    cap_from_flip_rate = rate > 0;
    cap_period_ns = rate > 0 ? vblank_period_ns * (rate + 1) : 0;
    cap_slot_ns = 0;
    cap_submitted = cap_ready;
}

void VideoOutDriver::PaceGuestFlip() {
    const s64 cap_period_ns = this->cap_period_ns.load(std::memory_order_relaxed);
    if (cap_period_ns == 0) {
        return;
    }
    const s64 now = SteadyNs();
    s64 release;
    {
        std::scoped_lock lk{cap_mutex};
        // The next frame's slot comes after those of the frames still in flight. Start it a lead
        // before that slot, the lead being learned from how early frames have been ready.
        const s64 in_flight = static_cast<s64>(cap_submitted + 1 - cap_ready);
        const s64 slot = cap_slot_ns + (in_flight + 1) * cap_period_ns;
        release = std::clamp(slot - cap_lead_ns, now, now + cap_period_ns);
        ++cap_submitted;
    }
    if (release > now) {
        std::this_thread::sleep_for(std::chrono::nanoseconds{release - now});
    }
}

s64 VideoOutDriver::NoteFrameReady() {
    const s64 cap_period_ns = this->cap_period_ns.load(std::memory_order_relaxed);
    if (cap_period_ns == 0) {
        return 0;
    }
    constexpr s64 TargetEarly = 2'000'000;
    const s64 now = SteadyNs();
    std::scoped_lock lk{cap_mutex};
    ++cap_ready;
    if (cap_slot_ns == 0) {
        cap_slot_ns = now;
        cap_lead_ns = cap_period_ns;
        return now;
    }
    // A frame is shown on its slot, one period after the last one. One that is ready late is
    // shown when ready and moves the slots with it; one more than half a period late restarts
    // them.
    const s64 slot = cap_slot_ns + cap_period_ns;
    const s64 early = slot - now;
    if (early < 0) {
        cap_lead_ns += (TargetEarly - early) / 2;
    } else {
        cap_lead_ns += (TargetEarly - early) / 8;
    }
    cap_lead_ns = std::clamp<s64>(cap_lead_ns, 0, 3 * cap_period_ns);
    cap_slot_ns = early < -cap_period_ns / 2 ? now : std::max(slot, now);
    return cap_slot_ns;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    const s64 not_before_ns = NoteFrameReady();
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    {
        std::scoped_lock lock{mutex};
        requests.push({
            .frame = frame,
            .port = port,
            .flip_arg = flip_arg,
            .index = index,
            .eop = is_eop,
            .not_before_ns = not_before_ns,
        });
    }
    request_cv.notify_one();
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};

    // A frame that misses its vblank is shown as soon as a whole vblank period has passed since
    // the last flip, rather than held for the next vblank. Holding it made a frame that took a
    // little over a period show for two, so frames alternated between 16 and 33 ms, and the GPU
    // thread sat waiting for the game's next buffer to be released for up to a tenth of the time.
    // Vblanks themselves keep their pace, and frames are still shown at most once per period.
    // Waits on the condition variable are only accurate to about a millisecond, so this stops
    // a little before each vblank to keep it on time.
    constexpr auto EarlyFlipMargin = std::chrono::milliseconds{2};
    // A frame shown early is shown for at least this long before a vblank flips the next one.
    const auto min_flip_interval = vblank_period * 3 / 4;
    auto last_flip = std::chrono::steady_clock::now();

    // Flips the pacer shows at the game's flip rate are taken as they come, like those of a game
    // without one.
    const auto vblank_flip_rate = [this] {
        return cap_from_flip_rate.load(std::memory_order_relaxed) ? 0 : main_port.flip_rate;
    };

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    const auto flip_late_frames = [&] {
        const auto wait_start = std::chrono::steady_clock::now();
        const auto wait_end = wait_start + timer.GetTotalWait() - EarlyFlipMargin;
        std::unique_lock lk{mutex};
        while (vblank_flip_rate() == 0 && !DebugState.IsGuestThreadsPaused()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= wait_end) {
                break;
            }
            if (requests.empty()) {
                request_cv.wait_until(lk, wait_end);
                continue;
            }
            const auto due = last_flip + vblank_period;
            if (now < due) {
                request_cv.wait_until(lk, std::min(due, wait_end));
                continue;
            }
            const auto request = requests.front();
            requests.pop();
            lk.unlock();
            Flip(request);
            FRAME_END;
            last_flip = std::chrono::steady_clock::now();
            lk.lock();
        }
        lk.unlock();
        timer.Skip(std::chrono::steady_clock::now() - wait_start);
    };

    while (!token.stop_requested()) {
        flip_late_frames();
        timer.Start();

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        if (vblank_status.count % (vblank_flip_rate() + 1) == 0) {
            const bool flipped_recently =
                std::chrono::steady_clock::now() - last_flip < min_flip_interval;
            const auto request = flipped_recently ? Request{} : receive_request();
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request, vblank_flip_rate() != 0 ? SteadyNs() : 0);
                FRAME_END;
                last_flip = std::chrono::steady_clock::now();
            }
        }

        // Phase lock: move the vblanks toward the display's refreshes a little at a time.
        if (const s64 shift = vblank_shift_ns.load(std::memory_order_relaxed); shift != 0) {
            constexpr s64 MaxStep = 500'000;
            const s64 step = std::clamp(shift, -MaxStep, MaxStep);
            vblank_shift_ns.fetch_sub(step, std::memory_order_relaxed);
            timer.Skip(std::chrono::nanoseconds{-step});
        }

        {
            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};

            // Trigger flip events for the port
            for (auto event : main_port.vblank_events) {
                auto equeue = Kernel::GetEqueue(event);
                if (equeue != nullptr) {
                    equeue->TriggerEvent(
                        static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                        Kernel::OrbisKernelEvent::Filter::VideoOut,
                        reinterpret_cast<void*>(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                            (vblank_status.count << 16)));
                }
            }

            // Update vblank status
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }

        timer.End();
    }
}

} // namespace Libraries::VideoOut
