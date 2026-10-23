// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace BigPictureMode {

struct GamePatch {
    std::filesystem::path file;
    std::string name;
    std::string note;
    std::string app_version;
    bool enabled;
    // Mask patches find their code by pattern and apply on any game version.
    bool any_version;
};

/// The patches of every patch folder whose files.json lists the serial.
std::vector<GamePatch> LoadGamePatches(const std::string& serial);

/// Writes isEnabled of one patch back to its file.
bool SetGamePatchEnabled(const GamePatch& patch, bool enabled);

/// Draws the patch list of a game as checkboxes; returns false once closed.
bool DrawPatchesPage(const std::string& title, const std::string& app_version,
                     std::vector<GamePatch>& patches, float ui_scale);

} // namespace BigPictureMode
