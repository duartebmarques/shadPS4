// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>

#include "common/types.h"
#include "video_core/gr2_fsr3/fsr3_types.h"

// The fsr3* and fsr4* keys of the "GR2Fork" section, read from custom_configs/<serial>.json first
// and config.json second. The values live in atomics: the GPU command thread takes snapshots while
// the menu edits them.
namespace Gr2Fsr3::Settings {

/// One copy of every fsr3* and fsr4* key but fsr4AssetDir. Percentages are integers in [0, 100].
struct Values {
    bool enable{false};
    bool fsr4_enable{false};
    Fsr4Model fsr4_model{Fsr4Model::Auto};
    s32 sharpness{0};
    Fxaa fxaa{Fxaa::Off};
    s32 reactive_characters{100};
    s32 reactive_effects{100};
    s32 reactive_outline{100};
    s32 tc_characters{100};
    bool character_camera_motion{true};
    ColorSpace color_space{ColorSpace::Linear};
    DepthSource depth_source{DepthSource::Copy};
    bool lod_bias{true};
    u32 lod_bias_extra{2}; ///< Extra LOD bias in steps of -0.5, from 0 to 3.
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
    ContextState fsr4_context{ContextState::Idle};
    u64 fsr4_bytes{};
    const char* fsr4_model{""}; ///< Preset of the wanted FSR 4 context, or empty.
    const char* fsr4_tier{""};  ///< Tier of the wanted FSR 4 context, or empty.
    std::string fsr4_notice;
};

/// Returns the current values, reading the files on the first call. Any thread.
Values Snapshot();

/**
 * The folder of the FSR 4 v07 files, read with the other keys: fsr4AssetDir resolved against the
 * user folder, or its fsr4_v07 folder when the key is empty or absent. Any thread.
 */
std::filesystem::path Fsr4AssetDir();

/// Replaces the published status. GPU command thread.
void PublishStatus(const Status& status);

/// Returns the last published status. Any thread.
Status ReadStatus();

/**
 * Writes the keys of Values into the "GR2Fork" section of the file each was read from, or of
 * config.json for a key neither file held. The other keys of each file are kept. Returns false
 * when a file cannot be written. ImGui thread.
 */
bool Save();

} // namespace Gr2Fsr3::Settings
