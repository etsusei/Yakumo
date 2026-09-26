#include "native/angle_step.hpp"
#include "native/angle_step_bridge.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>

namespace {
int failures{};
void check(bool condition, const char *message) {
    if (!condition && ++failures <= 10) std::cerr << "FAIL: " << message << '\n';
}
void examples() {
    using mhp3rd::native::step_angle;
    check(step_angle(65530u, 5u, 10).value == 65540u, "forward wrap keeps the unwrapped accumulator");
    check(step_angle(5u, 65530u, 10).value == 0xfffffffbu, "backward wrap preserves unsigned subtraction");
    check(step_angle(0u, 32768u, 3).value == 0xfffffffdu, "half-turn tie goes backwards");
    check(step_angle(123u, 123u, 9).amount == 0, "already at target");
    check(step_angle(100u, 110u, 50).value == 110u, "step does not overshoot");
    check(step_angle(100u, 110u, 0).value == 100u, "zero step holds position");
    check(step_angle(100u, 110u, -3).value == 97u, "negative limits preserve original signed behavior");
    check(step_angle(0xffff0064u, 110u, 3).value == 0xffff0067u, "high accumulator bits survive");
}
bool same_cpu(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    return a.gpr == b.gpr && a.pc == b.pc && a.hi == b.hi && a.lo == b.lo && a.fcr31 == b.fcr31 &&
           a.vfpu_ctrl == b.vfpu_ctrl && std::memcmp(a.fpr.data(), b.fpr.data(), sizeof(a.fpr)) == 0 &&
           std::memcmp(a.vfpu.data(), b.vfpu.data(), sizeof(a.vfpu)) == 0;
}

// Original bytes are loaded only from the user's local ELF; none are embedded
// in this test. Every case compares registers and the entire scratch region.
void differential(const char *path) {
    using namespace psprecomp;
    using namespace mhp3rd::native;
    const auto elf = Elf32Image::from_file(path);
    Runtime oracle(elf.required_ram_size()), replacement(elf.required_ram_size());
    (void)elf.load_and_relocate(oracle.memory());
    (void)elf.load_and_relocate(replacement.memory());
    check(install_angle_step(replacement, AngleStepMode::Native), "supported code fingerprint is accepted");
    if (failures) return;
    constexpr std::uint32_t scratch = 0x08010000u;
    constexpr std::uint32_t return_pc = 0x08020000u;
    std::mt19937 random(0x4d485033u);
    constexpr std::array<std::int32_t, 12> limits{0, 1, 3, 512, 32767, 32768, 65535, 65536,
        -1, -32768, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
    std::uint64_t cases = 0;
    const auto run = [&](std::uint32_t current, std::uint32_t target, std::int32_t limit, unsigned layout) {
        std::array<std::uint8_t, 64> initial{};
        for (auto &b : initial) b = static_cast<std::uint8_t>(random());
        oracle.memory().copy_in(scratch, initial);
        replacement.memory().copy_in(scratch, initial);
        const auto current_address = scratch + 16u;
        const auto target_address = layout == 1 || layout == 4 ? current_address
                                      : layout == 2 ? current_address + 2u : scratch + 32u;
        for (Runtime *runtime : {&oracle, &replacement}) {
            runtime->memory().store32(current_address, current);
            runtime->memory().store32(target_address, target);
        }
        AllegrexContext expected;
        for (auto &reg : expected.gpr) reg = random();
        for (auto &reg : expected.fpr) reg = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
        for (auto &reg : expected.vfpu) reg = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
        for (auto &reg : expected.vfpu_ctrl) reg = random();
        expected.gpr[0] = 0;
        expected.gpr[4] = current_address;
        expected.gpr[5] = target_address | (layout == 3 || layout == 4 ? 0x40000000u : 0u);
        expected.gpr[6] = std::bit_cast<std::uint32_t>(limit);
        expected.gpr[31] = return_pc;
        expected.hi = random(); expected.lo = random(); expected.fcr31 = random();
        expected.pc = kAngleStepAddress;
        auto actual = expected;
        for (unsigned slice = 0; slice < 32 && expected.pc != return_pc; ++slice)
            (void)interpret_allegrex(oracle, expected, 1);
        check(expected.pc == return_pc, "reference returns within its instruction bound");
        apply_angle_step(replacement.memory(), actual);
        check(same_cpu(expected, actual), "every CPU output matches the original instructions");
        for (std::uint32_t i = 0; i < initial.size(); ++i)
            if (oracle.memory().load8(scratch + i) != replacement.memory().load8(scratch + i)) {
                check(false, "memory and surrounding canaries match"); break;
            }
        ++cases;
    };
    // Cover every relative angle, all step boundaries and high accumulator bits.
    for (std::uint32_t delta = 0; delta < 65536u && failures == 0; ++delta)
        for (const auto limit : limits)
            run(0xffff0123u, 0xa5a50000u | ((0x123u + delta) & 0xffffu), limit, 0);
    // Broader inputs include same-pointer, partial-overlap and uncached aliases.
    for (unsigned i = 0; i < 20000 && failures == 0; ++i)
        run(random(), random(), std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random())), i % 5);

    check(install_angle_step(replacement, AngleStepMode::Verify), "verify mode installs");
    AllegrexContext ctx;
    ctx.pc = kAngleStepAddress; ctx.gpr[4] = scratch + 16; ctx.gpr[5] = scratch + 32;
    ctx.gpr[6] = 10; ctx.gpr[31] = return_pc;
    replacement.memory().store32(ctx.gpr[4], 65530);
    replacement.memory().store32(ctx.gpr[5], 0xffff0005u);
    check(replacement.invoke_isolated_aot(kAngleStepAddress, ctx), "runtime dispatch reaches the verifier");
    check(angle_step_stats().verified == 1 && angle_step_stats().mismatches == 0, "live verifier agrees");
    check(replacement.memory().load32(scratch + 16) == 65540u, "verification leaves original result in place");
    ctx.pc = kAngleStepAddress; ctx.gpr[4] = scratch + 16; ctx.gpr[5] = scratch + 32;
    ctx.gpr[6] = 1; ctx.gpr[31] = kAngleStepAddress;
    replacement.memory().store32(ctx.gpr[4], 100);
    replacement.memory().store32(ctx.gpr[5], 110);
    check(replacement.invoke_isolated_aot(kAngleStepAddress, ctx), "verifier accepts a return inside the leaf");
    check(angle_step_stats().verified == 2 && angle_step_stats().mismatches == 0 &&
              replacement.memory().load32(scratch + 16) == 101u, "reference executes before checking its return");
    const auto word = replacement.memory().load32(kAngleStepAddress);
    replacement.memory().store32(kAngleStepAddress, word ^ 1u);
    check(!install_angle_step(replacement, AngleStepMode::Native), "modified code is refused");
    std::cout << "Differential cases: " << cases << '\n';
}
}
int main(int argc, char **argv) {
    if (argc > 2) {
        std::cerr << "Usage: mhp3rd_native_angle_tests [local EBOOT.ELF]\n";
        return 2;
    }
    try {
        examples();
        if (argc == 2) differential(argv[1]);
        else std::cout << "Pass a local supported EBOOT.ELF to run the original-code differential checks.\n";
    } catch (const std::exception &error) {
        std::cerr << "Test setup failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Native angle failures: " << failures << '\n';
    return failures ? 1 : 0;
}
