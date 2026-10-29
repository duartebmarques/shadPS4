// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "core/libraries/videoout/video_out.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <queue>

namespace Vulkan {
struct Frame;
}

namespace Libraries::VideoOut {

void LatNotePadRead();
void LatNoteGuestFlip();

/// Holds the calling guest thread after a flip for the frame cap.
void PaceGuestFrame();

/// Notes a submit-and-flip before it is submitted, and holds the guest after it so that its flips
/// reach the GPU command thread flip_lead_us after they are submitted.
void NoteQueuedFlip();
void WaitQueuedFlips();
/// The GPU command thread ran out of work: a guest held for the flip lead is let go.
void NoteGpuIdle();

struct VideoOutPort {
    SceVideoOutResolutionStatus resolution;
    std::array<VideoOutBuffer, MaxDisplayBuffers> buffer_slots;
    std::array<u64, MaxDisplayBuffers> buffer_labels; // should be contiguous in memory
    static_assert(sizeof(buffer_labels[0]) == 8u);
    std::array<BufferAttributeGroup, MaxDisplayBufferGroups> groups;
    FlipStatus flip_status;
    SceVideoOutVblankStatus vblank_status;
    std::vector<Kernel::OrbisKernelEqueue> flip_events;
    std::vector<Kernel::OrbisKernelEqueue> vblank_events;
    std::mutex vo_mutex;
    std::mutex port_mutex;
    std::condition_variable vo_cv;
    std::condition_variable vblank_cv;
    int flip_rate = 0;
    int prev_index = -1;
    bool is_open = false;
    bool is_hdr = false;

    s32 FindFreeGroup() const {
        s32 index = 0;
        while (index < groups.size() && groups[index].is_occupied) {
            index++;
        }
        return index;
    }

    bool IsVoLabel(const u64* address) const {
        const u64* start = &buffer_labels[0];
        const u64* end = &buffer_labels[MaxDisplayBuffers - 1];
        return address >= start && address <= end;
    }

    void WaitVoLabel(auto&& pred) {
        std::unique_lock lk{vo_mutex};
        vo_cv.wait(lk, pred);
    }

    void SignalVoLabel() {
        std::scoped_lock lk{vo_mutex};
        vo_cv.notify_one();
    }

    [[nodiscard]] int NumRegisteredBuffers() const {
        return std::count_if(buffer_slots.cbegin(), buffer_slots.cend(),
                             [](auto& buffer) { return buffer.group_index != -1; });
    }
};

struct ServiceThreadParams {
    u32 unknown;
    bool set_priority;
    u32 priority;
    bool set_affinity;
    u64 affinity;
};

class VideoOutDriver {
public:
    VideoOutDriver(u32 width, u32 height);
    ~VideoOutDriver();

    int Open(const ServiceThreadParams* params);
    void Close(s32 handle);

    VideoOutPort* GetPort(s32 handle);

    int RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses, s32 bufferNum,
                        const BufferAttribute* attribute);
    int UnregisterBuffers(VideoOutPort* port, s32 attributeIndex);
    int ChangeBufferAttribute(VideoOutPort* port, s32 bufferIndex,
                              const BufferAttribute* attribute);

    bool SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);
    void NoteQueuedFlip();
    void WaitQueuedFlips();
    void NoteGpuIdle();

    /// Called by the guest thread after it queued a flip: holds it until its next frame should
    /// start for the frame cap.
    void PaceGuestFlip();

    void SetFlipRate(VideoOutPort* port, s32 rate);

private:
    struct Request {
        Vulkan::Frame* frame;
        VideoOutPort* port;
        s64 flip_arg;
        s32 index;
        bool eop;
        s64 not_before_ns; // Frame cap slot

        operator bool() const noexcept {
            return frame != nullptr;
        }
    };

    enum class SwapKind {
        Game,  // A guest flip, dropped if a newer one arrives before it is presented
        Blank, // Video port out not open
        Last,  // Used when there is no flip request
    };

    struct SwapRequest {
        SwapKind kind;
        Vulkan::Frame* frame;
        bool hdr;
        s64 vblank_ns;     // Vblank the flip was made on, 0 if it was flipped between vblanks
        s64 not_before_ns; // Frame cap slot, the present is held until then
    };

    void Flip(const Request& req, s64 vblank_ns = 0);
    void DrawBlankFrame(); // Video port out not open
    void DrawLastFrame();  // Used when there is no flip request
    void SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);
    void PresentThread(std::stop_token token);
    void QueueSwap(const SwapRequest& req);
    void SwapThread(std::stop_token token);
    void TrackDisplay(s64 vblank_ns, s64 display_ns);
    s64 NoteFrameReady();
    void NoteFlipReached();

    std::mutex mutex;
    std::condition_variable request_cv;
    VideoOutPort main_port{};
    std::queue<Request> requests;
    u32 present_pacing{};
    std::mutex swap_mutex;
    std::condition_variable_any swap_cv;
    std::deque<SwapRequest> swap_requests;
    bool swap_busy{};
    u64 swap_dropped{};
    // Phase lock: how far the vblanks should move later, from the time frames reach the display.
    std::atomic<s64> vblank_shift_ns{};
    s64 display_floor_ns{};
    s64 display_margin_ns{};
    s64 last_display_delay_ns{};
    s64 last_miss_ns{};
    u64 display_misses{};
    u64 display_samples{};
    // Frame cap: the display slot of the last frame and how long before its slot a frame is
    // started. Without a configured cap, a game's flip rate sets it.
    std::atomic<s64> cap_period_ns{};
    std::atomic<bool> cap_from_flip_rate{};
    std::mutex cap_mutex;
    s64 cap_slot_ns{};
    s64 cap_lead_ns{};
    u64 cap_submitted{};
    // Flip lead: submit times of the guest flips the command thread has not reached yet, and how
    // long the guest is held after each flip.
    s64 flip_lead_ns{};
    std::mutex flip_lead_mutex;
    std::deque<s64> flip_lead_submits;
    s64 flip_lead_hold_ns{};
    std::condition_variable flip_lead_cv;
    bool flip_lead_holding{};
    u64 flip_lead_starved{};
    s64 flip_lead_lag_sum_ns{};
    u64 flip_lead_samples{};
    u64 cap_ready{};
    std::jthread present_thread;
    std::jthread swap_thread;
};

} // namespace Libraries::VideoOut
