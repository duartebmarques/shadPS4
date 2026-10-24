// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <imgui.h>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "common/elf_info.h"
#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/types.h"
#include "video_core/gr2_fsr3/fsr3_settings.h"
#include "video_core/gr2_fsr3/fsr3_types.h"
#include "video_core/gr2_fsr3/gr2_fsr3.h"

namespace Gr2Fsr3 {

namespace {

// How a key is written in JSON. Every value is held as a u32: 0 or 1, a percentage, the index of
// a name, or the test bits.
enum class Type : u8 {
    Bool,
    Percent, // An integer, clamped to [0, 100].
    Index,   // An unsigned integer below the number of names.
    Name,    // One of the names.
    Bits,    // An unsigned 32-bit integer.
};

struct Key {
    const char* name{};
    Type type{};
    std::atomic<u32>* value{};
    u32 default_value{};
    std::span<const char* const> names{};
};

constexpr Settings::Values Defaults{};

constexpr std::array Fsr4ModelNames{
    "auto", "native", "quality", "balanced", "performance", "ultraperf", "drs",
};
constexpr std::array FxaaNames{"Off", "Characters", "Everything"};
constexpr std::array ColorSpaceNames{"linear", "gamma"};
constexpr std::array DepthSourceNames{"copy", "main"};
constexpr std::array LodBiasExtraNames{"0", "-0.5", "-1.0", "-1.5"};
constexpr std::array DebugViewNames{
    "Off", "Motion vectors", "Stencil", "Depth", "Masks", "Reprojection error", "FSR bypass",
};
static_assert(Fsr4ModelNames.size() == magic_enum::enum_count<Fsr4Model>());
static_assert(FxaaNames.size() == magic_enum::enum_count<Fxaa>());
static_assert(ColorSpaceNames.size() == magic_enum::enum_count<ColorSpace>());
static_assert(DepthSourceNames.size() == magic_enum::enum_count<DepthSource>());
static_assert(DebugViewNames.size() == magic_enum::enum_count<DebugView>());

constexpr auto TestBitLabels = std::to_array<std::pair<const char*, u32>>({
    {"Bit 0: jitter off", TestBit::JitterOff},
    {"Bit 1: jitter sign flip", TestBit::JitterSignFlip},
    {"Bit 2: motion vector Y flip", TestBit::MotionVectorYFlip},
    {"Bit 3: coverage jitter, FSR off", TestBit::CoverageJitter},
    {"Bit 6: camera cut detection off", TestBit::CutDetectionOff},
    {"Bit 7: repeated view zeroing off", TestBit::RepeatZeroingOff},
    {"Bit 8: character mask history", TestBit::CharacterMaskHistory},
    {"Bit 12: FSR 4 depth 1 - z", TestBit::Fsr4DepthInverted},
    {"Bit 13: FSR 4 linear view depth", TestBit::Fsr4DepthLinear},
});

std::atomic<u32> g_enable{};
std::atomic<u32> g_fsr4_enable{};
std::atomic<u32> g_grr_enable{};
std::atomic<u32> g_grr_fsr4_enable{};
std::atomic<u32> g_fsr4_model{};
std::atomic<u32> g_sharpness{};
std::atomic<u32> g_fxaa{};
std::atomic<u32> g_reactive_characters{};
std::atomic<u32> g_reactive_effects{};
std::atomic<u32> g_reactive_outline{};
std::atomic<u32> g_tc_characters{};
std::atomic<u32> g_character_camera_motion{};
std::atomic<u32> g_color_space{};
std::atomic<u32> g_depth_source{};
std::atomic<u32> g_lod_bias{};
std::atomic<u32> g_lod_bias_extra{};
std::atomic<u32> g_fp32{};
std::atomic<u32> g_fallback_easu{};
std::atomic<u32> g_debug{};
std::atomic<u32> g_validate{};
std::atomic<u32> g_log{};
std::atomic<u32> g_test{};

constexpr auto Keys = std::to_array<Key>({
    {"fsr3Enable", Type::Bool, &g_enable, Defaults.enable},
    {"fsr4Enable", Type::Bool, &g_fsr4_enable, Defaults.fsr4_enable},
    {"grrFsr3Enable", Type::Bool, &g_grr_enable, Defaults.grr_enable},
    {"grrFsr4Enable", Type::Bool, &g_grr_fsr4_enable, Defaults.grr_fsr4_enable},
    {"fsr4Model", Type::Name, &g_fsr4_model, static_cast<u32>(Defaults.fsr4_model), Fsr4ModelNames},
    {"fsr3Sharpness", Type::Percent, &g_sharpness, Defaults.sharpness},
    {"fsr3Fxaa", Type::Index, &g_fxaa, static_cast<u32>(Defaults.fxaa), FxaaNames},
    {"fsr3ReactiveCharacters", Type::Percent, &g_reactive_characters, Defaults.reactive_characters},
    {"fsr3ReactiveEffects", Type::Percent, &g_reactive_effects, Defaults.reactive_effects},
    {"fsr3ReactiveOutline", Type::Percent, &g_reactive_outline, Defaults.reactive_outline},
    {"fsr3TcCharacters", Type::Percent, &g_tc_characters, Defaults.tc_characters},
    {"fsr3CharacterCameraMotion", Type::Bool, &g_character_camera_motion,
     Defaults.character_camera_motion},
    {"fsr3ColorSpace", Type::Name, &g_color_space, static_cast<u32>(Defaults.color_space),
     ColorSpaceNames},
    {"fsr3DepthSource", Type::Name, &g_depth_source, static_cast<u32>(Defaults.depth_source),
     DepthSourceNames},
    {"fsr3LodBias", Type::Bool, &g_lod_bias, Defaults.lod_bias},
    {"fsr3LodBiasExtra", Type::Index, &g_lod_bias_extra, Defaults.lod_bias_extra,
     LodBiasExtraNames},
    {"fsr3Fp32", Type::Bool, &g_fp32, Defaults.fp32},
    {"fsr3FallbackEasu", Type::Bool, &g_fallback_easu, Defaults.fallback_easu},
    {"fsr3Debug", Type::Index, &g_debug, static_cast<u32>(Defaults.debug), DebugViewNames},
    {"fsr3Validate", Type::Bool, &g_validate, Defaults.validate},
    {"fsr3Log", Type::Bool, &g_log, Defaults.log},
    {"fsr3Test", Type::Bits, &g_test, Defaults.test},
});

using KeySet = std::array<bool, Keys.size()>;

// The keys the per-game file holds. Save writes these into it and the others into config.json.
KeySet g_per_game{};
std::once_flag g_load_once;

// fsr4AssetDir is a string beside the table. It is read once; Save keeps it as the files hold it.
constexpr const char* AssetDirKey = "fsr4AssetDir";
std::filesystem::path g_fsr4_asset_dir;

std::mutex g_status_mutex;
Settings::Status g_status;

std::filesystem::path GameFile() {
    const std::string serial{Common::ElfInfo::Instance().GameSerial()};
    return Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) / (serial + ".json");
}

std::filesystem::path GlobalFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.json";
}

std::optional<u32> Decode(const Key& key, const nlohmann::json& value) {
    switch (key.type) {
    case Type::Bool:
        if (value.is_boolean()) {
            return static_cast<u32>(value.get<bool>());
        }
        break;
    case Type::Percent:
        if (value.is_number_integer()) {
            return static_cast<u32>(std::clamp<s64>(value.get<s64>(), 0, 100));
        }
        break;
    case Type::Index:
        if (value.is_number_unsigned() && value.get<u64>() < key.names.size()) {
            return static_cast<u32>(value.get<u64>());
        }
        break;
    case Type::Name:
        if (value.is_string()) {
            const auto it = std::ranges::find(key.names, value.get<std::string>());
            if (it != key.names.end()) {
                return static_cast<u32>(it - key.names.begin());
            }
        }
        break;
    case Type::Bits:
        if (value.is_number_unsigned() && value.get<u64>() <= std::numeric_limits<u32>::max()) {
            return static_cast<u32>(value.get<u64>());
        }
        break;
    }
    return std::nullopt;
}

nlohmann::json Encode(const Key& key, u32 value) {
    if (key.type == Type::Bool) {
        return value != 0;
    }
    if (key.type == Type::Name) {
        return key.names[value];
    }
    return value;
}

// Takes the keys of the "GR2Fork" section that no earlier file held.
void ReadFile(const std::filesystem::path& path, KeySet& taken,
              std::optional<std::string>& asset_dir) {
    std::ifstream file{path};
    if (!file) {
        return;
    }
    const auto root = nlohmann::json::parse(file, nullptr, false);
    if (!root.is_object() || !root.contains("GR2Fork") || !root.at("GR2Fork").is_object()) {
        return;
    }
    const auto& section = root.at("GR2Fork");
    for (std::size_t i = 0; i < Keys.size(); ++i) {
        const Key& key = Keys[i];
        if (taken[i] || !section.contains(key.name)) {
            continue;
        }
        const auto& value = section.at(key.name);
        if (const std::optional<u32> decoded = Decode(key, value)) {
            key.value->store(*decoded, std::memory_order_relaxed);
            taken[i] = true;
        } else {
            LOG_WARNING(Config, "GR2 FSR3: ignoring {} = {} in {}", key.name, value.dump(),
                        Common::FS::PathToUTF8String(path));
        }
    }
    if (!asset_dir && section.contains(AssetDirKey)) {
        const auto& value = section.at(AssetDirKey);
        if (value.is_string()) {
            asset_dir = value.get<std::string>();
        } else {
            LOG_WARNING(Config, "GR2 FSR3: ignoring {} = {} in {}", AssetDirKey, value.dump(),
                        Common::FS::PathToUTF8String(path));
        }
    }
}

void LoadFiles() {
    for (const Key& key : Keys) {
        key.value->store(key.default_value, std::memory_order_relaxed);
    }
    // The per-game file is read first, so a key it holds wins over the same key in config.json.
    KeySet taken{};
    std::optional<std::string> asset_dir;
    ReadFile(GameFile(), taken, asset_dir);
    g_per_game = taken;
    ReadFile(GlobalFile(), taken, asset_dir);

    // JSON strings are UTF-8. A relative folder starts from the user folder.
    const std::filesystem::path& user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    g_fsr4_asset_dir = asset_dir && !asset_dir->empty()
                           ? user_dir / std::filesystem::path{std::u8string_view{
                                            reinterpret_cast<const char8_t*>(asset_dir->data()),
                                            asset_dir->size()}}
                           : user_dir / "fsr4_v07";

    std::string changed;
    for (const Key& key : Keys) {
        const u32 value = key.value->load(std::memory_order_relaxed);
        if (value != key.default_value) {
            changed.append(" ").append(key.name).append("=").append(Encode(key, value).dump());
        }
    }
    if (asset_dir) {
        changed.append(" ")
            .append(AssetDirKey)
            .append("=")
            .append(nlohmann::json(*asset_dir).dump());
    }
    LOG_INFO(Config, "GR2 FSR3: settings{}", changed.empty() ? " at their defaults" : changed);
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file{path};
    file << text;
    file.close();
    return !file.fail();
}

// Sets the keys that belong to the file in its "GR2Fork" section and keeps the rest of the file.
// Refuses a file that holds anything but a JSON object.
bool WriteFile(const std::filesystem::path& path, bool per_game) {
    nlohmann::json root = nlohmann::json::object();
    if (std::ifstream file{path}; file) {
        root = nlohmann::json::parse(file, nullptr, false);
    }
    if (!root.is_object()) {
        LOG_ERROR(Config, "GR2 FSR3: {} holds no JSON object, the settings are not saved",
                  Common::FS::PathToUTF8String(path));
        return false;
    }
    auto& section = root["GR2Fork"];
    if (!section.is_object()) {
        section = nlohmann::json::object();
    }
    for (std::size_t i = 0; i < Keys.size(); ++i) {
        if (g_per_game[i] == per_game) {
            section[Keys[i].name] = Encode(Keys[i], Keys[i].value->load(std::memory_order_relaxed));
        }
    }
    const std::string text = root.dump(2);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::error_code ec;
    bool written = WriteText(temporary, text);
    if (written) {
        std::filesystem::rename(temporary, path, ec);
        // Windows refuses to replace a file that another process holds open without delete
        // sharing; the file is then written in place.
        written = !ec || WriteText(path, text);
    }
    std::filesystem::remove(temporary, ec);
    if (!written) {
        LOG_ERROR(Config, "GR2 FSR3: cannot write {}", Common::FS::PathToUTF8String(path));
    }
    return written;
}

void EditBool(const char* label, std::atomic<u32>& setting) {
    bool value = setting.load(std::memory_order_relaxed) != 0;
    if (ImGui::Checkbox(label, &value)) {
        setting.store(value, std::memory_order_relaxed);
    }
}

void EditPercent(const char* label, std::atomic<u32>& setting) {
    int value = static_cast<int>(setting.load(std::memory_order_relaxed));
    if (ImGui::SliderInt(label, &value, 0, 100, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        setting.store(static_cast<u32>(value), std::memory_order_relaxed);
    }
}

void EditChoice(const char* label, std::atomic<u32>& setting, std::span<const char* const> names) {
    int value = static_cast<int>(setting.load(std::memory_order_relaxed));
    if (ImGui::Combo(label, &value, names.data(), static_cast<int>(names.size()))) {
        setting.store(static_cast<u32>(value), std::memory_order_relaxed);
    }
}

} // namespace

Settings::Values Settings::Snapshot() {
    std::call_once(g_load_once, LoadFiles);
    const auto load = [](const std::atomic<u32>& setting) {
        return setting.load(std::memory_order_relaxed);
    };
    return {
        .enable = load(g_enable) != 0,
        .fsr4_enable = load(g_fsr4_enable) != 0,
        .grr_enable = load(g_grr_enable) != 0,
        .grr_fsr4_enable = load(g_grr_fsr4_enable) != 0,
        .fsr4_model = static_cast<Fsr4Model>(load(g_fsr4_model)),
        .sharpness = static_cast<s32>(load(g_sharpness)),
        .fxaa = static_cast<Fxaa>(load(g_fxaa)),
        .reactive_characters = static_cast<s32>(load(g_reactive_characters)),
        .reactive_effects = static_cast<s32>(load(g_reactive_effects)),
        .reactive_outline = static_cast<s32>(load(g_reactive_outline)),
        .tc_characters = static_cast<s32>(load(g_tc_characters)),
        .character_camera_motion = load(g_character_camera_motion) != 0,
        .color_space = static_cast<ColorSpace>(load(g_color_space)),
        .depth_source = static_cast<DepthSource>(load(g_depth_source)),
        .lod_bias = load(g_lod_bias) != 0,
        .lod_bias_extra = load(g_lod_bias_extra),
        .fp32 = load(g_fp32) != 0,
        .fallback_easu = load(g_fallback_easu) != 0,
        .debug = static_cast<DebugView>(load(g_debug)),
        .validate = load(g_validate) != 0,
        .log = load(g_log) != 0,
        .test = load(g_test),
    };
}

std::filesystem::path Settings::Fsr4AssetDir() {
    std::call_once(g_load_once, LoadFiles);
    return g_fsr4_asset_dir;
}

void Settings::PublishStatus(const Status& status) {
    std::scoped_lock lock{g_status_mutex};
    g_status = status;
}

Settings::Status Settings::ReadStatus() {
    std::scoped_lock lock{g_status_mutex};
    return g_status;
}

bool Settings::Save() {
    std::call_once(g_load_once, LoadFiles);
    bool saved = true;
    if (std::ranges::contains(g_per_game, false)) {
        saved = WriteFile(GlobalFile(), false);
    }
    if (std::ranges::contains(g_per_game, true)) {
        saved = WriteFile(GameFile(), true) && saved;
    }
    return saved;
}

void DrawMenu() {
    static const bool is_gr2 = Common::IsGravityRush2();
    static const bool is_grr = Common::IsGravityRushRemastered();
    if (!(is_gr2 || is_grr) || !ImGui::BeginMenu(is_gr2 ? "FSR 3.1 (GR2)" : "FSR 3.1 (GRR)")) {
        return;
    }
    std::call_once(g_load_once, LoadFiles);
    EditBool("Enable FSR 3.1", is_gr2 ? g_enable : g_grr_enable);
    EditBool("Enable FSR 4 (INT8, experimental)", is_gr2 ? g_fsr4_enable : g_grr_fsr4_enable);
    EditChoice("FSR 4 model", g_fsr4_model, Fsr4ModelNames);
    EditPercent("Sharpness", g_sharpness);
    EditChoice("FXAA", g_fxaa, FxaaNames);
    EditPercent("Reactive characters", g_reactive_characters);
    if (is_gr2) {
        EditPercent("Reactive effects", g_reactive_effects);
        EditPercent("Reactive outlines", g_reactive_outline);
    }
    EditPercent("T&C characters", g_tc_characters);
    if (is_gr2) {
        EditBool("Character camera motion", g_character_camera_motion);
    }
    EditChoice("Colour space", g_color_space, ColorSpaceNames);
    if (is_gr2) {
        EditChoice("Depth source", g_depth_source, DepthSourceNames);
        EditBool("Texture LOD bias", g_lod_bias);
        EditChoice("Extra LOD bias", g_lod_bias_extra, LodBiasExtraNames);
    }
    EditBool("FP32 shaders", g_fp32);
    if (is_gr2) {
        EditBool("EASU fallback", g_fallback_easu);
    }
    EditChoice("Debug view", g_debug, DebugViewNames);
    EditBool("Validation", g_validate);
    EditBool("Periodic log", g_log);
    if (ImGui::BeginMenu("Test bits")) {
        unsigned int bits = g_test.load(std::memory_order_relaxed);
        for (const auto& [label, bit] : TestBitLabels) {
            if (ImGui::CheckboxFlags(label, &bits, bit)) {
                g_test.store(bits, std::memory_order_relaxed);
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    const Settings::Status status = Settings::ReadStatus();
    ImGui::Text("Mode: %s (%s)", magic_enum::enum_name(status.mode).data(),
                magic_enum::enum_name(status.kind).data());
    ImGui::Text("Context: %s, %.1f MiB", magic_enum::enum_name(status.context).data(),
                static_cast<double>(status.context_bytes) / (1024.0 * 1024.0));
    const char* fsr4_state = magic_enum::enum_name(status.fsr4_context).data();
    if (status.fsr4_model[0] != '\0') {
        ImGui::Text("FSR 4: %s, %s model, %s tier, %.1f MiB", fsr4_state, status.fsr4_model,
                    status.fsr4_tier, static_cast<double>(status.fsr4_bytes) / (1024.0 * 1024.0));
    } else {
        ImGui::Text("FSR 4: %s", fsr4_state);
    }
    ImGui::TextWrapped("FSR 4 assets: %s",
                       Common::FS::PathToUTF8String(Settings::Fsr4AssetDir()).c_str());
    if (!status.fsr4_notice.empty()) {
        ImGui::TextWrapped("%s", status.fsr4_notice.c_str());
    }
    ImGui::Text("Render %ux%u -> output %ux%u", status.render_width, status.render_height,
                status.output_width, status.output_height);
    ImGui::Text("FSR dispatches: %llu", static_cast<unsigned long long>(status.fsr_dispatches));
    ImGui::Text("Frame interval: %.2f ms", status.frame_ms);
    if (!status.notice.empty()) {
        ImGui::TextWrapped("%s", status.notice.c_str());
    }
    if (ImGui::Button("Save")) {
        Settings::Save();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndMenu();
}

} // namespace Gr2Fsr3
