#include "testing/probes.hpp"
#include "testing/game_observers.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using mhp3rd::testing::ProbeTracker;
using mhp3rd::testing::ProbeVariant;
using namespace mhp3rd::testing;

constexpr std::uint32_t kAngle = 0x088775ACu;
constexpr std::uint32_t kVector = 0x08877818u;
constexpr std::uint32_t kScale = 0x08878B28u;

int failures = 0;
void check(bool value, std::string_view message) {
    if (!value && ++failures <= 30) std::cerr << "FAIL: " << message << '\n';
}

struct FakeClock {
    std::uint64_t now{};
    static std::uint64_t read(void *context) noexcept {
        return static_cast<FakeClock *>(context)->now;
    }
};

void counts_and_variants() {
    FakeClock clock{100};
    ProbeTracker tracker(kProbeAngle | kProbeVector, FakeClock::read, &clock);
    int runtime = 1, context = 2;
    check(tracker.enter(&runtime, &context, kScale, true) == 0, "unselected leaf is disabled");
    const auto aot = tracker.enter(&runtime, &context, kAngle, true);
    check(aot != 0, "selected certified entry tracked");
    clock.now = 275;
    tracker.exit_aot(aot, &runtime, &context, kAngle, 0x1000u, 0x1000u);
    const auto native = tracker.enter(&runtime, &context, kAngle, true);
    clock.now = 325;
    tracker.finish(native, &runtime, &context, kAngle, ProbeVariant::Native);
    const auto verify = tracker.enter(&runtime, &context, kAngle, true);
    clock.now = 525;
    tracker.finish(verify, &runtime, &context, kAngle, ProbeVariant::Verify);
    const auto fallback = tracker.enter(&runtime, &context, kAngle, true);
    clock.now = 550;
    tracker.finish(fallback, &runtime, &context, kAngle, ProbeVariant::Fallback);
    const auto no_certificate = tracker.enter(&runtime, &context, kAngle, false);
    clock.now = 600;
    tracker.exit_aot(no_certificate, &runtime, &context, kAngle, 0x1000u, 0x1000u);

    const auto snapshot = tracker.flush();
    const auto &angle = snapshot.leaves[0];
    check(angle.entry_hits == 5 && angle.certified_entries == 4 &&
          angle.uncertified_entries == 1, "entry and certification counts");
    check(angle.completed == 4 && angle.uncertified_returns == 1 &&
          angle.incomplete == 0, "certified completion excludes uncertified return");
    check(angle.variants[0].calls == 1 && angle.variants[0].total_ns == 175 &&
          angle.variants[0].max_ns == 175, "AOT duration matches entry and return");
    check(angle.variants[1].calls == 1 && angle.variants[1].total_ns == 50,
          "native duration remains separate");
    check(angle.variants[2].calls == 1 && angle.variants[2].total_ns == 200,
          "verification duration remains separate");
    check(angle.variants[3].calls == 1 && angle.variants[3].total_ns == 25,
          "fallback duration remains separate");
    check(snapshot.leaves[3].entry_hits == 0 && snapshot.leaves[3].completed == 0,
          "selected zero-call leaf has explicit empty coverage");
    check(snapshot.leaves[1].entry_hits == 0, "unselected leaf has no hits");
}

void abnormal_and_mismatched_scopes() {
    FakeClock clock{10};
    ProbeTracker tracker(kProbeAngle | kProbeVector, FakeClock::read, &clock);
    int runtime = 1, context = 2, other_context = 3;
    const auto angle = tracker.enter(&runtime, &context, kAngle, true);
    const auto vector = tracker.enter(&runtime, &context, kVector, true);
    clock.now = 20;
    tracker.exit_aot(angle, &runtime, &context, kAngle, 0x1000u, 0x1000u);
    auto snapshot = tracker.flush();
    check(snapshot.leaves[0].incomplete_mismatch == 1 &&
          snapshot.leaves[3].incomplete_mismatch == 1,
          "out-of-order exit invalidates both nested scopes");
    tracker.exit_aot(vector, &runtime, &context, kVector, 0x1000u, 0x1000u);
    snapshot = tracker.flush();
    check(snapshot.leaves[3].orphan_exits == 1, "late exit after mismatch is orphaned");

    const auto changed_context = tracker.enter(&runtime, &context, kAngle, true);
    tracker.finish(changed_context, &runtime, &other_context, kAngle, ProbeVariant::Native);
    snapshot = tracker.flush();
    check(snapshot.leaves[0].incomplete_mismatch == 2,
          "context identity is required for a completion");

    const auto wrong_return = tracker.enter(&runtime, &context, kAngle, true);
    tracker.exit_aot(wrong_return, &runtime, &context, kAngle, 0x2000u, 0x1000u);
    snapshot = tracker.flush();
    check(snapshot.leaves[0].return_mismatches == 1 &&
          snapshot.leaves[0].incomplete_mismatch == 3,
          "return target mismatch cannot create a timing sample");

    const auto failed = tracker.enter(&runtime, &context, kAngle, true);
    tracker.finish(failed, &runtime, &context, kAngle, ProbeVariant::Verify, false);
    const auto abandoned = tracker.enter(&runtime, &context, kAngle, true);
    tracker.abandon(abandoned, &runtime, &context, kAngle);
    snapshot = tracker.flush();
    check(snapshot.leaves[0].incomplete_failure == 1 &&
          snapshot.leaves[0].incomplete_abnormal == 1,
          "failed finish and destructor-style abandon have distinct reasons");
    check(snapshot.leaves[0].completed == 0, "bad scopes add no certified completions");
}

void bounded_stack_and_finalization() {
    FakeClock clock{1};
    ProbeTracker tracker(kProbeAngle, FakeClock::read, &clock);
    int runtime = 1, context = 2;
    std::uint64_t tokens[ProbeTracker::kMaxDepth]{};
    for (auto &token : tokens) token = tracker.enter(&runtime, &context, kAngle, true);
    check(tracker.enter(&runtime, &context, kAngle, true) == 0,
          "stack capacity bounds nesting");
    for (std::size_t i = ProbeTracker::kMaxDepth; i > 0; --i)
        tracker.finish(tokens[i - 1], &runtime, &context, kAngle, ProbeVariant::Native);
    auto snapshot = tracker.flush();
    check(snapshot.leaves[0].entry_hits == ProbeTracker::kMaxDepth + 1 &&
          snapshot.leaves[0].incomplete_overflow == 1 &&
          snapshot.leaves[0].completed == ProbeTracker::kMaxDepth,
          "overflow is explicit and tracked frames still complete");

    const auto outstanding = tracker.enter(&runtime, &context, kAngle, true);
    snapshot = tracker.flush(true);
    check(snapshot.final && snapshot.leaves[0].incomplete_abnormal == 1,
          "final flush marks outstanding scope incomplete");
    tracker.finish(outstanding, &runtime, &context, kAngle, ProbeVariant::Native);
    check(tracker.enter(&runtime, &context, kAngle, true) == 0,
          "finalized tracker cannot receive a new session's calls");
    check(tracker.flush().leaves[0].completed == ProbeTracker::kMaxDepth,
          "late finish cannot alter finalized summary");

    ProbeTracker restarted(kProbeAngle, FakeClock::read, &clock);
    check(restarted.flush(true).leaves[0].entry_hits == 0,
          "new session begins with independent zero-call coverage");
}

void thread_identity() {
    FakeClock clock{5};
    ProbeTracker tracker(kProbeAngle, FakeClock::read, &clock);
    int runtime = 1, context = 2;
    const auto token = tracker.enter(&runtime, &context, kAngle, true);
    std::thread other([&] {
        tracker.finish(token, &runtime, &context, kAngle, ProbeVariant::Native);
    });
    other.join();
    const auto snapshot = tracker.flush(true);
    check(snapshot.leaves[0].orphan_exits == 1 &&
          snapshot.leaves[0].incomplete_abnormal == 1 &&
          snapshot.leaves[0].completed == 0,
          "completion on a different host thread does not match the entry");
}

void recent_detail_ring() {
    FakeClock clock{100};
    ProbeTracker tracker(kProbeAngle, FakeClock::read, &clock);
    int runtime = 1, context = 2;
    for (unsigned i = 0; i < 40; ++i) {
        const auto token = tracker.enter(&runtime, &context, kAngle, true);
        clock.now += 10;
        tracker.finish(token, &runtime, &context, kAngle, ProbeVariant::Native);
    }
    const auto snapshot = tracker.flush();
    check(snapshot.recent_count == ProbeSnapshot::kRecentCapacity,
          "recent detail ring is bounded");
    check(snapshot.recent[0].sequence == 9 && snapshot.recent[0].token == 9 &&
          snapshot.recent[31].sequence == 40 && snapshot.recent[31].token == 40,
          "recent detail keeps the latest records in operation order");
    check(snapshot.recent[31].outcome == ProbeOutcome::Completed &&
          snapshot.recent[31].duration_known && snapshot.recent[31].duration_ns == 10,
          "completed detail retains measured duration");
    const auto incomplete_token = tracker.enter(&runtime, &context, kAngle, true);
    tracker.abandon(incomplete_token, &runtime, &context, kAngle);
    const auto last = tracker.flush().recent[31];
    check(last.sequence == 41 && last.outcome == ProbeOutcome::Abnormal &&
          !last.duration_known, "incomplete detail has no invented duration");
}

struct MemoryJournal {
    std::mutex mutex;
    std::vector<std::uint8_t> bytes;
};
class MemorySink final : public JournalSink {
public:
    explicit MemorySink(std::shared_ptr<MemoryJournal> state) : state_(std::move(state)) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        std::lock_guard lock(state_->mutex);
        state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
private:
    std::shared_ptr<MemoryJournal> state_;
};
struct JournalFixture {
    std::shared_ptr<MemoryJournal> state = std::make_shared<MemoryJournal>();
    std::shared_ptr<SessionRecorder> recorder = std::make_shared<SessionRecorder>(
        std::make_unique<MemorySink>(state), Fields{{"role", std::string("synthetic")}});
    std::shared_ptr<GameObserver> observer = std::make_shared<GameObserver>(recorder);

    JournalRecovery close() {
        check(recorder->close("probe_test"), "probe journal closes");
        std::lock_guard lock(state->mutex);
        return recover_journal(state->bytes);
    }
};

const JournalRecord *find_record(const JournalRecovery &journal, std::string_view event,
                                  std::string_view field = {}) {
    for (const auto &record : journal.records) {
        if (record.payload.find(event) != std::string::npos &&
            (field.empty() || record.payload.find(field) != std::string::npos))
            return &record;
    }
    return nullptr;
}

void callback_bookkeeping() {
    psprecomp::Runtime runtime;
    psprecomp::AllegrexContext context{};
    context.gpr[31] = 0x1000u;
    JournalFixture first;
    configure_native_probes(first.observer, kProbeAngle | kProbeVector);
    native_probe_aot_exit(runtime, context, kAngle, context.gpr[31]);
    native_probe_aot_enter(runtime, context, kAngle);
    native_probe_aot_exit(runtime, context, kAngle, context.gpr[31]);
    {
        NativeProbeScope native(runtime, context, kAngle);
        native.finish(ProbeVariant::Fallback);
    }
    flush_native_probe_detail();
    flush_native_probe_detail();
    flush_native_probes(true);
    const auto journal = first.close();
    check(journal.complete(), "callback journal recovers");
    const auto *summary = find_record(journal, "probe.summary", "\"leaf\":\"angle\"");
    check(summary && summary->payload.find("\"entry_hits\":2") != std::string::npos &&
          summary->payload.find("\"uncertified_entries\":2") != std::string::npos &&
          summary->payload.find("\"orphan_exits\":1") != std::string::npos &&
          summary->payload.find("\"coverage\":\"uncertified\"") != std::string::npos,
          "real callbacks classify unmatched and uncertified calls without timing");
    check(find_record(journal, "probe.summary", "\"leaf\":\"vector_construct\"") != nullptr,
          "final callback flush reports zero-call selected leaf");
    std::size_t details = 0;
    for (const auto &record : journal.records)
        if (record.payload.find("\"event\":\"probe.detail\"") != std::string::npos) ++details;
    check(details == 3, "explicit detail flushes deduplicate sequence numbers");

    JournalFixture second;
    configure_native_probes(second.observer, kProbeAngle);
    for (std::size_t i = 0; i <= ProbeTracker::kMaxDepth; ++i)
        native_probe_aot_enter(runtime, context, kAngle);
    for (std::size_t i = 0; i <= ProbeTracker::kMaxDepth; ++i)
        native_probe_aot_exit(runtime, context, kAngle, context.gpr[31]);
    // A new call after the overflow is also suppressed for this session.
    native_probe_aot_enter(runtime, context, kAngle);
    native_probe_aot_exit(runtime, context, kAngle, context.gpr[31]);
    flush_native_probes(true);
    const auto overflow_journal = second.close();
    check(overflow_journal.complete(), "overflow journal recovers");
    summary = find_record(overflow_journal, "probe.summary", "\"leaf\":\"angle\"");
    check(summary && summary->payload.find("\"entry_hits\":66") != std::string::npos &&
          summary->payload.find("\"incomplete_overflow\":66") != std::string::npos &&
          summary->payload.find("\"completed\":0") != std::string::npos &&
          summary->payload.find("\"uncertified_returns\":0") != std::string::npos,
          "overflow never pairs a nested same-entry return with an older scope");

    JournalFixture restarted;
    configure_native_probes(restarted.observer, kProbeAngle);
    flush_native_probes(true);
    const auto restart_journal = restarted.close();
    summary = find_record(restart_journal, "probe.summary", "\"leaf\":\"angle\"");
    check(summary && summary->payload.find("\"entry_hits\":0") != std::string::npos &&
          summary->payload.find("\"coverage\":\"not_covered\"") != std::string::npos,
          "restart does not inherit old session's calls or suppression");
}
} // namespace

int main() {
    counts_and_variants();
    abnormal_and_mismatched_scopes();
    bounded_stack_and_finalization();
    thread_identity();
    recent_detail_ring();
    callback_bookkeeping();
    if (failures) std::cerr << failures << " probe test(s) failed\n";
    return failures ? 1 : 0;
}
