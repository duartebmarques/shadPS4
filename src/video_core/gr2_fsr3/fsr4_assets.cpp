// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spirv/unified1/spirv.hpp11>

#include "common/path_util.h"
#include "video_core/gr2_fsr3/fsr4_assets.h"

// <vulkan/vulkan.h> must first be included through vk_common.h, which configures it.
#include "video_core/gr2_fsr3/fsr4_v07_bridge.h"

namespace Gr2Fsr3 {

namespace {

static_assert(Fsr4ShaderCount == GR2_FSR4_SHADER_COUNT);

// Far above the size of any shader or model data file of a v07 model set.
constexpr std::uintmax_t MaxFileBytes = 64_MB;

// File index and exact size of the initializer and the pre-pass weights.
constexpr std::array<std::pair<std::size_t, std::size_t>, 2> ModelDataSizes{{
    {Fsr4ShaderCount, GR2_FSR4_INITIALIZER_BYTES},
    {Fsr4ShaderCount + 1, GR2_FSR4_PRE_WEIGHTS_BYTES},
}};

constexpr u32 Subgroup(vk::SubgroupFeatureFlagBits bit) {
    return static_cast<u32>(bit);
}

// A SPIR-V capability the shaders may declare, with the device feature or the compute-stage
// subgroup operation it needs.
struct CapabilityRule {
    spv::Capability capability;
    const char* name;
    bool Fsr4DeviceFeatures::* feature; // Null when no feature is needed.
    u32 subgroup;                       // 0 when no subgroup operation is needed.
};

// The derivative capabilities use their NV spellings, which the KHR extension shares.
constexpr auto CapabilityRules = std::to_array<CapabilityRule>({
    {spv::Capability::Matrix, "Matrix", nullptr, 0},
    {spv::Capability::Shader, "Shader", nullptr, 0},
    {spv::Capability::Float16, "Float16", &Fsr4DeviceFeatures::shader_float16, 0},
    {spv::Capability::Float64, "Float64", &Fsr4DeviceFeatures::shader_float64, 0},
    {spv::Capability::Int64, "Int64", &Fsr4DeviceFeatures::shader_int64, 0},
    {spv::Capability::Int16, "Int16", &Fsr4DeviceFeatures::shader_int16, 0},
    {spv::Capability::ImageGatherExtended, "ImageGatherExtended",
     &Fsr4DeviceFeatures::image_gather_extended, 0},
    {spv::Capability::Int8, "Int8", &Fsr4DeviceFeatures::shader_int8, 0},
    {spv::Capability::Sampled1D, "Sampled1D", nullptr, 0},
    {spv::Capability::Image1D, "Image1D", nullptr, 0},
    {spv::Capability::SampledBuffer, "SampledBuffer", nullptr, 0},
    {spv::Capability::ImageBuffer, "ImageBuffer", nullptr, 0},
    {spv::Capability::StorageImageExtendedFormats, "StorageImageExtendedFormats",
     &Fsr4DeviceFeatures::storage_image_extended_formats, 0},
    {spv::Capability::ImageQuery, "ImageQuery", nullptr, 0},
    {spv::Capability::DerivativeControl, "DerivativeControl", nullptr, 0},
    {spv::Capability::StorageImageReadWithoutFormat, "StorageImageReadWithoutFormat",
     &Fsr4DeviceFeatures::storage_image_read_without_format, 0},
    {spv::Capability::StorageImageWriteWithoutFormat, "StorageImageWriteWithoutFormat",
     &Fsr4DeviceFeatures::storage_image_write_without_format, 0},
    {spv::Capability::GroupNonUniform, "GroupNonUniform", nullptr,
     Subgroup(vk::SubgroupFeatureFlagBits::eBasic)},
    {spv::Capability::GroupNonUniformVote, "GroupNonUniformVote", nullptr,
     Subgroup(vk::SubgroupFeatureFlagBits::eVote)},
    {spv::Capability::GroupNonUniformArithmetic, "GroupNonUniformArithmetic", nullptr,
     Subgroup(vk::SubgroupFeatureFlagBits::eArithmetic)},
    {spv::Capability::GroupNonUniformBallot, "GroupNonUniformBallot", nullptr,
     Subgroup(vk::SubgroupFeatureFlagBits::eBallot)},
    {spv::Capability::GroupNonUniformShuffle, "GroupNonUniformShuffle", nullptr,
     Subgroup(vk::SubgroupFeatureFlagBits::eShuffle)},
    {spv::Capability::GroupNonUniformShuffleRelative, "GroupNonUniformShuffleRelative", nullptr,
     Subgroup(vk::SubgroupFeatureFlagBits::eShuffleRelative)},
    {spv::Capability::GroupNonUniformClustered, "GroupNonUniformClustered", nullptr,
     Subgroup(vk::SubgroupFeatureFlagBits::eClustered)},
    {spv::Capability::GroupNonUniformQuad, "GroupNonUniformQuad", nullptr,
     Subgroup(vk::SubgroupFeatureFlagBits::eQuad)},
    {spv::Capability::StorageBuffer16BitAccess, "StorageBuffer16BitAccess",
     &Fsr4DeviceFeatures::storage_buffer_16bit, 0},
    {spv::Capability::UniformAndStorageBuffer16BitAccess, "UniformAndStorageBuffer16BitAccess",
     &Fsr4DeviceFeatures::uniform_storage_buffer_16bit, 0},
    {spv::Capability::StorageBuffer8BitAccess, "StorageBuffer8BitAccess",
     &Fsr4DeviceFeatures::storage_buffer_8bit, 0},
    {spv::Capability::UniformAndStorageBuffer8BitAccess, "UniformAndStorageBuffer8BitAccess",
     &Fsr4DeviceFeatures::uniform_storage_buffer_8bit, 0},
    {spv::Capability::ComputeDerivativeGroupQuadsNV, "ComputeDerivativeGroupQuads",
     &Fsr4DeviceFeatures::derivative_group_quads, 0},
    {spv::Capability::ComputeDerivativeGroupLinearNV, "ComputeDerivativeGroupLinear",
     &Fsr4DeviceFeatures::derivative_group_linear, 0},
    {spv::Capability::DotProductInputAll, "DotProductInputAll",
     &Fsr4DeviceFeatures::shader_integer_dot_product, 0},
    {spv::Capability::DotProductInput4x8Bit, "DotProductInput4x8Bit",
     &Fsr4DeviceFeatures::shader_integer_dot_product, 0},
    {spv::Capability::DotProductInput4x8BitPacked, "DotProductInput4x8BitPacked",
     &Fsr4DeviceFeatures::shader_integer_dot_product, 0},
    {spv::Capability::DotProduct, "DotProduct", &Fsr4DeviceFeatures::shader_integer_dot_product, 0},
});

// Extensions that are core in Vulkan 1.3; the capabilities they bring are checked on their own.
constexpr std::array<std::string_view, 6> CoreExtensions{
    "SPV_KHR_16bit_storage",       "SPV_KHR_8bit_storage",   "SPV_KHR_storage_buffer_storage_class",
    "SPV_KHR_integer_dot_product", "SPV_KHR_float_controls", "SPV_KHR_non_semantic_info",
};
constexpr std::string_view DerivativesExtension = "SPV_KHR_compute_shader_derivatives";

// What the loader needs from one module.
struct ModuleScan {
    std::string entry_point;
    std::vector<u32> capabilities;
    std::vector<std::string> extensions;
    // A variable is decorated with binding 2, the depth input of the pre-pass.
    bool binds_depth{};
};

const CapabilityRule* FindRule(u32 capability) {
    const auto rule = std::ranges::find_if(CapabilityRules, [capability](const CapabilityRule& r) {
        return static_cast<u32>(r.capability) == capability;
    });
    return rule == CapabilityRules.end() ? nullptr : &*rule;
}

// The literal string that starts at word `first` of an instruction, if it ends inside it. SPIR-V
// packs the characters into words in little-endian order, the byte order of the host.
std::optional<std::string_view> LiteralString(std::span<const u32> instruction, std::size_t first) {
    if (first >= instruction.size()) {
        return std::nullopt;
    }
    const std::span<const u32> words = instruction.subspan(first);
    const std::string_view chars{reinterpret_cast<const char*>(words.data()), words.size_bytes()};
    const std::size_t length = chars.find('\0');
    if (length == std::string_view::npos) {
        return std::nullopt;
    }
    return chars.substr(0, length);
}

// The scan of a module, or nothing when it is not well-formed SPIR-V with exactly one GLCompute
// entry point.
std::optional<ModuleScan> ScanModule(std::span<const u32> words) {
    if (words.size() < 5 || words[0] != spv::MagicNumber) {
        return std::nullopt;
    }
    ModuleScan scan{};
    u32 entry_points = 0;
    bool compute = false;
    for (std::size_t offset = 5; offset < words.size();) {
        const u32 word_count = words[offset] >> spv::WordCountShift;
        const u32 opcode = words[offset] & spv::OpCodeMask;
        if (word_count == 0 || word_count > words.size() - offset) {
            return std::nullopt;
        }
        const std::span<const u32> instruction = words.subspan(offset, word_count);
        offset += word_count;
        if (opcode == static_cast<u32>(spv::Op::OpCapability) && word_count >= 2) {
            scan.capabilities.push_back(instruction[1]);
        } else if (opcode == static_cast<u32>(spv::Op::OpExtension)) {
            const std::optional<std::string_view> name = LiteralString(instruction, 1);
            if (!name) {
                return std::nullopt;
            }
            scan.extensions.emplace_back(*name);
        } else if (opcode == static_cast<u32>(spv::Op::OpEntryPoint)) {
            const std::optional<std::string_view> name = LiteralString(instruction, 3);
            if (!name || ++entry_points > 1) {
                return std::nullopt;
            }
            compute = instruction[1] == static_cast<u32>(spv::ExecutionModel::GLCompute);
            scan.entry_point = *name;
        } else if (opcode == static_cast<u32>(spv::Op::OpDecorate) && word_count >= 4 &&
                   instruction[2] == static_cast<u32>(spv::Decoration::Binding)) {
            scan.binds_depth = scan.binds_depth || instruction[3] == 2;
        }
    }
    if (entry_points != 1 || !compute || scan.entry_point.empty() ||
        scan.entry_point.size() >= GR2_FSR4_ENTRY_POINT_MAX) {
        return std::nullopt;
    }
    return scan;
}

// The first capability or extension of a module that the device does not enable, or an empty
// string.
std::string UnsupportedItem(const ModuleScan& scan, const Fsr4DeviceLimits& device) {
    for (const u32 capability : scan.capabilities) {
        const CapabilityRule* rule = FindRule(capability);
        if (!rule) {
            return fmt::format("SPIR-V capability {}", capability);
        }
        const bool feature = !rule->feature || device.features.*(rule->feature);
        const bool subgroup = (device.subgroup_operations & rule->subgroup) == rule->subgroup;
        if (!feature || !subgroup) {
            return fmt::format("SPIR-V capability {}", rule->name);
        }
    }
    for (const std::string& extension : scan.extensions) {
        const bool core = std::ranges::any_of(
            CoreExtensions, [&extension](std::string_view name) { return name == extension; });
        if (!core &&
            !(extension == DerivativesExtension && device.features.compute_shader_derivatives)) {
            return fmt::format("SPIR-V extension {}", extension);
        }
    }
    return {};
}

} // namespace

Fsr4LoadResult LoadFsr4Assets(const std::filesystem::path& folder, u32 preset,
                              vk::Extent2D output_size, const Fsr4DeviceLimits& device) {
    Fsr4LoadResult result{};
    const auto [width, height] = output_size;
    Gr2Fsr4AssetNames names{};
    if (!Gr2Fsr4GetAssetNames(preset, width, height, &names)) {
        result.notice =
            fmt::format("FSR 4 supports outputs up to 7680x4320, not {}x{}", width, height);
        return result;
    }
    const u64 scratch_bytes = Gr2Fsr4ScratchBytes(width, height);
    const u64 allowed_bytes = std::min(device.max_storage_buffer_range, device.max_allocation_size);
    if (scratch_bytes > allowed_bytes) {
        result.notice =
            fmt::format("FSR 4 needs storage buffers of {} bytes for {}x{}, the device allows {}",
                        scratch_bytes, width, height, allowed_bytes);
        return result;
    }
    const std::string folder_name = Common::FS::PathToUTF8String(folder);
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        result.notice = fmt::format("FSR 4 asset folder {} does not exist", folder_name);
        return result;
    }

    std::array<std::vector<u8>, GR2_FSR4_FILE_COUNT> files;
    std::vector<std::string_view> missing;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::filesystem::path path = folder / names.files[i];
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) {
            missing.emplace_back(names.files[i]);
            continue;
        }
        if (size > MaxFileBytes) {
            result.notice = fmt::format("FSR 4: {} holds {} bytes, more than {}", names.files[i],
                                        size, MaxFileBytes);
            return result;
        }
        files[i].resize(static_cast<std::size_t>(size));
        std::ifstream file{path, std::ios::binary};
        if (!file.read(reinterpret_cast<char*>(files[i].data()),
                       static_cast<std::streamsize>(files[i].size()))) {
            missing.emplace_back(names.files[i]);
        }
    }
    if (!missing.empty()) {
        result.notice = fmt::format("FSR 4: {} of {} files are missing in {}, first {}",
                                    missing.size(), files.size(), folder_name, missing.front());
        return result;
    }
    for (const auto& [index, expected] : ModelDataSizes) {
        if (files[index].size() != expected) {
            result.notice = fmt::format("FSR 4: {} holds {} bytes, expected {}", names.files[index],
                                        files[index].size(), expected);
            return result;
        }
    }

    Fsr4Assets assets{};
    std::vector<std::string> entry_points;
    std::vector<u32> capabilities;
    std::vector<std::string> extensions;
    bool pre_pass_binds_depth = false;
    for (u32 pass = 0; pass < Fsr4ShaderCount; ++pass) {
        const std::vector<u8>& bytes = files[pass];
        const char* name = names.files[pass];
        if (bytes.empty() || bytes.size() % sizeof(u32) != 0) {
            result.notice = fmt::format("FSR 4: {} is not a compute SPIR-V module", name);
            return result;
        }
        std::vector<u32> words(bytes.size() / sizeof(u32));
        std::memcpy(words.data(), bytes.data(), bytes.size());
        const std::optional<ModuleScan> scan = ScanModule(words);
        if (!scan) {
            result.notice = fmt::format("FSR 4: {} is not a compute SPIR-V module", name);
            return result;
        }
        if (const std::string item = UnsupportedItem(*scan, device); !item.empty()) {
            result.notice =
                fmt::format("FSR 4: {} needs {}, which the device does not enable", name, item);
            return result;
        }
        // The backend repeats this check at creation, where the failing file is unknown.
        const Gr2Fsr4Shader shader{
            .words = words.data(),
            .size_bytes = bytes.size(),
            .entry_point = scan->entry_point.c_str(),
        };
        if (const s32 layout = Gr2Fsr4ValidateShader(&shader, pass); layout != VK_SUCCESS) {
            result.notice = fmt::format("FSR 4: {} does not match the v07 descriptor layout ({})",
                                        name, vk::to_string(static_cast<vk::Result>(layout)));
            return result;
        }
        entry_points.push_back(scan->entry_point);
        capabilities.insert(capabilities.end(), scan->capabilities.begin(),
                            scan->capabilities.end());
        extensions.insert(extensions.end(), scan->extensions.begin(), scan->extensions.end());
        if (pass == 0) {
            pre_pass_binds_depth = scan->binds_depth;
        }
        assets.shaders[pass] = std::move(words);
        assets.entry_points[pass] = scan->entry_point;
    }
    assets.initializer = std::move(files[Fsr4ShaderCount]);
    assets.pre_weights = std::move(files[Fsr4ShaderCount + 1]);

    const auto distinct = [](auto& values) {
        std::ranges::sort(values);
        values.erase(std::ranges::unique(values).begin(), values.end());
    };
    distinct(entry_points);
    distinct(capabilities);
    distinct(extensions);
    std::vector<const char*> capability_names;
    for (const u32 capability : capabilities) {
        capability_names.push_back(FindRule(capability)->name);
    }
    result.summary = fmt::format(
        "entry points {}, capabilities {}, extensions {}, pre-pass binds depth {}",
        fmt::join(entry_points, " "), fmt::join(capability_names, " "),
        extensions.empty() ? std::string{"none"} : fmt::format("{}", fmt::join(extensions, " ")),
        pre_pass_binds_depth ? "yes" : "no");
    result.assets = std::move(assets);
    return result;
}

} // namespace Gr2Fsr3
