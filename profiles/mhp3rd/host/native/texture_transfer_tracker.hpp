#pragma once

#include "native/texture_command_dispatch.hpp"
#include "native/texture_lifetime_tracker.hpp"
#include "psprecomp/runtime.hpp"
#include "resources/source_authority.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mhp3rd::native {

struct TextureTransferTrackerConfig {
    resources::SourceCodeIdentity code{};
    bool (*current_code)(void *, const psprecomp::Runtime &,
                         const psprecomp::AllegrexContext &,
                         const resources::SourceCodeIdentity &) noexcept{};
    void *current_code_user{};
    std::uint32_t max_loads{32u};
    std::uint32_t max_queue_frames{64u};
    std::uint32_t max_descriptors{64u};
    std::uint32_t max_pending_writers{32u};
    std::uint32_t max_read_frames{32u};
    std::uint32_t max_read_attempts{128u};
    std::uint64_t read_attempt_serial_limit{0xFFFFFFFFFFFFFFFFull};
};

enum class TextureTransferTrackerError {
    None,
    InvalidConfig,
    AuthorityRejected,
    InvalidObservation,
    UnpairedSelectedLoad,
    ConflictingFrame,
    InvalidDescriptor,
    InvalidRange,
    CodeChanged,
    NoCapacity,
    CounterExhausted,
    ObservationLost,
};

struct TextureTransferTrackerStats {
    std::uint64_t callbacks{}, loads_started{}, tail_transfers{};
    std::uint64_t enqueue_entries{}, descriptor_generations{}, queued_returns{};
    std::uint64_t descriptor_ring_reuses{}, descriptor_raw_pointer_reuses{};
    std::uint64_t descriptor_alias_reuses{}, unowned_enqueues{}, pending_writers{};
    std::uint64_t read_state8_entries{}, read_helper_entries{}, read_invocations{};
    std::uint64_t read_results{}, exact_read_results{}, retry_read_results{};
    std::uint64_t rejected_read_results{}, unowned_read_requests{}, unowned_read_results{};
    std::uint64_t failures{}, losses{};
    std::size_t active_loads{}, active_queue_frames{}, active_read_frames{}, live_writers{};
    std::size_t read_attempt_records{};
};

struct TextureTransferDescriptorRecord {
    std::uint64_t generation{}, load_generation{}, queue_generation{};
    std::uint64_t owner_invalidation_generation{};
    std::uint32_t raw_descriptor{}, raw_manager{}, raw_destination{}, write_footprint{};
    resources::AuthorityToken owner{}, descriptor{}, writer{};
    std::array<std::uint8_t, 32> snapshot{};
    bool associated_load{}, queue_return_observed{}, current{true};
    std::uint32_t queue_return_value{}; // 1 means queued, never completed
};

enum class TextureReadOutcome : std::uint8_t {
    None, ExactReadObserved, RetryObserved, Rejected,
};

struct TextureReadAttemptRecord {
    std::uint64_t serial{}, descriptor_generation{}, request_generation{};
    std::uint64_t owner_invalidation_generation{};
    resources::AuthorityToken owner{}, descriptor{}, writer{};
    std::uint32_t raw_owner{}, raw_manager{}, raw_record{}, raw_scratch{}, fd{}, requested_bytes{};
    std::int32_t result{};
    TextureReadOutcome outcome{TextureReadOutcome::None};
    bool queue_return_observed{}, result_observed{};
};

// G1a observes owner-scoped load requests and committed queue descriptors.
// It resets an old owner source on a real selected load, creates descriptor
// generations, and registers bounded pending-writer spans. It deliberately
// never calls begin_load/begin_fragment or any completion API: a requested
// length and an enqueued descriptor do not establish transfer completion.
class TextureTransferTracker final {
public:
    static constexpr std::size_t kMaxLoads = 32u;
    static constexpr std::size_t kMaxQueueFrames = 64u;
    static constexpr std::size_t kMaxDescriptors = 64u;
    static constexpr std::size_t kMaxPendingWriters = 32u;
    static constexpr std::size_t kMaxReadFrames = 32u;
    static constexpr std::size_t kMaxReadAttempts = 128u;

    TextureTransferTracker(psprecomp::Runtime &runtime,
                           resources::SourceAuthority &authority,
                           TextureLifetimeTracker &lifetime,
                           TextureTransferTrackerConfig config) noexcept;

    void observe(const psprecomp::Runtime &runtime,
                 const psprecomp::AllegrexContext &context,
                 TextureTransferCheckpoint checkpoint) noexcept;
    void observe_read(const psprecomp::Runtime &runtime,
                     const psprecomp::AllegrexContext &context,
                     TextureReadCheckpoint checkpoint) noexcept;
    [[nodiscard]] TextureTransferTrackerStats stats() const noexcept { return stats_; }
    [[nodiscard]] TextureTransferTrackerError error() const noexcept { return error_; }
    [[nodiscard]] std::size_t descriptor_record_count() const noexcept { return descriptor_count_; }
    [[nodiscard]] std::size_t read_attempt_count() const noexcept { return read_attempt_count_; }
    [[nodiscard]] bool descriptor_record(std::size_t index,
        TextureTransferDescriptorRecord &out) const noexcept;
    [[nodiscard]] bool read_attempt_record(std::size_t index,
        TextureReadAttemptRecord &out) const noexcept;

private:
    enum class LoadPhase : std::uint8_t { Empty, Started, TailTransferred, Queued };
    struct LoadFrame {
        LoadPhase phase{LoadPhase::Empty};
        const psprecomp::Runtime *runtime{};
        const psprecomp::AllegrexContext *context{};
        psprecomp::RuntimeExecutionContextToken execution{};
        std::uint64_t generation{};
        std::uint32_t sp{}, return_pc{}, owner_raw{}, selector{}, resource_id{};
        resources::AuthorityToken owner{};
        std::uint32_t manager{}, destination{}, group{}, target_pc{};
        std::uint32_t descriptors{};
    };
    struct QueueFrame {
        bool active{}, tracked{};
        const psprecomp::Runtime *runtime{};
        const psprecomp::AllegrexContext *context{};
        psprecomp::RuntimeExecutionContextToken execution{};
        std::uint64_t generation{}, load_generation{};
        std::uint32_t entry_sp{}, return_pc{}, manager{}, resource_id{};
        std::uint32_t destination{}, group{}, descriptors{};
        std::array<std::size_t, kMaxDescriptors> descriptor_indices{};
        std::size_t descriptor_count{};
    };
    struct WatchedOwner {
        bool used{};
        resources::AuthorityToken owner{};
        std::uint32_t raw_owner{}, raw_slot{};
        std::uint64_t request_generation{};
    };
    struct PendingWriter {
        bool pending{};
        resources::AuthorityToken owner{}, descriptor{}, writer{};
        std::uint64_t generation{};
        std::uint32_t raw_destination{}, bytes{};
    };
    struct DescriptorSnapshot {
        std::array<std::uint8_t, 32> bytes{};
        std::uint16_t resource_id{};
        std::uint32_t destination{}, chunk_bytes{}, offset{}, total_bytes{};
        std::uint32_t cancellation_pointer{};
        std::uint8_t group{}, first_marker{}, final_marker{}, deobfuscate{};
        std::uint32_t hash_flag{};
    };
    struct ReadFrame {
        bool active{}, selected{}, has_descriptor_record{}, retry_state8_required{};
        bool helper_entered{}, invocation_observed{};
        const psprecomp::Runtime *runtime{};
        const psprecomp::AllegrexContext *context{};
        psprecomp::RuntimeExecutionContextToken execution{};
        std::size_t descriptor_index{kMaxDescriptors};
        std::size_t active_attempt{kMaxReadAttempts};
        std::uint64_t descriptor_generation{}, request_generation{};
        std::uint64_t owner_invalidation_generation{};
        resources::AuthorityToken owner{};
        std::uint32_t raw_manager{}, raw_owner{}, raw_record{};
        std::uint32_t worker_sp{}, helper_sp{}, helper_return_pc{}, fd{};
        std::uint32_t raw_scratch{}, requested_bytes{};
        std::array<std::uint8_t, 32> descriptor_snapshot{};
        std::uint32_t state8_entries{};
    };

    psprecomp::Runtime *runtime_{};
    resources::SourceAuthority *authority_{};
    TextureLifetimeTracker *lifetime_{};
    TextureTransferTrackerConfig config_{};
    TextureTransferTrackerError error_{TextureTransferTrackerError::None};
    TextureTransferTrackerStats stats_{};
    std::uint64_t next_generation_{1u};
    std::array<LoadFrame, kMaxLoads> loads_{};
    std::array<QueueFrame, kMaxQueueFrames> queues_{};
    std::array<WatchedOwner, TextureLifetimeTracker::kMaxLeases> owners_{};
    std::array<PendingWriter, kMaxPendingWriters> writers_{};
    std::array<TextureTransferDescriptorRecord, kMaxDescriptors> descriptors_{};
    std::size_t descriptor_count_{};
    std::array<ReadFrame, kMaxReadFrames> read_frames_{};
    std::array<TextureReadAttemptRecord, kMaxReadAttempts> read_attempts_{};
    std::size_t read_attempt_count_{};
    std::uint64_t next_read_attempt_{1u};

    [[nodiscard]] bool code_ok(const psprecomp::Runtime &runtime,
                               const psprecomp::AllegrexContext &context) noexcept;
    [[nodiscard]] bool count(std::uint64_t &counter) noexcept;
    void fail(TextureTransferTrackerError error, bool loss) noexcept;
    void observe_impl(const psprecomp::Runtime &runtime,
                      const psprecomp::AllegrexContext &context,
                      TextureTransferCheckpoint checkpoint);
    void observe_read_impl(const psprecomp::Runtime &runtime,
                           const psprecomp::AllegrexContext &context,
                           TextureReadCheckpoint checkpoint);
    void observe_load_entry(const psprecomp::Runtime &runtime,
                            const psprecomp::AllegrexContext &context);
    void observe_load_tail(const psprecomp::Runtime &runtime,
                          const psprecomp::AllegrexContext &context);
    void observe_enqueue_entry(const psprecomp::Runtime &runtime,
                               const psprecomp::AllegrexContext &context);
    void observe_descriptor_commit(const psprecomp::Runtime &runtime,
                                  const psprecomp::AllegrexContext &context);
    void observe_enqueue_return(const psprecomp::Runtime &runtime,
                                const psprecomp::AllegrexContext &context);
    void observe_read_state8(const psprecomp::Runtime &runtime,
                             const psprecomp::AllegrexContext &context);
    void observe_read_helper_entry(const psprecomp::Runtime &runtime,
                                   const psprecomp::AllegrexContext &context);
    void observe_read_invoke(const psprecomp::Runtime &runtime,
                             const psprecomp::AllegrexContext &context);
    void observe_read_result(const psprecomp::Runtime &runtime,
                            const psprecomp::AllegrexContext &context);

    [[nodiscard]] LoadFrame *find_load(const psprecomp::Runtime &runtime,
                                       const psprecomp::AllegrexContext &context,
                                       std::uint32_t sp, std::uint32_t return_pc) noexcept;
    [[nodiscard]] QueueFrame *find_queue(const psprecomp::Runtime &runtime,
                                         const psprecomp::AllegrexContext &context,
                                         std::uint32_t sp, std::uint32_t return_pc) noexcept;
    [[nodiscard]] LoadFrame *new_load() noexcept;
    [[nodiscard]] QueueFrame *new_queue() noexcept;
    [[nodiscard]] ReadFrame *new_read_frame() noexcept;
    [[nodiscard]] ReadFrame *find_read_frame(const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context, std::uint32_t sp,
        std::uint32_t raw_record) noexcept;
    [[nodiscard]] ReadFrame *find_active_read_attempt(
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context) noexcept;
    [[nodiscard]] bool validate_read_frame(ReadFrame &frame,
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context,
        std::uint32_t raw_manager, std::uint32_t raw_record) noexcept;
    [[nodiscard]] WatchedOwner *watch_owner(std::uint32_t raw_owner,
                                            resources::AuthorityToken owner) noexcept;
    [[nodiscard]] bool read_descriptor(const psprecomp::Runtime &runtime,
                                       std::uint32_t raw,
                                       DescriptorSnapshot &snapshot) const noexcept;
    [[nodiscard]] bool descriptor_valid_for(const DescriptorSnapshot &descriptor,
                                            const LoadFrame &load) const noexcept;
    [[nodiscard]] bool intersects_watched_slot(std::uint32_t raw,
                                               std::uint32_t bytes) const noexcept;
    [[nodiscard]] bool descriptor_overlaps_history(std::uint32_t raw) const noexcept;
    [[nodiscard]] bool register_pending_writer(resources::AuthorityToken owner,
        resources::AuthorityToken descriptor, std::uint64_t generation,
        std::uint32_t raw_destination, std::uint32_t bytes) noexcept;
};

} // namespace mhp3rd::native
