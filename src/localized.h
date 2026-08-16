#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace repkg {

struct LocalizedResult {
    std::uint32_t englishTag{};
    std::uint32_t stringHash{};
    std::size_t stringIndex{};
    std::size_t oldByteLength{};
    std::size_t newByteLength{};
    std::uint16_t cipher{};
    std::string original;
    std::string replacement;
};

[[nodiscard]] std::vector<std::byte>
replace_localized_string(std::span<const std::byte> container,
                         std::span<const std::byte> englishData,
                         std::uint32_t stringHash,
                         std::string replacement,
                         LocalizedResult& result);

} // namespace repkg
