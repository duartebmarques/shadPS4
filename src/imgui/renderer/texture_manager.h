// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include "common/types.h"
#include "imgui/imgui_texture.h"

namespace vk {
class CommandBuffer;
}

namespace ImGui::Core::TextureManager {

struct Inner;

void StartWorker();

void StopWorker();

void DecodePngTexture(std::vector<u8> data, Inner* core);

void DecodePngFile(std::filesystem::path path, Inner* core);

void Submit();

// The GPU timeline of the command buffers ImGui draws into. A released texture is destroyed only
// once the work recorded until its release is done; without a timeline it is destroyed at once.
void SetGpuTimeline(std::function<u64()> current_tick, std::function<bool(u64)> is_tick_free);

}; // namespace ImGui::Core::TextureManager