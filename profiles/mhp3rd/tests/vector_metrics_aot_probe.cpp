#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"
#include "recomp_units.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Offline comparison of the registered production AOT object and a bounded
// interpreter on identical CPU and memory. No generated instruction data is
// embedded here, and neither Runtime::run nor a game subsystem is invoked.
namespace {
using psprecomp::AllegrexContext;
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
constexpr std::uint32_t external_return = 0x08020000u;
constexpr std::size_t window_size = 512u;
constexpr std::array<std::uint32_t, 4> aliases{0u, 0x40000000u, 0x80000000u, 0xa0000000u};
constexpr std::array<std::uint32_t, 13> edges{
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
    0x007fffffu, 0x00800000u, 0x3f800000u, 0xbf800000u,
    0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc01234u, 0x7fa05678u};

using Window = std::array<std::uint8_t, window_size>;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
float value(std::uint32_t raw) { return std::bit_cast<float>(raw); }
bool nan_bits(std::uint32_t raw) {
    return (raw & 0x7f800000u) == 0x7f800000u && (raw & 0x007fffffu) != 0u;
}

std::string hex(std::uint32_t raw) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << raw;
    return out.str();
}

std::uint32_t read32(const Window &bytes, std::size_t offset) {
    std::uint32_t raw{};
    for (unsigned i = 0; i < 4u; ++i) raw |= std::uint32_t(bytes.at(offset + i)) << (8u * i);
    return raw;
}

void write32(Window &bytes, std::size_t offset, std::uint32_t raw) {
    for (unsigned i = 0; i < 4u; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(raw >> (8u * i));
}

AllegrexContext random_cpu(std::mt19937 &random, const Leaf &leaf) {
    AllegrexContext cpu{};
    for (auto &reg : cpu.gpr) reg = random();
    for (auto &reg : cpu.fpr) reg = value(random());
    for (auto &reg : cpu.vfpu) reg = value(random());
    for (auto &reg : cpu.vfpu_ctrl) reg = random();
    cpu.gpr[0] = 0u;
    cpu.gpr[31] = external_return;
    cpu.hi = random();
    cpu.lo = random();
    cpu.fcr31 = random();
    cpu.pc = leaf.entry;
    return cpu;
}

unsigned interpret_leaf(Runtime &runtime, AllegrexContext &cpu, const Leaf &leaf) {
    for (unsigned slice = 0; slice < 12u; ++slice) {
        require(cpu.pc >= leaf.entry && cpu.pc < leaf.entry + leaf.size,
                std::string(leaf.name) + ": interpreter left certified span");
        const bool returning = cpu.pc == leaf.entry + leaf.size - 8u;
        const auto exit = psprecomp::interpret_allegrex(runtime, cpu, 1u);
        require(exit == psprecomp::InterpreterExit::Budget && !runtime.stopped(),
                std::string(leaf.name) + ": bounded interpreter failed");
        if (returning) {
            require(cpu.pc == external_return, std::string(leaf.name) + ": interpreter return failed");
            return slice + 1u;
        }
    }
    throw std::runtime_error(std::string(leaf.name) + ": interpreter exceeded instruction bound");
}

struct Difference {
    std::string field;
    std::uint32_t expected{};
    std::uint32_t actual{};
};

std::vector<Difference> compare_cpu(const AllegrexContext &expected,
                                    const AllegrexContext &actual) {
    std::vector<Difference> differences;
    const auto add = [&](const std::string &field, std::uint32_t left, std::uint32_t right) {
        if (left != right) differences.push_back({field, left, right});
    };
    for (unsigned i = 0; i < 32u; ++i) add("gpr[" + std::to_string(i) + "]", expected.gpr[i], actual.gpr[i]);
    add("hi", expected.hi, actual.hi);
    add("lo", expected.lo, actual.lo);
    add("pc", expected.pc, actual.pc);
    for (unsigned i = 0; i < 32u; ++i) add("fpr[" + std::to_string(i) + "]", bits(expected.fpr[i]), bits(actual.fpr[i]));
    add("fcr31", expected.fcr31, actual.fcr31);
    for (unsigned i = 0; i < 128u; ++i) add("vfpu[" + std::to_string(i) + "]", bits(expected.vfpu[i]), bits(actual.vfpu[i]));
    for (unsigned i = 0; i < 16u; ++i) add("vfpu_ctrl[" + std::to_string(i) + "]", expected.vfpu_ctrl[i], actual.vfpu_ctrl[i]);
    return differences;
}

struct Models {
    std::uint32_t fma_from_zero{};
    std::uint32_t multiply_first_fma_tail{};
    std::uint32_t reversed_first_pair{};
    std::uint32_t unfused{};
};

// These separate scalar expressions only classify observed AOT output. They
// are not a PSP floating-point specification. This file is built with
// -ffp-contract=off so the unfused expression stays independent of std::fma.
Models scalar_models(const Leaf &leaf, const Window &before, std::size_t a, std::size_t b) {
    std::array<float, 3> lane{};
    for (unsigned i = 0; i < 3u; ++i) {
        lane[i] = value(read32(before, a + i * 4u));
        if (leaf.distance) lane[i] -= value(read32(before, b + i * 4u));
    }
    const auto finalize = [&](float result) {
        return bits(leaf.square_root ? std::fabs(std::sqrt(result)) : result);
    };
    float from_zero = 0.0f;
    for (float x : lane) from_zero = std::fma(x, x, from_zero);
    from_zero = std::fma(0.0f, 0.0f, from_zero);
    float first = lane[0] * lane[0];
    first = std::fma(lane[1], lane[1], first);
    first = std::fma(lane[2], lane[2], first);
    first = std::fma(0.0f, 0.0f, first);
    float reversed = lane[1] * lane[1];
    reversed = std::fma(lane[0], lane[0], reversed);
    reversed = std::fma(lane[2], lane[2], reversed);
    reversed = std::fma(0.0f, 0.0f, reversed);
    float separate = 0.0f;
    for (float x : lane) {
        volatile float product = x * x;
        separate += product;
    }
    separate += 0.0f * 0.0f;
    return {finalize(from_zero), finalize(first), finalize(reversed), finalize(separate)};
}

struct Example {
    std::string family;
    unsigned index{};
    std::string field;
    std::uint32_t interpreter{};
    std::uint32_t aot{};
    std::array<std::uint32_t, 4> source_a{};
    std::array<std::uint32_t, 4> source_b{};
    Models models{};
    bool model_applicable{};
    std::vector<std::string> all_cpu_fields;
    std::uint32_t memory_different_bytes{};
    std::uint32_t first_memory_offset{window_size};
};

struct FamilyCounts {
    std::uint64_t cases{};
    std::uint64_t cpu_mismatch{};
    std::uint64_t result_mismatch{};
    std::uint64_t memory_mismatch{};
    std::uint64_t reversed_model_matches{};
};

struct Counts {
    std::uint64_t cases{};
    std::uint64_t ordinary_finite{};
    std::uint64_t standard_model_checked{};
    std::uint64_t nonstandard_prefix{};
    std::uint64_t unaligned{};
    std::uint64_t aliases{};
    std::uint64_t stack_source_overlap{};
    std::uint64_t cpu_mismatch_cases{};
    std::uint64_t result_mismatch_cases{};
    std::uint64_t memory_mismatch_cases{};
    std::uint64_t unexpected_memory_writes{};
    std::uint64_t memory_different_bytes{};
    std::uint64_t nan_output_cases{};
    std::uint64_t nan_payload_mismatch_cases{};
    std::uint64_t nan_class_mismatch_cases{};
    std::uint64_t direct_wrapper_mismatch_cases{};
    std::uint64_t fma_from_zero_matches[2]{};
    std::uint64_t multiply_first_fma_tail_matches[2]{};
    std::uint64_t reversed_first_pair_matches[2]{};
    std::uint64_t unfused_matches[2]{};
    std::uint64_t standard_fma_from_zero_matches[2]{};
    std::uint64_t standard_multiply_first_matches[2]{};
    std::uint64_t standard_reversed_first_matches[2]{};
    std::uint64_t standard_unfused_matches[2]{};
    unsigned max_slices{};
    std::map<std::string, FamilyCounts> families;
    std::map<std::string, std::uint64_t> cpu_fields;
    std::map<std::string, std::uint64_t> wrapper_fields;
    std::vector<Example> first_differences;
};

void run_case(Runtime &interpreter, Runtime &aot, std::mt19937 &random,
              const Leaf &leaf, Counts &counts, const std::string &family, unsigned index,
              std::size_t a, std::size_t b, std::size_t scratch,
              unsigned alias_a, unsigned alias_b, unsigned alias_stack,
              std::uint32_t ps, std::uint32_t pt, std::uint32_t pd,
              const std::array<std::uint32_t, 4> &a_bits,
              const std::array<std::uint32_t, 4> &b_bits,
              bool finite_model) {
    require(a + 16u <= window_size && b + 16u <= window_size && scratch + 4u <= window_size,
            "fixture exceeds 512-byte window");
    Window before{}, interpreted{}, compiled{}, direct_window{};
    for (auto &byte : before) byte = static_cast<std::uint8_t>(random());
    for (unsigned lane = 0; lane < 4u; ++lane) write32(before, a + lane * 4u, a_bits[lane]);
    if (leaf.distance)
        for (unsigned lane = 0; lane < 4u; ++lane) write32(before, b + lane * 4u, b_bits[lane]);
    interpreter.memory().copy_in(base, before);
    aot.memory().copy_in(base, before);

    auto initial = random_cpu(random, leaf);
    initial.gpr[4] = (base + static_cast<std::uint32_t>(a)) | aliases.at(alias_a);
    initial.gpr[5] = (base + static_cast<std::uint32_t>(b)) | aliases.at(alias_b);
    initial.gpr[29] = (base + static_cast<std::uint32_t>(scratch) + 16u) | aliases.at(alias_stack);
    initial.vfpu_ctrl[0] = ps;
    initial.vfpu_ctrl[1] = pt;
    initial.vfpu_ctrl[2] = pd;
    auto expected = initial;
    auto actual = initial;
    counts.max_slices = std::max(counts.max_slices, interpret_leaf(interpreter, expected, leaf));
    require(aot.invoke_isolated_aot(leaf.entry, actual), std::string(leaf.name) + ": AOT leaf absent");
    require(actual.pc == external_return, std::string(leaf.name) + ": AOT did not return to external sentinel");
    interpreter.memory().copy_out(base, interpreted);
    aot.memory().copy_out(base, compiled);

    // A native adapter can call the original generated unit with an external
    // zero return sentinel. Verify that this exits after exactly the leaf,
    // without taking a same-unit return through the local dispatcher.
    aot.memory().copy_in(base, before);
    const std::uint32_t mapped_internal_return = leaf.entry;
    auto direct = initial;
    direct.gpr[31] = 0u;
    psprecomp::recomp_unit_0028(aot, direct);
    aot.memory().copy_out(base, direct_window);
    const bool sentinel_exit = direct.pc == 0u;
    direct.gpr[31] = mapped_internal_return;
    direct.pc = mapped_internal_return;
    auto direct_expected = actual;
    direct_expected.gpr[31] = mapped_internal_return;
    direct_expected.pc = mapped_internal_return;
    const auto wrapper_differences = compare_cpu(direct_expected, direct);
    if (!sentinel_exit || !wrapper_differences.empty() || direct_window != compiled) {
        ++counts.direct_wrapper_mismatch_cases;
        if (!sentinel_exit) ++counts.wrapper_fields["zero_sentinel_exit"];
        for (const auto &difference : wrapper_differences) ++counts.wrapper_fields[difference.field];
        if (direct_window != compiled) ++counts.wrapper_fields["memory_window"];
    }

    const auto cpu_differences = compare_cpu(expected, actual);
    const bool result_mismatch = bits(expected.fpr[0]) != bits(actual.fpr[0]);
    const bool memory_mismatch = interpreted != compiled;
    auto &family_counts = counts.families[family];
    ++family_counts.cases;
    if (!cpu_differences.empty()) ++counts.cpu_mismatch_cases;
    if (result_mismatch) ++counts.result_mismatch_cases;
    if (memory_mismatch) ++counts.memory_mismatch_cases;
    family_counts.cpu_mismatch += !cpu_differences.empty();
    family_counts.result_mismatch += result_mismatch;
    family_counts.memory_mismatch += memory_mismatch;
    for (const auto &difference : cpu_differences) ++counts.cpu_fields[difference.field];
    std::uint32_t different_bytes{};
    std::uint32_t first_memory_offset = window_size;
    bool unexpected_write{};
    for (std::size_t i = 0; i < window_size; ++i) {
        if (interpreted[i] != compiled[i]) {
            ++different_bytes;
            if (first_memory_offset == window_size) first_memory_offset = static_cast<std::uint32_t>(i);
        }
        if ((i < scratch || i >= scratch + 4u) && (interpreted[i] != before[i] || compiled[i] != before[i]))
            unexpected_write = true;
    }
    counts.unexpected_memory_writes += unexpected_write;
    counts.memory_different_bytes += different_bytes;
    const auto interpreted_result = bits(expected.fpr[0]);
    const auto aot_result = bits(actual.fpr[0]);
    const bool interpreter_nan = nan_bits(interpreted_result);
    const bool aot_nan = nan_bits(aot_result);
    counts.nan_output_cases += interpreter_nan || aot_nan;
    counts.nan_payload_mismatch_cases += interpreter_nan && aot_nan && result_mismatch;
    counts.nan_class_mismatch_cases += interpreter_nan != aot_nan;
    const bool standard_prefix = ps == 0xE4u && pt == 0xE4u && pd == 0u;
    const Models models = standard_prefix ? scalar_models(leaf, before, a, b) : Models{};
    if ((!cpu_differences.empty() || memory_mismatch) && counts.first_differences.size() < 16u) {
        Example example{};
        example.family = family;
        example.index = index;
        if (!cpu_differences.empty()) {
            example.field = cpu_differences.front().field;
            example.interpreter = cpu_differences.front().expected;
            example.aot = cpu_differences.front().actual;
        } else {
            for (std::size_t i = 0; i < window_size; ++i) {
                if (interpreted[i] != compiled[i]) {
                    example.field = "memory_byte[" + std::to_string(i) + "]";
                    example.interpreter = interpreted[i];
                    example.aot = compiled[i];
                    break;
                }
            }
        }
        for (unsigned lane = 0; lane < 4u; ++lane) {
            example.source_a[lane] = read32(before, a + lane * 4u);
            example.source_b[lane] = read32(before, b + lane * 4u);
        }
        for (const auto &difference : cpu_differences) example.all_cpu_fields.push_back(difference.field);
        example.memory_different_bytes = different_bytes;
        example.first_memory_offset = first_memory_offset;
        example.model_applicable = standard_prefix;
        example.models = models;
        counts.first_differences.push_back(example);
    }
    if (standard_prefix) {
        ++counts.standard_model_checked;
        for (unsigned side = 0; side < 2u; ++side) {
            const std::uint32_t output = side == 0u ? bits(expected.fpr[0]) : bits(actual.fpr[0]);
            counts.standard_fma_from_zero_matches[side] += output == models.fma_from_zero;
            counts.standard_multiply_first_matches[side] += output == models.multiply_first_fma_tail;
            counts.standard_reversed_first_matches[side] += output == models.reversed_first_pair;
            counts.standard_unfused_matches[side] += output == models.unfused;
            if (finite_model) {
                counts.fma_from_zero_matches[side] += output == models.fma_from_zero;
                counts.multiply_first_fma_tail_matches[side] += output == models.multiply_first_fma_tail;
                counts.reversed_first_pair_matches[side] += output == models.reversed_first_pair;
                counts.unfused_matches[side] += output == models.unfused;
            }
        }
        family_counts.reversed_model_matches += aot_result == models.reversed_first_pair;
    }
    if (finite_model) ++counts.ordinary_finite;
    ++counts.cases;
    if (ps != 0xE4u || pt != 0xE4u || pd != 0u) ++counts.nonstandard_prefix;
    if ((a | b | scratch) & 3u) ++counts.unaligned;
    if (alias_a || alias_b || alias_stack) ++counts.aliases;
    if ((a < scratch + 4u && scratch < a + 16u) ||
        (leaf.distance && b < scratch + 4u && scratch < b + 16u)) ++counts.stack_source_overlap;
}

Counts run_leaf(Runtime &interpreter, Runtime &aot, const Leaf &leaf) {
    std::mt19937 random(0x56454332u ^ leaf.entry);
    Counts counts{};
    for (unsigned i = 0; i < edges.size(); ++i) {
        std::array<std::uint32_t, 4> x{edges[i], 0u, 0u, edges[(i + 5u) % edges.size()]};
        std::array<std::uint32_t, 4> y{0u, 0u, 0u, edges[(i + 7u) % edges.size()]};
        run_case(interpreter, aot, random, leaf, counts, "single_lane_edge", i,
                 64u, 112u, 320u, 0u, 0u, 0u, 0xE4u, 0xE4u, 0u, x, y, false);
    }
    constexpr std::array<std::array<std::uint32_t, 3>, 8> stress{{
        {0x7f7fffffu, 0x00800000u, 0x3f800000u},
        {0x7f800000u, 0x00000000u, 0x3f800000u},
        {0xff800000u, 0x7f800000u, 0x80000000u},
        {0x7fc01234u, 0x7fc05678u, 0x3f800000u},
        {0x7fa05678u, 0x7fc01234u, 0x3f800000u},
        {0x00000001u, 0x007fffffu, 0x00800000u},
        {0x3f800001u, 0xbf800001u, 0x33800000u},
        {0x80000000u, 0x00000000u, 0x80000001u},
    }};
    for (unsigned i = 0; i < stress.size(); ++i) {
        const std::array<std::uint32_t, 4> x{stress[i][0], stress[i][1], stress[i][2], edges[(i + 2u) % edges.size()]};
        const std::array<std::uint32_t, 4> y{stress[(i + 3u) % stress.size()][0], stress[(i + 3u) % stress.size()][1],
                                             stress[(i + 3u) % stress.size()][2], edges[(i + 7u) % edges.size()]};
        run_case(interpreter, aot, random, leaf, counts, "mixed_numeric_edge", i,
                 64u, 112u, 320u, 0u, 0u, 0u, 0xE4u, 0xE4u, 0u, x, y, false);
    }
    if (leaf.distance) {
        constexpr std::array<std::array<std::uint32_t, 3>, 4> near_equal{{
            {0x3f800000u, 0xbf800000u, 0x35800000u},
            {0x3f800001u, 0x40000001u, 0x00800000u},
            {0x7f7fffffu, 0xff7fffffu, 0x007fffffu},
            {0x80000000u, 0x00000000u, 0x80000001u},
        }};
        constexpr std::array<std::array<std::uint32_t, 3>, 4> neighbors{{
            {0x3f800000u, 0xbf800000u, 0x35800000u},
            {0x3f800000u, 0x40000000u, 0x007fffffu},
            {0x7f7ffffeu, 0xff7ffffeu, 0x00800000u},
            {0x00000000u, 0x80000000u, 0x00000001u},
        }};
        for (unsigned i = 0; i < near_equal.size(); ++i) {
            const std::array<std::uint32_t, 4> x{
                near_equal[i][0], near_equal[i][1], near_equal[i][2], 0x7fc01234u};
            const std::array<std::uint32_t, 4> y{
                neighbors[i][0], neighbors[i][1], neighbors[i][2], 0x7fa05678u};
            run_case(interpreter, aot, random, leaf, counts, "distance_cancellation", i,
                     64u, 112u, 320u, 0u, 0u, 0u, 0xE4u, 0xE4u, 0u, x, y, false);
        }
    }
    std::uniform_real_distribution<float> finite(-1024.0f, 1024.0f);
    for (unsigned i = 0; i < 1024u; ++i) {
        std::array<std::uint32_t, 4> x{}, y{};
        for (unsigned lane = 0; lane < 4u; ++lane) {
            x[lane] = bits(finite(random));
            y[lane] = bits(finite(random));
        }
        run_case(interpreter, aot, random, leaf, counts, "ordinary_finite", i,
                 48u + i % 4u, 120u + (i / 4u) % 4u, 320u + (i / 16u) % 4u,
                 i % 4u, (i / 4u) % 4u, (i / 16u) % 4u,
                 0xE4u, 0xE4u, 0u, x, y, true);
    }
    constexpr std::array<std::uint32_t, 7> prefix_s{
        0xE4u, 0x1E4u, 0x1F000u, 0xF0E4u, 0x100E4u, 0x0001Bu, 0x10000u};
    constexpr std::array<std::uint32_t, 6> prefix_d{0u, 0x100u, 0x700u, 1u, 3u, 0x201u};
    for (unsigned i = 0; i < 256u; ++i) {
        std::array<std::uint32_t, 4> x{}, y{};
        for (unsigned lane = 0; lane < 4u; ++lane) {
            x[lane] = edges[random() % edges.size()];
            y[lane] = edges[random() % edges.size()];
        }
        run_case(interpreter, aot, random, leaf, counts, "nonstandard_prefix", i,
                 64u + i % 4u, 112u + (i / 4u) % 4u, 320u,
                 i % 4u, (i / 4u) % 4u, (i / 16u) % 4u,
                 prefix_s[i % prefix_s.size()], prefix_s[(i / 7u) % prefix_s.size()],
                 prefix_d[(i / 49u) % prefix_d.size()], x, y, false);
    }
    for (int displacement = -15; displacement <= 3; ++displacement) {
        for (unsigned variant = 0; variant < 4u; ++variant) {
            const auto a = static_cast<std::size_t>(320 + displacement);
            const auto b = variant % 2u ? a : static_cast<std::size_t>(320 - displacement);
            std::array<std::uint32_t, 4> x{}, y{};
            for (unsigned lane = 0; lane < 4u; ++lane) {
                x[lane] = edges[(variant + lane + 2u) % edges.size()];
                y[lane] = edges[(variant + lane + 6u) % edges.size()];
            }
            run_case(interpreter, aot, random, leaf, counts, "stack_source_overlap",
                     static_cast<unsigned>((displacement + 15) * 4 + variant),
                     a, b, 320u, variant, (variant + 1u) % 4u, (variant + 2u) % 4u,
                     0xE4u, 0xE4u, 0u, x, y, false);
        }
    }
    return counts;
}

void write_counts(const Counts &counts) {
    std::cout << "\"cases\":" << counts.cases
              << ",\"ordinary_finite\":" << counts.ordinary_finite
              << ",\"standard_model_checked\":" << counts.standard_model_checked
              << ",\"nonstandard_prefix\":" << counts.nonstandard_prefix
              << ",\"unaligned\":" << counts.unaligned
              << ",\"aliased_addresses\":" << counts.aliases
              << ",\"stack_source_overlap\":" << counts.stack_source_overlap
              << ",\"cpu_mismatch_cases\":" << counts.cpu_mismatch_cases
              << ",\"result_mismatch_cases\":" << counts.result_mismatch_cases
              << ",\"memory_mismatch_cases\":" << counts.memory_mismatch_cases
              << ",\"unexpected_memory_writes\":" << counts.unexpected_memory_writes
              << ",\"memory_different_bytes\":" << counts.memory_different_bytes
              << ",\"nan_output_cases\":" << counts.nan_output_cases
              << ",\"nan_payload_mismatch_cases\":" << counts.nan_payload_mismatch_cases
              << ",\"nan_class_mismatch_cases\":" << counts.nan_class_mismatch_cases
              << ",\"direct_wrapper_mismatch_cases\":" << counts.direct_wrapper_mismatch_cases
              << ",\"max_interpreter_slices\":" << counts.max_slices;
    const auto pair = [](const char *name, const std::uint64_t (&values)[2]) {
        std::cout << ",\"" << name << "\":{\"interpreter\":" << values[0]
                  << ",\"aot\":" << values[1] << '}';
    };
    pair("fma_from_zero_matches", counts.fma_from_zero_matches);
    pair("multiply_first_fma_tail_matches", counts.multiply_first_fma_tail_matches);
    pair("reversed_first_pair_matches", counts.reversed_first_pair_matches);
    pair("unfused_matches", counts.unfused_matches);
    pair("standard_fma_from_zero_matches", counts.standard_fma_from_zero_matches);
    pair("standard_multiply_first_matches", counts.standard_multiply_first_matches);
    pair("standard_reversed_first_matches", counts.standard_reversed_first_matches);
    pair("standard_unfused_matches", counts.standard_unfused_matches);
    std::cout << ",\"families\":{";
    bool first_family = true;
    for (const auto &[name, family] : counts.families) {
        if (!first_family) std::cout << ',';
        first_family = false;
        std::cout << '"' << name << "\":{\"cases\":" << family.cases
                  << ",\"cpu_mismatch\":" << family.cpu_mismatch
                  << ",\"result_mismatch\":" << family.result_mismatch
                  << ",\"memory_mismatch\":" << family.memory_mismatch
                  << ",\"aot_reversed_model_matches\":" << family.reversed_model_matches << '}';
    }
    std::cout << '}';
    const auto write_field_counts = [](const char *name, const std::map<std::string, std::uint64_t> &fields) {
        std::cout << ",\"" << name << "\":{";
        bool first = true;
        for (const auto &[field, count] : fields) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << '"' << field << "\":" << count;
        }
        std::cout << '}';
    };
    write_field_counts("cpu_difference_fields", counts.cpu_fields);
    write_field_counts("direct_wrapper_difference_fields", counts.wrapper_fields);
    std::cout << ",\"first_differences\":[";
    for (std::size_t i = 0; i < counts.first_differences.size(); ++i) {
        if (i) std::cout << ',';
        const auto &example = counts.first_differences[i];
        std::cout << "{\"family\":\"" << example.family << "\",\"index\":" << example.index
                  << ",\"field\":\"" << example.field << "\",\"interpreter\":\""
                  << hex(example.interpreter) << "\",\"aot\":\"" << hex(example.aot) << "\"";
        const auto words = [](const char *name, const std::array<std::uint32_t, 4> &raw) {
            std::cout << ",\"" << name << "\":[";
            for (unsigned lane = 0; lane < 4u; ++lane) {
                if (lane) std::cout << ',';
                std::cout << '"' << hex(raw[lane]) << '"';
            }
            std::cout << ']';
        };
        words("source_a", example.source_a);
        words("source_b", example.source_b);
        std::cout << ",\"all_cpu_fields\":[";
        for (std::size_t field = 0; field < example.all_cpu_fields.size(); ++field) {
            if (field) std::cout << ',';
            std::cout << '"' << example.all_cpu_fields[field] << '"';
        }
        std::cout << "],\"memory_different_bytes\":" << example.memory_different_bytes
                  << ",\"first_memory_offset\":" << example.first_memory_offset
                  << ",\"model_applicable\":" << (example.model_applicable ? "true" : "false")
                  << ",\"models\":{\"fma_from_zero\":\"" << hex(example.models.fma_from_zero)
                  << "\",\"multiply_first_fma_tail\":\"" << hex(example.models.multiply_first_fma_tail)
                  << "\",\"reversed_first_pair\":\"" << hex(example.models.reversed_first_pair)
                  << "\",\"unfused\":\"" << hex(example.models.unfused) << "\"}}";
    }
    std::cout << ']';
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: vector_metrics_aot_probe local-EBOOT.ELF");
        constexpr const char *supported_elf_hash =
            "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";
        const auto elf_hash = psprecomp::sha256_file(argv[1]);
        require(elf_hash == supported_elf_hash, "unsupported ELF identity");
        const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
        Runtime interpreter(elf.required_ram_size()), aot(elf.required_ram_size());
        (void)elf.load_and_relocate(interpreter.memory());
        (void)elf.load_and_relocate(aot.memory());
        psprecomp::register_generated_functions(aot);

        std::cout << "{\"schema_version\":1,\"oracle\":\"production AOT versus bounded interpreter\","
                  << "\"probe_compiler\":\"" << __VERSION__ << "\","
#if defined(__APPLE__) && defined(__aarch64__)
                  << "\"probe_platform\":\"macOS arm64\","
#else
                  << "\"probe_platform\":\"other\","
#endif
                  << "\"elf_sha256\":\"" << elf_hash << "\",\"leaves\":[";
        bool invariants_ok = true;
        for (unsigned i = 0; i < leaves.size(); ++i) {
            const auto &leaf = leaves[i];
            std::array<std::uint8_t, 40> span{};
            interpreter.memory().copy_out(leaf.entry, std::span<std::uint8_t>(span).first(leaf.size));
            const auto hash = psprecomp::sha256_bytes(std::span<const std::uint8_t>(span).first(leaf.size));
            require(hash == leaf.hash, std::string(leaf.name) + ": full-span hash mismatch");
            require(aot.has_function(leaf.entry), std::string(leaf.name) + ": production AOT unregistered");
            const Counts counts = run_leaf(interpreter, aot, leaf);
            invariants_ok = invariants_ok && counts.direct_wrapper_mismatch_cases == 0u &&
                            counts.unexpected_memory_writes == 0u;
            if (i) std::cout << ',';
            std::cout << "{\"name\":\"" << leaf.name << "\",\"entry\":\"" << hex(leaf.entry)
                      << "\",\"size\":" << leaf.size << ",\"span_sha256\":\"" << hash << "\",";
            write_counts(counts);
            std::cout << '}';
        }
        std::cout << "],\"invariants_passed\":" << (invariants_ok ? "true" : "false")
                  << ",\"limitations\":[\"Isolated AOT uses an external return sentinel; direct generated-wrapper audit restores a same-unit return after zero-sentinel exit\","
                  << "\"AOT and interpreter are host software results, not physical PSP arithmetic\","
                  << "\"No full-game launch, dynamic caller coverage, or production native implementation\"]}\n";
        return invariants_ok ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "vector metric AOT probe: " << error.what() << '\n';
        return 1;
    }
}
