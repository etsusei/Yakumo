#include "native/translation_matrix.hpp"

namespace mhp3rd::native {
std::array<std::uint32_t, 16> translation_matrix(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    std::array<std::uint32_t, 16> matrix{};
    for (unsigned i = 0; i < matrix.size(); i += 5u) matrix[i] = 0x3f800000u;
    matrix[12] = x;
    matrix[13] = y;
    matrix[14] = z;
    return matrix;
}
} // namespace mhp3rd::native
