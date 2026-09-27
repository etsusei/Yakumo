#include "native/vector_metrics_bridge.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/sha256.hpp"
#include "recomp_units.hpp"

#include <array>
#include <bit>
#include <cfenv>
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
#include <vector>

// Bounded, offline comparison against the production generated unit. The
// interpreter is a second observation, not the native numerical oracle.
namespace {
using mhp3rd::native::MetricExcludedRange;
using mhp3rd::native::NativeMode;
using mhp3rd::native::NativeStats;
using mhp3rd::native::VectorMetric;
using mhp3rd::native::VectorMetricBridge;
using mhp3rd::native::VectorMetricLeaf;
using psprecomp::AllegrexContext;
using psprecomp::Runtime;

constexpr char supported_elf_hash[] =
    "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";
constexpr std::uint32_t ram_base = 0x08010000u;
constexpr std::uint32_t vram_base = 0x04010000u;
constexpr std::uint32_t external_return = 0x08020000u;
constexpr std::size_t window_size = 512u;
constexpr std::array<std::uint32_t, 4> aliases{0u, 0x40000000u, 0x80000000u, 0xa0000000u};
constexpr std::array<std::uint32_t, 16> edges{
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
    0x007fffffu, 0x00800000u, 0x3f800000u, 0xbf800000u,
    0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u,
    0x7fc01234u, 0x7fc05678u, 0x7fa05678u, 0xffa04567u};
using Window = std::array<std::uint8_t, window_size>;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

std::string hex(std::uint32_t word) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(8) << std::setfill('0') << word;
    return output.str();
}

std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
float value(std::uint32_t raw) { return std::bit_cast<float>(raw); }

std::uint32_t read32(const Window &bytes, std::size_t offset) {
    std::uint32_t word{};
    for (unsigned byte = 0; byte < 4u; ++byte)
        word |= std::uint32_t(bytes.at(offset + byte)) << (8u * byte);
    return word;
}

void write32(Window &bytes, std::size_t offset, std::uint32_t word) {
    for (unsigned byte = 0; byte < 4u; ++byte)
        bytes.at(offset + byte) = static_cast<std::uint8_t>(word >> (8u * byte));
}

std::string first_cpu_difference(const AllegrexContext &a, const AllegrexContext &b) {
    const auto difference = [](const std::string &name, std::uint32_t left, std::uint32_t right) {
        return name + " expected=" + hex(left) + " actual=" + hex(right);
    };
    for (unsigned i = 0; i < 32u; ++i)
        if (a.gpr[i] != b.gpr[i]) return difference("gpr[" + std::to_string(i) + "]", a.gpr[i], b.gpr[i]);
    if (a.hi != b.hi) return difference("hi", a.hi, b.hi);
    if (a.lo != b.lo) return difference("lo", a.lo, b.lo);
    if (a.pc != b.pc) return difference("pc", a.pc, b.pc);
    for (unsigned i = 0; i < 32u; ++i)
        if (bits(a.fpr[i]) != bits(b.fpr[i]))
            return difference("fpr[" + std::to_string(i) + "]", bits(a.fpr[i]), bits(b.fpr[i]));
    if (a.fcr31 != b.fcr31) return difference("fcr31", a.fcr31, b.fcr31);
    for (unsigned i = 0; i < 128u; ++i)
        if (bits(a.vfpu[i]) != bits(b.vfpu[i]))
            return difference("vfpu[" + std::to_string(i) + "]", bits(a.vfpu[i]), bits(b.vfpu[i]));
    for (unsigned i = 0; i < 16u; ++i)
        if (a.vfpu_ctrl[i] != b.vfpu_ctrl[i])
            return difference("vfpu_ctrl[" + std::to_string(i) + "]", a.vfpu_ctrl[i], b.vfpu_ctrl[i]);
    return {};
}

std::string first_memory_difference(const Window &a, const Window &b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return "byte[" + std::to_string(i) + "] expected=" + hex(a[i]) + " actual=" + hex(b[i]);
    return {};
}

void call_production_original(Runtime &runtime, AllegrexContext &cpu) {
    const auto return_address = cpu.gpr[31];
    cpu.gpr[31] = 0u;
    psprecomp::recomp_unit_0028(runtime, cpu);
    cpu.gpr[31] = return_address;
    require(!runtime.stopped() && cpu.pc == 0u, "production wrapper did not return at the zero sentinel");
    cpu.pc = return_address;
}

std::uint64_t counted_original_calls{};
void counted_original(Runtime &runtime, AllegrexContext &cpu) {
    ++counted_original_calls;
    psprecomp::recomp_unit_0028(runtime, cpu);
}

std::uint64_t faulty_original_calls{};
std::uint32_t expected_scratch_before{};
bool faulty_original_saw_pristine{};
void corrupt_scalar_and_scratch(Runtime &runtime, AllegrexContext &cpu) {
    const auto raw = bits(cpu.fpr[0]) ^ 1u;
    cpu.fpr[0] = value(raw);
    runtime.memory().store32(cpu.gpr[29] - 16u, raw);
}
void faulty_original(Runtime &runtime, AllegrexContext &cpu) {
    ++faulty_original_calls;
    faulty_original_saw_pristine &= runtime.memory().load32(cpu.gpr[29] - 16u) == expected_scratch_before;
    psprecomp::recomp_unit_0028(runtime, cpu);
    corrupt_scalar_and_scratch(runtime, cpu);
}

std::uint64_t guarded_original_calls{};
void guarded_original(Runtime &, AllegrexContext &cpu) {
    ++guarded_original_calls;
    cpu.pc = 0u;
}

std::uint64_t invalid_exit_original_calls{}, throwing_original_calls{};
bool throwing_original_saw_zero_ra{};
void invalid_exit_original(Runtime &, AllegrexContext &) {
    ++invalid_exit_original_calls;
}
void throwing_original(Runtime &, AllegrexContext &cpu) {
    ++throwing_original_calls;
    throwing_original_saw_zero_ra = cpu.gpr[31] == 0u;
    throw std::runtime_error("injected original failure");
}

struct TestCase {
    const char *family{"finite"};
    unsigned index{};
    std::uint32_t base{ram_base};
    std::size_t a{64u}, b{112u}, scratch{320u};
    unsigned alias_a{}, alias_b{}, alias_stack{};
    std::array<std::uint32_t, 4> first{}, second{};
    std::uint32_t prefix_s{0xe4u}, prefix_t{0xe4u}, prefix_d{};
    bool code_source{};
    bool synthetic_exclusion{};
    int rounding{FE_TONEAREST};

    [[nodiscard]] bool admitted() const {
        return base == ram_base && !code_source && !synthetic_exclusion &&
               rounding == FE_TONEAREST && prefix_s == 0xe4u && prefix_t == 0xe4u && prefix_d == 0u;
    }
};

const char *leaf_name(VectorMetric kind) {
    switch (kind) {
    case VectorMetric::Norm: return "norm";
    case VectorMetric::NormSquared: return "norm_squared";
    case VectorMetric::Distance: return "distance";
    case VectorMetric::DistanceSquared: return "distance_squared";
    }
    return "unknown";
}

std::vector<TestCase> cases_for(const VectorMetricLeaf &leaf) {
    const bool distance = mhp3rd::native::metric_uses_distance(leaf.kind);
    std::vector<TestCase> cases;
    const auto add = [&](TestCase test) {
        test.index = static_cast<unsigned>(cases.size());
        cases.push_back(test);
    };
    for (unsigned lane = 0; lane < 3u; ++lane) {
        for (unsigned i = 0; i < edges.size(); ++i) {
            TestCase test;
            test.family = "single_lane_edge";
            test.first[lane] = edges[i];
            test.first[3] = edges[(i + 5u) % edges.size()];
            test.second[3] = edges[(i + 9u) % edges.size()];
            if (distance && i % 2u) test.second[lane] = edges[(i + 7u) % edges.size()];
            add(test);
        }
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
    for (unsigned i = 0; i < 16u; ++i) {
        TestCase test;
        test.family = "mixed_edge";
        for (unsigned lane = 0; lane < 3u; ++lane) {
            test.first[lane] = stress[i % stress.size()][lane];
            test.second[lane] = stress[(i + 3u) % stress.size()][lane];
        }
        test.first[3] = edges[i];
        test.second[3] = edges[(i + 5u) % edges.size()];
        add(test);
    }
    if (distance) {
        constexpr std::array<std::array<std::uint32_t, 3>, 4> near{{
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
        for (unsigned i = 0; i < near.size(); ++i) {
            TestCase test;
            test.family = "cancellation";
            for (unsigned lane = 0; lane < 3u; ++lane) {
                test.first[lane] = near[i][lane];
                test.second[lane] = neighbors[i][lane];
            }
            test.first[3] = 0x7fc01234u;
            test.second[3] = 0x7fa05678u;
            add(test);
        }
    }
    std::mt19937 random(0x56454332u ^ leaf.entry);
    std::uniform_real_distribution<float> finite(-1024.0f, 1024.0f);
    for (unsigned i = 0; i < 512u; ++i) {
        TestCase test;
        test.family = "finite_random";
        test.a = 48u + i % 4u;
        test.b = 120u + (i / 4u) % 4u;
        test.scratch = 320u + (i / 16u) % 4u;
        test.alias_a = i % 4u;
        test.alias_b = (i / 4u) % 4u;
        test.alias_stack = (i / 16u) % 4u;
        for (unsigned lane = 0; lane < 4u; ++lane) {
            test.first[lane] = bits(finite(random));
            test.second[lane] = bits(finite(random));
        }
        add(test);
    }
    for (unsigned i = 0; i < 256u; ++i) {
        TestCase test;
        test.family = "raw_finite_random";
        test.a = 64u + i % 4u;
        test.b = 112u + (i / 4u) % 4u;
        test.scratch = 320u + (i / 16u) % 4u;
        test.alias_a = i % 4u;
        test.alias_b = (i / 4u) % 4u;
        test.alias_stack = (i / 16u) % 4u;
        for (unsigned lane = 0; lane < 4u; ++lane) {
            test.first[lane] = random();
            test.second[lane] = random();
            if ((test.first[lane] & 0x7f800000u) == 0x7f800000u) test.first[lane] ^= 0x00800000u;
            if ((test.second[lane] & 0x7f800000u) == 0x7f800000u) test.second[lane] ^= 0x00800000u;
        }
        add(test);
    }
    constexpr std::array<std::uint32_t, 7> prefixes{
        0xe4u, 0x1e4u, 0x1f000u, 0xf0e4u, 0x100e4u, 0x1bu, 0x10000u};
    constexpr std::array<std::uint32_t, 6> destinations{0u, 0x100u, 0x700u, 1u, 3u, 0x201u};
    for (unsigned i = 0; i < 64u; ++i) {
        TestCase test;
        test.family = "nonstandard_prefix";
        test.a += i % 4u;
        test.b += (i / 4u) % 4u;
        test.alias_a = i % 4u;
        test.alias_b = (i / 4u) % 4u;
        test.alias_stack = (i / 16u) % 4u;
        test.prefix_s = prefixes[i % prefixes.size()];
        test.prefix_t = prefixes[(i / 7u) % prefixes.size()];
        test.prefix_d = destinations[(i / 11u) % destinations.size()];
        if (test.admitted()) test.prefix_s = 0x1e4u;
        for (unsigned lane = 0; lane < 4u; ++lane) {
            test.first[lane] = edges[random() % edges.size()];
            test.second[lane] = edges[random() % edges.size()];
        }
        add(test);
    }
    for (int displacement = -12; displacement <= 3; ++displacement) {
        for (unsigned variant = 0; variant < 4u; ++variant) {
            TestCase test;
            test.family = "stack_source_overlap";
            test.a = static_cast<std::size_t>(320 + displacement);
            test.b = variant % 2u ? test.a : static_cast<std::size_t>(320 - displacement);
            test.alias_a = variant;
            test.alias_b = (variant + 1u) % 4u;
            test.alias_stack = (variant + 2u) % 4u;
            for (unsigned lane = 0; lane < 4u; ++lane) {
                test.first[lane] = edges[(variant + lane + 2u) % edges.size()];
                test.second[lane] = edges[(variant + lane + 6u) % edges.size()];
            }
            add(test);
        }
    }
    for (unsigned i = 0; i < 32u; ++i) {
        TestCase test;
        test.family = "source_source_overlap";
        test.a = 64u + i % 4u;
        test.b = static_cast<std::size_t>(static_cast<int>(test.a) + static_cast<int>(i % 25u) - 12);
        test.alias_a = i % 4u;
        test.alias_b = (i + 1u) % 4u;
        for (unsigned lane = 0; lane < 4u; ++lane) {
            test.first[lane] = edges[(i + lane) % edges.size()];
            test.second[lane] = edges[(i + lane + 5u) % edges.size()];
        }
        add(test);
    }
    for (unsigned i = 0; i < 8u; ++i) {
        TestCase test;
        test.family = "vram_fallback";
        test.base = vram_base;
        test.alias_a = i % 4u;
        test.alias_b = (i + 1u) % 4u;
        test.alias_stack = (i + 2u) % 4u;
        test.first = {edges[i], edges[i + 1u], edges[i + 2u], edges[i + 3u]};
        test.second = {edges[i + 4u], edges[i + 5u], edges[i + 6u], edges[i + 7u]};
        add(test);
    }
    {
        TestCase test;
        test.family = "synthetic_excluded_source";
        test.a = 448u;
        test.synthetic_exclusion = true;
        test.first = {0x3f800000u, 0x40000000u, 0x40400000u, 0x7fa05678u};
        add(test);
    }
    {
        TestCase test;
        test.family = "executable_source";
        test.code_source = true;
        test.second = {0x3f800000u, 0u, 0u, 0x7fc01234u};
        add(test);
    }
    {
        TestCase test;
        test.family = "directed_rounding_fallback";
        test.rounding = FE_DOWNWARD;
        test.first = {0x3f800001u, 0x3f000001u, 0x40000001u, 0u};
        test.second = {0x3e800001u, 0x3f000000u, 0x3f800000u, 0u};
        add(test);
    }
    return cases;
}

AllegrexContext random_cpu(std::mt19937 &random, const VectorMetricLeaf &leaf,
                          const TestCase &test) {
    AllegrexContext cpu{};
    for (auto &word : cpu.gpr) word = random();
    for (auto &word : cpu.fpr) word = value(random());
    for (auto &word : cpu.vfpu) word = value(random());
    for (auto &word : cpu.vfpu_ctrl) word = random();
    cpu.gpr[0] = 0u;
    cpu.gpr[4] = test.code_source ? leaf.entry : (test.base + static_cast<std::uint32_t>(test.a)) | aliases.at(test.alias_a);
    cpu.gpr[5] = (test.base + static_cast<std::uint32_t>(test.b)) | aliases.at(test.alias_b);
    cpu.gpr[29] = (test.base + static_cast<std::uint32_t>(test.scratch) + 16u) | aliases.at(test.alias_stack);
    cpu.gpr[31] = test.index % 17u == 0u ? leaf.entry :
                  test.index % 29u == 0u ? leaf.entry + 4u : external_return;
    cpu.hi = random();
    cpu.lo = random();
    cpu.fcr31 = random();
    cpu.pc = leaf.entry;
    cpu.vfpu_ctrl[0] = test.prefix_s;
    cpu.vfpu_ctrl[1] = test.prefix_t;
    cpu.vfpu_ctrl[2] = test.prefix_d;
    return cpu;
}

struct Fixture {
    Window before{};
    AllegrexContext cpu{};
};

Fixture make_fixture(const VectorMetricLeaf &leaf, const TestCase &test) {
    require(test.a + 16u <= window_size && test.b + 16u <= window_size &&
            test.scratch + 4u <= window_size, "test fixture escaped its memory window");
    std::mt19937 random(0x5c31a7d2u ^ leaf.entry ^ (test.index * 0x9e3779b9u));
    Fixture fixture;
    for (auto &byte : fixture.before) byte = static_cast<std::uint8_t>(random());
    if (!test.code_source)
        for (unsigned lane = 0; lane < 4u; ++lane)
            write32(fixture.before, test.a + lane * 4u, test.first[lane]);
    if (mhp3rd::native::metric_uses_distance(leaf.kind))
        for (unsigned lane = 0; lane < 4u; ++lane)
            write32(fixture.before, test.b + lane * 4u, test.second[lane]);
    fixture.cpu = random_cpu(random, leaf, test);
    return fixture;
}

void interpret_leaf(Runtime &runtime, AllegrexContext &cpu, const VectorMetricLeaf &leaf) {
    const auto return_address = cpu.gpr[31];
    for (unsigned slice = 0; slice < 12u; ++slice) {
        require(cpu.pc >= leaf.entry && cpu.pc < leaf.entry + leaf.size,
                "interpreter left the certified leaf before return");
        const bool returning = cpu.pc == leaf.entry + leaf.size - 8u;
        const auto exit = psprecomp::interpret_allegrex(runtime, cpu, 1u);
        require(exit == psprecomp::InterpreterExit::Budget && !runtime.stopped(),
                "interpreter did not complete its bounded slice");
        if (returning) {
            require(cpu.pc == return_address, "interpreter return missed the delay slot");
            return;
        }
    }
    throw std::runtime_error("interpreter exceeded twelve bounded slices");
}

struct ObservationCounts {
    std::uint64_t interpreter_cases{}, interpreter_numeric_disagreements{}, core_cases{};
    std::uint64_t interpreter_scalar_disagreements{}, interpreter_component_disagreements{};
    std::uint64_t interpreter_scratch_disagreements{};
};

void compare_interpreter_observation(const AllegrexContext &interpreter,
                                     const AllegrexContext &aot,
                                     const Window &interpreter_window,
                                     const Window &aot_window,
                                     std::size_t scratch, ObservationCounts &counts,
                                     const std::string &label) {
    ++counts.interpreter_cases;
    const bool different = !first_cpu_difference(interpreter, aot).empty() || interpreter_window != aot_window;
    counts.interpreter_numeric_disagreements += different;
    counts.interpreter_scalar_disagreements +=
        bits(interpreter.fpr[0]) != bits(aot.fpr[0]) ||
        bits(interpreter.vfpu[4]) != bits(aot.vfpu[4]);
    bool component_difference = false;
    for (unsigned index : {0u, 1u, 2u})
        component_difference |= bits(interpreter.vfpu[index]) != bits(aot.vfpu[index]);
    counts.interpreter_component_disagreements += component_difference;
    bool scratch_difference = false;
    for (std::size_t i = scratch; i < scratch + 4u; ++i)
        scratch_difference |= interpreter_window[i] != aot_window[i];
    counts.interpreter_scratch_disagreements += scratch_difference;
    auto structural = interpreter;
    structural.fpr[0] = aot.fpr[0];
    for (unsigned index : {0u, 1u, 2u, 4u}) structural.vfpu[index] = aot.vfpu[index];
    require(first_cpu_difference(structural, aot).empty(), label + ": interpreter structural CPU difference: " +
            first_cpu_difference(structural, aot));
    for (std::size_t i = 0; i < window_size; ++i)
        if ((i < scratch || i >= scratch + 4u) && interpreter_window[i] != aot_window[i])
            throw std::runtime_error(label + ": interpreter wrote outside scalar scratch at byte " + std::to_string(i));
}

void compare_core(const VectorMetricLeaf &leaf, const TestCase &test, const Window &before,
                  const AllegrexContext &aot, ObservationCounts &counts, const std::string &label) {
    if (test.code_source || test.rounding != FE_TONEAREST || test.prefix_s != 0xe4u ||
        test.prefix_t != 0xe4u || test.prefix_d != 0u) return;
    std::array<std::uint32_t, 3> first{}, second{};
    for (unsigned lane = 0; lane < 3u; ++lane) {
        first[lane] = read32(before, test.a + lane * 4u);
        if (mhp3rd::native::metric_uses_distance(leaf.kind))
            second[lane] = read32(before, test.b + lane * 4u);
    }
    const auto result = mhp3rd::native::vector_metric(leaf.kind, first, second);
    require(result.scalar == bits(aot.fpr[0]), label + ": portable scalar differs from generated AOT: " +
            hex(result.scalar) + " versus " + hex(bits(aot.fpr[0])));
    for (unsigned lane = 0; lane < 3u; ++lane)
        require(result.components[lane] == bits(aot.vfpu[lane]),
                label + ": portable component " + std::to_string(lane) + " differs from generated AOT");
    ++counts.core_cases;
}

struct ScopedRounding {
    int previous;
    explicit ScopedRounding(int mode) : previous(std::fegetround()) {
        require(previous != -1 && std::fesetround(mode) == 0, "could not set host rounding mode");
    }
    ~ScopedRounding() { (void)std::fesetround(previous); }
};

void run_case(Runtime &reference, Runtime &candidate, Runtime &interpreter,
              VectorMetricBridge &bridge, const VectorMetricLeaf &leaf,
              const TestCase &test, NativeMode mode, ObservationCounts *observation) {
    const auto label = std::string(leaf_name(leaf.kind)) + "/" + test.family +
                       "[" + std::to_string(test.index) + "]";
    ScopedRounding rounding(test.rounding);
    const auto fixture = make_fixture(leaf, test);
    reference.memory().copy_in(test.base, fixture.before);
    candidate.memory().copy_in(test.base, fixture.before);
    if (observation) interpreter.memory().copy_in(test.base, fixture.before);

    auto expected = fixture.cpu;
    auto actual = fixture.cpu;
    call_production_original(reference, expected);
    require(bridge.execute(candidate, actual), label + ": bridge refused valid production call");

    Window expected_window{}, actual_window{};
    reference.memory().copy_out(test.base, expected_window);
    candidate.memory().copy_out(test.base, actual_window);
    require(first_cpu_difference(expected, actual).empty(), label + ": " +
            first_cpu_difference(expected, actual));
    require(expected_window == actual_window, label + ": " +
            first_memory_difference(expected_window, actual_window));
    for (std::size_t i = 0; i < window_size; ++i)
        if ((i < test.scratch || i >= test.scratch + 4u) && expected_window[i] != fixture.before[i])
            throw std::runtime_error(label + ": production wrapper changed a canary at byte " + std::to_string(i));

    if (observation) {
        auto interpreted = fixture.cpu;
        interpret_leaf(interpreter, interpreted, leaf);
        Window interpreter_window{};
        interpreter.memory().copy_out(test.base, interpreter_window);
        compare_interpreter_observation(interpreted, expected, interpreter_window, expected_window,
                                        test.scratch, *observation, label);
        compare_core(leaf, test, fixture.before, expected, *observation, label);
    }
    (void)mode;
}

struct ModeCounts {
    std::uint64_t cases{}, admitted{}, original_calls{};
    NativeStats stats{};
};

ModeCounts run_mode(Runtime &reference, Runtime &candidate, Runtime &interpreter,
                    const VectorMetricLeaf &leaf, NativeMode mode,
                    std::span<const MetricExcludedRange> excluded,
                    const std::vector<TestCase> &cases, ObservationCounts *observation) {
    VectorMetricBridge bridge(leaf.kind);
    require(bridge.configure(candidate, mode, &counted_original, excluded),
            std::string(leaf_name(leaf.kind)) + ": bridge refused certified configuration");
    const auto original_before = counted_original_calls;
    ModeCounts counts;
    for (const auto &test : cases) {
        run_case(reference, candidate, interpreter, bridge, leaf, test, mode, observation);
        ++counts.cases;
        counts.admitted += test.admitted();
    }
    counts.stats = bridge.stats();
    counts.original_calls = counted_original_calls - original_before;
    const auto eligible = counts.admitted;
    const auto fallback = counts.cases - eligible;
    require(counts.stats.calls == counts.cases && counts.stats.errors == 0u &&
            counts.stats.mismatches == 0u, "bridge counters report an unexpected call, error or mismatch");
    switch (mode) {
    case NativeMode::Off:
        require(counts.stats.verified == 0u && counts.stats.native == 0u &&
                counts.stats.fallbacks == 0u && counts.original_calls == counts.cases,
                "off mode did not execute only the production original");
        break;
    case NativeMode::Verify:
        require(counts.stats.verified == eligible && counts.stats.native == 0u &&
                counts.stats.fallbacks == fallback && counts.original_calls == counts.cases,
                "verify mode counters or original-call count differ from admission decisions");
        break;
    case NativeMode::Native:
        require(counts.stats.verified == 0u && counts.stats.native == eligible &&
                counts.stats.fallbacks == fallback && counts.original_calls == fallback,
                "native mode counters or original-call count differ from admission decisions");
        break;
    }
    return counts;
}

std::vector<MetricExcludedRange> executable_ranges(const psprecomp::Elf32Image &elf) {
    std::vector<MetricExcludedRange> ranges;
    for (const auto &section : elf.sections())
        if ((section.flags & 4u) != 0u && section.size != 0u)
            ranges.push_back({elf.section_runtime_address(section), section.size});
    ranges.push_back({ram_base + 448u, 32u});
    return ranges;
}

void test_rejections(const psprecomp::Elf32Image &elf, Runtime &reference, Runtime &candidate,
                     const VectorMetricLeaf &leaf, std::span<const MetricExcludedRange> excluded) {
    const auto label = std::string(leaf_name(leaf.kind));
    VectorMetricBridge unbound(leaf.kind);
    auto unbound_cpu = make_fixture(leaf, TestCase{}).cpu;
    const auto unbound_before = unbound_cpu;
    require(unbound.mode() == NativeMode::Off && !unbound.execute(candidate, unbound_cpu) &&
            unbound.stats().calls == 0u && unbound.stats().errors == 1u &&
            first_cpu_difference(unbound_before, unbound_cpu).empty(),
            label + ": unbound bridge was not off and inert by default");
    Runtime blank(elf.required_ram_size());
    VectorMetricBridge unknown(leaf.kind);
    require(!unknown.configure(blank, NativeMode::Native, &counted_original, excluded) &&
            unknown.stats().errors == 1u && unknown.stats().calls == 0u,
            label + ": unknown full-span fingerprint was accepted");
    VectorMetricBridge missing_original(leaf.kind);
    require(!missing_original.configure(candidate, NativeMode::Native, nullptr, excluded) &&
            missing_original.stats().errors == 1u,
            label + ": null original was accepted");
    VectorMetricBridge missing_ranges(leaf.kind);
    require(!missing_ranges.configure(candidate, NativeMode::Native, &counted_original, {}) &&
            missing_ranges.stats().errors == 1u,
            label + ": empty excluded-code map was accepted");

    VectorMetricBridge bridge(leaf.kind);
    const auto before_calls = counted_original_calls;
    auto fixture = make_fixture(leaf, TestCase{});
    candidate.memory().copy_in(ram_base, fixture.before);
    require(bridge.configure(candidate, NativeMode::Native, &counted_original, excluded),
            label + ": rejection bridge did not configure");
    auto wrong_runtime = fixture.cpu;
    const auto wrong_runtime_before = wrong_runtime;
    require(!bridge.execute(reference, wrong_runtime) &&
            first_cpu_difference(wrong_runtime_before, wrong_runtime).empty(),
            label + ": wrong runtime changed CPU or entered original");
    auto wrong_pc = fixture.cpu;
    wrong_pc.pc += 4u;
    const auto wrong_pc_before = wrong_pc;
    require(!bridge.execute(candidate, wrong_pc) &&
            first_cpu_difference(wrong_pc_before, wrong_pc).empty(),
            label + ": wrong PC changed CPU or entered original");
    const auto delay_address = leaf.entry + leaf.size - 4u;
    const auto delay_word = candidate.memory().load32(delay_address);
    candidate.memory().store32(delay_address, delay_word ^ 1u);
    auto changed_code = fixture.cpu;
    const auto changed_before = changed_code;
    Window before_window{}, after_window{};
    candidate.memory().copy_out(ram_base, before_window);
    require(!bridge.execute(candidate, changed_code), label + ": changed code executed");
    candidate.memory().copy_out(ram_base, after_window);
    require(first_cpu_difference(changed_before, changed_code).empty() && before_window == after_window &&
            counted_original_calls == before_calls,
            label + ": refusal mutated state or called the original");
    require(!bridge.configure(candidate, NativeMode::Native, &counted_original, excluded),
            label + ": changed full span installed");
    candidate.memory().store32(delay_address, delay_word);
}

void test_original_failures(const psprecomp::Elf32Image &elf,
                            const VectorMetricLeaf &leaf,
                            std::span<const MetricExcludedRange> excluded) {
    const auto label = std::string(leaf_name(leaf.kind));
    TestCase test;
    test.index = 17u;
    test.prefix_s = 0x1e4u; // Reject native admission and exercise the original callback.
    const auto fixture = make_fixture(leaf, test);

    Runtime invalid_exit(elf.required_ram_size());
    (void)elf.load_and_relocate(invalid_exit.memory());
    invalid_exit.memory().copy_in(ram_base, fixture.before);
    VectorMetricBridge invalid_bridge(leaf.kind);
    require(invalid_bridge.configure(invalid_exit, NativeMode::Native, &invalid_exit_original, excluded),
            label + ": invalid-return bridge did not configure");
    auto cpu = fixture.cpu;
    const auto invalid_before = invalid_exit_original_calls;
    require(!invalid_bridge.execute(invalid_exit, cpu),
            label + ": original callback without zero-sentinel return was accepted");
    Window after{};
    invalid_exit.memory().copy_out(ram_base, after);
    const auto invalid_stats = invalid_bridge.stats();
    require(invalid_exit.stopped() && cpu.gpr[31] == fixture.cpu.gpr[31] &&
            after == fixture.before && invalid_exit_original_calls == invalid_before + 1u &&
            invalid_stats.calls == 1u && invalid_stats.fallbacks == 1u &&
            invalid_stats.errors == 1u && invalid_stats.native == 0u,
            label + ": invalid original return did not restore RA and report the failure");

    Runtime throwing(elf.required_ram_size());
    (void)elf.load_and_relocate(throwing.memory());
    throwing.memory().copy_in(ram_base, fixture.before);
    VectorMetricBridge throwing_bridge(leaf.kind);
    require(throwing_bridge.configure(throwing, NativeMode::Native, &throwing_original, excluded),
            label + ": throwing-original bridge did not configure");
    cpu = fixture.cpu;
    const auto throwing_before = throwing_original_calls;
    throwing_original_saw_zero_ra = false;
    bool caught = false;
    try { (void)throwing_bridge.execute(throwing, cpu); }
    catch (const std::runtime_error &) { caught = true; }
    throwing.memory().copy_out(ram_base, after);
    const auto throwing_stats = throwing_bridge.stats();
    require(caught && throwing_original_saw_zero_ra && cpu.gpr[31] == fixture.cpu.gpr[31] &&
            after == fixture.before && throwing_original_calls == throwing_before + 1u &&
            throwing_stats.calls == 1u && throwing_stats.fallbacks == 1u &&
            throwing_stats.errors == 1u && throwing_stats.native == 0u,
            label + ": throwing original did not restore RA and report the failure");
}

void test_faulty_reference(Runtime &reference, Runtime &candidate,
                           const VectorMetricLeaf &leaf, std::span<const MetricExcludedRange> excluded) {
    const auto label = std::string(leaf_name(leaf.kind));
    VectorMetricBridge bridge(leaf.kind);
    require(bridge.configure(candidate, NativeMode::Verify, &faulty_original, excluded),
            label + ": fault-injection verifier did not configure");
    const auto original_before = faulty_original_calls;
    for (unsigned turn = 0; turn < 2u; ++turn) {
        TestCase test;
        test.index = turn + 1000u;
        test.first = {0x3f800000u, 0x40000000u, 0x40400000u, 0x7fc01234u};
        test.second = {0x3e800000u, 0x3f000000u, 0x3f800000u, 0x7fa05678u};
        const auto fixture = make_fixture(leaf, test);
        reference.memory().copy_in(ram_base, fixture.before);
        candidate.memory().copy_in(ram_base, fixture.before);
        expected_scratch_before = read32(fixture.before, test.scratch);
        faulty_original_saw_pristine = true;
        auto expected = fixture.cpu;
        auto actual = fixture.cpu;
        call_production_original(reference, expected);
        corrupt_scalar_and_scratch(reference, expected);
        require(bridge.execute(candidate, actual), label + ": faulty original did not return");
        Window expected_window{}, actual_window{};
        reference.memory().copy_out(ram_base, expected_window);
        candidate.memory().copy_out(ram_base, actual_window);
        require(faulty_original_saw_pristine && first_cpu_difference(expected, actual).empty() &&
                expected_window == actual_window,
                label + ": verifier changed the faulty original result or speculatively wrote scratch");
        const auto stats = bridge.stats();
        require(stats.calls == turn + 1u && stats.verified == 1u && stats.mismatches == 1u &&
                stats.fallbacks == turn && stats.native == 0u && stats.errors == 0u,
                label + ": mismatch did not latch original fallback");
    }
    require(bridge.mismatch_latched(), label + ": verifier mismatch latch was not set");
    require(bridge.configure(candidate, NativeMode::Native, &faulty_original, excluded),
            label + ": native reconfiguration after mismatch failed");
    require(bridge.mismatch_latched(), label + ": reconfiguration cleared the mismatch latch");
    TestCase test;
    test.index = 1002u;
    test.first = {0x3f800000u, 0x40000000u, 0x40400000u, 0x7fc01234u};
    test.second = {0x3e800000u, 0x3f000000u, 0x3f800000u, 0x7fa05678u};
    const auto fixture = make_fixture(leaf, test);
    reference.memory().copy_in(ram_base, fixture.before);
    candidate.memory().copy_in(ram_base, fixture.before);
    expected_scratch_before = read32(fixture.before, test.scratch);
    faulty_original_saw_pristine = true;
    auto expected = fixture.cpu;
    auto actual = fixture.cpu;
    call_production_original(reference, expected);
    corrupt_scalar_and_scratch(reference, expected);
    require(bridge.execute(candidate, actual), label + ": latched native mode did not run original");
    Window expected_window{}, actual_window{};
    reference.memory().copy_out(ram_base, expected_window);
    candidate.memory().copy_out(ram_base, actual_window);
    const auto stats = bridge.stats();
    require(faulty_original_saw_pristine && first_cpu_difference(expected, actual).empty() &&
            expected_window == actual_window && stats.calls == 1u && stats.fallbacks == 1u &&
            stats.native == 0u && stats.verified == 0u && stats.mismatches == 0u &&
            stats.errors == 0u && faulty_original_calls - original_before == 3u,
            label + ": reconfigured native mode ignored the latched mismatch");
}

void test_unmapped_fallback(Runtime &candidate, const VectorMetricLeaf &leaf,
                            std::span<const MetricExcludedRange> excluded) {
    const auto label = std::string(leaf_name(leaf.kind));
    VectorMetricBridge guarded(leaf.kind);
    require(guarded.configure(candidate, NativeMode::Native, &guarded_original, excluded),
            label + ": guarded fallback did not configure");
    const auto before_calls = guarded_original_calls;
    TestCase test;
    const auto fixture = make_fixture(leaf, test);
    candidate.memory().copy_in(ram_base, fixture.before);
    for (unsigned variant = 0; variant < 3u; ++variant) {
        auto cpu = fixture.cpu;
        if (variant == 0u) cpu.gpr[4] = 0x00001000u;
        if (variant == 1u) cpu.gpr[29] = 0x00001010u;
        if (variant == 2u) cpu.gpr[5] = 0x00001000u;
        if (variant == 2u && !mhp3rd::native::metric_uses_distance(leaf.kind)) continue;
        const auto before = cpu;
        Window before_window{}, after_window{};
        candidate.memory().copy_out(ram_base, before_window);
        require(guarded.execute(candidate, cpu), label + ": unmapped fallback did not reach the original callback");
        candidate.memory().copy_out(ram_base, after_window);
        auto want = before;
        want.pc = before.gpr[31];
        require(first_cpu_difference(want, cpu).empty() && before_window == after_window,
                label + ": rejected pointer caused a native state or memory write");
    }
    const auto expected_calls = mhp3rd::native::metric_uses_distance(leaf.kind) ? 3u : 2u;
    const auto stats = guarded.stats();
    require(guarded_original_calls - before_calls == expected_calls &&
            stats.calls == expected_calls && stats.fallbacks == expected_calls &&
            stats.native == 0u && stats.verified == 0u && stats.errors == 0u,
            label + ": unmapped range did not route every call to original");

    VectorMetricBridge production(leaf.kind);
    require(production.configure(candidate, NativeMode::Native, &counted_original, excluded),
            label + ": production fallback did not configure");
    auto cpu = fixture.cpu;
    cpu.gpr[4] = 0x00001000u;
    const auto original_before = counted_original_calls;
    bool failed = false;
    try { failed = !production.execute(candidate, cpu); }
    catch (const std::exception &) { failed = true; }
    const auto production_stats = production.stats();
    require(failed && counted_original_calls == original_before + 1u &&
            production_stats.calls == 1u && production_stats.fallbacks == 1u &&
            production_stats.errors == 1u && production_stats.native == 0u,
            label + ": unmapped source did not execute and report production original failure");
}

void print_mode(const char *name, const ModeCounts &mode) {
    std::cout << ' ' << name << "{cases=" << mode.cases
              << ",calls=" << mode.stats.calls
              << ",verified=" << mode.stats.verified
              << ",native=" << mode.stats.native
              << ",fallbacks=" << mode.stats.fallbacks
              << ",mismatches=" << mode.stats.mismatches
              << ",errors=" << mode.stats.errors
              << ",original=" << mode.original_calls << '}';
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: native_vector_metrics_tests local-EBOOT.ELF");
        require(psprecomp::sha256_file(argv[1]) == supported_elf_hash,
                "unsupported ELF SHA-256; pass the registered local EBOOT.ELF");
        const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
        Runtime reference(elf.required_ram_size()), candidate(elf.required_ram_size());
        Runtime interpreter(elf.required_ram_size());
        (void)elf.load_and_relocate(reference.memory());
        (void)elf.load_and_relocate(candidate.memory());
        (void)elf.load_and_relocate(interpreter.memory());
        const auto excluded = executable_ranges(elf);
        require(excluded.size() > 1u, "ELF has no executable sections to exclude");
        require(std::fesetround(FE_TONEAREST) == 0, "cannot select round-to-nearest");

        for (const auto &leaf : mhp3rd::native::kVectorMetricLeaves) {
            std::array<std::uint8_t, 40> span{};
            candidate.memory().copy_out(leaf.entry, std::span(span).first(leaf.size));
            require(psprecomp::sha256_bytes(std::span(span).first(leaf.size)) == leaf.sha256,
                    std::string(leaf_name(leaf.kind)) + ": unsupported full leaf span");
            const auto cases = cases_for(leaf);
            ObservationCounts observation;
            const auto off = run_mode(reference, candidate, interpreter, leaf, NativeMode::Off,
                                      excluded, cases, &observation);
            const auto verify = run_mode(reference, candidate, interpreter, leaf, NativeMode::Verify,
                                         excluded, cases, nullptr);
            const auto native = run_mode(reference, candidate, interpreter, leaf, NativeMode::Native,
                                         excluded, cases, nullptr);
            require(observation.interpreter_cases == cases.size(),
                    "interpreter sample count differs from generated-AOT sample count");
            test_rejections(elf, reference, candidate, leaf, excluded);
            test_faulty_reference(reference, candidate, leaf, excluded);
            test_unmapped_fallback(candidate, leaf, excluded);
            if (leaf.kind == VectorMetric::Norm)
                test_original_failures(elf, leaf, excluded);
            std::cout << leaf_name(leaf.kind) << ": ";
            print_mode("off", off);
            print_mode("verify", verify);
            print_mode("native", native);
            std::cout << " core_AOT=" << observation.core_cases
                      << " interpreter_cases=" << observation.interpreter_cases
                      << " interpreter_numeric_disagreements="
                      << observation.interpreter_numeric_disagreements
                      << " interpreter_scalar_disagreements="
                      << observation.interpreter_scalar_disagreements
                      << " interpreter_component_disagreements="
                      << observation.interpreter_component_disagreements
                      << " interpreter_scratch_disagreements="
                      << observation.interpreter_scratch_disagreements << '\n';
        }
        std::cout << "limitations: production generated AOT and bounded interpreter are host software oracles; "
                     "unmapped production calls are expected to fail; no PSP hardware, SDL, game loop, "
                     "dynamic caller coverage, or live gameplay was exercised.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native vector metrics: " << error.what() << '\n';
        return 1;
    }
}
