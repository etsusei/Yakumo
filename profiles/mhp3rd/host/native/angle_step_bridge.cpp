#include "native/angle_step_bridge.hpp"
#include "native/angle_step.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

namespace mhp3rd::native {
namespace {
// Independently inspected from the user's supported NPJB-40001 executable.
// This fingerprint identifies the whole leaf, including both return paths;
// no executable bytes or generated game source are distributed here.
constexpr const char *kCodeSha256 = "c80198f08479038b9d5e0a26516fa8c180f60f11e11bf7521f21208efe63ee49";
AngleStepMode mode = AngleStepMode::Off;
AngleStepStats stats;

bool same_context(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    return a.gpr == b.gpr && a.hi == b.hi && a.lo == b.lo && a.pc == b.pc && a.fcr31 == b.fcr31 &&
           a.vfpu_ctrl == b.vfpu_ctrl && std::memcmp(a.fpr.data(), b.fpr.data(), sizeof(a.fpr)) == 0 &&
           std::memcmp(a.vfpu.data(), b.vfpu.data(), sizeof(a.vfpu)) == 0;
}

// The reference executes bytes loaded from the player's ELF, independently
// of the native formula. One-instruction slices also work when AOT registered
// every address. A branch and its delay slot count as one slice here.
bool reference(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t return_pc = ctx.gpr[31];
    for (unsigned slice = 0; slice < 32; ++slice) {
        if (ctx.pc < kAngleStepAddress || ctx.pc >= kAngleStepAddress + kAngleStepCodeSize) break;
        // Stop after executing either verified return instruction and its
        // delay slot, even if the caller's return address is inside this leaf.
        const bool returning = ctx.pc == kAngleStepAddress + 0x48u || ctx.pc == kAngleStepAddress + 0x5cu;
        const auto exit = psprecomp::interpret_allegrex(runtime, ctx, 1);
        if (exit == psprecomp::InterpreterExit::Stopped || exit == psprecomp::InterpreterExit::Unreachable) break;
        if (returning && ctx.pc == return_pc) return true;
    }
    runtime.stop("Native angle-step reference left its bounded leaf");
    return false;
}

void bridge(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    ++stats.calls;
    if (mode == AngleStepMode::Native) {
        apply_angle_step(runtime.memory(), ctx);
    } else if (stats.mismatches != 0) {
        (void)reference(runtime, ctx);
    } else {
        auto &memory = runtime.memory();
        const auto current_address = ctx.gpr[4], target_address = ctx.gpr[5];
        const auto old_current = memory.load32(current_address), old_target = memory.load32(target_address);
        auto prediction = ctx;
        apply_angle_step(memory, prediction);
        const auto predicted_current = memory.load32(current_address), predicted_target = memory.load32(target_address);
        // Restore both inputs, including when they alias or partially overlap.
        memory.store32(current_address, old_current);
        memory.store32(target_address, old_target);
        if (!reference(runtime, ctx)) return;
        ++stats.verified;
        if (!same_context(ctx, prediction) || memory.load32(current_address) != predicted_current ||
            memory.load32(target_address) != predicted_target) {
            ++stats.mismatches;
            std::cerr << "[native-angle] mismatch; retaining the original result and using the reference until exit\n";
        }
        // Verification leaves the original result in place. The native result
        // is only used by an explicit subsequent run in Native mode.
    }
    if (stats.calls == 1 || stats.calls % 1024u == 0) report_angle_step();
}
} // namespace

void apply_angle_step(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx) {
    const auto current_address = ctx.gpr[4], target_address = ctx.gpr[5];
    const auto target = memory.load16(target_address);
    // This write precedes reading current in the original. Its order matters
    // when the two pointers name the same word, including cached RAM aliases.
    memory.store32(target_address, target);
    const auto current = memory.load32(current_address);
    const auto result = step_angle(current, target, std::bit_cast<std::int32_t>(ctx.gpr[6]));
    memory.store32(current_address, result.value);
    // Preserve all observable caller-saved register outputs, not just v0.
    ctx.gpr[2] = std::bit_cast<std::uint32_t>(result.amount);
    ctx.gpr[3] = 0u - result.forward_distance;
    ctx.gpr[5] = current;
    ctx.gpr[6] = ctx.gpr[2];
    ctx.gpr[7] = current_address;
    ctx.gpr[8] = result.backwards ? 1u : 0u;
    ctx.pc = ctx.gpr[31];
}

bool install_angle_step(psprecomp::Runtime &runtime, AngleStepMode requested) {
    if (requested == AngleStepMode::Off) return false;
    std::array<std::uint8_t, kAngleStepCodeSize> code{};
    if (!runtime.memory().contains(kAngleStepAddress, code.size())) return false;
    for (std::size_t i = 0; i < code.size(); ++i)
        code[i] = runtime.memory().load8(kAngleStepAddress + static_cast<std::uint32_t>(i));
    if (psprecomp::sha256_bytes(code) != kCodeSha256) {
        std::cerr << "[native-angle] code fingerprint differs; original implementation retained\n";
        return false;
    }
    mode = requested;
    stats = {};
    runtime.register_function(kAngleStepAddress, &bridge, "mhp3rd_native_angle_step");
    std::cout << "[native-angle] installed " << (mode == AngleStepMode::Verify ? "verify" : "native")
              << " at 0x088775ac\n";
    return true;
}

void configure_angle_step(psprecomp::Runtime &runtime) {
    const char *value = std::getenv("MHP3RD_NATIVE_ANGLE_STEP");
    if (value == nullptr || std::string_view(value) == "off" || std::string_view(value) == "0") return;
    if (std::string_view(value) == "verify") (void)install_angle_step(runtime, AngleStepMode::Verify);
    else if (std::string_view(value) == "native") (void)install_angle_step(runtime, AngleStepMode::Native);
    else std::cerr << "[native-angle] expected off, verify or native; original implementation retained\n";
}

AngleStepStats angle_step_stats() { return stats; }
void report_angle_step() {
    if (mode == AngleStepMode::Off) return;
    std::cout << "[native-angle] calls=" << stats.calls << " verified=" << stats.verified
              << " mismatches=" << stats.mismatches << '\n';
}
} // namespace mhp3rd::native
