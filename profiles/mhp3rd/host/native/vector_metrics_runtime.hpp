#pragma once

#include "native/vector_metrics_bridge.hpp"

namespace psprecomp { class Elf32Image; }

namespace mhp3rd::native {

inline constexpr std::array<const char *, 4> kVectorMetricSwitches{
    "MHP3RD_NATIVE_VECTOR_NORM", "MHP3RD_NATIVE_VECTOR_NORM_SQUARED",
    "MHP3RD_NATIVE_VECTOR_DISTANCE", "MHP3RD_NATIVE_VECTOR_DISTANCE_SQUARED",
};
using VectorMetricModes = std::array<NativeMode, 4>;
[[nodiscard]] VectorMetricModes vector_metric_environment_modes();

// Startup-only registration owned by the runtime's caller. Construct after
// Runtime, destroy before it, and do not install/destroy while guest execution
// is running. Original is the generated unit wrapper resolved by CMake.
class VectorMetricRuntime final {
public:
    VectorMetricRuntime(psprecomp::Runtime &runtime, const psprecomp::Elf32Image &elf,
                        psprecomp::Runtime::RecompiledFunction original);
    ~VectorMetricRuntime();
    VectorMetricRuntime(const VectorMetricRuntime &) = delete;
    VectorMetricRuntime &operator=(const VectorMetricRuntime &) = delete;
    [[nodiscard]] bool install(const VectorMetricModes &modes);
    [[nodiscard]] std::array<NativeStats, 4> stats() const noexcept;
    void report() const;

private:
    psprecomp::Runtime &runtime_;
    psprecomp::Runtime::RecompiledFunction original_;
    std::vector<MetricExcludedRange> excluded_;
    std::array<VectorMetricBridge, 4> bridges_{{
        VectorMetricBridge(VectorMetric::Norm), VectorMetricBridge(VectorMetric::NormSquared),
        VectorMetricBridge(VectorMetric::Distance), VectorMetricBridge(VectorMetric::DistanceSquared),
    }};
    VectorMetricModes modes_{};
    bool installed_{};
    std::array<bool, 4> hooked_{};
    static bool dispatch_entry(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context,
                               std::uint32_t entry);
    static void invoke(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context, std::size_t index);
    template<std::size_t Index>
    static void hook(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context) {
        invoke(runtime, context, Index);
    }
};

} // namespace mhp3rd::native
