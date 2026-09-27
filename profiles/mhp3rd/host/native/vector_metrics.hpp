#pragma once

#include <array>
#include <cstdint>

namespace mhp3rd::native {

enum class VectorMetric { Norm, NormSquared, Distance, DistanceSquared };

struct VectorMetricResult {
    std::array<std::uint32_t, 3> components;
    std::uint32_t scalar;
};

[[nodiscard]] constexpr bool metric_uses_distance(VectorMetric kind) noexcept {
    return kind == VectorMetric::Distance || kind == VectorMetric::DistanceSquared;
}

[[nodiscard]] constexpr bool metric_uses_sqrt(VectorMetric kind) noexcept {
    return kind == VectorMetric::Norm || kind == VectorMetric::Distance;
}

// Inputs and outputs are raw IEEE-754 single-precision words. Norm components
// retain the input words; distance components contain the rounded differences.
[[nodiscard]] VectorMetricResult vector_metric(
    VectorMetric kind, const std::array<std::uint32_t, 3> &a,
    const std::array<std::uint32_t, 3> &b = {}) noexcept;

} // namespace mhp3rd::native
