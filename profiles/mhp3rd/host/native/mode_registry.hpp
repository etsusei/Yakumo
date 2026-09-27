#pragma once

#include <array>
#include <string_view>

namespace mhp3rd::native {
inline constexpr std::string_view kNativeModeSchema = "yakumo-native-modes-v2";
// Ordering preserves the historical five fields, then appends the metrics.
// A v2 recording must explicitly declare every field, including disabled ones.
inline constexpr std::array<const char *, 9> kNativeModeSwitches{
    "MHP3RD_NATIVE_ANGLE_STEP", "MHP3RD_NATIVE_SCALE_MATRIX",
    "MHP3RD_NATIVE_TRANSLATION_MATRIX", "MHP3RD_NATIVE_VECTOR_CONSTRUCT",
    "MHP3RD_NATIVE_MATRIX_COPY", "MHP3RD_NATIVE_VECTOR_NORM",
    "MHP3RD_NATIVE_VECTOR_NORM_SQUARED", "MHP3RD_NATIVE_VECTOR_DISTANCE",
    "MHP3RD_NATIVE_VECTOR_DISTANCE_SQUARED",
};
} // namespace mhp3rd::native
