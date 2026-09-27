#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>

// Standalone, bounded contract probe. It deliberately does not register game
// functions, call Runtime::run, or contain executable bytes from the game.
namespace {
using psprecomp::AllegrexContext;
using psprecomp::GuestMemory;
using psprecomp::Runtime;

struct Leaf {
    const char *name;
    std::uint32_t entry;
    std::uint32_t size;
    const char *hash;
    bool distance;
    bool square_root;
};

constexpr std::array<Leaf, 4> leaves{{
    {"norm", 0x08877244u, 32u, "1b1da8d38edcbeb7eb486e12977c6a667fa699639b6ef8d8d7d3f664219b12a0", false, true},
    {"norm_squared", 0x08877264u, 28u, "1fa5cdbdd2f96479f4cecb659eb7f68dd95a82ef853f99d7a299dc707c241cc1", false, false},
    {"distance", 0x08877280u, 40u, "977da7d41ecfd722f43c497c0f4627bb4faa7d57126d1cefa8f92138115b02a1", true, true},
    {"distance_squared", 0x088772A8u, 36u, "05771b861950457e9e065384220c69cc255874d63fe09e62face44defea00d36", true, false},
}};

constexpr std::uint32_t base = 0x08010000u;
constexpr std::size_t window_size = 512u;
constexpr std::array<std::uint32_t, 4> aliases{0u, 0x40000000u, 0x80000000u, 0xa0000000u};
constexpr std::array<std::uint32_t, 13> edges{
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
    0x007fffffu, 0x00800000u, 0x3f800000u, 0xbf800000u,
    0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc01234u, 0x7fa05678u};

std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
float value(std::uint32_t raw) { return std::bit_cast<float>(raw); }
std::string hex(std::uint32_t raw) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(8) << raw;
    return out.str();
}
void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}
std::uint32_t read32(const std::array<std::uint8_t, window_size> &bytes, std::size_t offset) {
    std::uint32_t raw = 0;
    for (unsigned i = 0; i < 4; ++i) raw |= std::uint32_t(bytes.at(offset + i)) << (8u * i);
    return raw;
}
void write32(std::array<std::uint8_t, window_size> &bytes, std::size_t offset, std::uint32_t raw) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(raw >> (8u * i));
}

void random_cpu(AllegrexContext &cpu, std::mt19937 &random, const Leaf &leaf) {
    for (auto &reg : cpu.gpr) reg = random();
    for (auto &reg : cpu.fpr) reg = value(random());
    for (auto &reg : cpu.vfpu) reg = value(random());
    for (auto &reg : cpu.vfpu_ctrl) reg = random();
    cpu.gpr[0] = 0u;
    cpu.hi = random();
    cpu.lo = random();
    cpu.fcr31 = random();
    cpu.pc = leaf.entry;
}

unsigned execute_leaf(Runtime &runtime, AllegrexContext &cpu, const Leaf &leaf) {
    const auto return_pc = cpu.gpr[31];
    for (unsigned slice = 0; slice < 12u; ++slice) {
        require(cpu.pc >= leaf.entry && cpu.pc < leaf.entry + leaf.size,
                std::string(leaf.name) + ": left certified span before return");
        const bool returning = cpu.pc == leaf.entry + leaf.size - 8u;
        const auto exit = psprecomp::interpret_allegrex(runtime, cpu, 1u);
        require(exit == psprecomp::InterpreterExit::Budget && !runtime.stopped(),
                std::string(leaf.name) + ": bounded interpreter failed");
        if (returning) {
            require(cpu.pc == return_pc, std::string(leaf.name) + ": return or delay slot mismatch");
            return slice + 1u;
        }
    }
    throw std::runtime_error(std::string(leaf.name) + ": instruction bound exceeded");
}

struct Counts {
    std::uint64_t cases{};
    std::uint64_t standard{};
    std::uint64_t prefixes{};
    std::uint64_t overlaps{};
    std::uint64_t unaligned{};
    std::uint64_t aliases{};
    std::uint64_t internal_return_cases{};
    std::uint64_t finite_model_checked{};
    std::uint64_t finite_model_mismatches{};
    std::uint64_t fused_model_mismatches{};
    std::uint64_t changed_vfpu[128]{};
    std::uint64_t changed_fpr[32]{};
    std::uint64_t changed_ctrl[16]{};
    unsigned max_slices{};
    std::array<std::uint32_t, 13> edge_outputs{};
    std::array<std::array<std::uint32_t, 2>, 4> prefix_comparisons{};
};

// Independent scalar calculation for ordinary finite values with standard
// prefixes. This cross-check is deliberately narrower than the state oracle.
std::uint32_t finite_model(const Leaf &leaf,
                           const std::array<std::uint8_t, window_size> &before,
                           std::size_t a, std::size_t b, bool fused) {
    float sum = 0.0f;
    for (unsigned lane = 0; lane < 3; ++lane) {
        float element = value(read32(before, a + 4u * lane));
        if (leaf.distance) element -= value(read32(before, b + 4u * lane));
        sum = fused ? std::fma(element, element, sum) : sum + element * element;
    }
    if (leaf.square_root) sum = std::fabs(std::sqrt(sum));
    return bits(sum);
}

std::uint32_t one_case(Runtime &runtime, std::mt19937 &random, const Leaf &leaf,
              Counts &count, std::size_t a, std::size_t b, std::size_t scratch,
              unsigned alias_a, unsigned alias_b, unsigned alias_stack,
              std::uint32_t ps, std::uint32_t pt, std::uint32_t pd,
              const std::array<std::uint32_t, 4> &a_bits,
              const std::array<std::uint32_t, 4> &b_bits,
              bool check_finite, int edge_index = -1) {
    require(a + 16u <= window_size && b + 16u <= window_size && scratch + 4u <= window_size,
            "probe fixture exceeds window");
    std::array<std::uint8_t, window_size> before{}, after{};
    for (auto &byte : before) byte = static_cast<std::uint8_t>(random());
    for (unsigned lane = 0; lane < 4; ++lane) write32(before, a + lane * 4u, a_bits[lane]);
    if (leaf.distance)
        for (unsigned lane = 0; lane < 4; ++lane) write32(before, b + lane * 4u, b_bits[lane]);
    runtime.memory().copy_in(base, before);

    AllegrexContext cpu{};
    random_cpu(cpu, random, leaf);
    cpu.gpr[4] = (base + static_cast<std::uint32_t>(a)) | aliases[alias_a];
    cpu.gpr[5] = (base + static_cast<std::uint32_t>(b)) | aliases[alias_b];
    cpu.gpr[29] = (base + static_cast<std::uint32_t>(scratch) + 16u) | aliases[alias_stack];
    const bool internal_return = count.cases % 97u == 0u;
    cpu.gpr[31] = internal_return ? leaf.entry : 0x08020000u;
    cpu.vfpu_ctrl[0] = ps;
    cpu.vfpu_ctrl[1] = pt;
    cpu.vfpu_ctrl[2] = pd;
    const AllegrexContext initial = cpu;
    const unsigned slices = execute_leaf(runtime, cpu, leaf);
    count.max_slices = std::max(count.max_slices, slices);
    runtime.memory().copy_out(base, after);

    require(cpu.pc == initial.gpr[31] && cpu.gpr == initial.gpr &&
            cpu.hi == initial.hi && cpu.lo == initial.lo && cpu.fcr31 == initial.fcr31,
            std::string(leaf.name) + ": non-output CPU field changed");
    for (unsigned i = 0; i < 32; ++i) {
        if (bits(cpu.fpr[i]) != bits(initial.fpr[i])) ++count.changed_fpr[i];
        if (i != 0u) require(bits(cpu.fpr[i]) == bits(initial.fpr[i]),
                             std::string(leaf.name) + ": unexpected FPR change");
    }
    for (unsigned i = 0; i < 128; ++i) {
        if (bits(cpu.vfpu[i]) != bits(initial.vfpu[i])) ++count.changed_vfpu[i];
        if (i >= (leaf.distance ? 8u : 5u))
            require(bits(cpu.vfpu[i]) == bits(initial.vfpu[i]),
                    std::string(leaf.name) + ": unexpected VFPU change");
    }
    if (!leaf.distance) {
        for (unsigned lane = 0; lane < 4u; ++lane)
            require(bits(cpu.vfpu[lane]) == read32(before, a + lane * 4u),
                    std::string(leaf.name) + ": source scratch lane differs from loaded word");
    } else {
        require(bits(cpu.vfpu[3]) == read32(before, a + 12u),
                std::string(leaf.name) + ": source fourth scratch lane differs from loaded word");
        for (unsigned lane = 1; lane < 4u; ++lane)
            require(bits(cpu.vfpu[4u + lane]) == read32(before, b + lane * 4u),
                    std::string(leaf.name) + ": second-source scratch lane differs from loaded word");
    }
    for (unsigned i = 0; i < 16; ++i) {
        if (cpu.vfpu_ctrl[i] != initial.vfpu_ctrl[i]) ++count.changed_ctrl[i];
        require(cpu.vfpu_ctrl[i] == (i == 0u || i == 1u ? 0xE4u : i == 2u ? 0u : initial.vfpu_ctrl[i]),
                std::string(leaf.name) + ": unexpected VFPU control change");
    }
    require(bits(cpu.fpr[0]) == read32(after, scratch) &&
            bits(cpu.vfpu[4]) == read32(after, scratch),
            std::string(leaf.name) + ": FPR0, VFPU scalar and scratch disagree");
    for (std::size_t i = 0; i < window_size; ++i)
        if (i < scratch || i >= scratch + 4u)
            require(after[i] == before[i], std::string(leaf.name) + ": memory canary changed");

    if (check_finite) {
        ++count.finite_model_checked;
        const auto expected = finite_model(leaf, before, a, b, false);
        const auto fused = finite_model(leaf, before, a, b, true);
        if (bits(cpu.fpr[0]) != expected) ++count.finite_model_mismatches;
        if (bits(cpu.fpr[0]) != fused) ++count.fused_model_mismatches;
    }
    if (edge_index >= 0) count.edge_outputs[static_cast<std::size_t>(edge_index)] = bits(cpu.fpr[0]);
    ++count.cases;
    if (ps == 0xE4u && pt == 0xE4u && pd == 0u) ++count.standard;
    else ++count.prefixes;
    if ((a < scratch + 4u && scratch < a + 16u) ||
        (leaf.distance && b < scratch + 4u && scratch < b + 16u)) ++count.overlaps;
    if ((a | b | scratch) & 3u) ++count.unaligned;
    if (alias_a || alias_b || alias_stack) ++count.aliases;
    if (internal_return) ++count.internal_return_cases;
    return bits(cpu.fpr[0]);
}

Counts probe_leaf(Runtime &runtime, const Leaf &leaf) {
    std::mt19937 random(0x56454331u ^ leaf.entry);
    Counts count{};
    constexpr std::array<std::uint32_t, 7> prefix_s{
        0xE4u, 0x1E4u, 0x1F000u, 0xF0E4u, 0x100E4u, 0x0001Bu, 0x10000u};
    constexpr std::array<std::uint32_t, 6> prefix_d{
        0u, 0x100u, 0x700u, 1u, 3u, 0x201u};

    // One reproducible raw-bit example per edge class, with fourth words set
    // independently so their VFPU load effects are visible.
    for (unsigned e = 0; e < edges.size(); ++e) {
        const std::array<std::uint32_t, 4> a{edges[e], 0u, 0u, edges[(e + 5u) % edges.size()]};
        const std::array<std::uint32_t, 4> b{0u, 0u, 0u, edges[(e + 7u) % edges.size()]};
        one_case(runtime, random, leaf, count, 64u, 112u, 320u, 0u, 0u, 0u,
                 0xE4u, 0xE4u, 0u, a, b, false, static_cast<int>(e));
    }

    // Controlled finite inputs include different scales and cancellation.
    std::uniform_real_distribution<float> finite(-1024.0f, 1024.0f);
    for (unsigned i = 0; i < 2048u; ++i) {
        std::array<std::uint32_t, 4> a{}, b{};
        for (unsigned lane = 0; lane < 4; ++lane) {
            a[lane] = bits(finite(random));
            b[lane] = bits(finite(random));
        }
        one_case(runtime, random, leaf, count, 48u + i % 4u, 120u + (i / 4u) % 4u,
                 320u + i % 4u, i % 4u, (i / 4u) % 4u, (i / 16u) % 4u,
                 0xE4u, 0xE4u, 0u, a, b, true);
    }

    // Prefixes exercise swizzle, constants, negate, saturation and masks.
    for (unsigned i = 0; i < 756u; ++i) {
        std::array<std::uint32_t, 4> a{}, b{};
        for (unsigned lane = 0; lane < 4; ++lane) {
            a[lane] = edges[random() % edges.size()];
            b[lane] = edges[random() % edges.size()];
        }
        one_case(runtime, random, leaf, count, 64u + i % 4u, 112u + (i / 4u) % 4u,
                 320u, i % 4u, (i / 4u) % 4u, (i / 16u) % 4u,
                 prefix_s[i % prefix_s.size()], prefix_s[(i / 7u) % prefix_s.size()],
                 prefix_d[(i / 49u) % prefix_d.size()], a, b, false);
    }

    // The stack write may overlap a source, and the two sources may overlap
    // each other. Every load must precede the one scratch store.
    for (int displacement = -15; displacement <= 3; ++displacement) {
        for (unsigned variant = 0; variant < 4; ++variant) {
            const auto a = static_cast<std::size_t>(320 + displacement);
            const auto b = variant % 2u ? a : static_cast<std::size_t>(320 - displacement);
            const std::array<std::uint32_t, 4> x{
                edges[(variant + 2u) % edges.size()], edges[(variant + 3u) % edges.size()],
                edges[(variant + 4u) % edges.size()], edges[(variant + 5u) % edges.size()]};
            const std::array<std::uint32_t, 4> y{
                edges[(variant + 6u) % edges.size()], edges[(variant + 7u) % edges.size()],
                edges[(variant + 8u) % edges.size()], edges[(variant + 9u) % edges.size()]};
            one_case(runtime, random, leaf, count, a, b, 320u, variant, (variant + 1u) % 4u,
                     (variant + 2u) % 4u, 0xE4u, 0xE4u, 0u, x, y, false);
        }
    }

    // Hold input memory and initial CPU bits fixed while changing one prefix.
    // Fourth words are deliberately nonzero but cannot enter a standard .t dot.
    const std::array<std::uint32_t, 4> x{
        0xc0000000u, 0x40400000u, 0x40800000u, 0x41100000u};
    const std::array<std::uint32_t, 4> y{
        0x3f800000u, 0x40000000u, 0u, 0x41300000u};
    constexpr std::array<std::array<std::uint32_t, 3>, 4> variants{{
        {0x1E4u, 0xE4u, 0u}, // S absolute on lane zero
        {0xE4u, 0xE0u, 0u}, // T lane-one swizzle
        {0xE4u, 0xE4u, 1u}, // D positive saturation on lane zero
        {0xE4u, 0xE4u, 0x100u}, // D mask on lane zero
    }};
    for (unsigned i = 0; i < variants.size(); ++i) {
        auto first_random = random;
        auto second_random = random;
        count.prefix_comparisons[i][0] = one_case(
            runtime, first_random, leaf, count, 64u, 112u, 320u, 0u, 0u, 0u,
            0xE4u, 0xE4u, 0u, x, y, false);
        count.prefix_comparisons[i][1] = one_case(
            runtime, second_random, leaf, count, 64u, 112u, 320u, 0u, 0u, 0u,
            variants[i][0], variants[i][1], variants[i][2], x, y, false);
        random = second_random;
    }
    return count;
}

void write_counts(const Counts &count) {
    std::cout << "\"cases\":" << count.cases
              << ",\"standard\":" << count.standard
              << ",\"prefixes\":" << count.prefixes
              << ",\"stack_source_overlaps\":" << count.overlaps
              << ",\"unaligned\":" << count.unaligned
              << ",\"aliased_addresses\":" << count.aliases
              << ",\"internal_return_cases\":" << count.internal_return_cases
              << ",\"finite_model_checked\":" << count.finite_model_checked
              << ",\"finite_scalar_model_mismatches\":" << count.finite_model_mismatches
              << ",\"fused_scalar_model_mismatches\":" << count.fused_model_mismatches
              << ",\"max_interpreter_slices\":" << count.max_slices;
    const auto write_changes = [](const char *name, const auto &changes) {
        std::cout << ",\"" << name << "\":[";
        bool first = true;
        for (unsigned i = 0; i < std::size(changes); ++i) {
            if (!changes[i]) continue;
            if (!first) std::cout << ',';
            std::cout << i;
            first = false;
        }
        std::cout << ']';
    };
    write_changes("changed_fpr_indices", count.changed_fpr);
    write_changes("changed_vfpu_indices", count.changed_vfpu);
    write_changes("changed_vfpu_control_indices", count.changed_ctrl);
    std::cout << ",\"edge_outputs\":[";
    for (unsigned i = 0; i < edges.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << "{\"input\":\"" << hex(edges[i]) << "\",\"output\":\""
                  << hex(count.edge_outputs[i]) << "\"}";
    }
    std::cout << ']';
    constexpr std::array<const char *, 4> names{
        "S_abs_lane0", "T_swizzle_lane1", "D_saturate_lane0", "D_mask_lane0"};
    std::cout << ",\"prefix_examples\":[";
    for (unsigned i = 0; i < names.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << "{\"change\":\"" << names[i] << "\",\"standard\":\""
                  << hex(count.prefix_comparisons[i][0]) << "\",\"prefixed\":\""
                  << hex(count.prefix_comparisons[i][1]) << "\"}";
    }
    std::cout << ']';
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: vector_metrics_contract_probe local-EBOOT.ELF");
        const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
        constexpr const char *supported_elf_hash =
            "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";
        const auto elf_hash = psprecomp::sha256_file(argv[1]);
        require(elf_hash == supported_elf_hash, "unsupported ELF identity");
        Runtime runtime(elf.required_ram_size());
        (void)elf.load_and_relocate(runtime.memory());
        std::cout << "{\"schema_version\":1,\"oracle\":\"bounded original-instruction interpreter\","
                  << "\"probe_compiler\":\"" << __VERSION__ << "\","
#if defined(__APPLE__) && defined(__aarch64__)
                  << "\"probe_platform\":\"macOS arm64\","
#else
                  << "\"probe_platform\":\"other\","
#endif
                  << "\"elf_sha256\":\"" << elf_hash << "\",\"leaves\":[";
        for (unsigned index = 0; index < leaves.size(); ++index) {
            const Leaf &leaf = leaves[index];
            std::array<std::uint8_t, 40> span{};
            runtime.memory().copy_out(leaf.entry, std::span<std::uint8_t>(span).first(leaf.size));
            const auto hash = psprecomp::sha256_bytes(std::span<const std::uint8_t>(span).first(leaf.size));
            require(hash == leaf.hash, std::string(leaf.name) + ": span hash mismatch");
            const Counts count = probe_leaf(runtime, leaf);
            if (index) std::cout << ',';
            std::cout << "{\"name\":\"" << leaf.name << "\",\"entry\":\""
                      << hex(leaf.entry) << "\",\"size\":" << leaf.size
                      << ",\"span_sha256\":\"" << hash << "\",";
            write_counts(count);
            std::cout << '}';
        }
        std::cout << "],\"limitations\":[\"Host software interpreter is not physical PSP arithmetic\","
                  << "\"No full-game launch or dynamic caller coverage\"]}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "vector metric contract probe: " << error.what() << '\n';
        return 1;
    }
}
