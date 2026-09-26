#include "native/scale_matrix.hpp"

namespace mhp3rd::native {
std::array<std::uint32_t, 16> scale_matrix(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    std::array<std::uint32_t, 16> matrix{};
    matrix[0] = x;
    matrix[5] = y;
    matrix[10] = z;
    matrix[15] = 0x3f800000u;
    return matrix;
}
} // namespace mhp3rd::native
