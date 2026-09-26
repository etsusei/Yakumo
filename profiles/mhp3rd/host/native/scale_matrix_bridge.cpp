#include "native/scale_matrix.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

namespace mhp3rd::native {
namespace {
// Complete function, including its return's memory-writing delay slot.
constexpr std::uint32_t kCodeSize = 36u;
constexpr const char *kCodeSha256 = "d9ad67fd4b7e26ea8b213c8297489c288c39161f9190bb267f9f5ade35a799b6";
ScaleMatrixMode mode = ScaleMatrixMode::Off;
ScaleMatrixStats stats;

bool same_context(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    return a.gpr == b.gpr && a.hi == b.hi && a.lo == b.lo && a.pc == b.pc && a.fcr31 == b.fcr31 &&
           a.vfpu_ctrl == b.vfpu_ctrl && std::memcmp(a.fpr.data(), b.fpr.data(), sizeof(a.fpr)) == 0 &&
           std::memcmp(a.vfpu.data(), b.vfpu.data(), sizeof(a.vfpu)) == 0;
}

bool reference(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto return_pc = ctx.gpr[31];
    for (unsigned slice = 0; slice < 12; ++slice) {
        if (ctx.pc < kScaleMatrixAddress || ctx.pc >= kScaleMatrixAddress + kCodeSize) break;
        const bool returning = ctx.pc == kScaleMatrixAddress + 28u;
        const auto exit = psprecomp::interpret_allegrex(runtime, ctx, 1);
        if (exit == psprecomp::InterpreterExit::Stopped || exit == psprecomp::InterpreterExit::Unreachable) break;
        if (returning && ctx.pc == return_pc) return true;
    }
    runtime.stop("Native scale-matrix reference left its bounded leaf");
    return false;
}

void bridge(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    ++stats.calls;
    if (stats.mismatches) {
        ++stats.fallbacks;
        (void)reference(runtime, ctx);
    } else if (mode == ScaleMatrixMode::Native) {
        if (apply_scale_matrix(runtime.memory(), ctx)) ++stats.native;
        else { ++stats.fallbacks; (void)reference(runtime, ctx); }
    } else {
        auto &memory = runtime.memory();
        const auto address = ctx.gpr[4];
        std::array<std::uint8_t, 64> before{}, predicted{}, original{};
        memory.copy_out(address, before);
        auto prediction = ctx;
        if (!apply_scale_matrix(memory, prediction)) {
            ++stats.fallbacks;
            (void)reference(runtime, ctx);
        } else {
            memory.copy_out(address, predicted);
            memory.copy_in(address, before);
            if (!reference(runtime, ctx)) return;
            memory.copy_out(address, original);
            ++stats.verified;
            if (!same_context(prediction, ctx) || predicted != original) {
                ++stats.mismatches;
                std::cerr << "[native-scale] mismatch; original result retained, reference used until exit\n";
            }
        }
    }
    if (stats.calls == 1 || stats.calls % 16384u == 0) report_scale_matrix();
}
} // namespace

bool apply_scale_matrix(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx) {
    if (ctx.vfpu_ctrl[0] != 0xe4u || ctx.vfpu_ctrl[1] != 0xe4u || ctx.vfpu_ctrl[2] != 0u) return false;
    const auto matrix = scale_matrix(ctx.fpr_bits(12), ctx.fpr_bits(13), ctx.fpr_bits(14));
    for (std::uint32_t i = 0; i < matrix.size(); ++i) memory.store32(ctx.gpr[4] + i * 4u, matrix[i]);
    // The original leaves an identity matrix in M000: the three scale values
    // are stored to memory from scalar FPU registers, not into those VFPU lanes.
    for (unsigned i = 0; i < 16; ++i) ctx.vfpu[i] = i % 5u == 0u ? 1.0f : 0.0f;
    ctx.pc = ctx.gpr[31];
    return true;
}

bool install_scale_matrix(psprecomp::Runtime &runtime, ScaleMatrixMode requested) {
    if (requested == ScaleMatrixMode::Off) return false;
    std::array<std::uint8_t, kCodeSize> code{};
    if (!runtime.memory().contains(kScaleMatrixAddress, code.size())) return false;
    runtime.memory().copy_out(kScaleMatrixAddress, code);
    if (psprecomp::sha256_bytes(code) != kCodeSha256) {
        std::cerr << "[native-scale] code fingerprint differs; replacement not installed\n";
        return false;
    }
    mode = requested; stats = {};
    runtime.register_function(kScaleMatrixAddress, &bridge, "mhp3rd_native_scale_matrix");
    std::cout << "[native-scale] installed " << (mode == ScaleMatrixMode::Verify ? "verify" : "native")
              << " at 0x08878b28\n";
    return true;
}
void configure_scale_matrix(psprecomp::Runtime &runtime) {
    const char *value = std::getenv("MHP3RD_NATIVE_SCALE_MATRIX");
    if (value == nullptr || std::string_view(value) == "off" || std::string_view(value) == "0") return;
    if (std::string_view(value) == "verify") (void)install_scale_matrix(runtime, ScaleMatrixMode::Verify);
    else if (std::string_view(value) == "native") (void)install_scale_matrix(runtime, ScaleMatrixMode::Native);
    else std::cerr << "[native-scale] expected off, verify or native; original implementation retained\n";
}
ScaleMatrixStats scale_matrix_stats() { return stats; }
void report_scale_matrix() {
    if (mode == ScaleMatrixMode::Off) return;
    std::cout << "[native-scale] calls=" << stats.calls << " verified=" << stats.verified << " native=" << stats.native
              << " fallbacks=" << stats.fallbacks << " mismatches=" << stats.mismatches << '\n';
}
} // namespace mhp3rd::native
