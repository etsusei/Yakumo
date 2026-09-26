#include "testing/state_observation.hpp"

#include "debug/game_state.hpp"
#include "perf/frame_stats.hpp"
#include "psprecomp/guest_memory.hpp"
#include "testing/overlay_observation.hpp"

#include <bit>
#include <cstddef>
#include <cmath>
#include <string>
#include <type_traits>
#include <utility>

namespace mhp3rd::testing {
namespace {
namespace game = debug::p3rd;

// Conservative observation bounds for this supported image. They are not
// asserted game caps; an out-of-range value is reported as unavailable.
constexpr std::int16_t kMaximumPlausibleHealth = 1000;
constexpr std::uint16_t kMaximumPlausibleStamina = 6000;
constexpr std::uint32_t kMaximumPlausibleQuestFrames = 3u * 60u * 60u * 30u;

std::optional<bool> character_loaded(const psprecomp::GuestMemory &memory) {
    // The game's existing reader inspects exactly these 12 UTF-16 units. A
    // full-width name need not have a terminator in this range. Validate it
    // without decoding or retaining any of the player's name.
    constexpr auto bytes = game::kHunterNameUnits * 2u;
    if (!memory.contains(game::kHunterName, bytes)) return std::nullopt;
    if (memory.load16(game::kHunterName) == 0u) return false;
    for (std::size_t i = 0; i < game::kHunterNameUnits; ++i) {
        const auto unit = memory.load16(game::kHunterName + static_cast<std::uint32_t>(2u * i));
        if (unit == 0u) return true;
        if (unit < 0x20u || (unit >= 0xD800u && unit <= 0xDFFFu) || unit >= 0xFFFEu)
            return std::nullopt;
    }
    return true;
}

bool same_identity(const OverlayObservation &observed,
                   const ObservedOverlayIdentity &current) noexcept {
    const auto &a = observed.identity;
    const auto &b = current.identity;
    return observed.active && observed.generation != 0u && a.matched_corpus &&
        a.base == game::kTaskSlot && a.name == "game_task.ovl" &&
        a.image_size == b.image_size && a.code_size == b.code_size &&
        a.name == b.name && a.header_fingerprint == b.header_fingerprint &&
        a.code_fingerprint == b.code_fingerprint;
}

bool same_generation(const OverlayObservation &a, const OverlayObservation &b) noexcept {
    return a.active && b.active && a.generation == b.generation &&
        a.code_epoch == b.code_epoch && a.identity.base == b.identity.base &&
        a.identity.header_fingerprint == b.identity.header_fingerprint &&
        a.identity.code_fingerprint == b.identity.code_fingerprint;
}

void read_health(const psprecomp::GuestMemory &memory, GameStateSnapshot &out) {
    if (!memory.contains(game::kHealth, 2u) ||
        !memory.contains(game::kRecoverableHealth, 2u) ||
        !memory.contains(game::kMostHealth, 2u)) return;
    const auto maximum = static_cast<std::int16_t>(memory.load16(game::kMostHealth));
    if (maximum <= 0 || maximum > kMaximumPlausibleHealth) return;
    out.maximum_health = maximum;
    const auto current = static_cast<std::int16_t>(memory.load16(game::kHealth));
    if (current < 0 || current > maximum) return;
    out.health = current;
    const auto recoverable = static_cast<std::int16_t>(memory.load16(game::kRecoverableHealth));
    if (recoverable >= current && recoverable <= maximum)
        out.recoverable_health = recoverable;
}

void read_stamina(const psprecomp::GuestMemory &memory, GameStateSnapshot &out) {
    if (!memory.contains(game::kStamina, 4u) ||
        !memory.contains(game::kMostStamina, 2u)) return;
    const auto maximum = memory.load16(game::kMostStamina);
    if (maximum == 0u || maximum > kMaximumPlausibleStamina) return;
    out.maximum_stamina = maximum;
    const float current = std::bit_cast<float>(memory.load32(game::kStamina));
    if (std::isfinite(current) && current >= 0.0f && current <= static_cast<float>(maximum) + 0.5f)
        out.stamina = static_cast<double>(current);
}

void read_clock(const psprecomp::GuestMemory &memory, GameStateSnapshot &out) {
    if (!memory.contains(game::kQuestTimeLimit, 4u) ||
        !memory.contains(game::kQuestTimeLeft, 4u)) return;
    const auto limit = memory.load32(game::kQuestTimeLimit);
    if (limit == 0u || limit > kMaximumPlausibleQuestFrames) return;
    out.quest_time_limit_frames = limit;
    const auto left = memory.load32(game::kQuestTimeLeft);
    if (left <= limit) out.quest_time_left_frames = left;
}

void read_monsters(const psprecomp::GuestMemory &memory, GameStateSnapshot &out) {
    static_assert(game::kMonsterSlots == 5u);
    if (!memory.contains(game::kMonsterTable, game::kMonsterSlots * 4u)) return;
    for (std::uint32_t i = 0; i < game::kMonsterSlots; ++i) {
        auto &slot = out.monsters[i];
        const auto address = memory.load32(game::kMonsterTable + i * 4u);
        if (address == 0u) {
            slot.status = MonsterState::Status::Empty;
            continue;
        }
        if (psprecomp::GuestMemory::canonical(address) < 0x08800000u ||
            !memory.contains(address, game::kMonsterMostHealth + 2u)) {
            slot.status = MonsterState::Status::InvalidPointer;
            continue;
        }
        const auto maximum = static_cast<std::int16_t>(memory.load16(address + game::kMonsterMostHealth));
        if (maximum <= 0) {
            slot.status = MonsterState::Status::UnclassifiedOrUnspawned;
            continue;
        }
        const auto health = static_cast<std::int16_t>(memory.load16(address + game::kMonsterHealth));
        const auto kind = memory.load8(address + game::kMonsterKind);
        if (kind >= game::kMonsterKinds || health < 0 || health > maximum) {
            slot.status = MonsterState::Status::InvalidValues;
            continue;
        }
        slot.status = MonsterState::Status::Observed;
        slot.kind = kind;
        slot.health = health;
        slot.maximum = maximum;
    }
}

const char *quest_status_name(GameStateSnapshot::QuestStatus status) noexcept {
    switch (status) {
    case GameStateSnapshot::QuestStatus::UnsupportedExecutable: return "unsupported_executable";
    case GameStateSnapshot::QuestStatus::CharacterUnavailable: return "character_unavailable";
    case GameStateSnapshot::QuestStatus::CharacterUnloaded: return "character_unloaded";
    case GameStateSnapshot::QuestStatus::OverlayUnavailable: return "overlay_unavailable";
    case GameStateSnapshot::QuestStatus::OverlayIdentityChanged: return "overlay_identity_changed";
    case GameStateSnapshot::QuestStatus::Verified: return "verified";
    }
    return "unknown";
}

const char *monster_status_name(MonsterState::Status status) noexcept {
    switch (status) {
    case MonsterState::Status::Unavailable: return "unavailable";
    case MonsterState::Status::Empty: return "empty";
    case MonsterState::Status::InvalidPointer: return "invalid_pointer";
    case MonsterState::Status::UnclassifiedOrUnspawned: return "unavailable_or_unspawned";
    case MonsterState::Status::InvalidValues: return "invalid_values";
    case MonsterState::Status::Observed: return "observed";
    }
    return "unknown";
}

template <typename T>
void add_optional(Fields &fields, std::string name, std::optional<T> value) {
    if (!value) fields.push_back({std::move(name), nullptr});
    else if constexpr (std::is_same_v<T, bool>) fields.push_back({std::move(name), *value});
    else if constexpr (std::is_floating_point_v<T>) fields.push_back({std::move(name), static_cast<double>(*value)});
    else if constexpr (std::is_signed_v<T>) fields.push_back({std::move(name), static_cast<std::int64_t>(*value)});
    else fields.push_back({std::move(name), static_cast<std::uint64_t>(*value)});
}

void add_metric(Fields &fields, const char *name, double value, bool meaningful = true) {
    const bool available = meaningful && std::isfinite(value) && value >= 0.0;
    if (available) fields.push_back({name, value});
    else fields.push_back({name, nullptr});
    fields.push_back({std::string(name) + "_available", available});
}

bool safe_present_mode(const std::string &mode) noexcept {
    if (mode.empty() || mode.size() > 64u) return false;
    for (const unsigned char ch : mode)
        if (ch < 0x20u || ch > 0x7Eu) return false;
    return true;
}
} // namespace

GameStateSnapshot read_game_state(const psprecomp::GuestMemory &memory,
                                  const GameObserver &observer,
                                  bool supported_executable) noexcept {
    GameStateSnapshot out;
    out.supported_executable = supported_executable;
    if (!supported_executable) return out;
    out.quest_status = GameStateSnapshot::QuestStatus::CharacterUnavailable;
    try {
        out.character_loaded = character_loaded(memory);
        if (!out.character_loaded) return out;
        if (!*out.character_loaded) {
            out.quest_status = GameStateSnapshot::QuestStatus::CharacterUnloaded;
            return out;
        }
        if (memory.contains(game::kMoney, 4u)) {
            const auto value = memory.load32(game::kMoney);
            if (value <= game::kMostMoney) out.money = value;
        }
        out.quest_status = GameStateSnapshot::QuestStatus::OverlayUnavailable;
        const auto observed = observer.overlay_at(game::kTaskSlot);
        if (!observed || observed->identity.base != game::kTaskSlot ||
            observed->identity.name != "game_task.ovl" || !observed->identity.matched_corpus)
            return out;
        const auto current = read_overlay_identity(memory, game::kTaskSlot);
        if (!current || !same_identity(*observed, *current)) {
            out.quest_status = GameStateSnapshot::QuestStatus::OverlayIdentityChanged;
            return out;
        }
        out.quest_status = GameStateSnapshot::QuestStatus::Verified;
        out.overlay_generation = observed->generation;
        out.code_epoch = observed->code_epoch;
        read_health(memory, out);
        read_stamina(memory, out);
        read_clock(memory, out);
        read_monsters(memory, out);
        const auto still_observed = observer.overlay_at(game::kTaskSlot);
        if (!still_observed || !same_generation(*observed, *still_observed)) {
            const auto retained_money = out.money;
            out = GameStateSnapshot{};
            out.supported_executable = true;
            out.character_loaded = true;
            out.money = retained_money;
            out.quest_status = GameStateSnapshot::QuestStatus::OverlayIdentityChanged;
        }
    } catch (...) {
        const auto retained_character = out.character_loaded;
        const auto retained_money = out.money;
        out = GameStateSnapshot{};
        out.supported_executable = true;
        out.character_loaded = retained_character;
        out.money = retained_money;
        out.quest_status = GameStateSnapshot::QuestStatus::CharacterUnavailable;
    }
    return out;
}

void observe_game_state(const psprecomp::GuestMemory &memory, GameObserver &observer,
                        bool supported_executable) noexcept {
    try {
        const auto state = read_game_state(memory, observer, supported_executable);
        Fields fields;
        fields.reserve(40u);
        fields.push_back({"source", std::string("debug_game_state_offsets_v1")});
        fields.push_back({"supported_executable", state.supported_executable});
        add_optional(fields, "character_loaded", state.character_loaded);
        add_optional(fields, "money_zenny", state.money);
        fields.push_back({"quest_status", std::string(quest_status_name(state.quest_status))});
        add_optional(fields, "overlay_generation", state.overlay_generation);
        add_optional(fields, "overlay_code_epoch", state.code_epoch);
        add_optional(fields, "health_current", state.health);
        add_optional(fields, "health_recoverable", state.recoverable_health);
        add_optional(fields, "health_maximum", state.maximum_health);
        add_optional(fields, "stamina_current_game_units", state.stamina);
        add_optional(fields, "stamina_maximum_game_units", state.maximum_stamina);
        add_optional(fields, "quest_time_left_frames", state.quest_time_left_frames);
        add_optional(fields, "quest_time_limit_frames", state.quest_time_limit_frames);
        for (std::size_t i = 0; i < state.monsters.size(); ++i) {
            const auto prefix = "monster_slot_" + std::to_string(i) + "_";
            fields.push_back({prefix + "status", std::string(monster_status_name(state.monsters[i].status))});
            add_optional(fields, prefix + "kind", state.monsters[i].kind);
            add_optional(fields, prefix + "health", state.monsters[i].health);
            add_optional(fields, prefix + "maximum_health", state.monsters[i].maximum);
        }
        observer.emit(EventKind::State, "game.state", std::move(fields));
    } catch (...) {
        observer.emit(EventKind::Error, "observer.state_error");
    }
}

void PerformanceObservation::observe(GameObserver &observer, const perf::Summary &summary) noexcept {
    if (!summary.valid || summary.second == 0u || summary.second <= last_second_) return;
    last_second_ = summary.second;
    try {
        Fields fields;
        fields.reserve(60u);
        fields.push_back({"source", std::string("perf.last_second")});
        fields.push_back({"second", summary.second});
        add_metric(fields, "fps", summary.fps);
        add_metric(fields, "game_fps", summary.game_fps);
        add_metric(fields, "emulation_speed", summary.speed);
        add_metric(fields, "display_lists_per_second", summary.lists);
        add_metric(fields, "draws_per_frame", summary.draws);
        add_metric(fields, "recorded_draws_per_frame", summary.recorded_draws);
        add_metric(fields, "render_passes_per_frame", summary.passes);
        add_metric(fields, "cleared_passes_per_frame", summary.cleared_passes);
        add_metric(fields, "target_copies_per_frame", summary.copies);
        add_metric(fields, "frame_average_ms", summary.frame_avg_ms);
        add_metric(fields, "frame_maximum_ms", summary.frame_max_ms);
        add_metric(fields, "guest_ms", summary.guest_ms);
        add_metric(fields, "render_ms", summary.render_ms);
        add_metric(fields, "wait_ms", summary.wait_ms);
        add_metric(fields, "pacing_ms", summary.pacing_ms);
        add_metric(fields, "overlay_ms", summary.overlay_ms);
        add_metric(fields, "interpolation_frame_rate", summary.frame_rate);
        add_metric(fields, "interpolation_requested_rate", summary.requested_rate);
        fields.push_back({"gpu_timing_available", summary.gpu_valid});
        add_metric(fields, "gpu_average_ms", summary.gpu_avg_ms, summary.gpu_valid);
        add_metric(fields, "gpu_maximum_ms", summary.gpu_max_ms, summary.gpu_valid);
        add_metric(fields, "vertex_peak_mib", summary.vertex_mib);
        add_metric(fields, "index_peak_mib", summary.index_mib);
        const bool mode_available = safe_present_mode(summary.present_mode);
        if (mode_available) fields.push_back({"present_mode", summary.present_mode});
        else fields.push_back({"present_mode", nullptr});
        fields.push_back({"present_mode_available", mode_available});
        add_optional(fields, "display_width", summary.width ? std::optional{summary.width} : std::nullopt);
        add_optional(fields, "display_height", summary.height ? std::optional{summary.height} : std::nullopt);
        add_metric(fields, "display_refresh_hz", summary.refresh_hz, summary.refresh_hz > 0.0f);
        observer.emit(EventKind::Performance, "perf.summary", std::move(fields));
    } catch (...) {
        observer.emit(EventKind::Error, "observer.performance_error");
    }
}

} // namespace mhp3rd::testing
