#pragma once

#include "native/contracts.hpp"

#include <array>
#include <cstdint>

namespace psprecomp { class Runtime; class GuestMemory; struct AllegrexContext; }

namespace mhp3rd::native {
inline constexpr std::uint32_t kVectorConstructAddress = 0x08877818u;

// Copy the three IEEE-754 bit patterns unchanged and append a literal zero.
[[nodiscard]] std::array<std::uint32_t, 4> vector_construct(std::uint32_t x, std::uint32_t y,
                                                            std::uint32_t z) noexcept;
using VectorConstructMode = NativeMode;
using VectorConstructStats = NativeStats;

// False leaves memory and context untouched. The supported guest contract is
// a writable 16-byte span; the original remains the fallback for other spans.
[[nodiscard]] bool apply_vector_construct(psprecomp::GuestMemory &memory,
                                          psprecomp::AllegrexContext &context);
[[nodiscard]] bool install_vector_construct(psprecomp::Runtime &runtime, VectorConstructMode mode);
void configure_vector_construct(psprecomp::Runtime &runtime);
[[nodiscard]] VectorConstructStats vector_construct_stats();
void report_vector_construct();
} // namespace mhp3rd::native
