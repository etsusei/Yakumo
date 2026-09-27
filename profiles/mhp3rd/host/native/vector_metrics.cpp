#include "native/vector_metrics.hpp"

#include <bit>
#include <cmath>
#include <limits>

namespace mhp3rd::native {
namespace {

static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == sizeof(std::uint32_t));

float value(std::uint32_t bits) noexcept { return std::bit_cast<float>(bits); }

std::uint32_t bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }

} // namespace

VectorMetricResult vector_metric(VectorMetric kind, const std::array<std::uint32_t, 3> &a,
                                 const std::array<std::uint32_t, 3> &b) noexcept {
    VectorMetricResult result{a, 0u};
    std::array<float, 3> lanes{};

    for (std::size_t i = 0; i < lanes.size(); ++i) {
        const float first = value(a[i]);
        lanes[i] = metric_uses_distance(kind) ? first - value(b[i]) : first;
        if (metric_uses_distance(kind)) result.components[i] = bits(lanes[i]);
    }

    // Keep the observed production AOT order: round y*y to float, then fuse
    // x*x, z*z and the zero-filled fourth lane into the running sum.
    const float y_square = lanes[1] * lanes[1];
    float sum = std::fma(lanes[0], lanes[0], y_square);
    sum = std::fma(lanes[2], lanes[2], sum);
    sum = std::fma(0.0f, 0.0f, sum);
    result.scalar = bits(metric_uses_sqrt(kind) ? std::fabs(std::sqrt(sum)) : sum);
    return result;
}

} // namespace mhp3rd::native
