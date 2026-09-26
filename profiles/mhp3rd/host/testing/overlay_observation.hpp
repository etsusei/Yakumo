#pragma once
#include "testing/game_observers.hpp"
#include <optional>
namespace psprecomp { class GuestMemory; }
namespace mhp3rd::testing {
struct ObservedOverlayIdentity {
    OverlayIdentity identity;
    std::uint64_t corpus_hash{};
};
// Reads only header and immutable code. No corpus trust is inferred here;
// the caller must compare the hash and sizes against its loaded corpus table.
[[nodiscard]] std::optional<ObservedOverlayIdentity> read_overlay_identity(
    const psprecomp::GuestMemory &memory, std::uint32_t base) noexcept;
} // namespace mhp3rd::testing
