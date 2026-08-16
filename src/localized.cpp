#include "localized.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <tuple>

#include "package.h"

namespace repkg {
namespace {

constexpr std::uint32_t kLiteralPartHash = 0x811C9DC5U;
constexpr std::size_t kPartSize = 0x20;
constexpr std::size_t kCombinationSize = 0x10;

template <typename T>
[[nodiscard]] T get(std::span<const std::byte> bytes, std::size_t offset,
                    std::string_view label) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw Error(std::string(label) + " is outside localized data");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

template <typename T>
void put(std::span<std::byte> bytes, std::size_t offset, T value,
         std::string_view label) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw Error(std::string(label) + " is outside rebuilt localized data");
    }
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

[[nodiscard]] std::size_t relative(std::span<const std::byte> bytes,
                                   std::size_t field, std::int64_t bias = 0) {
    const std::int64_t delta = get<std::int64_t>(bytes, field, "relative pointer");
    if (field > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error("localized pointer field is too large");
    }
    const std::int64_t target = static_cast<std::int64_t>(field) + delta + bias;
    if (target < 0 || static_cast<std::uint64_t>(target) > bytes.size()) {
        throw Error("localized relative pointer is outside its entry");
    }
    return static_cast<std::size_t>(target);
}

[[nodiscard]] std::pair<std::uint64_t, std::size_t>
table(std::span<const std::byte> bytes, std::size_t countField) {
    return {get<std::uint64_t>(bytes, countField, "table count"),
            relative(bytes, countField + 8, 16)};
}

[[nodiscard]] unsigned encoded_width(unsigned char lead) noexcept {
    if (lead <= 0xBF) return 1;
    if (lead <= 0xDF) return 2;
    if (lead <= 0xEF) return 3;
    return 4;
}

[[nodiscard]] unsigned plain_width(unsigned char lead) noexcept {
    if (lead <= 0x7F) return 1;
    if (lead <= 0xDF) return 2;
    if (lead <= 0xEF) return 3;
    return 4;
}

[[nodiscard]] std::string decode(std::span<const std::byte> encoded, std::uint16_t cipher) {
    std::string output(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        const unsigned width = encoded_width(static_cast<unsigned char>(encoded[offset]));
        if (width > encoded.size() - offset) throw Error("localized string ends inside a UTF-8 codepoint");
        const std::size_t shifted = offset + width - 1;
        output[shifted] = static_cast<char>(static_cast<unsigned char>(output[shifted]) + cipher);
        offset += width;
    }
    return output;
}

[[nodiscard]] std::vector<std::byte> encode(std::string_view text, std::uint16_t cipher) {
    std::vector<std::byte> output(text.size());
    std::memcpy(output.data(), text.data(), text.size());
    std::size_t offset = 0;
    while (offset < output.size()) {
        const unsigned width = plain_width(static_cast<unsigned char>(output[offset]));
        if (width > output.size() - offset) throw Error("replacement ends inside a UTF-8 codepoint");
        const std::size_t shifted = offset + width - 1;
        output[shifted] = static_cast<std::byte>(static_cast<unsigned char>(output[shifted]) - cipher);
        if (width == 1 && static_cast<unsigned char>(output[offset]) > 0xBF) {
            throw Error("replacement cannot be represented by the literal cipher");
        }
        offset += width;
    }
    if (decode(output, cipher) != text) throw Error("localized codec round trip failed");
    return output;
}

void validate_literals(std::span<const std::byte> data) {
    const auto [count, combinations] = table(data, 0x48);
    if (count > (data.size() - combinations) / kCombinationSize) {
        throw Error("localized combination table exceeds its entry");
    }
    for (std::size_t combinationIndex = 0; combinationIndex < count; ++combinationIndex) {
        const std::size_t combination = combinations + combinationIndex * kCombinationSize;
        const std::int64_t partCount = get<std::int64_t>(data, combination + 8, "part count");
        if (partCount < 0 || partCount > 0x10000) throw Error("localized part count is invalid");
        if (partCount == 0) continue;
        const std::size_t parts = relative(data, combination);
        if (static_cast<std::uint64_t>(partCount) > (data.size() - parts) / kPartSize) {
            throw Error("localized part table exceeds its entry");
        }
        for (std::size_t partIndex = 0; partIndex < static_cast<std::size_t>(partCount); ++partIndex) {
            const std::size_t part = parts + partIndex * kPartSize;
            if (get<std::uint32_t>(data, part + 0x10, "part hash") != kLiteralPartHash) continue;
            const std::size_t bytes = relative(data, part + 8);
            const std::uint16_t length = get<std::uint16_t>(data, part + 0x14, "literal length");
            const std::uint16_t cipher = get<std::uint16_t>(data, part + 0x18, "literal cipher");
            if (length > data.size() - bytes) throw Error("localized literal exceeds its entry");
            (void)decode(data.subspan(bytes, length), cipher);
        }
    }
}

} // namespace

std::vector<std::byte> replace_localized_string(std::span<const std::byte> container,
                                                std::span<const std::byte> englishData,
                                                std::uint32_t stringHash,
                                                std::string replacement,
                                                LocalizedResult& result) {
    if (container.size() < 0x4C) throw Error("localized string container is truncated");
    const std::uint32_t englishTag = get<std::uint32_t>(container, 0x18, "English tag");
    const auto [hashCount, hashes] = table(container, 0x08);
    if (hashCount > (container.size() - hashes) / 4) throw Error("localized hash table exceeds its container");
    std::size_t stringIndex = std::numeric_limits<std::size_t>::max();
    std::size_t matches = 0;
    for (std::size_t index = 0; index < hashCount; ++index) {
        if (get<std::uint32_t>(container, hashes + index * 4, "string hash") == stringHash) {
            stringIndex = index;
            ++matches;
        }
    }
    if (matches != 1) throw Error("localized hash must have exactly one matching row");
    if (englishData.size() < 0x58) throw Error("localized English data is truncated");
    const auto [combinationCount, combinations] = table(englishData, 0x48);
    if (combinationCount > (englishData.size() - combinations) / kCombinationSize
        || stringIndex >= combinationCount) throw Error("localized combination table is invalid");
    const std::size_t combination = combinations + stringIndex * kCombinationSize;
    const std::int64_t partCount = get<std::int64_t>(englishData, combination + 8, "part count");
    if (partCount != 1) throw Error("localized replacement requires exactly one literal part");
    const std::size_t part = relative(englishData, combination);
    if (part + kPartSize > englishData.size()) throw Error("localized part exceeds its entry");
    if (get<std::uint32_t>(englishData, part + 0x10, "part hash") != kLiteralPartHash) {
        throw Error("localized string part is a variable, not literal text");
    }
    const std::size_t dataOffset = relative(englishData, part + 8);
    const std::uint16_t oldLength = get<std::uint16_t>(englishData, part + 0x14, "literal length");
    const std::uint16_t cipher = get<std::uint16_t>(englishData, part + 0x18, "literal cipher");
    if (oldLength > englishData.size() - dataOffset) throw Error("localized literal exceeds its entry");
    const std::string original = decode(englishData.subspan(dataOffset, oldLength), cipher);
    const std::vector<std::byte> encoded = encode(replacement, cipher);
    if (encoded.size() > 0xFFFF) throw Error("localized literal exceeds its 16-bit length");
    const std::size_t oldEnd = dataOffset + oldLength;
    const std::int64_t delta = static_cast<std::int64_t>(encoded.size()) - oldLength;

    struct Pointer { std::size_t field; std::int64_t bias; std::size_t target; };
    std::vector<Pointer> pointers;
    for (const std::size_t countField : {0x08U, 0x18U, 0x28U, 0x38U, 0x48U}) {
        if (get<std::uint64_t>(englishData, countField, "array count") != 0) {
            pointers.push_back(Pointer{countField + 8, 16, relative(englishData, countField + 8, 16)});
        }
    }
    for (std::size_t combinationIndex = 0; combinationIndex < combinationCount; ++combinationIndex) {
        const std::size_t row = combinations + combinationIndex * kCombinationSize;
        const std::int64_t count = get<std::int64_t>(englishData, row + 8, "part count");
        if (count < 0 || count > 0x10000) throw Error("localized part count is invalid");
        if (count == 0) continue;
        const std::size_t parts = relative(englishData, row);
        if (static_cast<std::uint64_t>(count) > (englishData.size() - parts) / kPartSize) {
            throw Error("localized part table exceeds its entry");
        }
        pointers.push_back(Pointer{row, 0, parts});
        for (std::size_t partIndex = 0; partIndex < static_cast<std::size_t>(count); ++partIndex) {
            const std::size_t item = parts + partIndex * kPartSize;
            if (get<std::uint32_t>(englishData, item + 0x10, "part hash") == kLiteralPartHash) {
                pointers.push_back(Pointer{item + 8, 0, relative(englishData, item + 8)});
            }
        }
    }

    const std::uint64_t stringBytes = get<std::uint64_t>(englishData, 0x38, "string byte count");
    const std::size_t stringStart = relative(englishData, 0x40, 16);
    if (dataOffset < stringStart || oldEnd > stringStart + stringBytes) {
        throw Error("replacement is outside the localized string-data array");
    }
    if (stringStart < 0x20) throw Error("localized string-data header is invalid");
    const std::size_t stringHeader = stringStart - 0x20;

    std::vector<std::byte> output;
    output.reserve(static_cast<std::size_t>(static_cast<std::int64_t>(englishData.size()) + delta));
    output.insert(output.end(), englishData.begin(), englishData.begin() + dataOffset);
    output.insert(output.end(), encoded.begin(), encoded.end());
    output.insert(output.end(), englishData.begin() + oldEnd, englishData.end());
    auto relocate = [&](std::size_t position) -> std::size_t {
        if (position <= dataOffset) return position;
        if (position >= oldEnd) return static_cast<std::size_t>(static_cast<std::int64_t>(position) + delta);
        throw Error("localized pointer intersects resized literal data");
    };
    for (const Pointer& pointer : pointers) {
        const std::size_t field = relocate(pointer.field);
        const std::size_t target = relocate(pointer.target);
        put<std::int64_t>(output, field,
                          static_cast<std::int64_t>(target) - static_cast<std::int64_t>(field)
                              - pointer.bias,
                          "relocated pointer");
    }
    const std::size_t newPart = relocate(part);
    put<std::uint16_t>(output, newPart + 0x14, static_cast<std::uint16_t>(encoded.size()), "literal length");
    put<std::uint64_t>(output, 0, output.size(), "localized object size");
    const std::int64_t newStringBytes = static_cast<std::int64_t>(stringBytes) + delta;
    if (newStringBytes < 0) throw Error("localized string-data size underflow");
    put<std::uint64_t>(output, 0x38, static_cast<std::uint64_t>(newStringBytes), "string byte count");
    put<std::uint64_t>(output, relocate(stringHeader) + 0x10,
                       static_cast<std::uint64_t>(newStringBytes), "string capacity");
    validate_literals(output);

    result = LocalizedResult{englishTag, stringHash, stringIndex, oldLength,
                             encoded.size(), cipher, original, std::move(replacement)};
    return output;
}

} // namespace repkg
