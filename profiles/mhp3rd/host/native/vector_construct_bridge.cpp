#include "native/vector_construct.hpp"
#include "native/bridge_contracts.hpp"
#include "testing/probes.hpp"

#include <cstdlib>
#include <iostream>

namespace mhp3rd::native {
namespace {
// Complete function, including the return instruction and its delay slot.
constexpr std::uint32_t kCodeSize = 24u;
constexpr const char *kCodeSha256 = "c0c7dc6c33d91c46b68700e1454930520debad811da2d7629a88a4e7d5b3349d";
VectorConstructMode mode = VectorConstructMode::Off;
VectorConstructStats stats;

bool reference(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    return run_bounded_reference(runtime, ctx, kVectorConstructAddress, kCodeSize, 0x10u, 8u,
                                 "Native vector-constructor reference left its bounded leaf");
}

void finish_vector_state(psprecomp::AllegrexContext &ctx) noexcept {
    ctx.pc = ctx.gpr[31];
}

void bridge(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    testing::NativeProbeScope probe(runtime, ctx, kVectorConstructAddress);
    ++stats.calls;
    try {
        if (stats.mismatches) {
            ++stats.fallbacks;
            const bool reference_ok = reference(runtime, ctx);
            if (!reference_ok) ++stats.errors;
            probe.finish(testing::ProbeVariant::Fallback, reference_ok);
        } else if (mode == VectorConstructMode::Native) {
            if (apply_vector_construct(runtime.memory(), ctx)) {
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
            MemoryShadow<16> shadow;
            if (!memory.contains(address, 16u) ||
                !shadow.capture_word(memory, address) ||
                !shadow.capture_word(memory, address + 4u) ||
                !shadow.capture_word(memory, address + 8u) ||
                !shadow.capture_word(memory, address + 12u)) {
                ++stats.fallbacks;
                const bool reference_ok = reference(runtime, ctx);
                if (!reference_ok) ++stats.errors;
                probe.finish(testing::ProbeVariant::Fallback, reference_ok);
            } else {
                const auto words = vector_construct(ctx.fpr_bits(12), ctx.fpr_bits(13), ctx.fpr_bits(14));
                auto prediction = ctx;
                for (std::uint32_t i = 0; i < words.size(); ++i)
                    shadow.store32(address + i * 4u, words[i]);
                finish_vector_state(prediction);
                if (!reference(runtime, ctx)) {
                    ++stats.errors;
                    probe.finish(testing::ProbeVariant::Verify, false);
                    return;
                }
                ++stats.verified;
                const bool matches = same_context(prediction, ctx) && shadow.matches(memory);
                if (!matches) {
                    ++stats.mismatches;
                    testing::native_probe_verification_mismatch(runtime, ctx, kVectorConstructAddress);
                    probe.finish(testing::ProbeVariant::Verify, false);
                    std::cerr << "[native-vector] mismatch; original result retained, reference used until exit\n";
                } else probe.finish(testing::ProbeVariant::Verify);
            }
        }
    } catch (...) { ++stats.errors; throw; }
    if (stats.calls == 1 || stats.calls % 16384u == 0) report_vector_construct();
}
} // namespace

bool apply_vector_construct(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx) {
    const auto address = ctx.gpr[4];
    if (!memory.contains(address, 16u)) return false;
    const auto words = vector_construct(ctx.fpr_bits(12), ctx.fpr_bits(13), ctx.fpr_bits(14));
    for (std::uint32_t i = 0; i < words.size(); ++i)
        memory.store32(address + i * 4u, words[i]);
    finish_vector_state(ctx);
    return true;
}

bool install_vector_construct(psprecomp::Runtime &runtime, VectorConstructMode requested) {
    if (requested == VectorConstructMode::Off) return false;
    if (!matches_code_fingerprint<kCodeSize>(runtime.memory(), kVectorConstructAddress, kCodeSha256)) {
        ++stats.errors;
        std::cerr << "[native-vector] code fingerprint differs; replacement not installed\n";
        return false;
    }
    mode = requested;
    stats = {};
    runtime.register_function(kVectorConstructAddress, &bridge, "mhp3rd_native_vector_construct");
    std::cout << "[native-vector] installed " << (mode == VectorConstructMode::Verify ? "verify" : "native")
              << " at 0x08877818\n";
    return true;
}

void configure_vector_construct(psprecomp::Runtime &runtime) {
    const auto requested = parse_native_mode(std::getenv("MHP3RD_NATIVE_VECTOR_CONSTRUCT"));
    if (!requested) {
        ++stats.errors;
        std::cerr << "[native-vector] expected off, verify or native; original implementation retained\n";
    } else if (*requested != VectorConstructMode::Off) {
        (void)install_vector_construct(runtime, *requested);
    }
}

VectorConstructStats vector_construct_stats() { return stats; }
void report_vector_construct() {
    if (mode == VectorConstructMode::Off && stats.errors == 0) return;
    std::cout << "[native-vector] calls=" << stats.calls << " verified=" << stats.verified
              << " native=" << stats.native << " fallbacks=" << stats.fallbacks
              << " mismatches=" << stats.mismatches << " errors=" << stats.errors << '\n';
}
} // namespace mhp3rd::native
