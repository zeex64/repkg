#include "texture.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#include "package.h"

namespace repkg {
namespace {

constexpr std::size_t kTigerHeaderSize = 0x28;
constexpr std::size_t kDdsLegacyHeaderSize = 4 + 124;
constexpr std::size_t kDdsDx10HeaderSize = kDdsLegacyHeaderSize + 20;
constexpr std::uint32_t kDdsMagic = 0x20534444U;
constexpr std::uint32_t kDx10FourCc = 0x30315844U;
constexpr std::uint32_t kDdsPixelAlphaPixels = 0x1U;
constexpr std::uint32_t kDdsPixelAlpha = 0x2U;
constexpr std::uint32_t kDdsPixelFourCc = 0x4U;
constexpr std::uint32_t kDdsPixelRgb = 0x40U;
constexpr std::uint32_t kDdsPixelLuminance = 0x20000U;
constexpr std::uint32_t kDdsCapsTexture = 0x1000U;
constexpr std::uint32_t kDdsCapsComplex = 0x8U;
constexpr std::uint32_t kDdsCaps2Cube = 0xFE00U;
constexpr std::uint32_t kDdsCaps2Volume = 0x200000U;
constexpr std::uint32_t kDdsResourceTexture2d = 3U;
constexpr std::uint32_t kDdsResourceTexture3d = 4U;
constexpr std::uint32_t kDdsResourceMiscCube = 0x4U;

constexpr std::uint32_t four_cc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a))
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8U)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16U)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24U);
}

template <typename T>
[[nodiscard]] T load(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw Error("texture file is truncated");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

template <typename T>
void store(std::span<std::byte> bytes, std::size_t offset, T value) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw Error("internal texture header overflow");
    }
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

[[nodiscard]] std::uint32_t legacy_dxgi_format(std::span<const std::byte> input) {
    const std::uint32_t flags = load<std::uint32_t>(input, 0x50);
    const std::uint32_t formatFourCc = load<std::uint32_t>(input, 0x54);
    const std::uint32_t bits = load<std::uint32_t>(input, 0x58);
    const std::uint32_t red = load<std::uint32_t>(input, 0x5C);
    const std::uint32_t green = load<std::uint32_t>(input, 0x60);
    const std::uint32_t blue = load<std::uint32_t>(input, 0x64);
    const std::uint32_t alpha = load<std::uint32_t>(input, 0x68);

    if ((flags & kDdsPixelFourCc) != 0) {
        switch (formatFourCc) {
        case four_cc('D', 'X', 'T', '1'): return 71U; // BC1_UNORM
        case four_cc('D', 'X', 'T', '3'): return 74U; // BC2_UNORM
        case four_cc('D', 'X', 'T', '5'): return 77U; // BC3_UNORM
        case four_cc('A', 'T', 'I', '1'):
        case four_cc('B', 'C', '4', 'U'): return 80U; // BC4_UNORM
        case four_cc('A', 'T', 'I', '2'):
        case four_cc('B', 'C', '5', 'U'): return 83U; // BC5_UNORM
        default: throw Error("unsupported legacy DDS FourCC");
        }
    }
    if ((flags & kDdsPixelRgb) != 0 && bits == 32U) {
        if (red == 0x000000FFU && green == 0x0000FF00U && blue == 0x00FF0000U
            && alpha == 0xFF000000U) {
            return 28U; // R8G8B8A8_UNORM
        }
        if (red == 0x00FF0000U && green == 0x0000FF00U && blue == 0x000000FFU) {
            if (alpha == 0xFF000000U) return 87U; // B8G8R8A8_UNORM
            if (alpha == 0U) return 88U;          // B8G8R8X8_UNORM
        }
    }
    if ((flags & kDdsPixelRgb) != 0 && bits == 16U) {
        if (red == 0xF800U && green == 0x07E0U && blue == 0x001FU && alpha == 0U) {
            return 85U; // B5G6R5_UNORM
        }
        if (red == 0x7C00U && green == 0x03E0U && blue == 0x001FU
            && alpha == 0x8000U) {
            return 86U; // B5G5R5A1_UNORM
        }
        if (red == 0x0F00U && green == 0x00F0U && blue == 0x000FU
            && alpha == 0xF000U) {
            return 115U; // B4G4R4A4_UNORM
        }
    }
    if ((flags & kDdsPixelLuminance) != 0 && bits == 8U && red == 0xFFU) {
        return 61U; // R8_UNORM
    }
    if ((flags & kDdsPixelLuminance) != 0 && (flags & kDdsPixelAlphaPixels) != 0
        && bits == 16U && red == 0x00FFU && alpha == 0xFF00U) {
        return 49U; // R8G8_UNORM
    }
    if ((flags & kDdsPixelAlpha) != 0 && bits == 8U && alpha == 0xFFU) {
        return 65U; // A8_UNORM
    }
    throw Error("unsupported legacy DDS pixel format; save as DDS DX10 instead");
}

} // namespace

bool parse_texture_descriptor(std::span<const std::byte> bytes,
                              TextureDescriptor& descriptor) noexcept {
    if (bytes.size() != kTigerHeaderSize) return false;
    std::memcpy(&descriptor.dataSize, bytes.data(), sizeof descriptor.dataSize);
    std::memcpy(&descriptor.format, bytes.data() + 4, sizeof descriptor.format);
    std::memcpy(&descriptor.unknown08, bytes.data() + 8, sizeof descriptor.unknown08);
    std::uint16_t marker = 0;
    std::memcpy(&marker, bytes.data() + 0x0C, sizeof marker);
    if (marker != 0xCAFEU) return false;
    std::memcpy(&descriptor.width, bytes.data() + 0x0E, sizeof descriptor.width);
    std::memcpy(&descriptor.height, bytes.data() + 0x10, sizeof descriptor.height);
    std::memcpy(&descriptor.depth, bytes.data() + 0x12, sizeof descriptor.depth);
    std::memcpy(&descriptor.arraySize, bytes.data() + 0x14, sizeof descriptor.arraySize);
    std::memcpy(descriptor.unknownWords.data(), bytes.data() + 0x16,
                descriptor.unknownWords.size() * sizeof(std::uint16_t));
    std::memcpy(&descriptor.largeBuffer, bytes.data() + 0x24,
                sizeof descriptor.largeBuffer);
    return descriptor.width != 0 && descriptor.height != 0 && descriptor.depth != 0
           && descriptor.arraySize != 0;
}

std::vector<std::byte> encode_texture_descriptor(const TextureDescriptor& descriptor) {
    if (descriptor.width == 0 || descriptor.height == 0 || descriptor.depth == 0
        || descriptor.arraySize == 0) {
        throw Error("texture dimensions, depth and array size must be nonzero");
    }
    std::vector<std::byte> output(kTigerHeaderSize);
    store(output, 0x00, descriptor.dataSize);
    store(output, 0x04, descriptor.format);
    store(output, 0x08, descriptor.unknown08);
    store<std::uint16_t>(output, 0x0C, 0xCAFEU);
    store(output, 0x0E, descriptor.width);
    store(output, 0x10, descriptor.height);
    store(output, 0x12, descriptor.depth);
    store(output, 0x14, descriptor.arraySize);
    std::memcpy(output.data() + 0x16, descriptor.unknownWords.data(),
                descriptor.unknownWords.size() * sizeof(std::uint16_t));
    store(output, 0x24, descriptor.largeBuffer);
    return output;
}

void write_texture_dds(const std::filesystem::path& path,
                       const TextureDescriptor& descriptor,
                       std::span<const std::byte> pixels,
                       std::uint8_t textureSubtype) {
    if (pixels.size() != descriptor.dataSize) {
        throw Error("texture descriptor size does not match its pixel entry");
    }
    std::vector<std::byte> output(kDdsDx10HeaderSize + pixels.size());
    store(output, 0x00, kDdsMagic);
    store<std::uint32_t>(output, 0x04, 124U);
    std::uint32_t flags = 0x00081007U;
    if (textureSubtype == 3) flags |= 0x00800000U;
    store(output, 0x08, flags);
    store<std::uint32_t>(output, 0x0C, descriptor.height);
    store<std::uint32_t>(output, 0x10, descriptor.width);
    store<std::uint32_t>(output, 0x14, descriptor.dataSize);
    store<std::uint32_t>(output, 0x18,
                         textureSubtype == 3 ? descriptor.depth : 0U);
    store<std::uint32_t>(output, 0x1C, 1U);
    store<std::uint32_t>(output, 0x4C, 32U);
    store<std::uint32_t>(output, 0x50, 0x4U);
    store(output, 0x54, kDx10FourCc);
    std::uint32_t caps = kDdsCapsTexture;
    if (textureSubtype != 1 || descriptor.arraySize > 1) caps |= kDdsCapsComplex;
    store(output, 0x6C, caps);
    if (textureSubtype == 2) store(output, 0x70, kDdsCaps2Cube);
    store(output, 0x80, descriptor.format);
    store(output, 0x84,
          textureSubtype == 3 ? kDdsResourceTexture3d : kDdsResourceTexture2d);
    store(output, 0x88, textureSubtype == 2 ? kDdsResourceMiscCube : 0U);
    const std::uint32_t arraySize = textureSubtype == 3
                                        ? 1U
                                        : (std::max<std::uint32_t>)(1U,
                                              textureSubtype == 2
                                                  ? descriptor.arraySize / 6U
                                                  : descriptor.arraySize);
    store(output, 0x8C, arraySize);
    std::copy(pixels.begin(), pixels.end(), output.begin() + kDdsDx10HeaderSize);
    write_binary(path, output);
}

EditableTexture read_texture_dds(const std::filesystem::path& path,
                                 const TextureDescriptor& templateDescriptor,
                                 std::uint8_t textureSubtype) {
    const std::vector<std::byte> input = read_binary(path);
    if (input.size() < kDdsLegacyHeaderSize
        || load<std::uint32_t>(input, 0x00) != kDdsMagic
        || load<std::uint32_t>(input, 0x04) != 124U
        || load<std::uint32_t>(input, 0x4C) != 32U) {
        throw Error("editable texture must be a valid DDS file");
    }
    const bool hasDx10Header = (load<std::uint32_t>(input, 0x50) & kDdsPixelFourCc) != 0
                               && load<std::uint32_t>(input, 0x54) == kDx10FourCc;
    const std::size_t pixelOffset = hasDx10Header ? kDdsDx10HeaderSize
                                                  : kDdsLegacyHeaderSize;
    if (input.size() <= pixelOffset) throw Error("DDS pixel payload is empty");
    const std::uint32_t width = load<std::uint32_t>(input, 0x10);
    const std::uint32_t height = load<std::uint32_t>(input, 0x0C);
    const std::uint32_t depthField = load<std::uint32_t>(input, 0x18);
    const std::uint32_t mipCount = (std::max)(1U, load<std::uint32_t>(input, 0x1C));
    const std::uint32_t caps2 = load<std::uint32_t>(input, 0x70);
    const std::uint32_t resourceDimension = hasDx10Header
                                                ? load<std::uint32_t>(input, 0x84)
                                                : (textureSubtype == 3
                                                       ? kDdsResourceTexture3d
                                                       : kDdsResourceTexture2d);
    const std::uint32_t miscFlag = hasDx10Header
                                       ? load<std::uint32_t>(input, 0x88)
                                       : ((caps2 & kDdsCaps2Cube) != 0
                                              ? kDdsResourceMiscCube
                                              : 0U);
    const std::uint32_t ddsArray = hasDx10Header ? load<std::uint32_t>(input, 0x8C) : 1U;
    const std::uint32_t maximumMipCount = 1U + static_cast<std::uint32_t>(
        std::bit_width((std::max)(width, (std::max)(height, (std::max)(1U, depthField))))
        - 1U);
    if (width == 0 || height == 0 || width > 0xFFFFU || height > 0xFFFFU
        || ddsArray == 0 || ddsArray > 0xFFFFU || mipCount > maximumMipCount) {
        throw Error("DDS dimensions, array size or mip count are unsupported");
    }
    if ((textureSubtype == 3 && resourceDimension != kDdsResourceTexture3d)
        || (textureSubtype != 3 && resourceDimension != kDdsResourceTexture2d)
        || (textureSubtype == 2 && (miscFlag & kDdsResourceMiscCube) == 0)) {
        throw Error("DDS resource dimension does not match the Tiger texture subtype");
    }
    if (!hasDx10Header && textureSubtype == 3 && (caps2 & kDdsCaps2Volume) == 0) {
        throw Error("legacy DDS is not marked as a volume texture");
    }
    TextureDescriptor descriptor = templateDescriptor;
    descriptor.format = hasDx10Header ? load<std::uint32_t>(input, 0x80)
                                      : legacy_dxgi_format(input);
    descriptor.width = static_cast<std::uint16_t>(width);
    descriptor.height = static_cast<std::uint16_t>(height);
    descriptor.depth = static_cast<std::uint16_t>(
        textureSubtype == 3 ? (std::max)(1U, depthField) : 1U);
    const std::uint32_t tigerArray = textureSubtype == 2 ? ddsArray * 6U : ddsArray;
    if (tigerArray > 0xFFFFU) throw Error("DDS cube array is too large");
    descriptor.arraySize = static_cast<std::uint16_t>(tigerArray);
    EditableTexture output{descriptor,
                           std::vector<std::byte>(input.begin() + pixelOffset,
                                                  input.end())};
    if (output.pixels.empty() || output.pixels.size() > 0xFFFFFFFFULL) {
        throw Error("DDS pixel payload is empty or too large");
    }
    output.descriptor.dataSize = static_cast<std::uint32_t>(output.pixels.size());
    return output;
}

} // namespace repkg
