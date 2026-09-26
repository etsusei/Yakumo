#include "testing/probes.hpp"

#include "testing/game_observers.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

namespace mhp3rd::testing {
namespace {
constexpr std::array<std::uint32_t, 5> kEntries{
    0x088775ACu, 0x08878B28u, 0x08878B4Cu, 0x08877818u, 0x08879D08u,
};
constexpr std::array<const char *, 5> kNames{
    "angle", "scale_matrix", "translation_matrix", "vector_construct", "matrix_copy",
};
constexpr std::array<std::uint32_t, 5> kBits{
    kProbeAngle, kProbeScale, kProbeTranslation, kProbeVector, kProbeCopy,
};
constexpr std::array<const char *, 4> kVariantNames{"aot", "native", "verify", "fallback"};
std::atomic<std::uint64_t> next_counter_epoch{1};

const char *outcome_name(ProbeOutcome outcome) noexcept {
    switch (outcome) {
    case ProbeOutcome::Completed: return "completed";
    case ProbeOutcome::UncertifiedReturn: return "uncertified_return";
    case ProbeOutcome::Abnormal: return "abnormal";
    case ProbeOutcome::Mismatch: return "scope_mismatch";
    case ProbeOutcome::Failure: return "failed";
    case ProbeOutcome::Overflow: return "stack_overflow";
    case ProbeOutcome::ReturnMismatch: return "return_mismatch";
    case ProbeOutcome::OrphanExit: return "orphan_exit";
    }
    return "unknown";
}

std::uint64_t steady_ns(void *) noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void increment(std::uint64_t &value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

void add_saturating(std::uint64_t &value, std::uint64_t addition) noexcept {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    value = addition > max - value ? max : value + addition;
}

template <std::size_t Size>
bool matches_span(const psprecomp::GuestMemory &memory, std::uint32_t entry,
                  const char *sha256) {
    std::array<std::uint8_t, Size> bytes{};
    if (!memory.contains(entry, Size)) return false;
    memory.copy_out(entry, bytes);
    return psprecomp::sha256_bytes(bytes) == sha256;
}

// This checks the current guest bytes on every selected entry. Runtime code
// writes are not guaranteed to advance an observer epoch, so an epoch-only
// cache would allow a stale certificate after mutation.
bool certify(psprecomp::Runtime &runtime, std::uint32_t entry) noexcept {
    try {
        const auto &memory = runtime.memory();
        switch (entry) {
        case 0x088775ACu:
            return matches_span<100>(memory, entry,
                "c80198f08479038b9d5e0a26516fa8c180f60f11e11bf7521f21208efe63ee49");
        case 0x08878B28u:
            return matches_span<36>(memory, entry,
                "d9ad67fd4b7e26ea8b213c8297489c288c39161f9190bb267f9f5ade35a799b6");
        case 0x08878B4Cu:
            return matches_span<36>(memory, entry,
                "5b4fa38cc0f789cbf2339d55400eabeb849a4ab5bfc505f169bfb7704fe57038");
        case 0x08877818u:
            return matches_span<24>(memory, entry,
                "c0c7dc6c33d91c46b68700e1454930520debad811da2d7629a88a4e7d5b3349d");
        case 0x08879D08u:
            return matches_span<80>(memory, entry,
                "e918aeb6363b81be6cab6ddb2c2605f2179d24bcfa418ea3f24dc06ef393f6c9");
        default: return false;
        }
    } catch (...) {
        return false;
    }
}

void add_variant_fields(Fields &fields, const char *name, const ProbeVariantStats &stats) {
    const std::string prefix(name);
    fields.push_back({prefix + "_calls", stats.calls});
    fields.push_back({prefix + "_total_ns", stats.total_ns});
    fields.push_back({prefix + "_max_ns", stats.max_ns});
}
} // namespace

ProbeTracker::ProbeTracker(std::uint32_t mask, Clock clock, void *clock_context) noexcept
    : mask_(mask & kProbeAll), clock_(clock ? clock : steady_ns), clock_context_(clock_context) {
    for (std::size_t i = 0; i < leaves_.size(); ++i) leaves_[i].entry = kEntries[i];
}

int ProbeTracker::leaf_index(std::uint32_t entry) noexcept {
    for (std::size_t i = 0; i < kEntries.size(); ++i)
        if (entry == kEntries[i]) return static_cast<int>(i);
    return -1;
}

ProbeTracker::ThreadSlot *ProbeTracker::slot_for(std::thread::id id, bool create) noexcept {
    for (auto &slot : threads_) if (slot.id == id) return &slot;
    if (!create) return nullptr;
    for (auto &slot : threads_) {
        if (slot.id == std::thread::id{} || slot.depth == 0) {
            slot.id = id;
            return &slot;
        }
    }
    return nullptr;
}

void ProbeTracker::detail(ProbeDetail value) noexcept {
    value.sequence = detail_sequence_;
    if (detail_sequence_ != std::numeric_limits<std::uint64_t>::max()) ++detail_sequence_;
    recent_[recent_next_] = value;
    recent_next_ = (recent_next_ + 1) % recent_.size();
    if (recent_count_ < recent_.size()) ++recent_count_;
}

void ProbeTracker::incomplete(Frame frame, std::uint64_t ProbeLeafStats::*reason,
                              ProbeOutcome outcome, ProbeVariant variant,
                              bool variant_known) noexcept {
    const int index = leaf_index(frame.entry);
    if (index < 0) return;
    auto &stats = leaves_[static_cast<std::size_t>(index)];
    increment(stats.incomplete);
    increment(stats.*reason);
    detail({0, frame.token, frame.entry, frame.aot_path ? ProbeVariant::Aot : variant,
            outcome, frame.start_ns, 0, frame.aot_path || variant_known,
            true, false, frame.certified});
}

void ProbeTracker::complete(Frame frame, ProbeVariant variant, std::uint64_t end_ns) noexcept {
    const int index = leaf_index(frame.entry);
    if (index < 0) return;
    auto &stats = leaves_[static_cast<std::size_t>(index)];
    if (!frame.certified) {
        increment(stats.uncertified_returns);
        detail({0, frame.token, frame.entry, variant, ProbeOutcome::UncertifiedReturn,
                frame.start_ns, 0, true, true, false, false});
        return;
    }
    const auto variant_index = static_cast<std::size_t>(variant);
    if (variant_index >= stats.variants.size()) {
        incomplete(frame, &ProbeLeafStats::incomplete_failure, ProbeOutcome::Failure,
                   variant, true);
        return;
    }
    // A supplied clock may be nonmonotonic; never turn that into a huge sample.
    if (end_ns < frame.start_ns) {
        incomplete(frame, &ProbeLeafStats::incomplete_failure, ProbeOutcome::Failure,
                   variant, true);
        return;
    }
    const auto duration = end_ns - frame.start_ns;
    auto &variant_stats = stats.variants[variant_index];
    increment(stats.completed);
    increment(variant_stats.calls);
    add_saturating(variant_stats.total_ns, duration);
    variant_stats.max_ns = std::max(variant_stats.max_ns, duration);
    detail({0, frame.token, frame.entry, variant, ProbeOutcome::Completed,
            frame.start_ns, duration, true, true, true, true});
}

void ProbeTracker::mismatch(ThreadSlot &slot, std::uint32_t exit_entry) noexcept {
    bool matched_entry = false;
    for (std::size_t i = 0; i < slot.depth; ++i) {
        matched_entry |= slot.frames[i].entry == exit_entry;
        incomplete(slot.frames[i], &ProbeLeafStats::incomplete_mismatch,
                   ProbeOutcome::Mismatch);
    }
    slot.depth = 0;
    if (!matched_entry) {
        const int index = leaf_index(exit_entry);
        if (index >= 0) {
            increment(leaves_[static_cast<std::size_t>(index)].orphan_exits);
            detail({0, 0, exit_entry, ProbeVariant::Aot, ProbeOutcome::OrphanExit,
                    0, 0, true, false, false, false});
        }
    }
}

std::uint64_t ProbeTracker::enter(const void *runtime, const void *context,
                                  std::uint32_t entry, bool certified,
                                  bool aot_path) noexcept {
    const int index = leaf_index(entry);
    if (index < 0 || (mask_ & kBits[static_cast<std::size_t>(index)]) == 0) return 0;
    std::lock_guard lock(mutex_);
    if (final_) return 0;
    auto &stats = leaves_[static_cast<std::size_t>(index)];
    increment(stats.entry_hits);
    increment(certified ? stats.certified_entries : stats.uncertified_entries);
    auto *slot = slot_for(std::this_thread::get_id(), true);
    if (!slot || slot->depth == kMaxDepth || next_token_ == 0) {
        increment(stats.incomplete);
        increment(stats.incomplete_overflow);
        detail({0, 0, entry, ProbeVariant::Aot, ProbeOutcome::Overflow,
                0, 0, aot_path, false, false, certified});
        return 0;
    }
    const auto token = next_token_++;
    slot->frames[slot->depth++] = {runtime, context, entry, token,
                                  clock_(clock_context_), certified, aot_path};
    return token;
}

void ProbeTracker::exit_aot(std::uint64_t token, const void *runtime, const void *context,
                            std::uint32_t entry, std::uint32_t jump_target,
                            std::uint32_t expected_return) noexcept {
    if (token == 0) return;
    const auto end_ns = clock_(clock_context_);
    std::lock_guard lock(mutex_);
    if (final_) return;
    const int index = leaf_index(entry);
    if (index < 0) return;
    auto *slot = slot_for(std::this_thread::get_id(), false);
    if (!slot || slot->depth == 0) {
        increment(leaves_[static_cast<std::size_t>(index)].orphan_exits);
        detail({0, token, entry, ProbeVariant::Aot, ProbeOutcome::OrphanExit,
                0, 0, true, false, false, false});
        return;
    }
    const auto frame = slot->frames[slot->depth - 1];
    if (frame.token != token || frame.runtime != runtime || frame.context != context ||
        frame.entry != entry) {
        mismatch(*slot, entry);
        return;
    }
    --slot->depth;
    if (jump_target != expected_return) {
        increment(leaves_[static_cast<std::size_t>(index)].return_mismatches);
        incomplete(frame, &ProbeLeafStats::incomplete_mismatch,
                   ProbeOutcome::ReturnMismatch);
    } else {
        complete(frame, ProbeVariant::Aot, end_ns);
    }
}

void ProbeTracker::finish(std::uint64_t token, const void *runtime, const void *context,
                          std::uint32_t entry, ProbeVariant variant, bool success) noexcept {
    if (token == 0) return;
    const auto end_ns = clock_(clock_context_);
    std::lock_guard lock(mutex_);
    if (final_) return;
    const int index = leaf_index(entry);
    if (index < 0) return;
    auto *slot = slot_for(std::this_thread::get_id(), false);
    if (!slot || slot->depth == 0) {
        increment(leaves_[static_cast<std::size_t>(index)].orphan_exits);
        detail({0, token, entry, variant, ProbeOutcome::OrphanExit,
                0, 0, true, false, false, false});
        return;
    }
    const auto frame = slot->frames[slot->depth - 1];
    if (frame.token != token || frame.runtime != runtime || frame.context != context ||
        frame.entry != entry) {
        mismatch(*slot, entry);
        return;
    }
    --slot->depth;
    if (success) complete(frame, variant, end_ns);
    else incomplete(frame, &ProbeLeafStats::incomplete_failure,
                    ProbeOutcome::Failure, variant, true);
}

void ProbeTracker::abandon(std::uint64_t token, const void *runtime, const void *context,
                           std::uint32_t entry) noexcept {
    if (token == 0) return;
    std::lock_guard lock(mutex_);
    if (final_) return;
    const int index = leaf_index(entry);
    if (index < 0) return;
    auto *slot = slot_for(std::this_thread::get_id(), false);
    if (!slot || slot->depth == 0) {
        increment(leaves_[static_cast<std::size_t>(index)].orphan_exits);
        detail({0, token, entry, ProbeVariant::Aot, ProbeOutcome::OrphanExit,
                0, 0, false, false, false, false});
        return;
    }
    const auto frame = slot->frames[slot->depth - 1];
    if (frame.token != token || frame.runtime != runtime || frame.context != context ||
        frame.entry != entry) {
        mismatch(*slot, entry);
        return;
    }
    --slot->depth;
    incomplete(frame, &ProbeLeafStats::incomplete_abnormal, ProbeOutcome::Abnormal);
}

void ProbeTracker::reject(std::uint64_t token, const void *runtime,
                          const void *context, std::uint32_t entry) noexcept {
    if (token == 0) return;
    std::lock_guard lock(mutex_);
    if (final_) return;
    auto *slot = slot_for(std::this_thread::get_id(), false);
    if (!slot || slot->depth == 0) {
        const int index = leaf_index(entry);
        if (index >= 0) increment(leaves_[static_cast<std::size_t>(index)].orphan_exits);
        return;
    }
    const auto frame = slot->frames[slot->depth - 1];
    if (frame.token != token || frame.runtime != runtime || frame.context != context ||
        frame.entry != entry) {
        mismatch(*slot, entry);
        return;
    }
    --slot->depth;
    incomplete(frame, &ProbeLeafStats::incomplete_mismatch, ProbeOutcome::Mismatch);
}

void ProbeTracker::orphan_exit(std::uint32_t entry) noexcept {
    const int index = leaf_index(entry);
    if (index < 0 || (mask_ & kBits[static_cast<std::size_t>(index)]) == 0) return;
    std::lock_guard lock(mutex_);
    if (!final_) {
        increment(leaves_[static_cast<std::size_t>(index)].orphan_exits);
        detail({0, 0, entry, ProbeVariant::Aot, ProbeOutcome::OrphanExit,
                0, 0, true, false, false, false});
    }
}

void ProbeTracker::overflow_entry(std::uint32_t entry, bool certified) noexcept {
    const int index = leaf_index(entry);
    if (index < 0 || (mask_ & kBits[static_cast<std::size_t>(index)]) == 0) return;
    std::lock_guard lock(mutex_);
    if (final_) return;
    auto &stats = leaves_[static_cast<std::size_t>(index)];
    increment(stats.entry_hits);
    increment(certified ? stats.certified_entries : stats.uncertified_entries);
    increment(stats.incomplete);
    increment(stats.incomplete_overflow);
    detail({0, 0, entry, ProbeVariant::Aot, ProbeOutcome::Overflow,
            0, 0, true, false, false, certified});
}

void ProbeTracker::invalidate_current_thread() noexcept {
    std::lock_guard lock(mutex_);
    if (final_) return;
    auto *slot = slot_for(std::this_thread::get_id(), false);
    if (!slot) return;
    for (std::size_t i = 0; i < slot->depth; ++i)
        incomplete(slot->frames[i], &ProbeLeafStats::incomplete_overflow,
                   ProbeOutcome::Overflow);
    slot->depth = 0;
}

ProbeSnapshot ProbeTracker::flush(bool final) noexcept {
    std::lock_guard lock(mutex_);
    if (final && !final_) {
        for (auto &slot : threads_) {
            for (std::size_t i = 0; i < slot.depth; ++i)
                incomplete(slot.frames[i], &ProbeLeafStats::incomplete_abnormal,
                           ProbeOutcome::Abnormal);
            slot.depth = 0;
            slot.id = {};
        }
        final_ = true;
    }
    ProbeSnapshot snapshot;
    snapshot.mask = mask_;
    snapshot.final = final_;
    snapshot.leaves = leaves_;
    snapshot.recent_count = recent_count_;
    const auto oldest = (recent_next_ + recent_.size() - recent_count_) % recent_.size();
    for (std::size_t i = 0; i < recent_count_; ++i)
        snapshot.recent[i] = recent_[(oldest + i) % recent_.size()];
    return snapshot;
}

class NativeProbeSession {
public:
    NativeProbeSession(std::shared_ptr<GameObserver> observer, std::uint32_t mask)
        : observer_(std::move(observer)), tracker_(mask), mask_(mask & kProbeAll),
          counter_epoch_(next_counter_epoch.fetch_add(1, std::memory_order_relaxed)) {}

    [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint32_t mask() const noexcept { return mask_; }
    [[nodiscard]] bool selected(std::uint32_t entry) const noexcept {
        return (mask_ & mask_for(entry)) != 0;
    }
    [[nodiscard]] std::uint64_t enter(psprecomp::Runtime &runtime,
                                      const psprecomp::AllegrexContext &context,
                                      std::uint32_t entry, bool aot_path = false) noexcept {
        if (!active() || !selected(entry)) return 0;
        return tracker_.enter(&runtime, &context, entry, certify(runtime, entry), aot_path);
    }
    void exit_aot(std::uint64_t token, psprecomp::Runtime &runtime,
                  const psprecomp::AllegrexContext &context, std::uint32_t entry,
                  std::uint32_t jump_target, std::uint32_t entry_return_address) noexcept {
        if (!active()) return;
        tracker_.exit_aot(token, &runtime, &context, entry, jump_target, entry_return_address);
    }
    void finish(std::uint64_t token, const void *runtime, const void *context,
                std::uint32_t entry, ProbeVariant variant, bool success) noexcept {
        if (active()) tracker_.finish(token, runtime, context, entry, variant, success);
    }
    void abandon(std::uint64_t token, const void *runtime, const void *context,
                 std::uint32_t entry) noexcept {
        if (active()) tracker_.abandon(token, runtime, context, entry);
    }
    void reject(std::uint64_t token, const void *runtime, const void *context,
                std::uint32_t entry) noexcept {
        if (active()) tracker_.reject(token, runtime, context, entry);
    }
    void orphan_exit(std::uint32_t entry) noexcept {
        if (active()) tracker_.orphan_exit(entry);
    }
    void overflow_entry(psprecomp::Runtime &runtime, std::uint32_t entry) noexcept {
        if (active() && selected(entry))
            tracker_.overflow_entry(entry, certify(runtime, entry));
    }
    void invalidate_current_thread() noexcept {
        if (active()) tracker_.invalidate_current_thread();
    }
    void emit_snapshot(bool final, std::string_view boundary = {}) noexcept {
        if (final) active_.store(false, std::memory_order_release);
        else if (!active()) return;
        const auto snapshot = tracker_.flush(final);
        bool issues = false;
        for (std::size_t i = 0; i < snapshot.leaves.size(); ++i) {
            if ((snapshot.mask & kBits[i]) == 0) continue;
            const auto &stats = snapshot.leaves[i];
            try {
                const bool coverage_complete = stats.entry_hits != 0 &&
                    stats.certified_entries == stats.entry_hits &&
                    stats.completed == stats.entry_hits && stats.incomplete == 0 &&
                    stats.orphan_exits == 0;
                const char *coverage = coverage_complete ? "certified" :
                    stats.entry_hits == 0 && stats.orphan_exits == 0 ? "not_covered" :
                    stats.entry_hits == 0 ? "orphan_exit" :
                    stats.certified_entries == 0 ? "uncertified" : "partial";
                Fields fields{
                    {"entry", static_cast<std::uint64_t>(stats.entry)},
                    {"leaf", std::string(kNames[i])},
                    {"boundary", std::string(boundary)},
                    {"counter_epoch", counter_epoch_},
                    {"coverage", std::string(coverage)},
                    {"coverage_complete", coverage_complete},
                    {"certification", std::string("full_current_span_sha256_per_entry")},
                    {"timing_scope", std::string("matched_entry_return_monotonic_ns")},
                    {"timing_overhead", std::string("instrumentation_and_observer_callbacks_included")},
                    {"final", snapshot.final},
                    {"entry_hits", stats.entry_hits},
                    {"certified_entries", stats.certified_entries},
                    {"uncertified_entries", stats.uncertified_entries},
                    {"uncertified_returns", stats.uncertified_returns},
                    {"completed", stats.completed},
                    {"incomplete", stats.incomplete},
                    {"incomplete_abnormal", stats.incomplete_abnormal},
                    {"incomplete_mismatch", stats.incomplete_mismatch},
                    {"incomplete_failure", stats.incomplete_failure},
                    {"incomplete_overflow", stats.incomplete_overflow},
                    {"orphan_exits", stats.orphan_exits},
                    {"return_mismatches", stats.return_mismatches},
                };
                for (std::size_t variant = 0; variant < kVariantNames.size(); ++variant)
                    add_variant_fields(fields, kVariantNames[variant], stats.variants[variant]);
                observer_->emit(EventKind::Probe, "probe.summary", std::move(fields), final);
                if (stats.uncertified_entries || stats.incomplete || stats.orphan_exits ||
                    stats.return_mismatches) {
                    issues = true;
                    observer_->emit(EventKind::Error, "probe.issues", {
                        {"entry", static_cast<std::uint64_t>(stats.entry)},
                        {"uncertified_entries", stats.uncertified_entries},
                        {"incomplete", stats.incomplete},
                        {"orphan_exits", stats.orphan_exits},
                        {"return_mismatches", stats.return_mismatches},
                    }, final);
                }
            } catch (...) {
                observer_->emit(EventKind::Error, "probe.reporting_error", {}, final);
            }
        }
        if (final || issues) emit_detail(snapshot);
    }

    void emit_detail() noexcept { emit_detail(tracker_.flush()); }

private:
    friend void native_probe_verification_mismatch(psprecomp::Runtime &runtime,
                                                   const psprecomp::AllegrexContext &context,
                                                   std::uint32_t entry) noexcept;
    void emit_verification_mismatch(psprecomp::Runtime &runtime,
                                    const psprecomp::AllegrexContext &context,
                                    std::uint32_t entry) noexcept {
        if (!active() || !selected(entry) || !observer_) return;
        const bool certified = certify(runtime, entry);
        try {
            observer_->emit(EventKind::Error, "native.verification_mismatch", {
                {"entry", static_cast<std::uint64_t>(entry)},
                {"evidence", std::string("same_input_reference")},
                {"certified", certified},
                {"guest_pc", static_cast<std::uint64_t>(context.pc)},
            });
        } catch (...) {
            observer_->emit(EventKind::Error, "probe.reporting_error");
        }
    }
    void emit_detail(const ProbeSnapshot &snapshot) noexcept {
        std::lock_guard lock(detail_mutex_);
        if (snapshot.recent_count == 0) return;
        const auto first = snapshot.recent[0].sequence;
        try {
            if (first > last_emitted_sequence_ && first - last_emitted_sequence_ > 1) {
                observer_->emit(EventKind::Probe, "probe.detail_gap", {
                    {"first_missing_sequence", last_emitted_sequence_ + 1},
                    {"last_missing_sequence", first - 1},
                    {"retained_capacity", static_cast<std::uint64_t>(snapshot.recent.size())},
                    {"reason", std::string("bounded_retention")},
                });
            }
            for (std::size_t i = 0; i < snapshot.recent_count; ++i) {
                const auto &item = snapshot.recent[i];
                if (item.sequence <= last_emitted_sequence_) continue;
                const auto variant_index = static_cast<std::size_t>(item.variant);
                const char *variant = item.variant_known && variant_index < kVariantNames.size()
                    ? kVariantNames[variant_index] : "unresolved";
                observer_->emit(EventKind::Probe, "probe.detail", {
                    {"sequence", item.sequence},
                    {"token", item.token},
                    {"entry", static_cast<std::uint64_t>(item.entry)},
                    {"variant", std::string(variant)},
                    {"outcome", std::string(outcome_name(item.outcome))},
                    {"start_ns", item.start_known ? FieldValue{item.start_ns} : FieldValue{nullptr}},
                    {"duration_ns", item.duration_known ? FieldValue{item.duration_ns} : FieldValue{nullptr}},
                    {"certified", item.certified},
                }, snapshot.final);
                last_emitted_sequence_ = item.sequence;
            }
        } catch (...) {
            observer_->emit(EventKind::Error, "probe.reporting_error", {}, snapshot.final);
        }
    }
    static std::uint32_t mask_for(std::uint32_t entry) noexcept {
        for (std::size_t i = 0; i < kEntries.size(); ++i)
            if (entry == kEntries[i]) return kBits[i];
        return 0;
    }
    std::shared_ptr<GameObserver> observer_;
    ProbeTracker tracker_;
    const std::uint32_t mask_;
    const std::uint64_t counter_epoch_;
    std::atomic<bool> active_{true};
    std::mutex detail_mutex_;
    std::uint64_t last_emitted_sequence_{};
};

namespace {
std::shared_ptr<NativeProbeSession> current_session;
std::atomic<std::uint32_t> current_mask{};

struct AotLink {
    std::weak_ptr<NativeProbeSession> session;
    const void *runtime{};
    const void *context{};
    std::uint32_t entry{};
    std::uint32_t entry_return_address{};
    psprecomp::RuntimeExecutionContextToken guest_thread{};
    std::uint64_t token{};
};
thread_local std::array<AotLink, ProbeTracker::kMaxDepth> aot_links{};
thread_local std::size_t aot_depth{};
thread_local std::weak_ptr<NativeProbeSession> suppressed_session;
thread_local bool suppressed{};

void compact_active_links(const std::shared_ptr<NativeProbeSession> &session) noexcept {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < aot_depth; ++i) {
        auto linked = aot_links[i].session.lock();
        if (linked && linked->active() && linked == session) {
            if (kept != i) aot_links[kept] = std::move(aot_links[i]);
            ++kept;
        }
    }
    for (std::size_t i = kept; i < aot_depth; ++i) aot_links[i] = {};
    aot_depth = kept;
}
} // namespace

void configure_native_probes(std::shared_ptr<GameObserver> observer, std::uint32_t mask) noexcept {
    std::shared_ptr<NativeProbeSession> next;
    try {
        if (observer && (mask & kProbeAll) != 0)
            next = std::make_shared<NativeProbeSession>(observer, mask & kProbeAll);
    } catch (...) {
        if (observer) observer->emit(EventKind::Error, "probe.configuration_error");
        next.reset();
    }
    current_mask.store(next ? next->mask() : 0, std::memory_order_release);
    auto previous = std::atomic_exchange_explicit(&current_session, std::move(next),
                                                   std::memory_order_acq_rel);
    if (previous) previous->emit_snapshot(true);
}

void flush_native_probes(bool final, std::string_view boundary) noexcept {
    std::shared_ptr<NativeProbeSession> session;
    if (final) {
        current_mask.store(0, std::memory_order_release);
        session = std::atomic_exchange_explicit(&current_session,
            std::shared_ptr<NativeProbeSession>{}, std::memory_order_acq_rel);
    } else {
        session = std::atomic_load_explicit(&current_session, std::memory_order_acquire);
    }
    if (session) session->emit_snapshot(final, boundary);
}

void flush_native_probe_detail() noexcept {
    auto session = std::atomic_load_explicit(&current_session, std::memory_order_acquire);
    if (session && session->active()) session->emit_detail();
}

void native_probe_verification_mismatch(psprecomp::Runtime &runtime,
                                        const psprecomp::AllegrexContext &context,
                                        std::uint32_t entry) noexcept {
    if (current_mask.load(std::memory_order_acquire) == 0) return;
    auto session = std::atomic_load_explicit(&current_session, std::memory_order_acquire);
    if (session) session->emit_verification_mismatch(runtime, context, entry);
}

void native_probe_aot_enter(psprecomp::Runtime &runtime,
                            const psprecomp::AllegrexContext &context,
                            std::uint32_t entry) noexcept {
    if (current_mask.load(std::memory_order_acquire) == 0) return;
    auto session = std::atomic_load_explicit(&current_session, std::memory_order_acquire);
    if (!session || !session->selected(entry)) return;
    if (suppressed) {
        if (auto old = suppressed_session.lock(); old && old == session && old->active()) {
            session->overflow_entry(runtime, entry);
            return;
        }
        suppressed = false;
        suppressed_session.reset();
    }
    compact_active_links(session);
    const auto token = session->enter(runtime, context, entry, true);
    if (token == 0 && !session->active()) return;
    if (token == 0 || aot_depth == aot_links.size()) {
        // An untracked nested return must not consume an older same-entry
        // token. Discard the whole open stack and suppress AOT matching on
        // this host thread until the next session. An abnormal unwind cannot
        // otherwise be distinguished from a later same-entry call.
        session->invalidate_current_thread();
        suppressed_session = session;
        suppressed = true;
        for (std::size_t i = 0; i < aot_depth; ++i) aot_links[i] = {};
        aot_depth = 0;
        return;
    }
    aot_links[aot_depth++] = {session, &runtime, &context, entry, context.gpr[31],
                              psprecomp::capture_runtime_execution_context(), token};
}

void native_probe_aot_exit(psprecomp::Runtime &runtime,
                           const psprecomp::AllegrexContext &context,
                           std::uint32_t entry, std::uint32_t jump_target) noexcept {
    if (suppressed) {
        const auto old = suppressed_session.lock();
        const auto current = std::atomic_load_explicit(&current_session, std::memory_order_acquire);
        if (old && old == current && old->active()) return;
        suppressed = false;
        suppressed_session.reset();
    }
    if (aot_depth == 0) {
        if (auto session = std::atomic_load_explicit(&current_session, std::memory_order_acquire))
            session->orphan_exit(entry);
        return;
    }
    std::size_t found = aot_depth;
    for (std::size_t i = aot_depth; i > 0; --i) {
        const auto &link = aot_links[i - 1];
        if (link.runtime == &runtime && link.context == &context && link.entry == entry) {
            found = i - 1;
            break;
        }
    }
    if (found == aot_depth) {
        if (auto session = std::atomic_load_explicit(&current_session, std::memory_order_acquire))
            session->orphan_exit(entry);
        return;
    }
    auto link = std::move(aot_links[found]);
    for (std::size_t i = found; i < aot_depth; ++i) aot_links[i] = {};
    aot_depth = found;
    if (auto session = link.session.lock()) {
        if (!psprecomp::runtime_execution_context_matches(link.guest_thread))
            session->reject(link.token, &runtime, &context, entry);
        else
            session->exit_aot(link.token, runtime, context, entry, jump_target,
                              link.entry_return_address);
    }
}

NativeProbeScope::NativeProbeScope(psprecomp::Runtime &runtime,
                                   const psprecomp::AllegrexContext &context,
                                   std::uint32_t entry) noexcept
    : runtime_(&runtime), context_(&context), entry_(entry) {
    if (current_mask.load(std::memory_order_acquire) == 0) return;
    session_ = std::atomic_load_explicit(&current_session, std::memory_order_acquire);
    if (session_) {
        token_ = session_->enter(runtime, context, entry);
        if (token_) {
            const auto guest_thread = psprecomp::capture_runtime_execution_context();
            guest_thread_uid_ = guest_thread.thread_uid;
            guest_thread_generation_ = guest_thread.switch_generation;
        }
    }
}

NativeProbeScope::~NativeProbeScope() {
    if (session_ && token_) session_->abandon(token_, runtime_, context_, entry_);
}

void NativeProbeScope::finish(ProbeVariant variant, bool success) noexcept {
    if (!session_ || !token_) return;
    if (!psprecomp::runtime_execution_context_matches(
            {guest_thread_uid_, guest_thread_generation_}))
        session_->reject(token_, runtime_, context_, entry_);
    else
        session_->finish(token_, runtime_, context_, entry_, variant, success);
    token_ = 0;
    session_.reset();
}
} // namespace mhp3rd::testing
