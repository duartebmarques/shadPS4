// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"
#include "video_core/gr2_fsr3/fsr3_types.h"

// The fsr3* keys of the "GR2Fork" section, read from custom_configs/<serial>.json first and
// config.json second. The values live in atomics: the GPU command thread takes snapshots while the
// menu edits them.
namespace Gr2Fsr3::Settings {

/// One copy of every fsr3* key. Percentages are integers in [0, 100].
struct Values {
    bool enable{false};
    s32 sharpness{25};
    s32 reactive_characters{50};
    s32 reactive_effects{70};
    s32 reactive_outline{20};
    s32 tc_characters{0};
    ColorSpace color_space{ColorSpace::Linear};
    DepthSource depth_source{DepthSource::Copy};
    bool lod_bias{true};
    bool fp32{false};
    bool fallback_easu{false};
    DebugView debug{DebugView::Off};
    bool validate{false};
    bool log{false};
    u32 test{0};
};

/// State of the module shown by the menu.
struct Status {
    Mode mode{Mode::Stock};
    Kind kind{Kind::Native};
    ContextState context{ContextState::Idle};
    u32 render_width{};
    u32 render_height{};
    u32 output_width{};
    u32 output_height{};
    u64 fsr_dispatches{};
    float frame_ms{};
    u64 context_bytes{};
    std::string notice;
};

/// Returns the current values, reading the files on the first call. Any thread.
Values Snapshot();

/// Replaces the published status. GPU command thread.
void PublishStatus(const Status& status);

/// Returns the last published status. Any thread.
Status ReadStatus();

/**
 * Writes every key into the "GR2Fork" section of the file it was read from, or of config.json for
 * a key neither file held. The other keys of each file are kept. Returns false when a file cannot
 * be written. ImGui thread.
 */
bool Save();

} // namespace Gr2Fsr3::Settings
