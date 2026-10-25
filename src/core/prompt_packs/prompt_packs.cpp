// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include "common/elf_info.h"
#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/string_util.h"
#include "common/types.h"
#include "core/file_sys/fs.h"
#include "core/prompt_packs/prompt_packs.h"

namespace PromptPacks {

namespace {

using Core::FileSys::FileMapContext;
using Core::FileSys::FileStat;
using Core::FileSys::IFile;
using Core::FileSys::MmapPolicy;

constexpr u64 MaxPackFormat = 2;
constexpr u64 HeaderSize = 32;
constexpr u64 EntrySize = 30;
constexpr u32 EncryptedTocFlag = 4;
constexpr u64 MaxTocLength = 64_MB;

// An original member, possibly of another archive, whose bytes a delta copies after those of the
// member it rebuilds.
struct Source {
    std::string archive_name;
    std::string archive;
    std::string path;
    u64 size{};
    u32 crc{};
};

struct Member {
    std::string archive_name; // As the pack writes it.
    std::string archive;      // Lower case.
    std::string path;         // Lower case, without a leading slash.
    u64 orig_size{};
    u32 orig_crc{};
    u64 new_size{};
    u32 new_crc{};
    std::filesystem::path delta;
    std::vector<Source> sources;
};

struct Pack {
    std::string name; // The layout or the mod.
    std::filesystem::path dir;
    bool motion_controls{true};
    bool has_input_override{};
    std::vector<Member> members;
};

// The packs of the running game in the order they apply: the button prompt layout, then the mods.
struct PackSet {
    std::vector<Pack> packs;
    std::optional<std::size_t> layout;
};

// A mod pack of Gravity Rush 2 and the GR2Fork setting that turns it off. Mods are on unless the
// setting is false.
struct ModPack {
    const char* folder;
    const char* key;
};

constexpr std::array Gr2ModPacks{
    ModPack{"halloween", "gr2HalloweenItems"},
};

// An archive as the game sees it with the pack applied: the patched table of contents, the
// original bytes after it, then the replaced members.
struct Overlay {
    u64 base_size{};
    std::vector<u8> toc;
    std::vector<u8> data;

    u64 Size() const {
        return base_size + data.size();
    }
};

// The parts of a PSARC header the overlay uses. All header fields are big-endian.
struct Toc {
    u64 length{};
    u64 entry_count{};
    u64 block_size{};
    u64 width{}; // Bytes per block-size slot.
    u64 table_offset{};
    u64 block_count{};
};

struct Entry {
    u64 block{};
    u64 length{};
    u64 offset{};
};

u64 ReadBe(const u8* bytes, u64 count) {
    u64 value = 0;
    for (u64 i = 0; i < count; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

void WriteBe(u8* bytes, u64 count, u64 value) {
    for (u64 i = count; i-- > 0;) {
        bytes[i] = static_cast<u8>(value);
        value >>= 8;
    }
}

u32 ReadLe32(const u8* bytes) {
    return static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 |
           static_cast<u32>(bytes[2]) << 16 | static_cast<u32>(bytes[3]) << 24;
}

u32 Crc32(std::span<const u8> bytes) {
    return static_cast<u32>(crc32_z(crc32_z(0, nullptr, 0), bytes.data(), bytes.size()));
}

u64 BlockCount(u64 length, u64 block_size) {
    return (length + block_size - 1) / block_size;
}

std::string NormalizeMemberPath(std::string_view path) {
    while (path.starts_with('/')) {
        path.remove_prefix(1);
    }
    return Common::ToLower(path);
}

// The lower-case file name of an archive under /app0, or nothing for any other path.
std::optional<std::string> ArchiveName(std::string_view guest_path) {
    if (!guest_path.starts_with("/app0/")) {
        return std::nullopt;
    }
    std::string name = Common::ToLower(guest_path.substr(guest_path.find_last_of('/') + 1));
    if (!name.ends_with(".psarc")) {
        return std::nullopt;
    }
    return name;
}

template <typename T>
std::optional<T> Field(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    if constexpr (std::is_same_v<T, std::string>) {
        if (!it->is_string()) {
            return std::nullopt;
        }
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!it->is_boolean()) {
            return std::nullopt;
        }
    } else if (!it->is_number_unsigned()) {
        return std::nullopt;
    }
    return it->get<T>();
}

std::optional<u32> ParseCrc(const std::string& text) {
    u32 value{};
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value, 16);
    if (text.size() != 8 || result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<u8>> ReadHostFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    std::vector<u8> bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad()) {
        return std::nullopt;
    }
    return bytes;
}

// Reads a GR2Fork setting from the per-game file first, then from config.json, like the other
// GR2Fork settings. A value of another type is ignored.
template <typename T>
std::optional<T> ReadSetting(const char* key) {
    const std::string serial{Common::ElfInfo::Instance().GameSerial()};
    const std::array files{
        Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) / (serial + ".json"),
        Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.json",
    };
    for (const auto& path : files) {
        std::ifstream file{path};
        if (!file) {
            continue;
        }
        const auto root = nlohmann::json::parse(file, nullptr, false);
        if (!root.is_object()) {
            continue;
        }
        const auto section = root.find("GR2Fork");
        if (section == root.end() || !section->is_object()) {
            continue;
        }
        if (const auto value = Field<T>(*section, key)) {
            return value;
        }
        if (section->contains(key)) {
            LOG_WARNING(Loader, "Prompt packs: ignoring {} = {} in {}", key,
                        section->at(key).dump(), Common::FS::PathToUTF8String(path));
        }
    }
    return std::nullopt;
}

std::string ReadLayout(const char* key) {
    const auto layout = ReadSetting<std::string>(key);
    return layout ? Common::ToLower(*layout) : "off";
}

bool IsSafeRelative(const std::filesystem::path& path) {
    return !path.is_absolute() &&
           std::none_of(path.begin(), path.end(),
                        [](const std::filesystem::path& part) { return part == ".."; });
}

std::optional<Source> ParseSource(const nlohmann::json& object) {
    const auto archive = Field<std::string>(object, "archive");
    const auto path = Field<std::string>(object, "path");
    const auto size = Field<u64>(object, "size");
    const auto crc = Field<std::string>(object, "crc32");
    const auto crc_value = crc ? ParseCrc(*crc) : std::nullopt;
    if (!archive || !path || !size || !crc_value) {
        return std::nullopt;
    }
    return Source{
        .archive_name = *archive,
        .archive = Common::ToLower(*archive),
        .path = NormalizeMemberPath(*path),
        .size = *size,
        .crc = *crc_value,
    };
}

std::optional<Member> ParseMember(const nlohmann::json& object, const std::filesystem::path& dir) {
    const auto archive = Field<std::string>(object, "archive");
    const auto path = Field<std::string>(object, "path");
    const auto orig_size = Field<u64>(object, "orig_size");
    const auto orig_crc = Field<std::string>(object, "orig_crc32");
    const auto new_size = Field<u64>(object, "new_size");
    const auto new_crc = Field<std::string>(object, "new_crc32");
    const auto delta = Field<std::string>(object, "delta");
    if (!archive || !path || !orig_size || !orig_crc || !new_size || !new_crc || !delta) {
        return std::nullopt;
    }
    const auto orig_crc_value = ParseCrc(*orig_crc);
    const auto new_crc_value = ParseCrc(*new_crc);
    const std::filesystem::path delta_path{*delta};
    if (!orig_crc_value || !new_crc_value || !IsSafeRelative(delta_path)) {
        return std::nullopt;
    }
    Member member{
        .archive_name = *archive,
        .archive = Common::ToLower(*archive),
        .path = NormalizeMemberPath(*path),
        .orig_size = *orig_size,
        .orig_crc = *orig_crc_value,
        .new_size = *new_size,
        .new_crc = *new_crc_value,
        .delta = dir / delta_path,
    };
    if (const auto sources = object.find("sources"); sources != object.end()) {
        if (!sources->is_array()) {
            return std::nullopt;
        }
        for (const auto& item : *sources) {
            auto source = ParseSource(item);
            if (!source) {
                return std::nullopt;
            }
            member.sources.push_back(std::move(*source));
        }
    }
    return member;
}

// Loads prompt_mods/<game>/<folder>/pack.json. kind names the field that holds the folder name:
// "layout" for a button prompt pack, "mod" for a mod pack.
std::optional<Pack> LoadPack(const std::string& game, const std::string& folder, const char* kind) {
    Pack pack{
        .name = folder,
        .dir =
            Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "prompt_mods" / game / folder,
    };
    const auto manifest_path = pack.dir / "pack.json";
    std::ifstream file{manifest_path};
    if (!file) {
        LOG_ERROR(Loader, "Prompt packs: {} is missing, the {} pack is not applied",
                  Common::FS::PathToUTF8String(manifest_path), folder);
        return std::nullopt;
    }
    const auto root = nlohmann::json::parse(file, nullptr, false);
    const auto members = root.is_object() ? root.find("members") : root.end();
    const auto format = root.is_object() ? Field<u64>(root, "format") : std::nullopt;
    if (!format || *format == 0 || *format > MaxPackFormat ||
        Field<std::string>(root, "game") != game || Field<std::string>(root, kind) != folder ||
        members == root.end() || !members->is_array()) {
        LOG_ERROR(Loader, "Prompt packs: {} is not a {} pack for {} {}",
                  Common::FS::PathToUTF8String(manifest_path), kind, game, folder);
        return std::nullopt;
    }
    for (const auto& object : *members) {
        auto member = ParseMember(object, pack.dir);
        if (!member) {
            LOG_ERROR(Loader, "Prompt packs: {} holds a malformed member {}",
                      Common::FS::PathToUTF8String(manifest_path), object.dump());
            return std::nullopt;
        }
        pack.members.push_back(std::move(*member));
    }
    pack.motion_controls = Field<bool>(root, "motion_controls").value_or(true);
    std::error_code ec;
    pack.has_input_override = std::filesystem::is_regular_file(pack.dir / "input.ini", ec);
    LOG_INFO(Loader, "Prompt packs: {} {} {} with {} members", game, folder,
             Field<std::string>(root, "version").value_or("(no version)"), pack.members.size());
    return pack;
}

PackSet LoadPacks() {
    PackSet set;
    const bool is_gr2 = Common::IsGravityRush2();
    if (!is_gr2 && !Common::IsGravityRushRemastered()) {
        return set;
    }
    const std::string game = is_gr2 ? "GR2" : "GRR";
    const std::string layout = ReadLayout(is_gr2 ? "gr2PromptLayout" : "grrPromptLayout");
    if (layout == "xbox" || layout == "switch") {
        if (auto pack = LoadPack(game, layout, "layout")) {
            set.layout = set.packs.size();
            set.packs.push_back(std::move(*pack));
        }
    } else if (layout != "off") {
        LOG_WARNING(Loader, "Prompt packs: unknown layout \"{}\", the game keeps its prompts",
                    layout);
    }
    if (!is_gr2) {
        return set;
    }
    const auto mods = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "prompt_mods" / game;
    for (const ModPack& mod : Gr2ModPacks) {
        if (!ReadSetting<bool>(mod.key).value_or(true)) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(mods / mod.folder, ec)) {
            LOG_INFO(Loader, "Prompt packs: the {} mod is not installed", mod.folder);
            continue;
        }
        if (auto pack = LoadPack(game, mod.folder, "mod")) {
            set.packs.push_back(std::move(*pack));
        }
    }
    return set;
}

const PackSet& ActivePacks() {
    static const PackSet none;
    static std::mutex mutex;
    static PackSet set;
    static bool loaded{};
    std::scoped_lock lock{mutex};
    if (!loaded) {
        // The settings are per game, so nothing is decided before the game serial is known.
        if (Common::ElfInfo::Instance().GameSerial().empty()) {
            return none;
        }
        loaded = true;
        set = LoadPacks();
    }
    return set;
}

const Pack* LayoutPack() {
    const PackSet& set = ActivePacks();
    return set.layout ? &set.packs[*set.layout] : nullptr;
}

bool HasArchive(const PackSet& set, const std::string& name) {
    return std::ranges::any_of(set.packs, [&](const Pack& pack) {
        return std::ranges::any_of(pack.members,
                                   [&](const Member& member) { return member.archive == name; });
    });
}

bool ReadAt(IFile& file, u64 offset, std::span<u8> dst) {
    if (!file.Seek(static_cast<s64>(offset), Common::FS::SeekOrigin::SetOrigin)) {
        return false;
    }
    u64 done = 0;
    while (done < dst.size()) {
        const s64 got = file.Read(dst.data() + done, dst.size() - done);
        if (got <= 0) {
            return false;
        }
        done += static_cast<u64>(got);
    }
    return true;
}

std::optional<Toc> ReadToc(IFile& file, std::vector<u8>& toc_bytes) {
    std::array<u8, HeaderSize> header{};
    if (!ReadAt(file, 0, header) || std::memcmp(header.data(), "PSAR", 4) != 0 ||
        std::memcmp(header.data() + 8, "zlib", 4) != 0) {
        return std::nullopt;
    }
    Toc toc{
        .length = ReadBe(header.data() + 12, 4),
        .entry_count = ReadBe(header.data() + 20, 4),
        .block_size = ReadBe(header.data() + 24, 4),
    };
    const u64 entry_size = ReadBe(header.data() + 16, 4);
    const u64 flags = ReadBe(header.data() + 28, 4);
    if (entry_size != EntrySize || toc.entry_count == 0 || toc.block_size == 0 ||
        (flags & EncryptedTocFlag) != 0) {
        return std::nullopt;
    }
    toc.width = 1;
    while ((u64{1} << (8 * toc.width)) < toc.block_size) {
        ++toc.width;
    }
    toc.table_offset = HeaderSize + toc.entry_count * EntrySize;
    if (toc.length < toc.table_offset || toc.length > MaxTocLength) {
        return std::nullopt;
    }
    toc.block_count = (toc.length - toc.table_offset) / toc.width;
    toc_bytes.resize(toc.length);
    if (!ReadAt(file, 0, toc_bytes)) {
        return std::nullopt;
    }
    return toc;
}

u8* EntryBytes(std::vector<u8>& toc_bytes, u64 index) {
    return toc_bytes.data() + HeaderSize + index * EntrySize;
}

Entry ReadEntry(std::vector<u8>& toc_bytes, u64 index) {
    const u8* bytes = EntryBytes(toc_bytes, index);
    return {
        .block = ReadBe(bytes + 16, 4),
        .length = ReadBe(bytes + 20, 5),
        .offset = ReadBe(bytes + 25, 5),
    };
}

u8* BlockSlot(std::vector<u8>& toc_bytes, const Toc& toc, u64 block) {
    return toc_bytes.data() + toc.table_offset + block * toc.width;
}

// A stored size of 0 is a raw block of block_size bytes, a stored size equal to the block's
// length is a raw block, and any other size is a zlib stream.
std::optional<std::vector<u8>> ReadMember(IFile& file, const Toc& toc, std::vector<u8>& toc_bytes,
                                          const Entry& entry) {
    const u64 blocks = BlockCount(entry.length, toc.block_size);
    if (entry.block > toc.block_count || blocks > toc.block_count - entry.block) {
        return std::nullopt;
    }
    std::vector<u8> member(entry.length);
    std::vector<u8> stored;
    u64 offset = entry.offset;
    for (u64 i = 0; i < blocks; ++i) {
        const u64 length = std::min(toc.block_size, entry.length - i * toc.block_size);
        const u64 slot = ReadBe(BlockSlot(toc_bytes, toc, entry.block + i), toc.width);
        stored.resize(slot != 0 ? slot : toc.block_size);
        if (!ReadAt(file, offset, stored)) {
            return std::nullopt;
        }
        u8* dst = member.data() + i * toc.block_size;
        if (stored.size() == length) {
            std::memcpy(dst, stored.data(), length);
        } else {
            uLongf dst_length = static_cast<uLongf>(length);
            if (uncompress(dst, &dst_length, stored.data(), static_cast<uLong>(stored.size())) !=
                    Z_OK ||
                dst_length != length) {
                return std::nullopt;
            }
        }
        offset += stored.size();
    }
    return member;
}

// Builds a member from a GPD1 delta: COPY ops take ranges of the original, DATA ops carry the
// bytes that are not in it.
std::optional<std::vector<u8>> ApplyDelta(std::span<const u8> orig, std::span<const u8> delta) {
    u64 pos = 0;
    const auto take = [&](u64 count) -> const u8* {
        if (delta.size() - pos < count) {
            return nullptr;
        }
        const u8* bytes = delta.data() + pos;
        pos += count;
        return bytes;
    };
    const u8* header = take(16);
    if (header == nullptr || std::memcmp(header, "GPD1", 4) != 0 ||
        ReadLe32(header + 4) != orig.size()) {
        return std::nullopt;
    }
    const u64 new_size = ReadLe32(header + 8);
    const u32 op_count = ReadLe32(header + 12);
    std::vector<u8> member;
    member.reserve(new_size);
    for (u32 i = 0; i < op_count; ++i) {
        const u8* kind = take(1);
        if (kind == nullptr) {
            return std::nullopt;
        }
        if (*kind == 0) {
            const u8* args = take(8);
            if (args == nullptr) {
                return std::nullopt;
            }
            const u64 offset = ReadLe32(args);
            const u64 length = ReadLe32(args + 4);
            if (offset > orig.size() || length > orig.size() - offset ||
                length > new_size - member.size()) {
                return std::nullopt;
            }
            member.insert(member.end(), orig.data() + offset, orig.data() + offset + length);
        } else if (*kind == 1) {
            const u8* args = take(4);
            if (args == nullptr) {
                return std::nullopt;
            }
            const u64 length = ReadLe32(args);
            const u8* bytes = take(length);
            if (bytes == nullptr || length > new_size - member.size()) {
                return std::nullopt;
            }
            member.insert(member.end(), bytes, bytes + length);
        } else {
            return std::nullopt;
        }
    }
    if (member.size() != new_size || pos != delta.size()) {
        return std::nullopt;
    }
    return member;
}

// Whether any block of any entry is a zlib stream. Some archives store every block raw.
bool HasZlibBlocks(std::vector<u8>& toc_bytes, const Toc& toc) {
    for (u64 index = 0; index < toc.entry_count; ++index) {
        const Entry entry = ReadEntry(toc_bytes, index);
        const u64 blocks = BlockCount(entry.length, toc.block_size);
        if (entry.block > toc.block_count || blocks > toc.block_count - entry.block) {
            continue;
        }
        for (u64 i = 0; i < blocks; ++i) {
            const u64 length = std::min(toc.block_size, entry.length - i * toc.block_size);
            const u64 slot = ReadBe(BlockSlot(toc_bytes, toc, entry.block + i), toc.width);
            if (slot != 0 && slot != length) {
                return true;
            }
        }
    }
    return false;
}

// Appends the member in block_size pieces and fills its block-size slots. A piece is stored raw
// unless compress is set and zlib shrinks it.
void AppendBlocks(std::span<const u8> member, const Toc& toc, std::vector<u8>& toc_bytes,
                  u64 first_block, bool compress, std::vector<u8>& data) {
    std::vector<u8> packed(compressBound(static_cast<uLong>(toc.block_size)));
    for (u64 i = 0; i < BlockCount(member.size(), toc.block_size); ++i) {
        const u8* piece = member.data() + i * toc.block_size;
        const u64 length = std::min(toc.block_size, member.size() - i * toc.block_size);
        uLongf packed_length = static_cast<uLongf>(packed.size());
        const bool shrunk = compress &&
                            compress2(packed.data(), &packed_length, piece,
                                      static_cast<uLong>(length), Z_DEFAULT_COMPRESSION) == Z_OK &&
                            packed_length < length;
        u64 slot;
        if (shrunk) {
            data.insert(data.end(), packed.data(), packed.data() + packed_length);
            slot = packed_length;
        } else {
            data.insert(data.end(), piece, piece + length);
            slot = length == toc.block_size ? 0 : length;
        }
        WriteBe(BlockSlot(toc_bytes, toc, first_block + i), toc.width, slot);
    }
}

// Set while the packs open their other archives, so those opens return the original files.
thread_local bool g_opening_archives{};

// An archive the packs replace members of or read source members from.
struct Archive {
    std::string name; // As a pack writes it.
    std::unique_ptr<IFile> owned;
    IFile* file{};
    Toc toc;
    std::vector<u8> toc_bytes;
    bool compress{};
    std::unordered_map<std::string, u64> entries; // Member path -> entry index, 0 when absent.
};

using MemberKey = std::pair<std::string, std::string>; // Archive, member path.

// Reads the table of contents and finds the entry of every path the packs ask for.
bool IndexArchive(Archive& archive) {
    const auto toc = ReadToc(*archive.file, archive.toc_bytes);
    if (!toc) {
        LOG_ERROR(Loader, "Prompt packs: {} is not a PSARC archive the packs can patch",
                  archive.name);
        return false;
    }
    archive.toc = *toc;
    const auto manifest =
        ReadMember(*archive.file, *toc, archive.toc_bytes, ReadEntry(archive.toc_bytes, 0));
    if (!manifest) {
        LOG_ERROR(Loader, "Prompt packs: the file list of {} is unreadable", archive.name);
        return false;
    }
    // The new blocks follow the archive's own convention, so a reader that only ever sees raw
    // blocks in it gets raw blocks.
    archive.compress = HasZlibBlocks(archive.toc_bytes, *toc);

    // Line i of the file list names entry i + 1.
    const std::string_view list{reinterpret_cast<const char*>(manifest->data()), manifest->size()};
    u64 line_index = 0;
    for (u64 start = 0; start < list.size() && line_index + 1 < toc->entry_count; ++line_index) {
        const u64 end = std::min<u64>(list.find('\n', start), list.size());
        std::string_view line = list.substr(start, end - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (const auto it = archive.entries.find(NormalizeMemberPath(line));
            it != archive.entries.end()) {
            it->second = line_index + 1;
        }
        start = end + 1;
    }
    return true;
}

std::optional<std::vector<u8>> ReadOriginal(std::map<std::string, Archive>& archives,
                                            const std::string& name, const std::string& path) {
    Archive& archive = archives.at(name);
    const u64 index = archive.file != nullptr ? archive.entries.at(path) : 0;
    if (index == 0) {
        LOG_ERROR(Loader, "Prompt packs: {} has no {}", archive.name, path);
        return std::nullopt;
    }
    return ReadMember(*archive.file, archive.toc, archive.toc_bytes,
                      ReadEntry(archive.toc_bytes, index));
}

// Applies one pack on top of the members the packs before it replaced, or changes nothing when
// any member fails to verify. A pack may list a path once per version of it the pack accepts;
// the member whose original is the path's current content applies.
bool ApplyPack(const Pack& pack, std::map<std::string, Archive>& archives,
               std::map<MemberKey, std::vector<u8>>& members, std::vector<MemberKey>& order) {
    std::map<MemberKey, std::vector<u8>> updates;
    for (const Member& member : pack.members) {
        MemberKey key{member.archive, member.path};
        if (updates.contains(key)) {
            continue;
        }
        std::optional<std::vector<u8>> current;
        if (const auto it = members.find(key); it != members.end()) {
            current = it->second;
        } else {
            current = ReadOriginal(archives, member.archive, member.path);
        }
        if (!current) {
            return false;
        }
        const u32 crc = Crc32(*current);
        const auto match = std::ranges::find_if(pack.members, [&](const Member& other) {
            return other.archive == member.archive && other.path == member.path &&
                   other.orig_size == current->size() && other.orig_crc == crc;
        });
        if (match == pack.members.end()) {
            LOG_ERROR(Loader, "Prompt packs: {} in {} is not the version the {} pack was made for",
                      member.path, member.archive_name, pack.name);
            return false;
        }
        for (const Source& source : match->sources) {
            const auto bytes = ReadOriginal(archives, source.archive, source.path);
            if (!bytes || bytes->size() != source.size || Crc32(*bytes) != source.crc) {
                LOG_ERROR(Loader,
                          "Prompt packs: {} in {} is not the version the {} pack was made for",
                          source.path, source.archive_name, pack.name);
                return false;
            }
            current->insert(current->end(), bytes->begin(), bytes->end());
        }
        Archive& archive = archives.at(member.archive);
        const Entry entry = ReadEntry(archive.toc_bytes, archive.entries.at(member.path));
        const auto delta = ReadHostFile(match->delta);
        auto updated = delta ? ApplyDelta(*current, *delta) : std::nullopt;
        if (!updated || updated->size() != match->new_size || Crc32(*updated) != match->new_crc ||
            BlockCount(updated->size(), archive.toc.block_size) !=
                BlockCount(entry.length, archive.toc.block_size)) {
            LOG_ERROR(Loader, "Prompt packs: the delta for {} in {} is missing or damaged",
                      member.path, member.archive_name);
            return false;
        }
        updates.emplace(std::move(key), std::move(*updated));
    }
    for (const Member& member : pack.members) {
        MemberKey key{member.archive, member.path};
        if (!members.contains(key) && std::ranges::find(order, key) == order.end()) {
            order.push_back(key);
        }
    }
    for (auto& [key, bytes] : updates) {
        members[key] = std::move(bytes);
    }
    return true;
}

// Applies every pack at the first open of an archive one of them replaces members of. The
// archives sit in one folder. A pack that fails to verify is left out and the others still apply.
std::unordered_map<std::string, std::shared_ptr<const Overlay>> BuildOverlays(
    std::string_view guest_path, const std::string& opened, IFile& opened_file,
    const PackSet& set) {
    const std::string folder{guest_path.substr(0, guest_path.find_last_of('/') + 1)};
    std::map<std::string, Archive> archives;
    const auto want = [&](const std::string& archive, const std::string& name,
                          const std::string& path) {
        Archive& entry = archives[archive];
        if (entry.name.empty()) {
            entry.name = name;
        }
        entry.entries.emplace(path, 0);
    };
    for (const Pack& pack : set.packs) {
        for (const Member& member : pack.members) {
            want(member.archive, member.archive_name, member.path);
            for (const Source& source : member.sources) {
                want(source.archive, source.archive_name, source.path);
            }
        }
    }
    for (auto& [name, archive] : archives) {
        if (name == opened) {
            archive.file = &opened_file;
        } else {
            g_opening_archives = true;
            archive.owned = Common::Singleton<Core::FileSys::MntPoints>::Instance()->Open(
                folder + archive.name, false);
            g_opening_archives = false;
            archive.file = archive.owned.get();
            if (archive.file == nullptr) {
                LOG_ERROR(Loader, "Prompt packs: {}{} is missing", folder, archive.name);
            }
        }
        if (archive.file != nullptr && !IndexArchive(archive)) {
            archive.file = nullptr;
        }
    }

    std::map<MemberKey, std::vector<u8>> members;
    std::vector<MemberKey> order;
    for (const Pack& pack : set.packs) {
        if (!ApplyPack(pack, archives, members, order)) {
            LOG_ERROR(Loader, "Prompt packs: the {} pack is not applied", pack.name);
        }
    }

    std::unordered_map<std::string, std::shared_ptr<Overlay>> built;
    std::unordered_map<std::string, u64> replaced;
    for (const MemberKey& key : order) {
        const auto member = members.find(key);
        if (member == members.end()) {
            continue;
        }
        Archive& archive = archives.at(key.first);
        auto& overlay = built[key.first];
        if (!overlay) {
            overlay = std::make_shared<Overlay>();
            overlay->base_size = archive.file->Size();
            overlay->toc = archive.toc_bytes;
        }
        const u64 index = archive.entries.at(key.second);
        const Entry entry = ReadEntry(overlay->toc, index);
        u8* entry_bytes = EntryBytes(overlay->toc, index);
        WriteBe(entry_bytes + 20, 5, member->second.size());
        WriteBe(entry_bytes + 25, 5, overlay->Size());
        AppendBlocks(member->second, archive.toc, overlay->toc, entry.block, archive.compress,
                     overlay->data);
        ++replaced[key.first];
    }
    std::unordered_map<std::string, std::shared_ptr<const Overlay>> overlays;
    for (auto& [name, overlay] : built) {
        LOG_INFO(Loader, "Prompt packs: replaced {} members in {}", replaced[name],
                 archives.at(name).name);
        overlays.emplace(name, std::move(overlay));
    }
    return overlays;
}

std::shared_ptr<const Overlay> GetOverlay(std::string_view guest_path, const std::string& archive,
                                          IFile& file, const PackSet& set) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::shared_ptr<const Overlay>> overlays;
    static bool built{};
    std::scoped_lock lock{mutex};
    if (!built) {
        built = true;
        overlays = BuildOverlays(guest_path, archive, file, set);
    }
    const auto it = overlays.find(archive);
    // Another archive of the same name is served unchanged.
    if (it == overlays.end() || it->second->base_size != file.Size()) {
        return nullptr;
    }
    return it->second;
}

class OverlayFile final : public IFile {
public:
    OverlayFile(std::unique_ptr<IFile> base, std::shared_ptr<const Overlay> overlay)
        : m_base{std::move(base)}, m_overlay{std::move(overlay)} {}

    s64 Read(void* dst, u64 size) override {
        const s64 got = ReadVirtual(m_pos, static_cast<u8*>(dst), size);
        if (got > 0) {
            m_pos += static_cast<u64>(got);
        }
        return got;
    }

    s64 Write(const void*, u64) override {
        return -1;
    }

    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override {
        s64 start = 0;
        switch (origin) {
        case Common::FS::SeekOrigin::SetOrigin:
            break;
        case Common::FS::SeekOrigin::CurrentPosition:
            start = static_cast<s64>(m_pos);
            break;
        case Common::FS::SeekOrigin::End:
            start = static_cast<s64>(Size());
            break;
        }
        if (start + offset < 0) {
            return false;
        }
        m_pos = static_cast<u64>(start + offset);
        return true;
    }

    u64 Tell() const override {
        return m_pos;
    }

    u64 Size() const override {
        return m_overlay->Size();
    }

    bool Flush() override {
        return true;
    }

    bool IsOpen() const override {
        return m_base->IsOpen();
    }

    std::optional<std::filesystem::path> GetMetadataHostPath() const override {
        return m_base->GetMetadataHostPath();
    }

    void Stat(FileStat& out) override {
        m_base->Stat(out);
        out.size = Size();
    }

    MmapPolicy GetMmapPolicy() const override {
        return MmapPolicy::Copy;
    }

    bool Map(u8* addr, u64 size, u64 offset, u32 raw_prot, const FileMapContext& ctx) override {
        ctx.map_anonymous(addr, size);
        const s64 got = std::max<s64>(ReadVirtual(offset, addr, size), 0);
        std::memset(addr + got, 0, size - static_cast<u64>(got));
        ctx.protect(addr, size, raw_prot);
        return true;
    }

private:
    s64 ReadVirtual(u64 pos, u8* dst, u64 size) {
        if (size == 0 || pos >= Size()) {
            return 0;
        }
        size = std::min(size, Size() - pos);
        const u64 toc_end = m_overlay->toc.size();
        const u64 base_end = m_overlay->base_size;
        u64 done = 0;
        while (done < size) {
            const u64 at = pos + done;
            u64 count = 0;
            if (at < toc_end) {
                count = std::min(size - done, toc_end - at);
                std::memcpy(dst + done, m_overlay->toc.data() + at, count);
            } else if (at < base_end) {
                if (!m_base->Seek(static_cast<s64>(at), Common::FS::SeekOrigin::SetOrigin)) {
                    break;
                }
                const s64 got = m_base->Read(dst + done, std::min(size - done, base_end - at));
                if (got <= 0) {
                    break;
                }
                count = static_cast<u64>(got);
            } else {
                count = std::min(size - done, Size() - at);
                std::memcpy(dst + done, m_overlay->data.data() + (at - base_end), count);
            }
            done += count;
        }
        return done == 0 ? -1 : static_cast<s64>(done);
    }

    std::unique_ptr<IFile> m_base;
    std::shared_ptr<const Overlay> m_overlay;
    u64 m_pos{};
};

} // namespace

std::unique_ptr<IFile> WrapArchive(std::string_view guest_path, std::unique_ptr<IFile> file) {
    if (g_opening_archives || !file || !file->IsOpen() || !file->IsReadOnly()) {
        return file;
    }
    const auto name = ArchiveName(guest_path);
    if (!name || !HasArchive(ActivePacks(), *name)) {
        return file;
    }
    auto overlay = GetOverlay(guest_path, *name, *file, ActivePacks());
    if (!overlay) {
        return file;
    }
    return std::make_unique<OverlayFile>(std::move(file), std::move(overlay));
}

void FixArchiveStat(std::string_view guest_path, FileStat& stat) {
    const auto name = ArchiveName(guest_path);
    if (!name || !HasArchive(ActivePacks(), *name)) {
        return;
    }
    // Opening the archive builds its overlay if no open has built it yet.
    const auto file =
        Common::Singleton<Core::FileSys::MntPoints>::Instance()->Open(guest_path, false);
    if (file) {
        stat.size = file->Size();
    }
}

std::filesystem::path InputOverrideFile() {
    const Pack* pack = LayoutPack();
    return pack != nullptr && pack->has_input_override ? pack->dir / "input.ini"
                                                       : std::filesystem::path{};
}

bool MotionAllowed() {
    const Pack* pack = LayoutPack();
    return pack == nullptr || pack->motion_controls;
}

} // namespace PromptPacks
