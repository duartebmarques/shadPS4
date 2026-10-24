// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/types.h"
#include "video_core/gr2_fsr3/grr_capture.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class GraphicsPipeline;
class Rasterizer;
} // namespace Vulkan

namespace Gr2Fsr3 {

/**
 * Logs the structure of the Gravity Rush Remastered frame that the FXAA replacement relies on:
 * pass order, targets, formats, stencil writers and the draws the jitter takes. Each line is
 * logged once per distinct value up to a fixed count, and the draws into the scene depth are
 * surveyed for a fixed number of frames. GPU command thread only.
 */
class GrrSurvey {
public:
    GrrSurvey();
    ~GrrSurvey();

    GrrSurvey(const GrrSurvey&) = delete;
    GrrSurvey& operator=(const GrrSurvey&) = delete;

    /// Counts and classifies a draw; scene_depth is the last scene depth address.
    void OnDraw(const Vulkan::GraphicsPipeline& pipeline, const AmdGpu::Liverpool& liverpool,
                const GrrDrawHashes& hashes, VAddr scene_depth);

    /// Logs the images an FXAA draw binds, looked up through the texture cache.
    void OnFxaa(const GrrFxaaDraw& draw, bool recognized, const GrrSceneDepth& scene,
                Vulkan::Rasterizer& rasterizer);

    /// Ends the frame and starts the next, at the exposure dispatch or at the first draw into the
    /// scene depth.
    void OnFrameStart(bool exposure);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gr2Fsr3
