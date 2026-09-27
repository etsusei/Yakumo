#include "pixel_decode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mhp3rd::resources {
namespace {

constexpr std::uint32_t kOpaqueBlack = 0xFF000000u;

std::uint16_t load16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[at + 1u]) << 8u);
}

std::uint32_t load32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[at + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[at + 3u]) << 24u);
}

bool contains(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t count) {
    return at <= bytes.size() && count <= bytes.size() - at;
}

std::uint32_t expand_5650(std::uint16_t value) {
    const std::uint32_t r = (value & 31u) * 255u / 31u;
    const std::uint32_t g = ((value >> 5u) & 63u) * 255u / 63u;
    const std::uint32_t b = ((value >> 11u) & 31u) * 255u / 31u;
    return kOpaqueBlack | (b << 16u) | (g << 8u) | r;
}

std::uint32_t expand_5551(std::uint16_t value) {
    const std::uint32_t r = (value & 31u) * 255u / 31u;
    const std::uint32_t g = ((value >> 5u) & 31u) * 255u / 31u;
    const std::uint32_t b = ((value >> 10u) & 31u) * 255u / 31u;
    const std::uint32_t a = ((value >> 15u) & 1u) * 255u;
    return (a << 24u) | (b << 16u) | (g << 8u) | r;
}

std::uint32_t expand_4444(std::uint16_t value) {
    const std::uint32_t r = (value & 15u) * 17u;
    const std::uint32_t g = ((value >> 4u) & 15u) * 17u;
    const std::uint32_t b = ((value >> 8u) & 15u) * 17u;
    const std::uint32_t a = ((value >> 12u) & 15u) * 17u;
    return (a << 24u) | (b << 16u) | (g << 8u) | r;
}

// The block formats reverse the red/blue positions in their RGB565 endpoints.
std::uint32_t expand_dxt_565(std::uint16_t value) {
    const std::uint32_t r = ((value >> 11u) & 31u) * 255u / 31u;
    const std::uint32_t g = ((value >> 5u) & 63u) * 255u / 63u;
    const std::uint32_t b = (value & 31u) * 255u / 31u;
    return (b << 16u) | (g << 8u) | r;
}

std::uint32_t interpolate(std::uint32_t a, std::uint32_t b, std::uint32_t numerator,
                          std::uint32_t denominator) {
    std::uint32_t result = 0u;
    for (std::uint32_t shift = 0u; shift < 24u; shift += 8u) {
        const std::uint32_t left = (a >> shift) & 255u;
        const std::uint32_t right = (b >> shift) & 255u;
        result |= ((left * (denominator - numerator) + right * numerator) / denominator) << shift;
    }
    return result;
}

void decode_dxt_block(std::span<const std::uint8_t> block, PixelFormat format,
                      std::array<std::uint32_t, 16> &output) {
    const std::uint16_t endpoint0 = load16(block, 4u);
    const std::uint16_t endpoint1 = load16(block, 6u);
    std::array<std::uint32_t, 4> colors{};
    colors[0] = expand_dxt_565(endpoint0);
    colors[1] = expand_dxt_565(endpoint1);
    const bool one_bit_alpha = format == PixelFormat::Dxt1 && endpoint0 <= endpoint1;
    colors[2] = interpolate(colors[0], colors[1], 1u, one_bit_alpha ? 2u : 3u);
    colors[3] = one_bit_alpha ? 0u : interpolate(colors[0], colors[1], 2u, 3u);

    std::uint64_t alpha_codes = 0u;
    if (format == PixelFormat::Dxt5) {
        for (std::uint32_t i = 0u; i < 6u; ++i)
            alpha_codes |= static_cast<std::uint64_t>(block[8u + i]) << (8u * i);
    }
    for (std::uint32_t row = 0u; row < 4u; ++row) {
        const std::uint32_t selectors = block[row];
        for (std::uint32_t column = 0u; column < 4u; ++column) {
            const std::uint32_t pixel = row * 4u + column;
            const std::uint32_t selector = (selectors >> (2u * column)) & 3u;
            std::uint32_t alpha = 255u;
            if (format == PixelFormat::Dxt1) {
                if (one_bit_alpha && selector == 3u) alpha = 0u;
            } else if (format == PixelFormat::Dxt3) {
                const std::uint32_t pair = block[8u + pixel / 2u];
                alpha = ((pixel & 1u) != 0u ? pair >> 4u : pair & 15u) * 17u;
            } else {
                const std::uint32_t code = (alpha_codes >> (3u * pixel)) & 7u;
                const std::uint32_t alpha0 = block[14u];
                const std::uint32_t alpha1 = block[15u];
                if (code == 0u) alpha = alpha0;
                else if (code == 1u) alpha = alpha1;
                else if (alpha0 > alpha1) alpha = ((8u - code) * alpha0 + (code - 1u) * alpha1) / 7u;
                else if (code < 6u) alpha = ((6u - code) * alpha0 + (code - 1u) * alpha1) / 5u;
                else alpha = code == 6u ? 0u : 255u;
            }
            output[pixel] = (colors[selector] & 0x00FFFFFFu) | (alpha << 24u);
        }
    }
}

std::uint32_t format_bits(PixelFormat format) {
    switch (format) {
    case PixelFormat::Clut4: return 4u;
    case PixelFormat::Clut8: return 8u;
    case PixelFormat::Rgba5650:
    case PixelFormat::Rgba5551:
    case PixelFormat::Rgba4444:
    case PixelFormat::Clut16: return 16u;
    case PixelFormat::Rgba8888:
    case PixelFormat::Clut32: return 32u;
    case PixelFormat::Dxt1:
    case PixelFormat::Dxt3:
    case PixelFormat::Dxt5: return 0u;
    }
    return 0u;
}

bool is_dxt(PixelFormat format) {
    return format == PixelFormat::Dxt1 || format == PixelFormat::Dxt3 || format == PixelFormat::Dxt5;
}

bool is_indexed(PixelFormat format) {
    return format == PixelFormat::Clut4 || format == PixelFormat::Clut8 ||
           format == PixelFormat::Clut16 || format == PixelFormat::Clut32;
}

std::size_t palette_entry_bytes(PaletteFormat format) {
    switch (format) {
    case PaletteFormat::Rgba5650:
    case PaletteFormat::Rgba5551:
    case PaletteFormat::Rgba4444: return 2u;
    case PaletteFormat::Rgba8888: return 4u;
    case PaletteFormat::Unspecified: return 0u;
    }
    return 0u;
}

bool known_palette_format(PaletteFormat format) {
    return format == PaletteFormat::Unspecified || palette_entry_bytes(format) != 0u;
}

std::uint32_t palette_color(std::span<const std::uint8_t> palette, std::size_t at,
                            PaletteFormat format) {
    switch (format) {
    case PaletteFormat::Rgba5650: return expand_5650(load16(palette, at));
    case PaletteFormat::Rgba5551: return expand_5551(load16(palette, at));
    case PaletteFormat::Rgba4444: return expand_4444(load16(palette, at));
    case PaletteFormat::Rgba8888: return load32(palette, at);
    case PaletteFormat::Unspecified: break;
    }
    return kOpaqueBlack; // Validated before decoding.
}

std::vector<std::uint8_t> unswizzle(std::span<const std::uint8_t> encoded,
                                    std::size_t row_bytes, std::uint32_t height) {
    std::vector<std::uint8_t> linear(encoded.size());
    const std::size_t block_columns = row_bytes / 16u;
    for (std::size_t block_y = 0u; block_y < height / 8u; ++block_y) {
        for (std::size_t block_x = 0u; block_x < block_columns; ++block_x) {
            for (std::size_t within_y = 0u; within_y < 8u; ++within_y) {
                const std::size_t source = ((block_y * block_columns + block_x) * 8u + within_y) * 16u;
                const std::size_t destination = (block_y * 8u + within_y) * row_bytes + block_x * 16u;
                std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(source), 16u,
                            linear.begin() + static_cast<std::ptrdiff_t>(destination));
            }
        }
    }
    return linear;
}

} // namespace

PixelDecodeResult decode_pixels(const PixelDecodeSpec &spec, std::span<const std::uint8_t> texels,
                                std::span<const std::uint8_t> palette, std::size_t max_pixels) {
    PixelDecodeResult result{};
    const auto fail = [&result](PixelDecodeError error) {
        result.error = error;
        result.rgba.clear();
        return result;
    };

    const bool compressed = is_dxt(spec.format);
    const std::uint32_t bits = format_bits(spec.format);
    if (bits == 0u && !compressed) return fail(PixelDecodeError::InvalidFormat);
    switch (spec.layout) {
    case PixelLayout::Linear:
    case PixelLayout::Swizzled16x8:
    case PixelLayout::LegacySwizzleCompatibility: break;
    case PixelLayout::Unspecified:
    default: return fail(PixelDecodeError::LayoutUnspecified);
    }
    if (spec.width == 0u || spec.height == 0u || spec.width > 4096u || spec.height > 4096u ||
        spec.stride_pixels > 65535u)
        return fail(PixelDecodeError::InvalidExtent);
    if (spec.palette_shift > 31u || spec.palette_mask > 255u || spec.palette_offset > 31u ||
        !known_palette_format(spec.palette_format) ||
        (is_indexed(spec.format) && spec.palette_format == PaletteFormat::Unspecified))
        return fail(PixelDecodeError::InvalidPaletteState);

    const std::size_t pixel_count = static_cast<std::size_t>(spec.width) * spec.height;
    if (pixel_count > max_pixels) return fail(PixelDecodeError::PixelBudgetExceeded);

    std::size_t row_bytes = 0u;
    bool unaligned_compatibility = false;
    if (compressed) {
        const std::size_t blocks_x = (static_cast<std::size_t>(spec.width) + 3u) / 4u;
        const std::size_t blocks_y = (static_cast<std::size_t>(spec.height) + 3u) / 4u;
        result.required_texel_bytes = blocks_x * blocks_y * (spec.format == PixelFormat::Dxt1 ? 8u : 16u);
    } else {
        const std::size_t stride = spec.stride_pixels == 0u ? spec.width : spec.stride_pixels;
        row_bytes = stride * bits / 8u;
        if (row_bytes == 0u) return fail(PixelDecodeError::InvalidExtent);
        result.required_texel_bytes = row_bytes * spec.height;
        if (spec.layout != PixelLayout::Linear && (row_bytes % 16u != 0u || spec.height % 8u != 0u)) {
            if (spec.layout == PixelLayout::Swizzled16x8)
                return fail(PixelDecodeError::InvalidSwizzleAlignment);
            unaligned_compatibility = true;
        }
    }
    if (texels.size() < result.required_texel_bytes) return fail(PixelDecodeError::MissingTexelBytes);
    result.legacy_unaligned_bypass = unaligned_compatibility;
    const auto window = texels.first(result.required_texel_bytes);
    std::vector<std::uint8_t> linear_storage;
    std::span<const std::uint8_t> readable = window;
    if (!compressed && spec.layout != PixelLayout::Linear && !result.legacy_unaligned_bypass) {
        linear_storage = unswizzle(window, row_bytes, spec.height);
        readable = linear_storage;
    }

    result.rgba.assign(pixel_count, kOpaqueBlack);
    if (compressed) {
        const std::size_t blocks_x = (static_cast<std::size_t>(spec.width) + 3u) / 4u;
        const std::size_t blocks_y = (static_cast<std::size_t>(spec.height) + 3u) / 4u;
        const std::size_t block_bytes = spec.format == PixelFormat::Dxt1 ? 8u : 16u;
        std::array<std::uint32_t, 16> decoded{};
        for (std::size_t by = 0u; by < blocks_y; ++by) {
            for (std::size_t bx = 0u; bx < blocks_x; ++bx) {
                const std::size_t at = (by * blocks_x + bx) * block_bytes;
                decode_dxt_block(readable.subspan(at, block_bytes), spec.format, decoded);
                for (std::size_t row = 0u; row < 4u; ++row) {
                    const std::size_t y = by * 4u + row;
                    if (y >= spec.height) break;
                    for (std::size_t column = 0u; column < 4u; ++column) {
                        const std::size_t x = bx * 4u + column;
                        if (x < spec.width) result.rgba[y * spec.width + x] = decoded[row * 4u + column];
                    }
                }
            }
        }
        return result;
    }

    const std::size_t entry_bytes = palette_entry_bytes(spec.palette_format);
    for (std::size_t y = 0u; y < spec.height; ++y) {
        const std::size_t row = y * row_bytes;
        for (std::size_t x = 0u; x < spec.width; ++x) {
            std::size_t at = row;
            std::size_t needed = 0u;
            switch (spec.format) {
            case PixelFormat::Clut4: at += x / 2u; needed = 1u; break;
            case PixelFormat::Clut8: at += x; needed = 1u; break;
            case PixelFormat::Rgba5650:
            case PixelFormat::Rgba5551:
            case PixelFormat::Rgba4444:
            case PixelFormat::Clut16: at += x * 2u; needed = 2u; break;
            case PixelFormat::Rgba8888:
            case PixelFormat::Clut32: at += x * 4u; needed = 4u; break;
            case PixelFormat::Dxt1:
            case PixelFormat::Dxt3:
            case PixelFormat::Dxt5: break; // Handled above.
            }
            if (!contains(readable, at, needed)) continue;

            std::uint32_t value = 0u;
            if (spec.format == PixelFormat::Clut4)
                value = ((x & 1u) != 0u ? readable[at] >> 4u : readable[at] & 15u);
            else if (needed == 1u) value = readable[at];
            else if (needed == 2u) value = load16(readable, at);
            else value = load32(readable, at);

            if (is_indexed(spec.format)) {
                const std::uint32_t entry = ((value >> spec.palette_shift) & spec.palette_mask) |
                                            (spec.palette_offset << 4u);
                const std::size_t palette_at = static_cast<std::size_t>(entry) * entry_bytes;
                // On failure this is only a lower bound: later texels may name
                // entries beyond the first one missing from the supplied span.
                result.required_palette_bytes = std::max(result.required_palette_bytes, palette_at + entry_bytes);
                if (!contains(palette, palette_at, entry_bytes))
                    return fail(PixelDecodeError::MissingPaletteBytes);
                result.rgba[y * spec.width + x] = palette_color(palette, palette_at, spec.palette_format);
            } else {
                switch (spec.format) {
                case PixelFormat::Rgba5650: result.rgba[y * spec.width + x] = expand_5650(static_cast<std::uint16_t>(value)); break;
                case PixelFormat::Rgba5551: result.rgba[y * spec.width + x] = expand_5551(static_cast<std::uint16_t>(value)); break;
                case PixelFormat::Rgba4444: result.rgba[y * spec.width + x] = expand_4444(static_cast<std::uint16_t>(value)); break;
                case PixelFormat::Rgba8888: result.rgba[y * spec.width + x] = value; break;
                default: break; // Validated above.
                }
            }
        }
    }
    return result;
}

} // namespace mhp3rd::resources
