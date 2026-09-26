#include "native/vector_construct.hpp"

namespace mhp3rd::native {
std::array<std::uint32_t, 4> vector_construct(std::uint32_t x, std::uint32_t y,
                                              std::uint32_t z) noexcept {
    return {x, y, z, 0u};
}
} // namespace mhp3rd::native
