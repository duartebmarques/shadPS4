// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/assert.h"
#include "common/debug.h"
#include "common/rdtsc.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/buffer_cache/stream_copy_lane.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_record_thread.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/skipcache/skipcache.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;
std::mutex Scheduler::queue_mutex;

namespace {
struct QueuedSubmit {
    std::vector<vk::CommandBuffer> cmd_buffers;
    vk::Semaphore timeline;
    u64 tick;
};

struct SubmitWorker {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<QueuedSubmit> queue;
    bool busy{};
    std::jthread thread;
};

SubmitWorker g_submit_worker;

void SubmitThread(std::stop_token stoken, vk::Queue queue) {
    Common::SetCurrentThreadName("shadPS4:GpuSubmit");
    auto& w = g_submit_worker;
    while (true) {
        QueuedSubmit job;
        {
            std::unique_lock lk{w.mutex};
            if (!w.cv.wait(lk, stoken, [&] { return !w.queue.empty(); })) {
                return;
            }
            job = std::move(w.queue.front());
            w.queue.pop_front();
            w.busy = true;
        }
        const vk::TimelineSemaphoreSubmitInfo timeline_si = {
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &job.tick,
        };
        const vk::SubmitInfo submit_info = {
            .pNext = &timeline_si,
            .commandBufferCount = static_cast<u32>(job.cmd_buffers.size()),
            .pCommandBuffers = job.cmd_buffers.data(),
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &job.timeline,
        };
        {
            std::scoped_lock queue_lk{Scheduler::queue_mutex};
            ImGui::Core::TextureManager::Submit();
            const auto result = queue.submit(submit_info);
            ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during submit");
        }
        {
            std::scoped_lock lk{w.mutex};
            w.busy = false;
        }
        w.cv.notify_all();
    }
}
} // Anonymous namespace

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore},
      threaded_recording{threaded_recording} {
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    pop_poll_throttle_ = std::max<u32>(EmulatorSettings.GetPendingPopThrottle(), 1u);
    tick_query_interval_ns_ = u64{EmulatorSettings.GetTickQueryIntervalUs()} * 1000;
    if (threaded_recording) {
        record_thread = std::make_unique<RecordThread>(instance, work_semaphore);
        chunk = record_thread->AcquireChunk();
    }
    BeginSession();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (async_submit_) {
        g_submit_worker.thread.request_stop();
        g_submit_worker.thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    ++rs_calls_;
#ifndef NDEBUG
    // While the colour tail is still zeroed by every producer, the narrowed
    // relation and the whole-struct one must agree exactly.
    DEBUG_ASSERT((render_state == new_state) ==
                 (std::memcmp(&render_state, &new_state, sizeof(RenderState)) == 0));
#endif
    rs_interrupted_ += !is_rendering;
    if (is_rendering && render_state == new_state) {
        return;
    }
    ++rs_restarts_;
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    CommandBuffer().beginRendering(rendering_info);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

vk::CommandBuffer Scheduler::UploadCommandBuffer() {
    ASSERT_MSG(!threaded_recording, "The recording thread owns the command buffers");
    auto& upload_cmdbuf = sessions.back().upload;
    if (upload_cmdbuf) {
        return upload_cmdbuf;
    }
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    upload_cmdbuf = command_pool.Commit();
    Check(upload_cmdbuf.begin(begin_info));
    return upload_cmdbuf;
}

void Scheduler::EnableAsyncSubmit() {
    async_submit_ = true;
    g_submit_worker.thread = std::jthread(SubmitThread, instance.GetGraphicsQueue());
}

void Scheduler::DrainSubmits() {
    auto& w = g_submit_worker;
    std::unique_lock lk{w.mutex};
    w.cv.wait(lk, [&] { return w.queue.empty() && !w.busy; });
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    const u64 t0 = Common::FencedRDTSC();
    Wait(presubmit_tick);
    RecordWait(WaitSite::Finish, Common::FencedRDTSC() - t0);
}

u64 Scheduler::WaitTagged(u64 tick, WaitSite site) {
    if (work_semaphore.IsFree(tick)) {
        Wait(tick); // still needs the flush path; it will not block
        return 0;
    }
    const u64 t0 = Common::FencedRDTSC();
    Wait(tick);
    const u64 blocked = Common::FencedRDTSC() - t0;
    RecordWait(site, blocked);
    return blocked;
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    if (threaded_recording) {
        if (const u64 blocked = record_thread->WaitSubmitted(tick)) {
            RecordWait(WaitSite::RecordDrain, blocked);
        }
    }
    work_semaphore.Wait(tick);
}

void Scheduler::WaitSubmitted(u64 tick) {
    if (threaded_recording) {
        record_thread->WaitSubmitted(tick);
    }
}

void Scheduler::ResetCommandBuffer() {
    if (threaded_recording) {
        record_thread->ResetCommandBuffer();
        return;
    }
    Check(RawCommandBuffer().reset());
}

void Scheduler::DispatchChunk(const SubmitRequest* request) {
    if (const u64 blocked = record_thread->Dispatch(std::move(chunk), request)) {
        RecordWait(WaitSite::RecordDrain, blocked);
    }
    chunk = record_thread->AcquireChunk();
}

void Scheduler::PopPendingOperations() {
    // Callers poll at draw rate and the queue is empty in steady state; the
    // count gate spares those polls the lock and the semaphore query ioctl.
    // Waiters needing a fresher tick refresh on their own miss paths.
    if (pending_ops_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    // While an op waits out its GPU tick the queue stays non-empty and every
    // draw would otherwise take the lock and issue a SYNCOBJ_QUERY ioctl;
    // the ops polled here are latency-tolerant (deferred frees; writebacks
    // that must land first go through DrainPendingOperations), so attempt the
    // pop once per N non-empty polls instead. An op retires at most N draws
    // late. A 0 setting latches as 1.
    if (++pop_poll_counter_ < pop_poll_throttle_) {
        return;
    }
    pop_poll_counter_ = 0;
    DrainPendingOperations();
}

void Scheduler::DrainPendingOperations() {
    if (pending_ops_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::unique_lock lk(pending_ops_mutex);
    work_semaphore.Refresh();
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        pending_ops_count.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::BeginSession() {
    if (threaded_recording) {
        // The record thread begins the next command buffer itself; only the dynamic state
        // needs re-emitting into it.
        dynamic_state.Invalidate();
        return;
    }
    EndSession();

    auto& session = sessions.emplace_back();

    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    session.primary = command_pool.Commit();
    Check(session.primary.begin(begin_info));
    session.seq = ++session_seq;
    if (span_pool) {
        session.span_slot = span_next;
        span_next = (span_next + 1) % TimestampSlots;
        session.primary.resetQueryPool(span_pool, session.span_slot * 2, 2);
        session.primary.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, span_pool,
                                        session.span_slot * 2);
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::EndSession() {
    if (sessions.empty()) {
        return;
    }

    if (on_session) {
        on_session();
    }

    const auto& session = sessions.back();
    if (session.upload) {
        Check(session.upload.end());
    }

    EndRendering();
    // Every batch ends with a device-to-host memory barrier, so the host may read the GPU writes
    // of a batch out of a host visible arena once its tick passes.
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    session.primary.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
    if (session.span_slot != UINT32_MAX) {
        session.primary.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, span_pool,
                                        session.span_slot * 2 + 1);
    }
    Check(session.primary.end());
}

Scheduler::GpuTimestamp Scheduler::WriteGpuTimestamp() {
    if (!timestamp_pool) {
        const vk::Device device = instance.GetDevice();
        timestamp_period = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        timestamp_pool = Check(device.createQueryPool({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = TimestampSlots,
        }));
        device.resetQueryPool(timestamp_pool, 0, TimestampSlots);
        span_pool = Check(device.createQueryPool({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = TimestampSlots * 2,
        }));
    }
    const u32 slot = timestamp_next;
    timestamp_next = (timestamp_next + 1) % TimestampSlots;
    CommandBuffer().writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, timestamp_pool,
                                    slot);
    return {slot, sessions.back().seq};
}

void Scheduler::AccumulateBusy(u64 session_limit, u64 tick_limit) {
    const vk::Device device = instance.GetDevice();
    while (!busy_spans.empty() && busy_spans.front().session < session_limit &&
           busy_spans.front().tick <= tick_limit) {
        std::array<u64, 2> ts{};
        if (device.getQueryPoolResults(span_pool, busy_spans.front().slot * 2, 2, sizeof(ts),
                                       ts.data(), sizeof(u64),
                                       vk::QueryResultFlagBits::e64) != vk::Result::eSuccess) {
            break;
        }
        busy_spans.pop_front();
        const u64 start = std::max(ts[0], busy_prev_end);
        if (ts[1] > start) {
            busy_ticks += ts[1] - start;
            busy_prev_end = ts[1];
        }
    }
}

u64 Scheduler::ReadGpuBusyNs(const GpuTimestamp& timestamp) {
    const vk::Device device = instance.GetDevice();
    u64 ts{};
    const auto result =
        device.getQueryPoolResults(timestamp_pool, timestamp.slot, 1, sizeof(ts), &ts, sizeof(ts),
                                   vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
    device.resetQueryPool(timestamp_pool, timestamp.slot, 1);

    std::scoped_lock lk{busy_mutex};
    AccumulateBusy(timestamp.session, UINT64_MAX);
    u64 busy = busy_ticks;
    if (result == vk::Result::eSuccess && !busy_spans.empty() &&
        busy_spans.front().session == timestamp.session) {
        // The part of the timestamp's own session that ran before it.
        u64 start{};
        if (device.getQueryPoolResults(span_pool, busy_spans.front().slot * 2, 1, sizeof(start),
                                       &start, sizeof(start),
                                       vk::QueryResultFlagBits::e64) == vk::Result::eSuccess) {
            start = std::max(start, busy_prev_end);
            busy += ts > start ? ts - start : 0;
        }
    }
    return static_cast<u64>(static_cast<double>(busy) * timestamp_period);
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    // The record thread takes submit_mutex for its queue submit; holding it here while a
    // Record() below waits on the full queue would deadlock.
    std::unique_lock lk{submit_mutex, std::defer_lock};
    if (!threaded_recording) {
        lk.lock();
    }
    // Every stream-lane byte this command buffer reads must be in place
    // before the queue submit.
    VideoCore::StreamCopyLane::Instance().DrainProducer();
    const u64 signal_value = work_semaphore.NextTick();
    // Cmdbuf rollover: mid-draw submits (stream wraparound flushes) must
    // synchronously invalidate all skip caches - a draw-entry token snapshot
    // cannot see this flush.
    VideoCore::Skipcache::Framework::Instance().InvalidateAll();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    if (threaded_recording) {
        EndRendering();
        info.AddSignal(work_semaphore.Handle(), signal_value);
        const SubmitRequest request{
            .info = info,
            .signal_tick = signal_value,
        };
        DispatchChunk(&request);
        work_semaphore.Refresh();
        BeginSession();
        PopPendingOperations();
        return;
    }

    EndSession();

    std::vector<vk::CommandBuffer> cmd_buffers;
    cmd_buffers.reserve(sessions.size() * 2);

    for (const auto& session : sessions) {
        if (session.upload) {
            cmd_buffers.push_back(session.upload);
        }
        cmd_buffers.push_back(session.primary);
    }
    if (span_pool) {
        std::scoped_lock busy_lk{busy_mutex};
        // Span slots are reused TimestampSlots sessions later: retire finished ones first.
        if (busy_spans.size() >= TimestampSlots / 2) {
            work_semaphore.Refresh();
            AccumulateBusy(UINT64_MAX, work_semaphore.KnownGpuTick());
        }
        for (const auto& session : sessions) {
            if (session.span_slot != UINT32_MAX) {
                busy_spans.push_back({session.seq, signal_value, session.span_slot});
            }
        }
    }
    sessions.clear();

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    std::array<vk::PipelineStageFlags, std::tuple_size_v<decltype(info.wait_semas)>>
        wait_stage_masks;
    wait_stage_masks.fill(vk::PipelineStageFlagBits::eAllCommands);

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = static_cast<u32>(cmd_buffers.size()),
        .pCommandBuffers = cmd_buffers.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    if (async_submit_ && info.num_wait_semas == 0 && info.num_signal_semas == 1 && !info.fence) {
        {
            std::scoped_lock lk{g_submit_worker.mutex};
            g_submit_worker.queue.push_back({std::move(cmd_buffers), timeline, signal_value});
        }
        g_submit_worker.cv.notify_all();
    } else {
        DrainSubmits();
        std::scoped_lock queue_lk{queue_mutex};
        ImGui::Core::TextureManager::Submit();
        auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
        ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    }

    work_semaphore.Refresh();
    BeginSession();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

namespace {

// The dynamic state setters of one Commit, stored for replay on the record thread. The method
// names mirror vk::CommandBuffer, so Commit's decision code instantiates against either.
struct DynamicStateSnapshot {
    enum Emit : u32 {
        EmitViewports = 1u << 0,
        EmitScissors = 1u << 1,
        EmitDepthTestEnable = 1u << 2,
        EmitDepthWriteEnable = 1u << 3,
        EmitDepthCompareOp = 1u << 4,
        EmitDepthBoundsTestEnable = 1u << 5,
        EmitDepthBounds = 1u << 6,
        EmitDepthBiasEnable = 1u << 7,
        EmitDepthBias = 1u << 8,
        EmitStencilTestEnable = 1u << 9,
        EmitStencilOp = 1u << 10,
        EmitStencilReference = 1u << 11,
        EmitStencilWriteMask = 1u << 12,
        EmitStencilCompareMask = 1u << 13,
        EmitPrimitiveRestartEnable = 1u << 14,
        EmitRasterizerDiscardEnable = 1u << 15,
        EmitCullMode = 1u << 16,
        EmitFrontFace = 1u << 17,
        EmitBlendConstants = 1u << 18,
        EmitColorWriteMasks = 1u << 19,
        EmitLineWidth = 1u << 20,
        EmitFeedbackLoop = 1u << 21,
    };
    struct StencilOpCall {
        vk::StencilFaceFlags face_mask;
        StencilOps ops;
    };
    struct StencilValueCall {
        vk::StencilFaceFlags face_mask;
        u32 value;
    };

    u32 emit{};
    u32 num_viewports{};
    u32 num_scissors{};
    u32 num_stencil_ops{};
    u32 num_stencil_references{};
    u32 num_stencil_write_masks{};
    u32 num_stencil_compare_masks{};
    std::array<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports{};
    std::array<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors{};
    std::array<StencilOpCall, 2> stencil_ops{};
    std::array<StencilValueCall, 2> stencil_references{};
    std::array<StencilValueCall, 2> stencil_write_masks{};
    std::array<StencilValueCall, 2> stencil_compare_masks{};
    vk::Bool32 depth_test_enable{};
    vk::Bool32 depth_write_enable{};
    vk::CompareOp depth_compare_op{};
    vk::Bool32 depth_bounds_test_enable{};
    float min_depth_bounds{};
    float max_depth_bounds{};
    vk::Bool32 depth_bias_enable{};
    float depth_bias_constant_factor{};
    float depth_bias_clamp{};
    float depth_bias_slope_factor{};
    vk::Bool32 stencil_test_enable{};
    vk::Bool32 primitive_restart_enable{};
    vk::Bool32 rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};
    std::array<float, 4> blend_constants{};
    u32 first_color_attachment{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    vk::ImageAspectFlags feedback_loop_aspect{};

    void setViewportWithCount(const Viewports& viewports_) {
        num_viewports = static_cast<u32>(viewports_.size());
        std::ranges::copy(viewports_, viewports.begin());
        emit |= EmitViewports;
    }
    void setScissorWithCount(const Scissors& scissors_) {
        num_scissors = static_cast<u32>(scissors_.size());
        std::ranges::copy(scissors_, scissors.begin());
        emit |= EmitScissors;
    }
    void setDepthTestEnable(vk::Bool32 enable) {
        depth_test_enable = enable;
        emit |= EmitDepthTestEnable;
    }
    void setDepthWriteEnable(vk::Bool32 enable) {
        depth_write_enable = enable;
        emit |= EmitDepthWriteEnable;
    }
    void setDepthCompareOp(vk::CompareOp compare_op) {
        depth_compare_op = compare_op;
        emit |= EmitDepthCompareOp;
    }
    void setDepthBoundsTestEnable(vk::Bool32 enable) {
        depth_bounds_test_enable = enable;
        emit |= EmitDepthBoundsTestEnable;
    }
    void setDepthBounds(float min_bounds, float max_bounds) {
        min_depth_bounds = min_bounds;
        max_depth_bounds = max_bounds;
        emit |= EmitDepthBounds;
    }
    void setDepthBiasEnable(vk::Bool32 enable) {
        depth_bias_enable = enable;
        emit |= EmitDepthBiasEnable;
    }
    void setDepthBias(float constant_factor, float clamp, float slope_factor) {
        depth_bias_constant_factor = constant_factor;
        depth_bias_clamp = clamp;
        depth_bias_slope_factor = slope_factor;
        emit |= EmitDepthBias;
    }
    void setStencilTestEnable(vk::Bool32 enable) {
        stencil_test_enable = enable;
        emit |= EmitStencilTestEnable;
    }
    void setStencilOp(vk::StencilFaceFlags face_mask, vk::StencilOp fail_op, vk::StencilOp pass_op,
                      vk::StencilOp depth_fail_op, vk::CompareOp compare_op) {
        stencil_ops[num_stencil_ops++] = {face_mask, {fail_op, pass_op, depth_fail_op, compare_op}};
        emit |= EmitStencilOp;
    }
    void setStencilReference(vk::StencilFaceFlags face_mask, u32 reference) {
        stencil_references[num_stencil_references++] = {face_mask, reference};
        emit |= EmitStencilReference;
    }
    void setStencilWriteMask(vk::StencilFaceFlags face_mask, u32 write_mask) {
        stencil_write_masks[num_stencil_write_masks++] = {face_mask, write_mask};
        emit |= EmitStencilWriteMask;
    }
    void setStencilCompareMask(vk::StencilFaceFlags face_mask, u32 compare_mask) {
        stencil_compare_masks[num_stencil_compare_masks++] = {face_mask, compare_mask};
        emit |= EmitStencilCompareMask;
    }
    void setPrimitiveRestartEnable(vk::Bool32 enable) {
        primitive_restart_enable = enable;
        emit |= EmitPrimitiveRestartEnable;
    }
    void setRasterizerDiscardEnable(vk::Bool32 enable) {
        rasterizer_discard_enable = enable;
        emit |= EmitRasterizerDiscardEnable;
    }
    void setCullMode(vk::CullModeFlags cull_mode_) {
        cull_mode = cull_mode_;
        emit |= EmitCullMode;
    }
    void setFrontFace(vk::FrontFace front_face_) {
        front_face = front_face_;
        emit |= EmitFrontFace;
    }
    void setBlendConstants(const float* constants) {
        std::copy_n(constants, blend_constants.size(), blend_constants.begin());
        emit |= EmitBlendConstants;
    }
    void setColorWriteMaskEXT(u32 first_attachment, const ColorWriteMasks& masks) {
        first_color_attachment = first_attachment;
        color_write_masks = masks;
        emit |= EmitColorWriteMasks;
    }
    void setLineWidth(float width) {
        line_width = width;
        emit |= EmitLineWidth;
    }
    void setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags aspect_mask) {
        feedback_loop_aspect = aspect_mask;
        emit |= EmitFeedbackLoop;
    }

    /// Issues the stored setters in Commit's order.
    void Replay(vk::CommandBuffer cmdbuf) const {
        if (emit & EmitViewports) {
            cmdbuf.setViewportWithCount(
                vk::ArrayProxy<const vk::Viewport>(num_viewports, viewports.data()));
        }
        if (emit & EmitScissors) {
            cmdbuf.setScissorWithCount(
                vk::ArrayProxy<const vk::Rect2D>(num_scissors, scissors.data()));
        }
        if (emit & EmitDepthTestEnable) {
            cmdbuf.setDepthTestEnable(depth_test_enable);
        }
        if (emit & EmitDepthWriteEnable) {
            cmdbuf.setDepthWriteEnable(depth_write_enable);
        }
        if (emit & EmitDepthCompareOp) {
            cmdbuf.setDepthCompareOp(depth_compare_op);
        }
        if (emit & EmitDepthBoundsTestEnable) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enable);
        }
        if (emit & EmitDepthBounds) {
            cmdbuf.setDepthBounds(min_depth_bounds, max_depth_bounds);
        }
        if (emit & EmitDepthBiasEnable) {
            cmdbuf.setDepthBiasEnable(depth_bias_enable);
        }
        if (emit & EmitDepthBias) {
            cmdbuf.setDepthBias(depth_bias_constant_factor, depth_bias_clamp,
                                depth_bias_slope_factor);
        }
        if (emit & EmitStencilTestEnable) {
            cmdbuf.setStencilTestEnable(stencil_test_enable);
        }
        for (u32 i = 0; i < num_stencil_ops; ++i) {
            const auto& [face_mask, ops] = stencil_ops[i];
            cmdbuf.setStencilOp(face_mask, ops.fail_op, ops.pass_op, ops.depth_fail_op,
                                ops.compare_op);
        }
        for (u32 i = 0; i < num_stencil_references; ++i) {
            cmdbuf.setStencilReference(stencil_references[i].face_mask,
                                       stencil_references[i].value);
        }
        for (u32 i = 0; i < num_stencil_write_masks; ++i) {
            cmdbuf.setStencilWriteMask(stencil_write_masks[i].face_mask,
                                       stencil_write_masks[i].value);
        }
        for (u32 i = 0; i < num_stencil_compare_masks; ++i) {
            cmdbuf.setStencilCompareMask(stencil_compare_masks[i].face_mask,
                                         stencil_compare_masks[i].value);
        }
        if (emit & EmitPrimitiveRestartEnable) {
            cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
        }
        if (emit & EmitRasterizerDiscardEnable) {
            cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
        }
        if (emit & EmitCullMode) {
            cmdbuf.setCullMode(cull_mode);
        }
        if (emit & EmitFrontFace) {
            cmdbuf.setFrontFace(front_face);
        }
        if (emit & EmitBlendConstants) {
            cmdbuf.setBlendConstants(blend_constants.data());
        }
        if (emit & EmitColorWriteMasks) {
            cmdbuf.setColorWriteMaskEXT(first_color_attachment, color_write_masks);
        }
        if (emit & EmitLineWidth) {
            cmdbuf.setLineWidth(line_width);
        }
        if (emit & EmitFeedbackLoop) {
            cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_aspect);
        }
    }
};
static_assert(std::is_trivially_copyable_v<DynamicStateSnapshot>);

} // namespace

void DynamicState::Commit(const Instance& instance, Scheduler& scheduler) {
    if (!scheduler.HasRecordingThread()) {
        const vk::CommandBuffer cmdbuf = scheduler.RawCommandBuffer();
        CommitTo(instance, cmdbuf);
        return;
    }
    DynamicStateSnapshot snapshot{};
    CommitTo(instance, snapshot);
    if (snapshot.emit) {
        scheduler.Record([snapshot](vk::CommandBuffer cmdbuf) { snapshot.Replay(cmdbuf); });
    }
}

template <typename CmdBuf>
void DynamicState::CommitTo(const Instance& instance, CmdBuf& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskEnabled()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        } else {
            ++color_write_mask_skips_;
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
