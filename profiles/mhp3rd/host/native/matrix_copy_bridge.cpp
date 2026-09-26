#include "native/matrix_copy.hpp"
#include "native/bridge_contracts.hpp"
#include "testing/probes.hpp"

#include <cstdlib>
#include <iostream>

namespace mhp3rd::native {
namespace {

// Complete leaf, including the return and its nop delay slot. The ninth store
// is the instruction immediately before the return.
constexpr std::uint32_t kCodeSize = 80u;
constexpr const char *kCodeSha256 = "e918aeb6363b81be6cab6ddb2c2605f2179d24bcfa418ea3f24dc06ef393f6c9";
MatrixCopyMode mode = MatrixCopyMode::Off;
MatrixCopyStats stats;

bool reference(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context) {
    return run_bounded_reference(runtime, context, kMatrixCopyAddress, kCodeSize, 0x48u, 24u,
                                 "Native matrix-copy reference left its bounded leaf");
}

void bridge(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context) {
    testing::NativeProbeScope probe(runtime, context, kMatrixCopyAddress);
    ++stats.calls;
    try {
        auto &memory = runtime.memory();
        if (stats.mismatches != 0u) {
            ++stats.fallbacks;
            const bool reference_ok = reference(runtime, context);
            if (!reference_ok) ++stats.errors;
            probe.finish(testing::ProbeVariant::Fallback, reference_ok);
        } else if (mode == MatrixCopyMode::Native) {
            if (apply_matrix_copy(memory, context)) {
                ++stats.native;
                probe.finish(testing::ProbeVariant::Native);
            } else {
                ++stats.fallbacks;
                const bool reference_ok = reference(runtime, context);
                if (!reference_ok) ++stats.errors;
                probe.finish(testing::ProbeVariant::Fallback, reference_ok);
            }
        } else if (!matrix_copy_words_mapped(memory, context.gpr[4], context.gpr[5])) {
            ++stats.fallbacks;
            const bool reference_ok = reference(runtime, context);
            if (!reference_ok) ++stats.errors;
            probe.finish(testing::ProbeVariant::Fallback, reference_ok);
        } else {
            // Capture all 18 addresses before simulating stores so the shadow
            // models partial overlap and physical cached/uncached aliases.
            MemoryShadow<72> shadow;
            for (std::uint32_t offset : {0u, 16u, 32u}) {
                for (std::uint32_t column : {0u, 4u, 8u}) {
                    if (!shadow.capture_word(memory, context.gpr[5] + offset + column) ||
                        !shadow.capture_word(memory, context.gpr[4] + offset + column)) {
                        ++stats.fallbacks;
                        const bool reference_ok = reference(runtime, context);
                        if (!reference_ok) ++stats.errors;
                        probe.finish(testing::ProbeVariant::Fallback, reference_ok);
                        return;
                    }
                }
            }
            auto prediction = context;
            const auto last = copy_matrix_words(context.gpr[4], context.gpr[5],
                [&shadow](std::uint32_t address) { return shadow.load32(address); },
                [&shadow](std::uint32_t address, std::uint32_t bits) { shadow.store32(address, bits); });
            prediction.set_vfpu_scalar_bits(0u, last[0]);
            prediction.set_vfpu_scalar_bits(32u, last[1]);
            prediction.set_vfpu_scalar_bits(64u, last[2]);
            prediction.pc = prediction.gpr[31];
            if (!reference(runtime, context)) {
                ++stats.errors;
                probe.finish(testing::ProbeVariant::Verify, false);
                return;
            }
            ++stats.verified;
            const bool matches = same_context(prediction, context) && shadow.matches(memory);
            if (!matches) {
                ++stats.mismatches;
                probe.finish(testing::ProbeVariant::Verify, false);
                std::cerr << "[native-matrix-copy] mismatch; original result retained, reference used until exit\n";
            } else probe.finish(testing::ProbeVariant::Verify);
        }
    } catch (...) { ++stats.errors; throw; }
    if (stats.calls == 1u || stats.calls % 16384u == 0u) report_matrix_copy();
}

} // namespace

bool install_matrix_copy(psprecomp::Runtime &runtime, MatrixCopyMode requested) {
    if (requested == MatrixCopyMode::Off) return false;
    if (!matches_code_fingerprint<kCodeSize>(runtime.memory(), kMatrixCopyAddress, kCodeSha256)) {
        ++stats.errors;
        std::cerr << "[native-matrix-copy] code fingerprint differs; replacement not installed\n";
        return false;
    }
    mode = requested;
    stats = {};
    runtime.register_function(kMatrixCopyAddress, &bridge, "mhp3rd_native_matrix_copy");
    std::cout << "[native-matrix-copy] installed " << (mode == MatrixCopyMode::Verify ? "verify" : "native")
              << " at 0x08879d08\n";
    return true;
}

void configure_matrix_copy(psprecomp::Runtime &runtime) {
    const auto requested = parse_native_mode(std::getenv("MHP3RD_NATIVE_MATRIX_COPY"));
    if (!requested) {
        ++stats.errors;
        std::cerr << "[native-matrix-copy] expected off, verify or native; original implementation retained\n";
    } else if (*requested != MatrixCopyMode::Off) (void)install_matrix_copy(runtime, *requested);
}

MatrixCopyStats matrix_copy_stats() { return stats; }

void report_matrix_copy() {
    if (mode == MatrixCopyMode::Off && stats.errors == 0u) return;
    std::cout << "[native-matrix-copy] calls=" << stats.calls << " verified=" << stats.verified
              << " native=" << stats.native << " fallbacks=" << stats.fallbacks
              << " mismatches=" << stats.mismatches << " errors=" << stats.errors << '\n';
}

} // namespace mhp3rd::native
