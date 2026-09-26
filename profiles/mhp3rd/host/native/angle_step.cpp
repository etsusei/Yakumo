#include "native/angle_step.hpp"

#include <algorithm>
#include <bit>

namespace mhp3rd::native {

AngleStep step_angle(std::uint32_t current, std::uint16_t target, std::int32_t limit) noexcept {
    const std::uint32_t forward = (static_cast<std::uint32_t>(target) - current) & 0xffffu;
    const bool backwards = forward > 0x7fffu;
    const auto distance = static_cast<std::int32_t>(backwards ? 0x10000u - forward : forward);
    const std::int32_t amount = std::min(limit, distance);
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(amount);
    return {backwards ? current - bits : current + bits, amount, forward, backwards};
}

} // namespace mhp3rd::native
