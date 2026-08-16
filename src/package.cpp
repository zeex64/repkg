#include "package.h"

#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <system_error>

#include "middleware/compression/oodle/runtime.h"
#include "middleware/content/packages/reader/reader.h"
#include "middleware/crypto/aes_gcm_decrypt.h"

namespace repkg {
namespace fs = std::filesystem;
namespace oodle = sunrise::middleware::compression::oodle;
namespace reader = sunrise::middleware::content::packages::reader;
namespace aes_gcm = sunrise::middleware::crypto::aes_gcm;

namespace {

constexpr std::size_t kSignatureOffset = 0x800;
constexpr std::size_t kSignatureSize = 0x100;
constexpr std::size_t kMiscOffset = 0x1000;
constexpr std::size_t kMetadataMinimumOffset = 0x1800;
constexpr std::size_t kMetadataMinimumSize = 0x280;
constexpr std::size_t kEntrySize = 0x10;
constexpr std::size_t kBlockRecordSize = 0x30;
constexpr std::size_t kDiskAlignment = 0x800;
constexpr std::size_t kFooterSize = 0x800;
constexpr std::uint32_t kFooterMagic = 0xDEADBEEFU;
constexpr std::uint32_t kArrayMarker = 0x80809FBDU;
constexpr std::uint64_t kEntryArrayType = 0x80809EF3ULL;
constexpr std::uint64_t kBlockArrayType = 0x80809EEEULL;
constexpr std::uint64_t kNamedArrayType = 0x80809EECULL;
constexpr std::uint64_t kWideArrayType = 0x80809D02ULL;
constexpr std::uint64_t kTagPairArrayType = 0x80809A13ULL;
constexpr std::uint32_t kStringBlobType = 0x80800065U;
constexpr std::uint16_t kEncodedFlags = 0x3;
constexpr std::uint16_t kAlternateKeyFlag = 0x4;
constexpr std::byte kNonceBranch{0xF9};

const reader::BlockKeys kKeys{
    {std::byte{0xD6}, std::byte{0x2A}, std::byte{0xB2}, std::byte{0xC1},
     std::byte{0x0C}, std::byte{0xC0}, std::byte{0x1B}, std::byte{0xC5},
     std::byte{0x35}, std::byte{0xDB}, std::byte{0x7B}, std::byte{0x86},
     std::byte{0x55}, std::byte{0xC7}, std::byte{0xDC}, std::byte{0x3B}},
    {std::byte{0x3A}, std::byte{0x4A}, std::byte{0x5D}, std::byte{0x36},
     std::byte{0x73}, std::byte{0xA6}, std::byte{0x60}, std::byte{0x58},
     std::byte{0x7E}, std::byte{0x63}, std::byte{0xE6}, std::byte{0x76},
     std::byte{0xE4}, std::byte{0x08}, std::byte{0x92}, std::byte{0xB5}},
    {std::byte{0x84}, std::byte{0xDF}, std::byte{0x11}, std::byte{0xC0},
     std::byte{0xAC}, std::byte{0xAB}, std::byte{0xFA}, std::byte{0x20},
     std::byte{0x33}, std::byte{0x11}, std::byte{0x26}, std::byte{0x99}},
};

template <typename T>
[[nodiscard]] T load(std::span<const std::byte> bytes, std::size_t offset,
                     std::string_view label = "field") {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw Error(std::string(label) + " is outside its buffer");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

template <typename T>
void store(std::span<std::byte> bytes, std::size_t offset, T value,
           std::string_view label = "field") {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw Error(std::string(label) + " is outside its buffer");
    }
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

[[nodiscard]] std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0
        || value > std::numeric_limits<std::uint64_t>::max() - alignment + 1) {
        throw Error("invalid alignment operation");
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

void check_range(std::uint64_t offset, std::uint64_t size, std::uint64_t total,
                 std::string_view label) {
    if (offset > total || size > total - offset) {
        throw Error(std::string(label) + " exceeds the package file");
    }
}

[[nodiscard]] std::vector<std::byte> read_at(std::ifstream& stream, std::uint64_t offset,
                                             std::size_t size, std::string_view label) {
    std::vector<std::byte> output(size);
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream || (size != 0 && !stream.read(reinterpret_cast<char*>(output.data()),
                                               static_cast<std::streamsize>(size)))) {
        throw Error("short read for " + std::string(label));
    }
    return output;
}

[[nodiscard]] std::array<std::byte, 20> sha1(std::span<const std::byte> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<std::byte, 20> output{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status >= 0 && !bytes.empty()) {
        if (bytes.size() > std::numeric_limits<ULONG>::max()) status = -1;
        else status = BCryptHashData(hash,
                                    reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                                    static_cast<ULONG>(bytes.size()), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(output.data()),
                                  static_cast<ULONG>(output.size()), 0);
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw Error("Windows SHA-1 operation failed");
    return output;
}

[[nodiscard]] Header parse_header(std::span<const std::byte> raw) {
    if (raw.size() != kHeaderSize) throw Error("package header is not 0x170 bytes");
    Header h{};
    std::copy(raw.begin(), raw.end(), h.raw.begin());
    h.version = load<std::uint16_t>(raw, 0x00);
    h.platform = load<std::uint16_t>(raw, 0x02);
    h.packageId = load<std::uint16_t>(raw, 0x04);
    h.localeCheckEnable = load<std::uint8_t>(raw, 0x06);
    h.groupId = load<std::uint64_t>(raw, 0x08);
    h.buildTime = load<std::uint64_t>(raw, 0x10);
    h.contentBuild = load<std::uint32_t>(raw, 0x18);
    h.contentRevision = load<std::uint32_t>(raw, 0x1C);
    h.patchId = load<std::uint16_t>(raw, 0x20);
    h.language = load<std::uint8_t>(raw, 0x22);
    h.wordA4 = load<std::uint32_t>(raw, 0xA4);
    h.wordA8 = load<std::uint32_t>(raw, 0xA8);
    h.wordAC = load<std::uint32_t>(raw, 0xAC);
    h.signatureOffset = load<std::uint32_t>(raw, 0xB0);
    h.entryCount = load<std::uint32_t>(raw, 0xB4);
    h.betaEntryOffset = load<std::uint32_t>(raw, 0xB8);
    h.blockCount = load<std::uint32_t>(raw, 0xD0);
    h.betaBlockOffset = load<std::uint32_t>(raw, 0xD4);
    h.unknownEC = load<std::uint32_t>(raw, 0xEC);
    h.miscOffset = load<std::uint32_t>(raw, 0xF0);
    h.miscSize = load<std::uint32_t>(raw, 0xF4);
    std::copy_n(raw.begin() + 0xF8, h.miscSha1.size(), h.miscSha1.begin());
    h.metadataOffset = load<std::uint32_t>(raw, 0x110);
    h.metadataSize = load<std::uint32_t>(raw, 0x114);
    std::copy_n(raw.begin() + 0x118, h.metadataSha1.size(), h.metadataSha1.begin());
    h.footerOffset = load<std::uint32_t>(raw, 0x160);
    h.fileSize = load<std::uint32_t>(raw, 0x164);
    h.localeToken = load<std::uint32_t>(raw, 0x168);
    h.localeId = load<std::uint8_t>(raw, 0x16C);
    return h;
}

[[nodiscard]] Entry parse_entry(std::span<const std::byte> raw) {
    return Entry{load<std::uint32_t>(raw, 0), load<std::uint32_t>(raw, 4),
                 load<std::uint64_t>(raw, 8)};
}

void encode_entry(std::span<std::byte> output, std::size_t offset, const Entry& entry) {
    store(output, offset, entry.reference);
    store(output, offset + 4, entry.typeInfo);
    store(output, offset + 8, entry.blockInfo);
}

[[nodiscard]] Block parse_block(std::span<const std::byte> raw) {
    Block block{};
    block.offset = load<std::uint32_t>(raw, 0);
    block.size = load<std::uint32_t>(raw, 4);
    block.patchId = load<std::uint16_t>(raw, 8);
    block.flags = load<std::uint16_t>(raw, 10);
    std::copy_n(raw.begin() + 12, block.digest.size(), block.digest.begin());
    std::copy_n(raw.begin() + 32, block.tag.size(), block.tag.begin());
    return block;
}

void encode_block_record(std::span<std::byte> output, std::size_t offset, const Block& block) {
    store(output, offset, block.offset);
    store(output, offset + 4, block.size);
    store(output, offset + 8, block.patchId);
    store(output, offset + 10, block.flags);
    std::copy(block.digest.begin(), block.digest.end(), output.begin() + offset + 12);
    std::copy(block.tag.begin(), block.tag.end(), output.begin() + offset + 32);
}

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t>
misc_array(std::span<const std::byte> misc, std::size_t pair, std::uint64_t type,
           std::string_view label) {
    const std::uint64_t count = load<std::uint64_t>(misc, pair, label);
    const std::uint64_t relative = load<std::uint64_t>(misc, pair + 8, label);
    if (count == 0) {
        if (relative != 0) throw Error("empty " + std::string(label) + " has a pointer");
        return {0, 0};
    }
    if (count > kEntryLimit) throw Error(std::string(label) + " exceeds 8192 rows");
    const std::uint64_t target = pair + 0x18 + relative;
    if (target < 0x20 || target > misc.size()) throw Error(std::string(label) + " pointer is invalid");
    const std::uint64_t header = target - 0x20;
    if (load<std::uint32_t>(misc, static_cast<std::size_t>(header + 0x0C)) != kArrayMarker
        || load<std::uint64_t>(misc, static_cast<std::size_t>(header + 0x18)) != type) {
        throw Error(std::string(label) + " array header is invalid");
    }
    const std::uint64_t capacity = load<std::uint64_t>(misc, static_cast<std::size_t>(header + 0x10));
    if (capacity < count || target + capacity * 0x10 > misc.size()) {
        throw Error(std::string(label) + " capacity is invalid");
    }
    return {count, target};
}

[[nodiscard]] std::pair<std::uint64_t, std::uint64_t>
metadata_array(std::span<const std::byte> metadata, std::size_t pair,
               std::uint64_t type, std::size_t rowSize, std::string_view label) {
    const std::uint64_t count = load<std::uint64_t>(metadata, pair, label);
    const std::uint64_t relative = load<std::uint64_t>(metadata, pair + 8, label);
    if (count == 0) {
        if (relative != 0) throw Error("empty " + std::string(label) + " has a pointer");
        return {0, 0};
    }
    if (count > kEntryLimit) throw Error(std::string(label) + " exceeds 8192 rows");
    const std::uint64_t target = pair + 0x18 + relative;
    if (target < 0x20 || target > metadata.size()) {
        throw Error(std::string(label) + " pointer is invalid");
    }
    const std::uint64_t header = target - 0x20;
    if (load<std::uint32_t>(metadata, static_cast<std::size_t>(header + 0x0C))
            != kArrayMarker
        || load<std::uint64_t>(metadata, static_cast<std::size_t>(header + 0x18))
            != type) {
        throw Error(std::string(label) + " array header is invalid");
    }
    const std::uint64_t capacity = load<std::uint64_t>(
        metadata, static_cast<std::size_t>(header + 0x10));
    if (capacity < count || rowSize == 0
        || capacity > (metadata.size() - target) / rowSize) {
        throw Error(std::string(label) + " capacity is invalid");
    }
    return {count, target};
}

[[nodiscard]] std::vector<std::byte> build_misc(std::uint16_t packageId,
                                                const std::vector<NamedTag>& names,
                                                const std::vector<WideHash>& hashes) {
    std::vector<std::byte> misc(0x60);
    auto append_array = [&](std::size_t pair, std::size_t count, std::uint64_t type,
                            std::span<const std::byte> pad) -> std::size_t {
        if (count == 0) return 0;
        const std::size_t capacity = (std::max<std::size_t>)(8, count);
        const std::size_t header = misc.size();
        misc.resize(misc.size() + 0x20);
        const std::size_t target = misc.size();
        store<std::uint32_t>(misc, header + 0x0C, kArrayMarker);
        store<std::uint64_t>(misc, header + 0x10, capacity);
        store<std::uint64_t>(misc, header + 0x18, type);
        misc.resize(misc.size() + capacity * 0x10);
        for (std::size_t index = count; index < capacity; ++index) {
            std::copy(pad.begin(), pad.end(), misc.begin() + target + index * 0x10);
        }
        store<std::uint64_t>(misc, pair, count);
        store<std::uint64_t>(misc, pair + 8, target - (pair + 0x18));
        return target;
    };

    std::array<std::byte, 16> widePad{};
    store<std::uint32_t>(widePad, 8, 0xFFFFFFFFU);
    store<std::uint32_t>(widePad, 12, 0xFFFFFFFFU);
    const std::size_t wideTarget = append_array(0x30, hashes.size(), kWideArrayType, widePad);
    for (std::size_t index = 0; index < hashes.size(); ++index) {
        store<std::uint64_t>(misc, wideTarget + index * 0x10, hashes[index].hash);
        store<std::uint32_t>(misc, wideTarget + index * 0x10 + 8,
                             tag_for(packageId, hashes[index].entryIndex));
        store<std::uint32_t>(misc, wideTarget + index * 0x10 + 12, hashes[index].classId);
    }

    std::array<std::byte, 16> namePad{};
    store<std::uint32_t>(namePad, 0, 0xFFFFFFFFU);
    store<std::uint32_t>(namePad, 4, 0xFFFFFFFFU);
    const std::size_t namedTarget = append_array(0x10, names.size(), kNamedArrayType, namePad);
    if (!names.empty()) {
        std::size_t bytes = 0;
        for (const auto& name : names) bytes += name.name.size() + 1;
        const std::size_t blob = misc.size();
        misc.resize(misc.size() + 0x10);
        store<std::uint32_t>(misc, blob, 0U);
        store<std::uint32_t>(misc, blob + 4, kStringBlobType);
        store<std::uint64_t>(misc, blob + 8, bytes);
        std::size_t nameCursor = misc.size();
        misc.resize(misc.size() + bytes);
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto& name = names[index];
            const std::size_t row = namedTarget + index * 0x10;
            store<std::uint32_t>(misc, row, tag_for(packageId, name.entryIndex));
            store<std::uint32_t>(misc, row + 4, name.classId);
            store<std::uint64_t>(misc, row + 8, nameCursor - (row + 8));
            std::memcpy(misc.data() + nameCursor, name.name.c_str(), name.name.size() + 1);
            nameCursor += name.name.size() + 1;
        }
    }
    store<std::uint64_t>(misc, 0, misc.size());
    store<std::uint64_t>(misc, 8, 3ULL);
    return misc;
}

struct BuiltLayout {
    std::vector<Entry> entries;
    std::vector<std::vector<std::byte>> logicalBlocks;
};

[[nodiscard]] BuiltLayout layout_entries(const std::vector<AuthoredEntry>& authored,
                                         std::uint32_t firstBlock = 0) {
    BuiltLayout output;
    std::vector<std::byte> logical;
    for (const auto& input : authored) {
        const std::uint64_t aligned = align_up(logical.size(), 0x10);
        logical.resize(static_cast<std::size_t>(aligned));
        const std::uint64_t relativeBlock = aligned / kBlockSize;
        const std::uint64_t startBlock = firstBlock + relativeBlock;
        const std::uint64_t startOffset = aligned % kBlockSize;
        if (startBlock >= 0x4000) throw Error("package exceeds the 14-bit block index");
        if (input.data.size() > 0xFFFFFFFFULL) throw Error("entry exceeds 32-bit size");
        const std::uint64_t info = (static_cast<std::uint64_t>(input.data.size()) << 28U)
                                   | ((startOffset >> 4U) << 14U) | startBlock;
        output.entries.push_back(Entry{input.reference, input.typeInfo, info});
        logical.insert(logical.end(), input.data.begin(), input.data.end());
    }
    for (std::size_t offset = 0; offset < logical.size(); offset += kBlockSize) {
        const std::size_t count = (std::min)(kBlockSize, logical.size() - offset);
        output.logicalBlocks.emplace_back(logical.begin() + offset, logical.begin() + offset + count);
    }
    return output;
}

struct OutputParts {
    std::array<std::byte, kHeaderSize> header{};
    std::vector<std::byte> misc;
    std::vector<std::byte> metadata;
    std::vector<Block> newBlocks;
    std::vector<std::vector<std::byte>> payloads;
    std::uint32_t footerOffset{};
    std::uint32_t fileSize{};
};

[[nodiscard]] OutputParts compose(std::uint16_t packageId, std::uint16_t patchId,
                                  const Profile& profile, const std::vector<Entry>& entries,
                                  const std::vector<Block>& inheritedBlocks,
                                  const std::vector<std::vector<std::byte>>& logicalBlocks,
                                  const std::vector<NamedTag>& names,
                                  const std::vector<WideHash>& hashes,
                                  const std::vector<TagPair>& tagPairs,
                                  bool alternateKey,
                                  const Runtime& runtime) {
    if (entries.empty() || entries.size() > kEntryLimit) throw Error("package needs 1..8192 entries");
    OutputParts out{};
    out.misc = build_misc(packageId, names, hashes);
    const std::size_t allocatedEntries = (std::max<std::size_t>)(8, entries.size());
    const std::size_t entryRelative = 0x60;
    const std::size_t blockRelative = entryRelative + allocatedEntries * kEntrySize + 0x20;
    const std::size_t totalBlocks = inheritedBlocks.size() + logicalBlocks.size();
    const std::size_t allocatedBlocks = (std::max<std::size_t>)(8, totalBlocks);
    const std::size_t tagPairHeader = blockRelative + allocatedBlocks * kBlockRecordSize;
    const std::size_t allocatedTagPairs = tagPairs.empty()
        ? 0 : (std::max<std::size_t>)(8, tagPairs.size());
    const std::uint64_t metadataSize = (std::max<std::uint64_t>)(
        kMetadataMinimumSize,
        tagPairHeader + (tagPairs.empty() ? 0 : 0x20 + allocatedTagPairs * 8));
    const std::uint64_t metadataOffset = (std::max<std::uint64_t>)(
        kMetadataMinimumOffset, align_up(kMiscOffset + out.misc.size(), kDiskAlignment));
    std::uint64_t payloadCursor = align_up(metadataOffset + metadataSize, kDiskAlignment);

    for (const auto& logical : logicalBlocks) {
        Block block{};
        std::vector<std::byte> encoded = runtime.encode_block(
            logical, packageId, alternateKey, block);
        payloadCursor = align_up(payloadCursor, kDiskAlignment);
        if (payloadCursor > 0xFFFFFFFFULL) throw Error("package block offset exceeds 32 bits");
        block.offset = static_cast<std::uint32_t>(payloadCursor);
        block.patchId = patchId;
        out.newBlocks.push_back(block);
        out.payloads.push_back(std::move(encoded));
        payloadCursor += block.size;
    }
    const std::uint64_t footer = align_up(payloadCursor, kDiskAlignment);
    const std::uint64_t fileSize = footer + kFooterSize;
    if (fileSize > 0xFFFFFFFFULL) throw Error("package exceeds the 32-bit file-size field");
    out.footerOffset = static_cast<std::uint32_t>(footer);
    out.fileSize = static_cast<std::uint32_t>(fileSize);

    std::vector<Block> allBlocks = inheritedBlocks;
    allBlocks.insert(allBlocks.end(), out.newBlocks.begin(), out.newBlocks.end());
    out.metadata.resize(static_cast<std::size_t>(metadataSize));
    const std::uint64_t entryOffset = metadataOffset + entryRelative;
    const std::uint64_t blockOffset = metadataOffset + blockRelative;
    store<std::uint64_t>(out.metadata, 0x00, metadataSize);
    store<std::uint64_t>(out.metadata, 0x08, 1ULL);
    store<std::uint64_t>(out.metadata, 0x10, entries.size());
    store<std::uint64_t>(out.metadata, 0x18, entryOffset - (metadataOffset + 0x28));
    store<std::uint64_t>(out.metadata, 0x20, allBlocks.size());
    store<std::uint64_t>(out.metadata, 0x28, blockOffset - (metadataOffset + 0x38));
    store<std::uint32_t>(out.metadata, 0x4C, kArrayMarker);
    store<std::uint64_t>(out.metadata, 0x50, allocatedEntries);
    store<std::uint64_t>(out.metadata, 0x58, kEntryArrayType);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        encode_entry(out.metadata, entryRelative + index * kEntrySize, entries[index]);
    }
    const std::size_t blockHeader = blockRelative - 0x20;
    store<std::uint32_t>(out.metadata, blockHeader + 0x0C, kArrayMarker);
    store<std::uint64_t>(out.metadata, blockHeader + 0x10, allocatedBlocks);
    store<std::uint64_t>(out.metadata, blockHeader + 0x18, kBlockArrayType);
    for (std::size_t index = 0; index < allBlocks.size(); ++index) {
        encode_block_record(out.metadata, blockRelative + index * kBlockRecordSize, allBlocks[index]);
    }
    if (!tagPairs.empty()) {
        const std::size_t tagPairTarget = tagPairHeader + 0x20;
        store<std::uint64_t>(out.metadata, 0x30, tagPairs.size());
        store<std::uint64_t>(out.metadata, 0x38, tagPairTarget - 0x48);
        store<std::uint32_t>(out.metadata, tagPairHeader + 0x0C, kArrayMarker);
        store<std::uint64_t>(out.metadata, tagPairHeader + 0x10, allocatedTagPairs);
        store<std::uint64_t>(out.metadata, tagPairHeader + 0x18, kTagPairArrayType);
        for (std::size_t index = 0; index < tagPairs.size(); ++index) {
            store<std::uint32_t>(out.metadata, tagPairTarget + index * 8,
                                 tagPairs[index].firstTag);
            store<std::uint32_t>(out.metadata, tagPairTarget + index * 8 + 4,
                                 tagPairs[index].secondTag);
        }
        std::fill(out.metadata.begin() + tagPairTarget + tagPairs.size() * 8,
                  out.metadata.begin() + tagPairTarget + allocatedTagPairs * 8,
                  std::byte{0xFF});
    }

    store<std::uint16_t>(out.header, 0x00, 38U);
    store<std::uint16_t>(out.header, 0x02, 2U);
    store<std::uint16_t>(out.header, 0x04, packageId);
    store<std::uint8_t>(out.header, 0x06, profile.localeCheckEnable);
    store<std::uint64_t>(out.header, 0x08, profile.groupId);
    store<std::uint64_t>(out.header, 0x10, profile.buildTime);
    store<std::uint32_t>(out.header, 0x18, profile.contentBuild);
    store<std::uint32_t>(out.header, 0x1C, profile.contentRevision);
    store<std::uint16_t>(out.header, 0x20, patchId);
    store<std::uint8_t>(out.header, 0x22, profile.language);
    store<std::uint32_t>(out.header, 0xA4, profile.wordA4);
    store<std::uint32_t>(out.header, 0xA8, profile.wordA8);
    store<std::uint32_t>(out.header, 0xAC, profile.wordAC);
    store<std::uint32_t>(out.header, 0xB0, static_cast<std::uint32_t>(kSignatureOffset));
    store<std::uint32_t>(out.header, 0xB4, static_cast<std::uint32_t>(entries.size()));
    store<std::uint32_t>(out.header, 0xD0, static_cast<std::uint32_t>(allBlocks.size()));
    store<std::uint32_t>(out.header, 0xEC, profile.unknownEC);
    store<std::uint32_t>(out.header, 0xF0, static_cast<std::uint32_t>(kMiscOffset));
    store<std::uint32_t>(out.header, 0xF4, static_cast<std::uint32_t>(out.misc.size()));
    const auto miscDigest = sha1(out.misc);
    std::copy(miscDigest.begin(), miscDigest.end(), out.header.begin() + 0xF8);
    store<std::uint32_t>(out.header, 0x110, static_cast<std::uint32_t>(metadataOffset));
    store<std::uint32_t>(out.header, 0x114, static_cast<std::uint32_t>(metadataSize));
    const auto metadataDigest = sha1(out.metadata);
    std::copy(metadataDigest.begin(), metadataDigest.end(), out.header.begin() + 0x118);
    store<std::uint32_t>(out.header, 0x160, out.footerOffset);
    store<std::uint32_t>(out.header, 0x164, out.fileSize);
    store<std::uint32_t>(out.header, 0x168, profile.localeToken);
    store<std::uint8_t>(out.header, 0x16C, profile.localeId);
    return out;
}

void write_output(const fs::path& output, const OutputParts& parts) {
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
    fs::path temporary = output;
    temporary += L".repkg.tmp";
    std::error_code ignored;
    fs::remove(temporary, ignored);
    try {
        std::fstream stream(temporary, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!stream) throw Error("could not create output " + temporary.string());
        stream.write(reinterpret_cast<const char*>(parts.header.data()), parts.header.size());
        stream.seekp(kSignatureOffset, std::ios::beg);
        std::array<std::byte, kSignatureSize> signature{};
        stream.write(reinterpret_cast<const char*>(signature.data()), signature.size());
        stream.seekp(kMiscOffset, std::ios::beg);
        stream.write(reinterpret_cast<const char*>(parts.misc.data()), parts.misc.size());
        const std::uint32_t metadataOffset = load<std::uint32_t>(parts.header, 0x110);
        stream.seekp(metadataOffset, std::ios::beg);
        stream.write(reinterpret_cast<const char*>(parts.metadata.data()), parts.metadata.size());
        if (parts.newBlocks.size() != parts.payloads.size()) throw Error("internal block count mismatch");
        for (std::size_t index = 0; index < parts.newBlocks.size(); ++index) {
            stream.seekp(parts.newBlocks[index].offset, std::ios::beg);
            stream.write(reinterpret_cast<const char*>(parts.payloads[index].data()),
                         parts.payloads[index].size());
        }
        stream.seekp(parts.footerOffset, std::ios::beg);
        std::array<std::byte, kFooterSize> footer{};
        store<std::uint32_t>(footer, 0, kFooterMagic);
        stream.write(reinterpret_cast<const char*>(footer.data()), footer.size());
        stream.flush();
        if (!stream) throw Error("failed while writing output package");
        stream.close();
        if (!MoveFileExW(temporary.c_str(), output.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw Error("could not install completed output package");
        }
    } catch (...) {
        fs::remove(temporary, ignored);
        throw;
    }
}

[[nodiscard]] fs::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0 || copied >= buffer.size()) throw Error("could not locate repkg.exe");
    buffer.resize(copied);
    return fs::path(buffer).parent_path();
}

[[nodiscard]] std::array<std::byte, aes_gcm::kNonceSize>
package_nonce(std::uint16_t packageId) {
    auto nonce = kKeys.nonceBase;
    nonce[0] ^= static_cast<std::byte>((packageId >> 8U) & 0xFFU);
    nonce[1] = kNonceBranch;
    nonce[11] ^= static_cast<std::byte>(packageId & 0xFFU);
    return nonce;
}

[[nodiscard]] std::wstring family_stem(const fs::path& path) {
    const std::wstring name = path.filename().wstring();
    const std::wregex pattern(LR"((.+)_([0-9]+)\.pkg)", std::regex::icase);
    std::wsmatch match;
    if (!std::regex_match(name, match, pattern)) throw Error("package filename has no numeric patch suffix");
    return match[1].str();
}

[[nodiscard]] fs::path chain_leaf(std::uint16_t packageId, std::uint16_t patchId) {
    std::wostringstream name;
    name << L"w64_depkg_" << std::hex << std::nouppercase << std::setw(4)
         << std::setfill(L'0') << packageId << L"_" << std::dec << patchId << L".pkg";
    return name.str();
}

} // namespace

std::uint32_t Entry::starting_block() const noexcept { return static_cast<std::uint32_t>(blockInfo & 0x3FFFU); }
std::uint32_t Entry::starting_offset() const noexcept { return static_cast<std::uint32_t>(((blockInfo >> 14U) & 0x3FFFU) << 4U); }
std::uint64_t Entry::logical_size() const noexcept { return blockInfo >> 28U; }

Package Package::open(const fs::path& path, bool verifyPhysicalBlocks) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw Error("could not open package " + path.string());
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > 0xFFFFFFFFULL) throw Error("package size is invalid");
    const std::uint64_t size = static_cast<std::uint64_t>(end);
    Header h = parse_header(read_at(stream, 0, kHeaderSize, "header"));
    if (h.version != 38 || h.platform != 2) throw Error("package version/platform is unsupported");
    if (h.packageId < 0x100 || h.packageId > 0x19FF) throw Error("package id is outside 0x0100..0x19ff");
    if (h.entryCount == 0 || h.entryCount > kEntryLimit || h.blockCount == 0) throw Error("package table counts are invalid");
    if (h.fileSize != size || h.footerOffset + kFooterSize != size) throw Error("package file/footer size is invalid");
    if (h.signatureOffset != kSignatureOffset) throw Error("package signature offset is invalid");
    if ((h.miscOffset == 0) != (h.miscSize == 0)) throw Error("misc has only one of offset/size");
    check_range(h.miscOffset, h.miscSize, size, "misc");
    const auto misc = read_at(stream, h.miscOffset, h.miscSize, "misc");
    if (sha1(misc) != h.miscSha1
        && !(misc.empty()
             && std::all_of(h.miscSha1.begin(), h.miscSha1.end(),
                            [](std::byte value) { return value == std::byte{}; }))) {
        throw Error("misc SHA-1 mismatch");
    }
    std::vector<std::byte> metadata;
    if (h.metadataOffset != 0) {
        check_range(h.metadataOffset, h.metadataSize, size, "metadata");
        metadata = read_at(stream, h.metadataOffset, h.metadataSize, "metadata");
        if (sha1(metadata) != h.metadataSha1) throw Error("metadata SHA-1 mismatch");
    }
    const auto footer = read_at(stream, h.footerOffset, kFooterSize, "footer");
    if (load<std::uint32_t>(footer, 0) != kFooterMagic
        || std::any_of(footer.begin() + 4, footer.end(), [](std::byte value) { return value != std::byte{}; })) {
        throw Error("package footer is invalid");
    }

    std::uint64_t entryOffset = h.betaEntryOffset;
    std::uint64_t blockOffset = h.betaBlockOffset;
    if (h.pre_bl()) {
        if (metadata.size() < 0x60 || load<std::uint64_t>(metadata, 0) != metadata.size()
            || load<std::uint64_t>(metadata, 8) != 1
            || load<std::uint64_t>(metadata, 0x10) != h.entryCount
            || load<std::uint64_t>(metadata, 0x20) != h.blockCount) {
            throw Error("pre-BL metadata prefix is invalid");
        }
        entryOffset = h.metadataOffset + 0x28 + load<std::uint64_t>(metadata, 0x18);
        blockOffset = h.metadataOffset + 0x38 + load<std::uint64_t>(metadata, 0x28);
        if (entryOffset < h.metadataOffset + 0x20 || blockOffset < h.metadataOffset + 0x20) {
            throw Error("metadata table pointers are invalid");
        }
    }
    check_range(entryOffset, static_cast<std::uint64_t>(h.entryCount) * kEntrySize, size, "entry table");
    check_range(blockOffset, static_cast<std::uint64_t>(h.blockCount) * kBlockRecordSize, size, "block table");
    const auto rawEntries = read_at(stream, entryOffset, h.entryCount * kEntrySize, "entry table");
    const auto rawBlocks = read_at(stream, blockOffset, h.blockCount * kBlockRecordSize, "block table");

    Package package{};
    package.path = fs::absolute(path);
    package.header = h;
    package.entryTableOffset = entryOffset;
    package.blockTableOffset = blockOffset;
    for (std::size_t index = 0; index < h.entryCount; ++index) {
        package.entries.push_back(parse_entry(std::span(rawEntries).subspan(index * kEntrySize, kEntrySize)));
    }
    for (std::size_t index = 0; index < h.blockCount; ++index) {
        Block block = parse_block(std::span(rawBlocks).subspan(index * kBlockRecordSize, kBlockRecordSize));
        if (block.size == 0 || block.size > kBlockSize || block.patchId > h.patchId
            || block.flags > 0x0F || block.offset % kDiskAlignment != 0) {
            throw Error("block " + std::to_string(index) + " is invalid (offset="
                        + std::to_string(block.offset) + ", size=" + std::to_string(block.size)
                        + ", patch=" + std::to_string(block.patchId) + ", flags="
                        + std::to_string(block.flags) + ")");
        }
        if (block.patchId == h.patchId) {
            check_range(block.offset, block.size, h.footerOffset, "physical block");
            if (verifyPhysicalBlocks) {
                const auto bytes = read_at(stream, block.offset, block.size, "physical block");
                if (sha1(bytes) != block.digest) throw Error("physical block SHA-1 mismatch");
            }
        }
        package.blocks.push_back(block);
    }
    for (std::size_t index = 0; index < package.entries.size(); ++index) {
        const Entry& entry = package.entries[index];
        if (entry.logical_size() != 0
            && (entry.starting_block() >= package.blocks.size()
                || entry.starting_offset() >= kBlockSize || entry.starting_offset() % 0x10 != 0)) {
            throw Error("entry " + std::to_string(index) + " placement is invalid");
        }
    }

    if (h.pre_bl() && !misc.empty()) {
        if (misc.size() < 0x60 || load<std::uint64_t>(misc, 0) > misc.size()
            || load<std::uint64_t>(misc, 8) != 3) throw Error("pre-BL misc prefix is invalid");
        const auto [nameCount, nameTarget] = misc_array(misc, 0x10, kNamedArrayType, "named tags");
        for (std::size_t index = 0; index < nameCount; ++index) {
            const std::size_t row = static_cast<std::size_t>(nameTarget) + index * 0x10;
            const std::uint32_t tag = load<std::uint32_t>(misc, row);
            const std::uint32_t classId = load<std::uint32_t>(misc, row + 4);
            const std::uint64_t target = row + 8 + load<std::uint64_t>(misc, row + 8);
            if (target >= misc.size()) throw Error("named-tag string pointer is invalid");
            const auto begin = reinterpret_cast<const char*>(misc.data() + target);
            const auto limit = reinterpret_cast<const char*>(misc.data() + misc.size());
            const auto endName = std::find(begin, limit, '\0');
            if (begin == endName || endName == limit) throw Error("named-tag string is invalid");
            package.namedTags.push_back(NamedTag{tag_entry(tag), classId, std::string(begin, endName)});
        }
        const auto [wideCount, wideTarget] = misc_array(misc, 0x30, kWideArrayType, "wide hashes");
        for (std::size_t index = 0; index < wideCount; ++index) {
            const std::size_t row = static_cast<std::size_t>(wideTarget) + index * 0x10;
            const std::uint64_t hash = load<std::uint64_t>(misc, row);
            const std::uint32_t tag = load<std::uint32_t>(misc, row + 8);
            package.wideHashes.push_back(WideHash{hash, tag_entry(tag), load<std::uint32_t>(misc, row + 12)});
        }
    }
    if (h.pre_bl()) {
        const auto [pairCount, pairTarget] = metadata_array(
            metadata, 0x30, kTagPairArrayType, 8, "tag pairs");
        for (std::size_t index = 0; index < pairCount; ++index) {
            const std::size_t row = static_cast<std::size_t>(pairTarget) + index * 8;
            package.tagPairs.push_back(TagPair{
                load<std::uint32_t>(metadata, row),
                load<std::uint32_t>(metadata, row + 4),
            });
        }
    }
    return package;
}

Profile Profile::builtin() noexcept {
    return Profile{0xE2CAFC604440ADDDULL, 1598315345ULL, 0x00015281U, 2U, 0U,
                   0x0000000EU, 0x61C3F183U, 0x00000002U, 0U, 1U, 0x281141FDU, 0U};
}

Profile Profile::from_package(const Package& package) {
    if (!package.header.pre_bl()) throw Error("authoring profile must be a pre-BL package");
    const Header& h = package.header;
    return Profile{h.groupId, h.buildTime, h.contentBuild, h.contentRevision, h.language,
                   h.wordA4, h.wordA8, h.wordAC, h.unknownEC, h.localeCheckEnable,
                   h.localeToken, h.localeId};
}

Runtime::Runtime() {
    oodlePath_ = executable_directory() / L"oo2core_3_win64.dll";
    oodle_ = LoadLibraryW(oodlePath_.c_str());
    if (oodle_ == nullptr) {
        throw Error("oo2core_3_win64.dll must be beside the executable");
    }
    scratch_ = std::make_unique<reader::Scratch>();
}

Runtime::~Runtime() {
    if (scratch_) reader::close_files(*scratch_);
    if (oodle_ != nullptr) FreeLibrary(oodle_);
}

std::vector<std::byte> Runtime::encode_block(std::span<const std::byte> input,
                                             std::uint16_t packageId,
                                             bool alternateKey,
                                             Block& record) const {
    if (input.empty() || input.size() > kBlockSize) throw Error("encoded block input is outside 1..0x40000");
    std::array<std::byte, kBlockSize> decoded{};
    std::copy(input.begin(), input.end(), decoded.begin());
    std::size_t capacity = 0;
    if (!oodle::required_capacity(oodle_, decoded.size(), capacity)) throw Error("Oodle rejected block capacity query");
    std::vector<std::byte> compressed(capacity);
    std::size_t written = 0;
    if (!oodle::compress(oodle_, decoded, compressed, written) || written == 0) throw Error("Oodle block compression failed");
    compressed.resize(written);
    if (compressed.size() >= decoded.size()) {
        std::vector<std::byte> stored(decoded.begin(), decoded.end());
        record.size = static_cast<std::uint32_t>(stored.size());
        record.flags = 0;
        record.digest = sha1(stored);
        record.tag = {};
        return stored;
    }
    std::vector<std::byte> ciphertext(written);
    const auto nonce = package_nonce(packageId);
    const auto& key = alternateKey ? kKeys.alternate : kKeys.primary;
    if (!aes_gcm::encrypt(key, nonce, compressed, ciphertext, record.tag)) {
        throw Error("AES-GCM block encoding failed");
    }
    record.size = static_cast<std::uint32_t>(ciphertext.size());
    record.flags = kEncodedFlags | (alternateKey ? kAlternateKeyFlag : 0);
    record.digest = sha1(ciphertext);
    return ciphertext;
}

std::vector<std::byte> Runtime::read_tag(const fs::path& directory, std::uint32_t tag,
                                         std::uint32_t* classId) const {
    const fs::path resolvedDirectory = fs::absolute(directory).lexically_normal();
    if (readerDirectory_ != resolvedDirectory) {
        reader::close_files(*scratch_);
        readerDirectory_ = resolvedDirectory;
    }
    const std::wstring directoryText = readerDirectory_.wstring();
    const reader::Source source{directoryText, &kKeys};
    std::vector<std::byte> output;
    std::uint32_t actualClass = 0;
    const bool okay = classId == nullptr
                          ? reader::read_tag(source, *scratch_, tag, output)
                          : reader::read_tag(source, *scratch_, tag, output, actualClass);
    if (!okay) throw Error("could not authenticate and decode TagHash 0x" + [&] {
        std::ostringstream value; value << std::hex << std::uppercase << std::setw(8)
                                        << std::setfill('0') << tag; return value.str(); }());
    if (classId != nullptr) *classId = actualClass;
    return output;
}

std::uint32_t tag_for(std::uint16_t packageId, std::uint32_t entryIndex) noexcept {
    return kTagBase + (static_cast<std::uint32_t>(packageId) << 13U) + entryIndex;
}

std::uint16_t tag_package(std::uint32_t tag) {
    if (tag < kTagBase) throw Error("TagHash is below the package range");
    return static_cast<std::uint16_t>((tag - kTagBase) >> 13U);
}

std::uint32_t tag_entry(std::uint32_t tag) {
    if (tag < kTagBase) throw Error("TagHash is below the package range");
    return (tag - kTagBase) & 0x1FFFU;
}

Package build_package(const fs::path& output, std::uint16_t packageId, std::uint16_t patchId,
                      const Profile& profile, const std::vector<AuthoredEntry>& entries,
                      const std::vector<NamedTag>& namedTags,
                      const std::vector<WideHash>& wideHashes,
                      const std::vector<TagPair>& tagPairs, bool alternateKey,
                      const Runtime& runtime) {
    if (packageId < 0x100 || packageId > 0x19FF) throw Error("package_id must be within 0x0100..0x19ff");
    if (entries.empty() || entries.size() > kEntryLimit) throw Error("package needs 1..8192 entries");
    for (const auto& row : namedTags) {
        if (row.entryIndex >= entries.size() || row.name.empty() || row.name.find('\0') != std::string::npos)
            throw Error("named-tag row is invalid");
    }
    std::uint64_t previous = 0;
    bool first = true;
    for (const auto& row : wideHashes) {
        if (row.entryIndex >= entries.size() || (!first && row.hash <= previous)) throw Error("wide hashes must be unique and sorted");
        first = false; previous = row.hash;
    }
    BuiltLayout layout = layout_entries(entries);
    if (layout.logicalBlocks.empty()) throw Error("package must contain nonempty payload data");
    OutputParts parts = compose(packageId, patchId, profile, layout.entries, {},
                                layout.logicalBlocks, namedTags, wideHashes, tagPairs,
                                alternateKey, runtime);
    write_output(output, parts);
    return Package::open(output, true);
}

Package build_patch(const fs::path& output, const Package& base,
                    const std::vector<Replacement>& replacements, const Runtime& runtime) {
    if (!base.header.pre_bl()) throw Error("patch base must use the pre-BL package format");
    if (replacements.empty()) throw Error("patch needs at least one replacement");
    if (base.header.patchId == 0xFF) throw Error("base patch id is already 255");
    std::vector<AuthoredEntry> authored;
    std::vector<std::uint32_t> indices;
    for (const auto& replacement : replacements) {
        if (replacement.entryIndex >= base.entries.size() || replacement.data.empty()) throw Error("replacement entry is invalid");
        if (std::find(indices.begin(), indices.end(), replacement.entryIndex) != indices.end()) throw Error("entry is replaced more than once");
        indices.push_back(replacement.entryIndex);
        const Entry& original = base.entries[replacement.entryIndex];
        authored.push_back(AuthoredEntry{original.reference, original.typeInfo, replacement.data});
    }
    BuiltLayout layout = layout_entries(authored, static_cast<std::uint32_t>(base.blocks.size()));
    std::vector<Entry> allEntries = base.entries;
    for (std::size_t index = 0; index < indices.size(); ++index) allEntries[indices[index]] = layout.entries[index];
    OutputParts parts = compose(base.header.packageId, base.header.patchId + 1,
                                Profile::from_package(base), allEntries, base.blocks,
                                layout.logicalBlocks, base.namedTags, base.wideHashes,
                                base.tagPairs, false, runtime);
    write_output(output, parts);
    return Package::open(output, true);
}

fs::path make_isolated_chain(const Package& base, const fs::path* addition) {
    static std::atomic<unsigned> counter{};
    const fs::path root = fs::temp_directory_path()
                          / (L"repkg-chain-" + std::to_wstring(GetCurrentProcessId()) + L"-"
                             + std::to_wstring(GetTickCount64()) + L"-"
                             + std::to_wstring(counter.fetch_add(1)));
    fs::create_directories(root);
    try {
        const std::wstring family = family_stem(base.path);
        const std::wregex pattern(L"^" + family + LR"(_([0-9]+)\.pkg$)", std::regex::icase);
        for (const auto& item : fs::directory_iterator(base.path.parent_path())) {
            if (!item.is_regular_file()) continue;
            std::wsmatch match;
            const std::wstring name = item.path().filename().wstring();
            if (!std::regex_match(name, match, pattern)) continue;
            const unsigned long patch = std::stoul(match[1].str());
            if (patch > base.header.patchId || patch > 0xFFFFUL) continue;
            const fs::path target = root / chain_leaf(
                base.header.packageId, static_cast<std::uint16_t>(patch));
            std::error_code error;
            fs::create_hard_link(item.path(), target, error);
            if (error) fs::copy_file(item.path(), target, fs::copy_options::overwrite_existing);
        }
        if (addition != nullptr) {
            const Package added = Package::open(*addition, false);
            if (added.header.packageId != base.header.packageId) {
                throw Error("isolated-chain addition belongs to another package id");
            }
            const fs::path target = root / chain_leaf(added.header.packageId,
                                                       added.header.patchId);
            std::error_code error;
            fs::create_hard_link(*addition, target, error);
            if (error) fs::copy_file(*addition, target, fs::copy_options::overwrite_existing);
        }
        return root;
    } catch (...) {
        remove_tree(root);
        throw;
    }
}

void remove_tree(const fs::path& path) noexcept {
    std::error_code ignored;
    fs::remove_all(path, ignored);
}

std::vector<std::byte> read_binary(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw Error("could not open " + path.string());
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max()) throw Error("file is too large");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw Error("short read from " + path.string());
    return bytes;
}

std::string read_text(const fs::path& path) {
    const auto bytes = read_binary(path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_binary(const fs::path& path, std::span<const std::byte> bytes) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || (!bytes.empty() && !stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
        throw Error("could not write " + path.string());
    }
}

} // namespace repkg
