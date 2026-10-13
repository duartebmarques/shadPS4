// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <magic_enum/magic_enum.hpp>

#include "common/alignment.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/buffer_cache/stream_copy_lane.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_guest_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;
static constexpr u64 READBACK_WINDOW_SIZE = 512_KB;
// Every readback window lies inside one tracker region.
static_assert(HIGHER_PAGE_SIZE % READBACK_WINDOW_SIZE == 0);

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    // Guest memory in place: the arena must accept exported (dma-buf) memory.
    const bool want_in_place =
        EmulatorSettings.GetInplaceGuestMemory() >= 2 && instance.IsGuestMemoryExportSupported();
    const vk::ExternalMemoryBufferCreateInfo external_ci = {
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const vk::BufferCreateInfo probe_ci = {
        .pNext = want_in_place ? &external_ci : nullptr,
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    if (!instance.IsMaintenance6Supported()) {
        null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                     MemoryType::DeviceLocal, "Null Index Buffer");
        runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);
    }

    if (EmulatorSettings.IsResidencyBitmap()) {
        resident_bits.resize((u64{blocks_per_arena_page} * NUM_ARENA_PAGES + 63) / 64);
        resident_full.resize((resident_bits.size() + 63) / 64);
    }
    if (EmulatorSettings.IsStreamBarrierSkip()) {
        // ObtainBuffer hands the stream buffer out for read-only binds alone, and its one GPU
        // written part, compute shared memory, is bound without tracking.
        runtime.SetUntrackedBuffer(&stream_buffer);
    }
    clean_sync_peek = EmulatorSettings.IsCleanSyncPeek();
    readback_offload = EmulatorSettings.IsReadbackOffload();

    if (want_in_place) {
        // A sparse arena page with external memory must be creatable before chunks go live.
        const auto [probe_result, probe_buffer] = device.createBuffer(probe_ci);
        if (probe_result == vk::Result::eSuccess) {
            device.destroyBuffer(probe_buffer);
            sparse_external_memory = true;
            if (Vulkan::GuestMemory::Install(instance, reqs.memoryTypeBits)) {
                in_place = true;
                in_place_bits.resize((u64{blocks_per_arena_page} * NUM_ARENA_PAGES + 63) / 64);
                in_place_full.resize((in_place_bits.size() + 63) / 64);
                in_place_write_ticks =
                    std::make_unique<std::atomic<u64>[]>(u64{1} << (40 - WriteTickShift));
                in_place_small_reads = EmulatorSettings.GetInplaceGuestMemory() >= 3;
                cp_written_words = in_place_bits.size();
                cp_written_bits = std::make_unique<std::atomic<u64>[]>(cp_written_words);
                LOG_INFO(Render_Vulkan, "Guest memory in place: small reads in place: {}",
                         in_place_small_reads);
            } else {
                sparse_external_memory = false;
            }
        } else {
            LOG_WARNING(Render_Vulkan,
                        "Guest memory in place: sparse buffers cannot take external memory ({})",
                        vk::to_string(probe_result));
        }
    }
}

BufferCache::~BufferCache() = default;

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    if (readback_offload && !assume_locks && OffloadReadback(device_addr, size, is_write)) {
        return;
    }
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, READBACK_WINDOW_SIZE), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + READBACK_WINDOW_SIZE, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

u64 BufferCache::CollectDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                                  DownloadCopies& copies) {
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    return total_size_bytes;
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    if (readback_offload) {
        // A pending readback owns the GPU modified pages of its window until it finishes.
        std::unique_lock lk{readback_mutex};
        readback_cv.wait(lk, [&] { return !IsReadbackPending(device_addr, size); });
        MergeReadbackReturns();
    }
    DownloadCopies copies;
    const VAddr arena_base = arena->cpu_addr;
    const u64 total_size_bytes = CollectDownloads(arena, device_addr, size, copies);
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
}

struct BufferCache::Readback {
    VAddr window{};
    DownloadCopies copies{};
    Vulkan::StagingBufferRef staging{};
    VAddr arena_base{};
    u64 write_seq{};
    u64 tick{};
    bool busy{};
};

bool BufferCache::OffloadReadback(VAddr device_addr, u64 size, bool is_write) {
    const VAddr window = Common::AlignDown(device_addr, READBACK_WINDOW_SIZE);
    if (device_addr + size > window + READBACK_WINDOW_SIZE) {
        return false;
    }
    // Guest signals stay pending until the readback is done: other threads that fault on the
    // window wait for this one, and a guest handler could wait for them.
    Libraries::Kernel::Sigset all_signals;
    Libraries::Kernel::Sigset old_sigmask{};
    Libraries::Kernel::posix_sigfillset(&all_signals);
    Libraries::Kernel::posix_pthread_sigmask(POSIX_SIG_SETMASK, &all_signals, &old_sigmask);
    SCOPE_EXIT {
        Libraries::Kernel::posix_pthread_sigmask(POSIX_SIG_SETMASK, &old_sigmask, nullptr);
    };
    // A request still GPU modified after two rounds of its own takes the synchronous download,
    // which copies and unmarks in one GPU thread command.
    u32 attempts = 0;
    while (attempts < 2) {
        Readback job{.window = window};
        liverpool->SendCommand<true>([this, &job] { RecordReadback(job); });
        if (job.busy) {
            std::unique_lock lk{readback_mutex};
            readback_cv.wait(lk, [&] { return !IsReadbackPending(window, READBACK_WINDOW_SIZE); });
        } else {
            ++attempts;
            if (!job.copies.empty()) {
                scheduler.GetWorkSemaphore()->Wait(job.tick);
                FinishReadback(job);
            }
        }
        bool resolved = true;
        if (is_write) {
            // Marks the range CPU modified if it is GPU clean.
            memory_tracker->InvalidateRegion(device_addr, size, [&resolved] { resolved = false; });
        } else {
            resolved = !memory_tracker->IsRegionGpuModified(device_addr, size);
        }
        if (resolved) {
            return true;
        }
    }
    return false;
}

void BufferCache::RecordReadback(Readback& job) {
    {
        std::scoped_lock lk{readback_mutex};
        if (IsReadbackPending(job.window, READBACK_WINDOW_SIZE)) {
            job.busy = true;
            return;
        }
        MergeReadbackReturns();
    }
    const u64 block = job.window >> block_shift;
    const auto* arena = GetArena(block, block);
    const u64 total_size_bytes =
        CollectDownloads(arena, job.window, READBACK_WINDOW_SIZE, job.copies);
    if (total_size_bytes == 0) {
        return;
    }
    // Held until FinishReadback releases it; the pool reuses it once the GPU passes that release.
    job.staging = staging_pool.Request(total_size_bytes, MemoryType::HostCached, 0, true);
    for (auto& copy : job.copies) {
        copy.dstOffset += job.staging.offset;
    }
    job.arena_base = arena->cpu_addr;
    job.write_seq = memory_tracker->GpuWriteSeq(job.window, READBACK_WINDOW_SIZE);
    runtime.CopyBuffer(arena, job.staging.buffer, job.copies);
    job.tick = scheduler.CurrentTick();
    scheduler.Flush();
    std::scoped_lock lk{readback_mutex};
    pending_readbacks.push_back(job.window);
}

void BufferCache::FinishReadback(const Readback& job) {
    job.staging.Invalidate();
    // The write-back runs ahead of the verdict: other downloads of the window wait while this one
    // is pending, and a vetoed window stays GPU modified.
    for (const auto& copy : job.copies) {
        auto* dst_addr = std::bit_cast<u8*>(job.arena_base + copy.srcOffset);
        memory->TryWriteBacking(
            dst_addr, job.staging.mapped + (copy.dstOffset - job.staging.offset), copy.size);
    }
    const bool unmarked = memory_tracker->TryUnmarkRegionAsGpuModified(
        job.window, READBACK_WINDOW_SIZE, job.write_seq);
    {
        std::scoped_lock lk{readback_mutex};
        if (!unmarked) {
            for (const auto& copy : job.copies) {
                readback_returns.emplace_back(job.arena_base + copy.srcOffset, copy.size);
            }
        }
        std::erase(pending_readbacks, job.window);
    }
    readback_cv.notify_all();
    // The staging pool belongs to the GPU thread.
    liverpool->SendCommand([this, staging = job.staging] { staging_pool.FreeDeferred(staging); });
}

bool BufferCache::IsReadbackPending(VAddr addr, u64 size) const {
    return std::ranges::any_of(pending_readbacks, [&](VAddr window) {
        return window < addr + size && addr < window + READBACK_WINDOW_SIZE;
    });
}

void BufferCache::MergeReadbackReturns() {
    for (const auto& [addr, size] : readback_returns) {
        gpu_modified_ranges.Add(addr, size);
    }
    readback_returns.clear();
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    // For read-only buffers use device local stream buffer to reduce renderpass breaks. With
    // guest memory in place this also keeps small reads a snapshot of guest memory at record
    // time, as command processor writes executed on the CPU expect.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (in_place_small_reads) {
            if (const auto bound = TryInPlaceRead(device_addr, size)) {
                return *bound;
            }
        }
        if (const auto lane_offset = StreamViaLane(device_addr, size)) {
            return {&stream_buffer, *lane_offset};
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    if (in_place && AllInPlace(first_block, last_block)) {
        // The arena is the guest's memory here: nothing to upload, track or read back.
        if (is_written) {
            NoteInPlaceWrite(device_addr, size);
        } else if (is_texel_buffer) {
            SynchronizeMemoryFromImage(arena, device_addr, size);
        }
        return {arena, arena->Offset(device_addr)};
    }
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        if (in_place) {
            NoteInPlaceWrite(device_addr, size);
            ForEachNotInPlace(device_addr, size,
                              [&](VAddr addr, u64 part) { gpu_modified_ranges.Add(addr, part); });
        } else {
            gpu_modified_ranges.Add(device_addr, size);
        }
    }
    return {arena, arena->Offset(device_addr)};
}

std::optional<u64> BufferCache::StreamViaLane(VAddr device_addr, u32 size) {
    // Workers read the never-protected backing view and every queued byte lands before the submit
    // that reads it (see StreamCopyLane). Commit's flush would run before the workers write, so
    // non-coherent rings copy inline, and the lane has a single producer, the GPU thread (the
    // recording thread with the draw pipe).
    auto& lane = StreamCopyLane::Instance();
    if (!lane.Enabled() || size < StreamCopyLane::kMinLaneBytes || !stream_buffer.is_coherent ||
        !liverpool->OnGpuThread() ||
        (liverpool->draw_pipe && AmdGpu::Liverpool::OnCommandThread())) {
        return std::nullopt;
    }
    Core::MemoryManager::BackingSpan spans[2];
    const bool hardened = lane.Hardened();
    const u32 num_spans = memory->ResolveBackingSpans(device_addr, size, spans, 2, hardened);
    if (num_spans == 0) {
        lane.NoteInlineUnresolved();
        return std::nullopt;
    }
    // 64-byte slots keep each job's cache lines on one core.
    const u64 alignment = std::max<u64>(instance.UniformMinAlignment(), 64);
    auto [dst, offset] = stream_buffer.Map(size, alignment);
    bool queued = true;
    for (u32 i = 0; i < num_spans; ++i) {
        if (queued) {
            queued = lane.Push(spans[i].ptr, dst, static_cast<u32>(spans[i].size));
        }
        if (!queued) {
            std::memcpy(dst, spans[i].ptr, spans[i].size);
        }
        dst += spans[i].size;
    }
    if (hardened) {
        Core::MemoryManager::EndBackingPush();
    }
    stream_buffer.Commit();
    return offset;
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size) ||
           (in_place && IsInPlaceWritePending(addr, size));
}

void BufferCache::SynchronizeDmaBuffers() {
    fault_process_pending = true;
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

static bool AnyClear(const u64* words, u64 first_bit, u64 last_bit) {
    for (u64 word = first_bit >> 6; word <= last_bit >> 6; ++word) {
        u64 mask = ~u64{0};
        if (word == first_bit >> 6) {
            mask &= ~u64{0} << (first_bit & 63);
        }
        if (word == last_bit >> 6) {
            mask &= ~u64{0} >> (63 - (last_bit & 63));
        }
        if ((words[word] & mask) != mask) {
            return true;
        }
    }
    return false;
}

static bool AllSet(const std::vector<u64>& bits, const std::vector<u64>& full, u64 first_block,
                   u64 last_block) {
    if (last_block >= bits.size() * 64) {
        return false;
    }
    const u64 first_word = first_block >> 6;
    const u64 last_word = last_block >> 6;
    if (last_word - first_word < 2) {
        return !AnyClear(bits.data(), first_block, last_block);
    }
    // The words in between are checked through the summary, one bit per full word.
    return !AnyClear(bits.data(), first_block, (first_word << 6) | 63) &&
           !AnyClear(full.data(), first_word + 1, last_word - 1) &&
           !AnyClear(bits.data(), last_word << 6, last_block);
}

static void SetBit(std::vector<u64>& bits, std::vector<u64>& full, u64 block) {
    u64& word = bits[block >> 6];
    word |= u64{1} << (block & 63);
    if (word == ~u64{0}) {
        full[block >> 12] |= u64{1} << ((block >> 6) & 63);
    }
}

static void ClearBit(std::vector<u64>& bits, std::vector<u64>& full, u64 block) {
    bits[block >> 6] &= ~(u64{1} << (block & 63));
    full[block >> 12] &= ~(u64{1} << ((block >> 6) & 63));
}

bool BufferCache::AllResident(u64 first_block, u64 last_block) const {
    return AllSet(resident_bits, resident_full, first_block, last_block);
}

void BufferCache::ForEachNotInPlace(VAddr device_addr, u64 size, auto&& func) const {
    const VAddr end_addr = device_addr + size;
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (end_addr - 1) >> block_shift;
    const auto is_in_place = [&](u64 block) {
        return block < in_place_bits.size() * 64 &&
               ((in_place_bits[block >> 6] >> (block & 63)) & 1) != 0;
    };
    u64 block = first_block;
    while (block <= last_block) {
        if (is_in_place(block)) {
            ++block;
            continue;
        }
        u64 run_end = block + 1;
        while (run_end <= last_block && !is_in_place(run_end)) {
            ++run_end;
        }
        const VAddr part_start = std::max<VAddr>(device_addr, block << block_shift);
        const VAddr part_end = std::min<VAddr>(end_addr, run_end << block_shift);
        func(part_start, part_end - part_start);
        block = run_end;
    }
}

bool BufferCache::AllInPlace(u64 first_block, u64 last_block) const {
    return AllSet(in_place_bits, in_place_full, first_block, last_block);
}

bool BufferCache::IsCpWritten(u64 first_block, u64 last_block) const {
    for (u64 block = first_block; block <= last_block; ++block) {
        if ((block >> 6) >= cp_written_words) {
            return true;
        }
        if ((cp_written_bits[block >> 6].load(std::memory_order_relaxed) >> (block & 63)) & 1) {
            return true;
        }
    }
    return false;
}

bool BufferCache::NoteCpWrite(VAddr device_addr, u64 size) {
    if (!in_place_small_reads || size == 0) {
        return false;
    }
    bool newly_in_place = false;
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    for (u64 block = first_block; block <= last_block && (block >> 6) < cp_written_words; ++block) {
        const u64 bit = u64{1} << (block & 63);
        auto& word = cp_written_bits[block >> 6];
        if (!(word.load(std::memory_order_relaxed) & bit)) {
            word.fetch_or(bit, std::memory_order_relaxed);
            newly_in_place |= ((in_place_bits[block >> 6] >> (block & 63)) & 1) != 0;
        }
    }
    return newly_in_place;
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::TryInPlaceRead(VAddr device_addr,
                                                                         u32 size) {
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    if (IsCpWritten(first_block, last_block)) {
        ++fast_stats.small_cp_marked;
        return std::nullopt;
    }
    if (!AllInPlace(first_block, last_block)) {
        // Bind it in place only when all of it can be: device memory for a small read would
        // cost an upload that the stream copy avoids.
        vk::DeviceMemory memory{};
        u64 memory_block{};
        if (AllResident(first_block, last_block) ||
            InPlaceRun(first_block, last_block + 1, &memory, &memory_block) !=
                last_block - first_block + 1) {
            ++fast_stats.small_other;
            return std::nullopt;
        }
        EnsureResident(GetArena(first_block, last_block), first_block, last_block);
        if (!AllInPlace(first_block, last_block)) {
            ++fast_stats.small_other;
            return std::nullopt;
        }
    }
    ++fast_stats.small_in_place;
    const auto* arena = GetArena(first_block, last_block);
    return std::pair{arena, arena->Offset(device_addr)};
}

bool BufferCache::IsAnyInPlace(VAddr device_addr, u64 size) const {
    if (!in_place || size == 0) {
        return false;
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block =
        std::min<u64>((device_addr + size - 1) >> block_shift, in_place_bits.size() * 64 - 1);
    for (u64 block = first_block; block <= last_block; ++block) {
        if ((in_place_bits[block >> 6] >> (block & 63)) & 1) {
            return true;
        }
    }
    return false;
}

u64 BufferCache::InPlaceRun(u64 block, u64 end, vk::DeviceMemory* out_memory,
                            u64* out_memory_block) const {
    const auto phys = memory->DirectPhysOf(block << block_shift, block_size);
    if (!phys || (*phys & (block_size - 1)) != 0) {
        return 0;
    }
    const auto* chunk = Vulkan::GuestMemory::Find(*phys);
    if (!chunk || *phys + block_size > chunk->phys + chunk->size) {
        return 0;
    }
    u64 count = 1;
    while (block + count < end) {
        const auto next = memory->DirectPhysOf((block + count) << block_shift, block_size);
        if (!next || *next != *phys + (count << block_shift) ||
            *next + block_size > chunk->phys + chunk->size) {
            break;
        }
        ++count;
    }
    *out_memory = chunk->memory;
    *out_memory_block = (*phys - chunk->phys) >> block_shift;
    return count;
}

void BufferCache::UnmapInPlace(VAddr device_addr, u64 size) {
    if (!in_place || size == 0) {
        return;
    }
    std::scoped_lock lk{pending_unmaps_mutex};
    pending_unmaps.emplace_back(device_addr, size);
    has_pending_unmaps.store(true, std::memory_order_release);
}

void BufferCache::ProcessPendingUnmaps() {
    std::vector<std::pair<VAddr, u64>> ranges;
    {
        std::scoped_lock lk{pending_unmaps_mutex};
        ranges.swap(pending_unmaps);
        has_pending_unmaps.store(false, std::memory_order_release);
    }
    bool released = false;
    const u64 max_block = in_place_bits.size() * 64;
    for (const auto& [addr, size] : ranges) {
        const u64 first_block = addr >> block_shift;
        const u64 last_block = std::min<u64>((addr + size - 1) >> block_shift, max_block - 1);
        u64 block = first_block;
        while (block <= last_block) {
            if (((in_place_bits[block >> 6] >> (block & 63)) & 1) == 0) {
                ++block;
                continue;
            }
            u64 run_end = block;
            while (run_end <= last_block &&
                   ((in_place_bits[run_end >> 6] >> (run_end & 63)) & 1) != 0) {
                ClearBit(in_place_bits, in_place_full, run_end);
                if (!resident_bits.empty()) {
                    ClearBit(resident_bits, resident_full, run_end);
                }
                ++run_end;
            }
            // The blocks get rebound to whatever is mapped there at their next use.
            resident_ranges.Subtract(block, run_end);
            released = true;
            block = run_end;
        }
    }
    if (released) {
        // Rebinding must not overtake work already submitted against the old binding.
        bind_wait_tick = std::max(bind_wait_tick, scheduler.CurrentTick() - 1);
    }
}

void BufferCache::NoteInPlaceWrite(VAddr device_addr, u64 size) {
    const u64 tick = scheduler.CurrentTick();
    const u64 first = device_addr >> WriteTickShift;
    const u64 last = (device_addr + std::max<u64>(size, 1) - 1) >> WriteTickShift;
    const u64 limit = u64{1} << (40 - WriteTickShift);
    for (u64 page = first; page <= last && page < limit; ++page) {
        in_place_write_ticks[page].store(tick, std::memory_order_relaxed);
    }
}

bool BufferCache::IsInPlaceWritePending(VAddr device_addr, u64 size) const {
    const u64 first = device_addr >> WriteTickShift;
    const u64 last = (device_addr + std::max<u64>(size, 1) - 1) >> WriteTickShift;
    const u64 limit = u64{1} << (40 - WriteTickShift);
    for (u64 page = first; page <= last && page < limit; ++page) {
        const u64 tick = in_place_write_ticks[page].load(std::memory_order_relaxed);
        if (tick != 0 && !scheduler.IsFree(tick)) {
            return true;
        }
    }
    return false;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    if (in_place && has_pending_unmaps.load(std::memory_order_acquire)) {
        ProcessPendingUnmaps();
    }
    if (!resident_bits.empty()) {
        ++fast_stats.resident_checks;
        if (AllResident(first_block, last_block)) {
            ++fast_stats.resident_hits;
            return;
        }
    }
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1,
                               [&](u64 start, u64 end) { bind_ranges.Add({start, end}); });

    if (bind_ranges.Empty()) {
        return;
    }

    // Each gap splits into runs bound in place (to the chunk holding the guest memory) and runs
    // that get new device memory and the copying path.
    struct Run {
        u64 start;
        u64 end;
        vk::DeviceMemory memory;
        u64 memory_block;
        bool in_place;
    };
    boost::container::small_vector<Run, 8> runs;
    u64 device_blocks{};
    u64 total_blocks{};
    for (const auto& range : bind_ranges) {
        u64 block = range.start;
        while (block < range.end) {
            vk::DeviceMemory chunk_memory{};
            u64 chunk_block{};
            const u64 in_place_count =
                in_place ? InPlaceRun(block, range.end, &chunk_memory, &chunk_block) : 0;
            if (in_place_count != 0) {
                runs.push_back({block, block + in_place_count, chunk_memory, chunk_block, true});
                block += in_place_count;
                continue;
            }
            u64 run_end = block + 1;
            if (!in_place) {
                run_end = range.end;
            } else {
                while (run_end < range.end &&
                       InPlaceRun(run_end, run_end + 1, &chunk_memory, &chunk_block) == 0) {
                    ++run_end;
                }
            }
            runs.push_back({block, run_end, {}, device_blocks, false});
            device_blocks += run_end - block;
            block = run_end;
        }
    }
    for (const auto& run : runs) {
        total_blocks += run.end - run.start;
    }

    vk::DeviceMemory device_memory{};
    if (device_blocks != 0) {
        const vk::MemoryAllocateFlagsInfo alloc_flags = {
            .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
        };
        const vk::MemoryAllocateInfo alloc_info = {
            .pNext = &alloc_flags,
            .allocationSize = device_blocks << block_shift,
            .memoryTypeIndex = arena_memory_type_index,
        };
        device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(total_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& run : runs) {
        Backing backing;
        backing.start = run.start;
        backing.end = run.end;
        backing.memory = run.in_place ? run.memory : device_memory;
        backing.offset = run.memory_block;
        resident_ranges.Add(backing);
        for (u64 block = run.start; block < run.end; ++block) {
            if (!resident_bits.empty()) {
                SetBit(resident_bits, resident_full, block);
            }
            if (run.in_place) {
                SetBit(in_place_bits, in_place_full, block);
            }
        }

        LOG_INFO(Render, "Making range start={}, end={} resident{}", backing.start, backing.end,
                 run.in_place ? " in place" : "");

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (run.start << block_shift) - arena->cpu_addr,
            .size = (run.end - run.start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = run.memory_block << block_shift,
        });

        for (u64 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (run.end - run.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, run.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    // A read-only bind over pages without a CPU write uploads nothing and moves no page state.
    bool clean = false;
    if (clean_sync_peek && !is_written) {
        ++fast_stats.sync_peeks;
        clean = memory_tracker->IsUploadClean(device_addr, size);
        fast_stats.sync_clean += clean;
    }
    if (!clean) {
        memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
            if (!in_place) {
                copies.emplace_back(total_size_bytes, addr, size);
                total_size_bytes += size;
                return;
            }
            // An upload into a block bound in place would write an older snapshot of guest
            // memory over newer guest writes.
            ForEachNotInPlace(addr, size, [&](VAddr part_addr, u64 part_size) {
                copies.emplace_back(total_size_bytes, part_addr, part_size);
                total_size_bytes += part_size;
            });
        });
    }
    if (!copies.empty()) {
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        for (auto& copy : copies) {
            memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset, copy.size);
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    // After in-place unmaps, rebinding waits for the work submitted against the old binding.
    const u64 wait_tick = std::exchange(bind_wait_tick, 0);
    const auto wait_sema = scheduler.GetWorkSemaphore()->Handle();
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = wait_tick != 0 ? 1u : 0u,
        .pWaitSemaphoreValues = wait_tick != 0 ? &wait_tick : nullptr,
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = wait_tick != 0 ? 1u : 0u,
        .pWaitSemaphores = wait_tick != 0 ? &wait_sema : nullptr,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
