#include "resources/texture_commands.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

using mhp3rd::resources::TextureCommandDescriptor;
using mhp3rd::resources::TextureCommandError;
using mhp3rd::resources::TextureCommandRequest;
using mhp3rd::resources::TextureCommandResult;
using mhp3rd::resources::build_texture_commands;

int failures = 0;

void check(bool condition, const std::string &description) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description.c_str());
    if (!condition) ++failures;
}

TextureCommandDescriptor indexed(std::uint32_t format = 4u) {
    return {0xFE123456u, format, 3u, 5u, 0xDBA76543u, 3u, 9};
}

TextureCommandDescriptor without_palette(std::uint32_t format = 8u) {
    return {0xFE123456u, format, 3u, 5u, 0u, 0u, 0};
}

TextureCommandRequest request(std::uint32_t declared = 1u,
                              std::uint32_t override_count = 0u) {
    return {0x87654321u, 0x10000000u, declared, 0u, 0u, override_count, 1u};
}

bool has_default_failure(const TextureCommandResult &result,
                         TextureCommandError expected) {
    return result.error == expected && !result.ok() && result.patches.empty() &&
           result.state.source_token == 0u && result.state.command_base_token == 0u &&
           result.state.declared_count == 0u && result.state.count_byte == 0u;
}

void test_exact_words_for_every_format() {
    struct Case {
        std::uint32_t format;
        std::uint32_t c2;
        std::uint32_t c3;
        bool no_palette;
    };
    constexpr std::array<Case, 11> cases{{
        {0u, 0xC2000001u, 0xC3000000u, false},
        {1u, 0xC2000001u, 0xC3000001u, true},
        {2u, 0xC2000001u, 0xC3000002u, false},
        {3u, 0xC2000001u, 0xC3000003u, true},
        {4u, 0xC2000001u, 0xC3000004u, false},
        {5u, 0xC2000001u, 0xC3000005u, false},
        {6u, 0xC2000001u, 0xC3000006u, false},
        {7u, 0xC2000001u, 0xC3000007u, false},
        {8u, 0xC2000000u, 0xC3000008u, true},
        {9u, 0xC2000000u, 0xC3000009u, true},
        {10u, 0xC2000000u, 0xC300000Au, true},
    }};
    for (const auto &entry : cases) {
        const auto descriptor = entry.no_palette ? without_palette(entry.format)
                                                 : indexed(entry.format);
        const auto result = build_texture_commands(std::span{&descriptor, 1u}, request());
        const std::array<std::uint32_t, 9> expected{
            entry.c2, entry.c3, 0xA0123456u, 0xA8FE0003u, 0xB8000302u,
            entry.no_palette ? 0xC500FF00u : 0xC500FF03u,
            entry.no_palette ? 0xB0000000u : 0xB0A76543u,
            entry.no_palette ? 0xB1000000u : 0xB1DB0000u,
            entry.no_palette ? 0xC4000000u : 0xC4000002u,
        };
        check(result.ok() && result.patches.size() == 1u &&
                  result.patches.front().words == expected,
              "format " + std::to_string(entry.format) + " emits all nine certified words");
    }
    check(mhp3rd::resources::kTextureCommandStoreOrder ==
              (std::array<std::size_t, 9>{3u, 0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u}),
          "published store order matches the original instruction order");
}

void test_no_command_state_and_signed_count() {
    const std::span<const TextureCommandDescriptor> empty;
    auto input = request(0u);
    input.command_base_token = 3u;
    input.first_record = 0xFFFFFFFFu;
    input.first_slot = 0xFFFFFFFFu;
    input.destination_slots = 0u;
    auto result = build_texture_commands(empty, input, 0u);
    check(result.ok() && result.patches.empty() &&
              result.state.source_token == input.source_token &&
              result.state.command_base_token == 3u &&
              result.state.declared_count == 0u && result.state.count_byte == 0u,
          "zero count returns state without checking tokens, indexes, capacity or budget");

    input.declared_count = 0x123456ABu;
    input.override_count = 0xFFFFFFFFu;
    result = build_texture_commands(empty, input, 0u);
    check(result.ok() && result.patches.empty() &&
              result.state.declared_count == 0x123456ABu &&
              result.state.count_byte == 0xABu,
          "negative override skips commands but retains full declared count and low byte");

    input.declared_count = 0x800001CDu;
    input.override_count = 0u;
    result = build_texture_commands(empty, input);
    check(result.ok() && result.patches.empty() &&
              result.state.declared_count == 0x800001CDu &&
              result.state.count_byte == 0xCDu,
          "signed-negative declared count also skips all command validation");
}

void test_byte_masks_partial_updates_and_ownership() {
    std::vector<TextureCommandDescriptor> descriptors(257u, indexed());
    descriptors[254u] = indexed(4u);
    descriptors[255u] = without_palette(8u);
    descriptors[256u] = indexed(5u);
    auto input = request(0x123456AAu, 3u);
    input.first_record = 0xABCD00FEu;
    input.first_slot = 0x765400FFu;
    input.destination_slots = 258u;
    const auto result = build_texture_commands(descriptors, input);
    check(result.ok() && result.patches.size() == 3u &&
              result.required_source_records == 257u &&
              result.required_destination_slots == 258u,
          "three selected records and slots cross 255 without masking again");
    check(result.patches.size() == 3u &&
              result.patches[0].source_record == 254u &&
              result.patches[1].source_record == 255u &&
              result.patches[2].source_record == 256u &&
              result.patches[0].destination_slot == 255u &&
              result.patches[1].destination_slot == 256u &&
              result.patches[2].destination_slot == 257u,
          "only the initial source and destination indexes use their low byte");
    check(result.state.source_token == input.source_token &&
              result.state.command_base_token == input.command_base_token &&
              result.state.declared_count == 0x123456AAu &&
              result.state.count_byte == 0xAAu,
          "partial update state reports the declared count, not three emitted blocks");
    const auto owned_words = result.patches[1].words;
    descriptors[255u].image_token = 0u;
    descriptors.clear();
    check(result.patches[1].words == owned_words &&
              result.patches[1].words[2] == 0xA0123456u,
          "patches own command words after source descriptors change or disappear");

    std::vector<TextureCommandDescriptor> two(2u, indexed());
    input = request(2u, 0u);
    input.first_record = 1u;
    input.destination_slots = 3u;
    const auto failed = build_texture_commands(two, input);
    check(has_default_failure(failed, TextureCommandError::SourceRange) &&
              failed.required_source_records == 3u,
          "zero override uses full declared count, not records remaining after first_record");
}

void test_ranges_budgets_and_addresses() {
    const std::array descriptors{indexed(), indexed(), indexed()};
    auto input = request(2u);
    auto result = build_texture_commands(std::span{descriptors.data(), 1u}, input);
    check(has_default_failure(result, TextureCommandError::SourceRange) &&
              result.required_source_records == 2u &&
              result.required_destination_slots == 2u,
          "short source span rejects the complete selection");

    result = build_texture_commands(descriptors, input);
    check(has_default_failure(result, TextureCommandError::DestinationRange) &&
              result.required_destination_slots == 2u,
          "short destination rejects without emitting the first patch");

    input = request(3u);
    input.destination_slots = 3u;
    result = build_texture_commands(descriptors, input, 2u);
    check(has_default_failure(result, TextureCommandError::BlockBudget),
          "caller block budget limits a positive request");
    const std::vector<TextureCommandDescriptor> maximum(4096u, indexed());
    input = request(4096u);
    input.destination_slots = 4096u;
    result = build_texture_commands(maximum, input);
    check(result.ok() && result.patches.size() == 4096u &&
              result.patches.back().source_record == 4095u &&
              result.patches.back().destination_slot == 4095u,
          "hard 4096-block limit is inclusive");
    input = request(4097u);
    input.destination_slots = 4097u;
    result = build_texture_commands(descriptors, input, 5000u);
    check(has_default_failure(result, TextureCommandError::BlockBudget),
          "hard 4096-block limit also applies when caller budget is larger");

    input = request();
    input.command_base_token = 0x10000002u;
    result = build_texture_commands(std::span{descriptors.data(), 1u}, input);
    check(has_default_failure(result, TextureCommandError::DestinationAlignment),
          "unaligned destination base rejects a positive command range");

    input.command_base_token = 0xFFFFFF94u;
    input.first_slot = 0xAB000002u;
    input.destination_slots = 3u;
    result = build_texture_commands(std::span{descriptors.data(), 1u}, input);
    check(result.ok() && result.patches.size() == 1u &&
              result.patches[0].destination_slot == 2u,
          "last byte exactly at UINT32_MAX is representable with a nonzero slot");
    input.command_base_token += 4u;
    result = build_texture_commands(std::span{descriptors.data(), 1u}, input);
    check(has_default_failure(result, TextureCommandError::AddressOverflow),
          "last byte past UINT32_MAX rejects without wrapped addresses");
}

void test_dimensions_palette_counts_and_invalid_descriptors() {
    auto descriptor = indexed();
    auto result = build_texture_commands(std::span{&descriptor, 1u}, request());
    check(result.ok() && result.patches[0].words[4] == 0xB8000302u,
          "off-power dimensions 3 by 5 round to exponents 2 by 3");
    descriptor.width = descriptor.height = 1u;
    result = build_texture_commands(std::span{&descriptor, 1u}, request());
    check(result.ok() && result.patches[0].words[4] == 0xB8000000u,
          "dimension one uses exponent zero");
    descriptor.width = descriptor.height = 1024u;
    result = build_texture_commands(std::span{&descriptor, 1u}, request());
    check(result.ok() && result.patches[0].words[4] == 0xB8000A0Au,
          "dimension 1024 uses exponent ten");

    descriptor = indexed();
    descriptor.palette_token = 0u;
    for (std::uint32_t format = 0u; format <= 3u; ++format) {
        descriptor.palette_format = format;
        result = build_texture_commands(std::span{&descriptor, 1u}, request());
        check(result.ok() && result.patches[0].words[5] == (0xC500FF00u | format) &&
                  result.patches[0].words[6] == 0xB0000000u,
              "indexed palette format " + std::to_string(format) +
                  " accepts an opaque zero token");
    }

    struct CountCase { std::int32_t count; std::uint32_t c4; };
    constexpr std::array<CountCase, 7> counts{{
        {0, 0xC4000000u}, {1, 0xC4000001u}, {7, 0xC4000001u},
        {8, 0xC4000001u}, {9, 0xC4000002u}, {8448, 0xC4000420u},
        {32767, 0xC4001000u},
    }};
    for (const auto &entry : counts) {
        descriptor.palette_count = entry.count;
        result = build_texture_commands(std::span{&descriptor, 1u}, request());
        check(result.ok() && result.patches[0].words[8] == entry.c4,
              "palette count " + std::to_string(entry.count) +
                  " retains the original rounded CLUT load expression");
    }

    const auto invalid = [](TextureCommandDescriptor candidate,
                            TextureCommandError error) {
        const auto outcome = build_texture_commands(std::span{&candidate, 1u}, request());
        return has_default_failure(outcome, error);
    };
    descriptor = indexed();
    descriptor.format = 11u;
    check(invalid(descriptor, TextureCommandError::InvalidFormat),
          "format after 10 rejects");
    descriptor.format = std::numeric_limits<std::uint32_t>::max();
    check(invalid(descriptor, TextureCommandError::InvalidFormat),
          "wrapped-looking format rejects");
    descriptor = indexed();
    descriptor.width = 0u;
    check(invalid(descriptor, TextureCommandError::InvalidDimensions),
          "zero width rejects");
    descriptor = indexed();
    descriptor.height = 0u;
    check(invalid(descriptor, TextureCommandError::InvalidDimensions),
          "zero height rejects");
    descriptor = indexed();
    descriptor.width = 1025u;
    check(invalid(descriptor, TextureCommandError::InvalidDimensions),
          "width past the exponent table rejects");
    descriptor = indexed();
    descriptor.height = 1025u;
    check(invalid(descriptor, TextureCommandError::InvalidDimensions),
          "height past the exponent table rejects");

    descriptor = without_palette();
    descriptor.palette_token = 1u;
    check(invalid(descriptor, TextureCommandError::InvalidPalette),
          "no-palette format rejects palette token");
    descriptor = without_palette();
    descriptor.palette_format = 1u;
    check(invalid(descriptor, TextureCommandError::InvalidPalette),
          "no-palette format rejects palette format");
    descriptor = without_palette();
    descriptor.palette_count = 1;
    check(invalid(descriptor, TextureCommandError::InvalidPalette),
          "no-palette format rejects palette count");
    descriptor = indexed();
    descriptor.palette_format = 4u;
    check(invalid(descriptor, TextureCommandError::InvalidPalette),
          "indexed format rejects palette format after three");
    descriptor = indexed();
    descriptor.palette_count = -1;
    check(invalid(descriptor, TextureCommandError::InvalidPalette),
          "signed-negative palette count rejects");
    descriptor = indexed();
    descriptor.palette_count = 32768;
    check(invalid(descriptor, TextureCommandError::InvalidPalette),
          "palette count past signed 16-bit maximum rejects");

    std::array late{indexed(), indexed(), indexed()};
    late[2].height = 1025u;
    auto input = request(3u);
    input.destination_slots = 3u;
    result = build_texture_commands(late, input);
    check(has_default_failure(result, TextureCommandError::InvalidDimensions),
          "invalid later descriptor leaks no earlier patches or success state");
}

} // namespace

int main() {
    test_exact_words_for_every_format();
    test_no_command_state_and_signed_count();
    test_byte_masks_partial_updates_and_ownership();
    test_ranges_budgets_and_addresses();
    test_dimensions_palette_counts_and_invalid_descriptors();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
