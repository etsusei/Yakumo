#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mhp3rd::resources {

// Numeric IDs describe the existing encoded resource formats. No guest
// addresses or GE state are part of this portable byte-window interface.
enum class PixelFormat : std::uint8_t {
    Rgba5650, Rgba5551, Rgba4444, Rgba8888, Clut4, Clut8, Clut16, Clut32,
    Dxt1, Dxt3, Dxt5,
};
enum class PaletteFormat : std::uint8_t { Rgba5650, Rgba5551, Rgba4444, Rgba8888, Unspecified };
enum class PixelLayout : std::uint8_t {
    Unspecified, Linear, Swizzled16x8,
    // Explicit current-software compatibility: an unaligned swizzle request
    // keeps linear bytes, matching the legacy decoder. Not a hardware claim.
    LegacySwizzleCompatibility,
};
struct PixelDecodeSpec {
    PixelFormat format{PixelFormat::Rgba8888};
    std::uint32_t width{}, height{};
    // Zero selects width, as in the existing decoder. Sampled width may exceed
    // this stride; reads then cross into the next row within the given window.
    std::uint32_t stride_pixels{};
    PixelLayout layout{PixelLayout::Unspecified};
    PaletteFormat palette_format{PaletteFormat::Unspecified};
    std::uint32_t palette_shift{}, palette_mask{}, palette_offset{};
};
enum class PixelDecodeError : std::uint8_t {
    None, InvalidExtent, InvalidFormat, LayoutUnspecified, InvalidSwizzleAlignment,
    InvalidPaletteState, PixelBudgetExceeded, MissingTexelBytes, MissingPaletteBytes,
};
struct PixelDecodeResult {
    PixelDecodeError error{PixelDecodeError::None};
    std::vector<std::uint32_t> rgba; // red in the low byte, alpha in the high byte
    std::size_t required_texel_bytes{};
    std::size_t required_palette_bytes{};
    bool legacy_unaligned_bypass{};
    [[nodiscard]] bool ok() const noexcept { return error == PixelDecodeError::None; }
};

// Inputs remain immutable. Failure returns no pixels, with an explicit reason;
// missing neighboring bytes are never synthesized. Allocation failures throw.
// On a missing-palette error, required_palette_bytes is a lower bound through
// the first missing mapped entry; later pixels have not been scanned.
// DXT blocks use the existing resource's index-before-endpoint byte order.
[[nodiscard]] PixelDecodeResult decode_pixels(const PixelDecodeSpec &spec,
    std::span<const std::uint8_t> texels, std::span<const std::uint8_t> palette = {},
    std::size_t max_pixels = 16u * 1024u * 1024u);

} // namespace mhp3rd::resources
