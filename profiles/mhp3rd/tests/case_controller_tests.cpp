#include "testing/case_controller.hpp"
#include "testing/probes.hpp"

#include <algorithm>
#include <condition_variable>
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
using namespace mhp3rd::testing;

int failures{};
void check(bool condition, std::string_view message) {
    if (!condition && ++failures <= 50) std::cerr << "FAIL: " << message << '\n';
}

struct MemoryState {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::uint8_t> bytes;
    bool blocked{};
    bool fail_finish{};

    std::vector<std::uint8_t> copy() {
        std::lock_guard lock(mutex);
        return bytes;
    }
    void block(bool value) {
        {
            std::lock_guard lock(mutex);
            blocked = value;
        }
        changed.notify_all();
    }
};

class MemorySink final : public JournalSink {
public:
    explicit MemorySink(std::shared_ptr<MemoryState> state) : state_(std::move(state)) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        std::unique_lock lock(state_->mutex);
        state_->changed.wait(lock, [&] { return !state_->blocked; });
        state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
    bool finish() override {
        std::lock_guard lock(state_->mutex);
        return !state_->fail_finish;
    }
private:
    std::shared_ptr<MemoryState> state_;
};

struct Fixture {
    std::shared_ptr<MemoryState> state = std::make_shared<MemoryState>();
    std::shared_ptr<SessionRecorder> recorder;
    std::shared_ptr<GameObserver> observer;

    explicit Fixture(std::size_t queue_events = 1024) {
        RecorderOptions options;
        options.max_queue_events = queue_events;
        recorder = std::make_shared<SessionRecorder>(
            std::make_unique<MemorySink>(state),
            Fields{{"role", std::string("synthetic_case_test")}}, options);
        observer = std::make_shared<GameObserver>(recorder);
    }
    JournalRecovery close() {
        state->block(false);
        check(recorder->close("test_complete"), "recorder closes cleanly");
        auto recovered = recover_journal(state->copy());
        check(recovered.complete(), "journal recovers completely");
        return recovered;
    }
};

CaseCatalog catalog() {
    CaseCatalog result;
    result.sha256 = std::string(64, 'a');
    CaseSpec first;
    first.id = "CASE-01";
    first.version = 2;
    first.title = "Synthetic route";
    first.steps = {"Observe the route"};
    first.checkpoints = {"first", "second"};
    first.required_probes = {{0x088775ACu, 1}, {0x08877818u, 1}};
    first.required_state_fields = {"health_current"};
    first.human_acceptance = true;
    result.cases.push_back(std::move(first));
    CaseSpec second;
    second.id = "CASE-02";
    second.version = 1;
    second.title = "No checkpoints";
    result.cases.push_back(std::move(second));
    return result;
}

CaseSessionInfo session(std::string role = "baseline") {
    return {std::move(role), "build-1", "run-1", "batch-1", "B0", std::string(64, 'b')};
}

struct HookState {
    std::string configuration = std::string(64, 'c');
    std::vector<std::string> calls;
    std::uint32_t mask{};
    bool throw_on_observe{};
};

CaseHooks hooks(const std::shared_ptr<GameObserver> &observer, HookState &state) {
    return {
        [&state](std::uint32_t mask) {
            state.calls.push_back("select");
            state.mask = mask;
        },
        [observer, &state](std::string_view boundary) {
            state.calls.push_back("flush:" + std::string(boundary));
            observer->emit(EventKind::Probe, "native.probe_summary",
                           {{"boundary", std::string(boundary)}});
        },
        [observer, &state] {
            state.calls.push_back("observe");
            if (state.throw_on_observe) throw std::runtime_error("synthetic state failure");
            observer->emit(EventKind::State, "game.state", {{"health_current", std::uint64_t{10}}});
        },
        [observer, &state] {
            state.calls.push_back("detail");
            observer->emit(EventKind::Probe, "native.probe_detail");
        },
        [&state] {
            state.calls.push_back("configuration");
            return state.configuration;
        },
    };
}

bool contains(std::string_view json, std::string_view needle) {
    return json.find(needle) != std::string_view::npos;
}

std::vector<const JournalRecord *> kind(const JournalRecovery &recovered, EventKind wanted) {
    std::vector<const JournalRecord *> result;
    for (const auto &record : recovered.records)
        if (record.kind == wanted) result.push_back(&record);
    return result;
}

std::uint64_t first_sequence(const JournalRecovery &recovered, std::string_view event) {
    for (const auto &record : recovered.records)
        if (contains(record.payload, "\"event\":\"" + std::string(event) + "\""))
            return record.sequence;
    return 0;
}

std::uint64_t first_boundary_sequence(const JournalRecovery &recovered,
                                      std::string_view boundary) {
    for (const auto &record : recovered.records)
        if (contains(record.payload, "\"boundary\":\"" + std::string(boundary) + "\""))
            return record.sequence;
    return 0;
}

void test_protocol_and_retries() {
    Fixture fixture;
    HookState hook_state;
    CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, hook_state));
    check(controller.recording_healthy(), "open recorder is healthy");
    check(controller.begin(0), "case begins");
    check(hook_state.mask == (kProbeAngle | kProbeVector), "known case probes become exact mask");
    check(controller.active_case() == 0, "active case index is retained by controller");
    check(!controller.begin(1), "overlapping begin is refused");
    check(!controller.finish(CaseOutcome::Normal), "early normal end is refused");
    check(controller.checkpoint(), "first checkpoint recorded");
    check(controller.progress()[0].next_checkpoint == 1, "checkpoint cursor advances");
    check(controller.anomaly(), "fixed anomaly marker recorded");
    check(controller.checkpoint(), "second checkpoint recorded");
    check(!controller.checkpoint(), "extra checkpoint is refused");
    check(controller.finish(CaseOutcome::Normal), "normal completion after all checkpoints");
    check(controller.progress()[0].state == CaseProgressState::Normal, "normal result is retained");
    check(controller.begin(0), "case can be retried");
    check(controller.progress()[0].attempt == 2, "attempt number increments per case");
    check(controller.finish(CaseOutcome::Abnormal), "abnormal end may omit checkpoints");
    check(controller.begin(1), "different case begins after previous ends");
    check(controller.progress()[1].attempt == 1, "attempt numbers are per case");
    check(controller.finish(CaseOutcome::Skipped), "skipped case ends explicitly");
    check(!controller.active_case(), "no active case remains");
    const auto recovered = fixture.close();
    const auto begins = kind(recovered, EventKind::CaseBegin);
    const auto ends = kind(recovered, EventKind::CaseEnd);
    const auto checkpoints = kind(recovered, EventKind::Checkpoint);
    const auto anomalies = kind(recovered, EventKind::Anomaly);
    check(begins.size() == 3 && ends.size() == 3 && checkpoints.size() == 2 && anomalies.size() == 1,
          "only accepted actions produce case markers");
    if (begins.size() == 3 && ends.size() == 3) {
        check(contains(begins[0]->payload, "\"attempt\":1") &&
              contains(begins[1]->payload, "\"attempt\":2") &&
              contains(begins[2]->payload, "\"attempt\":1"), "attempt identity is serialized");
        check(contains(ends[0]->payload, "\"outcome\":\"normal\"") &&
              contains(ends[1]->payload, "\"outcome\":\"abnormal\"") &&
              contains(ends[2]->payload, "\"outcome\":\"skipped\""), "explicit outcomes are serialized");
    }
    check(first_sequence(recovered, "case.begin") < first_sequence(recovered, "native.probe_summary"),
          "begin precedes starting probe snapshot");
    check(first_sequence(recovered, "game.state") < first_sequence(recovered, "case.checkpoint"),
          "state observation precedes checkpoint marker");
    check(first_sequence(recovered, "case.checkpoint") < first_sequence(recovered, "native.probe_detail"),
          "detail follows checkpoint marker");
    check(first_boundary_sequence(recovered, "case_end") != 0 &&
          first_boundary_sequence(recovered, "case_end") < first_sequence(recovered, "case.end"),
          "final probe snapshot precedes CaseEnd");
    check(std::find(hook_state.calls.begin(), hook_state.calls.end(), "flush:case_end") != hook_state.calls.end(),
          "case end flush callback ran");
    for (const auto &record : recovered.records) {
        if (record.kind == EventKind::CaseBegin || record.kind == EventKind::CaseEnd ||
            record.kind == EventKind::Checkpoint || record.kind == EventKind::Anomaly) {
            check(contains(record.payload, "\"case_id\"") &&
                  contains(record.payload, "\"case_version\"") &&
                  contains(record.payload, "\"attempt\""), "all case markers share identity fields");
        }
    }
}

std::string prerequisite_for(std::string role, std::string configuration) {
    Fixture fixture;
    HookState state;
    state.configuration = std::move(configuration);
    auto identity = session(std::move(role));
    identity.build_version = "other-build";
    identity.run_id = "other-run";
    CaseController controller(fixture.observer, catalog(), identity, hooks(fixture.observer, state));
    check(controller.begin(0), "prerequisite fixture begins");
    const auto hash = controller.progress()[0].prerequisites_sha256;
    check(hash.size() == 64, "prerequisite digest is SHA-256");
    check(controller.finish(CaseOutcome::Uncertain), "uncertain end is explicit");
    (void)fixture.close();
    return hash;
}

void test_prerequisites_and_config_change() {
    const auto baseline = prerequisite_for("baseline", std::string(64, 'c'));
    const auto candidate = prerequisite_for("candidate", std::string(64, 'c'));
    check(baseline == candidate, "paired role and build/run labels do not affect prerequisites");
    check(baseline != prerequisite_for("candidate", std::string(64, 'd')),
          "current configuration changes prerequisites");

    Fixture fixture;
    HookState state;
    CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, state));
    check(controller.begin(0), "configuration-change case begins");
    state.configuration = std::string(64, 'd');
    check(!controller.checkpoint(), "changed configuration stops case");
    check(controller.progress()[0].state == CaseProgressState::Interrupted,
          "changed configuration retains interrupted progress");
    check(!controller.recording_healthy(), "configuration violation fails closed");
    const auto recovered = fixture.close();
    check(kind(recovered, EventKind::CaseEnd).empty(), "configuration change never invents CaseEnd");
    check(first_sequence(recovered, "case.interrupted") != 0, "interruption emits a state marker");
}

void test_lifetime_and_errors() {
    {
        Fixture fixture;
        HookState state;
        auto controller = std::make_shared<CaseController>(
            fixture.observer, catalog(), session(), hooks(fixture.observer, state));
        set_active_case_controller(controller);
        check(controller->begin(0), "routed case begins");
        auto panel_reference = active_case_controller();
        panel_reference.reset();
        check(controller->active_case() == 0, "case survives menu reference closure");
        controller->close("window_close");
        check(controller->progress()[0].state == CaseProgressState::Interrupted,
              "run close interrupts unfinished attempt");
        set_active_case_controller({});
        const auto recovered = fixture.close();
        check(kind(recovered, EventKind::CaseEnd).empty(), "window close leaves attempt unfinished");
        check(first_sequence(recovered, "case.interrupted") != 0, "window close records interruption");
    }
    {
        Fixture fixture;
        HookState state;
        state.throw_on_observe = true;
        CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, state));
        check(controller.begin(0), "callback-error case begins");
        check(!controller.checkpoint(), "callback exception is contained");
        check(controller.progress()[0].state == CaseProgressState::Interrupted,
              "callback failure interrupts case");
        check(!controller.recording_healthy(), "callback failure hides healthy status");
        const auto recovered = fixture.close();
        check(!kind(recovered, EventKind::Error).empty(), "callback failure emits Error evidence");
        check(kind(recovered, EventKind::CaseEnd).empty(), "callback failure has no normal end");
    }
    {
        HookState state;
        CaseController disabled({}, catalog(), session(), {});
        check(!disabled.begin(0) && !disabled.recording_healthy(), "disabled recorder rejects case start");
    }
    {
        Fixture fixture;
        HookState state;
        CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, state));
        (void)fixture.close();
        check(!controller.begin(0) && !controller.recording_healthy(), "closed recorder rejects case start");
    }
    {
        Fixture fixture;
        HookState state;
        CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, state));
        fixture.observer->emit(EventKind::State, "bad.event", {{"event", true}});
        check(!controller.begin(0), "observer emission error rejects case start");
        (void)fixture.close();
    }
    {
        Fixture fixture;
        HookState state;
        CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, state));
        check(controller.begin(1) && controller.finish(CaseOutcome::Normal),
              "checkpoint-free case can complete normally");
        fixture.observer->emit(EventKind::State, "bad.event", {{"event", true}});
        check(controller.effective_state(1) == CaseProgressState::Interrupted,
              "later observer failure cannot present a normal completion as healthy");
        (void)fixture.close();
    }
    {
        Fixture fixture;
        HookState state;
        CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, state));
        check(controller.begin(1) && controller.finish(CaseOutcome::Normal),
              "clean-close case completes normally");
        controller.close("window_close");
        (void)fixture.close();
        check(!controller.recording_healthy(), "closed recorder is unavailable for new actions");
        check(controller.effective_state(1) == CaseProgressState::Normal,
              "clean session close retains an already completed normal case");
    }
    {
        Fixture fixture;
        HookState state;
        CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, state));
        check(controller.begin(1) && controller.finish(CaseOutcome::Normal),
              "I/O-failure case completes before writer failure");
        controller.close("window_close");
        {
            std::lock_guard lock(fixture.state->mutex);
            fixture.state->fail_finish = true;
        }
        check(!fixture.recorder->close("test_complete"), "synthetic sink finalization fails");
        check(fixture.observer->recorder_health().io_failed, "recorder retains I/O failure");
        check(controller.effective_state(1) == CaseProgressState::Interrupted,
              "later I/O failure invalidates normal completion");
    }
}

void test_unsupported_probe_and_loss() {
    {
        Fixture fixture;
        HookState state;
        auto invalid = catalog();
        invalid.cases[0].required_probes.push_back({0x12345678u, 1});
        CaseController controller(fixture.observer, std::move(invalid), session(),
                                  hooks(fixture.observer, state));
        check(!controller.begin(0), "unsupported probe refuses case start");
        check(state.calls.empty(), "unsupported probe cannot silently select a partial mask");
        const auto recovered = fixture.close();
        check(kind(recovered, EventKind::CaseBegin).empty(), "unsupported probe emits no CaseBegin");
    }
    {
        Fixture fixture(4);
        HookState state;
        CaseController controller(fixture.observer, catalog(), session(), hooks(fixture.observer, state));
        check(controller.begin(0), "loss fixture begins");
        fixture.state->block(true);
        for (int i = 0; i < 100; ++i)
            fixture.observer->emit(EventKind::Input, "synthetic.input", {{"ordinal", std::uint64_t(i)}});
        check(fixture.observer->recorder_health().dropped_events > 0, "bounded queue records loss");
        check(!controller.recording_healthy(), "recording loss clears healthy state");
        check(!controller.checkpoint(), "recording loss stops active case");
        check(controller.progress()[0].state == CaseProgressState::Interrupted,
              "recording loss retains interrupted state");
        fixture.state->block(false);
        (void)fixture.close();
    }
}

} // namespace

int main() {
    test_protocol_and_retries();
    test_prerequisites_and_config_change();
    test_lifetime_and_errors();
    test_unsupported_probe_and_loss();
    if (failures) std::cerr << failures << " case controller checks failed\n";
    return failures ? 1 : 0;
}
