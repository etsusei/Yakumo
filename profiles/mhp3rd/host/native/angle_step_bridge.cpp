#include "native/angle_step_bridge.hpp"
#include "native/angle_step.hpp"
#include "native/bridge_contracts.hpp"

#include <bit>
#include <cstdlib>
#include <iostream>

namespace mhp3rd::native {
namespace {
// Independently inspected from the user's supported NPJB-40001 executable.
// This fingerprint identifies the whole leaf, including both return paths;
// no executable bytes or generated game source are distributed here.
constexpr const char *kCodeSha256 = "c80198f08479038b9d5e0a26516fa8c180f60f11e11bf7521f21208efe63ee49";
AngleStepMode mode = AngleStepMode::Off;
AngleStepStats stats;

// The reference executes bytes loaded from the player's ELF, independently
// of the native formula. One-instruction slices also work when AOT registered
// every address. A branch and its delay slot count as one slice here.
bool reference(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    // This leaf has two return paths. The common reference helper handles one
    // return at a time, so retain this small path-specific loop.
    const std::uint32_t return_pc = ctx.gpr[31];
    for (unsigned slice = 0; slice < 32; ++slice) {
        if (ctx.pc < kAngleStepAddress || ctx.pc >= kAngleStepAddress + kAngleStepCodeSize) break;
        const bool returning = ctx.pc == kAngleStepAddress + 0x48u || ctx.pc == kAngleStepAddress + 0x5cu;
        const auto exit = psprecomp::interpret_allegrex(runtime, ctx, 1);
        if (exit == psprecomp::InterpreterExit::Stopped || exit == psprecomp::InterpreterExit::Unreachable) break;
        if (returning && ctx.pc == return_pc) return true;
    }
    runtime.stop("Native angle-step reference left its bounded leaf");
    return false;
}

template <typename Memory>
void apply_to_memory(Memory &memory, psprecomp::AllegrexContext &ctx) {
    const auto current_address = ctx.gpr[4], target_address = ctx.gpr[5];
    const auto target = memory.load16(target_address);
    // This write precedes reading current in the original. Its order matters
    // when the pointers overlap, including through cached RAM aliases.
    memory.store32(target_address, target);
    const auto current = memory.load32(current_address);
    const auto result = step_angle(current, target, std::bit_cast<std::int32_t>(ctx.gpr[6]));
    memory.store32(current_address, result.value);
    ctx.gpr[2] = std::bit_cast<std::uint32_t>(result.amount);
    ctx.gpr[3] = 0u - result.forward_distance;
    ctx.gpr[5] = current;
    ctx.gpr[6] = ctx.gpr[2];
    ctx.gpr[7] = current_address;
    ctx.gpr[8] = result.backwards ? 1u : 0u;
    ctx.pc = ctx.gpr[31];
}

void bridge(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    ++stats.calls;
    try {
        if (stats.mismatches != 0) {
            ++stats.fallbacks;
            if (!reference(runtime, ctx)) ++stats.errors;
        } else if (mode == AngleStepMode::Native) {
            apply_angle_step(runtime.memory(), ctx);
            ++stats.native;
        } else {
            auto &memory = runtime.memory();
            MemoryShadow<8> shadow;
            if (!shadow.capture_word(memory, ctx.gpr[4]) || !shadow.capture_word(memory, ctx.gpr[5])) {
                ++stats.fallbacks;
                if (!reference(runtime, ctx)) ++stats.errors;
            } else {
                auto prediction = ctx;
                apply_to_memory(shadow, prediction);
                if (!reference(runtime, ctx)) { ++stats.errors; return; }
                ++stats.verified;
                if (!same_context(ctx, prediction) || !shadow.matches(memory)) {
                    ++stats.mismatches;
                    std::cerr << "[native-angle] mismatch; retaining the original result and using the reference until exit\n";
                }
            }
        }
    } catch (...) { ++stats.errors; throw; }
    if (stats.calls == 1 || stats.calls % 1024u == 0) report_angle_step();
}
} // namespace

void apply_angle_step(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx) {
    apply_to_memory(memory, ctx);
}

bool install_angle_step(psprecomp::Runtime &runtime, AngleStepMode requested) {
    if (requested == AngleStepMode::Off) return false;
    if (!matches_code_fingerprint<kAngleStepCodeSize>(runtime.memory(), kAngleStepAddress, kCodeSha256)) {
        ++stats.errors;
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
    const auto requested = parse_native_mode(value);
    if (!requested) {
        ++stats.errors;
        std::cerr << "[native-angle] expected off, verify or native; original implementation retained\n";
    } else if (*requested != AngleStepMode::Off) (void)install_angle_step(runtime, *requested);
}

AngleStepStats angle_step_stats() { return stats; }
void report_angle_step() {
    if (mode == AngleStepMode::Off && stats.errors == 0) return;
    std::cout << "[native-angle] calls=" << stats.calls << " verified=" << stats.verified
              << " native=" << stats.native << " fallbacks=" << stats.fallbacks
              << " mismatches=" << stats.mismatches << " errors=" << stats.errors << '\n';
}
} // namespace mhp3rd::native
