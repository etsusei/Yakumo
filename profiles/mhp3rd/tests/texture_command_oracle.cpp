#include "resources/tmh.hpp"
#include "resources/texture_commands.hpp"
#include "native/bridge_contracts.hpp"
#include "native/texture_commands_bridge.hpp"
#include "psprecomp/elf32.hpp"
#include "recomp_units.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using namespace psprecomp;
using namespace mhp3rd::resources;
constexpr std::uint32_t kEntry = 0x0889E5C0u, kHelper = 0x08876A10u;
constexpr std::uint32_t kTable = 0x089CED28u, kGlobal = 0x08AB3668u;
constexpr std::uint32_t kSource = 0x09000000u, kObject = 0x08201000u;
constexpr std::uint32_t kCommands = 0x08220000u, kStack = 0x08310000u;
constexpr std::uint32_t kReturn = 0x08001000u;
constexpr std::size_t kParentLimit = 256u * 1024u * 1024u, kChildLimit = 16u * 1024u * 1024u;
constexpr std::string_view kElfHash = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";
void require(bool value, const std::string &why) { if (!value) throw std::runtime_error(why); }

std::uint32_t load32(std::span<const std::uint8_t> bytes, std::size_t at) {
    require(at <= bytes.size() && bytes.size() - at >= 4u, "Fixture word outside source");
    return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1u]) << 8u) |
        (std::uint32_t(bytes[at + 2u]) << 16u) | (std::uint32_t(bytes[at + 3u]) << 24u);
}
void store32(std::span<std::uint8_t> bytes, std::size_t at, std::uint32_t value) {
    require(at <= bytes.size() && bytes.size() - at >= 4u, "Fixture write outside region");
    for (unsigned i = 0; i < 4u; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}
std::uint32_t exponent(std::uint32_t value) { return value == 0u ? 0u : std::bit_width(value - 1u); }

// Independently expressed register-packet model from the original consumer
// contract. This is a test oracle, not a production guest ABI replacement.
std::array<std::uint32_t, 9> command_words(const TmhDescriptor &d, std::uint32_t source) {
    require(d.width <= 1024u && d.height <= 1024u, "Descriptor exceeds certified exponent table");
    const auto image = source + d.image_offset;
    const auto palette = d.palette_offset ? source + *d.palette_offset : 0u;
    return {0xC2000000u | (d.image_format >= 8u && d.image_format <= 10u ? 0u : 1u),
        0xC3000000u | d.image_format,
        0xA0000000u | (image & 0x00FFFFFFu),
        0xA8000000u | ((image >> 8u) & 0x00FF0000u) | d.width,
        0xB8000000u | (exponent(d.height) << 8u) | exponent(d.width),
        0xC500FF00u | d.palette_format,
        0xB0000000u | (palette & 0x00FFFFFFu),
        0xB1000000u | ((palette >> 8u) & 0x00FF0000u),
        0xC4000000u | ((static_cast<std::uint32_t>(d.palette_count) + 7u) >> 3u)};
}

class Oracle {
    Runtime aot_, interpreted_, adapted_;
    std::mt19937 random_{0x434d4439u};
    std::array<std::uint8_t, 4100> table_{};
    std::vector<std::uint8_t> random_bytes(std::size_t count) {
        std::vector<std::uint8_t> result(count);
        for (auto &byte : result) byte = static_cast<std::uint8_t>(random_());
        return result;
    }
    void check_region(std::uint32_t address, std::span<const std::uint8_t> expected,
                      const char *name) {
        std::vector<std::uint8_t> got(expected.size());
        for (Runtime *runtime : {&aot_, &interpreted_, &adapted_}) {
            runtime->memory().copy_out(address, got);
            require(std::equal(got.begin(), got.end(), expected.begin()), std::string(name) + " differs or canary changed");
        }
    }
public:
    std::uint64_t calls{}, slots{};
    unsigned max_slices{};
    explicit Oracle(const Elf32Image &elf) : aot_(elf.required_ram_size()), interpreted_(elf.required_ram_size()),
        adapted_(elf.required_ram_size()) {
        (void)elf.load_and_relocate(aot_.memory()); (void)elf.load_and_relocate(interpreted_.memory());
        (void)elf.load_and_relocate(adapted_.memory());
        register_generated_functions(aot_);
        require(mhp3rd::native::matches_code_fingerprint<528>(aot_.memory(), kEntry,
            "c4f5b737eb673e30b7d21425ef67ed9d2c3bab11228aadcbb389e25f1519694b"), "Builder code span differs");
        require(mhp3rd::native::matches_code_fingerprint<236>(aot_.memory(), kHelper,
            "2da1298722400450aba70d5585e98b9b2a2ca3c0dd2057089700aa9375a76874"), "Descriptor helper span differs");
        aot_.memory().copy_out(kTable, table_);
        require(sha256_bytes(table_) == "c9a6501e384b7f7670e8c5c48c90f1b70b5d981dd6fc7fc37a0b7c5d566bf849",
                "Exponent table differs");
        for (std::uint32_t i = 0; i <= 1024u; ++i)
            require(load32(table_, i * 4u) == exponent(i), "Exponent table semantics differ");
    }

    std::string check(SharedBytes source, std::uint32_t destination_slot,
                      std::uint32_t first_record, std::uint32_t override_count, bool mirrored,
                      const std::vector<TmhDescriptor> *synthetic_descriptors = nullptr) {
        const auto bytes = source.bytes();
        require(bytes.size() >= 16u && bytes.size() <= kChildLimit, "Source outside fixture budget");
        const auto declared = load32(bytes, 8u);
        const auto effective = override_count != 0u ? override_count : declared;
        const std::uint32_t iterations = (effective & 0x80000000u) != 0u ? 0u : effective;
        require(iterations <= 4096u, "Unbounded builder iteration request");
        const auto first = first_record & 255u, destination = destination_slot & 255u;
        std::optional<TmhView> view;
        if (iterations) {
            if (!synthetic_descriptors) view = TmhView::parse(source);
            const auto size = synthetic_descriptors ? synthetic_descriptors->size() : view->records().size();
            require(first <= size && iterations <= size - first,
                    "Builder would walk beyond known source records");
        }
        const std::uint32_t mirror = mirrored ? 0x40000000u : 0u;
        const auto raw_source = kSource | mirror, object = (kObject + 32u) | mirror;
        const auto commands = (kCommands + 64u) | mirror, sp = (kStack + 128u) | mirror;
        std::vector<TextureCommandDescriptor> native_descriptors;
        if (iterations) {
            const auto count = synthetic_descriptors ? synthetic_descriptors->size() : view->records().size();
            native_descriptors.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto d = synthetic_descriptors ? synthetic_descriptors->at(i) : view->descriptor(i);
                native_descriptors.push_back({raw_source + d.image_offset, d.image_format, d.width, d.height,
                    d.palette_offset ? raw_source + *d.palette_offset : 0u, d.palette_format, d.palette_count});
            }
        }
        const TextureCommandRequest request{raw_source, commands, declared, first_record,
            destination_slot, override_count, std::size_t(destination + iterations + 2u)};
        const auto native = build_texture_commands(native_descriptors, request);
        require(native.ok() && native.patches.size() == iterations, "Portable command core rejected a certified input");
        require(native.state.source_token == raw_source && native.state.command_base_token == commands &&
                native.state.declared_count == declared && native.state.count_byte == static_cast<std::uint8_t>(declared),
                "Portable state metadata differs from the original contract");
        auto object_before = random_bytes(96u), stack_before = random_bytes(224u);
        auto commands_before = random_bytes(128u + std::size_t(destination + iterations + 2u) * 36u);
        auto object_expected = object_before, stack_expected = stack_before, commands_expected = commands_before;
        std::array<std::uint8_t, 4> global{};
        store32(global, 0u, random_());
        for (Runtime *runtime : {&aot_, &interpreted_, &adapted_}) {
            runtime->memory().copy_in(kSource, bytes);
            runtime->memory().copy_in(kObject, object_before);
            runtime->memory().copy_in(kCommands, commands_before);
            runtime->memory().copy_in(kStack, stack_before);
            runtime->memory().copy_in(kGlobal, global);
        }
        AllegrexContext initial{};
        for (auto &value : initial.gpr) value = random_();
        for (auto &value : initial.fpr) value = std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        for (auto &value : initial.vfpu) value = std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        for (auto &value : initial.vfpu_ctrl) value = random_();
        initial.hi = random_(); initial.lo = random_(); initial.fcr31 = random_();
        initial.gpr[0] = 0; initial.gpr[4] = object; initial.gpr[5] = commands; initial.gpr[6] = raw_source;
        initial.gpr[7] = destination_slot; initial.gpr[8] = first_record; initial.gpr[9] = override_count;
        initial.gpr[29] = sp; initial.gpr[31] = kReturn; initial.pc = kEntry;
        auto expected = initial;
        expected.pc = kReturn; expected.gpr[2] = declared; expected.gpr[3] = destination;
        constexpr std::array<unsigned, 10> saved{16, 17, 18, 19, 20, 21, 22, 23, 30, 31};
        for (std::size_t i = 0; i < saved.size(); ++i) store32(stack_expected, 80u + i * 4u, initial.gpr[saved[i]]);
        store32(object_expected, 32u, raw_source); store32(object_expected, 36u, commands);
        object_expected[40u] = static_cast<std::uint8_t>(declared);
        for (std::uint32_t i = 0; i < iterations; ++i) {
            const auto d = synthetic_descriptors ? synthetic_descriptors->at(first + i) : view->descriptor(first + i);
            const auto words = command_words(d, raw_source);
            require(native.patches[i].source_record == first + i &&
                    native.patches[i].destination_slot == destination + i && native.patches[i].words == words,
                    "Portable command patch differs from independent packet model");
            for (std::size_t j = 0; j < words.size(); ++j)
                store32(commands_expected, 64u + std::size_t(destination + i) * 36u + j * 4u, words[j]);
            if (i + 1u == iterations) {
                store32(stack_expected, 48u, raw_source + d.image_offset);
                store32(stack_expected, 52u, d.image_format);
                store32(stack_expected, 56u, std::uint32_t(d.width) | (std::uint32_t(d.height) << 16u));
                store32(stack_expected, 60u, d.palette_offset ? raw_source + *d.palette_offset : 0u);
                store32(stack_expected, 64u, d.palette_format);
                store32(stack_expected, 68u, static_cast<std::uint32_t>(d.palette_count));
                expected.gpr[3] = 0xC4000000u;
                expected.gpr[4] = words[7]; expected.gpr[5] = words[6];
                expected.gpr[6] = commands + (destination + i) * 36u;
                expected.gpr[7] = kTable + std::uint32_t(d.height) * 4u;
                expected.gpr[8] = kTable + std::uint32_t(d.width) * 4u;
                expected.gpr[9] = words[1]; expected.gpr[10] = words[2];
            }
        }
        auto adapted = initial;
        auto plan = mhp3rd::native::prepare_texture_commands(adapted_.memory(), adapted,
            {bytes.size(), request.destination_slots, 4096u});
        require(plan.ok() && plan.blocks() == iterations, "Command preparation rejected a certified input");
        require(mhp3rd::native::same_context(adapted, initial), "Preparation changed CPU");
        check_region(kObject, object_before, "Preparation object");
        check_region(kCommands, commands_before, "Preparation commands");
        check_region(kStack, stack_before, "Preparation stack");
        check_region(kSource, bytes, "Preparation source");
        check_region(kTable, table_, "Preparation table");
        check_region(kGlobal, global, "Preparation global");
        auto compiled = initial, interpreted = initial;
        require(aot_.invoke_isolated_aot(kEntry, compiled) && !aot_.stopped() && compiled.pc == kReturn,
                "Original compiled builder did not return");
        unsigned slices = 0;
        while (interpreted.pc != kReturn && slices++ < 2000000u) {
            const bool in_builder = interpreted.pc >= kEntry && interpreted.pc < kEntry + 528u;
            const bool in_helper = interpreted.pc >= kHelper && interpreted.pc < kHelper + 236u;
            require(in_builder || in_helper, "Original interpreter left certified builder/helper spans");
            require(interpret_allegrex(interpreted_, interpreted, 1u) == InterpreterExit::Budget &&
                    !interpreted_.stopped(), "Original builder interpreter failed");
        }
        require(interpreted.pc == kReturn, "Original builder exceeded instruction bound");
        max_slices = std::max(max_slices, slices);
        require(mhp3rd::native::same_context(compiled, interpreted), "Original AOT/interpreter CPU differs");
        if (!mhp3rd::native::same_context(compiled, expected)) {
            for (unsigned i = 0; i < 32u; ++i)
                if (compiled.gpr[i] != expected.gpr[i])
                    std::cerr << "GPR " << i << " got " << std::hex << compiled.gpr[i] << " want " << expected.gpr[i] << std::dec << '\n';
            throw std::runtime_error("Original CPU differs from independently modeled footprint");
        }
        require(mhp3rd::native::compare_texture_commands(plan, aot_.memory(), compiled).ok(),
                "Prepared plan differs from actual original AOT effects");
        require(mhp3rd::native::compare_texture_commands(plan, interpreted_.memory(), interpreted).ok(),
                "Prepared plan differs from original interpreter effects");
        const auto bridge = mhp3rd::native::commit_texture_commands(adapted_.memory(), adapted, plan);
        require(bridge.ok() && bridge.blocks == iterations,
                "Guest adapter rejected a certified input: " + std::to_string(static_cast<int>(bridge.error)));
        require(mhp3rd::native::same_context(adapted, compiled),
                "Actual guest adapter CPU differs from original AOT/interpreter");
        check_region(kObject, object_expected, "Object"); check_region(kCommands, commands_expected, "Command buffer");
        check_region(kStack, stack_expected, "Stack"); check_region(kSource, bytes, "Source");
        check_region(kTable, table_, "Table"); check_region(kGlobal, global, "Read-only global");
        ++calls; slots += iterations;
        return sha256_bytes(std::span<const std::uint8_t>(commands_expected).subspan(
            64u + std::size_t(destination) * 36u, std::size_t(iterations) * 36u));
    }
};

SharedBytes synthetic(std::size_t count, bool large = false) {
    std::vector<std::uint8_t> bytes(16u);
    const std::array<std::uint8_t, 8> marker{'.','T','M','H','0','.','1','4'};
    std::copy(marker.begin(), marker.end(), bytes.begin()); store32(bytes, 8u, static_cast<std::uint32_t>(count));
    constexpr std::array<std::array<std::uint16_t, 2>, 6> sizes{{{32,8},{68,68},{132,64},{160,168},{128,216},{1,1024}}};
    for (std::size_t i = 0; i < count; ++i) {
        auto [w,h] = sizes[i % sizes.size()];
        if (large) w = h = 1024u;
        const auto format = i % 3u == 2u ? 8u : (i % 3u == 1u || w == 1u ? 5u : 4u);
        const auto palette_format = i % 2u ? 3u : 1u;
        const auto palette_count = format == 4u ? 16u : (i % 5u == 0u ? 8448u : 256u);
        const auto image_size = format == 8u ? std::size_t((w + 3u) / 4u) * ((h + 3u) / 4u) * 8u :
            (std::size_t(w) * h * (format == 4u ? 4u : 8u) + 7u) / 8u;
        const auto palette_size = format == 8u ? 0u : palette_count * (palette_format == 3u ? 4u : 2u);
        const auto start = bytes.size(), record_size = 32u + image_size + (format == 8u ? 0u : 16u + palette_size);
        bytes.resize(start + record_size, 0x4bu);
        store32(bytes, start, static_cast<std::uint32_t>(record_size)); store32(bytes, start + 8u, 1u);
        store32(bytes, start + 16u, static_cast<std::uint32_t>(16u + image_size));
        store32(bytes, start + 24u, format); store32(bytes, start + 28u, std::uint32_t(w) | (std::uint32_t(h) << 16u));
        if (format != 8u) {
            const auto palette = start + 32u + image_size;
            store32(bytes, palette, 16u + palette_size); store32(bytes, palette + 8u, palette_format);
            store32(bytes, palette + 12u, palette_count);
        }
    }
    return SharedBytes::take(std::move(bytes));
}

void synthetic_gate(Oracle &oracle) {
    const auto empty = synthetic(0u), many = synthetic(258u), basic = synthetic(6u), big = synthetic(1u, true);
    (void)oracle.check(empty, 0xffffffffu, 0xffffffffu, 0u, false);
    for (bool mirror : {false, true}) {
        (void)oracle.check(basic, 0u, 0u, 0u, mirror);
        (void)oracle.check(basic, 0xdeadbeffu, 0xabcdef02u, 3u, mirror);
        (void)oracle.check(basic, 0xfffffffeu, 0xffffff05u, 1u, mirror);
        (void)oracle.check(basic, 7u, 255u, 0xffffffffu, mirror);
        (void)oracle.check(basic, 5u, 255u, 0x80000000u, mirror);
        (void)oracle.check(many, 255u, 255u, 3u, mirror);
        (void)oracle.check(big, 1u, 0u, 0u, mirror);
    }
    // An explicit override may select records beyond the declared header count.
    // Preserve descriptors from the unmodified fixture, then change only that
    // word; the original helper uses sizes rather than the header to walk.
    const auto basic_view = TmhView::parse(basic);
    std::vector<TmhDescriptor> beyond_declared;
    for (std::size_t i = 0; i < basic_view.records().size(); ++i)
        beyond_declared.push_back(basic_view.descriptor(i));
    std::vector<std::uint8_t> underdeclared(basic.bytes().begin(), basic.bytes().end());
    store32(underdeclared, 8u, 1u);
    const auto override_source = SharedBytes::take(std::move(underdeclared));
    for (bool mirror : {false, true})
        (void)oracle.check(override_source, 3u, 2u, 3u, mirror, &beyond_declared);

    // The palette walk is driven by the record's subblock count. Include a
    // second image subblock, so assuming palette == end(first image) fails.
    std::vector<std::uint8_t> multi(240u, 0u);
    store32(multi, 8u, 1u); store32(multi, 16u, 224u); store32(multi, 24u, 2u);
    store32(multi, 32u, 144u); store32(multi, 40u, 4u);
    store32(multi, 44u, 16u | (16u << 16u));
    store32(multi, 176u, 16u); store32(multi, 192u, 48u);
    store32(multi, 200u, 1u); store32(multi, 204u, 16u);
    const auto multi_source = SharedBytes::take(std::move(multi));
    const std::vector<TmhDescriptor> multi_descriptor{{48u, 4u, 16u, 16u, 208u,
        1u, 16, multi_source.slice(48u, 128u), multi_source.slice(208u, 32u)}};
    for (bool mirror : {false, true})
        (void)oracle.check(multi_source, 0u, 0u, 0u, mirror, &multi_descriptor);

    // A signed-negative header count takes the zero-command path without
    // parsing descriptors, but is still copied in full to r2 and low-byte state.
    std::vector<std::uint8_t> negative(empty.bytes().begin(), empty.bytes().end());
    store32(negative, 8u, 0x800001abu);
    (void)oracle.check(SharedBytes::take(std::move(negative)), 0x1111u, 0x2222u, 0u, true);
    // Exercise the remaining known format branches separately from the file
    // corpus. Their metadata is synthetic; this does not widen
    // the production TMH reader's established file-format domain.
    for (const std::uint32_t format : {0u, 1u, 2u, 3u, 6u, 7u, 9u, 10u}) {
        const bool has_palette = format == 0u || format == 2u || format == 6u || format == 7u;
        const std::size_t payload = format >= 9u ? 256u : (format == 3u || format == 7u ? 1024u : 512u);
        const std::uint32_t palette_format = format == 2u || format == 7u ? 2u : 0u;
        const std::uint32_t palette_count = 17u, palette_bytes = palette_count * 2u;
        std::vector<std::uint8_t> data(48u + payload + (has_palette ? 16u + palette_bytes : 0u), 0x57u);
        const std::array<std::uint8_t, 8> marker{'.','T','M','H','0','.','1','4'};
        std::copy(marker.begin(), marker.end(), data.begin()); store32(data, 8u, 1u);
        store32(data, 16u, static_cast<std::uint32_t>(data.size() - 16u));
        store32(data, 24u, 1u); store32(data, 32u, 16u + static_cast<std::uint32_t>(payload));
        store32(data, 40u, format); store32(data, 44u, 32u | (8u << 16u));
        const auto palette_offset = static_cast<std::uint32_t>(64u + payload);
        if (has_palette) {
            store32(data, palette_offset - 16u, 16u + palette_bytes);
            store32(data, palette_offset - 8u, palette_format);
            store32(data, palette_offset - 4u, palette_count);
        }
        auto source = SharedBytes::take(std::move(data));
        const std::vector<TmhDescriptor> expected{{48u, format, 32u, 8u,
            has_palette ? std::optional(palette_offset) : std::nullopt,
            has_palette ? palette_format : 0u, has_palette ? static_cast<std::int32_t>(palette_count) : 0,
            source.slice(48u, payload), has_palette ? std::optional(source.slice(palette_offset, palette_bytes)) : std::nullopt}};
        for (bool mirror : {false, true})
            (void)oracle.check(source, 0xaabb0001u, 0xeeff0000u, 0u, mirror, &expected);
    }
}

std::vector<std::uint8_t> read_parent(const std::filesystem::path &path, std::size_t size, const std::string &hash) {
    require(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path) &&
            size <= kParentLimit && std::filesystem::file_size(path) == size, "Parent file identity or size differs");
    std::vector<std::uint8_t> bytes(size);
    std::ifstream input(path, std::ios::binary); input.read(reinterpret_cast<char *>(bytes.data()), size);
    require(bool(input) && input.peek() == std::char_traits<char>::eof() && sha256_bytes(bytes) == hash, "Parent hash/read differs");
    return bytes;
}
} // namespace

int main(int argc, char **argv) {
    try {
        const bool synthetic_only = argc == 4 && std::string_view(argv[1]) == "--synthetic";
        require(synthetic_only || argc == 5, "usage: texture_command_oracle ELF raw-entries index.tsv new-report.json | --synthetic ELF new-report.json");
        const auto elf_path = argv[synthetic_only ? 2 : 1];
        const auto output_path = argv[synthetic_only ? 3 : 4];
        require(sha256_file(elf_path) == kElfHash, "Unsupported ELF");
        require(!std::filesystem::exists(output_path) && !std::filesystem::is_symlink(output_path), "Output already exists");
        Oracle oracle(Elf32Image::from_file(elf_path));
        std::ostringstream report;
        report << "{\"schema_version\":1,\"scope\":\"original_texture_command_builder_not_pixels\",\"inputs\":[";
        std::size_t inputs = 0u, descriptors = 0u;
        if (synthetic_only) synthetic_gate(oracle);
        else {
            std::ifstream index(argv[3]); require(bool(index), "Missing TMH index");
            std::string line; std::set<std::pair<std::uint64_t,std::string>> seen;
            while (std::getline(index, line)) {
                std::uint64_t id{}, parent_size{}, offset{}, length{};
                std::string parent_hash, child_hash, child_id, extra;
                std::istringstream row(line);
                require(bool(row >> id >> parent_size >> parent_hash >> offset >> length >> child_hash >> child_id) && !(row >> extra), "Malformed TMH index");
                require(id < 100000u && inputs < 50000u && parent_size <= kParentLimit && offset <= parent_size &&
                        length <= parent_size - offset && length <= kChildLimit, "Index bounds exceed budget");
                require(!child_id.empty() && child_id.find_first_not_of("0123456789.root") == child_id.npos &&
                        seen.insert({id, child_id}).second, "Invalid or duplicate child identity");
                std::ostringstream name; name << std::setw(5) << std::setfill('0') << id << ".bin";
                auto root = SharedBytes::take(read_parent(std::filesystem::path(argv[2]) / name.str(), parent_size, parent_hash));
                auto child = root.slice(offset, length); require(sha256_bytes(child.bytes()) == child_hash, "Child hash differs");
                const auto view = TmhView::parse(child); const auto count = view.records().size();
                const auto full = oracle.check(child, 0x12340000u | (inputs & 255u), 0xffffff00u, 0u, false);
                std::string partial;
                if (count) partial = oracle.check(child, 0xffff00ffu,
                    0xabcd0000u | static_cast<std::uint32_t>(std::min<std::size_t>(count - 1u, 255u)), 1u, true);
                if (inputs++) report << ',';
                report << "{\"entry_id\":" << id << ",\"child_id\":\"" << child_id << "\",\"child_sha256\":\"" << child_hash
                       << "\",\"offset_in_decoded_entry\":" << offset << ",\"size\":" << length << ",\"records\":" << count
                       << ",\"full_commands_sha256\":\"" << full << "\",\"partial_commands_sha256\":\"" << partial << "\"}";
                descriptors += count;
            }
            require(index.eof() && inputs, "Index incomplete or empty");
        }
        report << "],\"input_count\":" << inputs << ",\"descriptor_records\":" << descriptors
               << ",\"builder_calls\":" << oracle.calls << ",\"emitted_command_slots\":" << oracle.slots
               << ",\"portable_core_compared\":true,\"portable_calls\":" << oracle.calls
               << ",\"guest_adapter_compared\":true,\"adapter_calls\":" << oracle.calls
               << ",\"prepared_plan_compared\":true,\"plan_calls\":" << oracle.calls
               << ",\"max_interpreter_slices\":" << oracle.max_slices
               << ",\"synthetic_cases\":" << (synthetic_only ? oracle.calls : 0u) << ",\"success\":true}\n";
        std::ofstream output(output_path); require(bool(output), "Cannot create report"); output << report.str(); output.close();
        require(bool(output), "Report write failed");
        std::cout << "Texture command original gate: " << oracle.calls << " calls, " << oracle.slots << " command slots passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
