#pragma once

#include <cstdint>

namespace mhp3rd::native {

struct AngleStep {
    std::uint32_t value;
    std::int32_t amount;
    std::uint32_t forward_distance;
    bool backwards;
};

// Reproduce the game's bounded turn on a 65536-unit circle. Preserve the
// unwrapped 32-bit accumulator and signed step limit, including unusual
// negative limits. An exact half-turn takes the backwards direction.
[[nodiscard]] AngleStep step_angle(std::uint32_t current, std::uint16_t target,
                                   std::int32_t limit) noexcept;

} // namespace mhp3rd::native
