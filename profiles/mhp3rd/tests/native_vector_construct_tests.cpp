#include "native/vector_construct.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <random>

namespace {
int failures{};

void check(bool condition, const char *message) {
    if (!condition && ++failures <= 10) std::cerr << "FAIL: " << message << '\n';
}

bool same_cpu(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    return a.gpr == b.gpr && a.pc == b.pc && a.hi == b.hi && a.lo == b.lo && a.fcr31 == b.fcr31 &&
           a.vfpu_ctrl == b.vfpu_ctrl && std::memcmp(a.fpr.data(), b.fpr.data(), sizeof(a.fpr)) == 0 &&
           std::memcmp(a.vfpu.data(), b.vfpu.data(), sizeof(a.vfpu)) == 0;
}

bool original_with_bound(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    using namespace psprecomp;
    constexpr auto entry = mhp3rd::native::kVectorConstructAddress;
    const auto return_pc = ctx.gpr[31];
    for (unsigned slice = 0; slice < 8u; ++slice) {
        if (ctx.pc < entry || ctx.pc >= entry + 24u) return false;
        const bool returning = ctx.pc == entry + 0x10u;
        const auto exit = interpret_allegrex(runtime, ctx, 1u);
        if (exit == InterpreterExit::Stopped || exit == InterpreterExit::Unreachable) return false;
        if (returning && ctx.pc == return_pc) return true;
    }
    return false;
}

psprecomp::AllegrexContext random_context(std::mt19937 &random) {
    psprecomp::AllegrexContext ctx;
    for (auto &word : ctx.gpr) word = random();
    for (auto &word : ctx.fpr) word = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    for (auto &word : ctx.vfpu) word = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    for (auto &word : ctx.vfpu_ctrl) word = random();
    ctx.gpr[0] = 0u;
    ctx.hi = random();
    ctx.lo = random();
    ctx.fcr31 = random();
    ctx.pc = mhp3rd::native::kVectorConstructAddress;
    return ctx;
}

void examples() {
    using mhp3rd::native::vector_construct;
    check(vector_construct(0x80000000u, 0x7fc01234u, 0x7fa05678u) ==
              (std::array<std::uint32_t, 4>{0x80000000u, 0x7fc01234u, 0x7fa05678u, 0u}),
          "signed zero and both NaN payloads are copied as bits");
    check(vector_construct(1u, 0xffffffffu, 0x3f800000u) ==
              (std::array<std::uint32_t, 4>{1u, 0xffffffffu, 0x3f800000u, 0u}),
          "fourth word is literal zero regardless of the inputs");
}

void differential(const char *path) {
    using namespace psprecomp;
    using namespace mhp3rd::native;
    const auto elf = Elf32Image::from_file(path);
    Runtime oracle(elf.required_ram_size()), replacement(elf.required_ram_size());
    (void)elf.load_and_relocate(oracle.memory());
    (void)elf.load_and_relocate(replacement.memory());

    Runtime unrecognized(elf.required_ram_size());
    check(!install_vector_construct(unrecognized, VectorConstructMode::Native) &&
              vector_construct_stats().calls == 0 && vector_construct_stats().errors == 1 &&
              !unrecognized.has_function(kVectorConstructAddress),
          "unknown code refuses installation with one error and no hook");
    check(!install_vector_construct(replacement, VectorConstructMode::Off) &&
              !replacement.has_function(kVectorConstructAddress),
          "off mode installs no hook");
    check(install_vector_construct(replacement, VectorConstructMode::Native),
          "supported full-span fingerprint installs");
    if (failures) return;

    constexpr std::uint32_t ram_scratch = 0x08010000u;
    constexpr std::uint32_t vram_scratch = 0x04010000u;
    constexpr std::uint32_t return_pc = 0x08020000u;
    constexpr std::array<std::uint32_t, 4> aliases{0u, 0x40000000u, 0x80000000u, 0xc0000000u};
    std::mt19937 random(0x56454354u);
    std::uint64_t cases = 0;
    const auto run = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        const auto scratch = cases % 17u == 0u ? vram_scratch : ram_scratch;
        const auto destination = scratch + 16u + static_cast<std::uint32_t>(cases % 4u);
        std::array<std::uint8_t, 48> initial{};
        for (auto &byte : initial) byte = static_cast<std::uint8_t>(random());
        oracle.memory().copy_in(scratch, initial);
        replacement.memory().copy_in(scratch, initial);

        auto expected = random_context(random);
        expected.gpr[4] = destination | aliases[cases % aliases.size()];
        expected.gpr[31] = return_pc;
        expected.set_fpr_bits(12u, x);
        expected.set_fpr_bits(13u, y);
        expected.set_fpr_bits(14u, z);
        auto actual = expected;
        check(original_with_bound(oracle, expected), "original constructor returns within eight slices");
        check(apply_vector_construct(replacement.memory(), actual), "valid destination accepts native adapter");
        check(same_cpu(expected, actual), "all CPU state matches the original");
        std::array<std::uint8_t, 48> want{}, got{};
        oracle.memory().copy_out(scratch, want);
        replacement.memory().copy_out(scratch, got);
        check(want == got, "destination and surrounding canaries match exactly");
        ++cases;
    };

    constexpr std::array<std::uint32_t, 8> edges{0u, 0x80000000u, 1u, 0x3f800000u,
                                                 0xbf800000u, 0x7f800000u,
                                                 0x7fc01234u, 0x7fa05678u};
    for (auto x : edges) for (auto y : edges) for (auto z : edges) run(x, y, z);
    for (unsigned i = 0; i < 100000u && failures == 0; ++i) run(random(), random(), random());
    if (failures) return;

    // The actual registered hook must preserve prefixes and return through an
    // unaligned cached RAM alias, rather than only passing direct-adapter tests.
    std::array<std::uint8_t, 48> initial{};
    for (auto &byte : initial) byte = static_cast<std::uint8_t>(random());
    oracle.memory().copy_in(ram_scratch, initial);
    replacement.memory().copy_in(ram_scratch, initial);
    auto expected = random_context(random);
    expected.gpr[4] = (ram_scratch + 17u) | 0x80000000u;
    expected.gpr[31] = return_pc;
    expected.set_fpr_bits(12u, 0x80000000u);
    expected.set_fpr_bits(13u, 0x7fc01234u);
    expected.set_fpr_bits(14u, 0x7fa05678u);
    auto actual = expected;
    check(original_with_bound(oracle, expected), "native-hook oracle returns");
    check(replacement.invoke_isolated_aot(kVectorConstructAddress, actual), "native dispatch reaches hook");
    std::array<std::uint8_t, 48> want{}, got{};
    oracle.memory().copy_out(ram_scratch, want);
    replacement.memory().copy_out(ram_scratch, got);
    check(same_cpu(expected, actual) && want == got, "actual native hook matches CPU and memory oracle");
    check(vector_construct_stats().calls == 1 && vector_construct_stats().native == 1 &&
              vector_construct_stats().verified == 0 && vector_construct_stats().fallbacks == 0 &&
              vector_construct_stats().mismatches == 0 && vector_construct_stats().errors == 0,
          "native hook reports all six counters");

    check(install_vector_construct(replacement, VectorConstructMode::Verify), "verifier installs");
    for (auto &byte : initial) byte = static_cast<std::uint8_t>(random());
    oracle.memory().copy_in(ram_scratch, initial);
    replacement.memory().copy_in(ram_scratch, initial);
    expected = random_context(random);
    expected.gpr[4] = ram_scratch + 18u;
    expected.gpr[31] = kVectorConstructAddress + 4u; // Internal return PC still executes the delay slot.
    expected.set_fpr_bits(12u, 0x7fc01234u);
    expected.set_fpr_bits(13u, 0x80000000u);
    expected.set_fpr_bits(14u, 0x7fa05678u);
    actual = expected;
    check(original_with_bound(oracle, expected), "internal-return oracle executes through the delay slot");
    check(replacement.invoke_isolated_aot(kVectorConstructAddress, actual), "verifier dispatch reaches hook");
    oracle.memory().copy_out(ram_scratch, want);
    replacement.memory().copy_out(ram_scratch, got);
    check(same_cpu(expected, actual) && want == got, "verifier retains original CPU and memory result");
    check(vector_construct_stats().calls == 1 && vector_construct_stats().verified == 1 &&
              vector_construct_stats().native == 0 && vector_construct_stats().fallbacks == 0 &&
              vector_construct_stats().mismatches == 0 && vector_construct_stats().errors == 0,
          "verifier handles internal return and arbitrary VFPU prefixes");

    // Change one instruction after installation. Verification must leave the
    // altered original result in memory and disable later native replacement.
    const auto first_word = replacement.memory().load32(kVectorConstructAddress);
    oracle.memory().store32(kVectorConstructAddress, 0u);
    replacement.memory().store32(kVectorConstructAddress, 0u);
    initial.fill(0xadu);
    oracle.memory().copy_in(ram_scratch, initial);
    replacement.memory().copy_in(ram_scratch, initial);
    expected = random_context(random);
    expected.gpr[4] = ram_scratch + 16u;
    expected.gpr[31] = return_pc;
    expected.set_fpr_bits(12u, 0x12345678u);
    actual = expected;
    check(original_with_bound(oracle, expected), "altered original still returns");
    check(replacement.invoke_isolated_aot(kVectorConstructAddress, actual), "mismatch case reaches verifier");
    oracle.memory().copy_out(ram_scratch, want);
    replacement.memory().copy_out(ram_scratch, got);
    check(same_cpu(expected, actual) && want == got && replacement.memory().load32(ram_scratch + 16u) == 0xadadadadu,
          "mismatch retains original memory without speculative writes");
    check(vector_construct_stats().calls == 2 && vector_construct_stats().verified == 2 &&
              vector_construct_stats().mismatches == 1 && vector_construct_stats().native == 0 &&
              vector_construct_stats().errors == 0, "mismatch is counted once");
    oracle.memory().store32(kVectorConstructAddress, first_word);
    replacement.memory().store32(kVectorConstructAddress, first_word);

    initial.fill(0x5au);
    oracle.memory().copy_in(ram_scratch, initial);
    replacement.memory().copy_in(ram_scratch, initial);
    expected.set_fpr_bits(12u, 0x87654321u);
    expected.pc = kVectorConstructAddress;
    actual = expected;
    check(original_with_bound(oracle, expected), "post-mismatch original returns");
    check(replacement.invoke_isolated_aot(kVectorConstructAddress, actual), "post-mismatch fallback dispatches");
    oracle.memory().copy_out(ram_scratch, want);
    replacement.memory().copy_out(ram_scratch, got);
    check(same_cpu(expected, actual) && want == got && vector_construct_stats().calls == 3 &&
              vector_construct_stats().verified == 2 && vector_construct_stats().fallbacks == 1 &&
              vector_construct_stats().mismatches == 1 && vector_construct_stats().errors == 0,
          "later calls use original reference after mismatch");

    // The fingerprint includes the return delay slot, not just the stores.
    const auto delay_word = replacement.memory().load32(kVectorConstructAddress + 0x14u);
    replacement.memory().store32(kVectorConstructAddress + 0x14u, 0x24020001u);
    check(!install_vector_construct(replacement, VectorConstructMode::Native) &&
              vector_construct_stats().errors == 1 && vector_construct_stats().calls == 3,
          "changed delay slot refuses installation without resetting evidence");
    replacement.memory().store32(kVectorConstructAddress + 0x14u, delay_word);

    // A partial tail span could otherwise write three words before failing.
    const auto partial = GuestMemory::kPhysicalBase + replacement.memory().size() - 12u;
    std::array<std::uint8_t, 28> tail{}, tail_after{};
    for (auto &byte : tail) byte = static_cast<std::uint8_t>(random());
    replacement.memory().copy_in(partial - 16u, tail);
    actual = random_context(random);
    actual.gpr[4] = partial;
    const auto before = actual;
    check(!apply_vector_construct(replacement.memory(), actual), "partial destination span is rejected");
    replacement.memory().copy_out(partial - 16u, tail_after);
    check(same_cpu(before, actual) && tail_after == tail, "invalid span leaves context and memory untouched");

    // The hook records an unsupported span as a fallback. The original's
    // first store then raises the guest-memory error for this unmapped address.
    Runtime invalid(elf.required_ram_size());
    (void)elf.load_and_relocate(invalid.memory());
    check(install_vector_construct(invalid, VectorConstructMode::Native),
          "native hook installs for invalid-range fallback check");
    actual = random_context(random);
    actual.gpr[4] = 0x00001000u;
    actual.gpr[31] = return_pc;
    bool original_failed = false;
    try {
        (void)invalid.invoke_isolated_aot(kVectorConstructAddress, actual);
    } catch (const std::exception &) {
        original_failed = true;
    }
    check(original_failed && vector_construct_stats().calls == 1 &&
              vector_construct_stats().fallbacks == 1 && vector_construct_stats().errors == 1 &&
              vector_construct_stats().native == 0 && vector_construct_stats().verified == 0,
          "invalid hooked call falls back to the original and reports its memory error");

    check(install_vector_construct(replacement, VectorConstructMode::Verify),
          "verifier reinstalls for bounded-reference failure");
    const auto return_word = replacement.memory().load32(kVectorConstructAddress + 0x10u);
    replacement.memory().store32(kVectorConstructAddress + 0x10u, 0u);
    actual = random_context(random);
    actual.gpr[4] = ram_scratch + 16u;
    actual.gpr[31] = return_pc;
    check(replacement.invoke_isolated_aot(kVectorConstructAddress, actual), "broken return reaches verifier");
    check(replacement.stopped() && vector_construct_stats().calls == 1 &&
              vector_construct_stats().verified == 0 && vector_construct_stats().native == 0 &&
              vector_construct_stats().mismatches == 0 && vector_construct_stats().errors == 1,
          "reference cannot escape bounded leaf and records one error");
    replacement.memory().store32(kVectorConstructAddress + 0x10u, return_word);

    std::cout << "Vector constructor differential cases: " << cases << '\n';
}
} // namespace

int main(int argc, char **argv) {
    if (argc > 2) {
        std::cerr << "Usage: mhp3rd_native_vector_construct_tests [local EBOOT.ELF]\n";
        return 2;
    }
    try {
        examples();
        if (argc == 2) differential(argv[1]);
    } catch (const std::exception &e) {
        std::cerr << "Test setup failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "Native vector constructor failures: " << failures << '\n';
    return failures ? 1 : 0;
}
