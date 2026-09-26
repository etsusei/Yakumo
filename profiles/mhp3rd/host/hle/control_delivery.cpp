#include "hle/control_delivery.hpp"
#include "psprecomp/guest_memory.hpp"
#include "testing/game_observers.hpp"
#include <stdexcept>
namespace mhp3rd {
void deliver_control_buffer(psprecomp::GuestMemory &memory, std::uint32_t address,
    std::uint32_t count, std::uint64_t virtual_us, std::uint64_t vblank, const ControlSample &sample) {
    if (count == 0 || count > 64) throw std::invalid_argument("Invalid controller sample count");
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = address + i * 16u;
        memory.store32(entry, static_cast<std::uint32_t>(virtual_us));
        memory.store32(entry + 4u, sample.buttons);
        memory.store8(entry + 8u, sample.analog_x);
        memory.store8(entry + 9u, sample.analog_y);
        // The HD release's second stick occupies bytes 10 and 11.
        memory.store8(entry + 10u, sample.right_x);
        memory.store8(entry + 11u, sample.right_y);
        for (std::uint32_t j = 12u; j < 16u; ++j) memory.store8(entry + j, 0u);
    }
    if (auto observer = testing::active_observer())
        observer->pad({virtual_us, vblank, sample.buttons, count,
                       sample.analog_x, sample.analog_y, sample.right_x, sample.right_y});
}
} // namespace mhp3rd
