#include "native/texture_commands_bridge.hpp"

#include "resources/texture_commands.hpp"
#include "psprecomp/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
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

struct RegionImage {
    std::uint32_t address{};
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
};

struct GuestStore {
    std::uint32_t address{};
    std::uint32_t value{};
    std::uint8_t width{};
};

[[nodiscard]] RegionImage capture_region(const GuestMemory &memory,
                                         std::uint32_t address,
                                         std::size_t length, bool writable) {
    RegionImage region;
    region.address = address;
    region.before.resize(length);
    memory.copy_out(address, region.before);
    if (writable) region.after = region.before;
    return region;
}

void append_store(std::vector<GuestStore> &writes, RegionImage &region,
                  std::uint32_t address, std::uint32_t value,
                  std::uint8_t width) {
    writes.push_back({address, value, width});
    const auto offset = static_cast<std::size_t>(address - region.address);
    for (std::size_t i = 0; i < width; ++i) {
        region.after[offset + i] = static_cast<std::uint8_t>(value >> (i * 8u));
    }
}

void append_descriptor(std::vector<GuestStore> &writes, RegionImage &frame_image,
                       std::uint32_t frame,
                       const TextureCommandDescriptor &descriptor) {
    append_store(writes, frame_image, frame + 0u, descriptor.image_token, 4u);
    append_store(writes, frame_image, frame + 4u, descriptor.format, 4u);
    append_store(writes, frame_image, frame + 8u, descriptor.width, 2u);
    append_store(writes, frame_image, frame + 10u, descriptor.height, 2u);
    append_store(writes, frame_image, frame + 12u, descriptor.palette_token, 4u);
    append_store(writes, frame_image, frame + 16u, descriptor.palette_format, 4u);
    append_store(writes, frame_image, frame + 20u,
                 static_cast<std::uint32_t>(descriptor.palette_count), 4u);
}

[[nodiscard]] bool same_cpu(const psprecomp::AllegrexContext &a,
                            const psprecomp::AllegrexContext &b) noexcept {
    return a.gpr == b.gpr && a.hi == b.hi && a.lo == b.lo && a.pc == b.pc &&
           a.fcr31 == b.fcr31 && a.vfpu_ctrl == b.vfpu_ctrl &&
           std::memcmp(a.fpr.data(), b.fpr.data(), sizeof(a.fpr)) == 0 &&
           std::memcmp(a.vfpu.data(), b.vfpu.data(), sizeof(a.vfpu)) == 0;
}

enum class RegionMatch { Same, Mapping, Different };

[[nodiscard]] RegionMatch match_region(const GuestMemory &memory,
                                       const RegionImage &region,
                                       bool expected_after) noexcept {
    if (region.before.empty()) return RegionMatch::Same;
    const auto &expected = expected_after ? region.after : region.before;
    if (!main_ram_range(memory, region.address, expected.size())) {
        return RegionMatch::Mapping;
    }
    const auto *actual = memory.raw_pointer(region.address, expected.size());
    if (!actual) return RegionMatch::Mapping;
    return std::memcmp(actual, expected.data(), expected.size()) == 0
        ? RegionMatch::Same : RegionMatch::Different;
}

void perform_store(GuestMemory &memory, const GuestStore &write) {
    switch (write.width) {
    case 1u: memory.store8(write.address, static_cast<std::uint8_t>(write.value)); break;
    case 2u: memory.store16(write.address, static_cast<std::uint16_t>(write.value)); break;
    default: memory.store32(write.address, write.value); break;
    }
}

} // namespace

struct TextureCommandPlan::Impl {
    const GuestMemory *memory{};
    bool consumed{};
    bool has_commands{};
    std::uint32_t command_base{};
    psprecomp::AllegrexContext before_cpu{};
    psprecomp::AllegrexContext after_cpu{};
    RegionImage source, state, frame, output, builder, helper, table, global;
    std::vector<GuestStore> writes;
};

TextureCommandPlan::TextureCommandPlan() noexcept = default;
TextureCommandPlan::~TextureCommandPlan() = default;

TextureCommandPlan::TextureCommandPlan(TextureCommandPlan &&other) noexcept
    : impl_(std::move(other.impl_)), result_(other.result_) {
    other.result_ = {TextureBridgeError::InvalidPlan, 0u};
}

TextureCommandPlan &TextureCommandPlan::operator=(TextureCommandPlan &&other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        result_ = other.result_;
        other.result_ = {TextureBridgeError::InvalidPlan, 0u};
    }
    return *this;
}

bool TextureCommandPlan::ok() const noexcept {
    return result_.ok() && impl_ && !impl_->consumed;
}

TextureBridgeError TextureCommandPlan::error() const noexcept {
    if (result_.ok() && impl_ && impl_->consumed) return TextureBridgeError::ConsumedPlan;
    return result_.error;
}

std::size_t TextureCommandPlan::blocks() const noexcept {
    return result_.blocks;
}

TextureCommandPlan prepare_texture_commands(
    const GuestMemory &memory, const psprecomp::AllegrexContext &context,
    const TextureCommandBounds &bounds) {
    TextureCommandPlan plan;
    const auto fail = [&](TextureBridgeError error) -> TextureCommandPlan {
        plan.result_ = {error, 0u};
        return std::move(plan);
    };
    if (context.pc != kEntry) return fail(TextureBridgeError::Entry);

    const auto state = context.gpr[4];
    const auto command_base = context.gpr[5];
    const auto source = context.gpr[6];
    const auto first_slot = static_cast<std::size_t>(context.gpr[7] & 0xFFu);
    const auto first_record = static_cast<std::size_t>(context.gpr[8] & 0xFFu);
    const auto stack = context.gpr[29];
    if (bounds.source_bytes < 12u || bounds.source_bytes > kSourceLimit ||
        stack < kFrameBytes) {
        return fail(TextureBridgeError::Range);
    }
    if ((state & 3u) != 0u || (source & 3u) != 0u ||
        (stack & 3u) != 0u) {
        return fail(TextureBridgeError::Alignment);
    }
    const auto frame = stack - static_cast<std::uint32_t>(kFrameBytes);
    const auto source_range = main_ram_range(memory, source, bounds.source_bytes);
    const auto state_range = main_ram_range(memory, state, 9u);
    const auto stack_range = main_ram_range(memory, frame, kFrameBytes);
    const auto builder_range = main_ram_range(memory, kEntry, kBuilderBytes);
    const auto helper_range = main_ram_range(memory, kHelper, kHelperBytes);
    const auto table_range = main_ram_range(memory, kTable, kTableBytes);
    if (!source_range || !state_range || !stack_range) {
        return fail(TextureBridgeError::Range);
    }
    if (!builder_range || !helper_range || !table_range ||
        !matches_fingerprint<kBuilderBytes>(memory, kEntry, kBuilderHash) ||
        !matches_fingerprint<kHelperBytes>(memory, kHelper, kHelperHash) ||
        !matches_fingerprint<kTableBytes>(memory, kTable, kTableHash)) {
        return fail(TextureBridgeError::Dependency);
    }
    const PhysicalRange global_range{kGlobal, static_cast<std::uint64_t>(kGlobal) + 4u};
    constexpr std::size_t kDependencyCount = 4u;
    const std::array<PhysicalRange, kDependencyCount> dependencies{
        *builder_range, *helper_range, *table_range, global_range};
    if (overlaps(*source_range, *state_range) ||
        overlaps(*source_range, *stack_range) ||
        overlaps(*state_range, *stack_range)) {
        return fail(TextureBridgeError::Aliasing);
    }
    for (const auto dependency : dependencies) {
        if (overlaps(*source_range, dependency) ||
            overlaps(*state_range, dependency) ||
            overlaps(*stack_range, dependency)) {
            return fail(TextureBridgeError::Aliasing);
        }
    }

    const auto declared_count = memory.load32(source + 8u);
    const auto effective = context.gpr[9] == 0u ? declared_count : context.gpr[9];
    const bool has_commands = effective != 0u && (effective & 0x80000000u) == 0u;
    const auto count = has_commands ? static_cast<std::size_t>(effective) : 0u;
    if (has_commands && (count > kBlockLimit || count > bounds.max_blocks)) {
        return fail(TextureBridgeError::Budget);
    }

    std::vector<TextureCommandDescriptor> descriptors;
    resources::TextureCommandResult commands;
    std::uint32_t selected_address{};
    if (has_commands) {
        if (bounds.source_bytes < 16u) return fail(TextureBridgeError::Resource);
        const auto required_slots = first_slot + count;
        if (required_slots > bounds.destination_slots ||
            (command_base & 3u) != 0u) {
            return fail(required_slots > bounds.destination_slots
                        ? TextureBridgeError::Range : TextureBridgeError::Alignment);
        }
        const auto raw_selected = static_cast<std::uint64_t>(command_base) +
                                  first_slot * 36u;
        const auto selected_range = main_ram_range(memory, raw_selected, count * 36u);
        if (!selected_range || !main_ram_range(memory, command_base, 1u)) {
            return fail(TextureBridgeError::Range);
        }
        selected_address = static_cast<std::uint32_t>(raw_selected);
        if (!main_ram_range(memory, kGlobal, 4u)) {
            return fail(TextureBridgeError::Dependency);
        }
        for (const auto dependency : dependencies) {
            if (overlaps(*selected_range, dependency)) {
                return fail(TextureBridgeError::Aliasing);
            }
        }
        if (overlaps(*selected_range, *source_range) ||
            overlaps(*selected_range, *state_range) ||
            overlaps(*selected_range, *stack_range)) {
            return fail(TextureBridgeError::Aliasing);
        }
        // The original reads this memory-only ELF address once per command.
        // Its value is dead in the certified helper, but it must be mapped.
        (void)memory.load32(kGlobal);

        const auto required_records = first_record + count;
        descriptors.resize(required_records);
        if (!decode_descriptors(memory, source, bounds.source_bytes,
                                first_record, count, descriptors)) {
            return fail(TextureBridgeError::Resource);
        }
        const TextureCommandRequest request{
            source, command_base, declared_count, context.gpr[8],
            context.gpr[7], context.gpr[9], bounds.destination_slots};
        commands = resources::build_texture_commands(descriptors, request,
                                                      bounds.max_blocks);
        if (!commands.ok()) return fail(core_error(commands.error));
        if (commands.patches.size() != count) return fail(TextureBridgeError::Descriptor);
        for (std::size_t i = 0; i < count; ++i) {
            if (commands.patches[i].source_record != first_record + i ||
                commands.patches[i].destination_slot != first_slot + i) {
                return fail(TextureBridgeError::Descriptor);
            }
        }
    }

    auto prepared = std::make_unique<TextureCommandPlan::Impl>();
    prepared->memory = &memory;
    prepared->has_commands = has_commands;
    prepared->command_base = command_base;
    prepared->before_cpu = context;
    prepared->after_cpu = context;
    prepared->source = capture_region(memory, source, bounds.source_bytes, false);
    prepared->state = capture_region(memory, state, 9u, true);
    prepared->frame = capture_region(memory, frame, kFrameBytes, true);
    prepared->builder = capture_region(memory, kEntry, kBuilderBytes, false);
    prepared->helper = capture_region(memory, kHelper, kHelperBytes, false);
    prepared->table = capture_region(memory, kTable, kTableBytes, false);
    if (has_commands) {
        prepared->output = capture_region(memory, selected_address, count * 36u, true);
        prepared->global = capture_region(memory, kGlobal, 4u, false);
    }
    prepared->writes.reserve(13u + count * 16u);

    // Preserve the original store order, including every repeated descriptor
    // write into the stack frame. Untouched frame gaps retain their old bytes.
    constexpr std::array<std::size_t, 10> saved_regs{
        16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 30u, 31u};
    for (std::size_t i = 0; i < saved_regs.size(); ++i) {
        append_store(prepared->writes, prepared->frame,
                     frame + 32u + static_cast<std::uint32_t>(i * 4u),
                     context.gpr[saved_regs[i]], 4u);
    }
    append_store(prepared->writes, prepared->state,
                 state + 4u, command_base, 4u);
    if (has_commands) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto &patch = commands.patches[i];
            append_descriptor(prepared->writes, prepared->frame, frame,
                              descriptors[patch.source_record]);
            const auto slot = command_base + patch.destination_slot * 36u;
            for (const auto word_index : resources::kTextureCommandStoreOrder) {
                append_store(prepared->writes, prepared->output,
                             slot + static_cast<std::uint32_t>(word_index * 4u),
                             patch.words[word_index], 4u);
            }
        }
    }
    append_store(prepared->writes, prepared->state, state + 0u, source, 4u);
    append_store(prepared->writes, prepared->state, state + 8u,
                 static_cast<std::uint8_t>(declared_count), 1u);

    auto &after = prepared->after_cpu;
    after.gpr[2] = declared_count;
    if (has_commands) {
        const auto &last = commands.patches.back();
        const auto &last_descriptor = descriptors[last.source_record];
        after.gpr[3] = 0xC4000000u;
        after.gpr[4] = last.words[7];
        after.gpr[5] = last.words[6];
        after.gpr[6] = command_base + last.destination_slot * 36u;
        after.gpr[7] = kTable + last_descriptor.height * 4u;
        after.gpr[8] = kTable + last_descriptor.width * 4u;
        after.gpr[9] = last.words[1];
        after.gpr[10] = last.words[2];
    } else {
        after.gpr[3] = static_cast<std::uint32_t>(first_slot);
    }
    after.pc = context.gpr[31];
    plan.impl_ = std::move(prepared);
    plan.result_ = {TextureBridgeError::None, count};
    return plan;
}

TextureBridgeResult commit_texture_commands(
    GuestMemory &memory, psprecomp::AllegrexContext &context,
    TextureCommandPlan &plan) {
    if (!plan.result_.ok() || !plan.impl_) {
        return {TextureBridgeError::InvalidPlan, 0u};
    }
    auto &prepared = *plan.impl_;
    if (prepared.consumed) return {TextureBridgeError::ConsumedPlan, 0u};
    if (prepared.memory != &memory) {
        return {TextureBridgeError::StaleMemory, 0u};
    }
    if (!same_cpu(context, prepared.before_cpu)) {
        return {TextureBridgeError::StaleContext, 0u};
    }
    if (prepared.has_commands &&
        !main_ram_range(memory, prepared.command_base, 1u)) {
        return {TextureBridgeError::StaleMemory, 0u};
    }
    const std::array<const RegionImage *, 8> regions{
        &prepared.source, &prepared.state, &prepared.frame, &prepared.output,
        &prepared.builder, &prepared.helper, &prepared.table, &prepared.global};
    for (const auto *region : regions) {
        if (match_region(memory, *region, false) != RegionMatch::Same) {
            return {TextureBridgeError::StaleMemory, 0u};
        }
    }

    // A throwing write-watch callback may leave partial effects. Mark the plan
    // consumed before the first store so a diagnostic exception cannot replay it.
    prepared.consumed = true;
    for (const auto &write : prepared.writes) perform_store(memory, write);
    context = prepared.after_cpu;
    return {TextureBridgeError::None, plan.result_.blocks};
}

TextureBridgeComparison compare_texture_commands(
    const TextureCommandPlan &plan, const GuestMemory &memory,
    const psprecomp::AllegrexContext &context) {
    if (!plan.result_.ok() || !plan.impl_) {
        return {TextureBridgeCompareError::InvalidPlan};
    }
    const auto &prepared = *plan.impl_;
    if (prepared.consumed) return {TextureBridgeCompareError::ConsumedPlan};
    if (!same_cpu(context, prepared.after_cpu)) {
        return {TextureBridgeCompareError::Context};
    }
    if (prepared.has_commands &&
        !main_ram_range(memory, prepared.command_base, 1u)) {
        return {TextureBridgeCompareError::Mapping};
    }
    const auto compare_region = [&](const RegionImage &region, bool expected_after,
                                    TextureBridgeCompareError mismatch) {
        const auto status = match_region(memory, region, expected_after);
        if (status == RegionMatch::Mapping) return TextureBridgeCompareError::Mapping;
        if (status == RegionMatch::Different) return mismatch;
        return TextureBridgeCompareError::None;
    };
    const auto state_result = compare_region(prepared.state, true,
                                             TextureBridgeCompareError::State);
    if (state_result != TextureBridgeCompareError::None) return {state_result};
    const auto frame_result = compare_region(prepared.frame, true,
                                             TextureBridgeCompareError::Frame);
    if (frame_result != TextureBridgeCompareError::None) return {frame_result};
    const auto output_result = compare_region(prepared.output, true,
                                              TextureBridgeCompareError::Commands);
    if (output_result != TextureBridgeCompareError::None) return {output_result};
    const auto source_result = compare_region(prepared.source, false,
                                              TextureBridgeCompareError::Source);
    if (source_result != TextureBridgeCompareError::None) return {source_result};
    for (const auto *dependency : {&prepared.builder, &prepared.helper,
                                   &prepared.table, &prepared.global}) {
        const auto result = compare_region(*dependency, false,
                                           TextureBridgeCompareError::Dependency);
        if (result != TextureBridgeCompareError::None) return {result};
    }
    return {};
}

TextureBridgeResult apply_texture_commands(
    GuestMemory &memory, psprecomp::AllegrexContext &context,
    const TextureCommandBounds &bounds) {
    auto plan = prepare_texture_commands(memory, context, bounds);
    if (!plan.ok()) return {plan.error(), 0u};
    return commit_texture_commands(memory, context, plan);
}

} // namespace mhp3rd::native
