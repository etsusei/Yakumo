#include "testing/overlay_observation.hpp"
#include "psprecomp/guest_memory.hpp"
#include <array>
#include <string_view>

namespace mhp3rd::testing {
namespace {
constexpr std::size_t kHeaderBytes = 64;
constexpr std::uint32_t kMaxObservedCode = 8u * 1024u * 1024u;
std::uint64_t fnv(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (auto byte : bytes) { hash ^= byte; hash *= 0x100000001b3ull; }
    return hash;
}
std::string fingerprint(std::string_view prefix, std::uint64_t value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text(prefix);
    for (int shift = 60; shift >= 0; shift -= 4) text += hex[(value >> shift) & 15u];
    return text;
}
}
std::optional<ObservedOverlayIdentity> read_overlay_identity(
    const psprecomp::GuestMemory &memory, std::uint32_t base) noexcept {
    try {
        if (!memory.contains(base, kHeaderBytes)) return std::nullopt;
        std::array<std::uint8_t, kHeaderBytes> head{};
        memory.copy_out(base, head);
        const auto word = [&head](std::size_t at) {
            return std::uint32_t{head[at]} | (std::uint32_t{head[at + 1]} << 8u) |
                (std::uint32_t{head[at + 2]} << 16u) | (std::uint32_t{head[at + 3]} << 24u);
        };
        if (head[0] != 'M' || head[1] != 'W' || head[2] != 'o' || head[3] != '3' || word(8) != base)
            return std::nullopt;
        const auto code_size = word(12);
        const std::uint64_t image_size = std::uint64_t{kHeaderBytes} + code_size + word(16);
        if (code_size == 0 || code_size > kMaxObservedCode || image_size > 0xffffffffull ||
            !memory.contains(base, static_cast<std::size_t>(image_size))) return std::nullopt;
        const auto *image = memory.raw_pointer(base, kHeaderBytes + code_size);
        if (!image) return std::nullopt;
        const auto hash = fnv({image, kHeaderBytes + code_size});
        std::string name;
        for (std::size_t i = 32; i < 64 && head[i] != 0; ++i)
            name += head[i] >= 32 && head[i] < 127 ? static_cast<char>(head[i]) : '?';
        if (name.empty()) name = "<unnamed>";
        return ObservedOverlayIdentity{{base, static_cast<std::uint32_t>(image_size), code_size,
            std::move(name), fingerprint("fnv1a64-header:", fnv(head)),
            fingerprint("fnv1a64-header-code:", hash), false}, hash};
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace mhp3rd::testing
