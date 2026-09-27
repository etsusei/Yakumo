#include "resources/indexed_bundle.hpp"
#include "native/bridge_contracts.hpp"
#include "psprecomp/elf32.hpp"
#include "recomp_units.hpp"

#include <algorithm>
#include <array>
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
constexpr std::uint32_t count_entry = 0x088661bcu, pointer_entry = 0x088661c8u,
    length_entry = 0x08866234u, base = 0x08020000u, stack = 0x08050000u,
    return_pc = 0x08001000u;
constexpr std::size_t file_limit = 256u * 1024u * 1024u;
void require(bool condition, const std::string &why) { if (!condition) throw std::runtime_error(why); }

std::vector<std::uint8_t> read_file(const std::filesystem::path &path, std::uint64_t expected_size) {
    require(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path), "Not a regular source file");
    require(expected_size <= file_limit && std::filesystem::file_size(path) == expected_size, "Source size differs or exceeds limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(expected_size));
    std::ifstream file(path, std::ios::binary);
    if (!bytes.empty()) file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(bool(file) && file.peek() == std::char_traits<char>::eof(), "Source read failed or grew");
    return bytes;
}

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8u) |
        (std::uint32_t(bytes[offset + 2]) << 16u) | (std::uint32_t(bytes[offset + 3]) << 24u);
}

class Oracle {
    Runtime aot_, interpreter_;
    std::mt19937 random_{0x41535354u};
public:
    std::uint64_t calls{};
    explicit Oracle(const Elf32Image &elf) : aot_(elf.required_ram_size()), interpreter_(elf.required_ram_size()) {
        (void)elf.load_and_relocate(aot_.memory());
        (void)elf.load_and_relocate(interpreter_.memory());
        register_generated_functions(aot_);
        require(mhp3rd::native::matches_code_fingerprint<12>(aot_.memory(), count_entry,
            "4957f5372e34b70d76a56a232f14cdfbde36664457628f8b757aac861b817110"), "Count consumer differs");
        require(mhp3rd::native::matches_code_fingerprint<108>(aot_.memory(), pointer_entry,
            "eb001e25ea896978ea3ce1a330e39005bc9db9e4d83ac44781bd86cf755957a8"), "Pointer consumer differs");
        require(mhp3rd::native::matches_code_fingerprint<88>(aot_.memory(), length_entry,
            "92fb871ca96bc51b757fbd85e212a212c0306516df2b481de7e6f2419f05306a"), "Length consumer differs");
    }
    void query(std::uint32_t entry, std::uint32_t index, std::uint32_t expected) {
        AllegrexContext initial{};
        for (auto &v : initial.gpr) v = random_();
        for (auto &v : initial.fpr) v = std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        for (auto &v : initial.vfpu) v = std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        for (auto &v : initial.vfpu_ctrl) v = random_();
        initial.gpr[0] = 0;
        initial.gpr[4] = base;
        initial.gpr[5] = index;
        initial.gpr[29] = stack + 32u;
        initial.gpr[31] = return_pc;
        initial.pc = entry;
        initial.hi = random_(); initial.lo = random_(); initial.fcr31 = random_();
        std::array<std::uint8_t, 64> before{}, aot_stack{}, interpreter_stack{};
        for (auto &b : before) b = static_cast<std::uint8_t>(random_());
        aot_.memory().copy_in(stack, before); interpreter_.memory().copy_in(stack, before);
        auto compiled = initial, interpreted = initial;
        require(aot_.invoke_isolated_aot(entry, compiled) && compiled.pc == return_pc && !aot_.stopped(), "AOT return failed");
        unsigned steps = 0;
        while (interpreted.pc != return_pc && steps++ < 64u) {
            require(interpreted.pc >= count_entry && interpreted.pc < 0x0886628cu, "Interpreter escaped consumer trio");
            require(interpret_allegrex(interpreter_, interpreted, 1u) == InterpreterExit::Budget &&
                    !interpreter_.stopped(), "Interpreter slice failed");
        }
        require(interpreted.pc == return_pc, "Interpreter instruction bound exceeded");
        require(compiled.gpr[2] == expected, "Portable metadata differs from original consumer");
        require(mhp3rd::native::same_context(compiled, interpreted), "Original AOT and interpreter CPU differ");
        aot_.memory().copy_out(stack, aot_stack); interpreter_.memory().copy_out(stack, interpreter_stack);
        require(aot_stack == interpreter_stack, "Original AOT and interpreter stack differ");
        for (std::size_t i = 0; i < before.size(); ++i)
            if (entry == count_entry || i < 16u || i >= 28u)
                require(before[i] == aot_stack[i], "Consumer wrote outside its saved-register words");
        ++calls;
    }
    void check(const IndexedBundle &bundle) {
        const auto bytes = bundle.parent().bytes();
        const auto n = static_cast<std::uint32_t>(bundle.entries().size());
        const auto header = bytes.first(4u + 8u * n);
        // Accessors only read the count/index. Payload addresses are compared
        // as values; no child decoder, game caller, or resource loader runs.
        aot_.memory().copy_in(base, header); interpreter_.memory().copy_in(base, header);
        query(count_entry, 0u, n);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto record = bundle.entries()[i];
            query(pointer_entry, i, record.present() ? base + record.offset : 0u);
            query(length_entry, i, record.length);
        }
        query(pointer_entry, n, 0u); query(length_entry, n, 0xffffffffu);
        std::vector<std::uint8_t> after(header.size());
        aot_.memory().copy_out(base, after); require(std::equal(after.begin(), after.end(), header.begin()), "AOT changed resource table");
        interpreter_.memory().copy_out(base, after); require(std::equal(after.begin(), after.end(), header.begin()), "Interpreter changed resource table");
    }
};

std::string marker(std::span<const std::uint8_t> b) {
    if (b.size() >= 4u && word(b, 0) == 0x006f6d70u) return "pmo_marker";
    if (b.size() >= 4u && word(b, 0) == 0x484d542eu) return "TMH_marker";
    return "unknown";
}

struct Totals { std::uint64_t candidates{}, children{}, absent{}, nested{}, signature_supported{}, weak{}, child_bytes{}; };
void describe(std::ostream &out, const IndexedBundle &bundle, Oracle &oracle, Totals &totals, unsigned depth) {
    require(totals.candidates < 10000u, "Candidate node budget exceeded");
    oracle.check(bundle);
    ++totals.candidates;
    if (depth) ++totals.nested;
    const auto bytes = bundle.parent().bytes();
    bool signature_supported = false;
    std::uint64_t indexed_end = 4u + bundle.entries().size() * 8u;
    out << "{\"count\":" << bundle.entries().size() << ",\"coordinate_space\":\"decoded_parent_bytes\",\"children\":[";
    for (std::size_t i = 0; i < bundle.entries().size(); ++i) {
        if (i) out << ',';
        const auto record = bundle.entries()[i];
        require(totals.children++ < 200000u, "Child slot budget exceeded");
        out << "{\"index\":" << i << ",\"offset\":" << record.offset << ",\"advertised_length\":" << record.length;
        const auto child = bundle.child(i);
        if (!child) { ++totals.absent; out << ",\"status\":\"absent\"}"; continue; }
        const auto data = child->bytes();
        require(data.size() == record.length && data.data() == bytes.data() + record.offset,
                "Native child view differs from original index extent");
        require(data.size() <= (2ull << 30u) - totals.child_bytes, "Child hashing byte budget exceeded");
        totals.child_bytes += data.size();
        const auto kind = marker(data);
        signature_supported |= kind != "unknown";
        indexed_end = std::max(indexed_end, std::uint64_t(record.offset) + record.length);
        out << ",\"status\":\"present\",\"sha256\":\"" << sha256_bytes(data) << "\",\"annotation\":\"" << kind << '"';
        if (depth < 2u && data.size() >= 4u && word(data, 0) > 0u && word(data, 0) <= 4096u) {
            std::optional<IndexedBundle> nested;
            try { nested = IndexedBundle::parse(*child); } catch (const std::invalid_argument &) {}
            if (nested) { out << ",\"nested_candidate\":"; describe(out, *nested, oracle, totals, depth + 1u); }
        }
        out << '}';
    }
    out << "],\"indexed_extent_end\":" << indexed_end << ",\"unindexed_tail_length\":" << bytes.size() - indexed_end
        << ",\"classification\":\"" << (signature_supported ? "signature_supported_candidate" : "bounds_only_candidate") << "\"}";
    if (signature_supported) ++totals.signature_supported; else ++totals.weak;
}
}

int main(int argc, char **argv) {
    try {
        require(argc == 5, "usage: indexed_bundle_oracle EBOOT.ELF raw-entries manifest-index.tsv new-report.json");
        require(sha256_file(argv[1]) == "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c", "Unsupported ELF");
        require(!std::filesystem::exists(argv[4]), "Report already exists");
        const auto elf = Elf32Image::from_file(argv[1]);
        Oracle oracle(elf);
        std::ifstream index(argv[3]); require(bool(index), "Cannot read manifest index");
        std::ofstream out(argv[4]); require(bool(out), "Cannot create report");
        out << "{\"schema_version\":1,\"scope\":\"bounded_original_accessors_and_structural_candidates_not_gameplay_or_child_semantics\",\"entries\":[";
        Totals totals;
        std::uint64_t id{}, length{}, files{}; std::string hash, line;
        while (std::getline(index, line)) {
            std::istringstream row(line); std::string extra;
            require(bool(row >> id >> length >> hash) && !(row >> extra), "Truncated or surplus manifest row");
            require(id == files && files < 100000u && hash.size() == 64u &&
                    hash.find_first_not_of("0123456789abcdef") == std::string::npos, "Nonsequential or invalid manifest index");
            std::ostringstream name; name << std::setw(5) << std::setfill('0') << id << ".bin";
            auto data = read_file(std::filesystem::path(argv[2]) / name.str(), length);
            require(sha256_bytes(data) == hash, "Raw resource hash differs from manifest");
            if (files++) out << ',';
            out << "{\"id\":" << id << ",\"size\":" << length << ",\"sha256\":\"" << hash << '"';
            std::optional<IndexedBundle> bundle;
            if (data.size() >= 4u && word(data, 0) > 0u && word(data, 0) <= 4096u) {
                try { bundle = IndexedBundle::parse(SharedBytes::take(std::move(data))); }
                catch (const std::invalid_argument &) {}
            }
            if (bundle) { out << ",\"candidate\":"; describe(out, *bundle, oracle, totals, 0u); }
            else out << ",\"status\":\"not_classified_as_indexed_bundle\"";
            out << '}';
        }
        require(index.eof() && files != 0u, "Malformed or empty manifest index");
        out << "],\"files_checked\":" << files << ",\"candidate_nodes\":" << totals.candidates
            << ",\"nested_nodes\":" << totals.nested << ",\"children\":" << totals.children
            << ",\"absent_slots\":" << totals.absent << ",\"signature_supported_nodes\":" << totals.signature_supported
            << ",\"bounds_only_nodes\":" << totals.weak << ",\"original_calls_per_oracle\":" << oracle.calls
            << ",\"success\":true}\n";
        out.close(); require(bool(out), "Report write failed");
        std::cout << "Checked " << files << " resources; " << totals.candidates << " candidate nodes, "
                  << totals.children << " slots; " << oracle.calls << " original calls per oracle\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
