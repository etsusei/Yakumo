#include "gpu/texture_decode.hpp"
#include "perf/frame_stats.hpp"
#include "resources/pixel_decode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// This test links texture_decode.cpp without the performance subsystem. A
// non-alternating result selects its ordinary decoder route. A fresh process
// with MHP3RD_NO_FAST_TEXTURE_DECODE=1 also checks the legacy per-texel route.
namespace mhp3rd::perf {
bool alternate_off(NewPath) { return false; }
} // namespace mhp3rd::perf

namespace {
using mhp3rd::gpu::TextureFormat;
using mhp3rd::gpu::TextureSnapshot;
using mhp3rd::gpu::TextureState;
using mhp3rd::resources::PaletteFormat;
using mhp3rd::resources::PixelDecodeError;
using mhp3rd::resources::PixelDecodeResult;
using mhp3rd::resources::PixelDecodeSpec;
using mhp3rd::resources::PixelFormat;
using mhp3rd::resources::PixelLayout;

constexpr std::uint32_t kTexelAddress = 0x08010000u;
constexpr std::uint32_t kPaletteAddress = 0x08080000u;
constexpr std::size_t kPaletteEntries = 512u;

struct Case {
    PixelFormat format{};
    std::uint32_t width{}, height{}, stride{};
    PixelLayout layout{PixelLayout::Linear};
    std::uint32_t palette_format{}, shift{}, mask{}, offset{};
    bool legacy_dxt_swizzle{};
};

[[noreturn]] void fail(std::size_t number, const Case &test, const std::string &reason) {
    std::ostringstream message;
    message << "case " << number << " (format " << static_cast<unsigned>(test.format)
            << ", " << test.width << 'x' << test.height << ", stride " << test.stride
            << ", layout " << static_cast<unsigned>(test.layout) << ", palette "
            << test.palette_format << ", shift/mask/offset " << test.shift << '/'
            << test.mask << '/' << test.offset << "): " << reason;
    throw std::runtime_error(message.str());
}

void require(bool condition, std::size_t number, const Case &test, const std::string &reason) {
    if (!condition) fail(number, test, reason);
}

bool is_dxt(PixelFormat format) {
    return format == PixelFormat::Dxt1 || format == PixelFormat::Dxt3 || format == PixelFormat::Dxt5;
}

bool is_indexed(PixelFormat format) {
    return format == PixelFormat::Clut4 || format == PixelFormat::Clut8 ||
           format == PixelFormat::Clut16 || format == PixelFormat::Clut32;
}

std::uint32_t bits_per_texel(PixelFormat format) {
    switch (format) {
    case PixelFormat::Clut4: return 4u;
    case PixelFormat::Clut8: return 8u;
    case PixelFormat::Rgba5650:
    case PixelFormat::Rgba5551:
    case PixelFormat::Rgba4444:
    case PixelFormat::Clut16: return 16u;
    case PixelFormat::Rgba8888:
    case PixelFormat::Clut32: return 32u;
    default: return 0u;
    }
}

std::uint32_t row_bytes(const Case &test) {
    const std::uint32_t stride = test.stride != 0u ? test.stride : test.width;
    return stride * bits_per_texel(test.format) / 8u;
}

std::size_t texel_bytes(const Case &test) {
    if (is_dxt(test.format)) {
        const std::size_t block_size = test.format == PixelFormat::Dxt1 ? 8u : 16u;
        return static_cast<std::size_t>((test.width + 3u) / 4u) * ((test.height + 3u) / 4u) * block_size;
    }
    return static_cast<std::size_t>(row_bytes(test)) * test.height;
}

std::size_t palette_entry_bytes(const Case &test) {
    return test.palette_format == 3u ? 4u : 2u;
}

void set16(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<std::uint8_t>(value);
    bytes[at + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void fill_dxt_blocks(const Case &test, std::vector<std::uint8_t> &bytes, std::mt19937 &random) {
    const std::size_t block_size = test.format == PixelFormat::Dxt1 ? 8u : 16u;
    for (std::size_t at = 0u, block = 0u; at < bytes.size(); at += block_size, ++block) {
        // Selectors precede RGB565 endpoints in these PSP blocks. Alternating
        // endpoint order exercises DXT1's transparent and four-color modes.
        for (std::size_t row = 0u; row < 4u; ++row)
            bytes[at + row] = static_cast<std::uint8_t>((0xE4u << (row % 2u)) | (0xE4u >> (8u - row % 2u)));
        const bool reversed = (block & 1u) != 0u;
        set16(bytes, at + 4u, reversed ? 0x001Fu : 0xF800u);
        set16(bytes, at + 6u, reversed ? 0xF800u : 0x001Fu);
        if (test.format == PixelFormat::Dxt3) {
            for (std::size_t i = 8u; i < 16u; ++i)
                bytes[at + i] = static_cast<std::uint8_t>(((i - 8u) << 4u) | (15u - (i - 8u)));
        } else if (test.format == PixelFormat::Dxt5) {
            for (std::size_t i = 8u; i < 14u; ++i) bytes[at + i] = static_cast<std::uint8_t>(random());
            bytes[at + 14u] = reversed ? 18u : 220u;
            bytes[at + 15u] = reversed ? 220u : 18u;
        }
    }
}

std::vector<std::uint8_t> make_texels(const Case &test, std::mt19937 &random) {
    std::vector<std::uint8_t> bytes(texel_bytes(test));
    if (is_dxt(test.format)) {
        fill_dxt_blocks(test, bytes, random);
    } else {
        for (auto &byte : bytes) byte = static_cast<std::uint8_t>(random());
        // Stable boundary values expose component and alpha expansion errors.
        if (bytes.size() >= 4u) {
            bytes[0] = 0u;
            bytes[1] = 0xFFu;
            bytes[2] = 0x80u;
            bytes[3] = 0x7Fu;
        }
    }
    return bytes;
}

std::vector<std::uint8_t> make_palette(const Case &test, std::mt19937 &random) {
    if (!is_indexed(test.format)) return {};
    std::vector<std::uint8_t> palette(kPaletteEntries * palette_entry_bytes(test));
    for (auto &byte : palette) byte = static_cast<std::uint8_t>(random());
    return palette;
}

// The expected source view for palette-index accounting and the snapshot's
// documented in-place mutation. This fixture transformation is independent
// of the portable decoder and never modifies the supplied texel vector.
std::vector<std::uint8_t> expected_linear_bytes(const Case &test, std::span<const std::uint8_t> source) {
    std::vector<std::uint8_t> linear(source.begin(), source.end());
    const std::uint32_t pitch = row_bytes(test);
    if (test.layout == PixelLayout::Linear || pitch % 16u != 0u || test.height % 8u != 0u) return linear;
    const std::uint32_t columns = pitch / 16u;
    std::size_t from = 0u;
    for (std::uint32_t by = 0u; by < test.height / 8u; ++by) {
        for (std::uint32_t bx = 0u; bx < columns; ++bx) {
            for (std::uint32_t row = 0u; row < 8u; ++row) {
                const std::size_t to = static_cast<std::size_t>(by * 8u + row) * pitch + bx * 16u;
                std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(from), 16u,
                            linear.begin() + static_cast<std::ptrdiff_t>(to));
                from += 16u;
            }
        }
    }
    return linear;
}

std::optional<std::uint32_t> sample_index(const Case &test, std::span<const std::uint8_t> linear,
                                          std::uint32_t x, std::uint32_t y) {
    const std::size_t row = static_cast<std::size_t>(y) * row_bytes(test);
    const std::size_t at = row + (test.format == PixelFormat::Clut4 ? x / 2u :
                                  test.format == PixelFormat::Clut8 ? x :
                                  test.format == PixelFormat::Clut16 ? static_cast<std::size_t>(x) * 2u :
                                                                         static_cast<std::size_t>(x) * 4u);
    const std::size_t bytes = bits_per_texel(test.format) <= 8u ? 1u : bits_per_texel(test.format) / 8u;
    if (at + bytes > linear.size()) return std::nullopt; // legacy opaque-black pixel
    if (test.format == PixelFormat::Clut4) return (x & 1u) != 0u ? linear[at] >> 4u : linear[at] & 15u;
    std::uint32_t index = 0u;
    for (std::size_t i = 0u; i < bytes; ++i) index |= static_cast<std::uint32_t>(linear[at + i]) << (8u * i);
    return index;
}

std::size_t expected_palette_bytes(const Case &test, std::span<const std::uint8_t> linear) {
    if (!is_indexed(test.format)) return 0u;
    std::size_t highest = 0u;
    bool touched = false;
    for (std::uint32_t y = 0u; y < test.height; ++y) {
        for (std::uint32_t x = 0u; x < test.width; ++x) {
            const auto index = sample_index(test, linear, x, y);
            if (!index) continue;
            const std::uint32_t entry = (((*index) >> test.shift) & test.mask) | (test.offset << 4u);
            highest = std::max(highest, static_cast<std::size_t>(entry));
            touched = true;
        }
    }
    return touched ? (highest + 1u) * palette_entry_bytes(test) : 0u;
}

std::string pixel_mismatch(std::span<const std::uint32_t> expected,
                           std::span<const std::uint32_t> actual, std::uint32_t width) {
    std::ostringstream message;
    if (expected.size() != actual.size()) {
        message << "pixel count " << actual.size() << ", expected " << expected.size();
        return message.str();
    }
    for (std::size_t i = 0u; i < expected.size(); ++i) {
        if (expected[i] == actual[i]) continue;
        message << "first pixel difference at (" << i % width << ',' << i / width << ") [" << i
                << "]: legacy 0x" << std::hex << std::setw(8) << std::setfill('0') << expected[i]
                << ", core 0x" << std::setw(8) << actual[i];
        return message.str();
    }
    return {};
}

class Oracle {
public:
    void run(const Case &test) {
        const std::size_t number = ++cases_;
        require(test.width > 0u && test.height > 0u && test.width <= 1024u && test.height <= 1024u,
                number, test, "fixture extent outside legacy decoder limit");
        require(is_dxt(test.format) || row_bytes(test) > 0u, number, test, "empty fixture row");
        std::vector<std::uint8_t> texels = make_texels(test, random_);
        std::vector<std::uint8_t> palette = make_palette(test, random_);
        const std::vector<std::uint8_t> original_texels = texels;
        const std::vector<std::uint8_t> original_palette = palette;

        PixelDecodeSpec spec{};
        spec.format = test.format;
        spec.width = test.width;
        spec.height = test.height;
        spec.stride_pixels = test.stride;
        spec.layout = test.layout;
        spec.palette_format = is_indexed(test.format) ? static_cast<PaletteFormat>(test.palette_format) :
                                                        PaletteFormat::Unspecified;
        spec.palette_shift = test.shift;
        spec.palette_mask = test.mask;
        spec.palette_offset = test.offset;
        const PixelDecodeResult core = mhp3rd::resources::decode_pixels(spec, texels, palette);
        require(core.ok(), number, test, "core rejected valid fixture, error " + std::to_string(static_cast<unsigned>(core.error)));
        require(core.required_texel_bytes == texels.size(), number, test, "core texel window differs");
        const bool bypass = !is_dxt(test.format) && test.layout == PixelLayout::LegacySwizzleCompatibility &&
                            (row_bytes(test) % 16u != 0u || test.height % 8u != 0u);
        require(core.legacy_unaligned_bypass == bypass, number, test, "legacy swizzle bypass flag differs");

        const auto linear = is_dxt(test.format) ? std::vector<std::uint8_t>{} : expected_linear_bytes(test, texels);
        const std::size_t palette_needed = expected_palette_bytes(test, linear);
        require(core.required_palette_bytes == palette_needed, number, test,
                "actual-index palette window differs: core " + std::to_string(core.required_palette_bytes) +
                ", expected " + std::to_string(palette_needed));
        if (is_indexed(test.format)) {
            require(palette_needed > 0u && palette_needed <= palette.size(), number, test,
                    "fixture palette does not cover mapped indices");
            const auto minimal_palette = std::span<const std::uint8_t>(palette).first(palette_needed);
            const auto minimal = mhp3rd::resources::decode_pixels(spec, texels, minimal_palette);
            require(minimal.ok() && minimal.rgba == core.rgba, number, test,
                    "core failed with exact actual-index palette window");
            const auto missing = mhp3rd::resources::decode_pixels(spec, texels, minimal_palette.first(palette_needed - 1u));
            require(missing.error == PixelDecodeError::MissingPaletteBytes && missing.rgba.empty(), number, test,
                    "core accepted a missing mapped palette byte or returned partial pixels");
        }
        const auto short_texels = mhp3rd::resources::decode_pixels(
            spec, std::span<const std::uint8_t>(texels).first(texels.size() - 1u), palette);
        require(short_texels.error == PixelDecodeError::MissingTexelBytes && short_texels.rgba.empty(), number, test,
                "core accepted a short encoded window or returned partial pixels");

        TextureState state{};
        state.enabled = true;
        state.address = kTexelAddress;
        state.width = static_cast<std::uint16_t>(test.width);
        state.height = static_cast<std::uint16_t>(test.height);
        state.buffer_width = test.stride;
        state.format = static_cast<TextureFormat>(test.format);
        state.swizzled = is_dxt(test.format) ? test.legacy_dxt_swizzle : test.layout != PixelLayout::Linear;
        state.clut_address = kPaletteAddress;
        state.clut_format = test.palette_format;
        state.clut_shift = test.shift;
        state.clut_mask = test.mask;
        state.clut_offset = test.offset;
        memory_.copy_in(kTexelAddress, texels);
        if (!palette.empty()) memory_.copy_in(kPaletteAddress, palette);
        std::vector<std::uint32_t> legacy;
        require(mhp3rd::gpu::decode_texture(memory_, state, legacy), number, test, "legacy decoder rejected fixture");
        const std::string mismatch = pixel_mismatch(legacy, core.rgba, test.width);
        require(mismatch.empty(), number, test, mismatch);

        if (!is_dxt(test.format)) {
            TextureSnapshot snapshot;
            require(mhp3rd::gpu::snapshot_texture(memory_, state, snapshot), number, test,
                    "legacy snapshot rejected contiguous fixture");
            require(snapshot.texels == texels, number, test, "snapshot source copy differs");
            require(snapshot.clut == palette, number, test, "snapshot palette copy differs");
            std::vector<std::uint32_t> copied;
            require(mhp3rd::gpu::decode_snapshot(snapshot, copied), number, test, "snapshot decode rejected fixture");
            const std::string snapshot_mismatch = pixel_mismatch(legacy, copied, test.width);
            require(snapshot_mismatch.empty(), number, test, "snapshot: " + snapshot_mismatch);
            require(snapshot.texels == linear, number, test, "snapshot's in-place texel view differs");
            ++snapshot_cases_;
        }

        std::vector<std::uint8_t> after(texels.size());
        memory_.copy_out(kTexelAddress, after);
        require(texels == original_texels && after == texels, number, test, "source texels changed");
        if (!palette.empty()) {
            after.resize(palette.size());
            memory_.copy_out(kPaletteAddress, after);
            require(palette == original_palette && after == palette, number, test, "source palette changed");
        }
        ++by_format_[static_cast<unsigned>(test.format)];
        if (bypass) ++compatibility_bypasses_;
        if (test.width > test.stride && test.stride != 0u && !is_dxt(test.format)) ++cross_row_cases_;
    }

    void check_rejections() {
        Case test{PixelFormat::Clut32, 4u, 1u, 4u, PixelLayout::Linear, 3u, 20u, 0x7Fu, 3u};
        std::vector<std::uint8_t> texels(16u, 0u);
        // A high-bit 32-bit index maps to entry ((0xA4204567 >> 20) & 0x7f) | 0x30 = 0x72.
        texels[0] = 0x67u; texels[1] = 0x45u; texels[2] = 0x20u; texels[3] = 0xA4u;
        PixelDecodeSpec spec{};
        spec.format = test.format; spec.width = test.width; spec.height = test.height;
        spec.stride_pixels = test.stride; spec.layout = test.layout;
        spec.palette_format = PaletteFormat::Rgba8888;
        spec.palette_shift = test.shift; spec.palette_mask = test.mask; spec.palette_offset = test.offset;
        std::vector<std::uint8_t> palette(kPaletteEntries * 4u, 0u);
        const auto result = mhp3rd::resources::decode_pixels(spec, texels, palette);
        require(result.ok() && result.required_palette_bytes == (0x72u + 1u) * 4u,
                cases_ + 1u, test, "32-bit shifted index did not name expected palette entry");
        const auto short_palette = mhp3rd::resources::decode_pixels(
            spec, texels, std::span<const std::uint8_t>(palette).first(result.required_palette_bytes - 1u));
        require(short_palette.error == PixelDecodeError::MissingPaletteBytes && short_palette.rgba.empty(),
                cases_ + 1u, test, "32-bit shifted index accepted missing palette entry");
        ++rejections_;

        spec.layout = PixelLayout::Unspecified;
        const auto unspecified = mhp3rd::resources::decode_pixels(spec, texels, palette);
        require(unspecified.error == PixelDecodeError::LayoutUnspecified && unspecified.rgba.empty(),
                cases_ + 1u, test, "unspecified layout accepted");
        ++rejections_;

        test = Case{PixelFormat::Clut4, 19u, 9u, 21u, PixelLayout::Swizzled16x8, 1u, 0u, 0xFFu, 0u};
        spec = PixelDecodeSpec{};
        spec.format = test.format; spec.width = test.width; spec.height = test.height;
        spec.stride_pixels = test.stride; spec.layout = test.layout;
        spec.palette_format = PaletteFormat::Rgba5551;
        spec.palette_mask = test.mask;
        const std::vector<std::uint8_t> unaligned(texel_bytes(test), 0u);
        const auto strict = mhp3rd::resources::decode_pixels(spec, unaligned, palette);
        require(strict.error == PixelDecodeError::InvalidSwizzleAlignment && strict.rgba.empty(),
                cases_ + 1u, test, "strict swizzle accepted unaligned row or height");
        ++rejections_;
    }

    void report() const {
        std::cout << "pixel legacy oracle: " << cases_ << " cases, " << snapshot_cases_ << " snapshots, "
                  << compatibility_bypasses_ << " unaligned compatibility bypasses, " << cross_row_cases_
                  << " cross-row cases, " << rejections_ << " explicit rejections; per format:";
        for (unsigned i = 0u; i < by_format_.size(); ++i) std::cout << ' ' << i << '=' << by_format_[i];
        std::cout << " (" << (std::getenv("MHP3RD_NO_FAST_TEXTURE_DECODE") ? "legacy slow" : "legacy direct")
                  << ")\n";
    }

private:
    psprecomp::GuestMemory memory_{};
    std::mt19937 random_{0x50495836u};
    std::array<std::size_t, 11u> by_format_{};
    std::size_t cases_{}, snapshot_cases_{}, compatibility_bypasses_{}, cross_row_cases_{}, rejections_{};
};

std::uint32_t aligned_stride(PixelFormat format) {
    return 128u / bits_per_texel(format); // sixteen encoded bytes per row
}

void run_cases(Oracle &oracle) {
    std::mt19937 dimensions{0x4C41594Fu};
    for (unsigned id = 0u; id <= 10u; ++id) {
        const auto format = static_cast<PixelFormat>(id);
        if (is_dxt(format)) {
            const std::array<std::array<std::uint32_t, 2>, 8> sizes{{
                {{1u, 1u}}, {{3u, 5u}}, {{4u, 4u}}, {{7u, 9u}},
                {{17u, 13u}}, {{128u, 128u}}, {{1024u, 8u}}, {{8u, 1024u}},
            }};
            for (std::size_t i = 0u; i < sizes.size(); ++i)
                oracle.run(Case{format, sizes[i][0], sizes[i][1], static_cast<std::uint32_t>(3u + i * 7u),
                                PixelLayout::Linear, 0u, 0u, 0u, 0u, (i & 1u) != 0u});
            for (std::uint32_t trial = 0u; trial < 4u; ++trial)
                oracle.run(Case{format, 2u + dimensions() % 127u, 1u + dimensions() % 128u,
                                2u + dimensions() % 127u, PixelLayout::Linear, 0u, 0u, 0u, 0u,
                                (trial & 1u) != 0u});
            continue;
        }
        const auto indexed = is_indexed(format);
        for (std::uint32_t variant = 0u; variant < (indexed ? 4u : 1u); ++variant) {
            const std::uint32_t shift = format == PixelFormat::Clut32 ?
                std::array<std::uint32_t, 4>{0u, 5u, 16u, 27u}[variant] :
                std::array<std::uint32_t, 4>{0u, 1u, 3u, 7u}[variant];
            const std::uint32_t mask = std::array<std::uint32_t, 4>{0xFFu, 0x7Fu, 0x1Fu, 0x0Fu}[variant];
            const std::uint32_t offset = std::array<std::uint32_t, 4>{0u, 1u, 7u, 31u}[variant];
            oracle.run(Case{format, 8u, 8u, 8u, PixelLayout::Linear, variant, shift, mask, offset});
            oracle.run(Case{format, 17u, 9u, 23u, PixelLayout::Linear, variant, shift, mask, offset});
            oracle.run(Case{format, 23u, 9u, 17u, PixelLayout::Linear, variant, shift, mask, offset});
            oracle.run(Case{format, aligned_stride(format) + 5u, 16u, aligned_stride(format),
                            PixelLayout::Swizzled16x8, variant, shift, mask, offset});
            oracle.run(Case{format, aligned_stride(format) + 3u, 8u, aligned_stride(format),
                            PixelLayout::LegacySwizzleCompatibility, variant, shift, mask, offset});
            oracle.run(Case{format, 19u, 9u, 21u, PixelLayout::LegacySwizzleCompatibility,
                            variant, shift, mask, offset});
        }
        // 1024 is the legacy decoder's axis cap. These are small byte windows,
        // including a last-row overrun into its opaque-black fallback.
        oracle.run(Case{format, 1024u, 8u, 1017u, PixelLayout::Linear, 3u, 3u, 0x3Fu, 1u});
        oracle.run(Case{format, 8u, 1024u, 8u, PixelLayout::Linear, 2u, 1u, 0x7Fu, 2u});
        oracle.run(Case{format, 9u, 7u, 0u, PixelLayout::Linear, 1u, 1u, 0x7Fu, 1u});
        for (std::uint32_t trial = 0u; trial < 4u; ++trial) {
            const std::uint32_t width = 2u + dimensions() % 127u;
            const std::uint32_t height = 1u + dimensions() % 128u;
            const std::uint32_t stride = 2u + dimensions() % 127u;
            oracle.run(Case{format, width, height, stride,
                            (trial & 1u) != 0u ? PixelLayout::LegacySwizzleCompatibility : PixelLayout::Linear,
                            trial, format == PixelFormat::Clut32 ? 4u * trial : trial,
                            std::array<std::uint32_t, 4>{0xFFu, 0x7Fu, 0x3Fu, 0x0Fu}[trial], trial});
        }
    }
}
} // namespace

int main() {
    try {
        static_assert(static_cast<unsigned>(PixelFormat::Rgba5650) == static_cast<unsigned>(TextureFormat::Rgba5650));
        static_assert(static_cast<unsigned>(PixelFormat::Rgba5551) == static_cast<unsigned>(TextureFormat::Rgba5551));
        static_assert(static_cast<unsigned>(PixelFormat::Rgba4444) == static_cast<unsigned>(TextureFormat::Rgba4444));
        static_assert(static_cast<unsigned>(PixelFormat::Rgba8888) == static_cast<unsigned>(TextureFormat::Rgba8888));
        static_assert(static_cast<unsigned>(PixelFormat::Clut4) == static_cast<unsigned>(TextureFormat::Clut4));
        static_assert(static_cast<unsigned>(PixelFormat::Clut8) == static_cast<unsigned>(TextureFormat::Clut8));
        static_assert(static_cast<unsigned>(PixelFormat::Clut16) == static_cast<unsigned>(TextureFormat::Clut16));
        static_assert(static_cast<unsigned>(PixelFormat::Clut32) == static_cast<unsigned>(TextureFormat::Clut32));
        static_assert(static_cast<unsigned>(PixelFormat::Dxt1) == static_cast<unsigned>(TextureFormat::Dxt1));
        static_assert(static_cast<unsigned>(PixelFormat::Dxt3) == static_cast<unsigned>(TextureFormat::Dxt3));
        static_assert(static_cast<unsigned>(PixelFormat::Dxt5) == static_cast<unsigned>(TextureFormat::Dxt5));
        Oracle oracle;
        run_cases(oracle);
        oracle.check_rejections();
        oracle.report();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "pixel legacy oracle failed: " << error.what() << '\n';
        return 1;
    }
}
