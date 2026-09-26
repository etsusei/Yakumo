#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mhp3rd::native {

enum class NativeMode { Off, Verify, Native };

// Counts are per installed helper, not a percentage of migrated game code.
// A call is one entry to its hook. A verified call completed a comparison while
// retaining the original result; native means the replacement was committed.
// Fallbacks run the original due to an unsupported input or a prior mismatch.
// Errors count failed installation, reference, or execution. An error can also
// be a fallback, and an installation error can occur with zero calls.
struct NativeStats {
    std::uint64_t calls{}, verified{}, native{}, fallbacks{}, mismatches{}, errors{};
};

[[nodiscard]] inline std::optional<NativeMode> parse_native_mode(const char *value) noexcept {
    if (value == nullptr || std::string_view(value) == "off" || std::string_view(value) == "0")
        return NativeMode::Off;
    if (std::string_view(value) == "verify") return NativeMode::Verify;
    if (std::string_view(value) == "native") return NativeMode::Native;
    return std::nullopt;
}

} // namespace mhp3rd::native
