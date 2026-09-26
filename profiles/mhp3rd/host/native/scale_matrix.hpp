#pragma once

#include "native/contracts.hpp"

#include <array>
#include <cstdint>

namespace psprecomp { class Runtime; class GuestMemory; struct AllegrexContext; }
namespace mhp3rd::native {
inline constexpr std::uint32_t kScaleMatrixAddress = 0x08878b28u;
// IEEE-754 bits are copied exactly, including signed zero and NaN payloads.
[[nodiscard]] std::array<std::uint32_t, 16> scale_matrix(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept;
using ScaleMatrixMode = NativeMode;
using ScaleMatrixStats = NativeStats;
// False leaves memory and context untouched: an unusual VFPU prefix needs
// the original function. Ordinary scale-matrix construction uses no prefixes.
[[nodiscard]] bool apply_scale_matrix(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &context);
[[nodiscard]] bool install_scale_matrix(psprecomp::Runtime &runtime, ScaleMatrixMode mode);
void configure_scale_matrix(psprecomp::Runtime &runtime);
[[nodiscard]] ScaleMatrixStats scale_matrix_stats();
void report_scale_matrix();
} // namespace mhp3rd::native
