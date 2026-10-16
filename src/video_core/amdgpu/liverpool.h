// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include <deque>

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "core/platform.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/gfx_state_stamp.h"
#include "video_core/amdgpu/regs.h"

namespace Vulkan {
class Rasterizer;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

enum class DataSelect : u32;

struct PM4CmdWaitRegMem;
struct PM4CmdWriteData;

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    u32 index_buffer_num_indices{};
    GfxStateStamp gfx_stamp{};
    // One bit per context and uconfig register word the dynamic-state
    // updaters read; the stamp's dyn lane classifies writes against it.
    std::array<u64, (Regs::NumRegs - Regs::ContextRegWordOffset) / 64> dyn_reg_mask_{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};
    /// Draw pipe (vk_draw_pipe.h): the 32-word register blocks written since the last packet
    /// handed to the recording thread, and whether a CLEAR_STATE reset the file in between.
    static constexpr u32 PipeBlockWords = 32;
    std::array<u64, Regs::NumRegs / PipeBlockWords / 64> pipe_dirty{};
    bool pipe_reset{};
    bool draw_pipe{};

    void MarkPipeDirty(u32 word, u32 count) {
        if (!draw_pipe || count == 0) {
            return;
        }
        const u32 last = (word + count - 1) / PipeBlockWords;
        for (u32 block = word / PipeBlockWords; block <= last; ++block) {
            pipe_dirty[block / 64] |= u64{1} << (block % 64);
        }
    }
    template <typename T>
    void MarkPipeDirtyField(const T& field) {
        const auto offset = reinterpret_cast<const u8*>(&field) -
                            reinterpret_cast<const u8*>(regs.reg_array.data());
        MarkPipeDirty(static_cast<u32>(offset / sizeof(u32)),
                      static_cast<u32>((sizeof(T) + sizeof(u32) - 1) / sizeof(u32)));
    }
    /// Marks the registers other threads wrote (SetGsRingSizes) since the last call.
    void TakeExternalRegWrites() {
        if (external_reg_writes.load(std::memory_order_relaxed) &&
            external_reg_writes.exchange(false, std::memory_order_acquire)) {
            MarkPipeDirtyField(regs.vgt_esgs_ring_size);
            MarkPipeDirtyField(regs.vgt_gsvs_ring_size);
        }
    }

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        std::scoped_lock lk{submit_mutex};
        mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
        mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
        submit_done = true;
        submit_cv.notify_one();
    }

    void WaitGpuIdle() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    bool IsGpuIdle() const {
        return num_submits == 0;
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    /// True when a submit or a queued command is waiting for the parser. Work
    /// the GPU command thread runs while otherwise idle polls this to yield.
    bool HasPendingWork() const noexcept {
        return num_submits.load(std::memory_order_relaxed) != 0 ||
               num_commands.load(std::memory_order_relaxed) != 0;
    }

    /// The GPU command thread, or the draw pipe's recording thread: the threads that own the
    /// caches and the command recording (one at a time).
    bool OnGpuThread() const noexcept {
        return gpu_side_thread;
    }
    /// The GPU command thread only.
    static bool OnCommandThread() noexcept {
        return command_thread;
    }
    static void MarkGpuSideThread() noexcept {
        gpu_side_thread = true;
    }
    /// Draw pipe: the recording thread takes the core reserved for the command thread
    /// (gpu_thread_core_reserve); SHADPS4_PIPE_PLACEMENT=0 keeps it on the command thread.
    static bool PipeTakesReservedCore();

    /// Guest thread register write (sceGnmSetGsRingSizes).
    void SetGsRingSizes(u32 esgs_ring_size, u32 gsvs_ring_size) {
        regs.vgt_esgs_ring_size = esgs_ring_size;
        regs.vgt_gsvs_ring_size = gsvs_ring_size;
        external_reg_writes.store(true, std::memory_order_release);
    }

    u64 GetGfxStateStamp() const noexcept {
        return gfx_stamp.value;
    }
    u64 GetDynStateStamp() const noexcept {
        return gfx_stamp.dyn_value;
    }
    u64 GetRtStateStamp() const noexcept {
        return gfx_stamp.rt_value;
    }
    struct RegFunnelStats {
        u64 calls;
        u64 classified;
        u64 classified_rt;
    };
    RegFunnelStats DrainRegFunnelStats() noexcept {
        const RegFunnelStats out{gfx_stamp.funnel_calls, gfx_stamp.funnel_classified,
                                 gfx_stamp.funnel_classified_rt};
        gfx_stamp.funnel_calls = gfx_stamp.funnel_classified = gfx_stamp.funnel_classified_rt = 0;
        return out;
    }
    bool IsGfxStampActive() const noexcept {
        return gfx_stamp.active;
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([&sem, &func] {
                    func();
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_one();
            }
            sem.acquire();
        } else {
            std::scoped_lock lk{submit_mutex};
            command_queue.emplace(std::move(func));
            ++num_commands;
            submit_cv.notify_one();
        }
    }

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{64};

    // Purely diagnostic census of the packets the guest submits, drained and
    // reset by the 300-frame PACKETS line in Rasterizer::OnSubmit.
    // GPU-parser-thread confined, like the counters above.
    struct PacketStats {
        u64 draws;            // graphics draws submitted by the guest
        u64 predicated_draws; // ... of which carry the predicate header bit
        u64 dispatches;       // compute dispatches
        u64 occlusion_events; // ZpassDone events answered with a fake count
        u64 set_predication;  // SET_PREDICATION packets (currently ignored)
        u64 acb_dispatches;   // compute queue dispatches
        u64 gfx_to_acb;       // compute queue dispatches right after a graphics draw
        bool after_draw;      // the last draw or dispatch was a graphics draw
        // WAIT_REG_MEM on the graphics and compute queues: met by a pending label at the first
        // test, waits that were not met at once, loop iterations, and waits met by a label
        // another queue queued meanwhile (draw pipe only).
        u64 waits_pending;
        u64 waits;
        u64 wait_spins;
        u64 waits_pending_late;
        u64 acb_waits_pending;
        u64 acb_waits;
        u64 acb_wait_spins;
        u64 acb_waits_pending_late;
    };
    PacketStats packet_stats{};

    std::thread::id GetGpuCommandProcessorThread() {
        return gpu_id;
    }

#ifdef __linux__
    u32 GetGpuCommandProcessorThreadId() {
        return gpu_tid;
    }
#endif

private:
    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb);
    /// Consumes every graphics packet whose handling cannot suspend the parser.
    /// Returns empty when dcb is exhausted, else the cursor on the unconsumed
    /// packet the coroutine must handle. Out of line by contract: inlined back
    /// into the coroutine, `this` becomes an escaped-frame load again.
    SHAD_NO_INLINE std::span<const u32> RunGraphicsPackets(std::span<const u32> dcb, Task& ce_task,
                                                           uintptr_t base_addr);
    // The two hottest packet arms, shared by the pre-dispatch fast path and
    // their switch cases so the routes cannot drift. The hot bodies inline into
    // the caller; the assert-carrying tails stay cold and out of line.
    SHAD_FORCE_INLINE void SetContextRegHot(const union PM4Header* header, u32 count);
    SHAD_NO_INLINE void SetContextRegExtentTail(u32 reg_addr, const union PM4Header* header,
                                                const u32* payload);
    SHAD_FORCE_INLINE void SetShRegHot(const union PM4Header* header, u32 count);
    SHAD_NO_INLINE void SetShRegCompute(const union PM4Header* header, u32 count);
    // Consumes the register write at the front of dcb and every register
    // write, type-2 pad and empty NOP after it; a truncated packet ends the
    // run unconsumed. The caller polls before the first packet, the loop
    // after every other one.
    SHAD_NO_INLINE std::span<const u32> RunRegisterRun(std::span<const u32> dcb);
    Task ProcessCeUpdate(std::span<const u32> ccb);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid);

    /// Draw-rate callers poll this with an empty queue in steady state; the
    /// inline gate spares them the call.
    void ProcessCommands() {
        if (num_commands.load(std::memory_order_relaxed)) {
            DrainCommands();
        }
    }
    void DrainCommands();
    void Process(std::stop_token stoken);

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    u32 num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};
    // Frozen counter = every query pair differences to zero samples passed:
    // the title's own visibility logic then culls those draws before issue.
    bool occlude_all_{};
    // inplace_guest_memory >= 1: fence labels, their interrupts and the GPU idle interrupt wait
    // for the GPU to finish the work recorded before them.
    bool honest_fences_{};
    u64 timestamp_clock_base_{};
    u64 timestamp_perf_base_{};
    u64 guest_gpu_time_percent_{100};

public:
    /// A fence label captured when its packet is decoded, written after the GPU finished the
    /// work recorded before it.
    struct FenceWrite {
        VAddr address;
        u64 value;
        u32 num_bytes;
        bool cpu_write = true; ///< False: the GPU writes it; only WAIT_REG_MEM resolution needs it.
        /// A GPU clock source: the value is taken on the GPU, after the work before it.
        DataSelect timestamp{};
        u32 timestamp_slot{};
        u64 timestamp_session{};
    };
    /// Draw pipe recording thread: a WRITE_DATA or DMA_DATA packet handed over in order.
    void RunPipePm4(const u32* packet);
    /// Whether queued honest signals wait for work not submitted yet.
    bool HonestSignalsUnsubmitted() const;
    /// Draw pipe recording thread: a fence handed over by HonestFence (its label is queued).
    void RunPipeFence(std::optional<FenceWrite> write, std::optional<Platform::InterruptId> irq);

private:
    /// Queues a label write and/or interrupt to run once the GPU finished all work recorded so
    /// far, without submitting: FlushHonestSignals submits at guest submission ends and waits.
    void QueueHonestSignal(std::optional<FenceWrite> write,
                           std::optional<Platform::InterruptId> irq, bool label_queued = false);
    /// An honest fence in stream order: handed to the draw pipe's recording thread
    /// (SHADPS4_PIPE_FENCES=1), else queued here after the downloads and lane copies.
    void HonestFence(std::optional<FenceWrite> write, std::optional<Platform::InterruptId> irq);
    /// Submits the recorded work when it holds queued honest signals.
    void FlushHonestSignals();
    /// WRITE_DATA: on the GPU in stream order where it targets memory bound in place.
    void WriteData(const PM4CmdWriteData* write_data, u32 data_size);
    void RunDmaData(const struct PM4DmaData* dma_data);
    /// Whether a queued, not yet written label will make the wait pass (the GPU runs the queues
    /// in recording order, so the wait is met by stream order).
    bool PendingLabelSatisfies(const PM4CmdWaitRegMem* wait);

    // Labels queued and not yet written, oldest first; their writes pop them in order.
    std::mutex pending_labels_mutex_;
    std::deque<FenceWrite> pending_labels_;
    // Tick of the newest queued honest signal.
    std::atomic<u64> honest_signal_tick_{};
    // SHADPS4_PIPE_FENCES: fences, WRITE_DATA and DMA_DATA run in order on the draw pipe.
    bool pipe_fences_{};
    bool reg_run_{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    std::atomic<bool> external_reg_writes{};
    static inline thread_local bool gpu_side_thread = false;
    static inline thread_local bool command_thread = false;
    // SHADPS4_PIPE_CONSERVATIVE=1: every packet but register writes, draws and dispatches
    // drains the draw pipe.
    bool pipe_drain_all_{};
    bool command_thread_moved_{};
    std::jthread process_thread{};
    std::atomic<u32> num_submits{};
    std::atomic<u32> num_commands{};

public:
    // Register run census: run/runs behind parser_reg_run, outer counts every
    // packet reaching the opcode switch. Declared after num_commands so the
    // poll word keeps its line. GPU-parser-thread confined.
    struct RunStats {
        u64 run_packets;   // packets consumed by the run loop
        u64 runs;          // run loop entries that consumed at least one
        u64 outer_packets; // packets that reached the full dispatch
        // Which opcode ended each run, so the packets that reach the full
        // dispatch can be named instead of inferred. Its own line: the poll
        // word one cache line up is written by the guest submit thread.
        alignas(64) std::array<u64, 256> break_opcode{};
    };
    RunStats run_stats{};

private:
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    std::thread::id gpu_id;
#ifdef __linux__
    u32 gpu_tid;
#endif
    s32 curr_qid{-1};
    // One bit per context register word the render-target memo and the
    // render-scope cache read; the stamp's rt lane classifies writes against
    // it. Declared last so last_cb_extent and last_db_extent keep their offsets.
    std::array<u64, (Regs::NumRegs - Regs::ContextRegWordOffset) / 64> rt_reg_mask_{};
};

} // namespace AmdGpu
