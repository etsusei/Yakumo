#include "testing/texture_decode_observation.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using mhp3rd::gpu::TextureDecodeCounters;
using mhp3rd::gpu::TextureDecodeMode;
using mhp3rd::testing::EventKind;
using mhp3rd::testing::GameObserver;
using mhp3rd::testing::JournalRecord;
using mhp3rd::testing::JournalSink;
using mhp3rd::testing::SessionRecorder;
using mhp3rd::testing::TextureDecodeObservation;
using Clock = TextureDecodeObservation::Clock;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

struct MemoryState {
    std::mutex mutex;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] std::vector<std::uint8_t> copy() {
        std::lock_guard lock{mutex};
        return bytes;
    }
};

class MemorySink final : public JournalSink {
public:
    explicit MemorySink(std::shared_ptr<MemoryState> state) : state_(std::move(state)) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        std::lock_guard lock{state_->mutex};
        state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
private:
    std::shared_ptr<MemoryState> state_;
};

struct Fixture {
    std::shared_ptr<MemoryState> memory = std::make_shared<MemoryState>();
    std::shared_ptr<SessionRecorder> recorder = std::make_shared<SessionRecorder>(
        std::make_unique<MemorySink>(memory),
        mhp3rd::testing::Fields{{"role", std::string{"synthetic"}}});
    std::shared_ptr<GameObserver> observer = std::make_shared<GameObserver>(recorder);

    [[nodiscard]] std::vector<JournalRecord> finish() {
        require(recorder->close("test_done"), "synthetic journal did not close");
        auto recovered = mhp3rd::testing::recover_journal(memory->copy());
        require(recovered.complete(), "synthetic journal did not recover completely");
        require(!recovered.loss_seen, "synthetic journal lost events");
        std::vector<JournalRecord> records;
        for (auto &record : recovered.records) {
            if (record.kind == EventKind::RunBegin || record.kind == EventKind::RunEnd) continue;
            records.push_back(std::move(record));
        }
        return records;
    }
};

void expect_field(std::string_view payload, std::string_view key,
                  std::string_view literal) {
    const std::string needle = "\"" + std::string{key} + "\":" + std::string{literal};
    const auto at = payload.find(needle);
    require(at != std::string_view::npos, "missing or wrong JSON field: " + std::string{key});
    const auto next = at + needle.size();
    require(next < payload.size() && (payload[next] == ',' || payload[next] == '}'),
            "JSON field has an inexact value: " + std::string{key});
}

void expect_common(const JournalRecord &record, TextureDecodeMode mode, bool final) {
    require(record.kind == EventKind::State, "texture count used a non-state event");
    const auto &payload = record.payload;
    expect_field(payload, "event", "\"texture_decode.counters\"");
    expect_field(payload, "schema", "\"yakumo-texture-decode-v1\"");
    expect_field(payload, "mode", mode == TextureDecodeMode::Verify ? "\"verify\"" : "\"native\"");
    expect_field(payload, "scope", "\"renderer_cache_miss_decodes_not_all_draws\"");
    expect_field(payload, "final", final ? "true" : "false");
    expect_field(payload, "workers_drained", final ? "true" : "false");
}

void expect_counters(const JournalRecord &record, const TextureDecodeCounters &counters) {
    const auto &payload = record.payload;
    const auto number = [&](std::string_view name, std::uint64_t value) {
        expect_field(payload, name, std::to_string(value));
    };
    number("requests", counters.requests);
    number("immediate_requests", counters.immediate_requests);
    number("async_requests", counters.async_requests);
    number("async_capture_attempts", counters.async_capture_attempts);
    number("snapshot_rejected", counters.snapshot_rejected);
    number("unsupported_state", counters.unsupported_state);
    number("portable_success", counters.portable_success);
    number("verified", counters.verified);
    number("native", counters.native);
    number("fallbacks", counters.fallbacks);
    number("mismatches", counters.mismatches);
    number("errors", counters.errors);
    number("legacy_failures", counters.legacy_failures);
    number("portable_elapsed_ns", counters.portable_elapsed_ns);
    number("reference_elapsed_ns", counters.reference_elapsed_ns);
}

TextureDecodeCounters large_counters() {
    TextureDecodeCounters counters{};
    counters.requests = 4294967305ull;
    counters.immediate_requests = 4294967297ull;
    counters.async_requests = 8u;
    counters.async_capture_attempts = 11u;
    counters.snapshot_rejected = 2u;
    counters.unsupported_state = 3u;
    counters.portable_success = 9u;
    counters.verified = 7u;
    counters.native = 0u;
    counters.fallbacks = 5u;
    counters.mismatches = 1u;
    counters.errors = 2u;
    counters.legacy_failures = 4u;
    counters.portable_elapsed_ns = 4294967296123ull;
    counters.reference_elapsed_ns = 9876543210123ull;
    return counters;
}

void check_off_emits_nothing() {
    Fixture fixture;
    TextureDecodeObservation observation{fixture.observer, TextureDecodeMode::Off};
    const auto t0 = Clock::time_point{} + std::chrono::seconds{10};
    require(!observation.report({}, false, t0), "off emitted first periodic report");
    require(!observation.report(large_counters(), true, t0 + std::chrono::seconds{1}),
            "off emitted final report");
    require(fixture.finish().empty(), "off added an observation record");
    require(fixture.observer->emission_errors() == 0u, "off caused an observer error");
}

void check_periodic_and_final(TextureDecodeMode mode) {
    Fixture fixture;
    TextureDecodeObservation observation{fixture.observer, mode};
    const auto t0 = Clock::time_point{} + std::chrono::seconds{10};
    const TextureDecodeCounters zero{};
    const TextureDecodeCounters large = large_counters();
    require(observation.report(zero, false, t0), "first periodic report suppressed");
    require(!observation.report(large, false, t0 + std::chrono::milliseconds{999}),
            "subsecond periodic report emitted");
    require(observation.report(large, false, t0 + std::chrono::seconds{1}),
            "one-second periodic report suppressed");
    require(observation.report(large, true, t0 + std::chrono::milliseconds{1001}),
            "final report failed to bypass periodic interval");
    require(!observation.report(large, true, t0 + std::chrono::seconds{2}),
            "second final report emitted");
    require(!observation.report(large, false, t0 + std::chrono::seconds{3}),
            "periodic report emitted after final");
    const auto records = fixture.finish();
    require(records.size() == 3u, "periodic/final report count differs");
    expect_common(records[0], mode, false);
    expect_common(records[1], mode, false);
    expect_common(records[2], mode, true);
    expect_counters(records[0], zero);
    expect_counters(records[1], large);
    expect_counters(records[2], large);
    require(!observation.report(large, true, t0 + std::chrono::seconds{4}),
            "closed recorder received another final report");
    require(fixture.observer->emission_errors() == 0u,
            "suppressed report after close caused an observer error");
    require(fixture.recorder->health().accepted_events == 3u,
            "suppressed report after close added an event");
}

void check_expired_observer_is_not_rebound() {
    mhp3rd::testing::set_active_observer({});
    Fixture original;
    TextureDecodeObservation observation{original.observer, TextureDecodeMode::Verify};
    mhp3rd::testing::set_active_observer(original.observer);
    mhp3rd::testing::set_active_observer({});
    original.observer.reset();
    require(original.finish().empty(), "expired original observer had an unexpected event");

    Fixture replacement;
    mhp3rd::testing::set_active_observer(replacement.observer);
    const auto t0 = Clock::time_point{} + std::chrono::seconds{10};
    require(!observation.report(large_counters(), true, t0),
            "expired original observer rebound to active replacement");
    mhp3rd::testing::set_active_observer({});
    require(replacement.finish().empty(), "replacement observer received old texture report");
    require(replacement.observer->emission_errors() == 0u,
            "expired observer report caused replacement emission error");
}
} // namespace

int main() {
    try {
        check_off_emits_nothing();
        check_periodic_and_final(TextureDecodeMode::Verify);
        check_periodic_and_final(TextureDecodeMode::Native);
        check_expired_observer_is_not_rebound();
        std::cout << "texture decode observation lifecycle and journal fields passed\n";
        return 0;
    } catch (const std::exception &error) {
        mhp3rd::testing::set_active_observer({});
        std::cerr << "texture decode observation tests failed: " << error.what() << '\n';
        return 1;
    }
}
