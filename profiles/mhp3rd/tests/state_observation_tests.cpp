#include "testing/state_observation.hpp"

#include "debug/game_state.hpp"
#include "perf/frame_stats.hpp"
#include "psprecomp/guest_memory.hpp"
#include "testing/overlay_observation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace mhp3rd::testing;
namespace game = mhp3rd::debug::p3rd;

int failures{};
void check(bool condition, const char *description) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}

struct MemoryState {
    std::mutex mutex;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> copy() {
        std::lock_guard lock(mutex);
        return bytes;
    }
};
class MemorySink final : public JournalSink {
public:
    explicit MemorySink(std::shared_ptr<MemoryState> state) : state_(std::move(state)) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        std::lock_guard lock(state_->mutex);
        state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
private:
    std::shared_ptr<MemoryState> state_;
};
struct Fixture {
    std::shared_ptr<MemoryState> state = std::make_shared<MemoryState>();
    std::shared_ptr<SessionRecorder> recorder = std::make_shared<SessionRecorder>(
        std::make_unique<MemorySink>(state), Fields{{"role", std::string("synthetic")}});
    GameObserver observer{recorder};
    JournalRecovery finish() {
        check(recorder->close("test_done"), "recorder close");
        return recover_journal(state->copy());
    }
};

void put_overlay(psprecomp::GuestMemory &memory, const char *name = "game_task.ovl") {
    std::vector<std::uint8_t> image(80u, 0u);
    image[0] = 'M'; image[1] = 'W'; image[2] = 'o'; image[3] = '3';
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4u; ++i) image[at + i] = static_cast<std::uint8_t>(value >> (i * 8u));
    };
    put32(8, game::kTaskSlot);
    put32(12, 8u);
    put32(16, 8u);
    for (std::size_t i = 0; name[i] != 0; ++i) image[32u + i] = static_cast<std::uint8_t>(name[i]);
    for (std::size_t i = 64u; i < 72u; ++i) image[i] = static_cast<std::uint8_t>(i);
    memory.copy_in(game::kTaskSlot, image);
}

OverlayIdentity current_overlay(const psprecomp::GuestMemory &memory, bool matched = true) {
    auto observed = read_overlay_identity(memory, game::kTaskSlot);
    check(observed.has_value(), "synthetic overlay is readable");
    if (!observed) return {};
    observed->identity.matched_corpus = matched;
    return observed->identity;
}

void put_character(psprecomp::GuestMemory &memory) {
    memory.store16(game::kHunterName, 'T');
    memory.store16(game::kHunterName + 2u, 'E');
    memory.store16(game::kHunterName + 4u, 'S');
    memory.store16(game::kHunterName + 6u, 'T');
    memory.store16(game::kHunterName + 8u, 0u);
}

void put_quest_fields(psprecomp::GuestMemory &memory) {
    memory.store32(game::kMoney, 12'345u);
    memory.store16(game::kHealth, 100u);
    memory.store16(game::kRecoverableHealth, 125u);
    memory.store16(game::kMostHealth, 150u);
    memory.store32(game::kStamina, std::bit_cast<std::uint32_t>(724.5f));
    memory.store16(game::kMostStamina, 900u);
    memory.store32(game::kQuestTimeLimit, 90'000u);
    memory.store32(game::kQuestTimeLeft, 89'970u);
    constexpr std::uint32_t monster = 0x09FA0000u;
    memory.store32(game::kMonsterTable, monster);
    memory.store8(monster + game::kMonsterKind, 5u);
    memory.store16(monster + game::kMonsterHealth, 4'400u);
    memory.store16(monster + game::kMonsterMostHealth, 4'400u);
}

void state_checks() {
    psprecomp::GuestMemory memory(64u * 1024u * 1024u);
    Fixture fixture;
    put_character(memory);
    put_overlay(memory);
    put_quest_fields(memory);
    fixture.observer.overlay(current_overlay(memory));

    const auto unsupported = read_game_state(memory, fixture.observer, false);
    check(!unsupported.character_loaded && !unsupported.money && !unsupported.health &&
          unsupported.quest_status == GameStateSnapshot::QuestStatus::UnsupportedExecutable,
          "unsupported executable exposes no state");

    const auto ordinary = read_game_state(memory, fixture.observer, true);
    check(ordinary.character_loaded == true && ordinary.money == 12'345u &&
          ordinary.quest_status == GameStateSnapshot::QuestStatus::Verified &&
          ordinary.overlay_generation == 1u && ordinary.health == 100 &&
          ordinary.recoverable_health == 125 && ordinary.maximum_health == 150 &&
          ordinary.stamina == 724.5 && ordinary.maximum_stamina == 900 &&
          ordinary.quest_time_left_frames == 89'970u && ordinary.quest_time_limit_frames == 90'000u,
          "known supported quest fixture is read with its overlay generation");
    check(ordinary.monsters[0].status == MonsterState::Status::Observed &&
          ordinary.monsters[0].kind == 5u && ordinary.monsters[0].health == 4'400 &&
          ordinary.monsters[1].status == MonsterState::Status::Empty,
          "validated monster slots are reported");
    memory.store8(game::kTaskSlot + 72u, 0xEEu);
    check(read_game_state(memory, fixture.observer, true).quest_status ==
          GameStateSnapshot::QuestStatus::Verified,
          "mutable overlay data does not invalidate code identity");

    // Const observation must leave the whole guest RAM byte-for-byte intact.
    const auto *before_pointer = memory.raw_pointer(psprecomp::GuestMemory::kPhysicalBase, memory.size());
    check(before_pointer != nullptr, "full RAM span exists");
    const std::vector<std::uint8_t> before(before_pointer, before_pointer + memory.size());
    observe_game_state(memory, fixture.observer, true);
    const auto *after_pointer = memory.raw_pointer(psprecomp::GuestMemory::kPhysicalBase, memory.size());
    check(after_pointer && std::equal(before.begin(), before.end(), after_pointer),
          "state observation never mutates guest RAM");

    memory.store16(game::kHunterName, 0u);
    const auto unloaded = read_game_state(memory, fixture.observer, true);
    check(unloaded.character_loaded == false && !unloaded.money && !unloaded.health &&
          unloaded.quest_status == GameStateSnapshot::QuestStatus::CharacterUnloaded,
          "unloaded character does not expose quest values");
    put_character(memory);
    memory.store16(game::kHunterName, 0xD800u);
    const auto invalid_character = read_game_state(memory, fixture.observer, true);
    check(!invalid_character.character_loaded && !invalid_character.money &&
          invalid_character.quest_status == GameStateSnapshot::QuestStatus::CharacterUnavailable,
          "invalid character marker is unavailable");
    put_character(memory);
    for (std::uint32_t i = 0; i < game::kHunterNameUnits; ++i)
        memory.store16(game::kHunterName + i * 2u, static_cast<std::uint16_t>('A' + i));
    memory.store16(game::kHunterName + game::kHunterNameUnits * 2u, 0xD800u);
    check(read_game_state(memory, fixture.observer, true).character_loaded == true,
          "a valid fixed-width name does not depend on the following field");
    put_character(memory);
    psprecomp::GuestMemory small_memory(32u * 1024u * 1024u);
    put_character(small_memory);
    const auto missing_ram = read_game_state(small_memory, fixture.observer, true);
    check(missing_ram.character_loaded == true && !missing_ram.health &&
          missing_ram.quest_status == GameStateSnapshot::QuestStatus::OverlayIdentityChanged,
          "unmapped task overlay cannot authorize quest interpretation");

    fixture.observer.overlay_unload(game::kTaskSlot, "synthetic");
    const auto unloaded_overlay = read_game_state(memory, fixture.observer, true);
    check(unloaded_overlay.quest_status == GameStateSnapshot::QuestStatus::OverlayUnavailable &&
          !unloaded_overlay.health, "unloaded overlay cannot authorize quest reads");
    fixture.observer.overlay(current_overlay(memory, false));
    const auto unmatched = read_game_state(memory, fixture.observer, true);
    check(unmatched.quest_status == GameStateSnapshot::QuestStatus::OverlayUnavailable &&
          !unmatched.health, "unmatched overlay cannot authorize quest reads");
    fixture.observer.overlay(current_overlay(memory));
    memory.store8(game::kTaskSlot + 64u, 0xFFu);
    const auto stale_code = read_game_state(memory, fixture.observer, true);
    check(stale_code.quest_status == GameStateSnapshot::QuestStatus::OverlayIdentityChanged &&
          !stale_code.health, "code edit without header edit invalidates attribution");
    memory.store8(game::kTaskSlot + 64u, 64u);
    put_overlay(memory, "lobby_task.ovl");
    const auto replaced = read_game_state(memory, fixture.observer, true);
    check(replaced.quest_status == GameStateSnapshot::QuestStatus::OverlayIdentityChanged &&
          !replaced.health, "replaced task overlay invalidates attribution");
    put_overlay(memory);
    fixture.observer.code_epoch("synthetic invalidation");
    const auto stale_generation = read_game_state(memory, fixture.observer, true);
    check(stale_generation.quest_status == GameStateSnapshot::QuestStatus::OverlayUnavailable &&
          !stale_generation.health, "stale overlay generation cannot authorize reads");
    fixture.observer.overlay(current_overlay(memory));
    const auto refreshed = read_game_state(memory, fixture.observer, true);
    check(refreshed.quest_status == GameStateSnapshot::QuestStatus::Verified &&
          refreshed.overlay_generation && *refreshed.overlay_generation > 1u,
          "new validation generation restores attribution");

    constexpr std::uint32_t monster = 0x09FA0000u;
    memory.store16(monster + game::kMonsterMostHealth, 0u);
    check(read_game_state(memory, fixture.observer, true).monsters[0].status ==
          MonsterState::Status::UnclassifiedOrUnspawned,
          "zero monster maximum remains unclassified");
    memory.store16(monster + game::kMonsterMostHealth, 4'400u);
    memory.store32(game::kMonsterTable, 0x07FFFFFFu);
    memory.store16(game::kHealth, 0xFFFFu);
    memory.store32(game::kStamina, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
    memory.store32(game::kQuestTimeLeft, 100'000u);
    const auto invalid_values = read_game_state(memory, fixture.observer, true);
    check(invalid_values.quest_status == GameStateSnapshot::QuestStatus::Verified &&
          !invalid_values.health && invalid_values.maximum_health == 150 &&
          !invalid_values.stamina && invalid_values.maximum_stamina == 900 &&
          !invalid_values.quest_time_left_frames && invalid_values.quest_time_limit_frames == 90'000u &&
          invalid_values.monsters[0].status == MonsterState::Status::InvalidPointer,
          "invalid independent values are unavailable without hiding valid context");

    const auto journal = fixture.finish();
    check(journal.complete(), "state journal framing is complete");
    bool found_state = false;
    for (const auto &record : journal.records) {
        if (record.payload.find("\"event\":\"game.state\"") == std::string::npos) continue;
        found_state = true;
        check(record.payload.find("TEST") == std::string::npos, "hunter name never enters state journal");
        check(record.payload.find("\"quest_status\":\"verified\"") != std::string::npos,
              "state record contains verified quest gate");
    }
    check(found_state, "game state record was emitted");
}

void performance_checks() {
    Fixture fixture;
    PerformanceObservation observation;
    mhp3rd::perf::Summary summary;
    summary.valid = true;
    summary.second = 1u;
    summary.fps = std::numeric_limits<double>::quiet_NaN();
    summary.game_fps = 30.0;
    summary.speed = std::numeric_limits<double>::infinity();
    summary.gpu_valid = false;
    summary.gpu_avg_ms = std::numeric_limits<double>::quiet_NaN();
    summary.present_mode = "fifo";
    observation.observe(fixture.observer, summary);
    observation.observe(fixture.observer, summary);
    summary.second = 2u;
    summary.fps = 60.0;
    summary.speed = 1.0;
    observation.observe(fixture.observer, summary);
    summary.valid = false;
    summary.second = 3u;
    observation.observe(fixture.observer, summary);
    const auto journal = fixture.finish();
    check(journal.complete(), "performance journal framing is complete");
    int summaries = 0;
    for (const auto &record : journal.records) {
        if (record.payload.find("\"event\":\"perf.summary\"") == std::string::npos) continue;
        ++summaries;
        if (record.payload.find("\"second\":1") != std::string::npos) {
            check(record.payload.find("\"fps\":null") != std::string::npos &&
                  record.payload.find("\"fps_available\":false") != std::string::npos &&
                  record.payload.find("\"game_fps\":30") != std::string::npos &&
                  record.payload.find("\"emulation_speed\":null") != std::string::npos &&
                  record.payload.find("\"gpu_average_ms\":null") != std::string::npos,
                  "nonfinite values are unavailable but valid summary fields remain");
        }
    }
    check(summaries == 2, "each new valid performance second emits exactly once");
}
} // namespace

int main() {
    state_checks();
    performance_checks();
    std::cout << "State observation failures: " << failures << '\n';
    return failures ? 1 : 0;
}
