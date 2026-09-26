#include "native/scale_matrix.hpp"
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
    const auto result = mhp3rd::native::scale_matrix(0x80000000u, 0x7fc01234u, 0x40000000u);
    for (unsigned i = 0; i < 16; ++i)
        check(result[i] == (i == 0 ? 0x80000000u : i == 5 ? 0x7fc01234u : i == 10 ? 0x40000000u :
                            i == 15 ? 0x3f800000u : 0u), "scale matrix preserves input bits and homogeneous identity");
}
void differential(const char *path) {
    using namespace psprecomp;
    using namespace mhp3rd::native;
    const auto elf = Elf32Image::from_file(path);
    Runtime oracle(elf.required_ram_size()), replacement(elf.required_ram_size());
    (void)elf.load_and_relocate(oracle.memory());
    (void)elf.load_and_relocate(replacement.memory());
    check(install_scale_matrix(replacement, ScaleMatrixMode::Native), "supported function fingerprint is accepted");
    if (failures) return;
    constexpr std::uint32_t scratch = 0x08010000u, return_pc = 0x08020000u;
    std::mt19937 random(0x5343414cu);
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
        expected.gpr[0] = 0; expected.gpr[4] = (scratch + 16u) | (cases % 2 ? 0x40000000u : 0u);
        expected.gpr[31] = return_pc; expected.pc = kScaleMatrixAddress;
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
        if (!apply_scale_matrix(replacement.memory(), actual)) {
            ++fallbacks;
            check(same_cpu(actual, input), "prefix rejection leaves all registers untouched");
            // The rejected native call must not have changed the destination.
            std::array<std::uint8_t, 96> unchanged{};
            replacement.memory().copy_out(scratch, unchanged);
            check(unchanged == initial, "prefix rejection leaves output memory untouched");
            check(replacement.invoke_isolated_aot(kScaleMatrixAddress, actual), "unusual prefix reaches original fallback");
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
    check(install_scale_matrix(replacement, ScaleMatrixMode::Verify), "runtime verifier installs");
    AllegrexContext ctx;
    ctx.pc = kScaleMatrixAddress; ctx.gpr[4] = scratch + 16; ctx.gpr[31] = kScaleMatrixAddress;
    ctx.vfpu_ctrl[0] = ctx.vfpu_ctrl[1] = 0xe4u;
    ctx.fpr[12] = 2.f; ctx.fpr[13] = 3.f; ctx.fpr[14] = 4.f;
    check(replacement.invoke_isolated_aot(kScaleMatrixAddress, ctx), "dispatch reaches live verifier");
    check(scale_matrix_stats().verified == 1 && scale_matrix_stats().mismatches == 0,
          "live verifier accepts matching outputs and an internal return PC");
    replacement.memory().store32(kScaleMatrixAddress, replacement.memory().load32(kScaleMatrixAddress) ^ 1u);
    check(!install_scale_matrix(replacement, ScaleMatrixMode::Native), "modified function is refused");
    std::cout << "Scale differential cases: " << cases << ", prefix fallbacks: " << fallbacks << '\n';
}
}
int main(int argc, char **argv) {
    if (argc > 2) { std::cerr << "Usage: mhp3rd_native_scale_tests [local EBOOT.ELF]\n"; return 2; }
    try { examples(); if (argc == 2) differential(argv[1]); }
    catch (const std::exception &e) { std::cerr << "Test setup failed: " << e.what() << '\n'; return 1; }
    std::cout << "Native scale failures: " << failures << '\n';
    return failures ? 1 : 0;
}
