#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

class OutputGuard {
public:
    explicit OutputGuard(fs::path path) : path_(std::move(path)) {}
    OutputGuard(const OutputGuard&) = delete;
    OutputGuard& operator=(const OutputGuard&) = delete;
    ~OutputGuard() {
        if (active_) remove_tree(path_);
    }
    void release() noexcept { active_ = false; }
private:
    fs::path path_;
    bool active_{true};
};

[[nodiscard]] std::string escape(std::string_view input) {
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
                    std::ostringstream encoded;
                    encoded << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<unsigned>(value);
                    output += encoded.str();
                } else {
                    output.push_back(static_cast<char>(value));
                }
        }
    }
    return output;
}

[[nodiscard]] std::string hex(std::uint64_t value, unsigned width) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(width)
           << std::setfill('0') << value;
    return output.str();
}

[[nodiscard]] std::uint8_t file_type(const Entry& entry) noexcept {
    return static_cast<std::uint8_t>((entry.typeInfo >> 9U) & 0x7FU);
}

[[nodiscard]] std::uint8_t file_subtype(const Entry& entry) noexcept {
    return static_cast<std::uint8_t>((entry.typeInfo >> 6U) & 0x7U);
}

[[nodiscard]] bool starts_with(std::span<const std::byte> bytes,
                               std::initializer_list<unsigned char> prefix) noexcept {
    if (bytes.size() < prefix.size()) return false;
    std::size_t index = 0;
    for (const unsigned char value : prefix) {
        if (bytes[index++] != static_cast<std::byte>(value)) return false;
    }
    return true;
}

[[nodiscard]] std::string detected_extension(std::span<const std::byte> data) {
    if (starts_with(data, {'R', 'I', 'F', 'F'}) && data.size() >= 12
        && starts_with(data.subspan(8), {'W', 'A', 'V', 'E'})) return ".wem";
    if (starts_with(data, {'B', 'K', 'H', 'D'})) return ".bnk";
    if (starts_with(data, {'D', 'X', 'B', 'C'})) return ".dxbc";
    if (starts_with(data, {'D', 'D', 'S', ' '})) return ".dds";
    if (starts_with(data, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return ".png";
    if (starts_with(data, {0xFF, 0xD8, 0xFF})) return ".jpg";
    if (starts_with(data, {'O', 'g', 'g', 'S'})) return ".ogg";
    if (starts_with(data, {'C', 'R', 'I', 'D'})) return ".usm";
    if (starts_with(data, {'O', 'T', 'T', 'O'})
        || starts_with(data, {0x00, 0x01, 0x00, 0x00})) return ".otf";
    return ".bin";
}

[[nodiscard]] std::string asset_name(std::size_t index,
                                     std::span<const std::byte> data) {
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setw(4) << std::setfill('0') << index
           << detected_extension(data);
    return output.str();
}

struct TextureResource {
    std::string id;
    std::string path;
    std::uint32_t headerEntry{};
    std::uint32_t dataEntry{};
    std::uint8_t subtype{};
    TextureDescriptor descriptor;
};

[[nodiscard]] bool has_flag(const std::vector<std::wstring>& arguments,
                            std::wstring_view flag) {
    return std::find(arguments.begin(), arguments.end(), flag) != arguments.end();
}

[[nodiscard]] bool contains_path(const fs::path& parent, const fs::path& child) {
    auto parentPart = parent.begin();
    auto childPart = child.begin();
    for (; parentPart != parent.end() && childPart != child.end(); ++parentPart, ++childPart) {
        if (*parentPart != *childPart) return false;
    }
    return parentPart == parent.end();
}

void write_profile(std::ostream& output, const Header& header) {
    output << "  \"profile\": {\n"
           << "    \"group_id\": \"" << hex(header.groupId, 16) << "\",\n"
           << "    \"build_time\": " << header.buildTime << ",\n"
           << "    \"content_build\": \"" << hex(header.contentBuild, 8) << "\",\n"
           << "    \"content_revision\": " << header.contentRevision << ",\n"
           << "    \"language\": " << static_cast<unsigned>(header.language) << ",\n"
           << "    \"header_word_a4\": \"" << hex(header.wordA4, 8) << "\",\n"
           << "    \"header_word_a8\": \"" << hex(header.wordA8, 8) << "\",\n"
           << "    \"header_word_ac\": \"" << hex(header.wordAC, 8) << "\",\n"
           << "    \"unknown_ec\": \"" << hex(header.unknownEC, 8) << "\",\n"
           << "    \"locale_check_enable\": "
           << static_cast<unsigned>(header.localeCheckEnable) << ",\n"
           << "    \"locale_token\": \"" << hex(header.localeToken, 8) << "\",\n"
           << "    \"locale_id\": " << static_cast<unsigned>(header.localeId) << "\n"
           << "  },\n";
}

void unpack(const fs::path& input, const fs::path& destination, bool force) {
    const fs::path resolvedInput = fs::absolute(input).lexically_normal();
    const fs::path resolvedDestination = fs::absolute(destination).lexically_normal();
    if (resolvedDestination == resolvedDestination.root_path()
        || contains_path(resolvedDestination, resolvedInput)) {
        throw Error("output directory may not contain the input package");
    }
    const Package package = Package::open(resolvedInput, true);
    if (!package.header.pre_bl()) {
        throw Error("depkg rebuild output currently requires a d2_prebl source package");
    }
    if (fs::exists(destination) && !force) {
        throw Error("output directory already exists; pass --force to replace it");
    }
    fs::path temporary = destination;
    temporary += L".depkg.tmp";
    remove_tree(temporary);
    fs::create_directories(temporary / L"assets");
    OutputGuard temporaryGuard(temporary);
    ChainGuard chain(make_isolated_chain(package));
    Runtime runtime;

    std::vector<TextureResource> textures;
    std::vector<int> textureForEntry(package.entries.size(), -1);
    std::vector<bool> textureHeaderRole(package.entries.size(), false);
    for (std::size_t headerIndex = 0; headerIndex < package.entries.size(); ++headerIndex) {
        const Entry& headerEntry = package.entries[headerIndex];
        const std::uint8_t subtype = file_subtype(headerEntry);
        if (file_type(headerEntry) != 32 || subtype < 1 || subtype > 3
            || headerEntry.reference < kTagBase
            || tag_package(headerEntry.reference) != package.header.packageId) {
            continue;
        }
        const std::uint32_t dataIndex = tag_entry(headerEntry.reference);
        if (dataIndex >= package.entries.size() || textureForEntry[dataIndex] != -1) continue;
        const Entry& dataEntry = package.entries[dataIndex];
        if (file_type(dataEntry) != 40 || file_subtype(dataEntry) != subtype
            || dataEntry.reference
                   != tag_for(package.header.packageId, static_cast<std::uint32_t>(headerIndex))) {
            continue;
        }
        const std::vector<std::byte> header = runtime.read_tag(
            chain.path(), tag_for(package.header.packageId,
                                  static_cast<std::uint32_t>(headerIndex)));
        TextureDescriptor descriptor{};
        if (!parse_texture_descriptor(header, descriptor)
            || descriptor.largeBuffer != 0xFFFFFFFFU
            || descriptor.dataSize != dataEntry.logical_size()) {
            continue;
        }
        const std::vector<std::byte> pixels = runtime.read_tag(
            chain.path(), tag_for(package.header.packageId, dataIndex));
        std::ostringstream id;
        id << "texture_" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0')
           << tag_for(package.header.packageId, static_cast<std::uint32_t>(headerIndex));
        const std::string resourceId = id.str();
        const std::string path = "assets/" + resourceId + ".dds";
        write_texture_dds(temporary / fs::path(path), descriptor, pixels, subtype);
        const int resourceIndex = static_cast<int>(textures.size());
        textures.push_back(TextureResource{resourceId, path,
                                           static_cast<std::uint32_t>(headerIndex), dataIndex,
                                           subtype, descriptor});
        textureForEntry[headerIndex] = resourceIndex;
        textureForEntry[dataIndex] = resourceIndex;
        textureHeaderRole[headerIndex] = true;
    }

    bool hasPrimaryKeyBlocks = false;
    bool hasAlternateKeyBlocks = false;
    for (const Block& block : package.blocks) {
        if ((block.flags & 0x2U) == 0) continue;
        if ((block.flags & 0x4U) != 0) hasAlternateKeyBlocks = true;
        else hasPrimaryKeyBlocks = true;
    }
    if (hasPrimaryKeyBlocks && hasAlternateKeyBlocks) {
        throw Error("packages mixing primary and alternate encrypted blocks are not yet rebuildable");
    }

    std::ofstream manifest(temporary / L"package.json", std::ios::binary | std::ios::trunc);
    if (!manifest) throw Error("could not create depkg manifest");
    manifest << "{\n"
             << "  \"patch\": false,\n"
             << "  \"name\": \"" << escape(resolvedInput.stem().string()) << "\",\n"
             << "  \"package_id\": \"" << hex(package.header.packageId, 4) << "\",\n"
             << "  \"block_key\": \""
             << (hasAlternateKeyBlocks ? "alternate" : "primary") << "\",\n"
             << "  \"source\": {\n"
             << "    \"path\": \"" << escape(resolvedInput.string()) << "\",\n"
             << "    \"patch_generation\": " << package.header.patchId << ",\n"
             << "    \"flattened\": true\n"
             << "  },\n";
    write_profile(manifest, package.header);
    manifest << "  \"entries\": [\n";
    for (std::size_t index = 0; index < package.entries.size(); ++index) {
        const Entry& entry = package.entries[index];
        std::vector<std::byte> data;
        if (textureForEntry[index] == -1 && entry.logical_size() != 0) {
            std::uint32_t classId = 0;
            data = runtime.read_tag(chain.path(),
                                    tag_for(package.header.packageId,
                                            static_cast<std::uint32_t>(index)),
                                    &classId);
            if (classId != entry.reference || data.size() != entry.logical_size()) {
                throw Error("decoded entry " + std::to_string(index)
                            + " does not match its directory row");
            }
        }
        manifest << "    {\n"
                 << "      \"reference\": \"" << hex(entry.reference, 8) << "\",\n"
                 << "      \"type_info\": \"" << hex(entry.typeInfo, 8) << "\",\n";
        if (textureForEntry[index] != -1) {
            const TextureResource& texture = textures[textureForEntry[index]];
            manifest << "      \"resource\": \"" << texture.id << "\",\n"
                     << "      \"role\": \""
                     << (textureHeaderRole[index] ? "header" : "data") << "\"\n";
        } else {
            const std::string filename = asset_name(index, data);
            write_binary(temporary / L"assets" / fs::path(filename), data);
            manifest << "      \"path\": \"assets/" << filename << "\"\n";
        }
        manifest
                 << "    }" << (index + 1 == package.entries.size() ? "\n" : ",\n");
        if ((index + 1) % 256 == 0 || index + 1 == package.entries.size()) {
            std::cerr << "extracted " << (index + 1) << "/" << package.entries.size()
                      << " entries\r";
        }
    }
    std::cerr << "\n";
    manifest << "  ],\n  \"resources\": [";
    for (std::size_t index = 0; index < textures.size(); ++index) {
        const TextureResource& texture = textures[index];
        manifest << (index == 0 ? "\n" : ",\n")
                 << "    {\n"
                 << "      \"id\": \"" << texture.id << "\",\n"
                 << "      \"type\": \"texture\",\n"
                 << "      \"path\": \"" << texture.path << "\",\n"
                 << "      \"header_entry\": " << texture.headerEntry << ",\n"
                 << "      \"data_entry\": " << texture.dataEntry << ",\n"
                 << "      \"subtype\": " << static_cast<unsigned>(texture.subtype) << ",\n"
                 << "      \"unknown_08\": \"" << hex(texture.descriptor.unknown08, 8)
                 << "\",\n"
                 << "      \"unknown_words\": [";
        for (std::size_t word = 0; word < texture.descriptor.unknownWords.size(); ++word) {
            if (word != 0) manifest << ", ";
            manifest << "\"" << hex(texture.descriptor.unknownWords[word], 4) << "\"";
        }
        manifest << "],\n"
                 << "      \"large_buffer\": \""
                 << hex(texture.descriptor.largeBuffer, 8) << "\"\n"
                 << "    }";
    }
    manifest << (textures.empty() ? "" : "\n  ") << "],\n  \"named_tags\": [";
    for (std::size_t index = 0; index < package.namedTags.size(); ++index) {
        const NamedTag& row = package.namedTags[index];
        manifest << (index == 0 ? "\n" : ",\n")
                 << "    {\n"
                 << "      \"entry_index\": " << row.entryIndex << ",\n"
                 << "      \"class_id\": \"" << hex(row.classId, 8) << "\",\n"
                 << "      \"name\": \"" << escape(row.name) << "\"\n"
                 << "    }";
    }
    manifest << (package.namedTags.empty() ? "" : "\n  ") << "],\n  \"wide_hashes\": [";
    for (std::size_t index = 0; index < package.wideHashes.size(); ++index) {
        const WideHash& row = package.wideHashes[index];
        manifest << (index == 0 ? "\n" : ",\n")
                 << "    {\n"
                 << "      \"wide_hash\": \"" << hex(row.hash, 16) << "\",\n"
                 << "      \"entry_index\": " << row.entryIndex << ",\n"
                 << "      \"class_id\": \"" << hex(row.classId, 8) << "\"\n"
                 << "    }";
    }
    manifest << (package.wideHashes.empty() ? "" : "\n  ") << "],\n  \"tag_pairs\": [";
    for (std::size_t index = 0; index < package.tagPairs.size(); ++index) {
        const TagPair& row = package.tagPairs[index];
        manifest << (index == 0 ? "\n" : ",\n")
                 << "    {\n"
                 << "      \"first_tag\": \"" << hex(row.firstTag, 8) << "\",\n"
                 << "      \"second_tag\": \"" << hex(row.secondTag, 8) << "\"\n"
                 << "    }";
    }
    manifest << (package.tagPairs.empty() ? "" : "\n  ") << "]\n}\n";
    manifest.flush();
    if (!manifest) throw Error("failed while writing depkg manifest");
    manifest.close();

    if (fs::exists(destination)) remove_tree(destination);
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (error) throw Error("could not install completed depkg directory: " + error.message());
    temporaryGuard.release();
    std::cout << "unpacked " << package.entries.size() << " entries to "
              << destination.string() << "\n";
}

void usage() {
    std::cout << "depkg - unpack a Sunrise Tiger package into a rebuildable repkg project\n\n"
              << "  depkg PACKAGE [OUTPUT_DIRECTORY] [--force]\n\n"
              << "Patch generations are flattened by resolving inherited blocks from sibling\n"
              << "package files. Place oo2core_3_win64.dll beside depkg.exe.\n";
}

} // namespace
} // namespace repkg

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2 || std::wstring_view(argv[1]) == L"--help"
            || std::wstring_view(argv[1]) == L"-h") {
            repkg::usage();
            return argc < 2 ? 2 : 0;
        }
        std::vector<std::wstring> arguments;
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
        const std::filesystem::path input = std::filesystem::absolute(arguments[0]);
        const bool hasOutput = arguments.size() >= 2 && !arguments[1].starts_with(L"--");
        const std::size_t optionStart = hasOutput ? 2 : 1;
        for (std::size_t index = optionStart; index < arguments.size(); ++index) {
            if (arguments[index] != L"--force") {
                throw repkg::Error("unknown depkg option");
            }
        }
        const std::filesystem::path output = hasOutput
            ? std::filesystem::absolute(arguments[1])
            : std::filesystem::current_path() / input.stem();
        repkg::unpack(input, output, repkg::has_flag(arguments, L"--force"));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    }
}
