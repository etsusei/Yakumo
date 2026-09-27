#include "resources/texture_commands.hpp"

#include <bit>
#include <cstdint>
#include <limits>

namespace mhp3rd::resources {
namespace {

constexpr bool has_no_palette(std::uint32_t format) noexcept {
    return format == 1u || format == 3u || format == 8u ||
           format == 9u || format == 10u;
}

constexpr std::uint32_t raster_exponent(std::uint32_t dimension) noexcept {
    // The certified 1..1024 table is ceil(log2(n)); n == 1 yields zero.
    return std::bit_width(dimension - 1u);
}

TextureCommandError validate_descriptor(const TextureCommandDescriptor &descriptor) noexcept {
    if (descriptor.format > 10u) return TextureCommandError::InvalidFormat;
    if (descriptor.width == 0u || descriptor.width > 1024u ||
        descriptor.height == 0u || descriptor.height > 1024u) {
        return TextureCommandError::InvalidDimensions;
    }
    if (has_no_palette(descriptor.format)) {
        if (descriptor.palette_token != 0u || descriptor.palette_format != 0u ||
            descriptor.palette_count != 0) {
            return TextureCommandError::InvalidPalette;
        }
    } else if (descriptor.palette_format > 3u || descriptor.palette_count < 0 ||
               descriptor.palette_count > 32767) {
        return TextureCommandError::InvalidPalette;
    }
    return TextureCommandError::None;
}

std::array<std::uint32_t, 9> command_words(
    const TextureCommandDescriptor &descriptor) noexcept {
    const auto image = descriptor.image_token;
    const auto palette = descriptor.palette_token;
    return {
        0xC2000000u | (descriptor.format >= 8u ? 0u : 1u),
        0xC3000000u | descriptor.format,
        0xA0000000u | (image & 0x00FFFFFFu),
        0xA8000000u | ((image >> 8u) & 0x00FF0000u) | descriptor.width,
        0xB8000000u | (raster_exponent(descriptor.height) << 8u) |
            raster_exponent(descriptor.width),
        0xC500FF00u | descriptor.palette_format,
        0xB0000000u | (palette & 0x00FFFFFFu),
        0xB1000000u | ((palette >> 8u) & 0x00FF0000u),
        0xC4000000u | ((static_cast<std::uint32_t>(descriptor.palette_count) + 7u) >> 3u),
    };
}

} // namespace

TextureCommandResult build_texture_commands(
    std::span<const TextureCommandDescriptor> descriptors,
    const TextureCommandRequest &request, std::size_t max_blocks) {
    TextureCommandResult result;
    const std::uint32_t raw_count =
        request.override_count == 0u ? request.declared_count : request.override_count;
    if (raw_count == 0u || (raw_count & 0x80000000u) != 0u) {
        result.state = {request.source_token, request.command_base_token,
                        request.declared_count,
                        static_cast<std::uint8_t>(request.declared_count)};
        return result;
    }

    const auto count = static_cast<std::size_t>(raw_count);
    if (count > 4096u || count > max_blocks) {
        result.error = TextureCommandError::BlockBudget;
        return result;
    }

    const auto first_record = static_cast<std::size_t>(request.first_record & 0xFFu);
    const auto first_slot = static_cast<std::size_t>(request.first_slot & 0xFFu);
    result.required_source_records = first_record + count;
    result.required_destination_slots = first_slot + count;
    if (result.required_source_records > descriptors.size()) {
        result.error = TextureCommandError::SourceRange;
        return result;
    }
    if (result.required_destination_slots > request.destination_slots) {
        result.error = TextureCommandError::DestinationRange;
        return result;
    }
    if ((request.command_base_token & 3u) != 0u) {
        result.error = TextureCommandError::DestinationAlignment;
        return result;
    }

    const auto last_byte = static_cast<std::uint64_t>(request.command_base_token) +
                           (static_cast<std::uint64_t>(result.required_destination_slots) - 1u) *
                               36u +
                           35u;
    if (last_byte > std::numeric_limits<std::uint32_t>::max()) {
        result.error = TextureCommandError::AddressOverflow;
        return result;
    }

    // Validate the complete selection before adding any patch. A bad later
    // descriptor must not leave an apparently usable partial command list.
    for (std::size_t i = 0; i < count; ++i) {
        if (const auto error = validate_descriptor(descriptors[first_record + i]);
            error != TextureCommandError::None) {
            result.error = error;
            return result;
        }
    }

    result.patches.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.patches.push_back({static_cast<std::uint32_t>(first_record + i),
                                  static_cast<std::uint32_t>(first_slot + i),
                                  command_words(descriptors[first_record + i])});
    }
    result.state = {request.source_token, request.command_base_token,
                    request.declared_count,
                    static_cast<std::uint8_t>(request.declared_count)};
    return result;
}

} // namespace mhp3rd::resources
