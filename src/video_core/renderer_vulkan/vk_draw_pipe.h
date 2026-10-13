// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Two-stage draw pipeline (ported from bbport, deadinside28/bloodborne_pc, vk_draw_pipe.h).
// The GPU command thread (stage A) decodes PM4 and keeps the register file; for a draw or a
// dispatch it writes a packet (the register blocks changed since the previous packet and the
// draw parameters) into this ring and goes on decoding. The recording thread (stage B) owns the
// caches, the barriers and the command recording while packets are in flight: it applies each
// packet to its own register copy and records the draw. Anything else stage A does on that state
// waits for stage B to run dry first (Drain).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <x86intrin.h>

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);
    /// Runs once on stage B before the first packet.
    using ThreadInit = void (*)(void* context);

    DrawPipe(Handler handler_, ThreadInit init, void* context_)
        : handler{handler_}, context{context_} {
        ring = std::make_unique<u8[]>(Capacity);
        thread = std::jthread([this, init](std::stop_token stop) {
            Common::SetCurrentThreadName("shadPS4:DrawRec");
            on_stage_b = true;
            init(context);
            Run(stop);
        });
    }

    ~DrawPipe() {
        Drain(0);
        thread.request_stop();
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return on_stage_b;
    }

    /// Stage A: space for a packet of `size` bytes, valid until Commit().
    u8* Begin(u32 size) {
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: a header with zero size skips the rest of the ring.
            WaitForSpace(at, Capacity - offset);
            reinterpret_cast<Header*>(ring.get() + offset)->size = 0;
            at += Capacity - offset;
            head = at;
            published.store(at, std::memory_order_release);
        }
        WaitForSpace(at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    void Commit(u32 payload_size) {
        reinterpret_cast<Header*>(ring.get() + head % Capacity)->payload = payload_size;
        head += pending_size;
        published.store(head, std::memory_order_seq_cst);
        ++packets;
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    [[nodiscard]] bool Idle() const noexcept {
        return consumed.load(std::memory_order_acquire) == head;
    }

    /// Why stage A drained (statistics): a graphics PM4 opcode, a compute queue opcode
    /// (ReasonComputeOpcode + opcode), or one of these.
    enum Reason : u32 {
        ReasonComputeOpcode = 256,
        ReasonCommands = 512,
        ReasonSubmissionEnd,
        ReasonFault,
        ReasonFence,
        ReasonRasterizer,
        NumReasons,
    };

    /// Stage A: waits until stage B has run every committed packet.
    void Drain(u32 reason) {
        if (Idle()) {
            return;
        }
        const u32 slot = std::min<u32>(reason, ReasonRasterizer);
        ++drains[slot];
        const u64 start = __rdtsc();
        for (u32 spins = 0; consumed.load(std::memory_order_acquire) != head; ++spins) {
            if (spins < 4096) {
                _mm_pause();
            } else {
                std::this_thread::yield();
            }
        }
        drain_cycles[slot] += __rdtsc() - start;
    }

    /// Statistics, read and reset by stage A: packets, drains that waited and the cycles they
    /// waited by reason, cycles stage B spent running packets.
    u64 packets = 0;
    std::array<u64, NumReasons> drains{};
    std::array<u64, NumReasons> drain_cycles{};
    std::atomic<u64> busy_cycles{0};

private:
    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
    };
    static constexpr u64 Capacity = 16ull << 20;

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    void WaitForSpace(u64 at, u64 size) {
        while (at + size - consumed.load(std::memory_order_acquire) > Capacity) {
            _mm_pause();
        }
    }

    void Run(std::stop_token stop) {
        // Packets follow each other within microseconds while a frame is decoded: spin, and
        // sleep only after a longer pause. A long spin takes a core from the game's threads.
        static const auto spin_time = std::chrono::microseconds([] {
            if (const char* env = std::getenv("SHADPS4_PIPE_SPIN_US")) {
                return std::max(0, std::atoi(env));
            }
            return 50;
        }());
        u64 at = 0;
        while (true) {
            u64 available = published.load(std::memory_order_acquire);
            if (available == at) {
                const auto spin_until = std::chrono::steady_clock::now() + spin_time;
                for (u32 spins = 1; available == at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    _mm_pause();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (published.load(std::memory_order_seq_cst) == at &&
                            !stop.stop_requested()) {
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                    available = published.load(std::memory_order_acquire);
                }
            }
            const auto* header = reinterpret_cast<const Header*>(ring.get() + at % Capacity);
            if (header->size == 0) {
                at += Capacity - at % Capacity;
                consumed.store(at, std::memory_order_release);
                continue;
            }
            const u64 start = __rdtsc();
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            busy_cycles.fetch_add(__rdtsc() - start, std::memory_order_relaxed);
            at += header->size;
            consumed.store(at, std::memory_order_release);
        }
    }

    static inline thread_local bool on_stage_b = false;
    Handler handler;
    void* context;
    std::unique_ptr<u8[]> ring;
    u64 head = 0; ///< stage A's write position
    u32 pending_size = 0;
    alignas(64) std::atomic<u64> published{0};
    alignas(64) std::atomic<u64> consumed{0};
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<bool> sleeping{false};
    std::jthread thread;
};

} // namespace Vulkan
