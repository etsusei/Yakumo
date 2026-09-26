#include "native/matrix_copy.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <limits>

namespace mhp3rd::native {

bool matrix_copy_words_mapped(const psprecomp::GuestMemory &memory,
                              std::uint32_t destination, std::uint32_t source) noexcept {
    // The largest word ends at base + 43. Unsigned wrap could turn an invalid
    // span into unrelated, mapped memory, so reject it explicitly.
    constexpr auto kLastByte = 43u;
    if (destination > std::numeric_limits<std::uint32_t>::max() - kLastByte ||
        source > std::numeric_limits<std::uint32_t>::max() - kLastByte) return false;
    for (std::uint32_t offset : {0u, 16u, 32u}) {
        for (std::uint32_t column : {0u, 4u, 8u}) {
            if (!memory.contains(source + offset + column, 4u) ||
                !memory.contains(destination + offset + column, 4u)) return false;
        }
    }
    return true;
}

bool apply_matrix_copy(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &context) {
    const auto destination = context.gpr[4];
    const auto source = context.gpr[5];
    if (!matrix_copy_words_mapped(memory, destination, source)) return false;
    const auto last = copy_matrix_words(destination, source,
        [&memory](std::uint32_t address) { return memory.load32(address); },
        [&memory](std::uint32_t address, std::uint32_t bits) { memory.store32(address, bits); });
    context.set_vfpu_scalar_bits(0u, last[0]);
    context.set_vfpu_scalar_bits(32u, last[1]);
    context.set_vfpu_scalar_bits(64u, last[2]);
    context.pc = context.gpr[31];
    return true;
}

} // namespace mhp3rd::native
