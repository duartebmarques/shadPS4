// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "imgui/big_picture/patches_page.h"

namespace BigPictureMode {

namespace {

std::filesystem::path FindPatchFile(const std::filesystem::path& repo, const std::string& serial) {
    std::ifstream json_file{repo / "files.json"};
    if (!json_file) {
        return {};
    }
    const auto files = nlohmann::json::parse(json_file, nullptr, false);
    if (files.is_discarded() || !files.is_object()) {
        return {};
    }
    for (const auto& [filename, serials] : files.items()) {
        if (serials.is_array() &&
            std::find(serials.begin(), serials.end(), serial) != serials.end()) {
            return repo / filename;
        }
    }
    return {};
}

} // namespace

std::vector<GamePatch> LoadGamePatches(const std::string& serial) {
    std::vector<GamePatch> patches;
    const auto patch_dir = Common::FS::GetUserPath(Common::FS::PathType::PatchesDir);
    std::error_code ec;
    for (const auto& repo : std::filesystem::directory_iterator(patch_dir, ec)) {
        if (!repo.is_directory()) {
            continue;
        }
        const auto file = FindPatchFile(repo.path(), serial);
        pugi::xml_document doc;
        if (file.empty() || !doc.load_file(file.c_str())) {
            continue;
        }
        for (const auto& meta : doc.child("Patch").children("Metadata")) {
            bool any_version = true;
            for (const auto& line : meta.child("PatchList").children("Line")) {
                const std::string type = line.attribute("Type").value();
                any_version &= type == "mask" || type == "mask_jump32";
            }
            patches.push_back({
                .file = file,
                .name = meta.attribute("Name").value(),
                .note = meta.attribute("Note").value(),
                .app_version = meta.attribute("AppVer").value(),
                .enabled = std::string(meta.attribute("isEnabled").value()) == "true",
                .any_version = any_version,
            });
        }
    }
    return patches;
}

bool SetGamePatchEnabled(const GamePatch& patch, bool enabled) {
    pugi::xml_document doc;
    if (!doc.load_file(patch.file.c_str(), pugi::parse_default | pugi::parse_comments)) {
        return false;
    }
    for (auto meta : doc.child("Patch").children("Metadata")) {
        if (patch.name == meta.attribute("Name").value() &&
            patch.app_version == meta.attribute("AppVer").value()) {
            meta.attribute("isEnabled").set_value(enabled ? "true" : "false");
            if (!doc.save_file(patch.file.c_str(), "    ")) {
                LOG_ERROR(ImGui, "Could not write patch file {}", patch.file.string());
                return false;
            }
            return true;
        }
    }
    return false;
}

bool DrawPatchesPage(const std::string& title, const std::string& app_version,
                     std::vector<GamePatch>& patches, float ui_scale) {
    bool open = true;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Patches", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(ui_scale);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetWindowFocus();
    }
    ImGui::Text("Patches: %s (game version %s)", title.c_str(), app_version.c_str());
    ImGui::Separator();
    ImGui::BeginChild("PatchList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                      ImGuiChildFlags_NavFlattened);
    if (patches.empty()) {
        ImGui::TextWrapped("No patches for this game. Patch files go in the patches folder, each "
                           "patch repository in its own folder with a files.json index.");
    }
    for (std::size_t i = 0; i < patches.size(); ++i) {
        auto& patch = patches[i];
        const bool applies = patch.any_version || patch.app_version == app_version;
        ImGui::PushID(static_cast<int>(i));
        if (i == 0 && ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        bool enabled = patch.enabled;
        if (ImGui::Checkbox(patch.name.c_str(), &enabled) && SetGamePatchEnabled(patch, enabled)) {
            patch.enabled = enabled;
        }
        const bool focused = ImGui::IsItemFocused();
        if (patch.app_version != app_version) {
            ImGui::SameLine();
            ImGui::TextDisabled(applies ? "(made for version %s)" : "(version %s only)",
                                patch.app_version.c_str());
        }
        if (focused && !patch.note.empty()) {
            ImGui::SetTooltip("%s", patch.note.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button("Back") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        open = false;
    }
    ImGui::End();
    return open;
}

} // namespace BigPictureMode
