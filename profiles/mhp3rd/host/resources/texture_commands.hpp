#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mhp3rd::resources {
// Opaque 32-bit address tokens are encoded, never dereferenced. The caller
// establishes their backing storage and lifetime outside this value API.
struct TextureCommandDescriptor {
    std::uint32_t image_token{}, format{}, width{}, height{};
    std::uint32_t palette_token{}, palette_format{};
    std::int32_t palette_count{};
};
struct TextureCommandRequest {
    std::uint32_t source_token{}, command_base_token{}, declared_count{};
    std::uint32_t first_record{}, first_slot{}, override_count{};
    std::size_t destination_slots{};
};
struct TextureCommandState {
    std::uint32_t source_token{}, command_base_token{}, declared_count{};
    std::uint8_t count_byte{};
};
struct TextureCommandPatch {
    std::uint32_t source_record{}, destination_slot{};
    std::array<std::uint32_t, 9> words{};
};
enum class TextureCommandError {
    None, BlockBudget, SourceRange, DestinationRange, DestinationAlignment,
    AddressOverflow, InvalidFormat, InvalidDimensions, InvalidPalette,
};
struct TextureCommandResult {
    TextureCommandError error{TextureCommandError::None};
    TextureCommandState state{};
    std::vector<TextureCommandPatch> patches;
    std::size_t required_source_records{}, required_destination_slots{};
    [[nodiscard]] bool ok() const noexcept { return error == TextureCommandError::None; }
};
inline constexpr std::array<std::size_t, 9> kTextureCommandStoreOrder{3, 0, 1, 2, 4, 5, 6, 7, 8};

// Zero override selects the declared count; an effective signed-negative
// count emits nothing. Only the initial source/destination indexes are masked
// to a byte. Later indexes can exceed 255. Declared count is retained even
// for a partial update; count_byte is its low byte, not the number emitted.
// No-command requests inspect no descriptors or destination address/capacity.
// Positive requests require known formats 0..10, dimensions 1..1024 and a
// representable command range. Formats 1,3,8,9,10 require zero palette fields;
// the others accept palette formats 0..3 and counts 0..32767. This checked
// domain does not claim every such record is a supported TMH file or GPU use.
// Failure emits no patches/state; required counts may explain a range error.
// Inputs remain unchanged; allocation failures may throw.
[[nodiscard]] TextureCommandResult build_texture_commands(
    std::span<const TextureCommandDescriptor> descriptors,
    const TextureCommandRequest &request, std::size_t max_blocks = 4096u);
} // namespace mhp3rd::resources
