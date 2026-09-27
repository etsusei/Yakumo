#include "native/texture_commands_bridge.hpp"

#include "resources/texture_commands.hpp"
#include "psprecomp/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mhp3rd::native {
namespace {

using psprecomp::GuestMemory;
using resources::TextureCommandDescriptor;
using resources::TextureCommandError;
using resources::TextureCommandRequest;

constexpr std::uint32_t kEntry = 0x0889E5C0u;
constexpr std::uint32_t kHelper = 0x08876A10u;
constexpr std::uint32_t kTable = 0x089CED28u;
constexpr std::uint32_t kGlobal = 0x08AB3668u;
constexpr std::size_t kBuilderBytes = 528u;
constexpr std::size_t kHelperBytes = 236u;
constexpr std::size_t kTableBytes = 4100u;
constexpr std::size_t kSourceLimit = 16u * 1024u * 1024u;
constexpr std::size_t kBlockLimit = 4096u;
constexpr std::size_t kFrameBytes = 80u;

constexpr std::string_view kBuilderHash =
    "c4f5b737eb673e30b7d21425ef67ed9d2c3bab11228aadcbb389e25f1519694b";
constexpr std::string_view kHelperHash =
    "2da1298722400450aba70d5585e98b9b2a2ca3c0dd2057089700aa9375a76874";
constexpr std::string_view kTableHash =
    "c9a6501e384b7f7670e8c5c48c90f1b70b5d981dd6fc7fc37a0b7c5d566bf849";

struct PhysicalRange {
    std::uint64_t begin{};
    std::uint64_t end{};
};

// In addition to GuestMemory's mapping check, retain the unwrapped raw and
// canonical ends. A cached and an uncached pointer to the same bytes therefore
// receive the same physical interval for overlap checks.
[[nodiscard]] std::optional<PhysicalRange> main_ram_range(
    const GuestMemory &memory, std::uint64_t raw, std::size_t length) noexcept {
    if (length == 0u || raw > std::numeric_limits<std::uint32_t>::max() ||
        raw + length > (std::uint64_t{1} << 32u)) {
        return std::nullopt;
    }
    const auto address = static_cast<std::uint32_t>(raw);
    const auto physical = static_cast<std::uint64_t>(GuestMemory::canonical(address));
    const auto ram_start = static_cast<std::uint64_t>(GuestMemory::kPhysicalBase);
    const auto ram_end = ram_start + memory.size();
    if (physical < ram_start || physical + length > ram_end ||
        !memory.contains(address, length)) {
        return std::nullopt;
    }
    return PhysicalRange{physical, physical + length};
}

[[nodiscard]] constexpr bool overlaps(PhysicalRange a, PhysicalRange b) noexcept {
    return a.begin < b.end && b.begin < a.end;
}

template <std::size_t Size>
[[nodiscard]] bool matches_fingerprint(
    const GuestMemory &memory, std::uint32_t address,
    std::string_view expected) {
    std::array<std::uint8_t, Size> bytes{};
    memory.copy_out(address, bytes);
    return psprecomp::sha256_bytes(bytes) == expected;
}

[[nodiscard]] constexpr bool no_palette(std::uint32_t format) noexcept {
    return format == 1u || format == 3u || format == 8u ||
           format == 9u || format == 10u;
}

[[nodiscard]] std::uint32_t source_word(
    const GuestMemory &memory, std::uint32_t source, std::size_t offset) {
    // The caller has checked the complete source allocation, including this
    // field, and its unwrapped 32-bit address interval.
    return memory.load32(source + static_cast<std::uint32_t>(offset));
}

[[nodiscard]] std::uint16_t source_halfword(
    const GuestMemory &memory, std::uint32_t source, std::size_t offset) {
    return memory.load16(source + static_cast<std::uint32_t>(offset));
}

[[nodiscard]] constexpr bool contains_field(
    std::size_t begin, std::size_t end, std::size_t offset,
    std::size_t width) noexcept {
    return offset >= begin && offset <= end && width <= end - offset;
}

// Only fields the certified helper reaches are decoded. Record and subblock
// extents are nevertheless bounded by the caller's source allocation. A
// terminal extent need not be divisible by four; only a stride used to reach
// another accessed header must preserve that header's alignment.
[[nodiscard]] bool decode_descriptors(
    const GuestMemory &memory, std::uint32_t source,
    std::size_t source_bytes, std::size_t first, std::size_t count,
    std::vector<TextureCommandDescriptor> &descriptors) {
    const auto selected_end = first + count;
    std::size_t record = 16u;
    for (std::size_t index = 0; index < selected_end; ++index) {
        if (!contains_field(0u, source_bytes, record, 16u) ||
            (record & 3u) != 0u) {
            return false;
        }
        const auto record_size = source_word(memory, source, record);
        if (record_size < 16u || record_size > source_bytes - record) {
            return false;
        }
        const auto record_end = record + record_size;
        if (index >= first) {
            const auto image_block = record + 16u;
            if (!contains_field(record, record_end, image_block, 16u)) {
                return false;
            }
            const auto image_size = source_word(memory, source, image_block);
            if (image_size < 16u || image_size > record_end - image_block) {
                return false;
            }
            TextureCommandDescriptor descriptor{};
            descriptor.image_token = source + static_cast<std::uint32_t>(image_block + 16u);
            descriptor.format = source_word(memory, source, image_block + 8u);
            descriptor.width = source_halfword(memory, source, image_block + 12u);
            descriptor.height = source_halfword(memory, source, image_block + 14u);

            if (!no_palette(descriptor.format)) {
                const auto skip = source_word(memory, source, record + 8u);
                if (skip > kBlockLimit) return false;
                std::size_t block = image_block;
                for (std::size_t n = 0; n < skip; ++n) {
                    if (!contains_field(record, record_end, block, 16u) ||
                        (block & 3u) != 0u) {
                        return false;
                    }
                    const auto stride = source_word(memory, source, block);
                    if (stride < 16u || (stride & 3u) != 0u ||
                        stride > record_end - block) {
                        return false;
                    }
                    block += stride;
                }
                if (!contains_field(record, record_end, block, 16u) ||
                    (block & 3u) != 0u) {
                    return false;
                }
                const auto palette_size = source_word(memory, source, block);
                if (palette_size < 16u || palette_size > record_end - block) {
                    return false;
                }
                descriptor.palette_token = source + static_cast<std::uint32_t>(block + 16u);
                descriptor.palette_format = source_word(memory, source, block + 8u);
                const auto raw_count = source_halfword(memory, source, block + 12u);
                descriptor.palette_count = raw_count < 0x8000u
                    ? static_cast<std::int32_t>(raw_count)
                    : static_cast<std::int32_t>(raw_count) - 0x10000;
            }
            descriptors[index] = descriptor;
        }
        if (index + 1u < selected_end) {
            if ((record_size & 3u) != 0u) return false;
            record = record_end;
        }
    }
    return true;
}

[[nodiscard]] constexpr TextureBridgeError core_error(
    TextureCommandError error) noexcept {
    switch (error) {
    case TextureCommandError::None: return TextureBridgeError::None;
    case TextureCommandError::BlockBudget: return TextureBridgeError::Budget;
    case TextureCommandError::DestinationAlignment: return TextureBridgeError::Alignment;
    case TextureCommandError::InvalidFormat:
    case TextureCommandError::InvalidDimensions:
    case TextureCommandError::InvalidPalette: return TextureBridgeError::Descriptor;
    case TextureCommandError::SourceRange:
    case TextureCommandError::DestinationRange:
    case TextureCommandError::AddressOverflow: return TextureBridgeError::Range;
    }
    return TextureBridgeError::Descriptor;
}

void write_descriptor(GuestMemory &memory, std::uint32_t frame,
                      const TextureCommandDescriptor &descriptor) {
    memory.store32(frame + 0u, descriptor.image_token);
    memory.store32(frame + 4u, descriptor.format);
    memory.store16(frame + 8u, static_cast<std::uint16_t>(descriptor.width));
    memory.store16(frame + 10u, static_cast<std::uint16_t>(descriptor.height));
    memory.store32(frame + 12u, descriptor.palette_token);
    memory.store32(frame + 16u, descriptor.palette_format);
    memory.store32(frame + 20u, static_cast<std::uint32_t>(descriptor.palette_count));
}

} // namespace

TextureBridgeResult apply_texture_commands(
    GuestMemory &memory, psprecomp::AllegrexContext &context,
    const TextureCommandBounds &bounds) {
    if (context.pc != kEntry) return {TextureBridgeError::Entry, 0u};

    const auto state = context.gpr[4];
    const auto command_base = context.gpr[5];
    const auto source = context.gpr[6];
    const auto first_slot = static_cast<std::size_t>(context.gpr[7] & 0xFFu);
    const auto first_record = static_cast<std::size_t>(context.gpr[8] & 0xFFu);
    const auto stack = context.gpr[29];
    if (bounds.source_bytes < 12u || bounds.source_bytes > kSourceLimit ||
        stack < kFrameBytes) {
        return {TextureBridgeError::Range, 0u};
    }
    if ((state & 3u) != 0u || (source & 3u) != 0u ||
        (stack & 3u) != 0u) {
        return {TextureBridgeError::Alignment, 0u};
    }
    const auto frame = stack - static_cast<std::uint32_t>(kFrameBytes);
    const auto source_range = main_ram_range(memory, source, bounds.source_bytes);
    const auto state_range = main_ram_range(memory, state, 9u);
    const auto stack_range = main_ram_range(memory, frame, kFrameBytes);
    const auto builder_range = main_ram_range(memory, kEntry, kBuilderBytes);
    const auto helper_range = main_ram_range(memory, kHelper, kHelperBytes);
    const auto table_range = main_ram_range(memory, kTable, kTableBytes);
    if (!source_range || !state_range || !stack_range) {
        return {TextureBridgeError::Range, 0u};
    }
    if (!builder_range || !helper_range || !table_range ||
        !matches_fingerprint<kBuilderBytes>(memory, kEntry, kBuilderHash) ||
        !matches_fingerprint<kHelperBytes>(memory, kHelper, kHelperHash) ||
        !matches_fingerprint<kTableBytes>(memory, kTable, kTableHash)) {
        return {TextureBridgeError::Dependency, 0u};
    }
    const PhysicalRange global_range{kGlobal, static_cast<std::uint64_t>(kGlobal) + 4u};
    constexpr std::size_t kDependencyCount = 4u;
    const std::array<PhysicalRange, kDependencyCount> dependencies{
        *builder_range, *helper_range, *table_range, global_range};
    if (overlaps(*source_range, *state_range) ||
        overlaps(*source_range, *stack_range) ||
        overlaps(*state_range, *stack_range)) {
        return {TextureBridgeError::Aliasing, 0u};
    }
    for (const auto dependency : dependencies) {
        if (overlaps(*source_range, dependency) ||
            overlaps(*state_range, dependency) ||
            overlaps(*stack_range, dependency)) {
            return {TextureBridgeError::Aliasing, 0u};
        }
    }

    const auto declared_count = memory.load32(source + 8u);
    const auto effective = context.gpr[9] == 0u ? declared_count : context.gpr[9];
    const bool has_commands = effective != 0u && (effective & 0x80000000u) == 0u;
    const auto count = has_commands ? static_cast<std::size_t>(effective) : 0u;
    if (has_commands && (count > kBlockLimit || count > bounds.max_blocks)) {
        return {TextureBridgeError::Budget, 0u};
    }

    std::vector<TextureCommandDescriptor> descriptors;
    resources::TextureCommandResult commands;
    if (has_commands) {
        if (bounds.source_bytes < 16u) return {TextureBridgeError::Resource, 0u};
        const auto required_slots = first_slot + count;
        if (required_slots > bounds.destination_slots ||
            (command_base & 3u) != 0u) {
            return {required_slots > bounds.destination_slots
                        ? TextureBridgeError::Range : TextureBridgeError::Alignment, 0u};
        }
        const auto raw_selected = static_cast<std::uint64_t>(command_base) +
                                  first_slot * 36u;
        const auto selected_range = main_ram_range(memory, raw_selected, count * 36u);
        if (!selected_range || !main_ram_range(memory, command_base, 1u)) {
            return {TextureBridgeError::Range, 0u};
        }
        if (!main_ram_range(memory, kGlobal, 4u)) {
            return {TextureBridgeError::Dependency, 0u};
        }
        for (const auto dependency : dependencies) {
            if (overlaps(*selected_range, dependency)) {
                return {TextureBridgeError::Aliasing, 0u};
            }
        }
        if (overlaps(*selected_range, *source_range) ||
            overlaps(*selected_range, *state_range) ||
            overlaps(*selected_range, *stack_range)) {
            return {TextureBridgeError::Aliasing, 0u};
        }
        // The original reads this memory-only ELF address once per command.
        // Its value is dead in the certified helper, but it must be mapped.
        (void)memory.load32(kGlobal);

        const auto required_records = first_record + count;
        descriptors.resize(required_records);
        if (!decode_descriptors(memory, source, bounds.source_bytes,
                                first_record, count, descriptors)) {
            return {TextureBridgeError::Resource, 0u};
        }
        const TextureCommandRequest request{
            source, command_base, declared_count, context.gpr[8],
            context.gpr[7], context.gpr[9], bounds.destination_slots};
        commands = resources::build_texture_commands(descriptors, request,
                                                      bounds.max_blocks);
        if (!commands.ok()) return {core_error(commands.error), 0u};
    }

    // No adapter allocations or validation remain after this point. Each store has
    // already been proved to lie in a disjoint, mapped main-RAM interval.
    constexpr std::array<std::size_t, 10> saved_regs{
        16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 30u, 31u};
    for (std::size_t i = 0; i < saved_regs.size(); ++i) {
        memory.store32(frame + 32u + static_cast<std::uint32_t>(i * 4u),
                       context.gpr[saved_regs[i]]);
    }
    memory.store32(state + 4u, command_base);
    if (has_commands) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto &patch = commands.patches[i];
            write_descriptor(memory, frame,
                             descriptors[patch.source_record]);
            const auto slot = command_base + patch.destination_slot * 36u;
            for (const auto word_index : resources::kTextureCommandStoreOrder) {
                memory.store32(slot + static_cast<std::uint32_t>(word_index * 4u),
                               patch.words[word_index]);
            }
        }
    }
    memory.store32(state + 0u, source);
    memory.store8(state + 8u, static_cast<std::uint8_t>(declared_count));

    context.gpr[2] = declared_count;
    if (has_commands) {
        const auto &last = commands.patches.back();
        const auto &last_descriptor = descriptors[last.source_record];
        context.gpr[3] = 0xC4000000u;
        context.gpr[4] = last.words[7];
        context.gpr[5] = last.words[6];
        context.gpr[6] = command_base + last.destination_slot * 36u;
        context.gpr[7] = kTable + last_descriptor.height * 4u;
        context.gpr[8] = kTable + last_descriptor.width * 4u;
        context.gpr[9] = last.words[1];
        context.gpr[10] = last.words[2];
    } else {
        context.gpr[3] = static_cast<std::uint32_t>(first_slot);
    }
    context.pc = context.gpr[31];
    return {TextureBridgeError::None, count};
}

} // namespace mhp3rd::native
