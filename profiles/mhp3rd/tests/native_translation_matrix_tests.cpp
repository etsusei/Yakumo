#include "native/translation_matrix.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <bit>
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
void examples() {
    const auto result = mhp3rd::native::translation_matrix(0x80000000u, 0x7fc01234u, 0x40000000u);
    for (unsigned i = 0; i < 16; ++i)
        check(result[i] == (i == 12 ? 0x80000000u : i == 13 ? 0x7fc01234u : i == 14 ? 0x40000000u :
                            i % 5u == 0u ? 0x3f800000u : 0u), "translation matrix preserves input bits and homogeneous identity");
}
void differential(const char *path) {
    using namespace psprecomp;
    using namespace mhp3rd::native;
    const auto elf = Elf32Image::from_file(path);
    Runtime oracle(elf.required_ram_size()), replacement(elf.required_ram_size());
    (void)elf.load_and_relocate(oracle.memory());
    (void)elf.load_and_relocate(replacement.memory());
    Runtime unrecognized(elf.required_ram_size());
    check(!install_translation_matrix(unrecognized, TranslationMatrixMode::Native) &&
          translation_matrix_stats().calls == 0 && translation_matrix_stats().errors == 1 &&
          !unrecognized.has_function(kTranslationMatrixAddress),
          "failed installation records an error with zero calls and installs no hook");
    check(!install_translation_matrix(replacement, TranslationMatrixMode::Off) &&
          !replacement.has_function(kTranslationMatrixAddress), "off mode installs no hook");
    check(install_translation_matrix(replacement, TranslationMatrixMode::Native), "supported function fingerprint is accepted");
    if (failures) return;
    constexpr std::uint32_t scratch = 0x08010000u, return_pc = 0x08020000u;
    std::mt19937 random(0x5452414eu);
    std::uint64_t cases = 0, fallbacks = 0;
    const auto run = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z, bool unusual) {
        std::array<std::uint8_t, 96> initial{};
        for (auto &b : initial) b = static_cast<std::uint8_t>(random());
        oracle.memory().copy_in(scratch, initial);
        replacement.memory().copy_in(scratch, initial);
        AllegrexContext expected;
        for (auto &r : expected.gpr) r = random();
        for (auto &r : expected.fpr) r = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
        for (auto &r : expected.vfpu) r = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
        for (auto &r : expected.vfpu_ctrl) r = random();
        expected.gpr[0] = 0; expected.gpr[4] = (scratch + 16u + static_cast<std::uint32_t>(cases % 4u)) | (cases % 2 ? 0x40000000u : 0u);
        expected.gpr[31] = return_pc; expected.pc = kTranslationMatrixAddress;
        expected.hi = random(); expected.lo = random(); expected.fcr31 = random();
        expected.set_fpr_bits(12, x); expected.set_fpr_bits(13, y); expected.set_fpr_bits(14, z);
        expected.vfpu_ctrl[0] = unusual ? 0x0001f000u : 0xe4u;
        expected.vfpu_ctrl[1] = 0xe4u;
        expected.vfpu_ctrl[2] = unusual ? (random() & 0xfffu) : 0;
        const auto input = expected;
        auto actual = expected;
        for (unsigned slice = 0; slice < 12 && expected.pc != return_pc; ++slice)
            (void)interpret_allegrex(oracle, expected, 1);
        check(expected.pc == return_pc, "reference returns within the slice bound");
        if (!apply_translation_matrix(replacement.memory(), actual)) {
            ++fallbacks;
            check(same_cpu(actual, input), "prefix rejection leaves all registers untouched");
            // The rejected native call must not have changed the destination.
            std::array<std::uint8_t, 96> unchanged{};
            replacement.memory().copy_out(scratch, unchanged);
            check(unchanged == initial, "prefix rejection leaves output memory untouched");
            check(replacement.invoke_isolated_aot(kTranslationMatrixAddress, actual), "unusual prefix reaches original fallback");
        }
        check(same_cpu(expected, actual), "all CPU registers match the original");
        std::array<std::uint8_t, 96> want{}, got{};
        oracle.memory().copy_out(scratch, want); replacement.memory().copy_out(scratch, got);
        check(want == got, "matrix and surrounding canaries match exactly");
        ++cases;
    };
    constexpr std::array<std::uint32_t, 8> edges{0u, 0x80000000u, 1u, 0x3f800000u, 0xbf800000u,
                                               0x7f800000u, 0x7fc01234u, 0x7fa05678u};
    for (auto x : edges) for (auto y : edges) for (auto z : edges) run(x, y, z, false);
    for (unsigned i = 0; i < 100000 && failures == 0; ++i) run(random(), random(), random(), i % 10 == 0);
    AllegrexContext invalid;
    invalid.gpr[4] = 0xffffffffu; invalid.gpr[31] = return_pc;
    invalid.vfpu_ctrl[0] = invalid.vfpu_ctrl[1] = 0xe4u;
    const auto invalid_before = invalid;
    std::array<std::uint8_t, 96> untouched_before{}, untouched_after{};
    replacement.memory().copy_out(scratch, untouched_before);
    check(!apply_translation_matrix(replacement.memory(), invalid) && same_cpu(invalid, invalid_before),
          "invalid output span is rejected without changing CPU state");
    replacement.memory().copy_out(scratch, untouched_after);
    check(untouched_before == untouched_after, "invalid output rejection leaves scratch memory unchanged");
    check(install_translation_matrix(replacement, TranslationMatrixMode::Native),
          "native hook resets counters after differential fallback cases");
    AllegrexContext native_ctx;
    native_ctx.gpr[4] = scratch + 16u; native_ctx.gpr[31] = return_pc;
    native_ctx.vfpu_ctrl[0] = native_ctx.vfpu_ctrl[1] = 0xe4u;
    native_ctx.set_fpr_bits(12, 0x80000000u);
    native_ctx.set_fpr_bits(13, 0x7fc01234u);
    native_ctx.set_fpr_bits(14, 0x3f800000u);
    check(replacement.memory().contains(native_ctx.gpr[4], 64u) &&
          native_ctx.vfpu_ctrl[0] == 0xe4u && native_ctx.vfpu_ctrl[1] == 0xe4u &&
          native_ctx.vfpu_ctrl[2] == 0u, "native sample satisfies the translation contract");
    check(replacement.invoke_isolated_aot(kTranslationMatrixAddress, native_ctx), "native dispatch reaches the hook");
    check(translation_matrix_stats().calls == 1 && translation_matrix_stats().native == 1 &&
          translation_matrix_stats().verified == 0 && translation_matrix_stats().fallbacks == 0 &&
          translation_matrix_stats().mismatches == 0 && translation_matrix_stats().errors == 0,
          "native call commits one replacement and reports all six counters");
    check(install_translation_matrix(replacement, TranslationMatrixMode::Verify), "runtime verifier installs");
    AllegrexContext ctx;
    ctx.pc = kTranslationMatrixAddress; ctx.gpr[4] = scratch + 16; ctx.gpr[31] = kTranslationMatrixAddress;
    ctx.vfpu_ctrl[0] = ctx.vfpu_ctrl[1] = 0xe4u;
    ctx.fpr[12] = 2.f; ctx.fpr[13] = 3.f; ctx.fpr[14] = 4.f;
    check(replacement.invoke_isolated_aot(kTranslationMatrixAddress, ctx), "dispatch reaches live verifier");
    check(translation_matrix_stats().calls == 1 && translation_matrix_stats().verified == 1 &&
          translation_matrix_stats().native == 0 && translation_matrix_stats().fallbacks == 0 &&
          translation_matrix_stats().mismatches == 0 && translation_matrix_stats().errors == 0,
          "verifier accepts matching outputs and internal return PC with six counters");
    ctx = {};
    ctx.gpr[4] = scratch + 16u; ctx.gpr[31] = return_pc;
    ctx.vfpu_ctrl[0] = 0x1f000u; ctx.vfpu_ctrl[1] = 0xe4u;
    ctx.fpr[12] = 2.f; ctx.fpr[13] = 3.f; ctx.fpr[14] = 4.f;
    check(replacement.invoke_isolated_aot(kTranslationMatrixAddress, ctx), "unusual prefix reaches original fallback");
    check(translation_matrix_stats().calls == 2 && translation_matrix_stats().verified == 1 &&
          translation_matrix_stats().fallbacks == 1 && translation_matrix_stats().mismatches == 0 &&
          translation_matrix_stats().errors == 0, "unsupported prefix increments only the fallback counter");
    replacement.memory().store32(kTranslationMatrixAddress, replacement.memory().load32(kTranslationMatrixAddress) ^ 1u);
    check(!install_translation_matrix(replacement, TranslationMatrixMode::Native), "modified function is refused");
    check(translation_matrix_stats().errors == 1 && translation_matrix_stats().calls == 2,
          "fingerprint failure increments errors without replacing the installed hook");
    replacement.memory().store32(kTranslationMatrixAddress, replacement.memory().load32(kTranslationMatrixAddress) ^ 1u);

    check(install_translation_matrix(replacement, TranslationMatrixMode::Verify), "verifier reinstalls for bounded failure check");
    const auto return_word = replacement.memory().load32(kTranslationMatrixAddress + 28u);
    replacement.memory().store32(kTranslationMatrixAddress + 28u, 0u);
    ctx = {};
    ctx.gpr[4] = scratch + 16u; ctx.gpr[31] = return_pc;
    ctx.vfpu_ctrl[0] = ctx.vfpu_ctrl[1] = 0xe4u;
    ctx.fpr[12] = 2.f; ctx.fpr[13] = 3.f; ctx.fpr[14] = 4.f;
    check(replacement.invoke_isolated_aot(kTranslationMatrixAddress, ctx), "broken return reaches bounded reference");
    check(replacement.stopped() && translation_matrix_stats().calls == 1 &&
          translation_matrix_stats().verified == 0 && translation_matrix_stats().native == 0 &&
          translation_matrix_stats().errors == 1, "bounded reference failure stops and counts one error");
    replacement.memory().store32(kTranslationMatrixAddress + 28u, return_word);
    std::cout << "Translation differential cases: " << cases << ", prefix fallbacks: " << fallbacks << '\n';
}
}
int main(int argc, char **argv) {
    if (argc > 2) { std::cerr << "Usage: mhp3rd_native_translation_matrix_tests [local EBOOT.ELF]\n"; return 2; }
    try { examples(); if (argc == 2) differential(argv[1]); }
    catch (const std::exception &e) { std::cerr << "Test setup failed: " << e.what() << '\n'; return 1; }
    std::cout << "Native translation failures: " << failures << '\n';
    return failures ? 1 : 0;
}
