#pragma once

#include "testing/game_observers.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace psprecomp { class GuestMemory; }
namespace mhp3rd::perf { struct Summary; }

namespace mhp3rd::testing {

// These readings describe only the supported executable. The character's name
// is used to establish that a save is loaded, but is never returned or logged.
// Quest readings additionally require the current, corpus-matched task overlay.
struct MonsterState {
    enum class Status { Unavailable, Empty, InvalidPointer, UnclassifiedOrUnspawned, InvalidValues, Observed };
    Status status{Status::Unavailable};
    std::optional<std::uint8_t> kind;
    std::optional<std::int16_t> health, maximum;
};

struct GameStateSnapshot {
    enum class QuestStatus {
        UnsupportedExecutable, CharacterUnavailable, CharacterUnloaded,
        OverlayUnavailable, OverlayIdentityChanged, Verified
    };
    bool supported_executable{};
    std::optional<bool> character_loaded;
    std::optional<std::uint32_t> money;
    QuestStatus quest_status{QuestStatus::UnsupportedExecutable};
    std::optional<std::uint64_t> overlay_generation, code_epoch;
    std::optional<std::int16_t> health, recoverable_health, maximum_health;
    std::optional<double> stamina;
    std::optional<std::uint16_t> maximum_stamina;
    std::optional<std::uint32_t> quest_time_left_frames, quest_time_limit_frames;
    std::array<MonsterState, 5> monsters{};
};

// Bounded guest-memory reads. This translation unit includes the game-state address
// constants, but never links the debug command or cheat-write implementation.
[[nodiscard]] GameStateSnapshot read_game_state(const psprecomp::GuestMemory &memory,
    const GameObserver &observer, bool supported_executable) noexcept;
void observe_game_state(const psprecomp::GuestMemory &memory, GameObserver &observer,
    bool supported_executable) noexcept;

// One journal record per new valid perf::Summary::second. Invalid individual
// measurements are represented as unavailable fields, preserving the rest.
class PerformanceObservation {
public:
    void observe(GameObserver &observer, const perf::Summary &summary) noexcept;
private:
    std::uint64_t last_second_{};
};

} // namespace mhp3rd::testing
