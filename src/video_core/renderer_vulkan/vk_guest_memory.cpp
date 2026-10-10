// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_guest_memory.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

#include "common/logging/log.h"
#include "core/address_space.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Vulkan::GuestMemory {

namespace {

constexpr auto ChunkUsage =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;

vk::Device device;
u32 memory_type = ~0u;
bool installed = false;
std::shared_mutex mutex;
std::vector<Chunk> chunks; // Sorted by phys.
std::atomic<u64> chunk_bytes{0};

/// Cached, coherent, host-visible system memory (not device-local): the guest's own CPU reads
/// of its memory would crawl through an uncached mapping.
u32 FindChunkType(const vk::PhysicalDeviceMemoryProperties& props, u32 type_bits) {
    const auto want = vk::MemoryPropertyFlagBits::eHostVisible |
                      vk::MemoryPropertyFlagBits::eHostCached |
                      vk::MemoryPropertyFlagBits::eHostCoherent;
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        const auto flags = props.memoryTypes[i].propertyFlags;
        if ((type_bits & (1u << i)) && (flags & want) == want &&
            !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            return i;
        }
    }
    return ~0u;
}

struct Allocation {
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    int fd = -1;
};

/// A buffer of `size` bytes bound to exported memory, and its dma-buf fd.
std::optional<Allocation> Allocate(u64 size) {
    const vk::ExternalMemoryBufferCreateInfo external = {
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const auto [buffer_result, buffer] = device.createBuffer({
        .pNext = &external,
        .size = size,
        .usage = ChunkUsage,
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (buffer_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Guest memory chunk buffer: {}", vk::to_string(buffer_result));
        return std::nullopt;
    }
    const auto requirements = device.getBufferMemoryRequirements(buffer);
    if (!(requirements.memoryTypeBits & (1u << memory_type))) {
        device.destroyBuffer(buffer);
        LOG_ERROR(Render_Vulkan, "Guest memory chunk buffer rejects the chunk memory type");
        return std::nullopt;
    }
    // The sparse arena has device addresses, so memory bound to it needs the flag too.
    const vk::MemoryAllocateFlagsInfo flags_info = {
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::ExportMemoryAllocateInfo export_info = {
        .pNext = &flags_info,
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const auto [memory_result, memory] = device.allocateMemory({
        .pNext = &export_info,
        .allocationSize = std::max<u64>(requirements.size, size),
        .memoryTypeIndex = memory_type,
    });
    if (memory_result != vk::Result::eSuccess) {
        device.destroyBuffer(buffer);
        LOG_ERROR(Render_Vulkan, "Guest memory chunk allocation of {} MiB: {}", size >> 20,
                  vk::to_string(memory_result));
        return std::nullopt;
    }
    if (const auto result = device.bindBufferMemory(buffer, memory, 0);
        result != vk::Result::eSuccess) {
        device.freeMemory(memory);
        device.destroyBuffer(buffer);
        LOG_ERROR(Render_Vulkan, "Guest memory chunk binding: {}", vk::to_string(result));
        return std::nullopt;
    }
    const auto [fd_result, fd] = device.getMemoryFdKHR({
        .memory = memory,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    });
    if (fd_result != vk::Result::eSuccess) {
        device.freeMemory(memory);
        device.destroyBuffer(buffer);
        LOG_ERROR(Render_Vulkan, "Guest memory dma-buf export: {}", vk::to_string(fd_result));
        return std::nullopt;
    }
    return Allocation{buffer, memory, fd};
}

/// The address space's allocator: called under the memory manager's exclusive lock when a
/// mapping first touches a chunk.
int AllocChunk(PAddr phys, u64 size, void*) {
    const auto allocation = Allocate(size);
    if (!allocation) {
        return -1;
    }
    {
        std::unique_lock lk{mutex};
        const auto it = std::ranges::upper_bound(chunks, phys, {}, &Chunk::phys);
        chunks.insert(it, Chunk{phys, size, allocation->buffer, allocation->memory});
    }
    const u64 total = chunk_bytes.fetch_add(size) + size;
    LOG_INFO(Render_Vulkan,
             "Guest memory: direct memory {:#x}+{} MiB in GPU-visible memory (dma-buf), {} MiB "
             "so far",
             phys, size >> 20, total >> 20);
    return allocation->fd;
}

/// Undoes AllocChunk when the address space could not map the chunk.
void ReleaseChunk(PAddr phys, void*) {
    std::unique_lock lk{mutex};
    const auto it = std::ranges::find(chunks, phys, &Chunk::phys);
    if (it == chunks.end()) {
        return;
    }
    chunk_bytes.fetch_sub(it->size);
    device.destroyBuffer(it->buffer);
    device.freeMemory(it->memory);
    chunks.erase(it);
}

#ifdef __linux__
/// Exports a small allocation and checks that mappings at an offset alias it, as the address
/// space needs (guest mappings start inside chunks).
bool ProbeOffsetMapping() {
    constexpr u64 Size = 2_MB;
    constexpr u64 Offset = 1_MB;
    const auto allocation = Allocate(Size);
    if (!allocation) {
        return false;
    }
    bool ok = false;
    void* whole = mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, allocation->fd, 0);
    void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, allocation->fd, Offset);
    if (whole != MAP_FAILED && page != MAP_FAILED) {
        *static_cast<volatile u32*>(page) = 0x5ca1ab1e;
        ok = static_cast<volatile u32*>(whole)[Offset / 4] == 0x5ca1ab1e;
    }
    if (whole != MAP_FAILED) {
        munmap(whole, Size);
    }
    if (page != MAP_FAILED) {
        munmap(page, 4096);
    }
    close(allocation->fd);
    device.destroyBuffer(allocation->buffer);
    device.freeMemory(allocation->memory);
    return ok;
}
#endif

} // Anonymous namespace

bool Install(const Instance& instance, u32 arena_type_bits) {
#ifdef __linux__
    constexpr u32 AmdVendor = 0x1002;
    if (!instance.IsGuestMemoryExportSupported()) {
        LOG_WARNING(Render_Vulkan, "Guest memory in place: the driver cannot export dma-bufs");
        return false;
    }
    if (instance.GetVendorID() != AmdVendor) {
        LOG_WARNING(Render_Vulkan, "Guest memory in place is tested on AMD GPUs only; off");
        return false;
    }
    device = instance.GetDevice();
    memory_type = FindChunkType(instance.GetPhysicalDevice().getMemoryProperties(),
                                arena_type_bits);
    if (memory_type == ~0u) {
        LOG_WARNING(Render_Vulkan,
                    "Guest memory in place: no cached system memory type fits the arena");
        return false;
    }
    if (!ProbeOffsetMapping()) {
        LOG_WARNING(Render_Vulkan, "Guest memory in place: dma-buf offset mapping failed");
        return false;
    }
    Core::Memory::Instance()->InstallGuestMemoryChunks(&AllocChunk, &ReleaseChunk, nullptr);
    installed = true;
    LOG_INFO(Render_Vulkan, "Guest memory in place: on (memory type {})", memory_type);
    return true;
#else
    return false;
#endif
}

bool Installed() {
    return installed;
}

const Chunk* Find(u64 phys) {
    std::shared_lock lk{mutex};
    const auto it = std::ranges::upper_bound(chunks, phys, {}, &Chunk::phys);
    if (it == chunks.begin()) {
        return nullptr;
    }
    const Chunk& chunk = *std::prev(it);
    // Chunks are never destroyed once mapped, so the pointer stays valid.
    return phys < chunk.phys + chunk.size ? &chunk : nullptr;
}

u64 ChunkBytes() {
    return chunk_bytes.load(std::memory_order_relaxed);
}

} // namespace Vulkan::GuestMemory
