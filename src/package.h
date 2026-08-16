#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace sunrise::middleware::content::packages::reader {
struct Scratch;
}

namespace repkg {

inline constexpr std::size_t kHeaderSize = 0x170;
inline constexpr std::size_t kBlockSize = 0x40000;
inline constexpr std::size_t kEntryLimit = 8192;
inline constexpr std::uint32_t kTagBase = 0x80800000U;

class Error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Header {
    std::uint16_t version{};
    std::uint16_t platform{};
    std::uint16_t packageId{};
    std::uint8_t localeCheckEnable{};
    std::uint64_t groupId{};
    std::uint64_t buildTime{};
    std::uint32_t contentBuild{};
    std::uint32_t contentRevision{};
    std::uint16_t patchId{};
    std::uint8_t language{};
    std::uint32_t wordA4{};
    std::uint32_t wordA8{};
    std::uint32_t wordAC{};
    std::uint32_t signatureOffset{};
    std::uint32_t entryCount{};
    std::uint32_t betaEntryOffset{};
    std::uint32_t blockCount{};
    std::uint32_t betaBlockOffset{};
    std::uint32_t unknownEC{};
    std::uint32_t miscOffset{};
    std::uint32_t miscSize{};
    std::array<std::byte, 20> miscSha1{};
    std::uint32_t metadataOffset{};
    std::uint32_t metadataSize{};
    std::array<std::byte, 20> metadataSha1{};
    std::uint32_t footerOffset{};
    std::uint32_t fileSize{};
    std::uint32_t localeToken{};
    std::uint8_t localeId{};
    std::array<std::byte, kHeaderSize> raw{};

    [[nodiscard]] bool pre_bl() const noexcept { return raw[0x1A] == std::byte{1}; }
};

struct Entry {
    std::uint32_t reference{};
    std::uint32_t typeInfo{};
    std::uint64_t blockInfo{};

    [[nodiscard]] std::uint32_t starting_block() const noexcept;
    [[nodiscard]] std::uint32_t starting_offset() const noexcept;
    [[nodiscard]] std::uint64_t logical_size() const noexcept;
};

struct Block {
    std::uint32_t offset{};
    std::uint32_t size{};
    std::uint16_t patchId{};
    std::uint16_t flags{};
    std::array<std::byte, 20> digest{};
    std::array<std::byte, 16> tag{};
};

struct NamedTag {
    std::uint32_t entryIndex{};
    std::uint32_t classId{};
    std::string name;
};

struct WideHash {
    std::uint64_t hash{};
    std::uint32_t entryIndex{};
    std::uint32_t classId{};
};

struct TagPair {
    std::uint32_t firstTag{};
    std::uint32_t secondTag{};
};

struct Package {
    std::filesystem::path path;
    Header header;
    std::vector<Entry> entries;
    std::vector<Block> blocks;
    std::vector<NamedTag> namedTags;
    std::vector<WideHash> wideHashes;
    std::vector<TagPair> tagPairs;
    std::uint64_t entryTableOffset{};
    std::uint64_t blockTableOffset{};

    [[nodiscard]] static Package open(const std::filesystem::path& path,
                                      bool verifyPhysicalBlocks = true);
};

struct Profile {
    std::uint64_t groupId{};
    std::uint64_t buildTime{};
    std::uint32_t contentBuild{};
    std::uint32_t contentRevision{};
    std::uint8_t language{};
    std::uint32_t wordA4{};
    std::uint32_t wordA8{};
    std::uint32_t wordAC{};
    std::uint32_t unknownEC{};
    std::uint8_t localeCheckEnable{};
    std::uint32_t localeToken{};
    std::uint8_t localeId{};

    [[nodiscard]] static Profile builtin() noexcept;
    [[nodiscard]] static Profile from_package(const Package& package);
};

struct AuthoredEntry {
    std::uint32_t reference{};
    std::uint32_t typeInfo{};
    std::vector<std::byte> data;
};

struct Replacement {
    std::uint32_t entryIndex{};
    std::vector<std::byte> data;
};

class Runtime {
public:
    Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

    [[nodiscard]] const std::filesystem::path& oodle_path() const noexcept { return oodlePath_; }
    [[nodiscard]] std::vector<std::byte> encode_block(std::span<const std::byte> input,
                                                      std::uint16_t packageId,
                                                      bool alternateKey,
                                                      Block& record) const;
    [[nodiscard]] std::vector<std::byte> read_tag(const std::filesystem::path& directory,
                                                  std::uint32_t tag,
                                                  std::uint32_t* classId = nullptr) const;

private:
    HMODULE oodle_{};
    std::filesystem::path oodlePath_;
    mutable std::unique_ptr<sunrise::middleware::content::packages::reader::Scratch> scratch_;
    mutable std::filesystem::path readerDirectory_;
};

[[nodiscard]] std::uint32_t tag_for(std::uint16_t packageId,
                                    std::uint32_t entryIndex) noexcept;
[[nodiscard]] std::uint16_t tag_package(std::uint32_t tag);
[[nodiscard]] std::uint32_t tag_entry(std::uint32_t tag);

[[nodiscard]] Package build_package(const std::filesystem::path& output,
                                    std::uint16_t packageId,
                                    std::uint16_t patchId,
                                    const Profile& profile,
                                    const std::vector<AuthoredEntry>& entries,
                                    const std::vector<NamedTag>& namedTags,
                                    const std::vector<WideHash>& wideHashes,
                                    const std::vector<TagPair>& tagPairs,
                                    bool alternateKey,
                                    const Runtime& runtime);

[[nodiscard]] Package build_patch(const std::filesystem::path& output,
                                  const Package& base,
                                  const std::vector<Replacement>& replacements,
                                  const Runtime& runtime);

[[nodiscard]] std::filesystem::path make_isolated_chain(const Package& base,
                                                        const std::filesystem::path* addition = nullptr);
void remove_tree(const std::filesystem::path& path) noexcept;

[[nodiscard]] std::vector<std::byte> read_binary(const std::filesystem::path& path);
[[nodiscard]] std::string read_text(const std::filesystem::path& path);
void write_binary(const std::filesystem::path& path, std::span<const std::byte> bytes);

} // namespace repkg
