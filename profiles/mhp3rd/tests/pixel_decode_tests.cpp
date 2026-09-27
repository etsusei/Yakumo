#include "resources/pixel_decode.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

using mhp3rd::resources::PaletteFormat;
using mhp3rd::resources::PixelDecodeError;
using mhp3rd::resources::PixelDecodeSpec;
using mhp3rd::resources::PixelFormat;
using mhp3rd::resources::PixelLayout;
using mhp3rd::resources::decode_pixels;

namespace {

void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "pixel_decode_tests: " << message << '\n';
        std::abort();
    }
}

PixelDecodeSpec spec(PixelFormat format, std::uint32_t width, std::uint32_t height) {
    PixelDecodeSpec result{};
    result.format = format;
    result.width = width;
    result.height = height;
    result.layout = PixelLayout::Linear;
    return result;
}

void direct_formats() {
    {
        const auto image = decode_pixels(spec(PixelFormat::Rgba5650, 3u, 1u),
                                         std::array<std::uint8_t, 6>{0x1Fu, 0u, 0xE0u, 0x07u, 0u, 0xF8u});
        check(image.ok() && image.rgba == std::vector<std::uint32_t>{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u},
              "5650 expands red, green and blue in GE byte order");
    }
    {
        const auto image = decode_pixels(spec(PixelFormat::Rgba5551, 2u, 1u),
                                         std::array<std::uint8_t, 4>{0x1Fu, 0x80u, 0u, 0x7Cu});
        check(image.ok() && image.rgba == std::vector<std::uint32_t>{0xFF0000FFu, 0x00FF0000u},
              "5551 retains one-bit alpha");
    }
    {
        const auto image = decode_pixels(spec(PixelFormat::Rgba4444, 2u, 1u),
                                         std::array<std::uint8_t, 4>{0x2Au, 0xF1u, 0x23u, 0x01u});
        check(image.ok() && image.rgba == std::vector<std::uint32_t>{0xFF1122AAu, 0x00112233u},
              "4444 expands each nibble independently");
    }
    {
        const auto image = decode_pixels(spec(PixelFormat::Rgba8888, 1u, 1u),
                                         std::array<std::uint8_t, 4>{0x12u, 0x34u, 0x56u, 0x78u});
        check(image.ok() && image.rgba == std::vector<std::uint32_t>{0x78563412u},
              "8888 uses explicit little-endian loads");
    }
}

void indexed_formats() {
    {
        auto indexed = spec(PixelFormat::Clut4, 2u, 1u);
        indexed.palette_format = PaletteFormat::Rgba5650;
        indexed.palette_mask = 15u;
        const std::array<std::uint8_t, 1> texels{0x10u};
        const std::array<std::uint8_t, 4> palette{0x1Fu, 0u, 0u, 0xF8u};
        const auto image = decode_pixels(indexed, texels, palette);
        check(image.ok() && image.rgba == std::vector<std::uint32_t>{0xFF0000FFu, 0xFFFF0000u} &&
                  image.required_palette_bytes == 4u,
              "CLUT4 takes low nibble before high nibble and decodes 5650 entries");
    }
    {
        auto indexed = spec(PixelFormat::Clut8, 1u, 1u);
        indexed.palette_format = PaletteFormat::Rgba5551;
        indexed.palette_shift = 1u;
        indexed.palette_mask = 5u;
        indexed.palette_offset = 1u;
        std::array<std::uint8_t, 36> palette{};
        palette[34] = 0x1Fu;
        palette[35] = 0x80u;
        const auto image = decode_pixels(indexed, std::array<std::uint8_t, 1>{6u}, palette);
        check(image.ok() && image.rgba == std::vector<std::uint32_t>{0xFF0000FFu} &&
                  image.required_palette_bytes == 36u,
              "CLUT8 applies nonzero shift, mask and offset before 5551 lookup");
    }
    {
        auto indexed = spec(PixelFormat::Clut16, 1u, 1u);
        indexed.palette_format = PaletteFormat::Rgba4444;
        indexed.palette_mask = 255u;
        const std::array<std::uint8_t, 2> texels{2u, 0u};
        const std::array<std::uint8_t, 6> palette{0u, 0u, 0u, 0u, 0x2Au, 0xF1u};
        const auto image = decode_pixels(indexed, texels, palette);
        check(image.ok() && image.rgba == std::vector<std::uint32_t>{0xFF1122AAu},
              "CLUT16 indexes 4444 entries");
    }
    {
        auto indexed = spec(PixelFormat::Clut32, 1u, 1u);
        indexed.palette_format = PaletteFormat::Rgba8888;
        indexed.palette_shift = 24u;
        indexed.palette_mask = 0xFFu;
        indexed.palette_offset = 1u;
        std::array<std::uint8_t, 76> palette{};
        palette[72] = 0x12u;
        palette[73] = 0x34u;
        palette[74] = 0x56u;
        palette[75] = 0x78u;
        const auto image = decode_pixels(indexed, std::array<std::uint8_t, 4>{0x78u, 0x56u, 0x34u, 0x12u},
                                         palette);
        check(image.ok() && image.rgba == std::vector<std::uint32_t>{0x78563412u} &&
                  image.required_palette_bytes == 76u,
              "CLUT32 reads the full raw index; offset combines with OR, not addition");
    }
}

void block_formats() {
    {
        // Indices 0,1,2,3, then blue/red endpoints in PSP order. Endpoint 0
        // is lower, so index 3 is transparent and index 2 is their midpoint.
        const std::array<std::uint8_t, 8> block{0xE4u, 0u, 0u, 0u, 0x1Fu, 0u, 0u, 0xF8u};
        const auto image = decode_pixels(spec(PixelFormat::Dxt1, 4u, 4u), block);
        check(image.ok() && image.rgba.size() == 16u && image.rgba[0] == 0xFFFF0000u &&
                  image.rgba[1] == 0xFF0000FFu && image.rgba[2] == 0xFF7F007Fu &&
                  image.rgba[3] == 0u,
              "DXT1 decodes index-first block and transparent fourth color");
    }
    {
        const std::array<std::uint8_t, 8> block{0xE4u, 0u, 0u, 0u, 0u, 0xF8u, 0x1Fu, 0u};
        const auto image = decode_pixels(spec(PixelFormat::Dxt1, 3u, 2u), block);
        check(image.ok() && image.rgba.size() == 6u && image.rgba[2] == 0xFF5500AAu &&
                  image.required_texel_bytes == 8u,
              "DXT1 uses four-color thirds and clips partial blocks");
    }
    {
        const std::array<std::uint8_t, 16> block{0xE4u, 0u, 0u, 0u, 0u, 0xF8u, 0x1Fu, 0u,
                                                 0xF0u, 0x81u, 0u, 0u, 0u, 0u, 0u, 0u};
        const auto image = decode_pixels(spec(PixelFormat::Dxt3, 4u, 4u), block);
        check(image.ok() && image.rgba[0] == 0x000000FFu && image.rgba[1] == 0xFFFF0000u &&
                  image.rgba[2] == 0x115500AAu && image.rgba[3] == 0x88AA0055u,
              "DXT3 alpha nibbles follow the color portion");
    }
    {
        // Alpha codes 0,1,2,7 give 200,100,185,114 with integer division.
        const std::array<std::uint8_t, 16> block{0u, 0u, 0u, 0u, 0u, 0xF8u, 0x1Fu, 0u,
                                                 0x88u, 0x0Eu, 0u, 0u, 0u, 0u, 200u, 100u};
        const auto image = decode_pixels(spec(PixelFormat::Dxt5, 4u, 4u), block);
        check(image.ok() && image.rgba[0] == 0xC80000FFu && image.rgba[1] == 0x640000FFu &&
                  image.rgba[2] == 0xB90000FFu && image.rgba[3] == 0x720000FFu,
              "DXT5 interpolates the eight-alpha branch with legacy integer rounding");
    }
    {
        // Codes 6 and 7 are zero and 255 when alpha0 <= alpha1.
        const std::array<std::uint8_t, 16> block{0u, 0u, 0u, 0u, 0u, 0xF8u, 0x1Fu, 0u,
                                                 0x3Eu, 0u, 0u, 0u, 0u, 0u, 40u, 80u};
        const auto image = decode_pixels(spec(PixelFormat::Dxt5, 4u, 4u), block);
        check(image.ok() && image.rgba[0] == 0x000000FFu && image.rgba[1] == 0xFF0000FFu,
              "DXT5 retains zero and full alpha in the six-alpha branch");
    }
}

void swizzle_and_windows() {
    {
        auto indexed = spec(PixelFormat::Clut8, 32u, 8u);
        indexed.palette_format = PaletteFormat::Rgba8888;
        indexed.palette_mask = 255u;
        indexed.layout = PixelLayout::Swizzled16x8;
        std::array<std::uint8_t, 256> encoded{};
        std::array<std::uint8_t, 1024> palette{};
        for (std::size_t i = 0u; i < encoded.size(); ++i) {
            encoded[i] = static_cast<std::uint8_t>(i);
            palette[i * 4u] = static_cast<std::uint8_t>(i);
            palette[i * 4u + 3u] = 255u;
        }
        const auto original = encoded;
        const auto image = decode_pixels(indexed, encoded, palette);
        check(image.ok() && image.rgba[0] == 0xFF000000u && image.rgba[16] == 0xFF000080u &&
                  image.rgba[32] == 0xFF000010u && encoded == original && !image.legacy_unaligned_bypass,
              "strict 16x8 unswizzle maps tiles and leaves input immutable");
        indexed.layout = PixelLayout::LegacySwizzleCompatibility;
        const auto compatible = decode_pixels(indexed, encoded, palette);
        check(compatible.ok() && compatible.rgba == image.rgba && !compatible.legacy_unaligned_bypass,
              "aligned compatibility layout performs the same unswizzle");
    }
    {
        auto indexed = spec(PixelFormat::Clut8, 3u, 2u);
        indexed.palette_format = PaletteFormat::Rgba8888;
        indexed.palette_mask = 255u;
        const std::array<std::uint8_t, 6> encoded{1u, 2u, 3u, 4u, 5u, 6u};
        std::array<std::uint8_t, 28> palette{};
        for (std::size_t i = 1u; i <= 6u; ++i) {
            palette[i * 4u] = static_cast<std::uint8_t>(i);
            palette[i * 4u + 3u] = 255u;
        }
        indexed.layout = PixelLayout::Linear;
        const auto linear = decode_pixels(indexed, encoded, palette);
        indexed.layout = PixelLayout::Swizzled16x8;
        const auto strict = decode_pixels(indexed, encoded, palette);
        indexed.layout = PixelLayout::LegacySwizzleCompatibility;
        const auto compatible = decode_pixels(indexed, encoded, palette);
        check(linear.ok() && strict.error == PixelDecodeError::InvalidSwizzleAlignment && strict.rgba.empty() &&
                  compatible.ok() && compatible.rgba == linear.rgba && compatible.legacy_unaligned_bypass,
              "unaligned strict swizzle fails while named compatibility keeps linear bytes");
    }
    {
        auto wider = spec(PixelFormat::Rgba8888, 3u, 2u);
        wider.stride_pixels = 2u;
        const std::array<std::uint8_t, 20> encoded{1u, 0u, 0u, 255u, 2u, 0u, 0u, 255u,
                                                    3u, 0u, 0u, 255u, 4u, 0u, 0u, 255u,
                                                    5u, 0u, 0u, 255u};
        const auto image = decode_pixels(wider, encoded);
        check(image.ok() && image.required_texel_bytes == 16u &&
                  image.rgba == std::vector<std::uint32_t>{0xFF000001u, 0xFF000002u, 0xFF000003u,
                                                          0xFF000003u, 0xFF000004u, 0xFF000000u},
              "width beyond stride crosses rows but never reads beyond the exact window");
        const auto missing = decode_pixels(wider, std::span<const std::uint8_t>(encoded).first(15u));
        check(missing.error == PixelDecodeError::MissingTexelBytes && missing.required_texel_bytes == 16u &&
                  missing.rgba.empty(),
              "short encoded source reports complete window and no pixels");
    }
    {
        auto indexed = spec(PixelFormat::Clut8, 2u, 1u);
        indexed.palette_format = PaletteFormat::Rgba8888;
        indexed.palette_mask = 255u;
        const std::array<std::uint8_t, 2> encoded{0u, 3u};
        const std::array<std::uint8_t, 8> palette{1u, 0u, 0u, 255u, 2u, 0u, 0u, 255u};
        const auto missing = decode_pixels(indexed, encoded, palette);
        check(missing.error == PixelDecodeError::MissingPaletteBytes && missing.rgba.empty() &&
                  missing.required_palette_bytes == 16u,
              "first missing mapped palette entry reports a byte lower bound and no partial image");
    }
}

void rejected_inputs() {
    const std::array<std::uint8_t, 16> data{};
    {
        auto bad = spec(PixelFormat::Rgba8888, 1u, 1u);
        bad.format = static_cast<PixelFormat>(255u);
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidFormat, "unknown format is rejected");
    }
    {
        auto bad = spec(PixelFormat::Rgba8888, 1u, 1u);
        bad.layout = PixelLayout::Unspecified;
        check(decode_pixels(bad, data).error == PixelDecodeError::LayoutUnspecified,
              "unspecified layout is rejected");
        bad.layout = static_cast<PixelLayout>(255u);
        check(decode_pixels(bad, data).error == PixelDecodeError::LayoutUnspecified,
              "unknown layout is rejected");
    }
    {
        auto bad = spec(PixelFormat::Rgba8888, 0u, 1u);
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidExtent, "zero width is rejected");
        bad.width = 4097u;
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidExtent, "oversized axis is rejected");
        bad.width = 1u;
        bad.stride_pixels = 65536u;
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidExtent, "oversized stride is rejected");
        bad = spec(PixelFormat::Clut4, 1u, 1u);
        bad.palette_format = PaletteFormat::Rgba8888;
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidExtent,
              "zero-byte floored CLUT4 row cannot form an encoded window");
    }
    {
        auto bad = spec(PixelFormat::Rgba8888, 2u, 2u);
        check(decode_pixels(bad, data, {}, 3u).error == PixelDecodeError::PixelBudgetExceeded,
              "caller pixel budget is enforced before allocation");
    }
    {
        auto bad = spec(PixelFormat::Clut8, 1u, 1u);
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidPaletteState,
              "indexed format requires an explicit palette format");
        bad.palette_format = static_cast<PaletteFormat>(255u);
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidPaletteState,
              "unknown palette format is rejected");
        bad.palette_format = PaletteFormat::Rgba8888;
        bad.palette_shift = 32u;
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidPaletteState,
              "palette shift above 31 is rejected");
        bad.palette_shift = 0u;
        bad.palette_mask = 256u;
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidPaletteState,
              "palette mask above 255 is rejected");
        bad.palette_mask = 255u;
        bad.palette_offset = 32u;
        check(decode_pixels(bad, data).error == PixelDecodeError::InvalidPaletteState,
              "palette offset above 31 is rejected");
    }
    {
        auto block = spec(PixelFormat::Dxt1, 1u, 1u);
        block.layout = PixelLayout::Swizzled16x8;
        block.stride_pixels = 65535u;
        const auto decoded = decode_pixels(block, std::array<std::uint8_t, 8>{});
        check(decoded.ok() && decoded.required_texel_bytes == 8u && !decoded.legacy_unaligned_bypass,
              "DXT ignores stride and swizzle mapping after explicit layout selection");
        const auto short_block = decode_pixels(block, std::span<const std::uint8_t>(data).first(7u));
        check(short_block.error == PixelDecodeError::MissingTexelBytes && short_block.rgba.empty() &&
                  short_block.required_texel_bytes == 8u,
              "short DXT block is rejected with its required byte count");
    }
}

} // namespace

int main() {
    direct_formats();
    indexed_formats();
    block_formats();
    swizzle_and_windows();
    rejected_inputs();
    std::cout << "pixel_decode_tests: passed\n";
}
