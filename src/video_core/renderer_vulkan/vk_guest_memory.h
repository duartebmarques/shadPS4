// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Guest direct memory in GPU-visible system memory (inplace_guest_memory 2). Ported from bbport
// (deadinside28/bloodborne_pc, gpu/shim/bbport_guest_memory.*, GPL-2.0-or-later).
//
// Each chunk of direct memory (Core::AddressSpace::ChunkSize) is a Vulkan allocation of cached,
// coherent system memory exported as a dma-buf. The address space maps that dma-buf at its
// backing view and at every guest address mapping that memory, so the GPU can use the guest's
// own bytes in place: the buffer cache binds arena blocks to chunk memory instead of copying.
// A chunk that cannot be made stays in the memfd and keeps the copying path.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::GuestMemory {

struct Chunk {
    u64 phys = 0; ///< First byte of direct memory it holds.
    u64 size = 0;
    vk::Buffer buffer; ///< The whole chunk as a plain buffer (labels, transfers).
    vk::DeviceMemory memory;
};

/// Probes dma-buf export and offset mapping, and installs the chunk allocator into the guest
/// address space. `arena_type_bits` are the memory types the sparse arena accepts with external
/// memory; the chunk type must be one of them. Returns whether chunks are in use.
bool Install(const Instance& instance, u32 arena_type_bits);

/// Whether chunks were installed.
bool Installed();

/// The chunk holding direct memory offset `phys`, or null.
const Chunk* Find(u64 phys);

/// Bytes of direct memory held in chunks so far.
u64 ChunkBytes();

} // namespace Vulkan::GuestMemory
