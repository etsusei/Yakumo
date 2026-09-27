#include "native/vector_metrics_bridge.hpp"
#include "native/bridge_contracts.hpp"
#include "testing/probes.hpp"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <stdexcept>

namespace mhp3rd::native {
namespace {
bool standard_prefixes(const psprecomp::AllegrexContext &context) {
    return context.vfpu_ctrl[0] == 0xe4u && context.vfpu_ctrl[1] == 0xe4u &&
           context.vfpu_ctrl[2] == 0u;
}

struct Prediction {
    psprecomp::AllegrexContext context;
    MemoryShadow<36> memory;
    std::uint32_t scratch{}, result{};
};

Prediction predict(const psprecomp::GuestMemory &memory,
                   const psprecomp::AllegrexContext &context, VectorMetric kind) {
    Prediction result{context, {}, context.gpr[29] - 16u, 0u};
    std::array<std::uint32_t, 4> first{}, second{};
    // Capture all reads before the sole write, including the non-arithmetic
    // fourth lanes. Byte shadows preserve unaligned and aliased overlaps.
    for (std::uint32_t i = 0; i < 4u; ++i) {
        if (!result.memory.capture_word(memory, context.gpr[4] + 4u * i))
            throw std::logic_error("Vector metric source escaped admission");
        first[i] = result.memory.load32(context.gpr[4] + 4u * i);
        if (metric_uses_distance(kind)) {
            if (!result.memory.capture_word(memory, context.gpr[5] + 4u * i))
                throw std::logic_error("Vector metric second source escaped admission");
            second[i] = result.memory.load32(context.gpr[5] + 4u * i);
        }
    }
    if (!result.memory.capture_word(memory, result.scratch))
        throw std::logic_error("Vector metric scratch escaped admission");
    const auto metric = vector_metric(kind, {first[0], first[1], first[2]},
                                     {second[0], second[1], second[2]});
    for (unsigned i = 0; i < 4u; ++i) {
        result.context.vfpu[i] = std::bit_cast<float>(first[i]);
        if (metric_uses_distance(kind)) result.context.vfpu[4u + i] = std::bit_cast<float>(second[i]);
    }
    for (unsigned i = 0; i < 3u; ++i)
        result.context.vfpu[i] = std::bit_cast<float>(metric.components[i]);
    result.context.vfpu[4] = std::bit_cast<float>(metric.scalar);
    result.context.fpr[0] = std::bit_cast<float>(metric.scalar);
    result.context.vfpu_ctrl[0] = result.context.vfpu_ctrl[1] = 0xe4u;
    result.context.vfpu_ctrl[2] = 0u;
    result.context.pc = context.gpr[31];
    result.result = metric.scalar;
    result.memory.store32(result.scratch, result.result);
    return result;
}
} // namespace

VectorMetricBridge::VectorMetricBridge(VectorMetric kind) : leaf_{} {
    const auto found = std::find_if(kVectorMetricLeaves.begin(), kVectorMetricLeaves.end(),
                                   [kind](const auto &leaf) { return leaf.kind == kind; });
    if (found == kVectorMetricLeaves.end()) throw std::invalid_argument("Unknown vector metric");
    leaf_ = *found;
}

bool VectorMetricBridge::fingerprint(const psprecomp::GuestMemory &memory) const {
    std::array<std::uint8_t, 40> bytes{};
    if (!memory.contains(leaf_.entry, leaf_.size)) return false;
    const auto span = std::span(bytes).first(leaf_.size);
    memory.copy_out(leaf_.entry, span);
    return psprecomp::sha256_bytes(span) == leaf_.sha256;
}

bool VectorMetricBridge::configure(psprecomp::Runtime &runtime, NativeMode mode,
                                   psprecomp::Runtime::RecompiledFunction original_function,
                                   std::span<const MetricExcludedRange> excluded) {
    bool covers_leaf = false;
    bool valid_ranges = !excluded.empty();
    for (const auto &range : excluded) {
        const auto end = std::uint64_t(range.address) + range.size;
        valid_ranges &= range.size != 0u && range.address == psprecomp::GuestMemory::canonical(range.address) &&
                        end <= 0x20000000ull;
        covers_leaf |= range.address <= leaf_.entry && end >= std::uint64_t(leaf_.entry) + leaf_.size;
    }
    if (!original_function || !valid_ranges || !covers_leaf || !fingerprint(runtime.memory()) ||
        (mode != NativeMode::Off && mode != NativeMode::Verify && mode != NativeMode::Native)) {
        ++stats_.errors;
        return false;
    }
    excluded_.assign(excluded.begin(), excluded.end());
    runtime_ = &runtime;
    original_ = original_function;
    mode_ = mode;
    stats_ = {};
    return true;
}

bool VectorMetricBridge::data_range(const psprecomp::GuestMemory &memory,
                                    std::uint32_t address, std::uint32_t size) const {
    const auto physical = psprecomp::GuestMemory::canonical(address);
    const auto end = std::uint64_t(physical) + size;
    if (physical < psprecomp::GuestMemory::kPhysicalBase ||
        end > std::uint64_t(psprecomp::GuestMemory::kPhysicalBase) + memory.size() ||
        !memory.contains(address, size)) return false;
    for (const auto &range : excluded_)
        if (physical < std::uint64_t(range.address) + range.size && range.address < end) return false;
    return true;
}

bool VectorMetricBridge::original(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context) {
    // The unit can follow a return within itself. Substitute an unmapped
    // sentinel only for this certified RA-independent leaf to bound one call.
    // Calling the original wrapper directly also avoids adapter recursion.
    const auto return_address = context.gpr[31];
    testing::NativeProbeAotSuppression observation(runtime, context, leaf_.entry);
    context.gpr[31] = 0u;
    try { original_(runtime, context); }
    catch (...) { context.gpr[31] = return_address; throw; }
    context.gpr[31] = return_address;
    if (runtime.stopped() || context.pc != 0u) {
        runtime.stop("Vector metric original failed its bounded return");
        return false;
    }
    context.pc = return_address;
    return true;
}

bool VectorMetricBridge::execute(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context) {
    if (runtime_ != &runtime || !original_ || context.pc != leaf_.entry) {
        ++stats_.errors;
        return false;
    }
    testing::NativeProbeScope probe(runtime, context, leaf_.entry);
    if (runtime.stopped() || !fingerprint(runtime.memory())) {
        ++stats_.errors;
        probe.finish(testing::ProbeVariant::Fallback, false);
        return false;
    }
    ++stats_.calls;
    auto variant = mode_ == NativeMode::Off ? testing::ProbeVariant::Aot :
                   mode_ == NativeMode::Verify ? testing::ProbeVariant::Verify : testing::ProbeVariant::Native;
    try {
        if (mode_ == NativeMode::Off) {
            const bool ok = original(runtime, context);
            if (!ok) ++stats_.errors;
            probe.finish(variant, ok);
            return ok;
        }
        const auto &memory = runtime.memory();
        const bool admitted = standard_prefixes(context) && std::fegetround() == FE_TONEAREST &&
            data_range(memory, context.gpr[4], 16u) &&
            (!metric_uses_distance(leaf_.kind) || data_range(memory, context.gpr[5], 16u)) &&
            data_range(memory, context.gpr[29] - 16u, 4u);
        if (!admitted || mismatch_latched_) {
            variant = testing::ProbeVariant::Fallback;
            ++stats_.fallbacks;
            const bool ok = original(runtime, context);
            if (!ok) ++stats_.errors;
            probe.finish(variant, ok);
            return ok;
        }
        const auto prediction = predict(memory, context, leaf_.kind);
        if (mode_ == NativeMode::Native) {
            runtime.memory().store32(prediction.scratch, prediction.result);
            context = prediction.context;
            ++stats_.native;
            probe.finish(variant);
            return true;
        }
        if (!original(runtime, context)) {
            ++stats_.errors;
            probe.finish(variant, false);
            return false;
        }
        ++stats_.verified;
        if (!same_context(prediction.context, context) || !prediction.memory.matches(memory)) {
            ++stats_.mismatches;
            mismatch_latched_ = true;
            testing::native_probe_verification_mismatch(runtime, context, leaf_.entry);
            probe.finish(variant, false);
        } else {
            probe.finish(variant);
        }
        return true;
    } catch (...) { ++stats_.errors; probe.finish(variant, false); throw; }
}

} // namespace mhp3rd::native
