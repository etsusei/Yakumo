#include "native/matrix_copy.hpp"
#include "native/bridge_contracts.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>

namespace {

int failures{};

void check(bool condition, const char *message) {
    if (!condition && ++failures <= 12) std::cerr << "FAIL: " << message << '\n';
}

bool same_cpu(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    return mhp3rd::native::same_context(a, b);
}

struct SyntheticMemory {
    std::array<std::uint8_t, 112> bytes{};

    [[nodiscard]] std::uint32_t load32(std::uint32_t address) const {
        std::uint32_t bits = 0;
        for (unsigned i = 0; i < 4; ++i) bits |= std::uint32_t(bytes.at(address + i)) << (8u * i);
        return bits;
    }
    void store32(std::uint32_t address, std::uint32_t bits) {
        for (unsigned i = 0; i < 4; ++i) bytes.at(address + i) = std::uint8_t(bits >> (8u * i));
    }
};

void portable_examples() {
    using mhp3rd::native::copy_matrix_words;
    constexpr std::array<std::uint32_t, 9> bits{
        0x80000000u, 0x7fc01234u, 0x7fa05678u,
        0x00000001u, 0x7f800000u, 0xff800000u,
        0x3f800000u, 0x00000000u, 0xffffffffu};
    SyntheticMemory memory;
    memory.bytes.fill(0xa5u);
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            memory.store32(52u + 16u * row + 4u * column, bits[row * 3 + column]);
    const auto before = memory.bytes;
    const auto last = copy_matrix_words(4u, 52u,
        [&memory](std::uint32_t address) { return memory.load32(address); },
        [&memory](std::uint32_t address, std::uint32_t value) { memory.store32(address, value); });
    check(last == std::array<std::uint32_t, 3>{bits[6], bits[7], bits[8]},
          "portable copy returns final row as raw bits");
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned column = 0; column < 3; ++column)
            check(memory.load32(4u + 16u * row + 4u * column) == bits[row * 3 + column],
                  "portable copy writes the nine selected words");
        check(memory.load32(4u + 16u * row + 12u) == 0xa5a5a5a5u,
              "portable copy leaves each destination padding word unchanged");
    }
    check(memory.bytes[0] == before[0] && memory.bytes[3] == before[3] &&
          memory.bytes[48] == before[48] && memory.bytes[111] == before[111],
          "portable copy preserves surrounding canaries");

    // A forward overlap overwrites row one's source with row zero's output.
    // A bulk nine-word prefetch or memmove would produce the old row one.
    memory.bytes.fill(0u);
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            memory.store32(16u + 16u * row + 4u * column, 100u * row + column + 1u);
    const auto overlapped = copy_matrix_words(32u, 16u,
        [&memory](std::uint32_t address) { return memory.load32(address); },
        [&memory](std::uint32_t address, std::uint32_t value) { memory.store32(address, value); });
    check(memory.load32(32u) == 1u && memory.load32(48u) == 1u && memory.load32(64u) == 1u &&
          overlapped == std::array<std::uint32_t, 3>{1u, 2u, 3u},
          "portable row ordering observes prior row writes on forward overlap");

    memory.bytes.fill(0u);
    memory.store32(17u, 0x11223344u);
    memory.store32(21u, 0x55667788u);
    memory.store32(25u, 0x99aabbccu);
    const auto partial = copy_matrix_words(18u, 17u,
        [&memory](std::uint32_t address) { return memory.load32(address); },
        [&memory](std::uint32_t address, std::uint32_t value) { memory.store32(address, value); });
    (void)partial;
    check(memory.load32(18u) == 0x11223344u && memory.load32(22u) == 0x55667788u &&
          memory.load32(26u) == 0x99aabbccu,
          "portable copy loads an entire row before a partial-overlap store");
}

bool run_original(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context) {
    const auto return_pc = context.gpr[31];
    for (unsigned slice = 0; slice < 24; ++slice) {
        if (context.pc < mhp3rd::native::kMatrixCopyAddress ||
            context.pc >= mhp3rd::native::kMatrixCopyAddress + 80u) return false;
        const bool returning = context.pc == mhp3rd::native::kMatrixCopyAddress + 0x48u;
        const auto exit = psprecomp::interpret_allegrex(runtime, context, 1);
        if (exit == psprecomp::InterpreterExit::Stopped ||
            exit == psprecomp::InterpreterExit::Unreachable) return false;
        if (returning && context.pc == return_pc) return true;
    }
    return false;
}

void fill_cpu(psprecomp::AllegrexContext &context, std::mt19937 &random) {
    for (auto &reg : context.gpr) reg = random();
    for (auto &reg : context.fpr) reg = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    for (auto &reg : context.vfpu) reg = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    for (auto &reg : context.vfpu_ctrl) reg = random();
    context.gpr[0] = 0u;
    context.hi = random();
    context.lo = random();
    context.fcr31 = random();
    context.pc = mhp3rd::native::kMatrixCopyAddress;
}

void differential(const char *path) {
    using namespace psprecomp;
    using namespace mhp3rd::native;
    const auto elf = Elf32Image::from_file(path);
    Runtime original(elf.required_ram_size()), candidate(elf.required_ram_size());
    (void)elf.load_and_relocate(original.memory());
    (void)elf.load_and_relocate(candidate.memory());

    Runtime unknown(elf.required_ram_size());
    check(!install_matrix_copy(unknown, MatrixCopyMode::Native) &&
          matrix_copy_stats().calls == 0u && matrix_copy_stats().errors == 1u &&
          !unknown.has_function(kMatrixCopyAddress),
          "unknown code records an installation error with zero calls");
    check(!install_matrix_copy(candidate, MatrixCopyMode::Off) &&
          !candidate.has_function(kMatrixCopyAddress), "off mode leaves the original function installed");
    check(install_matrix_copy(candidate, MatrixCopyMode::Native),
          "full supported ELF fingerprint installs matrix copy");
    if (failures) return;

    constexpr std::uint32_t scratch = 0x08010000u;
    constexpr std::uint32_t external_return = 0x08020000u;
    constexpr std::array<std::uint32_t, 8> edge_bits{
        0u, 0x80000000u, 1u, 0x3f800000u, 0x7f800000u,
        0xff800000u, 0x7fc01234u, 0x7fa05678u};
    constexpr std::array<std::uint32_t, 4> aliases{0u, 0x40000000u, 0x80000000u, 0xa0000000u};
    std::mt19937 random(0x4d435059u);
    std::uint64_t cases = 0;
    const auto run = [&](unsigned source_offset, int displacement, unsigned source_alias,
                         unsigned destination_alias, bool edge_values) {
        std::array<std::uint8_t, 192> initial{};
        for (auto &byte : initial) byte = static_cast<std::uint8_t>(random());
        if (edge_values) {
            for (unsigned row = 0; row < 3; ++row) {
                for (unsigned column = 0; column < 3; ++column) {
                    const auto value = edge_bits[(cases + row * 3u + column) % edge_bits.size()];
                    const unsigned start = source_offset + row * 16u + column * 4u;
                    for (unsigned byte = 0; byte < 4; ++byte)
                        initial[start + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
                }
            }
        }
        original.memory().copy_in(scratch, initial);
        candidate.memory().copy_in(scratch, initial);
        const auto physical_source = scratch + source_offset;
        const auto physical_destination = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(physical_source) + displacement);
        AllegrexContext expected;
        fill_cpu(expected, random);
        expected.gpr[4] = physical_destination | aliases[destination_alias];
        expected.gpr[5] = physical_source | aliases[source_alias];
        expected.gpr[31] = cases % 97u == 0u ? kMatrixCopyAddress : external_return;
        auto actual = expected;
        const auto padding0 = candidate.memory().load32(expected.gpr[4] + 12u);
        const auto padding1 = candidate.memory().load32(expected.gpr[4] + 28u);
        const auto padding2 = candidate.memory().load32(expected.gpr[4] + 44u);
        check(run_original(original, expected), "local ELF reference returns within the instruction bound");
        check(apply_matrix_copy(candidate.memory(), actual), "mapped matrix copy accepts the case");
        check(same_cpu(expected, actual), "all CPU fields match local ELF including final VFPU bits");
        std::array<std::uint8_t, 192> want{}, got{};
        original.memory().copy_out(scratch, want);
        candidate.memory().copy_out(scratch, got);
        check(want == got, "entire output, canaries and aliases match local ELF");
        check(candidate.memory().load32(actual.gpr[4] + 12u) == padding0 &&
              candidate.memory().load32(actual.gpr[4] + 28u) == padding1 &&
              candidate.memory().load32(actual.gpr[4] + 44u) == padding2,
              "destination row padding remains untouched");
        ++cases;
    };

    // All displacements spanning the three rows, in both directions, with
    // alternating cached aliases and unaligned base pointers.
    for (int displacement = -43; displacement <= 43 && failures == 0; ++displacement)
        for (unsigned alias = 0; alias < aliases.size(); ++alias)
            run(64u + static_cast<unsigned>(displacement + 43) % 4u, displacement,
                alias, (alias + 1u) % aliases.size(), true);
    for (unsigned i = 0; i < 100000u && failures == 0; ++i) {
        const unsigned source_offset = 48u + random() % 49u;
        const int displacement = static_cast<int>(random() % 87u) - 43;
        run(source_offset, displacement, random() % aliases.size(), random() % aliases.size(),
            i % 11u == 0u);
    }

    AllegrexContext invalid;
    fill_cpu(invalid, random);
    invalid.gpr[4] = scratch + 32u;
    invalid.gpr[5] = GuestMemory::kPhysicalBase + candidate.memory().size() - 32u;
    invalid.gpr[31] = external_return;
    const auto unchanged_cpu = invalid;
    std::array<std::uint8_t, 192> unchanged{};
    candidate.memory().copy_out(scratch, unchanged);
    check(!apply_matrix_copy(candidate.memory(), invalid) && same_cpu(invalid, unchanged_cpu),
          "invalid later source row rejects without changing CPU state");
    std::array<std::uint8_t, 192> after_invalid{};
    candidate.memory().copy_out(scratch, after_invalid);
    check(after_invalid == unchanged, "invalid later source row causes no partial memory writes");
    invalid.gpr[5] = scratch + 64u;
    invalid.gpr[4] = 0xfffffff0u;
    check(!apply_matrix_copy(candidate.memory(), invalid) && invalid.pc == unchanged_cpu.pc,
          "wrapping destination span is refused before writing");

    check(install_matrix_copy(candidate, MatrixCopyMode::Native), "native hook resets counters");
    AllegrexContext native_context;
    native_context.gpr[4] = scratch + 24u;
    native_context.gpr[5] = scratch + 64u;
    native_context.gpr[31] = external_return;
    check(candidate.invoke_isolated_aot(kMatrixCopyAddress, native_context),
          "native dispatch reaches matrix copy hook");
    check(matrix_copy_stats().calls == 1u && matrix_copy_stats().native == 1u &&
          matrix_copy_stats().verified == 0u && matrix_copy_stats().fallbacks == 0u &&
          matrix_copy_stats().mismatches == 0u && matrix_copy_stats().errors == 0u,
          "native call commits one replacement with six correct counters");

    check(install_matrix_copy(candidate, MatrixCopyMode::Verify), "verification hook installs");
    AllegrexContext verify_context;
    fill_cpu(verify_context, random);
    verify_context.gpr[4] = scratch + 24u;
    verify_context.gpr[5] = (scratch + 48u) | 0x40000000u;
    verify_context.gpr[31] = kMatrixCopyAddress;
    check(candidate.invoke_isolated_aot(kMatrixCopyAddress, verify_context),
          "verifier executes original with an internal return PC");
    check(matrix_copy_stats().calls == 1u && matrix_copy_stats().verified == 1u &&
          matrix_copy_stats().native == 0u && matrix_copy_stats().fallbacks == 0u &&
          matrix_copy_stats().mismatches == 0u && matrix_copy_stats().errors == 0u,
          "verified call retains original result and compares all state");

    // Installation checks the full span; a changed byte after installation
    // also gives the verifier a controlled behavioral mismatch.
    const auto first_word = candidate.memory().load32(kMatrixCopyAddress);
    candidate.memory().store32(kMatrixCopyAddress, first_word ^ 4u);
    check(!install_matrix_copy(candidate, MatrixCopyMode::Native), "changed full-span fingerprint is refused");
    check(matrix_copy_stats().errors == 1u && matrix_copy_stats().calls == 1u,
          "failed reinstallation records an error without resetting prior counters");
    candidate.memory().store32(scratch + 48u, 0x11111111u);
    candidate.memory().store32(scratch + 52u, 0x22222222u);
    verify_context = {};
    verify_context.gpr[4] = scratch + 24u;
    verify_context.gpr[5] = scratch + 48u;
    verify_context.gpr[31] = external_return;
    check(candidate.invoke_isolated_aot(kMatrixCopyAddress, verify_context),
          "changed original still executes under installed verifier");
    check(matrix_copy_stats().calls == 2u && matrix_copy_stats().verified == 2u &&
          matrix_copy_stats().mismatches == 1u,
          "verifier detects changed original while retaining its result");
    candidate.memory().store32(kMatrixCopyAddress, first_word);
    verify_context = {};
    verify_context.gpr[4] = scratch + 24u;
    verify_context.gpr[5] = scratch + 48u;
    verify_context.gpr[31] = external_return;
    check(candidate.invoke_isolated_aot(kMatrixCopyAddress, verify_context) &&
          matrix_copy_stats().calls == 3u && matrix_copy_stats().fallbacks == 1u &&
          matrix_copy_stats().verified == 2u,
          "after mismatch all later calls use the original path");

    check(install_matrix_copy(candidate, MatrixCopyMode::Verify), "verifier reinstalls for bounded-exit test");
    const auto return_word = candidate.memory().load32(kMatrixCopyAddress + 0x48u);
    candidate.memory().store32(kMatrixCopyAddress + 0x48u, 0u);
    verify_context = {};
    verify_context.gpr[4] = scratch + 24u;
    verify_context.gpr[5] = scratch + 48u;
    verify_context.gpr[31] = external_return;
    check(candidate.invoke_isolated_aot(kMatrixCopyAddress, verify_context),
          "broken return reaches the bounded reference");
    check(candidate.stopped() && matrix_copy_stats().calls == 1u &&
          matrix_copy_stats().verified == 0u && matrix_copy_stats().native == 0u &&
          matrix_copy_stats().errors == 1u,
          "bounded reference stops before executing outside the leaf");
    candidate.memory().store32(kMatrixCopyAddress + 0x48u, return_word);

    Runtime invalid_runtime(elf.required_ram_size());
    (void)elf.load_and_relocate(invalid_runtime.memory());
    check(install_matrix_copy(invalid_runtime, MatrixCopyMode::Native),
          "native hook installs for invalid-span fallback test");
    invalid = {};
    invalid.gpr[4] = scratch + 24u;
    invalid.gpr[5] = GuestMemory::kPhysicalBase + invalid_runtime.memory().size() - 32u;
    invalid.gpr[31] = external_return;
    bool original_threw = false;
    try { (void)invalid_runtime.invoke_isolated_aot(kMatrixCopyAddress, invalid); }
    catch (const std::exception &) { original_threw = true; }
    check(original_threw && matrix_copy_stats().calls == 1u &&
          matrix_copy_stats().fallbacks == 1u && matrix_copy_stats().errors == 1u &&
          matrix_copy_stats().native == 0u,
          "invalid span falls back to original and records its memory error");

    std::cout << "Matrix-copy local ELF differential cases: " << cases << '\n';
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 2) {
        std::cerr << "Usage: mhp3rd_native_matrix_copy_tests [local EBOOT.ELF]\n";
        return 2;
    }
    try {
        portable_examples();
        if (argc == 2) differential(argv[1]);
    } catch (const std::exception &error) {
        std::cerr << "Matrix-copy test setup failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Native matrix-copy failures: " << failures << '\n';
    return failures ? 1 : 0;
}
