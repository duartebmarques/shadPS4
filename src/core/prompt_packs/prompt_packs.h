// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "core/file_sys/ifile.h"

// Button prompt packs for Gravity Rush 2 and Gravity Rush Remastered. A pack replaces members of
// the game's PSARC archives with Xbox or Switch Pro button art and text. The game files stay as
// they are: the archive the game opens is served with a patched table of contents and the new
// member data appended after the original bytes.
namespace PromptPacks {

/// Returns the handle the game reads an archive through: the original one, or one that serves
/// the archive with the members the active pack replaces.
std::unique_ptr<Core::FileSys::IFile> WrapArchive(std::string_view guest_path,
                                                  std::unique_ptr<Core::FileSys::IFile> file);

/// Sets the size a path stat reports for an archive the active pack replaces members of.
void FixArchiveStat(std::string_view guest_path, Core::FileSys::FileStat& stat);

/// The input config lines the active pack applies on top of the player's own config, or an empty
/// path when no pack is active.
std::filesystem::path InputOverrideFile();

/// Whether motion sensors may be enabled while the active pack is in use.
bool MotionAllowed();

} // namespace PromptPacks
