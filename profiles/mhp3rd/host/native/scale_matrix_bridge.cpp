#include "native/scale_matrix.hpp"
#include "native/bridge_contracts.hpp"
#include "testing/probes.hpp"

#include <bit>
#include <cstdlib>
#include <iostream>

namespace mhp3rd::native {
namespace {
// Complete function, including its return's memory-writing delay slot.
constexpr std::uint32_t kCodeSize = 36u;
constexpr const char *kCodeSha256 = "d9ad67fd4b7e26ea8b213c8297489c288c39161f9190bb267f9f5ade35a799b6";
ScaleMatrixMode mode = ScaleMatrixMode::Off;
ScaleMatrixStats stats;

bool standard_prefixes(const psprecomp::AllegrexContext &ctx) noexcept {
    return ctx.vfpu_ctrl[0] == 0xe4u && ctx.vfpu_ctrl[1] == 0xe4u && ctx.vfpu_ctrl[2] == 0u;
}

void finish_scale_state(psprecomp::AllegrexContext &ctx) noexcept {
    // The original keeps identity in M000. Under standard prefixes the
    // matrix-init instruction consumes the prefixes without changing them.
    for (unsigned i = 0; i < 16; ++i) ctx.vfpu[i] = i % 5u == 0u ? 1.0f : 0.0f;
    ctx.pc = ctx.gpr[31];
}

bool reference(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    return run_bounded_reference(runtime, ctx, kScaleMatrixAddress, kCodeSize, 28u, 12u,
                                 "Native scale-matrix reference left its bounded leaf");
}

void bridge(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    testing::NativeProbeScope probe(runtime, ctx, kScaleMatrixAddress);
    ++stats.calls;
    try {
        if (stats.mismatches) {
            ++stats.fallbacks;
            const bool reference_ok = reference(runtime, ctx);
            if (!reference_ok) ++stats.errors;
            probe.finish(testing::ProbeVariant::Fallback, reference_ok);
        } else if (mode == ScaleMatrixMode::Native) {
            if (apply_scale_matrix(runtime.memory(), ctx)) {
                ++stats.native;
                probe.finish(testing::ProbeVariant::Native);
            } else {
                ++stats.fallbacks;
                const bool reference_ok = reference(runtime, ctx);
                if (!reference_ok) ++stats.errors;
                probe.finish(testing::ProbeVariant::Fallback, reference_ok);
            }
        } else {
            auto &memory = runtime.memory();
            const auto address = ctx.gpr[4];
            if (!standard_prefixes(ctx) || !memory.contains(address, 64u)) {
                ++stats.fallbacks;
                const bool reference_ok = reference(runtime, ctx);
                if (!reference_ok) ++stats.errors;
                probe.finish(testing::ProbeVariant::Fallback, reference_ok);
            } else {
                const auto matrix = scale_matrix(ctx.fpr_bits(12), ctx.fpr_bits(13), ctx.fpr_bits(14));
                auto prediction = ctx;
                finish_scale_state(prediction);
                if (!reference(runtime, ctx)) {
                    ++stats.errors;
                    probe.finish(testing::ProbeVariant::Verify, false);
                    return;
                }
                ++stats.verified;
                bool same_memory = true;
                for (std::uint32_t i = 0; i < matrix.size(); ++i) {
                    if (memory.load32(address + i * 4u) != matrix[i]) { same_memory = false; break; }
                }
                const bool matches = same_context(prediction, ctx) && same_memory;
                if (!matches) {
                    ++stats.mismatches;
                    testing::native_probe_verification_mismatch(runtime, ctx, kScaleMatrixAddress);
                    probe.finish(testing::ProbeVariant::Verify, false);
                    std::cerr << "[native-scale] mismatch; original result retained, reference used until exit\n";
                } else probe.finish(testing::ProbeVariant::Verify);
            }
        }
    } catch (...) { ++stats.errors; throw; }
    if (stats.calls == 1 || stats.calls % 16384u == 0) report_scale_matrix();
}
} // namespace

bool apply_scale_matrix(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx) {
    if (!standard_prefixes(ctx) || !memory.contains(ctx.gpr[4], 64u)) return false;
    const auto matrix = scale_matrix(ctx.fpr_bits(12), ctx.fpr_bits(13), ctx.fpr_bits(14));
    for (std::uint32_t i = 0; i < matrix.size(); ++i) memory.store32(ctx.gpr[4] + i * 4u, matrix[i]);
    finish_scale_state(ctx);
    return true;
}

bool install_scale_matrix(psprecomp::Runtime &runtime, ScaleMatrixMode requested) {
    if (requested == ScaleMatrixMode::Off) return false;
    if (!matches_code_fingerprint<kCodeSize>(runtime.memory(), kScaleMatrixAddress, kCodeSha256)) {
        ++stats.errors;
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
    const auto requested = parse_native_mode(value);
    if (!requested) {
        ++stats.errors;
        std::cerr << "[native-scale] expected off, verify or native; original implementation retained\n";
    } else if (*requested != ScaleMatrixMode::Off) (void)install_scale_matrix(runtime, *requested);
}
ScaleMatrixStats scale_matrix_stats() { return stats; }
void report_scale_matrix() {
    if (mode == ScaleMatrixMode::Off && stats.errors == 0) return;
    std::cout << "[native-scale] calls=" << stats.calls << " verified=" << stats.verified << " native=" << stats.native
              << " fallbacks=" << stats.fallbacks << " mismatches=" << stats.mismatches
              << " errors=" << stats.errors << '\n';
}
} // namespace mhp3rd::native
