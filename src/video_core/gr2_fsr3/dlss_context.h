// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "common/types.h"
#include "video_core/gr2_fsr3/dlss_streamline.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/gr2_fsr3/fsr4_context.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace Gr2Fsr3 {

/// Parameters a DLSS context is created with.
struct DlssKey {
    vk::Extent2D render_size{};
    vk::Extent2D output_size{};
    DlssMode mode{};
    DlssPreset preset{};

    bool operator==(const DlssKey&) const = default;
};

/// The key for the session sizes: DLAA for equal sizes, otherwise the mode of the width ratio.
DlssKey DlssKeyFor(vk::Extent2D render_size, vk::Extent2D output_size, DlssPreset preset);

/**
 * Owns the DLSS Super Resolution context of Streamline viewport 0 and its four images. The options
 * are set on the GPU command thread while no other DLSS call runs; evaluations and the release run
 * through Scheduler::Record, the release after the GPU passes the tick that was current when the
 * context was retired.
 */
class DlssContext {
public:
    explicit DlssContext(const Vulkan::Instance& instance);
    ~DlssContext();

    DlssContext(const DlssContext&) = delete;
    DlssContext& operator=(const DlssContext&) = delete;

    /**
     * GPU command thread. Moves towards a ready context for `wanted`, or towards none when it is
     * empty: releases a retired context once the GPU is done with it, creates the images and sets
     * the options once `may_allocate` holds and the range and budget checks pass, and retires a
     * context that is not wanted or whose recorded call failed. `extra_bytes` is the memory the
     * caller allocates for the same sizes. Returns Idle when nothing is wanted.
     */
    ContextState Update(Vulkan::Scheduler& scheduler, const std::optional<DlssKey>& wanted,
                        bool may_allocate, u64 extra_bytes);

    /// Whether images exist or await release, or NGX still holds a feature.
    bool HoldsMemory() const;

    /// Number of contexts that have become ready.
    u64 Generation() const;

    /// Device memory of the images and of NGX, or 0.
    u64 MemoryBytes() const;

    /// Why DLSS is unsupported, failed or waiting while a key is wanted, or an empty string.
    std::string Notice() const;

    /// The images of the ready context.
    const Fsr4Images& Images() const;

    /**
     * GPU command thread. Records one evaluation of the ready context, outside dynamic rendering,
     * after the input images were written. Returns false, retiring the context and keeping its key
     * failed, when a recorded call of the context failed.
     */
    bool RecordDispatch(Vulkan::Scheduler& scheduler, const Fsr4Dispatch& dispatch);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gr2Fsr3
