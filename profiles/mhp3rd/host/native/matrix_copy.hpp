#pragma once

#include "native/contracts.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>

namespace psprecomp { class Runtime; class GuestMemory; struct AllegrexContext; }

namespace mhp3rd::native {

inline constexpr std::uint32_t kMatrixCopyAddress = 0x08879d08u;
using MatrixCopyMode = NativeMode;
using MatrixCopyStats = NativeStats;

// The three loads for a row all precede its stores. Earlier row stores can
// affect later row loads when the regions overlap, including through aliases.
// Accessors exchange raw little-endian words; no floating-point operation is
// performed. The returned words are the final row's VFPU scratch values.
template <typename LoadWord, typename StoreWord>
[[nodiscard]] std::array<std::uint32_t, 3> copy_matrix_words(
    std::uint32_t destination, std::uint32_t source, LoadWord &&load, StoreWord &&store) {
    std::array<std::uint32_t, 3> row{};
    for (std::uint32_t offset : {0u, 16u, 32u}) {
        row[0] = load(source + offset);
        row[1] = load(source + offset + 4u);
        row[2] = load(source + offset + 8u);
        store(destination + offset, row[0]);
        store(destination + offset + 4u, row[1]);
        store(destination + offset + 8u, row[2]);
    }
    return row;
}

// Accept only complete words mapped in the runtime's RAM/EDRAM windows.
// Reject before any write if an address wraps or any word is unavailable.
[[nodiscard]] bool matrix_copy_words_mapped(const psprecomp::GuestMemory &memory,
                                            std::uint32_t destination, std::uint32_t source) noexcept;
// False leaves memory and every CPU field untouched.
[[nodiscard]] bool apply_matrix_copy(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &context);
[[nodiscard]] bool install_matrix_copy(psprecomp::Runtime &runtime, MatrixCopyMode mode);
void configure_matrix_copy(psprecomp::Runtime &runtime);
[[nodiscard]] MatrixCopyStats matrix_copy_stats();
void report_matrix_copy();

} // namespace mhp3rd::native
