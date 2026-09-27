#pragma once

#include "native/contracts.hpp"
#include "native/vector_metrics.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <span>
#include <vector>

namespace mhp3rd::native {

struct VectorMetricLeaf {
    VectorMetric kind;
    std::uint32_t entry, size;
    const char *sha256;
};
inline constexpr std::array<VectorMetricLeaf, 4> kVectorMetricLeaves{{
    {VectorMetric::Norm, 0x08877244u, 32u, "1b1da8d38edcbeb7eb486e12977c6a667fa699639b6ef8d8d7d3f664219b12a0"},
    {VectorMetric::NormSquared, 0x08877264u, 28u, "1fa5cdbdd2f96479f4cecb659eb7f68dd95a82ef853f99d7a299dc707c241cc1"},
    {VectorMetric::Distance, 0x08877280u, 40u, "977da7d41ecfd722f43c497c0f4627bb4faa7d57126d1cefa8f92138115b02a1"},
    {VectorMetric::DistanceSquared, 0x088772a8u, 36u, "05771b861950457e9e065384220c69cc255874d63fe09e62face44defea00d36"},
}};

// Physical RAM ranges whose contents must not enter the native data path.
// The caller supplies all executable sections and overlay arenas, not only the
// four leaves. Ranges are copied, and must be refreshed before changing maps.
struct MetricExcludedRange { std::uint32_t address, size; };

// Owned adapter; no process-global modes, runtime registration, or environment
// switches. Startup and certified observation integration belong to VEC-003.
// Original must be the unmodified generated unit wrapper for these leaves.
// It must return when RA is zero. The certified leaves only read RA at return.
class VectorMetricBridge {
public:
    explicit VectorMetricBridge(VectorMetric kind);
    [[nodiscard]] bool configure(psprecomp::Runtime &runtime, NativeMode mode,
                                 psprecomp::Runtime::RecompiledFunction original,
                                 std::span<const MetricExcludedRange> excluded);
    // False reports an unbound/wrong context, changed code, or failed original;
    // it never silently substitutes interpreter arithmetic for original AOT.
    // Admission rejection in a bound invocation executes the original wrapper.
    [[nodiscard]] bool execute(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context);
    [[nodiscard]] NativeStats stats() const noexcept { return stats_; }
    [[nodiscard]] NativeMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool mismatch_latched() const noexcept { return mismatch_latched_; }

private:
    VectorMetricLeaf leaf_;
    psprecomp::Runtime *runtime_{};
    psprecomp::Runtime::RecompiledFunction original_{};
    NativeMode mode_{NativeMode::Off};
    NativeStats stats_{};
    bool mismatch_latched_{};
    std::vector<MetricExcludedRange> excluded_;
    [[nodiscard]] bool fingerprint(const psprecomp::GuestMemory &memory) const;
    [[nodiscard]] bool data_range(const psprecomp::GuestMemory &memory,
                                  std::uint32_t address, std::uint32_t size) const;
    [[nodiscard]] bool original(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context);
};

} // namespace mhp3rd::native
