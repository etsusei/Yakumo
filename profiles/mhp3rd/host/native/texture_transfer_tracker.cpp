#include "native/texture_transfer_tracker.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace mhp3rd::native {
namespace {

constexpr std::uint32_t kSelectedIndex = 7u;
constexpr std::uint32_t kSelectedManagerEnqueue = 0x08863CDCu;
constexpr std::uint32_t kSlotGroupOffset = 0x63u;
constexpr std::uint32_t kQueueConsumerOffset = 0x108Cu;
constexpr std::uint32_t kQueueStateOffset = 0x1094u;
constexpr std::uint32_t kQueueFdOffset = 0x1098u;
constexpr std::uint32_t kQueueRecordOffset = 0x8Cu;
constexpr std::uint32_t kTransformEventOffset = 0x2F7C8u;
constexpr std::uint32_t kReadScratchOffset = 0x98C0u;
constexpr std::uint32_t kReadScratchBytes = 0x20000u;
constexpr std::uint32_t kReadQueueRecords = 128u;
constexpr std::uint32_t kReadRecordStride = 32u;
constexpr std::uint32_t kWorkerState8 = 8u;
// The queue's ring stride begins 12 bytes before each descriptor. At the
// committed checkpoint, a2 (gpr[6]) points at the descriptor start because
// the original ring-pointer arithmetic has already added those 12 bytes.

bool same_execution(psprecomp::RuntimeExecutionContextToken token) noexcept {
    return psprecomp::runtime_execution_context_matches(token);
}

bool add_u32(std::uint32_t base, std::uint32_t offset,
             std::uint32_t &result) noexcept {
    const auto sum = std::uint64_t{base} + offset;
    if (sum > std::numeric_limits<std::uint32_t>::max()) return false;
    result = static_cast<std::uint32_t>(sum);
    return true;
}

bool valid_ram(const psprecomp::Runtime &runtime, std::uint32_t raw,
               std::uint32_t bytes) noexcept {
    if (bytes == 0u || std::uint64_t{raw} + bytes >
                           std::numeric_limits<std::uint32_t>::max()) return false;
    const auto physical = std::uint64_t{psprecomp::GuestMemory::canonical(raw)};
    const auto base = std::uint64_t{psprecomp::GuestMemory::kPhysicalBase};
    return physical >= base && physical + bytes <= base + runtime.memory().size() &&
           runtime.memory().contains(raw, bytes);
}

std::uint16_t read16(const std::uint8_t *p) noexcept {
    return static_cast<std::uint16_t>(std::uint16_t{p[0]} |
        static_cast<std::uint16_t>(std::uint16_t{p[1]} << 8u));
}

std::uint32_t read32(const std::uint8_t *p) noexcept {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8u) |
           (std::uint32_t{p[2]} << 16u) | (std::uint32_t{p[3]} << 24u);
}

bool canonical_equal(std::uint32_t lhs, std::uint32_t rhs) noexcept {
    return psprecomp::GuestMemory::canonical(lhs) ==
           psprecomp::GuestMemory::canonical(rhs);
}

bool same_call_arguments(const CompletionCallArguments &left,
                         const CompletionCallArguments &right) noexcept {
    return left.checkpoint == right.checkpoint &&
           left.site_pc == right.site_pc && left.callee_pc == right.callee_pc &&
           left.return_pc == right.return_pc && left.sp == right.sp &&
           left.ra == right.ra && left.a0 == right.a0 && left.a1 == right.a1 &&
           left.a2 == right.a2 && left.a3 == right.a3 &&
           left.manager == right.manager && left.descriptor == right.descriptor &&
           left.destination == right.destination && left.source == right.source &&
           left.bytes == right.bytes && left.logical_length == right.logical_length &&
           left.footprint == right.footprint &&
           left.event == right.event && left.has_manager == right.has_manager &&
           left.has_descriptor == right.has_descriptor &&
           left.has_destination == right.has_destination &&
           left.has_source == right.has_source && left.has_bytes == right.has_bytes &&
           left.has_logical_length == right.has_logical_length &&
           left.has_footprint == right.has_footprint &&
           left.has_event == right.has_event &&
           left.after_delay_slot == right.after_delay_slot;
}

} // namespace

TextureTransferTracker::TextureTransferTracker(
    psprecomp::Runtime &runtime, resources::SourceAuthority &authority,
    TextureLifetimeTracker &lifetime, TextureTransferTrackerConfig config) noexcept
    : runtime_(&runtime), authority_(&authority), lifetime_(&lifetime), config_(config) {
    if (config_.current_code == nullptr || config_.max_loads == 0u ||
        config_.max_loads > kMaxLoads || config_.max_queue_frames == 0u ||
        config_.max_queue_frames > kMaxQueueFrames || config_.max_descriptors == 0u ||
        config_.max_descriptors > kMaxDescriptors || config_.max_pending_writers == 0u ||
        config_.max_pending_writers > kMaxPendingWriters ||
        config_.max_read_frames == 0u || config_.max_read_frames > kMaxReadFrames ||
        config_.max_read_attempts == 0u || config_.max_read_attempts > kMaxReadAttempts ||
        config_.read_attempt_serial_limit == 0u ||
        authority.failure() != resources::AuthorityError::None ||
        lifetime.error() != TextureLifetimeTrackerError::None)
        fail(TextureTransferTrackerError::InvalidConfig, true);
}

bool TextureTransferTracker::count(std::uint64_t &counter) noexcept {
    if (counter == std::numeric_limits<std::uint64_t>::max()) {
        fail(TextureTransferTrackerError::CounterExhausted, true);
        return false;
    }
    ++counter;
    return true;
}

void TextureTransferTracker::fail(TextureTransferTrackerError error,
                                  bool loss) noexcept {
    if (error_ != TextureTransferTrackerError::None) return;
    error_ = error;
    if (stats_.failures != std::numeric_limits<std::uint64_t>::max()) ++stats_.failures;
    if (loss) {
        if (stats_.losses != std::numeric_limits<std::uint64_t>::max()) ++stats_.losses;
        try { (void)authority_->observer_lost(); } catch (...) {}
    }
    for (auto &load : loads_) load = LoadFrame{};
    for (auto &queue : queues_) queue = QueueFrame{};
    for (auto &read : read_frames_) read = ReadFrame{};
    for (auto &completion : completion_frames_) completion = CompletionFrame{};
    for (auto &receipt : completion_call_receipts_) receipt = CompletionCallReceipt{};
    completion_decoder_.reset();
    stats_.active_loads = 0u;
    stats_.active_queue_frames = 0u;
    stats_.active_read_frames = 0u;
    stats_.active_completions = 0u;
    completion_call_receipt_count_ = 0u;
    // Keep the Authority's external-writer records: observation loss must not
    // silently retire an asynchronous writer hazard.
}

bool TextureTransferTracker::code_ok(const psprecomp::Runtime &runtime,
                                     const psprecomp::AllegrexContext &context) noexcept {
    if (error_ != TextureTransferTrackerError::None) return false;
    if (authority_->failure() != resources::AuthorityError::None ||
        lifetime_->error() != TextureLifetimeTrackerError::None) {
        fail(TextureTransferTrackerError::AuthorityRejected, true);
        return false;
    }
    if (!config_.current_code(config_.current_code_user, runtime, context, config_.code)) {
        fail(TextureTransferTrackerError::CodeChanged, true);
        return false;
    }
    return true;
}

TextureTransferTracker::LoadFrame *TextureTransferTracker::find_load(
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
    std::uint32_t sp, std::uint32_t return_pc) noexcept {
    LoadFrame *found = nullptr;
    for (auto &load : loads_) {
        if (load.phase == LoadPhase::Empty || load.runtime != &runtime ||
            load.context != &context || load.sp != sp || load.return_pc != return_pc ||
            !same_execution(load.execution)) continue;
        if (found != nullptr) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return nullptr;
        }
        found = &load;
    }
    return found;
}

TextureTransferTracker::QueueFrame *TextureTransferTracker::find_queue(
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
    std::uint32_t sp, std::uint32_t return_pc) noexcept {
    QueueFrame *found = nullptr;
    for (auto &queue : queues_) {
        if (!queue.active || queue.runtime != &runtime || queue.context != &context ||
            queue.entry_sp != sp || queue.return_pc != return_pc ||
            !same_execution(queue.execution)) continue;
        if (found != nullptr) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return nullptr;
        }
        found = &queue;
    }
    return found;
}

TextureTransferTracker::LoadFrame *TextureTransferTracker::new_load() noexcept {
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(TextureTransferTrackerError::CounterExhausted, true);
        return nullptr;
    }
    for (std::size_t i = 0; i < config_.max_loads; ++i) {
        auto &load = loads_[i];
        if (load.phase != LoadPhase::Empty) continue;
        load = LoadFrame{};
        load.generation = next_generation_++;
        ++stats_.active_loads;
        return &load;
    }
    fail(TextureTransferTrackerError::NoCapacity, true);
    return nullptr;
}

TextureTransferTracker::QueueFrame *TextureTransferTracker::new_queue() noexcept {
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(TextureTransferTrackerError::CounterExhausted, true);
        return nullptr;
    }
    for (std::size_t i = 0; i < config_.max_queue_frames; ++i) {
        auto &queue = queues_[i];
        if (queue.active) continue;
        queue = QueueFrame{};
        queue.generation = next_generation_++;
        ++stats_.active_queue_frames;
        return &queue;
    }
    fail(TextureTransferTrackerError::NoCapacity, true);
    return nullptr;
}

TextureTransferTracker::ReadFrame *TextureTransferTracker::new_read_frame() noexcept {
    for (std::size_t i = 0; i < config_.max_read_frames; ++i) {
        auto &frame = read_frames_[i];
        if (frame.active) continue;
        frame = ReadFrame{};
        frame.active = true;
        ++stats_.active_read_frames;
        return &frame;
    }
    fail(TextureTransferTrackerError::NoCapacity, true);
    return nullptr;
}

TextureTransferTracker::ReadFrame *TextureTransferTracker::find_read_frame(
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
    std::uint32_t sp, std::uint32_t raw_record) noexcept {
    ReadFrame *found = nullptr;
    const auto physical_record = psprecomp::GuestMemory::canonical(raw_record);
    for (auto &frame : read_frames_) {
        if (!frame.active || frame.runtime != &runtime || frame.context != &context ||
            frame.worker_sp != sp ||
            psprecomp::GuestMemory::canonical(frame.raw_record) != physical_record ||
            !same_execution(frame.execution)) continue;
        if (found != nullptr) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return nullptr;
        }
        found = &frame;
    }
    return found;
}

TextureTransferTracker::ReadFrame *TextureTransferTracker::find_active_read_attempt(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) noexcept {
    ReadFrame *found = nullptr;
    for (auto &frame : read_frames_) {
        if (!frame.active || frame.runtime != &runtime || frame.context != &context ||
            frame.active_attempt == kMaxReadAttempts ||
            !same_execution(frame.execution)) continue;
        if (found != nullptr) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return nullptr;
        }
        found = &frame;
    }
    return found;
}

TextureTransferTracker::WatchedOwner *TextureTransferTracker::watch_owner(
    std::uint32_t raw_owner, resources::AuthorityToken owner) noexcept {
    for (auto &entry : owners_)
        if (entry.used && entry.owner == owner) return &entry;
    for (auto &entry : owners_) {
        if (entry.used) continue;
        std::uint32_t slot{};
        if (!add_u32(raw_owner, resources::SourceAuthority::kSlotOffset, slot) ||
            !valid_ram(*runtime_, slot, resources::SourceAuthority::kSlotBytes)) return nullptr;
        entry = {true, owner, raw_owner, slot};
        return &entry;
    }
    return nullptr;
}

bool TextureTransferTracker::read_descriptor(const psprecomp::Runtime &runtime,
    std::uint32_t raw, DescriptorSnapshot &snapshot) const noexcept {
    if (!valid_ram(runtime, raw, 32u)) return false;
    const auto *p = runtime.memory().raw_pointer(raw, 32u);
    if (p == nullptr) return false;
    std::copy_n(p, snapshot.bytes.size(), snapshot.bytes.begin());
    snapshot.resource_id = read16(p + 2u);
    snapshot.destination = read32(p + 4u);
    snapshot.chunk_bytes = read32(p + 8u);
    snapshot.offset = read32(p + 12u);
    snapshot.total_bytes = read32(p + 16u);
    snapshot.cancellation_pointer = read32(p + 20u);
    snapshot.group = p[24u];
    snapshot.first_marker = p[25u];
    snapshot.final_marker = p[26u];
    snapshot.deobfuscate = p[27u];
    snapshot.hash_flag = read32(p + 28u);
    return true;
}

bool TextureTransferTracker::descriptor_valid_for(const DescriptorSnapshot &d,
    const LoadFrame &load) const noexcept {
    if (read16(d.bytes.data()) != 1u || d.resource_id != load.resource_id ||
        d.group != load.group || d.first_marker > 1u || d.final_marker > 1u ||
        d.deobfuscate > 1u || d.hash_flag > 1u || d.total_bytes == 0u ||
        d.total_bytes > resources::SourceAuthority::kSlotBytes || d.chunk_bytes == 0u ||
        std::uint64_t{d.offset} + d.chunk_bytes > d.total_bytes) return false;
    std::uint32_t expected_slot{}, write_start{};
    if (!add_u32(load.owner_raw, resources::SourceAuthority::kSlotOffset, expected_slot) ||
        psprecomp::GuestMemory::canonical(expected_slot) !=
            psprecomp::GuestMemory::canonical(load.destination) ||
        psprecomp::GuestMemory::canonical(d.destination) !=
            psprecomp::GuestMemory::canonical(load.destination) ||
        !add_u32(d.destination, d.offset, write_start)) return false;
    const auto footprint = d.deobfuscate != 0u
        ? ((std::uint64_t{d.chunk_bytes} + 3u) & ~std::uint64_t{3u})
        : std::uint64_t{d.chunk_bytes};
    return std::uint64_t{d.offset} + footprint <= resources::SourceAuthority::kSlotBytes &&
           footprint <= std::numeric_limits<std::uint32_t>::max() &&
           valid_ram(*runtime_, write_start, static_cast<std::uint32_t>(footprint));
}

bool TextureTransferTracker::intersects_watched_slot(std::uint32_t raw,
    std::uint32_t bytes) const noexcept {
    if (!valid_ram(*runtime_, raw, bytes)) return false;
    const auto start = std::uint64_t{psprecomp::GuestMemory::canonical(raw)};
    const auto end = start + bytes;
    for (const auto &owner : owners_) {
        if (!owner.used) continue;
        const auto slot = std::uint64_t{psprecomp::GuestMemory::canonical(owner.raw_slot)};
        const auto slot_end = slot + resources::SourceAuthority::kSlotBytes;
        if (start < slot_end && slot < end) return true;
    }
    return false;
}

bool TextureTransferTracker::descriptor_overlaps_history(std::uint32_t raw) const noexcept {
    const auto start = std::uint64_t{psprecomp::GuestMemory::canonical(raw)};
    const auto end = start + 32u;
    for (std::size_t i = 0; i < descriptor_count_; ++i) {
        const auto old_start = std::uint64_t{
            psprecomp::GuestMemory::canonical(descriptors_[i].raw_descriptor)};
        const auto old_end = old_start + 32u;
        if (start < old_end && old_start < end) return true;
    }
    return false;
}

bool TextureTransferTracker::register_pending_writer(resources::AuthorityToken owner,
    resources::AuthorityToken descriptor, std::uint64_t generation,
    std::uint32_t raw_destination, std::uint32_t bytes) noexcept {
    PendingWriter *slot = nullptr;
    for (std::size_t i = 0; i < config_.max_pending_writers; ++i)
        if (!writers_[i].pending) { slot = &writers_[i]; break; }
    if (slot == nullptr) {
        fail(TextureTransferTrackerError::NoCapacity, true);
        return false;
    }
    const auto issued = authority_->begin_external_write(raw_destination, bytes);
    if (!issued.event.ok() || issued.token.instance == 0u || issued.token.serial == 0u) {
        fail(TextureTransferTrackerError::AuthorityRejected, true);
        return false;
    }
    *slot = {true, owner, descriptor, issued.token, generation, raw_destination, bytes};
    ++stats_.live_writers;
    return count(stats_.pending_writers);
}

bool TextureTransferTracker::descriptor_record(std::size_t index,
    TextureTransferDescriptorRecord &out) const noexcept {
    if (index >= descriptor_count_) return false;
    out = descriptors_[index];
    return out.generation != 0u;
}

bool TextureTransferTracker::read_attempt_record(std::size_t index,
    TextureReadAttemptRecord &out) const noexcept {
    if (index >= read_attempt_count_) return false;
    out = read_attempts_[index];
    return out.serial != 0u;
}

bool TextureTransferTracker::completion_record(std::size_t index,
    TextureCompletionRecord &out) const noexcept {
    if (index >= completion_record_count_) return false;
    out = completion_records_[index];
    return out.operation_serial != 0u;
}

bool TextureTransferTracker::completion_site_matches(
    TextureCompletionCheckpoint checkpoint, std::uint32_t site_pc) const noexcept {
    const auto *info = texture_completion_checkpoint_info(checkpoint);
    return info != nullptr && info->site_pc == site_pc;
}

namespace {

bool completion_worker_checkpoint(TextureCompletionCheckpoint checkpoint) noexcept {
    return checkpoint == TextureCompletionCheckpoint::WorkerEntry ||
        checkpoint == TextureCompletionCheckpoint::VerbatimBranch ||
        checkpoint == TextureCompletionCheckpoint::DigestSkippedBranch ||
        checkpoint == TextureCompletionCheckpoint::TransformCall ||
        checkpoint == TextureCompletionCheckpoint::TransformReturn ||
        checkpoint == TextureCompletionCheckpoint::DigestCall ||
        checkpoint == TextureCompletionCheckpoint::DigestReturn ||
        checkpoint == TextureCompletionCheckpoint::WorkerAckReturn ||
        checkpoint == TextureCompletionCheckpoint::UnsupportedCopyRoute ||
        checkpoint == TextureCompletionCheckpoint::WorkerCopyReturn;
}

bool completion_reader_checkpoint(TextureCompletionCheckpoint checkpoint) noexcept {
    return !completion_worker_checkpoint(checkpoint) &&
        checkpoint != TextureCompletionCheckpoint::GroupCancellation &&
        checkpoint != TextureCompletionCheckpoint::FullQueueCancellation;
}

} // namespace

TextureTransferTracker::CompletionFrame *TextureTransferTracker::find_completion(
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint) noexcept {
    CompletionFrame *found = nullptr;
    for (auto &frame : completion_frames_) {
        if (!frame.active || frame.runtime != &runtime) continue;
        if (completion_worker_checkpoint(checkpoint)) {
            if (frame.worker_context != nullptr && frame.worker_context != &context) continue;
            if (checkpoint != TextureCompletionCheckpoint::WorkerEntry &&
                frame.worker_context == nullptr) continue;
            if (!completion_worker_identity_valid(frame, runtime, context)) continue;
        } else if (completion_reader_checkpoint(checkpoint) &&
                   frame.reader_context != &context) {
            const bool wait_alias = checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn &&
                frame.reader_context != nullptr &&
                frame.reader_context->gpr[29] == context.gpr[29] &&
                psprecomp::runtime_thread_uid() == frame.reader_execution.thread_uid;
            if (!wait_alias) continue;
        }
        if (found != nullptr) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return nullptr;
        }
        found = &frame;
    }
    return found;
}

bool TextureTransferTracker::completion_worker_identity_valid(
    const CompletionFrame &frame, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) const noexcept {
    if (frame.runtime != &runtime || frame.origin.descriptor_index >= descriptor_count_)
        return false;
    if (frame.worker_context == nullptr) {
        // The audited worker entry receives a pointer to the current manager
        // object in a1 (gpr5).  The first word is the manager identity.  A
        // copied reader context does not satisfy this production shape.
        if (!valid_ram(runtime, context.gpr[5], 4u)) return false;
        const auto *p = runtime.memory().raw_pointer(context.gpr[5], 4u);
        return p != nullptr && canonical_equal(read32(p), frame.raw_manager);
    }
    // The worker's saved registers are repurposed while it processes the
    // descriptor. Once WorkerEntry has bound this stable execution context to
    // the operation, later branch/copy/ack callbacks use that binding plus
    // the resampled descriptor snapshot rather than treating arbitrary
    // worker registers as a second manager/descriptor claim.
    return frame.worker_context == &context;
}

bool TextureTransferTracker::completion_context_valid(
    const CompletionFrame &frame, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint) const noexcept {
    if (frame.runtime != &runtime) return false;
    if (completion_worker_checkpoint(checkpoint)) {
        if (frame.worker_context != &context ||
            !same_execution(frame.worker_execution)) return false;
        if (!completion_worker_identity_valid(frame, runtime, context)) return false;
    } else if (completion_reader_checkpoint(checkpoint)) {
        if (frame.reader_context != &context) {
            const bool wait_alias = checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn &&
                frame.reader_context != nullptr &&
                frame.reader_context->gpr[29] == context.gpr[29] &&
                psprecomp::runtime_thread_uid() == frame.reader_execution.thread_uid;
            if (!wait_alias) return false;
        }
        // The main thread may resume under a new runtime execution token while
        // the worker owns the guest. WorkerWaitReturn is the boundary that
        // revalidates and refreshes that token; every later reader edge uses it.
        if (checkpoint != TextureCompletionCheckpoint::WorkerWaitReturn &&
            !same_execution(frame.reader_execution)) return false;
    }
    return true;
}

bool TextureTransferTracker::completion_attempt_valid(
    const CompletionFrame &frame) const noexcept {
    if (frame.result_index >= read_attempt_count_ ||
        frame.completion_record_index >= completion_record_count_) return false;
    const auto &attempt = read_attempts_[frame.result_index];
    const auto &record = completion_records_[frame.completion_record_index];
    if (attempt.serial == 0u || attempt.serial != frame.operation_serial ||
        !attempt.result_observed || attempt.outcome != TextureReadOutcome::ExactReadObserved ||
        attempt.result <= 0 || attempt.descriptor_generation != frame.origin.descriptor_generation ||
        attempt.request_generation != frame.origin.request_generation ||
        attempt.owner_invalidation_generation != frame.origin.owner_invalidation_generation ||
        attempt.owner != frame.origin.owner || attempt.descriptor != record.descriptor ||
        attempt.writer != record.writer || record.operation_serial != frame.operation_serial ||
        record.descriptor_generation != frame.origin.descriptor_generation ||
        record.request_generation != frame.origin.request_generation) return false;
    return true;
}

bool TextureTransferTracker::completion_identity_valid(
    const CompletionFrame &frame, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint,
    const CompletionSample &sample) noexcept {
    if (frame.origin.descriptor_index >= descriptor_count_)
        return false;
    if (!completion_context_valid(frame, runtime, context, checkpoint)) return false;
    if (!completion_attempt_valid(frame)) return false;
    const auto *info = texture_completion_checkpoint_info(checkpoint);
    if (info == nullptr || sample.status == CompletionDecodeStatus::Invalid)
        return false;
    const auto syscall_call = sample.has_call &&
        (sample.call_checkpoint == TextureCompletionCallCheckpoint::WorkerRequestCall ||
         sample.call_checkpoint == TextureCompletionCallCheckpoint::WorkerEventSetCall ||
         sample.call_checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall ||
         sample.call_checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall);
    if (info->requires_call_receipt) {
        if (!sample.has_call || sample.call_checkpoint != info->call_checkpoint)
            return false;
        if (!syscall_call &&
            (!sample.call.has_manager || !sample.call.has_descriptor ||
             !canonical_equal(sample.call.manager, frame.raw_manager) ||
             !canonical_equal(sample.call.descriptor, frame.raw_record))) return false;
        if (checkpoint == TextureCompletionCheckpoint::WorkerRequestReturn ||
            checkpoint == TextureCompletionCheckpoint::WorkerEventSetReturn ||
            checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn ||
            checkpoint == TextureCompletionCheckpoint::WorkerAckReturn) {
            if (!sample.call.has_event || sample.call.event != frame.event_uid)
                return false;
        }
    } else if (sample.has_manager &&
               !canonical_equal(sample.manager, frame.raw_manager)) return false;
    if (sample.has_descriptor && !syscall_call &&
        !canonical_equal(sample.descriptor, frame.raw_record)) return false;
    if (sample.has_call && !syscall_call) {
        const auto call_checkpoint = sample.call_checkpoint;
        if (call_checkpoint == TextureCompletionCallCheckpoint::InlineCopyCall ||
            call_checkpoint == TextureCompletionCallCheckpoint::WorkerCopyCall) {
            if (!sample.call.has_destination || !sample.call.has_source ||
                !sample.call.has_logical_length || !sample.call.has_footprint ||
                !canonical_equal(sample.call.destination, frame.raw_destination) ||
                !canonical_equal(sample.call.source, frame.origin.raw_scratch) ||
                sample.call.logical_length != frame.logical_bytes ||
                sample.call.footprint != frame.logical_bytes) return false;
        } else if (call_checkpoint == TextureCompletionCallCheckpoint::TransformCall) {
            if (!sample.call.has_destination || !sample.call.has_logical_length ||
                !sample.call.has_footprint ||
                !canonical_equal(sample.call.destination, frame.raw_destination) ||
                sample.call.logical_length != frame.logical_bytes ||
                sample.call.footprint != frame.write_footprint) return false;
        } else if (call_checkpoint == TextureCompletionCallCheckpoint::DigestCall) {
            if (!sample.call.has_destination || !sample.call.has_logical_length ||
                !sample.call.has_footprint ||
                !canonical_equal(sample.call.destination, frame.raw_destination) ||
                sample.call.logical_length != frame.logical_bytes ||
                sample.call.footprint != frame.logical_bytes) return false;
        }
    }
    if (checkpoint == TextureCompletionCheckpoint::WorkerRequestCall) {
        std::uint32_t address{};
        if (!add_u32(frame.raw_manager, kTransformEventOffset, address) ||
            !valid_ram(runtime, address, 4u)) return false;
        const auto *event = runtime.memory().raw_pointer(address, 4u);
        if (event == nullptr || read32(event) != frame.event_uid) return false;
    }
    const auto &descriptor = descriptors_[frame.origin.descriptor_index];
    if (!descriptor.current || descriptor.generation != frame.origin.descriptor_generation ||
        descriptor.load_generation != frame.origin.request_generation ||
        descriptor.snapshot != frame.origin.descriptor_snapshot ||
        descriptor.raw_destination != frame.raw_destination ||
        descriptor.write_footprint != frame.write_footprint) {
        return false;
    }
    DescriptorSnapshot current;
    if (!read_descriptor(runtime, descriptor.raw_descriptor, current)) return false;
    if (checkpoint == TextureCompletionCheckpoint::RetirementReturn) {
        if (!frame.retirement_snapshot_valid || current.bytes[0] != 0u ||
            current.bytes[1] != 0u ||
            !std::equal(current.bytes.begin() + 2u, current.bytes.end(),
                        frame.origin.descriptor_snapshot.begin() + 2u)) return false;
    } else if (current.bytes != frame.origin.descriptor_snapshot) {
        return false;
    }
    const auto expected_writer = frame.completion_record_index < completion_record_count_
        ? completion_records_[frame.completion_record_index].writer
        : resources::AuthorityToken{};
    if (frame.origin.owner.instance == 0u || descriptor.owner != frame.origin.owner ||
        descriptor.writer != expected_writer || descriptor.writer.instance == 0u) {
        return false;
    }
    const auto owner = lifetime_->owner_token(frame.origin.raw_owner);
    const auto invalidation = lifetime_->owner_invalidation_generation(frame.origin.raw_owner);
    const auto watched = std::find_if(owners_.begin(), owners_.end(),
        [&frame](const WatchedOwner &candidate) {
            return candidate.used && candidate.owner == frame.origin.owner;
        });
    if (!owner || *owner != frame.origin.owner || !invalidation ||
        *invalidation != frame.origin.owner_invalidation_generation ||
        watched == owners_.end() ||
        watched->request_generation != frame.origin.request_generation) {
        return false;
    }
    bool writer_pending = false;
    for (const auto &writer : writers_) {
        if (writer.pending && writer.writer == descriptor.writer &&
            writer.generation == descriptor.generation &&
            writer.raw_destination == descriptor.raw_destination &&
            writer.bytes == descriptor.write_footprint) {
            writer_pending = true;
            break;
        }
    }
    if (!writer_pending) return false;
    return true;
}

bool TextureTransferTracker::begin_completion(const ReadFrame &frame,
    const TextureReadAttemptRecord &attempt) noexcept {
    if (!config_.observe_completion) return true;
    if (!frame.selected || frame.descriptor_index >= descriptor_count_ ||
        completion_record_count_ >= completion_records_.size()) {
        fail(TextureTransferTrackerError::NoCapacity, true);
        return false;
    }
    const auto &descriptor = descriptors_[frame.descriptor_index];
    if (!descriptor.current || !descriptor.associated_load ||
        descriptor.writer.instance == 0u || !attempt.result_observed ||
        attempt.outcome != TextureReadOutcome::ExactReadObserved ||
        attempt.serial == 0u || attempt.descriptor_generation != descriptor.generation ||
        attempt.request_generation != descriptor.load_generation ||
        attempt.descriptor != descriptor.descriptor || attempt.writer != descriptor.writer ||
        attempt.requested_bytes != read32(descriptor.snapshot.data() + 8u)) {
        fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return false;
    }
    DescriptorSnapshot current;
    if (frame.raw_manager == 0u || frame.raw_record == 0u ||
        !read_descriptor(*runtime_, frame.raw_record, current) ||
        current.bytes != descriptor.snapshot) {
        fail(TextureTransferTrackerError::InvalidDescriptor, true);
        return false;
    }
    CompletionFrame *slot = nullptr;
    for (auto &candidate : completion_frames_) {
        if (!candidate.active) { slot = &candidate; break; }
    }
    if (slot == nullptr) {
        fail(TextureTransferTrackerError::NoCapacity, true);
        return false;
    }
    slot->active = true;
    slot->origin = frame;
    slot->runtime = runtime_;
    slot->reader_context = frame.context;
    slot->reader_execution = frame.execution;
    slot->raw_manager = frame.raw_manager;
    slot->raw_record = frame.raw_record;
    slot->fd = frame.fd;
    slot->worker_sp = frame.worker_sp;
    slot->worker_return_pc = frame.helper_return_pc;
    slot->event_uid = 0u;
    slot->operation_serial = attempt.serial;
    slot->result_index = static_cast<std::size_t>(&attempt - read_attempts_.data());
    slot->completion_record_index = completion_record_count_;
    slot->raw_destination = descriptor.raw_destination;
    slot->logical_bytes = attempt.requested_bytes;
    slot->write_footprint = descriptor.write_footprint;
    slot->progress = CompletionProgress(
        descriptor.snapshot[27u] != 0u, descriptor.snapshot[28u] != 0u);
    auto &record = completion_records_[completion_record_count_++];
    record = TextureCompletionRecord{};
    record.operation_serial = attempt.serial;
    record.descriptor_generation = attempt.descriptor_generation;
    record.request_generation = attempt.request_generation;
    record.owner = attempt.owner;
    record.descriptor = attempt.descriptor;
    record.writer = attempt.writer;
    record.raw_destination = descriptor.raw_destination;
    record.logical_bytes = attempt.requested_bytes;
    record.write_footprint = descriptor.write_footprint;
    ++stats_.completion_started;
    ++stats_.active_completions;
    stats_.completion_records = completion_record_count_;
    return true;
}

bool TextureTransferTracker::retire_writer(const CompletionFrame &frame) noexcept {
    if (frame.origin.descriptor_index >= descriptor_count_) return false;
    const auto &descriptor = descriptors_[frame.origin.descriptor_index];
    const auto expected_writer = frame.completion_record_index < completion_record_count_
        ? completion_records_[frame.completion_record_index].writer
        : resources::AuthorityToken{};
    if (!descriptor.current || descriptor.generation != frame.origin.descriptor_generation ||
        descriptor.owner != frame.origin.owner || descriptor.writer != expected_writer)
        return false;
    const auto owner = lifetime_->owner_token(frame.origin.raw_owner);
    const auto invalidation = lifetime_->owner_invalidation_generation(frame.origin.raw_owner);
    if (!owner || *owner != frame.origin.owner || !invalidation ||
        *invalidation != frame.origin.owner_invalidation_generation)
        return false;
    for (auto &writer : writers_) {
        if (!writer.pending || writer.writer != descriptor.writer ||
            writer.generation != descriptor.generation ||
            writer.raw_destination != descriptor.raw_destination ||
            writer.bytes != descriptor.write_footprint) continue;
        const auto event = authority_->prove_external_quiescent(writer.writer);
        if (!event.ok()) return false;
        writer.pending = false;
        if (stats_.live_writers != 0u) --stats_.live_writers;
        if (frame.completion_record_index < completion_record_count_)
            completion_records_[frame.completion_record_index].writer_released = true;
        return true;
    }
    return false;
}

bool TextureTransferTracker::finish_completion(CompletionFrame &frame,
    TextureCompletionOutcome outcome) noexcept {
    if (frame.completion_record_index >= completion_record_count_) return false;
    // A terminal observation must leave a bounded identity record behind for
    // the original worker/reader continuation.  If that record cannot be
    // retained, keep the frame and its writer live and report the loss to the
    // caller instead of closing an operation that late callbacks cannot
    // correlate safely.
    if (closed_completion_count_ >= closed_completions_.size()) return false;
    auto &record = completion_records_[frame.completion_record_index];
    if (outcome == TextureCompletionOutcome::Completed) {
        if (!retire_writer(frame)) return false;
        ++stats_.completion_completed;
    } else if (outcome == TextureCompletionOutcome::Unsupported) {
        ++stats_.completion_unsupported;
    } else if (outcome == TextureCompletionOutcome::Aborted) {
        ++stats_.completion_aborted;
    } else if (outcome == TextureCompletionOutcome::Cancelled) {
        ++stats_.completion_cancelled;
    } else if (outcome == TextureCompletionOutcome::Invalid) {
        ++stats_.completion_invalid;
    }
    record.outcome = outcome;
    record.digest_computed = frame.digest_computed;
    if (closed_completion_count_ < closed_completions_.size()) {
        auto &closed = closed_completions_[closed_completion_count_++];
        closed = ClosedCompletion{};
        closed.used = true;
        closed.runtime = frame.runtime;
        closed.reader_context = frame.reader_context;
        closed.worker_context = frame.worker_context;
        closed.operation_serial = frame.operation_serial;
        closed.descriptor_generation = frame.origin.descriptor_generation;
        closed.request_generation = frame.origin.request_generation;
        closed.owner_invalidation_generation = frame.origin.owner_invalidation_generation;
        closed.owner = frame.origin.owner;
        if (frame.origin.descriptor_index < descriptor_count_) {
            const auto &descriptor = descriptors_[frame.origin.descriptor_index];
            closed.descriptor = descriptor.descriptor;
            closed.writer = descriptor.writer;
            closed.snapshot = descriptor.snapshot;
        }
        closed.raw_manager = frame.raw_manager;
        closed.raw_owner = frame.origin.raw_owner;
        closed.raw_record = frame.raw_record;
        closed.event_uid = frame.event_uid;
        if (closed.event_uid == 0u && frame.runtime != nullptr) {
            // A route closed before its first worker syscall (for example an
            // unsupported classifier result) still lets the original reader
            // issue that syscall. Resolve the manager's event handle the same
            // way the active path does so the late call can be correlated; a
            // changed handle simply fails to match and fails closed.
            std::uint32_t address{};
            if (add_u32(frame.raw_manager, kTransformEventOffset, address) &&
                valid_ram(*frame.runtime, address, 4u)) {
                if (const auto *event = frame.runtime->memory().raw_pointer(address, 4u))
                    closed.event_uid = read32(event);
            }
        }
        closed.raw_scratch = frame.origin.raw_scratch;
        closed.raw_destination = frame.raw_destination;
        closed.logical_bytes = frame.logical_bytes;
        closed.write_footprint = frame.write_footprint;
        closed.reader_execution = frame.reader_execution;
        closed.worker_execution = frame.worker_execution;
        closed.last_call = frame.last_call;
        closed.outcome = outcome;
    }
    // Keep operation call receipts alive until a matching late return is
    // observed.  Their operation/generation tokens prevent a new operation
    // from borrowing the closure, while dropping them here would make the
    // original continuation fail before ClosedCompletion can classify it.
    frame = CompletionFrame{};
    if (stats_.active_completions != 0u) --stats_.active_completions;
    return true;
}

void TextureTransferTracker::cancel_completions_for_descriptor(
    std::uint64_t generation) noexcept {
    for (auto &frame : completion_frames_) {
        if (!frame.active || frame.origin.descriptor_generation != generation) continue;
        (void)frame.progress.accept(CompletionStep::Cancel);
        (void)finish_completion(frame, TextureCompletionOutcome::Cancelled);
    }
}

bool TextureTransferTracker::completion_closed_matches(
    ClosedCompletion &closed, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint,
    const CompletionSample &sample) noexcept {
    if (!closed.used || closed.runtime != &runtime) return false;
    if (completion_worker_checkpoint(checkpoint)) {
        // A copy-worker continuation can be the first callback after an
        // unsupported terminal branch, so bind its worker context only after
        // the decoded manager/descriptor identity matches this closure.
        if (closed.worker_context == nullptr) {
            if (!sample.has_manager ||
                !canonical_equal(sample.manager, closed.raw_manager)) return false;
            closed.worker_context = &context;
            closed.worker_execution = psprecomp::capture_runtime_execution_context();
        } else if (closed.worker_context != &context ||
                   !same_execution(closed.worker_execution)) return false;
    } else if (completion_reader_checkpoint(checkpoint)) {
        const bool wait_alias = checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn &&
            closed.reader_context != nullptr &&
            closed.reader_context->gpr[29] == context.gpr[29] &&
            psprecomp::runtime_thread_uid() == closed.reader_execution.thread_uid;
        if ((closed.reader_context != &context && !wait_alias) ||
            (checkpoint != TextureCompletionCheckpoint::WorkerWaitReturn &&
             !same_execution(closed.reader_execution))) return false;
    }
    const auto *info = texture_completion_checkpoint_info(checkpoint);
    if (info == nullptr) return false;
    if (info->requires_call_receipt) {
        if (!sample.has_call || sample.call_checkpoint != info->call_checkpoint)
            return false;
        CompletionCallReceipt *receipt = nullptr;
        const auto current_execution = psprecomp::capture_runtime_execution_context();
        const auto *receipt_context =
            sample.call_checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall &&
                    closed.reader_context != nullptr
                ? closed.reader_context : &context;
        for (auto &candidate : completion_call_receipts_) {
            if (!candidate.active || candidate.runtime != &runtime ||
                candidate.operation_serial != closed.operation_serial ||
                candidate.descriptor_generation != closed.descriptor_generation ||
                candidate.request_generation != closed.request_generation ||
                candidate.context != receipt_context ||
                !same_call_arguments(candidate.call, sample.call) ||
                candidate.execution.thread_uid != current_execution.thread_uid ||
                (sample.call_checkpoint != TextureCompletionCallCheckpoint::WorkerWaitCall &&
                 candidate.execution.switch_generation != current_execution.switch_generation))
                continue;
            if (receipt != nullptr) return false;
            receipt = &candidate;
        }
        if (receipt == nullptr) return false;
        if (checkpoint != TextureCompletionCheckpoint::RetirementEntry) {
            receipt->active = false;
            if (completion_call_receipt_count_ != 0u) --completion_call_receipt_count_;
            closed.late_return_call = sample.call;
            closed.has_late_return_call = true;
        }
    }
    if (sample.has_manager && !canonical_equal(sample.manager, closed.raw_manager))
        return false;
    if (sample.has_descriptor && !canonical_equal(sample.descriptor, closed.raw_record))
        return false;
    if (sample.has_call) {
        const auto &call = sample.call;
        if (call.has_manager && !canonical_equal(call.manager, closed.raw_manager))
            return false;
        if (call.has_descriptor && !canonical_equal(call.descriptor, closed.raw_record))
            return false;
        if (call.has_destination &&
            !canonical_equal(call.destination, closed.raw_destination)) return false;
        if (call.has_source &&
            !canonical_equal(call.source, closed.raw_scratch)) return false;
        if (call.has_logical_length && call.logical_length != closed.logical_bytes)
            return false;
        if (call.has_footprint) {
            const auto expected = call.checkpoint == TextureCompletionCallCheckpoint::TransformCall
                ? closed.write_footprint : closed.logical_bytes;
            if (call.footprint != expected) return false;
        }
    }
    if (sample.has_call &&
        (checkpoint == TextureCompletionCheckpoint::WorkerRequestReturn ||
         checkpoint == TextureCompletionCheckpoint::WorkerEventSetReturn ||
         checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn ||
         checkpoint == TextureCompletionCheckpoint::WorkerAckReturn) &&
        (!sample.call.has_event || sample.call.event != closed.event_uid)) return false;
    const TextureTransferDescriptorRecord *current_descriptor = nullptr;
    for (const auto &descriptor : descriptors_) {
        if (!descriptor.current || descriptor.generation != closed.descriptor_generation ||
            !canonical_equal(descriptor.raw_descriptor, closed.raw_record)) continue;
        current_descriptor = &descriptor;
        break;
    }
    if (current_descriptor == nullptr || current_descriptor->owner != closed.owner ||
        current_descriptor->descriptor != closed.descriptor ||
        current_descriptor->writer != closed.writer) return false;
    if (closed.owner.instance != 0u) {
        const auto owner = lifetime_->owner_token(closed.raw_owner);
        const auto invalidation = lifetime_->owner_invalidation_generation(closed.raw_owner);
        if (!owner || *owner != closed.owner || !invalidation ||
            *invalidation != closed.owner_invalidation_generation) return false;
    }
    if (checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn) {
        closed.reader_context = &context;
        closed.reader_execution = psprecomp::capture_runtime_execution_context();
    }
    if (closed.outcome == TextureCompletionOutcome::Completed) {
        const auto *bytes = runtime.memory().raw_pointer(closed.raw_record, 32u);
        if (bytes == nullptr || bytes[0] != 0u || bytes[1] != 0u ||
            !std::equal(closed.snapshot.begin() + 2u, closed.snapshot.end(), bytes + 2u))
            return false;
    }
    return true;
}

bool TextureTransferTracker::completion_closed_call_matches(
    ClosedCompletion &closed, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    const CompletionSample &sample) noexcept {
    if (!sample.has_call || !closed.used || closed.runtime != &runtime) return false;
    const auto checkpoint = sample.call_checkpoint;
    const bool worker = checkpoint == TextureCompletionCallCheckpoint::WorkerCopyCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall ||
        checkpoint == TextureCompletionCallCheckpoint::TransformCall ||
        checkpoint == TextureCompletionCallCheckpoint::DigestCall;
    if (worker) {
        if (closed.worker_context == nullptr) {
            if (!sample.call.has_manager || !sample.call.has_descriptor ||
                !canonical_equal(sample.call.manager, closed.raw_manager) ||
                !canonical_equal(sample.call.descriptor, closed.raw_record)) return false;
            closed.worker_context = &context;
            closed.worker_execution = psprecomp::capture_runtime_execution_context();
        } else if (closed.worker_context != &context ||
                   !same_execution(closed.worker_execution)) return false;
    } else if (closed.reader_context != &context || !same_execution(closed.reader_execution)) {
        return false;
    }
    const auto *info = texture_completion_call_boundary_info(checkpoint);
    if (info == nullptr) return false;
    if (sample.call.return_pc != info->return_pc ||
        sample.call.sp != context.gpr[29] || context.gpr[31] != info->return_pc)
        return false;
    if (sample.call.has_manager && !canonical_equal(sample.call.manager, closed.raw_manager))
        return false;
    if (sample.call.has_descriptor &&
        !canonical_equal(sample.call.descriptor, closed.raw_record)) return false;
    if (sample.call.has_destination &&
        !canonical_equal(sample.call.destination, closed.raw_destination)) return false;
    if (sample.call.has_source &&
        !canonical_equal(sample.call.source, closed.raw_scratch)) return false;
    if (sample.call.has_logical_length && sample.call.logical_length != closed.logical_bytes)
        return false;
    if (sample.call.has_footprint) {
        const auto expected = checkpoint == TextureCompletionCallCheckpoint::TransformCall
            ? closed.write_footprint : closed.logical_bytes;
        if (sample.call.footprint != expected) return false;
    }
    if ((checkpoint == TextureCompletionCallCheckpoint::WorkerRequestCall ||
         checkpoint == TextureCompletionCallCheckpoint::WorkerEventSetCall ||
         checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall ||
         checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall) &&
        (!sample.call.has_event || sample.call.event != closed.event_uid)) return false;
    const TextureTransferDescriptorRecord *descriptor = nullptr;
    for (const auto &candidate : descriptors_) {
        if (candidate.current && candidate.generation == closed.descriptor_generation &&
            canonical_equal(candidate.raw_descriptor, closed.raw_record)) {
            descriptor = &candidate;
            break;
        }
    }
    if (descriptor == nullptr || descriptor->owner != closed.owner ||
        descriptor->descriptor != closed.descriptor || descriptor->writer != closed.writer)
        return false;
    if (closed.owner.instance != 0u) {
        const auto owner = lifetime_->owner_token(closed.raw_owner);
        const auto invalidation = lifetime_->owner_invalidation_generation(closed.raw_owner);
        if (!owner || *owner != closed.owner || !invalidation ||
            *invalidation != closed.owner_invalidation_generation) return false;
    }
    DescriptorSnapshot current;
    if (!read_descriptor(runtime, closed.raw_record, current)) return false;
    if (closed.outcome == TextureCompletionOutcome::Completed) {
        if (current.bytes[0] != 0u || current.bytes[1] != 0u ||
            !std::equal(current.bytes.begin() + 2u, current.bytes.end(),
                        closed.snapshot.begin() + 2u)) return false;
    } else if (current.bytes != closed.snapshot) {
        return false;
    }
    return true;
}

bool TextureTransferTracker::completion_closed_late_return_matches(
    const ClosedCompletion &closed, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint) const noexcept {
    if (!closed.used || closed.runtime != &runtime || !closed.has_late_return_call)
        return false;
    const auto *info = texture_completion_checkpoint_info(checkpoint);
    if (info == nullptr || !info->requires_call_receipt ||
        closed.late_return_call.checkpoint != info->call_checkpoint ||
        closed.late_return_call.sp != context.gpr[29] ||
        context.gpr[31] != closed.late_return_call.return_pc) return false;
    const bool worker = checkpoint == TextureCompletionCheckpoint::WorkerCopyReturn ||
        checkpoint == TextureCompletionCheckpoint::TransformReturn ||
        checkpoint == TextureCompletionCheckpoint::DigestReturn ||
        checkpoint == TextureCompletionCheckpoint::WorkerAckReturn;
    const auto *expected_context = worker ? closed.worker_context : closed.reader_context;
    const auto expected_execution = worker ? closed.worker_execution : closed.reader_execution;
    if (worker) return expected_context == &context && same_execution(expected_execution);
    const bool wait_alias = checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn &&
        expected_context != nullptr && expected_context->gpr[29] == context.gpr[29] &&
        psprecomp::runtime_thread_uid() == expected_execution.thread_uid;
    return (expected_context == &context || wait_alias) &&
        (checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn ||
         same_execution(expected_execution));
}

bool TextureTransferTracker::retain_closed_completion_call(
    ClosedCompletion &closed, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    const CompletionSample &sample) noexcept {
    if (!completion_closed_call_matches(closed, runtime, context, sample)) return false;
    for (const auto &receipt : completion_call_receipts_) {
        if (receipt.active && receipt.operation_serial == closed.operation_serial &&
            receipt.context == &context && same_call_arguments(receipt.call, sample.call))
            return false;
    }
    CompletionCallReceipt *slot = nullptr;
    for (auto &candidate : completion_call_receipts_) {
        if (!candidate.active) { slot = &candidate; break; }
    }
    if (slot == nullptr) return false;
    slot->active = true;
    slot->runtime = &runtime;
    slot->context = &context;
    slot->execution = psprecomp::capture_runtime_execution_context();
    slot->call = sample.call;
    slot->operation_serial = closed.operation_serial;
    slot->descriptor_generation = closed.descriptor_generation;
    slot->request_generation = closed.request_generation;
    slot->owner = closed.owner;
    slot->descriptor = closed.descriptor;
    slot->writer = closed.writer;
    slot->raw_manager = closed.raw_manager;
    slot->raw_record = closed.raw_record;
    slot->event_uid = closed.event_uid;
    ++completion_call_receipt_count_;
    closed.last_call = sample.call;
    return true;
}

void TextureTransferTracker::cancel_completions_for_manager(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) noexcept {
    const auto manager = context.gpr[4];
    if (manager == 0u) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    std::size_t matched = 0u;
    for (auto &frame : completion_frames_) {
        if (!frame.active || frame.runtime != &runtime ||
            frame.reader_context != &context || !same_execution(frame.reader_execution) ||
            !canonical_equal(frame.raw_manager, manager)) continue;
        ++matched;
        if (!frame.progress.accept(CompletionStep::Cancel) ||
            !finish_completion(frame, TextureCompletionOutcome::Cancelled)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
    }
    if (matched == 0u) {
        for (const auto &closed : closed_completions_) {
            if (closed.used && closed.runtime == &runtime &&
                closed.reader_context == &context && same_execution(closed.reader_execution) &&
                canonical_equal(closed.raw_manager, manager)) return;
        }
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
}

TextureTransferTracker::CompletionFrame *TextureTransferTracker::find_completion_for_call(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCallCheckpoint checkpoint) noexcept {
    const bool worker = checkpoint == TextureCompletionCallCheckpoint::WorkerCopyCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall ||
        checkpoint == TextureCompletionCallCheckpoint::TransformCall ||
        checkpoint == TextureCompletionCallCheckpoint::DigestCall;
    CompletionFrame *found = nullptr;
    for (auto &frame : completion_frames_) {
        if (!frame.active || frame.runtime != &runtime) continue;
        if (worker) {
            if (frame.worker_context != &context ||
                !same_execution(frame.worker_execution) ||
                !completion_worker_identity_valid(frame, runtime, context)) continue;
        } else {
            if (frame.reader_context != &context ||
                !same_execution(frame.reader_execution)) continue;
        }
        if (found != nullptr) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return nullptr;
        }
        found = &frame;
    }
    return found;
}

bool TextureTransferTracker::consume_completion_call(
    const CompletionFrame &frame,
    const CompletionSample &sample) noexcept {
    if (!sample.has_call) return false;
    const auto checkpoint = sample.call_checkpoint;
    const bool worker = checkpoint == TextureCompletionCallCheckpoint::WorkerCopyCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall ||
        checkpoint == TextureCompletionCallCheckpoint::TransformCall ||
        checkpoint == TextureCompletionCallCheckpoint::DigestCall;
    const auto *context = worker ? frame.worker_context : frame.reader_context;
    const auto current_execution = psprecomp::capture_runtime_execution_context();
    CompletionCallReceipt *found = nullptr;
    for (auto &receipt : completion_call_receipts_) {
        if (!receipt.active || receipt.runtime != frame.runtime ||
            receipt.context != context || receipt.call.checkpoint != checkpoint ||
            receipt.operation_serial != frame.operation_serial ||
            receipt.descriptor_generation != frame.origin.descriptor_generation ||
            receipt.request_generation != frame.origin.request_generation ||
            !same_call_arguments(receipt.call, sample.call) ||
            receipt.execution.thread_uid != current_execution.thread_uid ||
            (checkpoint != TextureCompletionCallCheckpoint::WorkerWaitCall &&
             receipt.execution.switch_generation != current_execution.switch_generation)) continue;
        if (found != nullptr) return false;
        found = &receipt;
    }
    if (found == nullptr) return false;
    found->active = false;
    if (completion_call_receipt_count_ != 0u) --completion_call_receipt_count_;
    return true;
}

bool TextureTransferTracker::has_completion_call(
    const CompletionFrame &frame,
    TextureCompletionCallCheckpoint checkpoint) const noexcept {
    const bool worker = checkpoint == TextureCompletionCallCheckpoint::WorkerCopyCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall ||
        checkpoint == TextureCompletionCallCheckpoint::TransformCall ||
        checkpoint == TextureCompletionCallCheckpoint::DigestCall;
    const auto execution = worker ? frame.worker_execution : frame.reader_execution;
    const auto *context = worker ? frame.worker_context : frame.reader_context;
    for (const auto &receipt : completion_call_receipts_) {
        if (receipt.active && receipt.runtime == frame.runtime &&
            receipt.context == context && receipt.call.checkpoint == checkpoint &&
            receipt.operation_serial == frame.operation_serial &&
            receipt.execution.thread_uid == execution.thread_uid &&
            receipt.execution.switch_generation == execution.switch_generation) return true;
    }
    return false;
}

void TextureTransferTracker::observe_completion_call_impl(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    const CompletionSample &sample) {
    if (sample.status != CompletionDecodeStatus::Observed || !sample.has_call) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    const auto checkpoint = sample.call_checkpoint;
    auto *frame = find_completion_for_call(runtime, context, checkpoint);
    if (frame == nullptr) {
        for (auto &closed : closed_completions_) {
            if (!closed.used || closed.runtime != &runtime) continue;
            if (retain_closed_completion_call(closed, runtime, context, sample)) return;
        }
        fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return;
    }
    if ((checkpoint == TextureCompletionCallCheckpoint::WorkerRequestCall ||
         checkpoint == TextureCompletionCallCheckpoint::WorkerEventSetCall ||
         checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall ||
         checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall) &&
        frame->event_uid == 0u) {
        std::uint32_t address{};
        if (!add_u32(frame->raw_manager, kTransformEventOffset, address) ||
            !valid_ram(runtime, address, 4u)) {
            fail(TextureTransferTrackerError::InvalidRange, true);
            return;
        }
        const auto *event = runtime.memory().raw_pointer(address, 4u);
        if (event == nullptr || (frame->event_uid = read32(event)) == 0u) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
    }
    const auto syscall_call =
        checkpoint == TextureCompletionCallCheckpoint::WorkerRequestCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerEventSetCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall;
    if ((!syscall_call && (!sample.call.has_manager || !sample.call.has_descriptor)) ||
        (!syscall_call && sample.call.has_manager &&
         !canonical_equal(sample.call.manager, frame->raw_manager)) ||
        (!syscall_call && sample.call.has_descriptor &&
         !canonical_equal(sample.call.descriptor, frame->raw_record)) ||
        (checkpoint == TextureCompletionCallCheckpoint::InlineCopyCall &&
         (!sample.call.has_destination || !sample.call.has_source ||
          !sample.call.has_logical_length ||
          !canonical_equal(sample.call.destination, frame->raw_destination) ||
          sample.call.logical_length != frame->logical_bytes)) ||
        (checkpoint == TextureCompletionCallCheckpoint::WorkerCopyCall &&
         (!sample.call.has_destination || !sample.call.has_source ||
          !sample.call.has_logical_length ||
          !canonical_equal(sample.call.destination, frame->raw_destination) ||
          sample.call.logical_length != frame->logical_bytes)) ||
        ((checkpoint == TextureCompletionCallCheckpoint::WorkerRequestCall ||
          checkpoint == TextureCompletionCallCheckpoint::WorkerEventSetCall ||
          checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall ||
          checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall) &&
         (!sample.call.has_event || sample.call.event != frame->event_uid))) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    for (const auto &receipt : completion_call_receipts_) {
        if (receipt.active && receipt.runtime == &runtime && receipt.context == &context &&
            receipt.call.checkpoint == checkpoint && receipt.operation_serial == frame->operation_serial &&
            same_execution(receipt.execution) &&
            same_call_arguments(receipt.call, sample.call)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
    }
    CompletionCallReceipt *slot = nullptr;
    for (auto &candidate : completion_call_receipts_) {
        if (!candidate.active) { slot = &candidate; break; }
    }
    if (slot == nullptr) {
        fail(TextureTransferTrackerError::NoCapacity, true);
        return;
    }
    slot->active = true;
    slot->runtime = &runtime;
    slot->context = &context;
    slot->execution = psprecomp::capture_runtime_execution_context();
    slot->call = sample.call;
    slot->operation_serial = frame->operation_serial;
    slot->descriptor_generation = frame->origin.descriptor_generation;
    slot->request_generation = frame->origin.request_generation;
    slot->owner = frame->origin.owner;
    slot->descriptor = frame->completion_record_index < completion_record_count_
        ? completion_records_[frame->completion_record_index].descriptor
        : resources::AuthorityToken{};
    slot->writer = frame->completion_record_index < completion_record_count_
        ? completion_records_[frame->completion_record_index].writer
        : resources::AuthorityToken{};
    slot->raw_manager = frame->raw_manager;
    slot->raw_record = frame->raw_record;
    slot->event_uid = frame->event_uid;
    ++completion_call_receipt_count_;
    if (checkpoint == TextureCompletionCallCheckpoint::WorkerRequestCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerEventSetCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall ||
        checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall) {
        if (slot->event_uid == 0u) {
            const std::uint32_t address = frame->raw_manager + kTransformEventOffset;
            if (!valid_ram(runtime, address, 4u)) {
                slot->active = false;
                if (completion_call_receipt_count_ != 0u) --completion_call_receipt_count_;
                fail(TextureTransferTrackerError::InvalidRange, true);
                return;
            }
            const auto *event = runtime.memory().raw_pointer(address, 4u);
            if (event == nullptr || (slot->event_uid = read32(event)) == 0u) {
                slot->active = false;
                if (completion_call_receipt_count_ != 0u) --completion_call_receipt_count_;
                fail(TextureTransferTrackerError::InvalidObservation, true);
                return;
            }
        }
    }
    frame->last_call = sample.call;
    if (checkpoint == TextureCompletionCallCheckpoint::TransformCall ||
        checkpoint == TextureCompletionCallCheckpoint::DigestCall) {
        frame->worker_bound = true;
    }
    if (checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall)
        frame->wait_call_observed = true;
}

void TextureTransferTracker::observe_completion_impl(
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
    const CompletionSample &sample) {
    if (!config_.observe_completion) return;
    if (sample.status == CompletionDecodeStatus::Invalid) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    const auto checkpoint = sample.checkpoint;
    if (checkpoint == TextureCompletionCheckpoint::GroupCancellation ||
        checkpoint == TextureCompletionCheckpoint::FullQueueCancellation) {
        cancel_completions_for_manager(runtime, context);
        return;
    }
    auto *frame = find_completion(runtime, context, checkpoint);
    if (frame == nullptr) {
        for (auto &closed : closed_completions_)
            if (completion_closed_matches(closed, runtime, context, checkpoint, sample)) return;
        fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return;
    }

    if (checkpoint == TextureCompletionCheckpoint::WorkerEntry) {
        if (frame->worker_context != nullptr ||
            !completion_worker_identity_valid(*frame, runtime, context)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        frame->worker_context = &context;
        frame->worker_execution = psprecomp::capture_runtime_execution_context();
        frame->worker_registered = true;
        DescriptorSnapshot worker_descriptor;
        if (!read_descriptor(runtime, frame->raw_record, worker_descriptor) ||
            worker_descriptor.bytes != frame->origin.descriptor_snapshot) {
            fail(TextureTransferTrackerError::InvalidDescriptor, true);
            return;
        }
        // WorkerEntry only registers a scheduler context. It is not a request
        // receipt and cannot move CompletionProgress to WorkerRunning.
        return;
    }

    if (checkpoint == TextureCompletionCheckpoint::WorkerRequestCall &&
        frame->event_uid == 0u) {
        std::uint32_t address{};
        if (!add_u32(frame->raw_manager, kTransformEventOffset, address) ||
            !valid_ram(runtime, address, 4u)) {
            fail(TextureTransferTrackerError::InvalidRange, true);
            return;
        }
        const auto *event = runtime.memory().raw_pointer(address, 4u);
        if (event == nullptr || (frame->event_uid = read32(event)) == 0u) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
    }
    if (!completion_identity_valid(*frame, runtime, context, checkpoint, sample)) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    auto accept = [this, frame](CompletionStep step) {
        if (!frame->progress.accept(step)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return false;
        }
        return true;
    };
    switch (checkpoint) {
    case TextureCompletionCheckpoint::ClassifierReturn:
        // The bounded selected route is the inline branch. A nonzero
        // classifier result is conservatively unsupported and retains writer.
        if (!sample.has_result || sample.result != 0u) {
            if (!frame->progress.accept(CompletionStep::UnsupportedCopyRoute) ||
                !finish_completion(*frame, TextureCompletionOutcome::Unsupported))
                fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        if (!frame->progress.observe_inline_classified()) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        break;
    case TextureCompletionCheckpoint::CopyReturn:
        if (frame->copy_return_observed ||
            !consume_completion_call(*frame, sample) ||
            !frame->progress.observe_copy_return()) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        frame->copy_return_observed = true;
        break;
    case TextureCompletionCheckpoint::HelperReturn:
        if (!consume_completion_call(*frame, sample) ||
            !accept(CompletionStep::HelperReturned)) return;
        break;
    case TextureCompletionCheckpoint::PolicyReturn:
        if (!sample.has_result || sample.result == 0u) {
            if (!frame->progress.accept(CompletionStep::Abort) ||
                !finish_completion(*frame, TextureCompletionOutcome::Aborted))
                fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        if (!consume_completion_call(*frame, sample) ||
            !accept(CompletionStep::PolicyAccepted)) return;
        break;
    case TextureCompletionCheckpoint::WorkerRequestCall:
        if (frame->request_call_observed || !accept(CompletionStep::WorkerRequestCall)) return;
        frame->request_call_observed = true;
        break;
    case TextureCompletionCheckpoint::WorkerRequestReturn:
        if (!frame->request_call_observed || frame->request_return_observed ||
            !consume_completion_call(*frame, sample)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        frame->request_return_observed = true;
        break;
    case TextureCompletionCheckpoint::WorkerEventSetReturn:
        if (!consume_completion_call(*frame,
                sample)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        break;
    case TextureCompletionCheckpoint::WorkerWaitReturn:
        if (frame->wait_return_observed || !frame->request_call_observed ||
            !frame->request_return_observed || !frame->wait_call_observed ||
            !frame->worker_bound || !frame->ack_return_observed ||
            !consume_completion_call(*frame, sample) ||
            !frame->progress.observe_main_wait_return()) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        frame->wait_return_observed = true;
        frame->reader_execution = psprecomp::capture_runtime_execution_context();
        break;
    case TextureCompletionCheckpoint::WorkerEntry:
        if (frame->worker_context != nullptr ||
            !completion_worker_identity_valid(*frame, runtime, context)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        frame->worker_context = &context;
        frame->worker_execution = psprecomp::capture_runtime_execution_context();
        frame->worker_registered = true;
        {
            DescriptorSnapshot worker_descriptor;
            if (!read_descriptor(runtime, frame->raw_record, worker_descriptor) ||
                worker_descriptor.bytes != frame->origin.descriptor_snapshot) {
                fail(TextureTransferTrackerError::InvalidDescriptor, true);
                return;
            }
        }
        return;
    case TextureCompletionCheckpoint::VerbatimBranch:
        if (sample.status != CompletionDecodeStatus::Skip ||
            sample.skip_reason != CompletionSkipReason::VerbatimBranch ||
            frame->progress.needs_transform) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        if (frame->progress.stage == CompletionStage::WorkerArmed) {
            if (!frame->worker_registered || frame->worker_bound ||
                !frame->progress.observe_worker_accept()) {
                fail(TextureTransferTrackerError::InvalidObservation, true);
                return;
            }
            frame->worker_bound = true;
        }
        if (!accept(CompletionStep::VerbatimObserved)) return;
        break;
    case TextureCompletionCheckpoint::DigestSkippedBranch:
        if (sample.status != CompletionDecodeStatus::Skip ||
            sample.skip_reason != (frame->progress.needs_digest
                                       ? CompletionSkipReason::DigestJoined
                                       : CompletionSkipReason::DigestNotRequested)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        if (frame->progress.needs_digest) {
            if (frame->progress.stage != CompletionStage::DigestReady) {
                fail(TextureTransferTrackerError::InvalidObservation, true);
                return;
            }
        } else {
            if (frame->progress.stage == CompletionStage::WorkerArmed) {
                if (!frame->worker_registered || frame->worker_bound ||
                    !frame->progress.observe_worker_accept()) {
                    fail(TextureTransferTrackerError::InvalidObservation, true);
                    return;
                }
                frame->worker_bound = true;
            }
            if (!accept(CompletionStep::DigestSkipped)) return;
        }
        break;
    case TextureCompletionCheckpoint::TransformCall:
        if (frame->progress.stage == CompletionStage::WorkerArmed) {
            if (!frame->worker_registered || frame->worker_bound ||
                !frame->progress.observe_worker_accept()) {
                fail(TextureTransferTrackerError::InvalidObservation, true);
                return;
            }
            frame->worker_bound = true;
        }
        if (!accept(CompletionStep::TransformCall)) return;
        break;
    case TextureCompletionCheckpoint::TransformReturn:
        if (!consume_completion_call(*frame, sample) ||
            !accept(CompletionStep::TransformReturned)) return;
        break;
    case TextureCompletionCheckpoint::DigestCall:
        frame->digest_computed = true;
        if (!accept(CompletionStep::DigestCall)) return;
        break;
    case TextureCompletionCheckpoint::DigestReturn:
        if (!consume_completion_call(*frame, sample) ||
            !accept(CompletionStep::DigestReturned)) return;
        break;
    case TextureCompletionCheckpoint::WorkerAckReturn:
        if (!frame->worker_bound || frame->ack_return_observed ||
            !consume_completion_call(*frame, sample) ||
            !frame->progress.observe_worker_ack_return()) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        frame->ack_return_observed = true;
        break;
    case TextureCompletionCheckpoint::RetirementCall:
        if (frame->origin.descriptor_index >= descriptor_count_) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        {
            const auto &descriptor = descriptors_[frame->origin.descriptor_index];
            const auto *bytes = runtime.memory().raw_pointer(descriptor.raw_descriptor, 32u);
            if (bytes == nullptr || !descriptor.current ||
                !std::equal(bytes, bytes + 32u, descriptor.snapshot.begin())) {
                fail(TextureTransferTrackerError::InvalidDescriptor, true);
                return;
            }
            std::copy_n(bytes, frame->retirement_snapshot.size(),
                        frame->retirement_snapshot.begin());
            const auto *consumer = runtime.memory().raw_pointer(
                frame->raw_manager + kQueueConsumerOffset, 4u);
            const auto *state = runtime.memory().raw_pointer(
                frame->raw_manager + kQueueStateOffset, 4u);
            if (consumer == nullptr || state == nullptr) {
                fail(TextureTransferTrackerError::InvalidRange, true);
                return;
            }
            frame->retirement_consumer = read32(consumer);
            if (frame->retirement_consumer >= kReadQueueRecords) {
                fail(TextureTransferTrackerError::InvalidObservation, true);
                return;
            }
            frame->retirement_expected_consumer =
                (frame->retirement_consumer + 1u) % kReadQueueRecords;
            frame->retirement_state = read32(state);
            frame->retirement_snapshot_valid = true;
        }
        if (!frame->wait_return_observed || !frame->progress.observe_retirement_call()) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        break;
    case TextureCompletionCheckpoint::RetirementEntry:
        if (!has_completion_call(*frame,
                 TextureCompletionCallCheckpoint::RetirementCall) ||
            !accept(CompletionStep::RetirementEntered)) return;
        break;
    case TextureCompletionCheckpoint::RetirementReturn:
        if (!frame->retirement_snapshot_valid) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        {
            const auto &descriptor = descriptors_[frame->origin.descriptor_index];
            const auto *bytes = runtime.memory().raw_pointer(descriptor.raw_descriptor, 32u);
            const auto *consumer = runtime.memory().raw_pointer(
                frame->raw_manager + kQueueConsumerOffset, 4u);
            const auto *state = runtime.memory().raw_pointer(
                frame->raw_manager + kQueueStateOffset, 4u);
            if (bytes == nullptr || consumer == nullptr || state == nullptr ||
                frame->retirement_snapshot[0] != 1u || frame->retirement_snapshot[1] != 0u ||
                bytes[0] != 0u || bytes[1] != 0u ||
                !std::equal(frame->retirement_snapshot.begin() + 2,
                            frame->retirement_snapshot.end(), bytes + 2u) ||
                read32(state) != 0u ||
                read32(consumer) != frame->retirement_expected_consumer) {
                fail(TextureTransferTrackerError::InvalidObservation, true);
                return;
            }
        }
        if (!consume_completion_call(*frame, sample) ||
            !accept(CompletionStep::RetirementReturned) ||
            !finish_completion(*frame, TextureCompletionOutcome::Completed))
            fail(TextureTransferTrackerError::InvalidObservation, true);
        break;
    case TextureCompletionCheckpoint::UnsupportedCopyRoute:
        if (!frame->progress.accept(CompletionStep::UnsupportedCopyRoute) ||
            !finish_completion(*frame, TextureCompletionOutcome::Unsupported))
            fail(TextureTransferTrackerError::InvalidObservation, true);
        break;
    case TextureCompletionCheckpoint::WorkerCopyReturn:
        if (!consume_completion_call(*frame, sample) ||
            !frame->progress.accept(CompletionStep::UnsupportedCopyRoute) ||
            !finish_completion(*frame, TextureCompletionOutcome::Unsupported))
            fail(TextureTransferTrackerError::InvalidObservation, true);
        break;
    case TextureCompletionCheckpoint::GroupCancellation:
    case TextureCompletionCheckpoint::FullQueueCancellation:
        break;
    }
}

bool TextureTransferTracker::validate_read_frame(ReadFrame &frame,
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
    std::uint32_t raw_manager, std::uint32_t raw_record) noexcept {
    if (!frame.active || frame.runtime != &runtime || frame.context != &context ||
        !same_execution(frame.execution) ||
        psprecomp::GuestMemory::canonical(raw_manager) !=
            psprecomp::GuestMemory::canonical(frame.raw_manager) ||
        psprecomp::GuestMemory::canonical(raw_record) !=
            psprecomp::GuestMemory::canonical(frame.raw_record)) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return false;
    }

    DescriptorSnapshot snapshot;
    if (!read_descriptor(runtime, raw_record, snapshot) ||
        snapshot.bytes != frame.descriptor_snapshot) {
        fail(TextureTransferTrackerError::InvalidDescriptor, true);
        return false;
    }
    std::uint32_t write_start{};
    if (snapshot.chunk_bytes == 0u || snapshot.total_bytes == 0u ||
        snapshot.total_bytes > resources::SourceAuthority::kSlotBytes ||
        std::uint64_t{snapshot.offset} + snapshot.chunk_bytes > snapshot.total_bytes ||
        !add_u32(snapshot.destination, snapshot.offset, write_start)) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return false;
    }
    const auto footprint64 = snapshot.deobfuscate != 0u
        ? ((std::uint64_t{snapshot.chunk_bytes} + 3u) & ~std::uint64_t{3u})
        : std::uint64_t{snapshot.chunk_bytes};
    if (footprint64 == 0u || footprint64 > std::numeric_limits<std::uint32_t>::max() ||
        std::uint64_t{snapshot.offset} + footprint64 >
            resources::SourceAuthority::kSlotBytes ||
        !valid_ram(runtime, write_start, static_cast<std::uint32_t>(footprint64))) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return false;
    }
    const auto footprint = static_cast<std::uint32_t>(footprint64);

    if (frame.selected &&
        (!frame.has_descriptor_record || frame.descriptor_index >= descriptor_count_)) {
        fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return false;
    }
    if (frame.has_descriptor_record && frame.descriptor_index < descriptor_count_) {
        const auto &record = descriptors_[frame.descriptor_index];
        if (!record.current || record.generation != frame.descriptor_generation ||
            record.load_generation != frame.request_generation ||
            psprecomp::GuestMemory::canonical(record.raw_descriptor) !=
                psprecomp::GuestMemory::canonical(frame.raw_record) ||
            psprecomp::GuestMemory::canonical(record.raw_manager) !=
                psprecomp::GuestMemory::canonical(frame.raw_manager) ||
            record.snapshot != frame.descriptor_snapshot ||
            record.write_footprint != footprint ||
            psprecomp::GuestMemory::canonical(record.raw_destination) !=
                psprecomp::GuestMemory::canonical(write_start)) {
            fail(TextureTransferTrackerError::InvalidDescriptor, true);
            return false;
        }
        if (frame.selected) {
            if (!record.associated_load || record.owner != frame.owner ||
                record.writer.instance == 0u ||
                record.owner_invalidation_generation != frame.owner_invalidation_generation) {
                fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
                return false;
            }
            const auto current_owner = lifetime_->owner_token(frame.raw_owner);
            const auto invalidation = lifetime_->owner_invalidation_generation(frame.raw_owner);
            const auto watched = std::find_if(owners_.begin(), owners_.end(),
                [&frame](const WatchedOwner &owner) {
                    return owner.used && owner.owner == frame.owner;
                });
            if (!current_owner || *current_owner != frame.owner || !invalidation ||
                *invalidation != frame.owner_invalidation_generation || watched == owners_.end() ||
                watched->request_generation != frame.request_generation) {
                fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
                return false;
            }
            const bool writer_pending = std::any_of(writers_.begin(), writers_.end(),
                [&record](const PendingWriter &writer) {
                    return writer.pending && writer.writer == record.writer &&
                        writer.generation == record.generation &&
                        writer.raw_destination == record.raw_destination &&
                        writer.bytes == record.write_footprint;
                });
            if (!writer_pending) {
                fail(TextureTransferTrackerError::InvalidObservation, true);
                return false;
            }
        } else if (record.associated_load || record.owner.instance != 0u ||
                   record.writer.instance != 0u || intersects_watched_slot(write_start, footprint)) {
            fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
            return false;
        }
    } else {
        // A record with no G1a generation is only safely outside this slice if
        // its physical storage is new and its valid write span is disjoint.
        if (descriptor_overlaps_history(raw_record) ||
            intersects_watched_slot(write_start, footprint)) {
            fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
            return false;
        }
    }
    return true;
}

void TextureTransferTracker::observe_load_entry(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) {
    if (context.gpr[5] != kSelectedIndex) return;
    if (!code_ok(runtime, context)) return;
    const auto raw_owner = context.gpr[4];
    const auto owner = lifetime_->owner_token(raw_owner);
    if (!owner) { fail(TextureTransferTrackerError::UnpairedSelectedLoad, true); return; }
    auto *watched = watch_owner(raw_owner, *owner);
    if (context.gpr[6] > 0xFFFFu || watched == nullptr) {
        fail(context.gpr[6] > 0xFFFFu ? TextureTransferTrackerError::InvalidObservation
                                      : TextureTransferTrackerError::NoCapacity, true);
        return;
    }
    for (const auto &prior : loads_) {
        if (prior.phase != LoadPhase::Empty && prior.runtime == &runtime &&
            psprecomp::GuestMemory::canonical(prior.owner_raw) ==
                psprecomp::GuestMemory::canonical(raw_owner)) {
            // A new selected request proves the previous wrapper returned.
            // If its queue-return checkpoint was lost, do not silently replace
            // that frame or its still-pending writer hazard.
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return;
        }
    }
    for (const auto &descriptor : descriptors_) {
        if (descriptor.current && descriptor.owner == *owner)
            cancel_completions_for_descriptor(descriptor.generation);
    }
    // Reload invalidation is observed immediately, but this checkpoint lacks
    // a proven route/length; never manufacture SourceAuthority::begin_load.
    const auto invalidated = authority_->reset_owner(*owner);
    if (!invalidated.ok()) { fail(TextureTransferTrackerError::AuthorityRejected, true); return; }
    auto *frame = new_load();
    if (frame == nullptr) return;
    frame->phase = LoadPhase::Started;
    frame->runtime = &runtime;
    frame->context = &context;
    frame->execution = psprecomp::capture_runtime_execution_context();
    frame->sp = context.gpr[29];
    frame->return_pc = context.gpr[31];
    frame->owner_raw = raw_owner;
    frame->selector = context.gpr[5];
    frame->resource_id = context.gpr[6] & 0xFFFFu;
    frame->owner = *owner;
    watched->request_generation = frame->generation;
    if (!count(stats_.loads_started)) return;
}

void TextureTransferTracker::observe_load_tail(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) {
    if (!code_ok(runtime, context)) return;
    auto *frame = find_load(runtime, context, context.gpr[29], context.gpr[31]);
    if (frame == nullptr) {
        if (error_ != TextureTransferTrackerError::None) return;
        std::uint32_t raw_owner{};
        resources::AuthorityToken owner{};
        const bool selected = context.gpr[25] == kSelectedManagerEnqueue &&
            lifetime_->selected_slot_owner(context.gpr[6], 1u, raw_owner, owner);
        if (selected) {
            (void)watch_owner(raw_owner, owner);
            fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        } else (void)count(stats_.unowned_enqueues);
        return;
    }
    if (frame->phase != LoadPhase::Started || context.gpr[4] == 0u ||
        (context.gpr[5] & 0xFFFFu) != frame->resource_id ||
        context.gpr[8] != 0u || context.gpr[9] != 1u ||
        context.gpr[25] != kSelectedManagerEnqueue) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    std::uint32_t expected_slot{};
    const auto *group_byte = runtime.memory().raw_pointer(frame->owner_raw + kSlotGroupOffset, 1u);
    if (group_byte == nullptr ||
        !add_u32(frame->owner_raw, resources::SourceAuthority::kSlotOffset, expected_slot) ||
        psprecomp::GuestMemory::canonical(context.gpr[6]) !=
            psprecomp::GuestMemory::canonical(expected_slot) ||
        static_cast<std::uint8_t>(context.gpr[7]) !=
            static_cast<std::uint8_t>(static_cast<unsigned>(*group_byte) + 5u)) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    frame->phase = LoadPhase::TailTransferred;
    frame->manager = context.gpr[4];
    frame->destination = context.gpr[6];
    frame->group = static_cast<std::uint8_t>(context.gpr[7]);
    frame->target_pc = context.gpr[25];
    (void)count(stats_.tail_transfers);
}

void TextureTransferTracker::observe_enqueue_entry(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) {
    if (!code_ok(runtime, context)) return;
    if (find_queue(runtime, context, context.gpr[29], context.gpr[31]) != nullptr) {
        fail(TextureTransferTrackerError::ConflictingFrame, true);
        return;
    }
    if (error_ != TextureTransferTrackerError::None) return;

    LoadFrame *load = nullptr;
    for (auto &candidate : loads_) {
        if (candidate.phase != LoadPhase::TailTransferred || candidate.runtime != &runtime ||
            candidate.context != &context || candidate.sp != context.gpr[29] ||
            candidate.return_pc != context.gpr[31] || !same_execution(candidate.execution)) continue;
        if (load != nullptr) { fail(TextureTransferTrackerError::ConflictingFrame, true); return; }
        load = &candidate;
    }
    if (load != nullptr && (context.gpr[4] != load->manager ||
        (context.gpr[5] & 0xFFFFu) != load->resource_id || context.gpr[6] != load->destination ||
        static_cast<std::uint8_t>(context.gpr[7]) != load->group)) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    std::uint32_t raw_owner{};
    resources::AuthorityToken owner{};
    if (load == nullptr &&
        lifetime_->selected_slot_owner(context.gpr[6], 1u, raw_owner, owner)) {
        // A selected-slot writer with no observed load tail cannot be safely
        // classified from its enqueue arguments alone.
        (void)watch_owner(raw_owner, owner);
        fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return;
    }

    auto *queue = new_queue();
    if (queue == nullptr) return;
    queue->active = true;
    queue->tracked = load != nullptr;
    queue->runtime = &runtime;
    queue->context = &context;
    queue->execution = psprecomp::capture_runtime_execution_context();
    queue->entry_sp = context.gpr[29];
    queue->return_pc = context.gpr[31];
    queue->manager = context.gpr[4];
    queue->resource_id = context.gpr[5] & 0xFFFFu;
    queue->destination = context.gpr[6];
    queue->group = static_cast<std::uint8_t>(context.gpr[7]);
    if (load != nullptr) queue->load_generation = load->generation;
    else if (!count(stats_.unowned_enqueues)) return;
    (void)count(stats_.enqueue_entries);
}

void TextureTransferTracker::observe_descriptor_commit(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) {
    if (!code_ok(runtime, context)) return;

    QueueFrame *queue = nullptr;
    for (auto &candidate : queues_) {
        if (!candidate.active || candidate.runtime != &runtime || candidate.context != &context ||
            candidate.entry_sp < 48u || context.gpr[29] != candidate.entry_sp - 48u ||
            !same_execution(candidate.execution)) continue;
        if (queue != nullptr) { fail(TextureTransferTrackerError::ConflictingFrame, true); return; }
        queue = &candidate;
    }
    LoadFrame *detached_load = nullptr;
    if (queue == nullptr) {
        for (auto &candidate : loads_) {
            if (candidate.phase == LoadPhase::Empty || candidate.runtime != &runtime ||
                candidate.context != &context || candidate.sp < 48u ||
                context.gpr[29] != candidate.sp - 48u ||
                !same_execution(candidate.execution)) continue;
            if (detached_load != nullptr) {
                fail(TextureTransferTrackerError::ConflictingFrame, true);
                return;
            }
            detached_load = &candidate;
        }
    }

    if (context.gpr[6] == 0u) {
        fail(TextureTransferTrackerError::InvalidDescriptor, true);
        return;
    }
    const auto raw_descriptor = context.gpr[6];
    const bool descriptor_reuse = descriptor_overlaps_history(raw_descriptor);
    DescriptorSnapshot descriptor;
    if (!read_descriptor(runtime, raw_descriptor, descriptor)) {
        // Without a readable 32-byte slot, neither reuse invalidation nor the
        // writer span can be accounted for. Lock authority rather than treating
        // this as proof that an unrelated request is disjoint.
        fail(TextureTransferTrackerError::InvalidDescriptor, true);
        return;
    }
    std::uint32_t write_start{};
    if (descriptor.chunk_bytes == 0u || descriptor.chunk_bytes >
        std::numeric_limits<std::uint32_t>::max() - 3u ||
        !add_u32(descriptor.destination, descriptor.offset, write_start)) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    const auto footprint64 = descriptor.deobfuscate != 0u
        ? ((std::uint64_t{descriptor.chunk_bytes} + 3u) & ~std::uint64_t{3u})
        : std::uint64_t{descriptor.chunk_bytes};
    if (footprint64 > std::numeric_limits<std::uint32_t>::max()) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    const auto footprint = static_cast<std::uint32_t>(footprint64);
    if (!valid_ram(runtime, write_start, footprint)) {
        // A false overlap result is meaningful only for a valid RAM span.
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    const bool overlaps_watched = intersects_watched_slot(write_start, footprint);
    const LoadFrame *load = nullptr;
    bool load_associated = false;
    TextureTransferTrackerError deferred_error = TextureTransferTrackerError::None;
    if (queue != nullptr && queue->tracked) {
        for (const auto &candidate : loads_)
            if (candidate.phase == LoadPhase::TailTransferred &&
                candidate.generation == queue->load_generation) { load = &candidate; break; }
        if (load == nullptr) deferred_error = TextureTransferTrackerError::UnpairedSelectedLoad;
        else load_associated = descriptor_valid_for(descriptor, *load);
        if (!load_associated && deferred_error == TextureTransferTrackerError::None)
            deferred_error = TextureTransferTrackerError::InvalidDescriptor;
        // A selected request is bounded by the 0x5800-byte owner slot, below
        // the original 0x20000 split threshold, so its queue frame must
        // commit exactly one descriptor. A repeated callback is not a reuse.
        if (queue->descriptors != 0u || (load != nullptr && load->descriptors != 0u)) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
    } else if (detached_load != nullptr) {
        if (detached_load->phase == LoadPhase::TailTransferred &&
            descriptor_valid_for(descriptor, *detached_load)) {
            load = detached_load;
            load_associated = true;
            deferred_error = TextureTransferTrackerError::UnpairedSelectedLoad;
        } else if (detached_load->phase == LoadPhase::TailTransferred) {
            load = detached_load;
            deferred_error = TextureTransferTrackerError::InvalidDescriptor;
            if (!overlaps_watched) {
                if (!descriptor_reuse) {
                    fail(deferred_error, true);
                    return;
                }
            }
        } else {
            load = detached_load;
            deferred_error = TextureTransferTrackerError::UnpairedSelectedLoad;
            if (!overlaps_watched && !descriptor_reuse) {
                fail(deferred_error, true);
                return;
            }
        }
    } else if (!overlaps_watched) {
        // The request manager is shared by unrelated assets. A descriptor
        // outside watched slots still needs a new generation when it reuses
        // or overlaps storage previously used by a selected descriptor.
        if (!descriptor_reuse) return;
    }

    const bool needs_writer = (queue != nullptr && queue->tracked) || load != nullptr ||
        detached_load != nullptr || overlaps_watched;
    if (descriptor_count_ >= config_.max_descriptors ||
        (queue != nullptr && queue->descriptor_count == queue->descriptor_indices.size())) {
        fail(TextureTransferTrackerError::NoCapacity, true);
        return;
    }

    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        fail(TextureTransferTrackerError::CounterExhausted, true);
        return;
    }
    const auto descriptor_issue = authority_->begin_descriptor(raw_descriptor, 32u);
    if (!descriptor_issue.event.ok() || descriptor_issue.token.instance == 0u ||
        descriptor_issue.token.serial == 0u) {
        fail(TextureTransferTrackerError::AuthorityRejected, true);
        return;
    }
    const auto owner = load != nullptr ? load->owner :
        (detached_load != nullptr ? detached_load->owner : resources::AuthorityToken{});
    const auto raw_owner = load != nullptr ? load->owner_raw :
        (detached_load != nullptr ? detached_load->owner_raw : 0u);
    const auto raw_manager = queue != nullptr ? queue->manager :
        (load != nullptr ? load->manager :
         (detached_load != nullptr ? detached_load->manager : 0u));
    std::uint64_t owner_invalidation_generation = 0u;
    if (owner.instance != 0u) {
        const auto invalidation = lifetime_->owner_invalidation_generation(raw_owner);
        if (!invalidation) {
            fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
            return;
        }
        owner_invalidation_generation = *invalidation;
    }
    const auto generation = next_generation_;
    if (needs_writer && !register_pending_writer(owner, descriptor_issue.token, generation,
                                                write_start, footprint)) return;
    auto &record = descriptors_[descriptor_count_];
    record.generation = generation;
    record.load_generation = load != nullptr ? load->generation : 0u;
    record.queue_generation = queue != nullptr ? queue->generation : 0u;
    record.owner_invalidation_generation = owner_invalidation_generation;
    record.raw_descriptor = raw_descriptor;
    record.raw_manager = raw_manager;
    record.raw_destination = write_start;
    record.write_footprint = footprint;
    record.owner = owner;
    record.descriptor = descriptor_issue.token;
    record.snapshot = descriptor.bytes;
    record.associated_load = load_associated;
    for (const auto &writer : writers_)
        if (writer.pending && writer.generation == generation) {
            record.writer = writer.writer;
            break;
        }
    bool any_overlap = false;
    bool raw_reuse = false;
    bool alias_reuse = false;
    const auto new_start = std::uint64_t{psprecomp::GuestMemory::canonical(record.raw_descriptor)};
    const auto new_end = new_start + 32u;
    for (std::size_t i = 0; i < descriptor_count_; ++i) {
        const auto old_raw = descriptors_[i].raw_descriptor;
        const auto old_physical = psprecomp::GuestMemory::canonical(old_raw);
        const auto old_start = std::uint64_t{old_physical};
        const auto old_end = old_start + 32u;
        if (new_start >= old_end || old_start >= new_end) continue;
        any_overlap = true;
        cancel_completions_for_descriptor(descriptors_[i].generation);
        descriptors_[i].current = false;
        if (old_raw == record.raw_descriptor) raw_reuse = true;
        else if (old_start == new_start) alias_reuse = true;
    }
    if ((raw_reuse && !count(stats_.descriptor_raw_pointer_reuses)) ||
        (!raw_reuse && alias_reuse && !count(stats_.descriptor_alias_reuses)) ||
        (any_overlap && !count(stats_.descriptor_ring_reuses))) return;
    const auto record_index = descriptor_count_++;
    ++next_generation_;
    if (!count(stats_.descriptor_generations)) return;
    if (queue != nullptr) {
        ++queue->descriptors;
        queue->descriptor_indices[queue->descriptor_count++] = record_index;
    }
    if (load != nullptr) {
        for (auto &candidate : loads_)
            if (candidate.phase == LoadPhase::TailTransferred &&
                candidate.generation == load->generation) {
                ++candidate.descriptors;
                break;
            }
    }
    if (deferred_error != TextureTransferTrackerError::None) {
        // A known selected descriptor without the enqueue-entry checkpoint is
        // still an asynchronous writer. Retain its descriptor/writer tokens,
        // then revoke all positive authority for the incomplete correlation.
        fail(deferred_error, true);
        return;
    }
}

void TextureTransferTracker::observe_enqueue_return(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) {
    if (!code_ok(runtime, context)) return;
    auto *queue = find_queue(runtime, context, context.gpr[29], context.gpr[31]);
    if (queue == nullptr) {
        if (error_ == TextureTransferTrackerError::None) (void)count(stats_.unowned_enqueues);
        return;
    }
    if (queue->tracked && context.gpr[2] == 1u && queue->descriptors == 0u) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    if (queue->tracked && context.gpr[2] == 1u && !count(stats_.queued_returns)) return;
    for (std::size_t i = 0; i < queue->descriptor_count; ++i) {
        auto &record = descriptors_[queue->descriptor_indices[i]];
        record.queue_return_observed = true;
        record.queue_return_value = context.gpr[2];
    }
    const auto load_generation = queue->load_generation;
    const bool tracked = queue->tracked;
    *queue = QueueFrame{};
    --stats_.active_queue_frames;
    if (tracked) {
        for (auto &load : loads_) {
            if (load.phase != LoadPhase::Empty && load.generation == load_generation) {
                load.phase = LoadPhase::Queued;
                --stats_.active_loads;
                load = LoadFrame{};
                break;
            }
        }
    }
}

void TextureTransferTracker::observe_read_state8(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) {
    if (!code_ok(runtime, context)) return;
    const auto raw_manager = context.gpr[17];
    const auto raw_record = context.gpr[18];
    std::uint32_t consumer_address{}, state_address{};
    if (raw_manager == 0u || raw_record == 0u ||
        !add_u32(raw_manager, kQueueConsumerOffset, consumer_address) ||
        !add_u32(raw_manager, kQueueStateOffset, state_address) ||
        !valid_ram(runtime, consumer_address, 4u) || !valid_ram(runtime, state_address, 4u)) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    const auto *consumer_bytes = runtime.memory().raw_pointer(consumer_address, 4u);
    const auto *state_bytes = runtime.memory().raw_pointer(state_address, 4u);
    if (consumer_bytes == nullptr || state_bytes == nullptr) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    const auto consumer = read32(consumer_bytes);
    if (read32(state_bytes) != kWorkerState8 || consumer >= kReadQueueRecords ||
        context.gpr[7] != consumer) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    std::uint32_t expected_record{};
    const auto record_offset = std::uint64_t{kQueueRecordOffset} +
        std::uint64_t{consumer} * kReadRecordStride;
    if (record_offset > std::numeric_limits<std::uint32_t>::max() ||
        !add_u32(raw_manager, static_cast<std::uint32_t>(record_offset), expected_record) ||
        psprecomp::GuestMemory::canonical(expected_record) !=
            psprecomp::GuestMemory::canonical(raw_record)) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }

    if (auto *existing = find_read_frame(runtime, context, context.gpr[29], raw_record)) {
        if (error_ != TextureTransferTrackerError::None) return;
        if (existing->helper_entered || existing->invocation_observed ||
            existing->active_attempt != kMaxReadAttempts ||
            !existing->retry_state8_required) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        if (!validate_read_frame(*existing, runtime, context, raw_manager, raw_record)) return;
        if (existing->state8_entries == std::numeric_limits<std::uint32_t>::max() ||
            !count(stats_.read_state8_entries)) {
            fail(TextureTransferTrackerError::CounterExhausted, true);
            return;
        }
        existing->retry_state8_required = false;
        ++existing->state8_entries;
        return;
    }
    if (error_ != TextureTransferTrackerError::None) return;
    for (const auto &old : read_frames_) {
        if (old.active && old.runtime == &runtime && old.context == &context &&
            same_execution(old.execution)) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return;
        }
    }

    DescriptorSnapshot descriptor;
    if (!read_descriptor(runtime, raw_record, descriptor)) {
        fail(TextureTransferTrackerError::InvalidDescriptor, true);
        return;
    }
    std::size_t descriptor_index = kMaxDescriptors;
    for (std::size_t i = 0; i < descriptor_count_; ++i) {
        if (!descriptors_[i].current ||
            psprecomp::GuestMemory::canonical(descriptors_[i].raw_descriptor) !=
                psprecomp::GuestMemory::canonical(raw_record)) continue;
        if (descriptor_index != kMaxDescriptors) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return;
        }
        descriptor_index = i;
    }
    const bool selected = descriptor_index != kMaxDescriptors &&
        descriptors_[descriptor_index].associated_load;
    if (selected && !descriptors_[descriptor_index].current) {
        fail(TextureTransferTrackerError::InvalidDescriptor, true);
        return;
    }
    if (!selected && descriptor_overlaps_history(raw_record) &&
        descriptor_index == kMaxDescriptors) {
        fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return;
    }
    std::uint32_t write_start{};
    if (descriptor.chunk_bytes == 0u || descriptor.total_bytes == 0u ||
        descriptor.total_bytes > resources::SourceAuthority::kSlotBytes ||
        std::uint64_t{descriptor.offset} + descriptor.chunk_bytes > descriptor.total_bytes ||
        !add_u32(descriptor.destination, descriptor.offset, write_start)) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    const auto footprint64 = descriptor.deobfuscate != 0u
        ? ((std::uint64_t{descriptor.chunk_bytes} + 3u) & ~std::uint64_t{3u})
        : std::uint64_t{descriptor.chunk_bytes};
    if (footprint64 == 0u || footprint64 > std::numeric_limits<std::uint32_t>::max() ||
        !valid_ram(runtime, write_start, static_cast<std::uint32_t>(footprint64))) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    const bool overlaps_watched = intersects_watched_slot(
        write_start, static_cast<std::uint32_t>(footprint64));
    if (!selected && overlaps_watched) {
        fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return;
    }

    auto *frame = new_read_frame();
    if (frame == nullptr) return;
    frame->runtime = &runtime;
    frame->context = &context;
    frame->execution = psprecomp::capture_runtime_execution_context();
    frame->descriptor_index = descriptor_index;
    frame->raw_manager = raw_manager;
        frame->raw_record = raw_record;
        frame->worker_sp = context.gpr[29];
    frame->state8_entries = 1u;
    frame->selected = selected;
    frame->has_descriptor_record = descriptor_index != kMaxDescriptors;
    frame->descriptor_snapshot = descriptor.bytes;
    frame->requested_bytes = descriptor.chunk_bytes;
    if (frame->has_descriptor_record) {
        const auto &record = descriptors_[descriptor_index];
        frame->descriptor_generation = record.generation;
        frame->request_generation = record.load_generation;
    }
    if (selected) {
        const auto &record = descriptors_[descriptor_index];
        frame->owner_invalidation_generation = record.owner_invalidation_generation;
        frame->owner = record.owner;
        const auto watched = std::find_if(owners_.begin(), owners_.end(),
            [&record](const WatchedOwner &owner) {
                return owner.used && owner.owner == record.owner;
            });
        if (watched == owners_.end()) {
            fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
            return;
        }
        frame->raw_owner = watched->raw_owner;
    } else {
        if (frame->has_descriptor_record) {
            const auto &record = descriptors_[descriptor_index];
            if (record.raw_manager != 0u &&
                psprecomp::GuestMemory::canonical(record.raw_manager) !=
                    psprecomp::GuestMemory::canonical(raw_manager)) {
                fail(TextureTransferTrackerError::InvalidObservation, true);
                return;
            }
        }
        if (!count(stats_.unowned_read_requests)) return;
    }
    if (!count(stats_.read_state8_entries) ||
        !validate_read_frame(*frame, runtime, context, raw_manager, raw_record)) return;
}

void TextureTransferTracker::observe_read_helper_entry(
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context) {
    if (!code_ok(runtime, context)) return;
    const auto raw_manager = context.gpr[4];
    const auto raw_record = context.gpr[6];
    auto *frame = find_read_frame(runtime, context, context.gpr[29], raw_record);
    if (frame == nullptr) {
        if (error_ != TextureTransferTrackerError::None) return;
        // If state-8 entry was missing, we may still fail closed for a selected
        // destination using the helper's complete record argument. A valid,
        // disjoint unowned read can be observed as an unselected local frame.
        DescriptorSnapshot descriptor;
        if (!read_descriptor(runtime, raw_record, descriptor)) {
            fail(TextureTransferTrackerError::InvalidDescriptor, true);
            return;
        }
        std::uint32_t write_start{};
        if (descriptor.chunk_bytes == 0u ||
            !add_u32(descriptor.destination, descriptor.offset, write_start)) {
            fail(TextureTransferTrackerError::InvalidRange, true);
            return;
        }
        const auto footprint64 = descriptor.deobfuscate != 0u
            ? ((std::uint64_t{descriptor.chunk_bytes} + 3u) & ~std::uint64_t{3u})
            : std::uint64_t{descriptor.chunk_bytes};
        if (footprint64 == 0u || footprint64 > std::numeric_limits<std::uint32_t>::max() ||
            !valid_ram(runtime, write_start, static_cast<std::uint32_t>(footprint64))) {
            fail(TextureTransferTrackerError::InvalidRange, true);
            return;
        }
        if (descriptor_overlaps_history(raw_record) ||
            intersects_watched_slot(write_start, static_cast<std::uint32_t>(footprint64))) {
            fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
            return;
        }
        frame = new_read_frame();
        if (frame == nullptr) return;
        frame->runtime = &runtime;
        frame->context = &context;
        frame->execution = psprecomp::capture_runtime_execution_context();
        frame->raw_manager = raw_manager;
        frame->raw_record = raw_record;
        frame->worker_sp = context.gpr[29];
        frame->descriptor_snapshot = descriptor.bytes;
        frame->requested_bytes = descriptor.chunk_bytes;
        if (!count(stats_.unowned_read_requests)) return;
    }
    if (frame->helper_entered || frame->invocation_observed ||
        frame->retry_state8_required ||
        context.gpr[29] != frame->worker_sp ||
        psprecomp::GuestMemory::canonical(raw_manager) !=
            psprecomp::GuestMemory::canonical(frame->raw_manager) ||
        psprecomp::GuestMemory::canonical(raw_record) !=
            psprecomp::GuestMemory::canonical(frame->raw_record) ||
        !validate_read_frame(*frame, runtime, context, raw_manager, raw_record)) {
        if (error_ == TextureTransferTrackerError::None)
            fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    std::uint32_t fd_address{};
    if (!add_u32(raw_manager, kQueueFdOffset, fd_address) ||
        !valid_ram(runtime, fd_address, 4u)) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    const auto *fd_bytes = runtime.memory().raw_pointer(fd_address, 4u);
    if (fd_bytes == nullptr || context.gpr[5] != read32(fd_bytes)) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    frame->helper_entered = true;
    frame->invocation_observed = false;
    frame->helper_sp = context.gpr[29];
    frame->helper_return_pc = context.gpr[31];
    frame->fd = context.gpr[5];
    if (!count(stats_.read_helper_entries)) return;
}

void TextureTransferTracker::observe_read_invoke(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) {
    if (!code_ok(runtime, context)) return;
    ReadFrame *frame = nullptr;
    for (auto &candidate : read_frames_) {
        if (!candidate.active || candidate.runtime != &runtime ||
            candidate.context != &context || !candidate.helper_entered ||
            candidate.invocation_observed || candidate.helper_sp != context.gpr[29] ||
            candidate.helper_return_pc != context.gpr[31] ||
            !same_execution(candidate.execution)) continue;
        if (frame != nullptr) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return;
        }
        frame = &candidate;
    }
    if (frame == nullptr) {
        if (error_ == TextureTransferTrackerError::None)
            fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return;
    }
    if (!validate_read_frame(*frame, runtime, context,
                             frame->raw_manager, frame->raw_record)) return;
    if (context.gpr[29] != frame->worker_sp ||
        psprecomp::GuestMemory::canonical(context.gpr[17]) !=
            psprecomp::GuestMemory::canonical(frame->raw_manager) ||
        psprecomp::GuestMemory::canonical(context.gpr[18]) !=
            psprecomp::GuestMemory::canonical(frame->raw_record) ||
        context.gpr[31] != frame->helper_return_pc) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    std::uint32_t expected_scratch{};
    if (!add_u32(frame->raw_manager, kReadScratchOffset, expected_scratch) ||
        !valid_ram(runtime, context.gpr[5], kReadScratchBytes) ||
        psprecomp::GuestMemory::canonical(context.gpr[5]) !=
            psprecomp::GuestMemory::canonical(expected_scratch) ||
        context.gpr[4] != frame->fd || context.gpr[6] == 0u ||
        context.gpr[6] != frame->requested_bytes ||
        context.gpr[6] > kReadScratchBytes ||
        intersects_watched_slot(context.gpr[5], kReadScratchBytes)) {
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    DescriptorSnapshot descriptor;
    if (!read_descriptor(runtime, frame->raw_record, descriptor) ||
        descriptor.chunk_bytes != context.gpr[6]) {
        fail(TextureTransferTrackerError::InvalidDescriptor, true);
        return;
    }
    frame->raw_scratch = context.gpr[5];
    frame->requested_bytes = context.gpr[6];
    frame->invocation_observed = true;
    if (frame->selected) {
        if (read_attempt_count_ >= config_.max_read_attempts) {
            fail(TextureTransferTrackerError::NoCapacity, true);
            return;
        }
        if (next_read_attempt_ == 0u ||
            next_read_attempt_ > config_.read_attempt_serial_limit) {
            fail(TextureTransferTrackerError::CounterExhausted, true);
            return;
        }
        auto &attempt = read_attempts_[read_attempt_count_];
        attempt = TextureReadAttemptRecord{};
        attempt.serial = next_read_attempt_++;
        attempt.descriptor_generation = frame->descriptor_generation;
        attempt.request_generation = frame->request_generation;
        attempt.owner_invalidation_generation = frame->owner_invalidation_generation;
        attempt.owner = frame->owner;
        attempt.raw_owner = frame->raw_owner;
        attempt.raw_manager = frame->raw_manager;
        attempt.raw_record = frame->raw_record;
        attempt.raw_scratch = frame->raw_scratch;
        attempt.fd = frame->fd;
        attempt.requested_bytes = frame->requested_bytes;
        const auto &descriptor_record = descriptors_[frame->descriptor_index];
        attempt.descriptor = descriptor_record.descriptor;
        attempt.writer = descriptor_record.writer;
        attempt.queue_return_observed = descriptor_record.queue_return_observed;
        frame->active_attempt = read_attempt_count_++;
        stats_.read_attempt_records = read_attempt_count_;
    }
    if (!count(stats_.read_invocations)) return;
}

void TextureTransferTracker::observe_read_result(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) {
    if (!code_ok(runtime, context)) return;
    ReadFrame *frame = nullptr;
    for (auto &candidate : read_frames_) {
        if (!candidate.active || candidate.runtime != &runtime ||
            candidate.context != &context || !candidate.helper_entered ||
            !candidate.invocation_observed || !same_execution(candidate.execution)) continue;
        if (frame != nullptr) {
            fail(TextureTransferTrackerError::ConflictingFrame, true);
            return;
        }
        frame = &candidate;
    }
    if (frame == nullptr) {
        if (error_ == TextureTransferTrackerError::None)
            fail(TextureTransferTrackerError::UnpairedSelectedLoad, true);
        return;
    }
    if (!frame->helper_entered || !frame->invocation_observed ||
        context.gpr[29] != frame->worker_sp ||
        context.gpr[31] != frame->helper_return_pc ||
        psprecomp::GuestMemory::canonical(context.gpr[17]) !=
            psprecomp::GuestMemory::canonical(frame->raw_manager) ||
        psprecomp::GuestMemory::canonical(context.gpr[18]) !=
            psprecomp::GuestMemory::canonical(frame->raw_record) ||
        context.gpr[2] != frame->requested_bytes ||
        !validate_read_frame(*frame, runtime, context,
                             frame->raw_manager, frame->raw_record)) {
        if (error_ == TextureTransferTrackerError::None)
            fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    if (!frame->selected) {
        if (!count(stats_.read_results) || !count(stats_.unowned_read_results)) return;
        *frame = ReadFrame{};
        --stats_.active_read_frames;
        return;
    }
    if (frame->active_attempt >= read_attempt_count_) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    auto &attempt = read_attempts_[frame->active_attempt];
    if (attempt.result_observed || attempt.serial == 0u ||
        attempt.descriptor_generation != frame->descriptor_generation ||
        attempt.request_generation != frame->request_generation ||
        attempt.raw_record != frame->raw_record ||
        attempt.raw_scratch != frame->raw_scratch ||
        attempt.requested_bytes != frame->requested_bytes) {
        fail(TextureTransferTrackerError::ConflictingFrame, true);
        return;
    }
    const auto result = std::bit_cast<std::int32_t>(context.gpr[16]);
    attempt.result = result;
    attempt.result_observed = true;
    if (!count(stats_.read_results)) return;
    if (result > static_cast<std::int32_t>(attempt.requested_bytes)) {
        attempt.outcome = TextureReadOutcome::Rejected;
        (void)count(stats_.rejected_read_results);
        fail(TextureTransferTrackerError::InvalidRange, true);
        return;
    }
    if (result > 0 && static_cast<std::uint32_t>(result) == attempt.requested_bytes) {
        attempt.outcome = TextureReadOutcome::ExactReadObserved;
        if (!count(stats_.exact_read_results)) return;
        if (!begin_completion(*frame, attempt)) return;
        *frame = ReadFrame{};
        --stats_.active_read_frames;
        return;
    }
    attempt.outcome = TextureReadOutcome::RetryObserved;
    if (!count(stats_.retry_read_results)) return;
    frame->helper_entered = false;
    frame->invocation_observed = false;
    frame->retry_state8_required = true;
    frame->active_attempt = kMaxReadAttempts;
}

void TextureTransferTracker::observe_read(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureReadCheckpoint checkpoint) noexcept {
    if (error_ != TextureTransferTrackerError::None) return;
    if (&runtime != runtime_) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    if (!code_ok(runtime, context)) return;
    if (!count(stats_.callbacks)) return;
    try { observe_read_impl(runtime, context, checkpoint); }
    catch (...) { fail(TextureTransferTrackerError::ObservationLost, true); }
}

void TextureTransferTracker::observe_read_impl(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context, TextureReadCheckpoint checkpoint) {
    switch (checkpoint) {
    case TextureReadCheckpoint::WorkerState8Entry:
        observe_read_state8(runtime, context); break;
    case TextureReadCheckpoint::ReadHelperEntry:
        observe_read_helper_entry(runtime, context); break;
    case TextureReadCheckpoint::ReadInvoke:
        observe_read_invoke(runtime, context); break;
    case TextureReadCheckpoint::ReadResult:
        observe_read_result(runtime, context); break;
    default:
        fail(TextureTransferTrackerError::InvalidObservation, true); break;
    }
}

void TextureTransferTracker::observe_completion(
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint, std::uint32_t site_pc) noexcept {
    if (error_ != TextureTransferTrackerError::None || !config_.observe_completion) {
        return;
    }
    if (&runtime != runtime_) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    if (!code_ok(runtime, context)) return;
    if (!count(stats_.callbacks)) return;
    try {
        const auto sample = completion_decoder_.decode_checkpoint(
            runtime, context, checkpoint, site_pc);
        if (sample.status == CompletionDecodeStatus::Invalid) {
            if (sample.error == CompletionDecodeError::MissingCallReceipt)
                for (const auto &closed : closed_completions_)
                    if (completion_closed_late_return_matches(
                            closed, runtime, context, checkpoint)) return;
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        observe_completion_impl(runtime, context, sample);
    }
    catch (...) { fail(TextureTransferTrackerError::ObservationLost, true); }
}

void TextureTransferTracker::observe_completion_call(
    const psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
    TextureCompletionCallCheckpoint checkpoint, std::uint32_t site_pc) noexcept {
    if (error_ != TextureTransferTrackerError::None || !config_.observe_completion) {
        return;
    }
    if (&runtime != runtime_) {
        fail(TextureTransferTrackerError::InvalidObservation, true);
        return;
    }
    if (!code_ok(runtime, context)) return;
    if (!count(stats_.callbacks)) return;
    try {
        const auto sample = completion_decoder_.decode_call(
            runtime, context, checkpoint, site_pc);
        if (sample.status == CompletionDecodeStatus::Invalid) {
            fail(TextureTransferTrackerError::InvalidObservation, true);
            return;
        }
        observe_completion_call_impl(runtime, context, sample);
    }
    catch (...) { fail(TextureTransferTrackerError::ObservationLost, true); }
}

void TextureTransferTracker::observe(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureTransferCheckpoint checkpoint) noexcept {
    if (error_ != TextureTransferTrackerError::None) return;
    if (&runtime != runtime_) { fail(TextureTransferTrackerError::InvalidObservation, true); return; }
    if (!count(stats_.callbacks)) return;
    try { observe_impl(runtime, context, checkpoint); }
    catch (...) { fail(TextureTransferTrackerError::ObservationLost, true); }
}

void TextureTransferTracker::observe_impl(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context, TextureTransferCheckpoint checkpoint) {
    switch (checkpoint) {
    case TextureTransferCheckpoint::OwnerLoadEntry: observe_load_entry(runtime, context); break;
    case TextureTransferCheckpoint::OwnerLoadTail: observe_load_tail(runtime, context); break;
    case TextureTransferCheckpoint::EnqueueEntry: observe_enqueue_entry(runtime, context); break;
    case TextureTransferCheckpoint::DescriptorCommit: observe_descriptor_commit(runtime, context); break;
    case TextureTransferCheckpoint::EnqueueReturn: observe_enqueue_return(runtime, context); break;
    default: fail(TextureTransferTrackerError::InvalidObservation, true); break;
    }
}

} // namespace mhp3rd::native
