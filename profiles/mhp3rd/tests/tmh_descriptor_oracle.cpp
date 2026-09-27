#include "resources/tmh.hpp"
#include "native/bridge_contracts.hpp"
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
#include <sstream>
#include <stdexcept>

namespace {
using namespace psprecomp;
using namespace mhp3rd::resources;
constexpr std::uint32_t entry = 0x08876a10u, source_address = 0x09000000u,
    output_address = 0x08020000u, return_address = 0x08001000u;
constexpr std::size_t parent_limit = 256u * 1024u * 1024u;
constexpr std::size_t child_limit = 16u * 1024u * 1024u;
void require(bool value, const std::string &why) { if (!value) throw std::runtime_error(why); }

std::vector<std::uint8_t> read_parent(const std::filesystem::path &path, std::size_t expected_size,
                                    const std::string &expected_hash) {
    require(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path), "Parent is not a regular file");
    require(expected_size <= parent_limit && std::filesystem::file_size(path) == expected_size, "Parent size differs or exceeds limit");
    std::vector<std::uint8_t> bytes(expected_size);
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(bool(stream) && stream.peek() == std::char_traits<char>::eof(), "Parent read failed or grew");
    require(sha256_bytes(bytes) == expected_hash, "Parent hash differs");
    return bytes;
}

void store16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
}
void store32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8u * i));
}

class Oracle {
    Runtime aot_, interpreted_;
    std::mt19937 random_{0x544d4834u};
public:
    std::uint64_t calls{}, images_with_palette{}, images_without_palette{};
    unsigned max_slices{};
    explicit Oracle(const Elf32Image &elf) : aot_(elf.required_ram_size()), interpreted_(elf.required_ram_size()) {
        (void)elf.load_and_relocate(aot_.memory());
        (void)elf.load_and_relocate(interpreted_.memory());
        register_generated_functions(aot_);
        require(mhp3rd::native::matches_code_fingerprint<236>(aot_.memory(), entry,
            "2da1298722400450aba70d5585e98b9b2a2ca3c0dd2057089700aa9375a76874"), "TMH descriptor span differs");
    }
    void check(const TmhView &view) {
        const auto bytes = view.parent().bytes();
        require(bytes.size() <= child_limit, "TMH exceeds isolated guest fixture limit");
        aot_.memory().copy_in(source_address, bytes); interpreted_.memory().copy_in(source_address, bytes);
        for (std::size_t i = 0; i < view.records().size(); ++i) {
            const auto descriptor = view.descriptor(i);
            require(descriptor.image.bytes().data() == bytes.data() + descriptor.image_offset,
                    "Owned image reference differs from TMH-relative offset");
            if (descriptor.palette_offset)
                require(descriptor.palette && descriptor.palette->bytes().data() == bytes.data() + *descriptor.palette_offset,
                        "Owned palette reference differs from TMH-relative offset");
            std::array<std::uint8_t, 64> before{}, want{}, compiled_output{}, interpreted_output{};
            for (auto &value : before) value = static_cast<std::uint8_t>(random_());
            want = before;
            store32(want, 16u, source_address + descriptor.image_offset);
            store32(want, 20u, descriptor.image_format);
            store16(want, 24u, descriptor.width); store16(want, 26u, descriptor.height);
            store32(want, 28u, descriptor.palette_offset ? source_address + *descriptor.palette_offset : 0u);
            store32(want, 32u, descriptor.palette_format);
            store32(want, 36u, static_cast<std::uint32_t>(descriptor.palette_count));
            aot_.memory().copy_in(output_address, before); interpreted_.memory().copy_in(output_address, before);
            AllegrexContext initial{};
            for (auto &value : initial.gpr) value = random_();
            for (auto &value : initial.fpr) value = std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
            for (auto &value : initial.vfpu) value = std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
            for (auto &value : initial.vfpu_ctrl) value = random_();
            initial.hi = random_(); initial.lo = random_(); initial.fcr31 = random_();
            initial.gpr[0] = 0u; initial.gpr[5] = output_address + 16u;
            initial.gpr[6] = source_address; initial.gpr[7] = static_cast<std::uint32_t>(i);
            initial.gpr[8] = 0u; initial.gpr[31] = return_address; initial.pc = entry;
            auto compiled = initial, interpreted = initial;
            require(aot_.invoke_isolated_aot(entry, compiled) && !aot_.stopped() && compiled.pc == return_address,
                    "Original TMH descriptor did not return");
            unsigned slices = 0;
            while (interpreted.pc != return_address && slices++ < 8192u) {
                require(interpreted.pc >= entry && interpreted.pc < entry + 236u, "TMH interpreter left its span");
                require(interpret_allegrex(interpreted_, interpreted, 1u) == InterpreterExit::Budget &&
                        !interpreted_.stopped(), "TMH interpreter failed");
            }
            require(interpreted.pc == return_address, "TMH interpreter exceeded slice bound");
            max_slices = std::max(max_slices, slices);
            require(mhp3rd::native::same_context(compiled, interpreted), "Original AOT/interpreter CPU differs");
            aot_.memory().copy_out(output_address, compiled_output);
            interpreted_.memory().copy_out(output_address, interpreted_output);
            require(compiled_output == interpreted_output && compiled_output == want,
                    "Native TMH descriptor or output canaries differ from originals");
            ++calls;
            if (descriptor.palette) ++images_with_palette; else ++images_without_palette;
        }
        std::vector<std::uint8_t> after(bytes.size());
        aot_.memory().copy_out(source_address, after);
        require(std::equal(after.begin(), after.end(), bytes.begin()), "AOT changed TMH source bytes");
        interpreted_.memory().copy_out(source_address, after);
        require(std::equal(after.begin(), after.end(), bytes.begin()), "Interpreter changed TMH source bytes");
    }
};
}

int main(int argc, char **argv) {
    try {
        require(argc == 5, "usage: tmh_descriptor_oracle EBOOT.ELF raw-entries tmh-index.tsv new-report.json");
        require(sha256_file(argv[1]) == "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c", "Unsupported ELF");
        require(!std::filesystem::exists(argv[4]), "Report already exists");
        Oracle oracle(Elf32Image::from_file(argv[1]));
        std::ifstream index(argv[3]); require(bool(index), "Missing TMH index");
        std::ofstream out(argv[4]); require(bool(out), "Cannot create report");
        out << "{\"schema_version\":1,\"scope\":\"encoded_TMH_descriptors_not_pixels_or_gameplay\",\"inputs\":[";
        std::uint64_t rows = 0, id{}, parent_size{}, offset{}, length{};
        std::string line, parent_hash, child_hash, child_id;
        while (std::getline(index, line)) {
            std::istringstream row(line); std::string extra;
            require(bool(row >> id >> parent_size >> parent_hash >> offset >> length >> child_hash >> child_id) && !(row >> extra), "Malformed TMH index row");
            require(rows < 50000u && id < 100000u && parent_size <= parent_limit && offset <= parent_size && length <= parent_size - offset,
                    "TMH source bounds or input count exceed limits");
            require(child_id.find_first_not_of("0123456789.root") == std::string::npos, "Unsafe child identity");
            std::ostringstream name; name << std::setw(5) << std::setfill('0') << id << ".bin";
            auto parent = SharedBytes::take(read_parent(std::filesystem::path(argv[2]) / name.str(), parent_size, parent_hash));
            auto child = parent.slice(offset, length);
            require(sha256_bytes(child.bytes()) == child_hash, "TMH child hash differs");
            const auto view = TmhView::parse(child);
            oracle.check(view);
            if (rows++) out << ',';
            out << "{\"entry_id\":" << id << ",\"child_id\":\"" << child_id << "\",\"child_sha256\":\"" << child_hash
                << "\",\"offset_in_decoded_entry\":" << offset << ",\"size\":" << length << ",\"records\":" << view.records().size()
                << ",\"consumed_bytes\":" << view.consumed_bytes() << ",\"unindexed_tail_length\":" << length - view.consumed_bytes() << '}';
        }
        require(index.eof() && rows, "TMH index incomplete or empty");
        out << "],\"input_count\":" << rows << ",\"descriptor_calls_per_oracle\":" << oracle.calls
            << ",\"with_palette\":" << oracle.images_with_palette << ",\"without_palette\":" << oracle.images_without_palette
            << ",\"max_interpreter_slices\":" << oracle.max_slices << ",\"success\":true}\n";
        out.close(); require(bool(out), "Report write failed");
        std::cout << "TMH native/original descriptor gate: " << rows << " inputs, " << oracle.calls << " records passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
