#include "native/texture_lifetime_tracker.hpp"

#include <algorithm>
#include <limits>

namespace mhp3rd::native {
namespace {

constexpr std::uint32_t kFactoryReturn = 0x088BD07Cu;
constexpr std::uint32_t kCommandReturn = 0x088B03E4u;
constexpr std::uint32_t kBuilderReturn = 0x088B0400u;
constexpr std::uint32_t kProvider = 0x088B7DE0u;
constexpr std::uint32_t kCaller = 0x088B0114u;
constexpr std::uint32_t kCommandStateOffset = 0x13F0u;

[[nodiscard]] bool add_raw(std::uint32_t base, std::uint32_t offset,
                           std::uint32_t &result) noexcept {
    const auto sum = std::uint64_t{base} + offset;
    if (sum > std::numeric_limits<std::uint32_t>::max()) return false;
    result = static_cast<std::uint32_t>(sum);
    return true;
}

[[nodiscard]] bool same_execution(psprecomp::RuntimeExecutionContextToken token) noexcept {
    return psprecomp::runtime_execution_context_matches(token);
}

[[nodiscard]] bool same_manager(std::uint32_t observed,
                                std::uint32_t configured) noexcept {
    return observed != 0u && configured != 0u &&
           psprecomp::GuestMemory::canonical(observed) ==
           psprecomp::GuestMemory::canonical(configured);
}

[[nodiscard]] bool complete_code(const resources::SourceCodeIdentity &code) noexcept {
    return code.module != 0u && code.epoch != 0u && code.factory != 0u &&
           code.caller != 0u && code.provider != 0u &&
           code.copy_worker != 0u && code.transform_worker != 0u;
}

} // namespace

TextureLifetimeTracker::TextureLifetimeTracker(
    psprecomp::Runtime &runtime, resources::SourceAuthority &authority,
    TextureLifetimeTrackerConfig config) noexcept
    : runtime_(&runtime), authority_(&authority), config_(config) {
    if (config.owner_allocator == 0u || config.command_allocator == 0u ||
        config.max_command_blocks == 0u || config.max_command_blocks > 4096u ||
        config.current_code == nullptr || !complete_code(config.code) ||
        authority.failure() != resources::AuthorityError::None)
        fail(TextureLifetimeTrackerError::InvalidConfig, true);
}

void TextureLifetimeTracker::fail(TextureLifetimeTrackerError error,
                                  bool loss) noexcept {
    if (error_ != TextureLifetimeTrackerError::None) return;
    error_ = error;
    if (stats_.errors != std::numeric_limits<std::uint64_t>::max()) ++stats_.errors;
    if (loss) {
        if (stats_.losses != std::numeric_limits<std::uint64_t>::max()) ++stats_.losses;
        try { (void)authority_->observer_lost(); } catch (...) {}
    }
    for (auto &frame : frames_) frame = Frame{};
    stats_.active_frames = 0u;
}

bool TextureLifetimeTracker::count(std::uint64_t &counter) noexcept {
    if (counter == std::numeric_limits<std::uint64_t>::max()) {
        fail(TextureLifetimeTrackerError::CounterExhausted, true);
        return false;
    }
    ++counter;
    return true;
}

bool TextureLifetimeTracker::code_ok(const psprecomp::Runtime &runtime,
                                     const psprecomp::AllegrexContext &context) noexcept {
    if (error_ != TextureLifetimeTrackerError::None) return false;
    if (authority_->failure() != resources::AuthorityError::None) {
        fail(TextureLifetimeTrackerError::AuthorityRejected, true);
        return false;
    }
    if (!config_.current_code(config_.current_code_user, runtime, context, config_.code)) {
        fail(TextureLifetimeTrackerError::CodeChanged, true);
        return false;
    }
    return true;
}

bool TextureLifetimeTracker::valid_ram(const psprecomp::Runtime &runtime,
                                       std::uint32_t raw,
                                       std::uint32_t bytes) const noexcept {
    if (bytes == 0u || std::uint64_t{raw} + bytes >
                           std::numeric_limits<std::uint32_t>::max()) return false;
    const auto physical = std::uint64_t{psprecomp::GuestMemory::canonical(raw)};
    const auto base = std::uint64_t{psprecomp::GuestMemory::kPhysicalBase};
    if (physical < base || physical + bytes > base + runtime.memory().size()) return false;
    return runtime.memory().contains(raw, bytes);
}

bool TextureLifetimeTracker::read_word(const psprecomp::Runtime &runtime,
                                       std::uint32_t raw,
                                       std::uint32_t &value) const noexcept {
    if (!valid_ram(runtime, raw, 4u)) return false;
    const auto *bytes = runtime.memory().raw_pointer(raw, 4u);
    if (bytes == nullptr) return false;
    value = std::uint32_t{bytes[0]} | (std::uint32_t{bytes[1]} << 8u) |
            (std::uint32_t{bytes[2]} << 16u) | (std::uint32_t{bytes[3]} << 24u);
    return true;
}

TextureLifetimeTracker::Frame *TextureLifetimeTracker::find_frame(
    FrameKind kind, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) noexcept {
    for (auto &frame : frames_) {
        if (frame.kind == kind && frame.runtime == &runtime &&
            frame.context == &context && frame.sp == context.gpr[29])
            return &frame;
    }
    return nullptr;
}

TextureLifetimeTracker::Frame *TextureLifetimeTracker::new_frame(
    FrameKind kind, const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) noexcept {
    if (find_frame(kind, runtime, context) != nullptr) {
        fail(TextureLifetimeTrackerError::ConflictingPhase, true);
        return nullptr;
    }
    for (auto &frame : frames_) {
        if (frame.kind != FrameKind::Empty) continue;
        frame = Frame{};
        frame.kind = kind;
        frame.runtime = &runtime;
        frame.context = &context;
        frame.execution = psprecomp::capture_runtime_execution_context();
        frame.sp = context.gpr[29];
        ++stats_.active_frames;
        return &frame;
    }
    fail(TextureLifetimeTrackerError::NoCapacity, true);
    return nullptr;
}

void TextureLifetimeTracker::drop_frame(Frame &frame) noexcept {
    if (frame.kind == FrameKind::Empty) return;
    frame = Frame{};
    --stats_.active_frames;
}

TextureLifetimeTracker::Lease *TextureLifetimeTracker::find_lease(
    LeaseKind kind, std::uint32_t raw) noexcept {
    for (auto &lease : leases_) {
        if (lease.kind == kind &&
            psprecomp::GuestMemory::canonical(lease.raw) ==
                psprecomp::GuestMemory::canonical(raw)) return &lease;
    }
    return nullptr;
}

const TextureLifetimeTracker::Lease *TextureLifetimeTracker::find_lease(
    LeaseKind kind, std::uint32_t raw) const noexcept {
    for (const auto &lease : leases_) {
        if (lease.kind == kind &&
            psprecomp::GuestMemory::canonical(lease.raw) ==
                psprecomp::GuestMemory::canonical(raw)) return &lease;
    }
    return nullptr;
}

TextureLifetimeTracker::Lease *TextureLifetimeTracker::new_lease() noexcept {
    for (auto &lease : leases_) if (lease.kind == LeaseKind::Empty) return &lease;
    fail(TextureLifetimeTrackerError::NoCapacity, true);
    return nullptr;
}

void TextureLifetimeTracker::forget_frames(resources::AuthorityToken token) noexcept {
    for (auto &frame : frames_) {
        if (frame.kind != FrameKind::Empty &&
            (frame.owner == token || frame.command == token)) drop_frame(frame);
    }
}

void TextureLifetimeTracker::release_lease(Lease &lease) {
    if (lease.kind == LeaseKind::Empty) return;
    const auto kind = lease.kind;
    const auto token = lease.token;
    const auto event = kind == LeaseKind::Owner
        ? authority_->release_owner(token) : authority_->release_command(token);
    forget_frames(token);
    lease = Lease{};
    if (kind == LeaseKind::Owner) {
        --stats_.live_owners;
        if (!count(stats_.owners_released)) return;
    } else {
        --stats_.live_commands;
        if (!count(stats_.commands_released)) return;
    }
    if (!event.ok()) fail(TextureLifetimeTrackerError::AuthorityRejected, true);
}

void TextureLifetimeTracker::observe(const psprecomp::Runtime &runtime,
                                     const psprecomp::AllegrexContext &context,
                                     TextureLifetimeCheckpoint checkpoint) noexcept {
    if (error_ != TextureLifetimeTrackerError::None) return;
    if (&runtime != runtime_) {
        fail(TextureLifetimeTrackerError::InvalidObservation, true);
        return;
    }
    if (!count(stats_.checkpoints)) return;
    try { observe_impl(runtime, context, checkpoint); }
    catch (...) { fail(TextureLifetimeTrackerError::ObservationLost, true); }
}

void TextureLifetimeTracker::observe_impl(const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureLifetimeCheckpoint checkpoint) {
    const auto &gpr = context.gpr;
    switch (checkpoint) {
    case TextureLifetimeCheckpoint::ReverseAllocate: {
        if (gpr[31] != kFactoryReturn) return;
        if (!code_ok(runtime, context)) return;
        if (!same_manager(gpr[4], config_.owner_allocator) ||
            gpr[5] != resources::SourceAuthority::kOwnerBytes || gpr[6] != 16u) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        auto *frame = new_frame(FrameKind::Factory, runtime, context);
        if (frame == nullptr) return;
        frame->phase = FramePhase::AllocationResult;
        frame->owner_manager = config_.owner_allocator;
        break;
    }
    case TextureLifetimeCheckpoint::FactoryAllocationResult: {
        auto *frame = find_frame(FrameKind::Factory, runtime, context);
        if (frame == nullptr || frame->phase != FramePhase::AllocationResult ||
            !same_execution(frame->execution)) {
            fail(TextureLifetimeTrackerError::MissingPhase, true);
            return;
        }
        if (!code_ok(runtime, context)) return;
        if (gpr[2] == 0u) { drop_frame(*frame); return; }
        if (!valid_ram(runtime, gpr[2], resources::SourceAuthority::kOwnerBytes) ||
            (psprecomp::GuestMemory::canonical(gpr[2]) & 15u) != 0u) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        frame->raw_owner_allocation = gpr[2];
        frame->phase = FramePhase::ConstructorCall;
        break;
    }
    case TextureLifetimeCheckpoint::FactoryConstructorCall: {
        auto *frame = find_frame(FrameKind::Factory, runtime, context);
        if (frame == nullptr || frame->phase != FramePhase::ConstructorCall ||
            !same_execution(frame->execution)) {
            fail(TextureLifetimeTrackerError::MissingPhase, true);
            return;
        }
        if (!code_ok(runtime, context)) return;
        if (gpr[4] != frame->raw_owner_allocation) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        frame->phase = FramePhase::ConstructorResult;
        break;
    }
    case TextureLifetimeCheckpoint::FactoryConstructorResult: {
        auto *frame = find_frame(FrameKind::Factory, runtime, context);
        if (frame == nullptr || frame->phase != FramePhase::ConstructorResult ||
            !same_execution(frame->execution)) {
            fail(TextureLifetimeTrackerError::MissingPhase, true);
            return;
        }
        if (!code_ok(runtime, context)) return;
        std::uint32_t vtable{}, slot{};
        const bool checked = add_raw(frame->raw_owner_allocation,
            resources::SourceAuthority::kSlotOffset, slot) &&
            valid_ram(runtime, slot, resources::SourceAuthority::kSlotBytes) &&
            read_word(runtime, frame->raw_owner_allocation, vtable);
        if (!checked || gpr[2] != 1u ||
            vtable != resources::SourceAuthority::kLobbyVtable) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        const auto *bytes = runtime.memory().raw_pointer(
            slot, resources::SourceAuthority::kSlotBytes);
        if (bytes == nullptr ||
            !std::all_of(bytes, bytes + resources::SourceAuthority::kSlotBytes,
                         [](std::uint8_t byte) { return byte == 0u; })) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        auto *lease = new_lease();
        if (lease == nullptr) return;
        const auto issue = authority_->construct_owner(frame->raw_owner_allocation,
            resources::SourceAuthority::kOwnerBytes, frame->owner_manager,
            vtable, config_.code);
        if (!issue.event.ok()) {
            fail(TextureLifetimeTrackerError::AuthorityRejected, true);
            return;
        }
        *lease = Lease{LeaseKind::Owner, issue.token, frame->raw_owner_allocation,
                       resources::SourceAuthority::kOwnerBytes, frame->owner_manager};
        ++stats_.live_owners;
        if (!count(stats_.owners_constructed)) return;
        drop_frame(*frame);
        break;
    }
    case TextureLifetimeCheckpoint::CallerTail: {
        auto *lease = find_lease(LeaseKind::Owner, gpr[30]);
        if (lease == nullptr) return;
        if (!code_ok(runtime, context)) return;
        std::uint32_t vtable{}, provider_address{}, caller_address{};
        std::uint32_t provider_slot{}, caller_slot{};
        if (!read_word(runtime, gpr[30], vtable) ||
            vtable != resources::SourceAuthority::kLobbyVtable ||
            !add_raw(vtable, 0x78u, provider_slot) ||
            !add_raw(vtable, 0xB4u, caller_slot) ||
            !read_word(runtime, provider_slot, provider_address) ||
            !read_word(runtime, caller_slot, caller_address) ||
            provider_address != kProvider || caller_address != kCaller) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        auto *frame = new_frame(FrameKind::Caller, runtime, context);
        if (frame == nullptr) return;
        frame->phase = FramePhase::ProviderResult;
        frame->owner = lease->token;
        frame->owner_manager = lease->manager;
        frame->command_manager = config_.command_allocator;
        frame->raw_owner = gpr[30];
        frame->raw_owner_allocation = lease->raw;
        frame->eligible = true;
        break;
    }
    case TextureLifetimeCheckpoint::ProviderResult: {
        auto *frame = find_frame(FrameKind::Caller, runtime, context);
        if (frame == nullptr) return;
        if (frame->phase != FramePhase::ProviderResult ||
            !same_execution(frame->execution)) {
            fail(TextureLifetimeTrackerError::ConflictingPhase, true);
            return;
        }
        if (!code_ok(runtime, context)) return;
        std::uint32_t expected{};
        if (!add_raw(frame->raw_owner, resources::SourceAuthority::kSlotOffset,
                     expected) || gpr[2] != expected ||
            !valid_ram(runtime, expected, resources::SourceAuthority::kSlotBytes)) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        frame->raw_root = gpr[2];
        frame->phase = FramePhase::ChildResult;
        break;
    }
    case TextureLifetimeCheckpoint::ChildResult: {
        auto *frame = find_frame(FrameKind::Caller, runtime, context);
        if (frame == nullptr) return;
        if (frame->phase != FramePhase::ChildResult ||
            !same_execution(frame->execution)) {
            fail(TextureLifetimeTrackerError::ConflictingPhase, true);
            return;
        }
        if (!code_ok(runtime, context)) return;
        if (gpr[2] == 0u || std::uint64_t{gpr[2]} < frame->raw_root ||
            std::uint64_t{gpr[2]} + 12u >
                std::uint64_t{frame->raw_root} + resources::SourceAuthority::kSlotBytes ||
            !valid_ram(runtime, gpr[2], 12u)) {
            if (!count(stats_.ineligible)) return;
            drop_frame(*frame);
            return;
        }
        frame->raw_child = gpr[2];
        frame->phase = FramePhase::ForwardAllocate;
        break;
    }
    case TextureLifetimeCheckpoint::ForwardAllocate: {
        if (gpr[31] != kCommandReturn) return;
        auto *frame = find_frame(FrameKind::Caller, runtime, context);
        if (frame == nullptr) return;
        if (frame->phase != FramePhase::ForwardAllocate ||
            !same_execution(frame->execution)) {
            fail(TextureLifetimeTrackerError::ConflictingPhase, true);
            return;
        }
        if (!code_ok(runtime, context)) return;
        std::uint32_t count_value{};
        if (!add_raw(frame->raw_child, 8u, count_value) ||
            !read_word(runtime, count_value, count_value)) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        const auto signed_count = static_cast<std::int32_t>(count_value);
        if (signed_count <= 0 || count_value > config_.max_command_blocks ||
            !same_manager(gpr[4], config_.command_allocator)) {
            frame->eligible = false;
            if (!count(stats_.ineligible)) return;
        } else {
            const auto expected_bytes = std::uint64_t{count_value} * 36u;
            if (expected_bytes > std::numeric_limits<std::uint32_t>::max() ||
                gpr[5] != expected_bytes || gpr[6] != 16u) {
                fail(TextureLifetimeTrackerError::InvalidObservation, true);
                return;
            }
            frame->requested_bytes = gpr[5];
        }
        frame->phase = FramePhase::CommandResult;
        break;
    }
    case TextureLifetimeCheckpoint::CommandAllocationResult: {
        auto *frame = find_frame(FrameKind::Caller, runtime, context);
        if (frame == nullptr) return;
        if (frame->phase != FramePhase::CommandResult ||
            !same_execution(frame->execution)) {
            fail(TextureLifetimeTrackerError::ConflictingPhase, true);
            return;
        }
        if (!code_ok(runtime, context)) return;
        if (!frame->eligible || gpr[2] == 0u) {
            frame->eligible = false;
            if (!count(stats_.ineligible)) return;
            frame->phase = FramePhase::BuilderCall;
            return;
        }
        if (!valid_ram(runtime, gpr[2], frame->requested_bytes) ||
            (psprecomp::GuestMemory::canonical(gpr[2]) & 15u) != 0u) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        auto *lease = new_lease();
        if (lease == nullptr) return;
        const auto issue = authority_->allocate_command(gpr[2],
            frame->requested_bytes, frame->command_manager);
        if (!issue.event.ok()) {
            fail(TextureLifetimeTrackerError::AuthorityRejected, true);
            return;
        }
        *lease = Lease{LeaseKind::Command, issue.token, gpr[2],
                       frame->requested_bytes, frame->command_manager};
        ++stats_.live_commands;
        if (!count(stats_.commands_allocated)) return;
        frame->command = issue.token;
        frame->raw_command = gpr[2];
        frame->phase = FramePhase::BuilderCall;
        break;
    }
    case TextureLifetimeCheckpoint::BuilderCall: {
        auto *frame = find_frame(FrameKind::Caller, runtime, context);
        if (frame == nullptr) return;
        if (frame->phase != FramePhase::BuilderCall ||
            !same_execution(frame->execution)) {
            fail(TextureLifetimeTrackerError::ConflictingPhase, true);
            return;
        }
        if (!code_ok(runtime, context)) return;
        if (!frame->eligible) { drop_frame(*frame); return; }
        std::uint32_t state{};
        if (!add_raw(frame->raw_owner, kCommandStateOffset, state) ||
            gpr[30] != frame->raw_owner || gpr[16] != frame->raw_child ||
            gpr[4] != state || gpr[5] != frame->raw_command ||
            gpr[6] != frame->raw_child || gpr[7] != 0u || gpr[8] != 0u ||
            find_lease(LeaseKind::Owner, frame->raw_owner) == nullptr ||
            find_lease(LeaseKind::Command, frame->raw_command) == nullptr) {
            fail(TextureLifetimeTrackerError::InvalidObservation, true);
            return;
        }
        frame->phase = FramePhase::BuilderEntry;
        if (!count(stats_.tickets_armed)) return;
        break;
    }
    case TextureLifetimeCheckpoint::Free: {
        if (gpr[5] == 0u) return;
        for (auto &lease : leases_) {
            if (lease.kind == LeaseKind::Empty) continue;
            const auto base = std::uint64_t{
                psprecomp::GuestMemory::canonical(lease.raw)};
            const auto pointer = std::uint64_t{
                psprecomp::GuestMemory::canonical(gpr[5])};
            if (pointer > base && pointer < base + lease.bytes) {
                fail(TextureLifetimeTrackerError::InvalidObservation, true);
                return;
            }
            if (pointer != base) continue;
            if (!same_manager(gpr[4], lease.manager)) {
                fail(TextureLifetimeTrackerError::InvalidObservation, true);
                return;
            }
            if (!code_ok(runtime, context)) return;
            release_lease(lease);
            if (error_ != TextureLifetimeTrackerError::None) return;
        }
        for (auto &frame : frames_) {
            if (frame.kind == FrameKind::Factory &&
                frame.raw_owner_allocation != 0u &&
                psprecomp::GuestMemory::canonical(frame.raw_owner_allocation) ==
                    psprecomp::GuestMemory::canonical(gpr[5])) drop_frame(frame);
        }
        break;
    }
    case TextureLifetimeCheckpoint::HeapReset:
    case TextureLifetimeCheckpoint::HeapInit: {
        bool relevant = false;
        for (const auto &lease : leases_)
            if (lease.kind != LeaseKind::Empty &&
                same_manager(lease.manager, gpr[4])) relevant = true;
        for (const auto &frame : frames_)
            if (frame.kind != FrameKind::Empty &&
                (same_manager(frame.owner_manager, gpr[4]) ||
                 same_manager(frame.command_manager, gpr[4])))
                relevant = true;
        if (!relevant) return;
        if (!code_ok(runtime, context)) return;
        for (auto &lease : leases_) {
            if (lease.kind != LeaseKind::Empty &&
                same_manager(lease.manager, gpr[4])) {
                release_lease(lease);
                if (error_ != TextureLifetimeTrackerError::None) return;
            }
        }
        for (auto &frame : frames_)
            if (frame.kind != FrameKind::Empty &&
                (same_manager(frame.owner_manager, gpr[4]) ||
                 same_manager(frame.command_manager, gpr[4])))
                drop_frame(frame);
        break;
    }
    case TextureLifetimeCheckpoint::OwnerReset: {
        auto *lease = find_lease(LeaseKind::Owner, gpr[4]);
        if (lease == nullptr) return;
        if (!code_ok(runtime, context)) return;
        const auto event = authority_->reset_owner(lease->token);
        if (!event.ok()) {
            fail(TextureLifetimeTrackerError::AuthorityRejected, true);
            return;
        }
        forget_frames(lease->token);
        break;
    }
    default:
        fail(TextureLifetimeTrackerError::InvalidObservation, true);
        break;
    }
}

std::optional<TextureBuilderTicket> TextureLifetimeTracker::consume_builder_ticket(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context) noexcept {
    if (error_ != TextureLifetimeTrackerError::None) return std::nullopt;
    if (&runtime != runtime_) {
        fail(TextureLifetimeTrackerError::InvalidObservation, true);
        return std::nullopt;
    }
    auto *frame = find_frame(FrameKind::Caller, runtime, context);
    if (frame == nullptr) return std::nullopt;
    if (frame->phase != FramePhase::BuilderEntry ||
        !same_execution(frame->execution) || !code_ok(runtime, context)) {
        fail(TextureLifetimeTrackerError::ConflictingPhase, true);
        return std::nullopt;
    }
    std::uint32_t state{};
    const auto &gpr = context.gpr;
    if (!add_raw(frame->raw_owner, kCommandStateOffset, state) ||
        gpr[30] != frame->raw_owner || gpr[16] != frame->raw_child ||
        gpr[4] != state || gpr[5] != frame->raw_command ||
        gpr[6] != frame->raw_child || gpr[7] != 0u || gpr[8] != 0u ||
        gpr[9] != 0u || gpr[31] != kBuilderReturn) {
        fail(TextureLifetimeTrackerError::InvalidObservation, true);
        return std::nullopt;
    }
    const auto *owner = find_lease(LeaseKind::Owner, frame->raw_owner);
    const auto *command = find_lease(LeaseKind::Command, frame->raw_command);
    if (owner == nullptr || command == nullptr || owner->token != frame->owner ||
        command->token != frame->command ||
        command->bytes != frame->requested_bytes) {
        fail(TextureLifetimeTrackerError::InvalidObservation, true);
        return std::nullopt;
    }
    TextureBuilderTicket ticket{frame->owner, frame->command,
        frame->raw_owner, frame->raw_owner_allocation, frame->raw_child,
        frame->raw_command, frame->requested_bytes, &runtime, &context,
        frame->execution, frame->sp, config_.code};
    if (!count(stats_.tickets_consumed)) return std::nullopt;
    drop_frame(*frame);
    return ticket;
}

std::optional<resources::AuthorityToken> TextureLifetimeTracker::owner_token(
    std::uint32_t raw) const noexcept {
    if (error_ != TextureLifetimeTrackerError::None) return std::nullopt;
    const auto *lease = find_lease(LeaseKind::Owner, raw);
    if (lease == nullptr) return std::nullopt;
    return lease->token;
}

std::optional<resources::AuthorityToken> TextureLifetimeTracker::command_token(
    std::uint32_t raw) const noexcept {
    if (error_ != TextureLifetimeTrackerError::None) return std::nullopt;
    const auto *lease = find_lease(LeaseKind::Command, raw);
    if (lease == nullptr) return std::nullopt;
    return lease->token;
}

TextureLifetimeTrackerStats TextureLifetimeTracker::stats() const noexcept {
    return stats_;
}

} // namespace mhp3rd::native
