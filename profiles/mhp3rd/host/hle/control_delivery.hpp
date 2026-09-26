#pragma once
#include <cstdint>
namespace psprecomp { class GuestMemory; }
namespace mhp3rd {
struct ControlSample {
    std::uint32_t buttons{};
    std::uint8_t analog_x{128}, analog_y{128}, right_x{128}, right_y{128};
};
// Called after the original guest-write loop has completed successfully.
void observe_control_delivery(std::uint32_t count, std::uint64_t virtual_us,
    std::uint64_t vblank, const ControlSample &sample) noexcept;
// The caller has already applied camera/UI transformations. Preserve the
// original 16-byte HD sample layout; observe only a fully delivered buffer.
void deliver_control_buffer(psprecomp::GuestMemory &memory, std::uint32_t address,
    std::uint32_t count, std::uint64_t virtual_us, std::uint64_t vblank, const ControlSample &sample);
} // namespace mhp3rd
