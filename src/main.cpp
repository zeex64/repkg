#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "json.h"
#include "localized.h"
#include "package.h"
#include "texture.h"

namespace repkg {
namespace fs = std::filesystem;

namespace {

class ChainGuard {
public:
    explicit ChainGuard(fs::path path) : path_(std::move(path)) {}
    ChainGuard(const ChainGuard&) = delete;
    ChainGuard& operator=(const ChainGuard&) = delete;
    ~ChainGuard() { remove_tree(path_); }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
private:
    fs::path path_;
};

[[nodiscard]] std::string narrow(std::wstring_view input) {
    if (input.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (count <= 0) throw Error("invalid Unicode command-line argument");
    std::string output(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), count,
                            nullptr, nullptr) != count) {
        throw Error("could not convert command-line argument");
    }
    return output;
}

[[nodiscard]] std::wstring widen(std::string_view input) {
    if (input.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) throw Error("manifest path is not valid UTF-8");
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), count) != count) {
        throw Error("could not convert manifest path");
    }
    return output;
}

[[nodiscard]] fs::path resolve_path(const fs::path& base, const json::Value& value,
                                    std::string_view label) {
    fs::path path = widen(value.string(label));
    if (path.is_relative()) path = base / path;
    return fs::absolute(path).lexically_normal();
}

[[nodiscard]] std::uint64_t parse_unsigned(std::string_view text,
                                           std::string_view label) {
    if (text.empty() || text.front() == '-') throw Error(std::string(label) + " must be unsigned");
    std::size_t consumed = 0;
    std::uint64_t output = 0;
    try {
        output = std::stoull(std::string(text), &consumed, 0);
    } catch (const std::exception&) {
        throw Error(std::string(label) + " is not an unsigned integer");
    }
    if (consumed != text.size()) throw Error(std::string(label) + " has trailing characters");
    return output;
}

[[nodiscard]] std::uint64_t integer(const json::Value& value, std::string_view label,
                                    std::uint64_t maximum = UINT64_MAX) {
    std::uint64_t output = 0;
    if (value.is_integer()) {
        const std::int64_t signedValue = value.integer(label);
        if (signedValue < 0) throw Error(std::string(label) + " must be unsigned");
        output = static_cast<std::uint64_t>(signedValue);
    } else if (value.is_string()) {
        output = parse_unsigned(value.string(label), label);
    } else {
        throw Error(std::string(label) + " must be an integer or quoted 0x value");
    }
    if (output > maximum) throw Error(std::string(label) + " exceeds its field width");
    return output;
}

[[nodiscard]] std::string hex(std::uint64_t value, unsigned width) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(width)
           << std::setfill('0') << value;
    return output.str();
}

[[nodiscard]] std::string json_escape(std::string_view input) {
    std::string output;
    for (const unsigned char value : input) {
        switch (value) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (value < 0x20) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<unsigned>(value);
                    output += escaped.str();
                } else output.push_back(static_cast<char>(value));
        }
    }
    return output;
}

void check_output_identity(const fs::path& output, std::uint16_t packageId,
                           std::uint16_t patchId) {
    const std::regex pattern(R"(.+_([0-9a-fA-F]{4})_([0-9]+)\.pkg)", std::regex::icase);
    std::smatch match;
    const std::string name = output.filename().string();
    if (!std::regex_match(name, match, pattern)
        || parse_unsigned("0x" + match[1].str(), "output package id") != packageId
        || parse_unsigned(match[2].str(), "output patch id") != patchId) {
        throw Error("output filename must end in _" + [&] {
            std::ostringstream id; id << std::hex << std::nouppercase << std::setw(4)
                                      << std::setfill('0') << packageId; return id.str(); }()
                    + "_" + std::to_string(patchId) + ".pkg");
    }
}

void refuse_existing(const fs::path& output, bool force) {
    if (fs::exists(output) && !force) {
        throw Error("output already exists; pass --force to replace it");
    }
}

struct PackageName {
    std::string name;
    std::string family;
    std::uint16_t packageId{};
    std::uint16_t patchId{};
};

[[nodiscard]] PackageName parse_package_name(const json::Value& value,
                                             bool requirePatch,
                                             const json::Value* explicitPackageId = nullptr) {
    const std::string name = value.string("name");
    const std::regex pattern(R"((w64_[A-Za-z0-9_]+_([0-9A-Fa-f]{4}))_([0-9]+))");
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) {
        if (requirePatch || explicitPackageId == nullptr) {
            throw Error("name must look like w64_<family>_<4-hex-package-id>_<patch> without .pkg");
        }
        const std::regex stockPattern(R"((w64_[A-Za-z0-9_]+)_([0-9]+))");
        if (!std::regex_match(name, match, stockPattern)) {
            throw Error("name must be a w64 package filename stem ending in _<patch>");
        }
        const std::uint64_t packageId = integer(*explicitPackageId, "package_id", 0xFFFF);
        const std::uint64_t patchId = parse_unsigned(match[2].str(), "name patch id");
        if (packageId < 0x100 || packageId > 0x19FF) {
            throw Error("package_id is outside 0x0100..0x19ff");
        }
        if (patchId > 0xFF) throw Error("package name must select generation 0..255");
        return PackageName{name, match[1].str(), static_cast<std::uint16_t>(packageId),
                           static_cast<std::uint16_t>(patchId)};
    }
    const std::uint64_t packageId = parse_unsigned("0x" + match[2].str(), "name package id");
    const std::uint64_t patchId = parse_unsigned(match[3].str(), "name patch id");
    if (packageId < 0x100 || packageId > 0x19FF) {
        throw Error("name package id is outside 0x0100..0x19ff");
    }
    if (patchId > 0xFF || (requirePatch && patchId == 0)) {
        throw Error(requirePatch ? "patch name must select generation 1..255"
                                 : "package name must select generation 0..255");
    }
    if (explicitPackageId != nullptr
        && integer(*explicitPackageId, "package_id", 0xFFFF) != packageId) {
        throw Error("package_id does not match the id encoded in name");
    }
    return PackageName{name, match[1].str(), static_cast<std::uint16_t>(packageId),
                       static_cast<std::uint16_t>(patchId)};
}

[[nodiscard]] fs::path manifest_package_directory(const json::Value& manifest,
                                                   const fs::path& manifestDirectory) {
    const fs::path gameDirectory = resolve_path(
        manifestDirectory, manifest.require("game_dir"), "game_dir");
    const fs::path packages = gameDirectory / L"packages";
    if (!fs::is_directory(packages)) {
        throw Error("game package directory does not exist: " + packages.string());
    }
    return packages;
}

[[nodiscard]] bool has_flag(const std::vector<std::wstring>& arguments,
                            std::wstring_view flag) {
    return std::find(arguments.begin(), arguments.end(), flag) != arguments.end();
}

[[nodiscard]] json::Value load_manifest(const fs::path& path) {
    try {
        json::Value manifest = json::parse(read_text(path));
        (void)manifest.object("manifest root");
        if (manifest.find("runtime") != nullptr) {
            throw Error("manifest 'runtime' is obsolete; place oo2core_3_win64.dll beside repkg.exe");
        }
        return manifest;
    } catch (const json::Error& error) {
        throw Error(error.what());
    }
}

[[nodiscard]] Profile manifest_profile(const json::Value& manifest,
                                       const fs::path& directory) {
    const json::Value* profile = manifest.find("profile");
    if (profile == nullptr) return Profile::builtin();
    if (profile->is_string()) {
        return Profile::from_package(
            Package::open(resolve_path(directory, *profile, "profile"), false));
    }
    (void)profile->object("profile");
    return Profile{
        integer(profile->require("group_id"), "profile.group_id"),
        integer(profile->require("build_time"), "profile.build_time"),
        static_cast<std::uint32_t>(integer(
            profile->require("content_build"), "profile.content_build", 0xFFFFFFFFULL)),
        static_cast<std::uint32_t>(integer(
            profile->require("content_revision"), "profile.content_revision", 0xFFFFFFFFULL)),
        static_cast<std::uint8_t>(integer(
            profile->require("language"), "profile.language", 0xFF)),
        static_cast<std::uint32_t>(integer(
            profile->require("header_word_a4"), "profile.header_word_a4", 0xFFFFFFFFULL)),
        static_cast<std::uint32_t>(integer(
            profile->require("header_word_a8"), "profile.header_word_a8", 0xFFFFFFFFULL)),
        static_cast<std::uint32_t>(integer(
            profile->require("header_word_ac"), "profile.header_word_ac", 0xFFFFFFFFULL)),
        static_cast<std::uint32_t>(integer(
            profile->require("unknown_ec"), "profile.unknown_ec", 0xFFFFFFFFULL)),
        static_cast<std::uint8_t>(integer(
            profile->require("locale_check_enable"), "profile.locale_check_enable", 0xFF)),
        static_cast<std::uint32_t>(integer(
            profile->require("locale_token"), "profile.locale_token", 0xFFFFFFFFULL)),
        static_cast<std::uint8_t>(integer(
            profile->require("locale_id"), "profile.locale_id", 0xFF)),
    };
}

void print_package(const Package& package, bool entries, bool lookups) {
    std::cout << "{\n"
              << "  \"path\": \"" << json_escape(package.path.string()) << "\",\n"
              << "  \"version\": " << package.header.version << ",\n"
              << "  \"platform\": " << package.header.platform << ",\n"
              << "  \"variant\": \"" << (package.header.pre_bl() ? "d2_prebl" : "d2_beta") << "\",\n"
              << "  \"package_id\": \"" << hex(package.header.packageId, 4) << "\",\n"
              << "  \"patch_id\": " << package.header.patchId << ",\n"
              << "  \"file_size\": " << package.header.fileSize << ",\n"
              << "  \"entry_count\": " << package.entries.size() << ",\n"
              << "  \"block_count\": " << package.blocks.size() << ",\n"
              << "  \"named_tag_count\": " << package.namedTags.size() << ",\n"
              << "  \"wide_hash_count\": " << package.wideHashes.size() << ",\n"
              << "  \"tag_pair_count\": " << package.tagPairs.size();
    if (entries) {
        std::cout << ",\n  \"entries\": [";
        for (std::size_t index = 0; index < package.entries.size(); ++index) {
            const Entry& entry = package.entries[index];
            std::cout << (index == 0 ? "\n" : ",\n")
                      << "    {\"index\": " << index
                      << ", \"tag\": \"" << hex(tag_for(package.header.packageId, static_cast<std::uint32_t>(index)), 8)
                      << "\", \"reference\": \"" << hex(entry.reference, 8)
                      << "\", \"type_info\": \"" << hex(entry.typeInfo, 8)
                      << "\", \"starting_block\": " << entry.starting_block()
                      << ", \"starting_offset\": " << entry.starting_offset()
                      << ", \"size\": " << entry.logical_size() << "}";
        }
        std::cout << (package.entries.empty() ? "" : "\n  ") << "]";
    }
    if (lookups) {
        std::cout << ",\n  \"named_tags\": [";
        for (std::size_t index = 0; index < package.namedTags.size(); ++index) {
            const auto& row = package.namedTags[index];
            std::cout << (index == 0 ? "\n" : ",\n")
                      << "    {\"name\": \"" << json_escape(row.name)
                      << "\", \"tag\": \"" << hex(tag_for(package.header.packageId, row.entryIndex), 8)
                      << "\", \"class_id\": \"" << hex(row.classId, 8) << "\"}";
        }
        std::cout << (package.namedTags.empty() ? "" : "\n  ") << "],\n  \"wide_hashes\": [";
        for (std::size_t index = 0; index < package.wideHashes.size(); ++index) {
            const auto& row = package.wideHashes[index];
            std::cout << (index == 0 ? "\n" : ",\n")
                      << "    {\"wide_hash\": \"" << hex(row.hash, 16)
                      << "\", \"tag\": \"" << hex(tag_for(package.header.packageId, row.entryIndex), 8)
                      << "\", \"class_id\": \"" << hex(row.classId, 8) << "\"}";
        }
        std::cout << (package.wideHashes.empty() ? "" : "\n  ") << "]";
    }
    std::cout << "\n}\n";
}

int command_inspect(const std::vector<std::wstring>& arguments) {
    if (arguments.size() < 1) throw Error("usage: repkg inspect PACKAGE [--verify-blocks] [--list-entries] [--list-lookups]");
    const Package package = Package::open(arguments[0], has_flag(arguments, L"--verify-blocks"));
    print_package(package, has_flag(arguments, L"--list-entries"),
                  has_flag(arguments, L"--list-lookups"));
    return 0;
}

int command_build(const std::vector<std::wstring>& arguments) {
    if (arguments.empty()) throw Error("usage: repkg MANIFEST [OUTPUT] [--force] [--no-verify]");
    const fs::path manifestPath = fs::absolute(arguments[0]);
    const bool force = has_flag(arguments, L"--force");
    const bool verify = !has_flag(arguments, L"--no-verify");
    const json::Value manifest = load_manifest(manifestPath);
    if (manifest.require("patch").boolean("patch")) {
        throw Error("manifest declares a patch package but reached the standalone builder");
    }
    const fs::path directory = manifestPath.parent_path();
    if (manifest.find("patch_id") != nullptr) {
        throw Error("package manifests derive the patch id from 'name'");
    }
    if (manifest.find("game_dir") != nullptr) {
        throw Error("standalone package manifests do not use 'game_dir'; output is written beside the manifest");
    }
    const PackageName packageName = parse_package_name(
        manifest.require("name"), false, manifest.find("package_id"));
    const fs::path defaultOutput = directory / widen(packageName.name + ".pkg");
    const bool hasOutputOverride = arguments.size() >= 2 && !arguments[1].starts_with(L"--");
    const std::size_t optionStart = hasOutputOverride ? 2 : 1;
    for (std::size_t index = optionStart; index < arguments.size(); ++index) {
        if (arguments[index] != L"--force" && arguments[index] != L"--no-verify") {
            throw Error("unknown build option '" + narrow(arguments[index]) + "'");
        }
    }
    const fs::path output = hasOutputOverride ? fs::absolute(arguments[1]) : defaultOutput;
    if (_stricmp(output.filename().string().c_str(),
                 (packageName.name + ".pkg").c_str()) != 0) {
        throw Error("package output filename must be " + packageName.name + ".pkg");
    }
    refuse_existing(output, force);

    const auto& rawEntries = manifest.require("entries").array("entries");
    struct CompiledEntry {
        std::string resource;
        std::string role;
        std::vector<std::byte> data;
    };
    std::map<std::uint32_t, CompiledEntry> compiledEntries;
    std::set<std::string> resourceIds;
    if (const json::Value* rawResources = manifest.find("resources")) {
        for (const json::Value& row : rawResources->array("resources")) {
            (void)row.object("resource");
            const std::string id = row.require("id").string("resource id");
            if (id.empty() || !resourceIds.insert(id).second) {
                throw Error("resource ids must be nonempty and unique");
            }
            const std::string type = row.require("type").string("resource type");
            if (type != "texture") {
                throw Error("unsupported editable resource type '" + type + "'");
            }
            const std::uint32_t headerIndex = static_cast<std::uint32_t>(integer(
                row.require("header_entry"), "texture header_entry", 0xFFFFFFFFULL));
            const std::uint32_t dataIndex = static_cast<std::uint32_t>(integer(
                row.require("data_entry"), "texture data_entry", 0xFFFFFFFFULL));
            const std::uint8_t subtype = static_cast<std::uint8_t>(integer(
                row.require("subtype"), "texture subtype", 0xFF));
            if (headerIndex >= rawEntries.size() || dataIndex >= rawEntries.size()
                || headerIndex == dataIndex || subtype < 1 || subtype > 3
                || compiledEntries.contains(headerIndex) || compiledEntries.contains(dataIndex)) {
                throw Error("texture resource entry mapping is invalid or duplicated");
            }
            TextureDescriptor templateDescriptor{};
            templateDescriptor.unknown08 = static_cast<std::uint32_t>(integer(
                row.require("unknown_08"), "texture unknown_08", 0xFFFFFFFFULL));
            const auto& words = row.require("unknown_words").array("texture unknown_words");
            if (words.size() != templateDescriptor.unknownWords.size()) {
                throw Error("texture unknown_words must contain exactly seven values");
            }
            for (std::size_t index = 0; index < words.size(); ++index) {
                templateDescriptor.unknownWords[index] = static_cast<std::uint16_t>(integer(
                    words[index], "texture unknown word", 0xFFFF));
            }
            templateDescriptor.largeBuffer = static_cast<std::uint32_t>(integer(
                row.require("large_buffer"), "texture large_buffer", 0xFFFFFFFFULL));
            if (templateDescriptor.largeBuffer != 0xFFFFFFFFU) {
                throw Error("editable textures with a separate large buffer are not supported yet");
            }
            EditableTexture texture = read_texture_dds(
                resolve_path(directory, row.require("path"), "texture path"),
                templateDescriptor, subtype);
            compiledEntries.emplace(
                headerIndex,
                CompiledEntry{id, "header", encode_texture_descriptor(texture.descriptor)});
            compiledEntries.emplace(
                dataIndex, CompiledEntry{id, "data", std::move(texture.pixels)});
        }
    }

    std::vector<AuthoredEntry> entries;
    for (std::size_t index = 0; index < rawEntries.size(); ++index) {
        const auto& row = rawEntries[index];
        (void)row.object("entry");
        std::vector<std::byte> data;
        const auto compiled = compiledEntries.find(static_cast<std::uint32_t>(index));
        if (compiled != compiledEntries.end()) {
            if (row.find("path") != nullptr
                || row.require("resource").string("entry resource")
                       != compiled->second.resource
                || row.require("role").string("entry resource role")
                       != compiled->second.role) {
                throw Error("editable resource entry does not match its resource mapping");
            }
            data = std::move(compiled->second.data);
        } else {
            if (row.find("resource") != nullptr || row.find("role") != nullptr) {
                throw Error("entry refers to an editable resource that was not compiled");
            }
            data = read_binary(resolve_path(directory, row.require("path"), "entry path"));
        }
        entries.push_back(AuthoredEntry{
            static_cast<std::uint32_t>(integer(row.require("reference"), "entry reference", 0xFFFFFFFFULL)),
            static_cast<std::uint32_t>(integer(row.require("type_info"), "entry type_info", 0xFFFFFFFFULL)),
            std::move(data),
        });
    }
    std::vector<NamedTag> names;
    if (const json::Value* rows = manifest.find("named_tags")) {
        std::set<std::string> unique;
        for (const auto& row : rows->array("named_tags")) {
            const std::string name = row.require("name").string("named-tag name");
            if (!unique.insert(name).second) throw Error("named-tag names must be unique");
            names.push_back(NamedTag{
                static_cast<std::uint32_t>(integer(row.require("entry_index"), "named-tag entry_index", 0xFFFFFFFFULL)),
                static_cast<std::uint32_t>(integer(row.require("class_id"), "named-tag class_id", 0xFFFFFFFFULL)),
                name,
            });
        }
    }
    std::vector<WideHash> hashes;
    if (const json::Value* rows = manifest.find("wide_hashes")) {
        for (const auto& row : rows->array("wide_hashes")) {
            hashes.push_back(WideHash{
                integer(row.require("wide_hash"), "wide_hash"),
                static_cast<std::uint32_t>(integer(row.require("entry_index"), "wide-hash entry_index", 0xFFFFFFFFULL)),
                static_cast<std::uint32_t>(integer(row.require("class_id"), "wide-hash class_id", 0xFFFFFFFFULL)),
            });
        }
    }
    std::vector<TagPair> tagPairs;
    if (const json::Value* rows = manifest.find("tag_pairs")) {
        for (const auto& row : rows->array("tag_pairs")) {
            tagPairs.push_back(TagPair{
                static_cast<std::uint32_t>(integer(
                    row.require("first_tag"), "tag-pair first_tag", 0xFFFFFFFFULL)),
                static_cast<std::uint32_t>(integer(
                    row.require("second_tag"), "tag-pair second_tag", 0xFFFFFFFFULL)),
            });
        }
    }
    bool alternateKey = false;
    if (const json::Value* value = manifest.find("block_key")) {
        const std::string mode = value->string("block_key");
        if (mode == "alternate") alternateKey = true;
        else if (mode != "primary") {
            throw Error("block_key must be 'primary' or 'alternate'");
        }
    }
    Runtime runtime;
    const Package package = build_package(output, packageName.packageId, packageName.patchId,
                                          manifest_profile(manifest, directory), entries,
                                          names, hashes, tagPairs, alternateKey, runtime);
    if (verify) {
        ChainGuard chain(make_isolated_chain(package));
        Runtime verifier;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].data.empty()) continue;
            const auto decoded = verifier.read_tag(
                chain.path(), tag_for(packageName.packageId, static_cast<std::uint32_t>(index)));
            if (decoded != entries[index].data) throw Error("native round-trip mismatch for entry " + std::to_string(index));
        }
    }
    print_package(package, false, false);
    return 0;
}

struct PatchPaths {
    std::string name;
    std::string family;
    std::uint16_t packageId{};
    std::uint16_t patchId{};
    fs::path base;
    fs::path target;
};

[[nodiscard]] PatchPaths patch_paths(const json::Value& manifest,
                                     const fs::path& manifestDirectory) {
    if (manifest.find("base") != nullptr || manifest.find("family") != nullptr
        || manifest.find("packages") != nullptr) {
        throw Error("patch manifests now use 'game_dir' and target 'name', not base/family/packages");
    }
    const PackageName parsed = parse_package_name(manifest.require("name"), true);
    const fs::path packageDirectory = manifest_package_directory(manifest, manifestDirectory);
    const std::string predecessor = parsed.family + "_"
                                    + std::to_string(parsed.patchId - 1) + ".pkg";
    const fs::path base = packageDirectory / widen(predecessor);
    if (!fs::is_regular_file(base)) {
        throw Error("required predecessor package does not exist: " + base.string());
    }
    return PatchPaths{parsed.name, parsed.family, parsed.packageId, parsed.patchId, base,
                      packageDirectory / widen(parsed.name + ".pkg")};
}

[[nodiscard]] std::uint32_t u32_at(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || 4 > bytes.size() - offset) throw Error("localized container is truncated");
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

int command_build_patch(const std::vector<std::wstring>& arguments) {
    if (arguments.empty()) throw Error("usage: repkg MANIFEST [OUTPUT] [--force] [--no-verify]");
    const fs::path manifestPath = fs::absolute(arguments[0]);
    const bool force = has_flag(arguments, L"--force");
    const bool verify = !has_flag(arguments, L"--no-verify");
    const json::Value manifest = load_manifest(manifestPath);
    if (!manifest.require("patch").boolean("patch")) {
        throw Error("manifest declares a standalone package but reached the patch builder");
    }
    const fs::path directory = manifestPath.parent_path();
    const PatchPaths paths = patch_paths(manifest, directory);
    const bool hasOutputOverride = arguments.size() >= 2 && !arguments[1].starts_with(L"--");
    const std::size_t optionStart = hasOutputOverride ? 2 : 1;
    for (std::size_t index = optionStart; index < arguments.size(); ++index) {
        if (arguments[index] != L"--force" && arguments[index] != L"--no-verify") {
            throw Error("unknown build-patch option '" + narrow(arguments[index]) + "'");
        }
    }
    const fs::path output = hasOutputOverride ? fs::absolute(arguments[1]) : paths.target;
    const Package base = Package::open(paths.base, true);
    if (base.header.packageId != paths.packageId || base.header.patchId + 1 != paths.patchId) {
        throw Error("predecessor header identity does not match target name");
    }
    check_output_identity(output, paths.packageId, paths.patchId);
    const std::string expected = paths.name + ".pkg";
    if (_stricmp(output.filename().string().c_str(), expected.c_str()) != 0) {
        throw Error("patch output filename must be " + expected);
    }
    refuse_existing(output, force);
    ChainGuard source(make_isolated_chain(base));
    Runtime runtime;
    std::map<std::uint32_t, std::vector<std::byte>> payloads;
    for (const auto& action : manifest.require("replacements").array("replacements")) {
        const std::string kind = action.require("type").string("replacement type");
        if (kind == "entry") {
            const json::Value* tagValue = action.find("tag");
            const json::Value* indexValue = action.find("entry_index");
            if ((tagValue == nullptr) == (indexValue == nullptr)) throw Error("entry replacement needs exactly one of tag or entry_index");
            std::uint32_t index = 0;
            if (tagValue != nullptr) {
                const std::uint32_t tag = static_cast<std::uint32_t>(integer(*tagValue, "replacement tag", 0xFFFFFFFFULL));
                if (tag_package(tag) != base.header.packageId) throw Error("replacement tag belongs to another package");
                index = tag_entry(tag);
            } else index = static_cast<std::uint32_t>(integer(*indexValue, "replacement entry_index", 0xFFFFFFFFULL));
            if (index >= base.entries.size() || payloads.contains(index)) throw Error("replacement entry index is invalid or duplicated");
            payloads[index] = read_binary(resolve_path(directory, action.require("path"), "replacement path"));
        } else if (kind == "localized_text") {
            const std::uint32_t containerTag = static_cast<std::uint32_t>(integer(action.require("container_tag"), "container_tag", 0xFFFFFFFFULL));
            const std::uint32_t stringHash = static_cast<std::uint32_t>(integer(action.require("string_hash"), "string_hash", 0xFFFFFFFFULL));
            const std::string text = action.require("text").string("localized text");
            if (tag_package(containerTag) != base.header.packageId) throw Error("localized container belongs to another package");
            const auto container = runtime.read_tag(source.path(), containerTag);
            if (container.size() < 0x1C) throw Error("localized container is truncated");
            const std::uint32_t englishTag = u32_at(container, 0x18);
            if (tag_package(englishTag) != base.header.packageId) throw Error("localized English data belongs to another package");
            const std::uint32_t englishIndex = tag_entry(englishTag);
            const auto found = payloads.find(englishIndex);
            const std::vector<std::byte> english = found == payloads.end()
                ? runtime.read_tag(source.path(), englishTag) : found->second;
            LocalizedResult details{};
            payloads[englishIndex] = replace_localized_string(container, english, stringHash, text, details);
            std::cout << "localized " << hex(stringHash, 8) << ": \""
                      << json_escape(details.original) << "\" -> \""
                      << json_escape(details.replacement) << "\"\n";
        } else {
            throw Error("unsupported replacement type '" + kind + "'");
        }
    }
    std::vector<Replacement> replacements;
    for (auto& [index, payload] : payloads) {
        if (payload.empty()) throw Error("replacement payload must not be empty");
        replacements.push_back(Replacement{index, std::move(payload)});
    }
    const Package package = build_patch(output, base, replacements, runtime);
    if (verify) {
        ChainGuard chain(make_isolated_chain(base, &output));
        Runtime verifier;
        for (const auto& replacement : replacements) {
            const auto decoded = verifier.read_tag(
                chain.path(), tag_for(base.header.packageId, replacement.entryIndex));
            if (decoded != replacement.data) throw Error("native round-trip mismatch for entry " + std::to_string(replacement.entryIndex));
        }
    }
    print_package(package, false, false);
    return 0;
}

int command_extract(const std::vector<std::wstring>& arguments) {
    if (arguments.size() != 3) throw Error("usage: repkg extract PACKAGES TAG OUTPUT");
    Runtime runtime;
    const std::uint32_t tag = static_cast<std::uint32_t>(parse_unsigned(narrow(arguments[1]), "tag"));
    write_binary(arguments[2], runtime.read_tag(fs::absolute(arguments[0]), tag));
    std::cout << "extracted " << hex(tag, 8) << " to " << fs::path(arguments[2]).string() << "\n";
    return 0;
}

int command_localized(const std::vector<std::wstring>& arguments) {
    if (arguments.size() != 5) throw Error("usage: repkg replace-localized-string CONTAINER ENGLISH HASH TEXT OUTPUT");
    LocalizedResult result{};
    const auto output = replace_localized_string(read_binary(arguments[0]), read_binary(arguments[1]),
        static_cast<std::uint32_t>(parse_unsigned(narrow(arguments[2]), "string hash")),
        narrow(arguments[3]), result);
    write_binary(arguments[4], output);
    std::cout << "replaced \"" << result.original << "\" with \"" << result.replacement
              << "\" (" << result.oldByteLength << " -> " << result.newByteLength << " bytes)\n";
    return 0;
}

int command_verify_directory(const std::vector<std::wstring>& arguments) {
    if (arguments.empty()) throw Error("usage: repkg verify-directory PACKAGES [--stock-only] [--verify-blocks]");
    const fs::path directory = fs::absolute(arguments[0]);
    const bool stockOnly = has_flag(arguments, L"--stock-only");
    const bool blocks = has_flag(arguments, L"--verify-blocks");
    std::size_t checked = 0;
    std::size_t skipped = 0;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (!item.is_regular_file() || _wcsicmp(item.path().extension().c_str(), L".pkg") != 0) continue;
        Package package;
        try {
            package = Package::open(item.path(), blocks);
        } catch (const std::exception& error) {
            throw Error(item.path().filename().string() + ": " + error.what());
        }
        if (stockOnly && package.header.packageId >= 0xAA0) { ++skipped; continue; }
        ++checked;
    }
    std::cout << "{\n  \"directory\": \"" << json_escape(directory.string())
              << "\",\n  \"checked\": " << checked << ",\n  \"skipped\": " << skipped
              << ",\n  \"physical_block_hashes_checked\": " << (blocks ? "true" : "false") << "\n}\n";
    return 0;
}

int command_manifest(const std::vector<std::wstring>& arguments) {
    if (arguments.empty()) {
        throw Error("usage: repkg MANIFEST [OUTPUT] [--force] [--no-verify]");
    }
    const fs::path manifestPath = fs::absolute(arguments[0]);
    const json::Value manifest = load_manifest(manifestPath);
    const bool patch = manifest.require("patch").boolean("patch");
    return patch ? command_build_patch(arguments) : command_build(arguments);
}

void usage() {
    std::cout
        << "repkg - Sunrise Tiger package compiler\n\n"
        << "Package creation:\n"
        << "  repkg MANIFEST [OUTPUT] [--force] [--no-verify]\n\n"
        << "Utilities:\n"
        << "  repkg inspect PACKAGE [--verify-blocks] [--list-entries] [--list-lookups]\n"
        << "  repkg extract PACKAGES TAG OUTPUT\n"
        << "  repkg replace-localized-string CONTAINER ENGLISH HASH TEXT OUTPUT\n"
        << "  repkg verify-directory PACKAGES [--stock-only] [--verify-blocks]\n\n"
        << "Place oo2core_3_win64.dll beside repkg.exe. Package keys and the verified\n"
        << "Sunrise build profile are compiled into repkg.\n";
}

} // namespace
} // namespace repkg

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2) {
            repkg::usage();
            return 0;
        }
        const std::wstring command = argv[1];
        std::vector<std::wstring> arguments;
        for (int index = 2; index < argc; ++index) arguments.emplace_back(argv[index]);
        if (command == L"inspect") return repkg::command_inspect(arguments);
        if (command == L"extract") return repkg::command_extract(arguments);
        if (command == L"replace-localized-string") return repkg::command_localized(arguments);
        if (command == L"verify-directory") return repkg::command_verify_directory(arguments);
        if (command == L"help" || command == L"--help" || command == L"-h") {
            repkg::usage();
            return 0;
        }
        if (std::filesystem::path(command).extension() == L".json") {
            std::vector<std::wstring> manifestArguments;
            manifestArguments.push_back(command);
            manifestArguments.insert(manifestArguments.end(), arguments.begin(), arguments.end());
            return repkg::command_manifest(manifestArguments);
        }
        throw repkg::Error("unknown command '" + repkg::narrow(command) + "'");
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    }
}
