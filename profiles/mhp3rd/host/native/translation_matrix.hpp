#pragma once

#include "native/contracts.hpp"

#include <array>
#include <cstdint>

namespace psprecomp { class Runtime; class GuestMemory; struct AllegrexContext; }
namespace mhp3rd::native {
inline constexpr std::uint32_t kTranslationMatrixAddress = 0x08878b4cu;
// IEEE-754 bits are copied exactly, including signed zero and NaN payloads.
[[nodiscard]] std::array<std::uint32_t, 16> translation_matrix(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept;
using TranslationMatrixMode = NativeMode;
using TranslationMatrixStats = NativeStats;
// False leaves memory and context untouched: unsupported memory or an unusual VFPU
// prefix needs the original function.
[[nodiscard]] bool apply_translation_matrix(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &context);
[[nodiscard]] bool install_translation_matrix(psprecomp::Runtime &runtime, TranslationMatrixMode mode);
void configure_translation_matrix(psprecomp::Runtime &runtime);
[[nodiscard]] TranslationMatrixStats translation_matrix_stats();
void report_translation_matrix();
} // namespace mhp3rd::native
