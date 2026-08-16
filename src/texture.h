#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace repkg {

struct TextureDescriptor {
    std::uint32_t dataSize{};
    std::uint32_t format{};
    std::uint32_t unknown08{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t depth{};
    std::uint16_t arraySize{};
    std::array<std::uint16_t, 7> unknownWords{};
    std::uint32_t largeBuffer{0xFFFFFFFFU};
};

struct EditableTexture {
    TextureDescriptor descriptor;
    std::vector<std::byte> pixels;
};

[[nodiscard]] bool parse_texture_descriptor(std::span<const std::byte> bytes,
                                            TextureDescriptor& descriptor) noexcept;
[[nodiscard]] std::vector<std::byte>
encode_texture_descriptor(const TextureDescriptor& descriptor);

void write_texture_dds(const std::filesystem::path& path,
                       const TextureDescriptor& descriptor,
                       std::span<const std::byte> pixels,
                       std::uint8_t textureSubtype);

[[nodiscard]] EditableTexture read_texture_dds(const std::filesystem::path& path,
                                               const TextureDescriptor& templateDescriptor,
                                               std::uint8_t textureSubtype);

} // namespace repkg
