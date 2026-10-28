// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "core/file_sys/ifile.h"

// Button prompt packs and mod packs for Gravity Rush 2 and Gravity Rush Remastered. A pack replaces
// members of the game's PSARC archives: a button prompt pack with Xbox or Switch Pro button art and
// text, a mod pack with its own content, applied after the button prompt pack. The game files stay
// as they are: the archive the game opens is served with a patched table of contents and the new
// member data appended after the original bytes. A pack may also replace bytes of the game's
// executable that its members depend on.
namespace PromptPacks {

/// Applies the code patches of the active packs to the loaded executable. A pack whose code does
/// not match is not applied at all. Runs once the executable is loaded, before its code is decoded
/// for the CPU patches.
void ApplyCodePatches();

/// Returns the handle the game reads an archive through: the original one, or one that serves
/// the archive with the members the active packs replace.
std::unique_ptr<Core::FileSys::IFile> WrapArchive(std::string_view guest_path,
                                                  std::unique_ptr<Core::FileSys::IFile> file);

/// Sets the size a path stat reports for an archive the active packs replace members of.
void FixArchiveStat(std::string_view guest_path, Core::FileSys::FileStat& stat);

/// The input config lines the button prompt pack applies on top of the player's own config, or an
/// empty path when no button prompt pack is active.
std::filesystem::path InputOverrideFile();

/// Whether motion sensors may be enabled while the button prompt pack is in use.
bool MotionAllowed();

} // namespace PromptPacks
