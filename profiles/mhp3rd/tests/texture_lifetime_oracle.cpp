#include "native/bridge_contracts.hpp"
#include "native/texture_command_dispatch.hpp"
#include "native/texture_lifetime_tracker.hpp"
#include "native/texture_transfer_tracker.hpp"
#include "overlay_module.hpp"
#include "psprecomp/elf32.hpp"
#include "recomp_units.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace psprecomp;
using namespace mhp3rd::native;
constexpr std::uint32_t end_pc = 0x08001000u, container = 0x08200000u;
constexpr std::uint32_t owner_manager = container + 4u, command_manager = 0x08202000u;
constexpr std::uint32_t owner_heap = 0x09200000u, command_heap = 0x09000000u;
constexpr std::uint32_t sp = 0x08400800u, overlay_base = 0x0A05E600u;
constexpr std::uint32_t ctor = 0x0A0E7460u;
// Keep the 0x20000-byte worker scratch range disjoint from the allocator
// regions used by the owner and command fixtures below.
constexpr std::uint32_t transfer_manager = 0x08600000u;
constexpr std::uint32_t kQueueConsumerOffset = 0x108Cu;
constexpr std::uint32_t kQueueStateOffset = 0x1094u;
constexpr std::uint32_t kQueueFdOffset = 0x1098u;
constexpr std::uint32_t kQueueRecordOffset = 0x8Cu;
constexpr std::uint32_t kReadScratchOffset = 0x98C0u;
constexpr std::uint32_t kReadScratchBytes = 0x20000u;
constexpr std::uint32_t kWorkerState8 = 8u;
constexpr mhp3rd::resources::SourceCodeIdentity supported_code{
    0xF6300296C8D954E5ull, 1u, 0x088BD058u, 0x088B0114u, 0x088B7DE0u,
    0x088652C4u, 0x08865378u};
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t at) {
    require(at + 4u <= bytes.size(), "Header field outside image");
    return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1u]) << 8u) |
        (std::uint32_t(bytes[at + 2u]) << 16u) | (std::uint32_t(bytes[at + 3u]) << 24u);
}
struct Library {
    void *handle{};
    explicit Library(const char *path) : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
        require(handle != nullptr, "Could not load original lobby module");
    }
    ~Library() { if (handle) dlclose(handle); }
};
struct Event {
    TextureLifetimeCheckpoint checkpoint{};
    AllegrexContext cpu{};
};
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
struct TransferEvent {
    TextureTransferCheckpoint checkpoint{};
    AllegrexContext cpu{};
    std::array<std::uint8_t, 32> descriptor{};
    bool has_descriptor{};
};
struct DescriptorOverride {
    std::uint32_t destination{}, offset{};
    std::optional<std::uint32_t> chunk_bytes, total_bytes;
    std::optional<std::uint8_t> deobfuscate;
};
#endif
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
struct ReadEvent {
    TextureReadCheckpoint checkpoint{};
    AllegrexContext cpu{};
};
struct ReadDescriptorOverride {
    TextureReadCheckpoint checkpoint{};
    std::uint32_t byte_offset{};
    std::uint8_t value{};
    bool flip{};
};
struct ReadRegisterOverride {
    TextureReadCheckpoint checkpoint{};
    std::uint32_t reg{}, value{};
    bool flip_alias{};
};
enum class ReadInterleave : std::uint8_t {
    None, OwnerReset, OwnerFreeReuse, SameIdReload, SyntheticDescriptorCommitSameBytes
};
struct ReadScenario {
    std::string name;
    std::vector<std::int32_t> results;
    std::uint32_t requested_bytes{32u};
    bool selected{true};
    bool uncached_manager{};
    ReadInterleave interleave{ReadInterleave::None};
    std::optional<ReadDescriptorOverride> descriptor_override;
    std::optional<ReadRegisterOverride> register_override;
    std::optional<TextureReadCheckpoint> drop, duplicate, wrong_context, wrong_runtime,
        switch_thread, lifecycle_at;
    std::uint32_t drop_occurrence{};
    std::optional<TextureLifetimeCheckpoint> lifecycle_event;
    std::uint32_t fault_register{};
    TextureTransferTrackerError expected_error{TextureTransferTrackerError::None};
    TextureTransferTrackerError expected_alternate_error{TextureTransferTrackerError::None};
    bool expect_incomplete{};
    bool expect_retry_route_rejected{};
    bool expect_stale_attempt_rejected{};
    bool reuse_selected_history{};
    bool synthetic_partial_descriptor_overlap{};
    std::uint32_t max_read_attempts{TextureTransferTracker::kMaxReadAttempts};
    std::uint64_t read_attempt_serial_limit{0xFFFFFFFFFFFFFFFFull};
};
thread_local std::size_t read_prefix_stop_after{};
thread_local std::size_t read_prefix_read_invocations{};
thread_local std::size_t read_prefix_stop_calls{};
#endif
struct Observations {
    std::array<Event, 256> events{};
    std::size_t count{};
    bool overflow{};
    unsigned builders{};
    TextureLifetimeTracker *tracker{};
    std::optional<TextureBuilderTicket> last_ticket;
    std::optional<TextureLifetimeCheckpoint> drop;
    bool replay_accepted{};
    bool switch_at_builder{};
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
    std::array<TransferEvent, 128> transfer_events{};
    std::size_t transfer_count{};
    std::size_t transfer_failure_event{128u};
    bool transfer_overflow{};
    TextureTransferTracker *transfer_tracker{};
    Runtime *other_runtime{};
    std::optional<TextureTransferCheckpoint> drop_transfer;
    std::optional<TextureTransferCheckpoint> duplicate_transfer;
    std::optional<TextureTransferCheckpoint> wrong_runtime_transfer;
    std::optional<TextureTransferCheckpoint> wrong_context_transfer;
    std::optional<TextureTransferCheckpoint> wrong_manager_transfer;
    std::uint32_t wrong_manager_value{};
    std::optional<TextureTransferCheckpoint> switch_thread_transfer;
    std::optional<DescriptorOverride> descriptor_override;
#endif
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
    std::array<ReadEvent, 256> read_events{};
    std::size_t read_count{};
    bool read_overflow{};
    TextureTransferTracker *read_tracker{};
    std::optional<TextureReadCheckpoint> drop_read;
    std::uint32_t drop_read_occurrence{}, drop_read_occurrences{};
    std::optional<TextureReadCheckpoint> duplicate_read;
    std::optional<TextureReadCheckpoint> wrong_runtime_read;
    std::optional<TextureReadCheckpoint> wrong_context_read;
    std::optional<TextureReadCheckpoint> switch_thread_read;
    std::optional<ReadDescriptorOverride> descriptor_override_read;
    std::optional<ReadRegisterOverride> register_override_read;
    bool synthetic_partial_descriptor_overlap_read{};
    ReadInterleave read_interleave{ReadInterleave::None};
    bool read_interleave_done{};
    std::uint32_t read_owner{};
    std::optional<TextureReadCheckpoint> lifecycle_at_read;
    std::optional<TextureLifetimeCheckpoint> lifecycle_event;
    std::uint32_t invalidate_owner{};
#endif
    static void lifetime(void *data, const Runtime &runtime, const AllegrexContext &ctx,
                         TextureLifetimeCheckpoint checkpoint) noexcept {
        auto &self = *static_cast<Observations *>(data);
        if (self.count == self.events.size()) { self.overflow = true; return; }
        self.events[self.count++] = {checkpoint, ctx};
        if (self.tracker && self.drop != checkpoint) self.tracker->observe(runtime, ctx, checkpoint);
        if (self.switch_at_builder && checkpoint == TextureLifetimeCheckpoint::BuilderCall) {
            const auto uid = runtime_thread_uid();
            set_runtime_thread_identity(uid == 3001 ? 3002 : 3001, "lifetime fault fixture");
            set_runtime_thread_identity(uid, "lifetime fixture restored");
        }
    }
    static bool entry(void *data, Runtime &runtime, AllegrexContext &ctx) noexcept {
        auto &self = *static_cast<Observations *>(data);
        ++self.builders;
        if (self.tracker) {
            self.last_ticket = self.tracker->consume_builder_ticket(runtime, ctx);
            if (self.last_ticket && self.tracker->consume_builder_ticket(runtime, ctx)) self.replay_accepted = true;
        }
        return false;
    }
    static void returned(void *, Runtime &, AllegrexContext &, std::uint32_t) noexcept {}
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
    static void transfer(void *data, const Runtime &runtime, const AllegrexContext &ctx,
                         TextureTransferCheckpoint checkpoint) noexcept {
        auto &self = *static_cast<Observations *>(data);
        if (self.transfer_count == self.transfer_events.size()) {
            self.transfer_overflow = true;
            return;
        }
        auto &event = self.transfer_events[self.transfer_count++];
        event.checkpoint = checkpoint;
        event.cpu = ctx;
        if (checkpoint == TextureTransferCheckpoint::DescriptorCommit) {
            const auto *bytes = runtime.memory().raw_pointer(ctx.gpr[6], 32u);
            if (bytes != nullptr) {
                std::copy_n(bytes, event.descriptor.size(), event.descriptor.begin());
                event.has_descriptor = true;
            }
        }
        if (!self.transfer_tracker || self.drop_transfer == checkpoint) return;
        AllegrexContext altered{};
        const AllegrexContext *observed = &ctx;
        if (self.wrong_context_transfer == checkpoint) {
            altered = ctx;
            observed = &altered;
        }
        const Runtime *observed_runtime = &runtime;
        if (self.wrong_runtime_transfer == checkpoint && self.other_runtime)
            observed_runtime = self.other_runtime;
        if (self.wrong_context_transfer == checkpoint) altered.gpr[29] += 4u;
        const auto thread = runtime_thread_uid();
        if (self.switch_thread_transfer == checkpoint) {
            set_runtime_thread_identity(thread == 3001 ? 3002 : 3001, "transfer fault fixture");
            set_runtime_thread_identity(thread, "transfer fixture restored");
        }
        std::optional<std::array<std::uint32_t, 4>> old_descriptor_words;
        std::optional<std::uint8_t> old_deobfuscate;
        std::optional<std::uint32_t> old_manager;
        if (self.wrong_manager_transfer == checkpoint) {
            auto &mutable_context = const_cast<AllegrexContext &>(ctx);
            old_manager = mutable_context.gpr[4];
            mutable_context.gpr[4] = self.wrong_manager_value;
        }
        if (checkpoint == TextureTransferCheckpoint::DescriptorCommit &&
            self.descriptor_override.has_value()) {
            // Test-only fault injection at the exact read-only callback edge:
            // corrupt two descriptor words, let the tracker classify them,
            // then restore them before original guest code resumes.
            auto &memory = const_cast<Runtime &>(runtime).memory();
            old_descriptor_words = std::array{
                memory.load32(ctx.gpr[6] + 4u), memory.load32(ctx.gpr[6] + 8u),
                memory.load32(ctx.gpr[6] + 12u), memory.load32(ctx.gpr[6] + 16u)};
            old_deobfuscate = memory.load8(ctx.gpr[6] + 27u);
            memory.store32(ctx.gpr[6] + 4u, self.descriptor_override->destination);
            if (self.descriptor_override->chunk_bytes.has_value())
                memory.store32(ctx.gpr[6] + 8u, *self.descriptor_override->chunk_bytes);
            memory.store32(ctx.gpr[6] + 12u, self.descriptor_override->offset);
            if (self.descriptor_override->total_bytes.has_value())
                memory.store32(ctx.gpr[6] + 16u, *self.descriptor_override->total_bytes);
            if (self.descriptor_override->deobfuscate.has_value())
                memory.store8(ctx.gpr[6] + 27u, *self.descriptor_override->deobfuscate);
        }
        self.transfer_tracker->observe(*observed_runtime, *observed, checkpoint);
        if (old_descriptor_words.has_value()) {
            auto &memory = const_cast<Runtime &>(runtime).memory();
            memory.store32(ctx.gpr[6] + 4u, (*old_descriptor_words)[0]);
            memory.store32(ctx.gpr[6] + 8u, (*old_descriptor_words)[1]);
            memory.store32(ctx.gpr[6] + 12u, (*old_descriptor_words)[2]);
            memory.store32(ctx.gpr[6] + 16u, (*old_descriptor_words)[3]);
            memory.store8(ctx.gpr[6] + 27u, *old_deobfuscate);
            self.descriptor_override.reset();
        }
        if (old_manager.has_value())
            const_cast<AllegrexContext &>(ctx).gpr[4] = *old_manager;
        if (self.transfer_tracker->error() != TextureTransferTrackerError::None &&
            self.transfer_failure_event == self.transfer_events.size())
            self.transfer_failure_event = self.transfer_count - 1u;
        if (self.duplicate_transfer == checkpoint)
            self.transfer_tracker->observe(runtime, ctx, checkpoint);
        if (self.transfer_tracker->error() != TextureTransferTrackerError::None &&
            self.transfer_failure_event == self.transfer_events.size())
            self.transfer_failure_event = self.transfer_count - 1u;
    }
#endif
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
    static void read(void *data, const Runtime &runtime, const AllegrexContext &ctx,
                     TextureReadCheckpoint checkpoint) noexcept {
        auto &self = *static_cast<Observations *>(data);
        if (self.read_count == self.read_events.size()) { self.read_overflow = true; return; }
        self.read_events[self.read_count++] = {checkpoint, ctx};
        bool drop = false;
        if (self.drop_read == checkpoint) {
            if (self.drop_read_occurrence == 0u) drop = true;
            else {
                ++self.drop_read_occurrences;
                drop = self.drop_read_occurrences == self.drop_read_occurrence;
            }
        }
        if (!self.read_tracker || drop) return;

        std::optional<std::uint8_t> old_byte;
        std::uint32_t raw_record{};
        if (checkpoint == TextureReadCheckpoint::ReadHelperEntry) raw_record = ctx.gpr[6];
        else raw_record = ctx.gpr[18];
        std::optional<std::array<std::uint8_t, 33>> old_partial_descriptor;
        if (checkpoint == TextureReadCheckpoint::ReadHelperEntry &&
            self.synthetic_partial_descriptor_overlap_read) {
            auto &memory = const_cast<Runtime &>(runtime).memory();
            const auto *original = memory.raw_pointer(raw_record, 33u);
            auto *overlap = memory.raw_pointer(raw_record + 1u, 32u);
            if (original == nullptr || overlap == nullptr) {
                self.read_overflow = true;
                return;
            }
            std::array<std::uint8_t, 32> descriptor{};
            std::copy_n(original, old_partial_descriptor.emplace().size(),
                        old_partial_descriptor->begin());
            std::copy_n(old_partial_descriptor->begin(), descriptor.size(), descriptor.begin());
            std::copy(descriptor.begin(), descriptor.end(), overlap);
            self.synthetic_partial_descriptor_overlap_read = false;
        }
        if (self.register_override_read.has_value() &&
            self.register_override_read->checkpoint == checkpoint &&
            self.register_override_read->reg >= ctx.gpr.size()) {
            self.read_overflow = true;
            return;
        }
        if (self.descriptor_override_read.has_value() &&
            self.descriptor_override_read->checkpoint == checkpoint) {
            const auto offset = self.descriptor_override_read->byte_offset;
            if (offset >= 32u || raw_record == 0u) { self.read_overflow = true; return; }
            auto &memory = const_cast<Runtime &>(runtime).memory();
            old_byte = memory.load8(raw_record + offset);
            const auto value = self.descriptor_override_read->flip
                ? static_cast<std::uint8_t>(*old_byte ^ 1u)
                : self.descriptor_override_read->value;
            memory.store8(raw_record + offset, value);
        }

        std::optional<std::pair<std::uint32_t, std::uint32_t>> old_register;
        if (self.register_override_read.has_value() &&
            self.register_override_read->checkpoint == checkpoint) {
            const auto reg = self.register_override_read->reg;
            auto &mutable_context = const_cast<AllegrexContext &>(ctx);
            old_register = std::pair{reg, mutable_context.gpr[reg]};
            auto value = self.register_override_read->value;
            if (self.register_override_read->flip_alias) {
                const auto raw = mutable_context.gpr[reg];
                value = psprecomp::GuestMemory::canonical(raw);
                if (raw == value) value |= 0x40000000u;
            }
            mutable_context.gpr[reg] = value;
        }

        AllegrexContext altered{};
        const AllegrexContext *observed = &ctx;
        if (self.wrong_context_read == checkpoint) {
            altered = ctx;
            altered.gpr[29] += 4u;
            observed = &altered;
        }
        const Runtime *observed_runtime = &runtime;
        if (self.wrong_runtime_read == checkpoint && self.other_runtime != nullptr)
            observed_runtime = self.other_runtime;
        const auto uid = runtime_thread_uid();
        if (self.switch_thread_read == checkpoint)
            set_runtime_thread_identity(uid == 3001 ? 3002 : 3001, "read fault fixture");

        if (self.lifecycle_at_read == checkpoint && self.lifecycle_event.has_value() &&
            self.invalidate_owner != 0u) {
            auto lifecycle_context = ctx;
            if (*self.lifecycle_event == TextureLifetimeCheckpoint::OwnerReset) {
                lifecycle_context.gpr[4] = self.invalidate_owner;
                self.tracker->observe(runtime, lifecycle_context, TextureLifetimeCheckpoint::OwnerReset);
            } else if (*self.lifecycle_event == TextureLifetimeCheckpoint::Free) {
                lifecycle_context.gpr[5] = self.invalidate_owner;
                lifecycle_context.gpr[4] = owner_manager;
                self.tracker->observe(runtime, lifecycle_context, TextureLifetimeCheckpoint::Free);
            }
        }
        self.read_tracker->observe_read(*observed_runtime, *observed, checkpoint);
        if (self.switch_thread_read == checkpoint)
            set_runtime_thread_identity(uid, "read fault fixture restored");
        if (old_byte.has_value()) {
            auto &memory = const_cast<Runtime &>(runtime).memory();
            memory.store8(raw_record + self.descriptor_override_read->byte_offset, *old_byte);
            self.descriptor_override_read.reset();
        }
        if (old_partial_descriptor.has_value()) {
            auto &memory = const_cast<Runtime &>(runtime).memory();
            auto *original = memory.raw_pointer(raw_record, old_partial_descriptor->size());
            if (original == nullptr) self.read_overflow = true;
            else std::copy(old_partial_descriptor->begin(), old_partial_descriptor->end(), original);
        }
        if (old_register.has_value()) {
            const auto [reg, value] = *old_register;
            const_cast<AllegrexContext &>(ctx).gpr[reg] = value;
            self.register_override_read.reset();
        }
        if (self.duplicate_read == checkpoint)
            self.read_tracker->observe_read(runtime, ctx, checkpoint);
    }
#endif
    const Event &one(TextureLifetimeCheckpoint point, std::size_t begin) const {
        const Event *found = nullptr;
        for (std::size_t i = begin; i < count; ++i) if (events[i].checkpoint == point) {
            require(found == nullptr, "Duplicate selected lifetime checkpoint");
            found = &events[i];
        }
        require(found != nullptr, "Missing selected lifetime checkpoint");
        return *found;
    }
};

class Gate {
    Runtime actual_, reference_;
    Observations observed_;
    TextureCommandDispatch binding_;
    std::uint32_t mirror_{};
    struct CodeRegion { std::uint32_t address{}; std::vector<std::uint8_t> bytes; };
    std::vector<CodeRegion> code_;
    std::unique_ptr<mhp3rd::resources::SourceAuthority> authority_;
    std::unique_ptr<TextureLifetimeTracker> tracker_;
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
    std::unique_ptr<TextureTransferTracker> transfer_tracker_;
    std::vector<psprecomp::PspImport> imports_;
    bool allow_transfer_loss_{};
    bool force_code_invalid_{};
#endif
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
    ReadScenario read_scenario_{};
    bool read_prefix_active_{};
    bool read_interleave_applied_{};
    std::uint32_t read_fd_{};
    std::uint32_t read_manager_{}, read_owner_{};
    std::uint32_t read_requested_bytes_{};
    std::uint32_t read_destination_address_{}, read_destination_bytes_{};
    std::size_t stop_after_read_results_{};
    std::size_t actual_read_calls_{}, reference_read_calls_{};
    std::vector<std::array<std::uint32_t, 5>> actual_read_trace_, reference_read_trace_;
    std::vector<std::uint8_t> destination_before_actual_, destination_before_reference_;
#endif
    bool allow_loss_{};
    static bool code_valid(void *user, const Runtime &runtime, const AllegrexContext &,
                           const mhp3rd::resources::SourceCodeIdentity &identity) noexcept {
        const auto &self = *static_cast<const Gate *>(user);
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
        if (self.force_code_invalid_) return false;
#endif
        if (identity != supported_code || self.code_.empty()) return false;
        for (const auto &region : self.code_) {
            const auto *bytes = runtime.memory().raw_pointer(region.address, region.bytes.size());
            if (!bytes || std::memcmp(bytes, region.bytes.data(), region.bytes.size()) != 0) return false;
        }
        return true;
    }
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
    bool is_import(std::uint32_t pc) const noexcept {
        return std::any_of(imports_.begin(), imports_.end(),
            [pc](const psprecomp::PspImport &entry) { return entry.stub_address == pc; });
    }
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
    void interleave_read_state_once() {
        if (read_interleave_applied_ || read_scenario_.interleave == ReadInterleave::None) return;
        read_interleave_applied_ = true;
        switch (read_scenario_.interleave) {
        case ReadInterleave::None: break;
        case ReadInterleave::OwnerReset: {
            auto reset = context(0x088A53C8u);
            reset.gpr[4] = read_owner_;
            tracker_->observe(actual_, reset, TextureLifetimeCheckpoint::OwnerReset);
            break;
        }
        case ReadInterleave::OwnerFreeReuse: {
            const auto old_owner = read_owner_;
            const auto check_lifetime = [this](const char *phase) {
                require(tracker_->error() == TextureLifetimeTrackerError::None,
                        std::string("Read free/reuse fixture failed at ") + phase +
                            " with lifetime error " +
                            std::to_string(static_cast<int>(tracker_->error())));
            };
            auto release = context(0x08879FF0u);
            release.gpr[4] = raw(owner_manager);
            release.gpr[5] = old_owner;
            tracker_->observe(actual_, release, TextureLifetimeCheckpoint::Free);
            check_lifetime("Free");
            auto allocation = context(0x088BD058u);
            allocation.gpr[29] = raw(sp - 0x100u);
            allocation.gpr[31] = 0x088BD07Cu;
            allocation.gpr[4] = raw(owner_manager);
            allocation.gpr[5] = mhp3rd::resources::SourceAuthority::kOwnerBytes;
            allocation.gpr[6] = 16u;
            tracker_->observe(actual_, allocation, TextureLifetimeCheckpoint::ReverseAllocate);
            check_lifetime("ReverseAllocate");
            allocation.gpr[2] = old_owner;
            tracker_->observe(actual_, allocation,
                              TextureLifetimeCheckpoint::FactoryAllocationResult);
            check_lifetime("FactoryAllocationResult");
            allocation.gpr[4] = old_owner;
            tracker_->observe(actual_, allocation,
                              TextureLifetimeCheckpoint::FactoryConstructorCall);
            check_lifetime("FactoryConstructorCall");
            const auto slot = old_owner +
                mhp3rd::resources::SourceAuthority::kSlotOffset;
            actual_.memory().zero(slot,
                mhp3rd::resources::SourceAuthority::kSlotBytes);
            reference_.memory().zero(slot,
                mhp3rd::resources::SourceAuthority::kSlotBytes);
            allocation.gpr[2] = 1u;
            tracker_->observe(actual_, allocation,
                              TextureLifetimeCheckpoint::FactoryConstructorResult);
            check_lifetime("FactoryConstructorResult");
            read_owner_ = old_owner;
            require(tracker_->owner_token(read_owner_).has_value(),
                    "Read free/reuse fixture did not issue a new owner lease");
            break;
        }
        case ReadInterleave::SameIdReload: {
            auto reload = context(0x088A5470u);
            reload.gpr[4] = read_owner_;
            reload.gpr[5] = 7u;
            reload.gpr[6] = 17u;
            transfer_tracker_->observe(actual_, reload,
                TextureTransferCheckpoint::OwnerLoadEntry);
            break;
        }
        case ReadInterleave::SyntheticDescriptorCommitSameBytes:
            // Inject the production transfer-observation callbacks while an
            // attempt is active. The old bytes are unchanged; only a newly
            // observed commit may advance the descriptor generation.
            {
            TextureTransferDescriptorRecord prior{};
            require(transfer_tracker_->descriptor_record(0u, prior) &&
                    prior.current && prior.associated_load && prior.writer.instance != 0u,
                    "Active-attempt reuse has no selected descriptor generation");
            auto enqueue = context(0x08863CDCu);
            enqueue.gpr[29] = raw(sp + 0x10000u);
            enqueue.gpr[4] = raw(transfer_manager);
            enqueue.gpr[5] = 18u;
            enqueue.gpr[6] = 0x08300000u;
            enqueue.gpr[7] = 5u;
            enqueue.gpr[8] = 0u;
            enqueue.gpr[9] = 1u;
            transfer_tracker_->observe(actual_, enqueue,
                TextureTransferCheckpoint::EnqueueEntry);
            require(transfer_tracker_->error() == TextureTransferTrackerError::None,
                    "Synthetic unowned enqueue-entry callback failed");
            enqueue.gpr[29] -= 48u;
            enqueue.gpr[6] = prior.raw_descriptor;
            transfer_tracker_->observe(actual_, enqueue,
                TextureTransferCheckpoint::DescriptorCommit);
            require(transfer_tracker_->error() == TextureTransferTrackerError::None,
                    "Synthetic same-byte descriptor-commit callback failed");
            TextureTransferDescriptorRecord old{}, current{};
            require(transfer_tracker_->descriptor_record_count() == 2u &&
                    transfer_tracker_->descriptor_record(0u, old) && !old.current &&
                    transfer_tracker_->descriptor_record(1u, current) && current.current &&
                    old.generation != current.generation && old.snapshot == current.snapshot &&
                    old.writer == prior.writer && current.writer != old.writer &&
                    transfer_tracker_->stats().live_writers == 2u,
                    "Same-byte descriptor commit did not create a fresh generation");
            break;
            }
        }
    }
#endif
    void model_import(Runtime &runtime, AllegrexContext &context, bool actual_side) {
        const auto stub = context.pc;
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
        if (read_prefix_active_ && stub == 0x089656A0u) {
            const auto expected_scratch64 = std::uint64_t{read_manager_} + 0x98C0u;
            require(expected_scratch64 <= std::numeric_limits<std::uint32_t>::max(),
                    "Read fixture scratch address overflows guest RAM");
            if (psprecomp::GuestMemory::canonical(context.gpr[5]) !=
                psprecomp::GuestMemory::canonical(static_cast<std::uint32_t>(expected_scratch64))) {
                context.gpr[2] = 0u;
                context.pc = context.gpr[31];
                return;
            }
            require(context.gpr[4] == read_fd_ &&
                    context.gpr[6] == read_requested_bytes_,
                    "Actual sceIoRead fd/request differs from the captured manager and descriptor");
            if (actual_side) interleave_read_state_once();
            auto &calls = actual_side ? actual_read_calls_ : reference_read_calls_;
            require(calls < read_scenario_.results.size(),
                    "Unexpected additional sceIoRead invocation");
            if (calls == 0u && read_destination_bytes_ != 0u) {
                auto &before = actual_side ? destination_before_actual_ : destination_before_reference_;
                before.resize(read_destination_bytes_);
                runtime.memory().copy_out(read_destination_address_, before);
            }
            const auto invocation = calls;
            const auto result = read_scenario_.results[calls++];
            if (actual_side) ++read_prefix_read_invocations;
            if (result > 0 && static_cast<std::uint32_t>(result) <= context.gpr[6] &&
                runtime.memory().contains(context.gpr[5], static_cast<std::uint32_t>(result))) {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(result));
                for (std::size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::uint8_t>(0x40u + ((i + invocation) & 0x3Fu));
                runtime.memory().copy_in(context.gpr[5], bytes);
            }
            context.gpr[2] = std::bit_cast<std::uint32_t>(result);
            auto &trace = actual_side ? actual_read_trace_ : reference_read_trace_;
            trace.push_back({stub, context.gpr[4], context.gpr[5], context.gpr[6], context.gpr[2]});
            context.pc = context.gpr[31];
            return;
        }
#else
        (void)actual_side;
#endif
        context.gpr[2] = 0u;
        context.pc = context.gpr[31];
    }
    std::string transfer_diagnostic() const {
        if (observed_.transfer_count == 0u) return "checkpoint=none";
        const auto index = observed_.transfer_failure_event < observed_.transfer_count
            ? observed_.transfer_failure_event : observed_.transfer_count - 1u;
        const auto &event = observed_.transfer_events[index];
        auto result = "checkpoint=" + std::to_string(static_cast<std::uint32_t>(event.checkpoint)) +
            " event_index=" + std::to_string(index) +
            " sp=" + std::to_string(event.cpu.gpr[29]) +
            " ra=" + std::to_string(event.cpu.gpr[31]) +
            " a0=" + std::to_string(event.cpu.gpr[4]) +
            " a1=" + std::to_string(event.cpu.gpr[5]) +
            " a2=" + std::to_string(event.cpu.gpr[6]) +
            " a3=" + std::to_string(event.cpu.gpr[7]);
        if (event.has_descriptor) {
            const auto u32 = [&event](std::size_t at) {
                return std::uint32_t{event.descriptor[at]} |
                    (std::uint32_t{event.descriptor[at + 1u]} << 8u) |
                    (std::uint32_t{event.descriptor[at + 2u]} << 16u) |
                    (std::uint32_t{event.descriptor[at + 3u]} << 24u);
            };
            result += " desc_id=" + std::to_string(std::uint16_t{event.descriptor[2]} |
                (std::uint16_t{event.descriptor[3]} << 8u)) +
                " desc_dest=" + std::to_string(u32(4u)) +
                " desc_chunk=" + std::to_string(u32(8u)) +
                " desc_offset=" + std::to_string(u32(12u)) +
                " desc_total=" + std::to_string(u32(16u)) +
                " desc_group=" + std::to_string(event.descriptor[24u]);
        }
        return result;
    }
#endif
public:
    unsigned calls{}, factories{}, caller_tails{}, owner_reuses{}, command_frees{}, owner_resets{}, heap_resets{}, max_steps{};
    Gate(const Elf32Image &elf, std::span<const std::uint8_t> overlay, Library &library,
         std::uint32_t mirror
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
         , bool one_descriptor = false, bool one_writer = false
#endif
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
         , std::uint32_t max_read_frames = TextureTransferTracker::kMaxReadFrames
         , std::uint32_t max_read_attempts = TextureTransferTracker::kMaxReadAttempts
         , std::uint64_t read_attempt_serial_limit = 0xFFFFFFFFFFFFFFFFull
#endif
         )
        : actual_(elf.required_ram_size()), reference_(elf.required_ram_size()),
          binding_(actual_, {&observed_, &Observations::entry, &Observations::returned,
              &Observations::lifetime
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
              , &Observations::transfer
#endif
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
              , &Observations::read
#endif
          }),
          mirror_(mirror) {
        require(binding_.installed(), "Could not bind lifetime observations");
        for (auto *runtime : {&actual_, &reference_}) {
            (void)elf.load_and_relocate(runtime->memory());
            runtime->memory().copy_in(overlay_base, overlay);
            runtime->memory().zero(overlay_base + static_cast<std::uint32_t>(overlay.size()), word(overlay, 20u));
        }
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
        const auto module_info = elf.find_module_info(actual_.memory());
        require(module_info.has_value(), "G1a fixture could not identify main-module imports");
        imports_ = elf.scan_imports(actual_.memory(), *module_info);
        require(!imports_.empty() && is_import(0x08965818u),
                "G1a fixture lacks the certified queue notification import");
        // Only bounded calls reached while setting up the original allocator
        // and enqueue path are modeled. They return zero and perform no I/O or
        // worker scheduling; actual worker completion stays outside G1a.
        for (const auto &import : imports_)
            actual_.register_hle(import.library, import.nid,
                [](Runtime &, AllegrexContext &ctx) { ctx.gpr[2] = 0u; });
#endif
        register_generated_functions(actual_);
        auto info = reinterpret_cast<const mhp3rd::OverlayModuleInfo *(*)()>(dlsym(library.handle, "mhp3rd_overlay_info"));
        auto install = reinterpret_cast<void (*)(Runtime &)>(dlsym(library.handle, "mhp3rd_register_overlay"));
        require(info && install && info()->base == overlay_base &&
                info()->abi_version == mhp3rd::kOverlayAbiVersion &&
                info()->hash == 0xF6300296C8D954E5ull, "Original module identity differs");
        install(actual_);
        for (const auto &section : elf.sections()) {
            if ((section.flags & 4u) == 0u || section.size == 0u) continue;
            CodeRegion region{elf.section_runtime_address(section), std::vector<std::uint8_t>(section.size)};
            actual_.memory().copy_out(region.address, region.bytes);
            code_.push_back(std::move(region));
        }
        code_.push_back({overlay_base, std::vector<std::uint8_t>(overlay.begin(),
            overlay.begin() + 64u + word(overlay, 12u))});
        mhp3rd::resources::SourceAuthorityConfig config;
        config.ram_bytes = static_cast<std::uint32_t>(actual_.memory().size());
        config.owner_allocator = owner_manager; config.command_allocator = command_manager;
        config.supported_code = supported_code;
        authority_ = std::make_unique<mhp3rd::resources::SourceAuthority>(config);
        TextureLifetimeTrackerConfig tracker_config;
        tracker_config.owner_allocator = owner_manager; tracker_config.command_allocator = command_manager;
        tracker_config.code = supported_code;
        tracker_config.current_code = &code_valid; tracker_config.current_code_user = this;
        tracker_ = std::make_unique<TextureLifetimeTracker>(actual_, *authority_, tracker_config);
        observed_.tracker = tracker_.get();
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
        TextureTransferTrackerConfig transfer_config;
        transfer_config.code = supported_code;
        transfer_config.current_code = &code_valid;
        transfer_config.current_code_user = this;
        if (one_descriptor) transfer_config.max_descriptors = 1u;
        if (one_writer) transfer_config.max_pending_writers = 1u;
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
        transfer_config.max_read_frames = max_read_frames;
        transfer_config.max_read_attempts = max_read_attempts;
        transfer_config.read_attempt_serial_limit = read_attempt_serial_limit;
#endif
        transfer_tracker_ = std::make_unique<TextureTransferTracker>(
            actual_, *authority_, *tracker_, transfer_config);
        observed_.transfer_tracker = transfer_tracker_.get();
        observed_.other_runtime = &reference_;
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
        observed_.read_tracker = transfer_tracker_.get();
#endif
#endif
        store(0x09FBE8D8u, raw(container));
        store(0x09FBE75Cu, raw(command_manager));
    }
    std::uint32_t raw(std::uint32_t value) const { return value | mirror_; }
    TextureLifetimeTrackerStats tracker_stats() const { return tracker_->stats(); }
    void store(std::uint32_t address, std::uint32_t value) {
        actual_.memory().store32(address, value); reference_.memory().store32(address, value);
    }
    AllegrexContext context(std::uint32_t entry) const {
        AllegrexContext ctx{};
        for (std::size_t i = 1; i < ctx.gpr.size(); ++i) ctx.gpr[i] = 0x34567000u + static_cast<std::uint32_t>(i);
        ctx.gpr[29] = raw(sp); ctx.gpr[31] = end_pc; ctx.pc = entry;
        return ctx;
    }
    AllegrexContext run(AllegrexContext ctx) {
        return run_from(ctx, ctx.pc);
    }
    AllegrexContext run_from(AllegrexContext ctx, std::uint32_t entry) {
        auto expected = ctx;
        expected.pc = entry;
        unsigned dispatches = 0;
        std::uint32_t dispatch_pc = entry;
        do {
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
            if (is_import(dispatch_pc)) {
                model_import(actual_, ctx, true);
                dispatch_pc = ctx.pc;
                continue;
            }
#endif
            require(++dispatches < 10000u && actual_.has_function(dispatch_pc), "AOT observation path escaped dispatch budget");
            const auto invoked = actual_.invoke_isolated_aot(dispatch_pc, ctx);
            require(invoked && !actual_.stopped(), "AOT observation path stopped at " +
                    std::to_string(dispatch_pc) + " ctx.pc=" + std::to_string(ctx.pc) +
                    " reason=" + actual_.stop_reason());
            dispatch_pc = ctx.pc;
        } while (ctx.pc != end_pc);
        unsigned steps = 0;
        do {
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
            if (read_prefix_active_ && expected.pc == 0x0886551Cu &&
                reference_read_calls_ >= stop_after_read_results_) {
                expected.pc = end_pc;
                break;
            }
#endif
            if (is_import(expected.pc)) {
                // G1a models enqueue imports; G1b models only the bounded
                // sceIoRead call and returns before the result comparison branch.
                model_import(reference_, expected, false);
                require(++steps < 2000000u, "Interpreter lifetime path exceeded instruction budget");
                continue;
            }
#endif
            require((expected.pc >= 0x08804000u && expected.pc < 0x08965A00u) ||
                    (expected.pc >= ctor && expected.pc < ctor + 112u), "Interpreter escaped certified main/constructor code");
            require(interpret_allegrex(reference_, expected, 1u) == InterpreterExit::Budget &&
                    !reference_.stopped(), "Interpreter lifetime path stopped");
            require(++steps < 2000000u, "Lifetime path exceeded instruction budget");
        } while (expected.pc != end_pc);
        require(same_context(ctx, expected), "Lifetime observer changed original CPU effects");
        const auto actual_ram = actual_.memory().bytes();
        const auto reference_ram = reference_.memory().bytes();
        if (actual_ram != reference_ram) {
            for (std::size_t i = 0; i < actual_ram.size(); ++i) {
                if (actual_ram[i] != reference_ram[i]) {
                    throw std::runtime_error("Lifetime observer changed original RAM effects at " +
                        std::to_string(i) + " actual=" + std::to_string(actual_ram[i]) +
                        " reference=" + std::to_string(reference_ram[i]));
                }
            }
        }
        require(actual_.memory().vram_bytes() == reference_.memory().vram_bytes(), "Lifetime observer changed original VRAM effects");
        require(!observed_.overflow, "Lifetime observation capacity exceeded");
        require(!observed_.replay_accepted, "One-use builder ticket was replayed");
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
        require(!observed_.transfer_overflow, "Transfer observation capacity exceeded");
        require(allow_transfer_loss_ || transfer_tracker_->error() == TextureTransferTrackerError::None,
                "Actual transfer tracker rejected a certified path: " +
                std::to_string(static_cast<int>(transfer_tracker_->error())) + " " +
                transfer_diagnostic());
#endif
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
        require(!observed_.read_overflow, "Read observation capacity exceeded");
#endif
        require(allow_loss_ || tracker_->error() == TextureLifetimeTrackerError::None,
                "Actual lifetime tracker rejected a certified path: " +
                std::to_string(static_cast<int>(tracker_->error())));
        ++calls; max_steps = std::max(max_steps, steps);
        return ctx;
    }
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
    AllegrexContext run_read_prefix(AllegrexContext ctx) {
        require(read_prefix_active_ && stop_after_read_results_ != 0u,
                "Read-prefix driver is not configured");
        read_prefix_stop_after = stop_after_read_results_;
        read_prefix_read_invocations = 0u;
        read_prefix_stop_calls = 0u;
        const auto result = run_from(ctx, ctx.pc);
        read_prefix_stop_after = 0u;
        require(read_prefix_stop_calls == actual_read_calls_ &&
                actual_read_calls_ == reference_read_calls_ &&
                actual_read_calls_ == stop_after_read_results_,
                "AOT/interpreter read prefix did not stop after the planned result");
        return result;
    }
#endif
    void initialize(std::uint32_t manager, std::uint32_t heap) {
        const auto first = observed_.count;
        auto ctx = context(0x08879DA4u);
        ctx.gpr[4] = raw(manager); ctx.gpr[5] = raw(heap); ctx.gpr[6] = 0x100000u;
        (void)run(ctx);
        require(observed_.one(TextureLifetimeCheckpoint::HeapInit, first).cpu.gpr[4] == raw(manager) &&
                observed_.one(TextureLifetimeCheckpoint::HeapReset, first).cpu.gpr[4] == raw(manager),
                "Composed unit did not observe init/reset entries");
    }
    std::uint32_t factory(bool owner_expected = true) {
        const auto first = observed_.count;
        store(raw(container + 0xB43130u), 0xFFFFFFFFu);
        for (std::uint32_t i = 0; i < 32u; i += 4u) store(raw(sp + i), 0x63120000u + i);
        store(raw(sp + 20u), end_pc);
        auto ctx = context(0x088BD058u);
        ctx.gpr[17] = 0x1234u;
        const auto owner = run(ctx).gpr[2];
        require(owner != 0u, "Original factory allocation failed");
        const auto &allocation = observed_.one(TextureLifetimeCheckpoint::ReverseAllocate, first).cpu;
        const auto &result = observed_.one(TextureLifetimeCheckpoint::FactoryAllocationResult, first).cpu;
        const auto &construct = observed_.one(TextureLifetimeCheckpoint::FactoryConstructorCall, first).cpu;
        const auto &finished = observed_.one(TextureLifetimeCheckpoint::FactoryConstructorResult, first).cpu;
        require(allocation.gpr[4] == raw(owner_manager) && allocation.gpr[5] == 0x2F470u &&
                allocation.gpr[6] == 16u && allocation.gpr[31] == 0x088BD07Cu &&
                result.gpr[2] == owner && construct.gpr[4] == owner && finished.gpr[2] == 1u,
                "Factory checkpoints broke allocation-to-constructor provenance");
        require(actual_.memory().load32(owner) == 0x0896FBC8u &&
                actual_.memory().load32(raw(container + 0xB43134u)) == owner,
                "Original factory did not record the constructed allocation");
        ++factories;
        if (owner_expected)
            require(tracker_->owner_token(owner).has_value(), "Factory did not issue a live owner lease");
        return owner;
    }
    std::uint32_t caller(std::uint32_t owner, bool ticket_expected = true, std::uint32_t count = 1u) {
        const auto first = observed_.count;
        const auto root = owner + 0x27C70u;
        // Deliberate fixture input, not an observed transfer or readiness receipt.
        store(root, 3u); store(root + 20u, 32u); store(root + 24u, 56u);
        store(root + 40u, count); store(root + 48u, 40u); store(root + 56u, 1u);
        store(root + 64u, 24u); store(root + 72u, 8u); store(root + 76u, 4u | (4u << 16u));
        for (std::uint32_t i = 0; i < 128u; i += 4u) store(raw(sp + i), 0x78230000u + i);
        store(raw(sp + 116u), end_pc);
        auto ctx = context(0x088B0398u); ctx.gpr[30] = owner;
        const auto before_builders = observed_.builders;
        (void)run(ctx);
        const auto &tail = observed_.one(TextureLifetimeCheckpoint::CallerTail, first).cpu;
        const auto &provider = observed_.one(TextureLifetimeCheckpoint::ProviderResult, first).cpu;
        const auto &child = observed_.one(TextureLifetimeCheckpoint::ChildResult, first).cpu;
        const auto &allocation = observed_.one(TextureLifetimeCheckpoint::ForwardAllocate, first).cpu;
        const auto &result = observed_.one(TextureLifetimeCheckpoint::CommandAllocationResult, first).cpu;
        const auto &builder = observed_.one(TextureLifetimeCheckpoint::BuilderCall, first).cpu;
        const auto command = actual_.memory().load32(owner + 0x13F4u);
        require(tail.gpr[30] == owner && provider.gpr[2] == root && child.gpr[2] == root + 32u &&
                allocation.gpr[4] == raw(command_manager) && allocation.gpr[5] == count * 36u &&
                allocation.gpr[6] == 16u && result.gpr[2] == command && builder.gpr[5] == command &&
                builder.gpr[6] == root + 32u && observed_.builders == before_builders + 1u,
                "Selected caller observations did not match actual original arguments/results");
        ++caller_tails;
        if (!ticket_expected) {
            require(!observed_.last_ticket, "Ineligible caller received a builder ticket");
            return command;
        }
        require(observed_.last_ticket.has_value() &&
                observed_.last_ticket->raw_owner == owner && observed_.last_ticket->raw_child == root + 32u &&
                observed_.last_ticket->raw_command == command && observed_.last_ticket->requested_command_bytes == 36u,
                "Actual caller checkpoints did not issue the expected one-use ticket");
        const auto &ticket = *observed_.last_ticket;
        require(authority_->failure() == mhp3rd::resources::AuthorityError::None &&
                authority_->permit(ticket.owner, ticket.command, 32u, 56u, 0u, 36u, supported_code).error ==
                    mhp3rd::resources::AuthorityError::NotReady,
                "Allocation/caller events fabricated source readiness without a transfer");
        return command;
    }
#ifdef MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE
    void configure_transfer_manager(bool uncached, std::uint32_t producer_index,
                                    std::uint32_t requested_bytes = 32u,
                                    bool deobfuscate = false) {
        auto manager = raw(transfer_manager);
        if (uncached) manager = psprecomp::GuestMemory::canonical(manager) | 0x40000000u;
        store(raw(0x08A3A03Cu), manager);
        store(manager, 0x0896F648u);
        store(manager + 0x11C0u, 17u);
        store(manager + 0x11C4u, requested_bytes);
        store(manager + 0x1090u, producer_index);
        store(manager + 0x2F7D4u, deobfuscate ? 1u : 0u);
    }
    AllegrexContext selected_load(std::uint32_t owner, std::uint32_t resource_id,
                                  bool uncached_manager, std::uint32_t producer_index,
                                  bool stale_pc, bool require_healthy = true,
                                  std::uint32_t requested_bytes = 32u,
                                  bool deobfuscate = false) {
        configure_transfer_manager(uncached_manager, producer_index,
                                   requested_bytes, deobfuscate);
        store(owner + 0x63u, 0u);
        auto ctx = context(0x088A5470u);
        ctx.gpr[4] = owner;
        ctx.gpr[5] = 7u;
        ctx.gpr[6] = resource_id;
        if (stale_pc) ctx.pc = 0x088A5564u;
        // 0x088A546C is an in-unit nop/goto into the instrumented entry at
        // 0x088A5470. invoke_isolated_aot selects that code, but the local goto
        // deliberately leaves ctx.pc at 546C when the callback runs.
        const auto result = run_from(ctx, stale_pc ? 0x088A546Cu : 0x088A5470u);
        if (!require_healthy) return result;
        const std::array expected{
            TextureTransferCheckpoint::OwnerLoadEntry,
            TextureTransferCheckpoint::OwnerLoadTail,
            TextureTransferCheckpoint::EnqueueEntry,
            TextureTransferCheckpoint::DescriptorCommit,
            TextureTransferCheckpoint::EnqueueReturn};
        require(observed_.transfer_count >= expected.size(),
                "Compiled transfer path missed a required checkpoint");
        const auto begin = observed_.transfer_count - expected.size();
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(observed_.transfer_events[begin + i].checkpoint == expected[i],
                    "Compiled transfer checkpoint order differs");
        if (stale_pc)
            require(observed_.transfer_events[begin].cpu.pc == 0x088A546Cu,
                    "Entry callback did not preserve stale guest ctx.pc");
        return result;
    }
#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
    TextureTransferTrackerStats read_prefix_scenario(const ReadScenario &scenario,
                                                       std::uint32_t sequence) {
        require(!scenario.results.empty() && scenario.results.size() <= 16u &&
                scenario.requested_bytes > 0u &&
                scenario.requested_bytes <= mhp3rd::resources::SourceAuthority::kSlotBytes,
                "Read fixture scenario exceeds its finite request bound");
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const bool seed_selected = scenario.selected || scenario.reuse_selected_history;
        read_owner_ = seed_selected ? factory() : 0u;
        std::uint32_t command = 0u;
        if (seed_selected) {
            command = caller(read_owner_);
            (void)selected_load(read_owner_, 17u, scenario.uncached_manager, 0u, false, true,
                                scenario.requested_bytes, false);
        }
        if (!scenario.selected) {
            require(enqueue_unowned(18u, 0x08300000u, 5u,
                                    scenario.requested_bytes).gpr[2] == 1u,
                    "Unowned read fixture could not enqueue its disjoint descriptor");
        }
        read_manager_ = actual_.memory().load32(raw(0x08A3A03Cu));
        std::uint32_t raw_record{};
        require(read_manager_ != 0u &&
                std::uint64_t{read_manager_} + kQueueRecordOffset <=
                    std::numeric_limits<std::uint32_t>::max(),
                "Read prefix has no valid worker manager record");
        raw_record = read_manager_ + kQueueRecordOffset;
        TextureTransferDescriptorRecord descriptor{};
        if (scenario.selected) {
            require(transfer_tracker_->descriptor_record(0u, descriptor) &&
                    descriptor.associated_load && descriptor.writer.instance != 0u &&
                    psprecomp::GuestMemory::canonical(descriptor.raw_descriptor) ==
                        psprecomp::GuestMemory::canonical(raw_record) &&
                    descriptor.raw_destination == read_owner_ + 0x27C70u &&
                    descriptor.write_footprint == scenario.requested_bytes,
                    "Selected read prefix did not start from the compiled descriptor path");
        } else if (scenario.reuse_selected_history) {
            TextureTransferDescriptorRecord old{};
            require(transfer_tracker_->descriptor_record_count() == 2u &&
                    transfer_tracker_->descriptor_record(0u, old) && !old.current &&
                    old.associated_load && old.writer.instance != 0u &&
                    transfer_tracker_->descriptor_record(1u, descriptor) &&
                    descriptor.current && !descriptor.associated_load &&
                    descriptor.owner.instance == 0u && descriptor.writer.instance == 0u &&
                    descriptor.generation > old.generation &&
                    psprecomp::GuestMemory::canonical(old.raw_descriptor) ==
                        psprecomp::GuestMemory::canonical(descriptor.raw_descriptor) &&
                    (old.raw_descriptor != descriptor.raw_descriptor) ==
                        scenario.uncached_manager &&
                    transfer_tracker_->stats().live_writers == 1u,
                    "Unowned read did not preserve the old writer across real ring reuse");
            require(actual_.memory().contains(raw_record, 32u),
                    "Reused unowned descriptor is outside guest RAM");
            descriptor.raw_destination = actual_.memory().load32(raw_record + 4u) +
                actual_.memory().load32(raw_record + 12u);
            descriptor.write_footprint = actual_.memory().load32(raw_record + 8u);
            require(descriptor.raw_descriptor == raw_record &&
                    descriptor.raw_destination == 0x08300000u &&
                    descriptor.write_footprint == scenario.requested_bytes,
                    "Unowned-history descriptor differs from the compiled enqueue output");
        } else {
            require(!transfer_tracker_->descriptor_record_count() &&
                    !transfer_tracker_->descriptor_record(0u, descriptor) &&
                    actual_.memory().contains(raw_record, 32u),
                    "Unowned disjoint read unexpectedly gained selected descriptor authority");
            descriptor.raw_descriptor = raw_record;
            descriptor.raw_manager = read_manager_;
            descriptor.raw_destination = actual_.memory().load32(raw_record + 4u) +
                actual_.memory().load32(raw_record + 12u);
            descriptor.write_footprint = actual_.memory().load32(raw_record + 8u);
            require(!descriptor.associated_load && descriptor.writer.instance == 0u &&
                    descriptor.raw_destination == 0x08300000u &&
                    descriptor.write_footprint == scenario.requested_bytes,
                    "Unowned disjoint read descriptor differs from the compiled enqueue output");
        }

        read_scenario_ = scenario;
        read_requested_bytes_ = scenario.requested_bytes;
        read_fd_ = 0x1200u + sequence;
        store(read_manager_ + kQueueConsumerOffset, 0u);
        store(read_manager_ + kQueueStateOffset, kWorkerState8);
        store(read_manager_ + kQueueFdOffset, read_fd_);
        store(read_manager_ + 0x2F7F0u, 11u);
        store(read_manager_ + 0x2F7C8u, 12u);
        store(raw(0x08300000u), read_manager_);
        // State 8 queries the guest current-thread context through this
        // audited global pointer before entering the helper. Supply a bounded
        // fixture-owned zero context so the worker follows that original path.
        store(0x08AB3640u, 0x08301000u);

        std::vector<std::uint8_t> scratch(kReadScratchBytes, 0xA5u);
        actual_.memory().copy_in(read_manager_ + kReadScratchOffset, scratch);
        reference_.memory().copy_in(read_manager_ + kReadScratchOffset, scratch);
        read_destination_address_ = descriptor.raw_destination;
        read_destination_bytes_ = descriptor.write_footprint;
        destination_before_actual_.clear();
        destination_before_reference_.clear();
        actual_read_calls_ = 0u;
        reference_read_calls_ = 0u;
        actual_read_trace_.clear();
        reference_read_trace_.clear();
        read_interleave_applied_ = false;
        read_prefix_active_ = true;
        stop_after_read_results_ = scenario.results.size();
        allow_transfer_loss_ = scenario.expected_error != TextureTransferTrackerError::None;
        observed_.read_count = 0u;
        observed_.read_overflow = false;
        observed_.drop_read = scenario.drop;
        observed_.drop_read_occurrence = scenario.drop_occurrence;
        observed_.drop_read_occurrences = 0u;
        observed_.duplicate_read = scenario.duplicate;
        observed_.wrong_context_read = scenario.wrong_context;
        observed_.wrong_runtime_read = scenario.wrong_runtime;
        observed_.switch_thread_read = scenario.switch_thread;
        observed_.descriptor_override_read = scenario.descriptor_override;
        observed_.register_override_read = scenario.register_override;
        observed_.synthetic_partial_descriptor_overlap_read =
            scenario.synthetic_partial_descriptor_overlap;
        if (scenario.synthetic_partial_descriptor_overlap)
            observed_.register_override_read = ReadRegisterOverride{
                TextureReadCheckpoint::ReadHelperEntry, 6u, raw_record + 1u, false};
        observed_.lifecycle_at_read = scenario.lifecycle_at;
        observed_.lifecycle_event = scenario.lifecycle_event;
        observed_.invalidate_owner = read_owner_;
        read_prefix_stop_after = stop_after_read_results_;
        read_prefix_read_invocations = 0u;
        read_prefix_stop_calls = 0u;

        auto worker = context(0x08865450u);
        worker.gpr[4] = 4u;
        worker.gpr[5] = raw(0x08300000u);
        worker.gpr[29] = raw(sp - 0x1000u);
        worker.gpr[31] = end_pc;
        const auto producer_thread = runtime_thread_uid();
        set_runtime_thread_identity(producer_thread == 3001u ? 3002u : 3001u,
                                    "read worker fixture");
        try { (void)run_from(worker, worker.pc); }
        catch (...) {
            set_runtime_thread_identity(producer_thread, "read worker fixture restored");
            read_prefix_active_ = false;
            read_prefix_stop_after = 0u;
            throw;
        }
        set_runtime_thread_identity(producer_thread, "read worker fixture restored");

        read_prefix_active_ = false;
        read_prefix_stop_after = 0u;
        require(actual_read_calls_ == scenario.results.size() &&
                reference_read_calls_ == scenario.results.size() &&
                actual_read_trace_ == reference_read_trace_,
                "AOT/interpreter read imports or arguments differ");
        require(read_prefix_stop_calls == scenario.results.size(),
                "The read-prefix oracle did not stop after each observed result");
        require(!destination_before_actual_.empty() &&
                actual_.memory().bytes().size() == reference_.memory().bytes().size(),
                "Read-prefix fixture did not capture a destination range");
        std::vector<std::uint8_t> actual_destination(read_destination_bytes_);
        std::vector<std::uint8_t> reference_destination(read_destination_bytes_);
        actual_.memory().copy_out(read_destination_address_, actual_destination);
        reference_.memory().copy_out(read_destination_address_, reference_destination);
        require(actual_destination == destination_before_actual_ &&
                reference_destination == destination_before_reference_,
                "The read-prefix oracle changed the owner destination before copy");

        const auto stats = transfer_tracker_->stats();
        require((scenario.selected || scenario.reuse_selected_history)
                    ? stats.live_writers >= 1u : stats.live_writers == 0u,
                "Read observation changed the destination-writer set unexpectedly");
        require(authority_->failure() == (scenario.expected_error == TextureTransferTrackerError::None
                    ? mhp3rd::resources::AuthorityError::None
                    : mhp3rd::resources::AuthorityError::ObservationLost),
                "Read observation produced an unexpected authority state");
        if (scenario.expected_error != TextureTransferTrackerError::None)
            require(transfer_tracker_->error() == scenario.expected_error ||
                    transfer_tracker_->error() == scenario.expected_alternate_error,
                    "Read fault did not produce its expected fail-closed result");
        else
            require(transfer_tracker_->error() == TextureTransferTrackerError::None,
                    "Healthy read prefix lost observation authority");
        if ((scenario.selected || scenario.reuse_selected_history) &&
            scenario.expected_error == TextureTransferTrackerError::None) {
            const auto owner_token = tracker_->owner_token(read_owner_);
            const auto command_current = tracker_->command_token(command);
            require(owner_token.has_value() && command_current.has_value() &&
                    authority_->permit(*owner_token, *command_current, 32u, 56u, 0u,
                        36u, supported_code).error ==
                            mhp3rd::resources::AuthorityError::NotReady,
                    "Read result alone granted source readiness");
        }
        if (scenario.reuse_selected_history &&
            scenario.expected_error == TextureTransferTrackerError::None) {
            require(transfer_tracker_->read_attempt_count() == 0u &&
                    stats.read_attempt_records == 0u && stats.unowned_read_results == 1u &&
                    stats.live_writers == 1u,
                    "Unowned history read gained selected authority or dropped its old writer");
        }
        if (scenario.expect_retry_route_rejected) {
            TextureReadAttemptRecord retry{};
            require(transfer_tracker_->read_attempt_count() == 1u &&
                    transfer_tracker_->read_attempt_record(0u, retry) && retry.serial == 1u &&
                    retry.result_observed && retry.result == 7 &&
                    retry.outcome == TextureReadOutcome::RetryObserved &&
                    stats.read_invocations == 1u && stats.read_results == 1u &&
                    stats.retry_read_results == 1u && stats.exact_read_results == 0u,
                    "Retry accepted an exact result after the second State8 observation was dropped");
        }
        if (scenario.expect_stale_attempt_rejected) {
            TextureReadAttemptRecord pending{};
            TextureTransferDescriptorRecord old{}, current{};
            const bool preserved = transfer_tracker_->read_attempt_count() == 1u &&
                    transfer_tracker_->read_attempt_record(0u, pending) &&
                    !pending.result_observed && pending.outcome == TextureReadOutcome::None &&
                    stats.read_invocations == 1u && stats.read_results == 0u &&
                    stats.exact_read_results == 0u && stats.live_writers >= 2u &&
                    transfer_tracker_->descriptor_record(0u, old) && !old.current &&
                    old.writer.instance != 0u && old.writer == pending.writer &&
                    transfer_tracker_->descriptor_record(1u, current) && current.current &&
                    !current.associated_load && current.owner.instance == 0u &&
                    current.writer.instance != 0u &&
                    current.generation != old.generation &&
                    current.snapshot == old.snapshot && current.writer != old.writer;
            if (!preserved) {
                throw std::runtime_error(
                    "Late-result same-byte generation audit differs: attempts=" +
                    std::to_string(transfer_tracker_->read_attempt_count()) +
                    " result_observed=" + std::to_string(pending.result_observed) +
                    " outcome=" + std::to_string(static_cast<int>(pending.outcome)) +
                    " invocations=" + std::to_string(stats.read_invocations) +
                    " results=" + std::to_string(stats.read_results) +
                    " exact=" + std::to_string(stats.exact_read_results) +
                    " writers=" + std::to_string(stats.live_writers) +
                    " descriptors=" + std::to_string(transfer_tracker_->descriptor_record_count()) +
                    " old_current=" + std::to_string(old.current) +
                    " new_current=" + std::to_string(current.current) +
                    " old_gen=" + std::to_string(old.generation) +
                    " new_gen=" + std::to_string(current.generation) +
                    " same_snapshot=" + std::to_string(current.snapshot == old.snapshot));
            }
        }
        if (scenario.expect_incomplete) {
            require(scenario.expected_error == TextureTransferTrackerError::None &&
                    transfer_tracker_->read_attempt_count() == scenario.results.size() &&
                    stats.active_read_frames == 1u,
                    "Missing read result did not remain an incomplete bounded attempt");
            TextureReadAttemptRecord pending{};
            require(transfer_tracker_->read_attempt_record(
                        transfer_tracker_->read_attempt_count() - 1u, pending) &&
                    !pending.result_observed && pending.outcome == TextureReadOutcome::None,
                    "Incomplete read attempt was promoted to a result");
        }
        if (scenario.selected && scenario.interleave == ReadInterleave::None &&
            scenario.expected_error == TextureTransferTrackerError::None &&
            !scenario.expect_incomplete && !scenario.drop && !scenario.duplicate &&
            !scenario.wrong_context && !scenario.wrong_runtime && !scenario.switch_thread &&
            !scenario.descriptor_override && !scenario.register_override) {
            require(transfer_tracker_->read_attempt_count() == scenario.results.size(),
                    "Read attempt history count differs from modeled imports");
            for (std::size_t i = 0; i < scenario.results.size(); ++i) {
                TextureReadAttemptRecord attempt{};
                require(transfer_tracker_->read_attempt_record(i, attempt) &&
                        attempt.serial == i + 1u &&
                        attempt.descriptor_generation == descriptor.generation &&
                        attempt.request_generation == descriptor.load_generation &&
                        attempt.raw_manager == read_manager_ &&
                        attempt.raw_record == descriptor.raw_descriptor &&
                        attempt.raw_scratch == read_manager_ + kReadScratchOffset &&
                        attempt.fd == read_fd_ && attempt.requested_bytes == scenario.requested_bytes &&
                        attempt.result == scenario.results[i] && attempt.result_observed,
                        "Read attempt record lost its descriptor/worker/import/result identity");
                const auto expected_outcome = scenario.results[i] > 0 &&
                    static_cast<std::uint32_t>(scenario.results[i]) == scenario.requested_bytes
                        ? TextureReadOutcome::ExactReadObserved : TextureReadOutcome::RetryObserved;
                require(attempt.outcome == expected_outcome,
                        "Read result classification differs from its signed result");
            }
            const bool complete = scenario.results.back() > 0 &&
                static_cast<std::uint32_t>(scenario.results.back()) == scenario.requested_bytes;
            require(stats.active_read_frames == (complete ? 0u : 1u),
                    "Read request frame closed before exact bytes or leaked after exact bytes");
        }
        allow_transfer_loss_ = false;
        return transfer_tracker_->stats();
    }
#endif
    AllegrexContext enqueue_unowned(std::uint32_t resource_id,
                                   std::uint32_t destination,
                                   std::uint32_t group,
                                   std::uint32_t requested_bytes = 32u,
                                   std::uint32_t offset = 0u) {
        configure_transfer_manager(false, 0u);
        const auto manager = raw(transfer_manager);
        store(manager + 0x11C0u, resource_id);
        store(manager + 0x11C4u, requested_bytes);
        auto ctx = context(0x08863CDCu);
        ctx.gpr[4] = manager;
        ctx.gpr[5] = resource_id;
        ctx.gpr[6] = destination;
        ctx.gpr[7] = group;
        ctx.gpr[8] = offset;
        ctx.gpr[9] = 1u;
        return run(ctx);
    }
    void invalid_unowned_range(std::uint32_t destination,
                               std::uint32_t requested_bytes,
                               std::uint32_t offset) {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const auto owner = factory();
        (void)selected_load(owner, 17u, false, 0u, false);
        TextureTransferDescriptorRecord selected{};
        require(transfer_tracker_->descriptor_record(0u, selected) &&
                selected.writer.instance != 0u,
                "Range fixture did not establish the old selected writer");
        allow_transfer_loss_ = true;
        require(enqueue_unowned(18u, destination, 5u, requested_bytes, offset).gpr[2] == 1u,
                "Original manager failed to return from the bounded range fixture");
        TextureTransferDescriptorRecord retained{};
        require(transfer_tracker_->error() == TextureTransferTrackerError::InvalidRange &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost &&
                transfer_tracker_->stats().live_writers == 1u &&
                transfer_tracker_->descriptor_record(0u, retained) &&
                retained.writer == selected.writer,
                "Invalid unowned range was treated as a proved disjoint span or evicted the old writer");
    }
    void invalid_selected_offset() {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const auto owner = factory();
        (void)selected_load(owner, 17u, false, 0u, false);
        TextureTransferDescriptorRecord selected{};
        require(transfer_tracker_->descriptor_record(0u, selected) &&
                selected.writer.instance != 0u,
                "Offset-overflow fixture did not establish the old selected writer");
        allow_transfer_loss_ = true;
        observed_.descriptor_override = DescriptorOverride{0xFFFFFFF0u, 0x20u};
        (void)selected_load(owner, 17u, false, 0u, false, false);
        TextureTransferDescriptorRecord retained{};
        require(transfer_tracker_->error() == TextureTransferTrackerError::InvalidRange &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost &&
                transfer_tracker_->stats().live_writers == 1u &&
                transfer_tracker_->descriptor_record(0u, retained) &&
                retained.writer == selected.writer && !observed_.descriptor_override,
                "Descriptor destination-plus-offset overflow did not fail closed with the old writer retained");
    }
    void invalid_selected_zero_chunk() {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const auto owner = factory();
        (void)selected_load(owner, 17u, false, 0u, false);
        TextureTransferDescriptorRecord selected{};
        require(transfer_tracker_->descriptor_record(0u, selected) &&
                selected.writer.instance != 0u,
                "Zero-length fixture did not establish the old selected writer");
        allow_transfer_loss_ = true;
        observed_.descriptor_override = DescriptorOverride{owner + 0x27C70u, 0u, 0u};
        (void)selected_load(owner, 17u, false, 0u, false, false);
        TextureTransferDescriptorRecord retained{};
        require(transfer_tracker_->error() == TextureTransferTrackerError::InvalidRange &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost &&
                transfer_tracker_->stats().live_writers == 1u &&
                transfer_tracker_->descriptor_record(0u, retained) &&
                retained.writer == selected.writer && !observed_.descriptor_override,
                "Zero-length descriptor was accepted or erased an earlier writer");
    }
    void exact_selected_slot_boundary() {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const auto owner = factory();
        (void)selected_load(owner, 17u, false, 0u, false, true,
                            mhp3rd::resources::SourceAuthority::kSlotBytes);
        TextureTransferDescriptorRecord record{};
        require(transfer_tracker_->error() == TextureTransferTrackerError::None &&
                transfer_tracker_->stats().live_writers == 1u &&
                transfer_tracker_->descriptor_record(0u, record) &&
                record.write_footprint == mhp3rd::resources::SourceAuthority::kSlotBytes &&
                record.snapshot[8] == 0x00u && record.snapshot[9] == 0x58u &&
                record.snapshot[10] == 0u && record.snapshot[11] == 0u &&
                authority_->failure() == mhp3rd::resources::AuthorityError::None &&
                caller(owner) != 0u,
                "Exact selected-slot boundary was rejected or granted source readiness");
    }
    void over_selected_slot_boundary() {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const auto owner = factory();
        (void)selected_load(owner, 17u, false, 0u, false);
        TextureTransferDescriptorRecord first{};
        require(transfer_tracker_->descriptor_record(0u, first) && first.writer.instance != 0u,
                "Over-slot fixture did not establish its old selected writer");
        allow_transfer_loss_ = true;
        (void)selected_load(owner, 17u, false, 0u, false, false,
                            mhp3rd::resources::SourceAuthority::kSlotBytes + 1u);
        TextureTransferDescriptorRecord rejected{};
        require(transfer_tracker_->error() == TextureTransferTrackerError::InvalidDescriptor &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost &&
                transfer_tracker_->stats().live_writers == 2u &&
                transfer_tracker_->descriptor_record(0u, first) && first.writer.instance != 0u &&
                transfer_tracker_->descriptor_record(1u, rejected) &&
                rejected.write_footprint == mhp3rd::resources::SourceAuthority::kSlotBytes + 1u &&
                rejected.writer.instance != 0u,
                "Over-slot request did not fail closed with both writer hazards retained");
    }
    void rounded_transform_footprint() {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const auto owner = factory();
        // Use a read-only-boundary field fault to present a valid five-byte
        // transformed descriptor to the tracker, then restore the descriptor
        // before the original enqueue resumes. No worker or transform runs.
        observed_.descriptor_override = DescriptorOverride{
            owner + 0x27C70u, 0u, 5u, 5u, 1u};
        (void)selected_load(owner, 17u, false, 0u, false);
        TextureTransferDescriptorRecord record{};
        require(transfer_tracker_->error() == TextureTransferTrackerError::None &&
                transfer_tracker_->stats().live_writers == 1u &&
                transfer_tracker_->descriptor_record(0u, record) &&
                record.snapshot[8] == 5u && record.snapshot[9] == 0u &&
                record.snapshot[16] == 5u && record.snapshot[27] == 1u &&
                record.write_footprint == 8u &&
                authority_->failure() == mhp3rd::resources::AuthorityError::None &&
                !observed_.descriptor_override && caller(owner) != 0u,
                "Rounded transform footprint was not tracked separately from logical bytes");
    }
    void healthy_g1a_and_reuse() {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        auto owner = factory();
        const auto first_owner_token = *tracker_->owner_token(owner);
        auto first_result = selected_load(owner, 17u, false, 0u, true);
        (void)first_result;
        require(authority_->failure() == mhp3rd::resources::AuthorityError::None,
                "Healthy G1a load poisoned SourceAuthority");
        auto stats = transfer_tracker_->stats();
        require(stats.loads_started == 1u && stats.tail_transfers == 1u &&
                stats.enqueue_entries == 1u && stats.descriptor_generations == 1u &&
                stats.queued_returns == 1u && stats.live_writers == 1u,
                "G1a load/enqueue/writer counts differ");
        TextureTransferDescriptorRecord first{};
        require(transfer_tracker_->descriptor_record(0u, first) && first.associated_load &&
                first.queue_return_observed && first.queue_return_value == 1u &&
                first.owner == first_owner_token && first.generation != 0u &&
                first.descriptor.instance != 0u && first.descriptor.serial != 0u &&
                first.writer.instance != 0u && first.writer.serial != 0u &&
                first.raw_destination == owner + 0x27C70u && first.write_footprint == 32u,
                "G1a descriptor record lacks its owner/generation/pending writer");
        const auto *snapshot = actual_.memory().raw_pointer(first.raw_descriptor, 32u);
        require(snapshot != nullptr &&
                std::equal(first.snapshot.begin(), first.snapshot.end(), snapshot) &&
                (first.snapshot[0] | (std::uint16_t{first.snapshot[1]} << 8u)) == 1u &&
                (first.snapshot[2] | (std::uint16_t{first.snapshot[3]} << 8u)) == 17u,
                "Descriptor snapshot is not the exact committed 32-byte ring record");

        const auto command = caller(owner);
        const auto old_command_token = *tracker_->command_token(command);
        const auto old_writer = first.writer;
        const auto old_slot_generation = tracker_->owner_invalidation_generation(owner);
        require(old_slot_generation.has_value(),
                "Owner reset fixture has no slot invalidation generation");
        const auto reset_revision = authority_->revision();
        store(owner + 0xB80u, 3u);
        auto reset = context(0x088A53C8u); reset.gpr[4] = owner;
        (void)run(reset);
        const auto new_slot_generation = tracker_->owner_invalidation_generation(owner);
        require(authority_->failure() == mhp3rd::resources::AuthorityError::None &&
                authority_->revision() > reset_revision &&
                new_slot_generation.has_value() &&
                *new_slot_generation == *old_slot_generation + 1u &&
                transfer_tracker_->stats().live_writers == 1u &&
                transfer_tracker_->descriptor_record(0u, first) && first.writer == old_writer,
                "Owner reset cleared an uncompleted destination hazard");

        auto release = context(0x088A3474u);
        release.gpr[4] = raw(command_manager); release.gpr[5] = owner;
        (void)run(release);
        require(!tracker_->command_token(command).has_value(),
                "Selected command release retained its lifetime lease");
        release = context(0x08879FF0u);
        release.gpr[4] = raw(owner_manager); release.gpr[5] = owner;
        (void)run(release);
        require(!tracker_->owner_token(owner).has_value() &&
                transfer_tracker_->stats().live_writers == 1u,
                "Owner free discarded the pending transfer writer");
        owner = factory();
        require(owner == first.raw_destination - 0x27C70u &&
                *tracker_->owner_token(owner) != first_owner_token &&
                transfer_tracker_->stats().live_writers == 1u,
                "Owner address reuse erased the old pending writer");

        (void)selected_load(owner, 17u, true, 0u, false);
        stats = transfer_tracker_->stats();
        TextureTransferDescriptorRecord second{};
        require(stats.loads_started == 2u && stats.descriptor_generations == 2u &&
                stats.queued_returns == 2u && stats.descriptor_ring_reuses == 1u &&
                stats.descriptor_alias_reuses == 1u &&
                stats.descriptor_raw_pointer_reuses == 0u && stats.live_writers == 2u &&
                transfer_tracker_->descriptor_record(1u, second) && second.associated_load &&
                second.owner == *tracker_->owner_token(owner) && second.owner != first.owner &&
                second.descriptor != first.descriptor && second.writer != first.writer &&
                psprecomp::GuestMemory::canonical(second.raw_descriptor) ==
                    psprecomp::GuestMemory::canonical(first.raw_descriptor) &&
                second.raw_descriptor != first.raw_descriptor,
                "Cached/uncached ring-slot reuse did not create a fresh generation");

        (void)selected_load(owner, 17u, false, 0u, false);
        stats = transfer_tracker_->stats();
        TextureTransferDescriptorRecord third{};
        require(stats.loads_started == 3u && stats.descriptor_generations == 3u &&
                stats.queued_returns == 3u && stats.descriptor_ring_reuses == 2u &&
                stats.descriptor_alias_reuses == 1u &&
                stats.descriptor_raw_pointer_reuses == 1u && stats.live_writers == 3u &&
                transfer_tracker_->descriptor_record(2u, third) && third.associated_load &&
                third.owner == second.owner && third.descriptor != second.descriptor &&
                third.writer != second.writer && third.raw_descriptor == first.raw_descriptor,
                "Same-raw-address ring reuse did not create a fresh generation");
        require(enqueue_unowned(18u, 0x08300000u, 5u).gpr[2] == 1u,
                "Original manager did not enqueue the disjoint unowned fixture");
        stats = transfer_tracker_->stats();
        TextureTransferDescriptorRecord unowned{};
        const bool unowned_recorded = transfer_tracker_->descriptor_record(3u, unowned);
        const auto *unowned_snapshot = unowned_recorded
            ? actual_.memory().raw_pointer(unowned.raw_descriptor, 32u) : nullptr;
        require(stats.descriptor_generations == 4u && stats.live_writers == 3u &&
                stats.descriptor_ring_reuses == 3u &&
                stats.descriptor_raw_pointer_reuses == 2u &&
                stats.descriptor_alias_reuses == 1u &&
                transfer_tracker_->descriptor_record(0u, first) && !first.current &&
                first.writer != mhp3rd::resources::AuthorityToken{} &&
                transfer_tracker_->descriptor_record(1u, second) && !second.current &&
                transfer_tracker_->descriptor_record(2u, third) && !third.current &&
                unowned_recorded && unowned.current &&
                !unowned.associated_load && unowned.owner == mhp3rd::resources::AuthorityToken{} &&
                unowned.writer == mhp3rd::resources::AuthorityToken{} &&
                unowned.queue_return_observed && unowned.queue_return_value == 1u &&
                unowned.descriptor != third.descriptor && unowned_snapshot != nullptr &&
                std::equal(unowned.snapshot.begin(), unowned.snapshot.end(), unowned_snapshot),
                "Disjoint request reuse failed to revoke the old descriptor generation");
        (void)selected_load(owner, 17u, false, 0u, false);
        stats = transfer_tracker_->stats();
        TextureTransferDescriptorRecord selected_again{};
        require(stats.descriptor_generations == 5u && stats.loads_started == 4u &&
                stats.queued_returns == 4u && stats.live_writers == 4u &&
                stats.descriptor_ring_reuses == 4u &&
                stats.descriptor_raw_pointer_reuses == 3u &&
                transfer_tracker_->descriptor_record(3u, unowned) && !unowned.current &&
                transfer_tracker_->descriptor_record(4u, selected_again) &&
                selected_again.current && selected_again.associated_load &&
                selected_again.owner == second.owner &&
                selected_again.descriptor != unowned.descriptor &&
                selected_again.writer != first.writer,
                "Selected reuse resurrected a descriptor token after disjoint reuse");
        require(caller(owner) != 0u &&
                authority_->failure() == mhp3rd::resources::AuthorityError::None &&
                transfer_tracker_->stats().live_writers == 4u,
                "Incomplete G1a requests did not remain healthy-but-not-ready");
    }
    void transfer_fault(TextureTransferCheckpoint checkpoint, unsigned mode,
                        TextureTransferTrackerError expected_error) {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const auto owner = factory();
        configure_transfer_manager(false, 0u);
        allow_transfer_loss_ = true;
        if (mode == 0u) observed_.drop_transfer = checkpoint;
        else if (mode == 1u) observed_.duplicate_transfer = checkpoint;
        else if (mode == 2u) observed_.wrong_runtime_transfer = checkpoint;
        else if (mode == 3u) observed_.wrong_context_transfer = checkpoint;
        else if (mode == 4u) {
            observed_.wrong_manager_transfer = checkpoint;
            observed_.wrong_manager_value = raw(transfer_manager + 0x1000u);
        }
        else if (mode == 5u) observed_.switch_thread_transfer = checkpoint;
        if (mode == 6u) force_code_invalid_ = true;
        if (mode == 7u) observed_.drop_transfer = checkpoint;
        if (mode == 8u) {
            allow_transfer_loss_ = false;
            (void)selected_load(owner, 17u, false, 0u, false);
            allow_transfer_loss_ = true;
            observed_.descriptor_override = DescriptorOverride{0x08300000u, 0u, std::nullopt};
            (void)selected_load(owner, 17u, false, 0u, false, false);
        } else {
            (void)selected_load(owner, 17u, false, 0u, false, false);
        }
        if (mode == 7u) {
            require(transfer_tracker_->error() == TextureTransferTrackerError::None,
                    "Dropped queue return was detected before the next owner request");
            observed_.drop_transfer.reset();
            (void)selected_load(owner, 17u, false, 0u, false, false);
        }
        if (mode == 8u) {
            TextureTransferDescriptorRecord old_record{}, new_record{};
            require(transfer_tracker_->stats().live_writers == 2u &&
                    transfer_tracker_->descriptor_record(0u, old_record) &&
                    transfer_tracker_->descriptor_record(1u, new_record) &&
                    old_record.writer.instance != 0u && new_record.writer.instance != 0u &&
                    old_record.writer != new_record.writer &&
                    new_record.raw_destination == 0x08300000u && !new_record.associated_load &&
                    !observed_.descriptor_override,
                    "Wrong destination did not retain both writer hazards with failed correlation");
        }
        force_code_invalid_ = false;
        require(authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost &&
                transfer_tracker_->error() == expected_error &&
                transfer_tracker_->stats().active_loads == 0u &&
                transfer_tracker_->stats().active_queue_frames == 0u,
                "Lost or duplicated transfer observation failed open");
        for (std::size_t i = 0; i < transfer_tracker_->descriptor_record_count(); ++i) {
            TextureTransferDescriptorRecord record{};
            if (transfer_tracker_->descriptor_record(i, record))
                require(record.writer.instance != 0u,
                        "A recorded descriptor lost its pending writer token");
        }
    }
    void capacity_fault() {
        initialize(owner_manager, owner_heap);
        initialize(command_manager, command_heap);
        const auto owner = factory();
        (void)selected_load(owner, 17u, false, 0u, false);
        TextureTransferDescriptorRecord first{};
        require(transfer_tracker_->descriptor_record(0u, first) &&
                transfer_tracker_->stats().live_writers == 1u,
                "Capacity fixture did not establish its retained first writer");
        allow_transfer_loss_ = true;
        (void)selected_load(owner, 17u, false, 0u, false, false);
        TextureTransferDescriptorRecord retained{};
        require(transfer_tracker_->error() == TextureTransferTrackerError::NoCapacity &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost &&
                transfer_tracker_->stats().live_writers == 1u &&
                transfer_tracker_->descriptor_record(0u, retained) &&
                retained.writer == first.writer,
                "Bounded-capacity loss evicted or forgot an older writer hazard");
    }
#endif
    void check() {
        initialize(owner_manager, owner_heap); initialize(command_manager, command_heap);
        auto owner = factory();
        const auto old_owner = *tracker_->owner_token(owner);
        auto command = caller(owner);
        const auto old_command = *tracker_->command_token(command);
        const auto revision = authority_->revision();
        store(owner + 0xB80u, 3u); // Exercise the original no-pending-cancel reset branch.
        auto reset = context(0x088A53C8u); reset.gpr[4] = owner;
        const auto reset_first = observed_.count;
        (void)run(reset);
        require(observed_.one(TextureLifetimeCheckpoint::OwnerReset, reset_first).cpu.gpr[4] == owner &&
                *tracker_->owner_token(owner) == old_owner && authority_->revision() > revision,
                "Owner reset lost its lease or missed source invalidation");
        ++owner_resets;
        auto first = observed_.count;
        auto release = context(0x088A3474u); release.gpr[4] = raw(command_manager); release.gpr[5] = owner;
        (void)run(release);
        require(observed_.one(TextureLifetimeCheckpoint::Free, first).cpu.gpr[5] == command &&
                actual_.memory().load32(owner + 0x13F4u) == 0u, "Original selected release was not observed");
        ++command_frees;
        require(!tracker_->command_token(command).has_value(), "Command free retained its live token");
        first = observed_.count;
        release = context(0x08879FF0u); release.gpr[4] = raw(owner_manager); release.gpr[5] = owner;
        (void)run(release);
        require(observed_.one(TextureLifetimeCheckpoint::Free, first).cpu.gpr[5] == owner,
                "Original owner free was not observed");
        require(!tracker_->owner_token(owner).has_value(), "Owner free retained its live token");
        require(factory() == owner, "Original owner address reuse not exercised");
        require(*tracker_->owner_token(owner) != old_owner, "Reused address retained an old allocation generation");
        ++owner_reuses;
        require(caller(owner) == command && *tracker_->command_token(command) != old_command,
                "Reused command address retained its previous generation");
        reset = context(0x08879D58u); reset.gpr[4] = raw(owner_manager);
        (void)run(reset);
        require(!tracker_->owner_token(owner) && tracker_->command_token(command).has_value(),
                "Heap reset missed its owners or invalidated the other heap");
        ++heap_resets;
    }
    void missing_constructor_checkpoint() {
        initialize(owner_manager, owner_heap);
        observed_.drop = TextureLifetimeCheckpoint::FactoryConstructorCall;
        allow_loss_ = true;
        const auto owner = factory(false);
        require(tracker_->error() == TextureLifetimeTrackerError::MissingPhase &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost &&
                !tracker_->owner_token(owner), "Missing constructor phase fabricated an owner lease");
    }
    void changed_code() {
        initialize(owner_manager, owner_heap); initialize(command_manager, command_heap);
        const auto owner = factory();
        const auto before = actual_.memory().load32(ctor + 108u);
        store(ctor + 108u, before ^ 1u); // This constructor is not executed by the subsequent caller.
        allow_loss_ = true;
        (void)caller(owner, false);
        require(tracker_->error() == TextureLifetimeTrackerError::CodeChanged &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost,
                "Changed current code did not revoke observations");
        store(ctor + 108u, before);
        require(!tracker_->owner_token(owner), "Restoring code silently restored stale authority");
    }
    void switched_thread() {
        initialize(owner_manager, owner_heap); initialize(command_manager, command_heap);
        const auto owner = factory();
        observed_.switch_at_builder = true; allow_loss_ = true;
        (void)caller(owner, false);
        require(tracker_->error() == TextureLifetimeTrackerError::ConflictingPhase &&
                authority_->failure() == mhp3rd::resources::AuthorityError::ObservationLost,
                "Switch-away/back accepted a stale caller ticket");
    }
    void empty_command() {
        initialize(owner_manager, owner_heap); initialize(command_manager, command_heap);
        const auto owner = factory();
        require(caller(owner, false, 0u) == 0u && tracker_->stats().commands_allocated == 0u &&
                tracker_->stats().active_frames == 0u && tracker_->stats().tickets_consumed == 0u,
                "No-command path leaked a positive lease or unfinished caller frame");
    }
};
}

#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
namespace mhp3rd::native {
bool texture_read_oracle_stop_after_result(psprecomp::Runtime &,
    psprecomp::AllegrexContext &) noexcept {
    ++::read_prefix_stop_calls;
    return ::read_prefix_stop_after != 0u &&
        ::read_prefix_read_invocations >= ::read_prefix_stop_after;
}
} // namespace mhp3rd::native
#endif

#ifdef MHP3RD_TEXTURE_READ_G1B_ORACLE
int main(int argc, char **argv) {
    try {
        require(argc == 5 || argc == 7,
                "usage: texture_read_observation_oracle EBOOT.ELF lobby.bin lobby.dylib report.json [scenario_start scenario_count]");
        require(!std::filesystem::exists(argv[4]), "Use a new G1b-read report path");
        require(sha256_file(argv[1]) == "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c",
                "Unsupported ELF");
        require(sha256_file(argv[2]) == "c34bf34f5e71993f5f2d20cdc39ec1b965f64b66d46f8d7672b449fba64b5aca",
                "Unsupported lobby image");
        require(sha256_file(argv[3]) == "35d381ffb06ff45f7357d3ef1634719bcfd4d5810eba1de9b32ca0443b62f538",
                "Unsupported lobby module");
        std::ifstream stream(argv[2], std::ios::binary);
        std::vector<std::uint8_t> overlay{std::istreambuf_iterator<char>(stream), {}};
        const auto elf = Elf32Image::from_file(argv[1]);
        Library module(argv[3]);
        std::vector<ReadScenario> scenarios;
        const auto add = [&scenarios](std::string name, std::vector<std::int32_t> results,
                                      std::uint32_t bytes = 32u) {
            ReadScenario scenario{};
            scenario.name = name;
            scenario.results = std::move(results);
            scenario.requested_bytes = bytes;
            scenarios.push_back(std::move(scenario));
        };
        const auto add_fault = [&scenarios](ReadScenario scenario) {
            scenarios.push_back(std::move(scenario));
        };
        const auto checkpoint_id = [](TextureReadCheckpoint checkpoint) -> const char * {
            switch (checkpoint) {
            case TextureReadCheckpoint::WorkerState8Entry: return "state8";
            case TextureReadCheckpoint::ReadHelperEntry: return "helper";
            case TextureReadCheckpoint::ReadInvoke: return "invoke";
            case TextureReadCheckpoint::ReadResult: return "result";
            }
            return "unknown";
        };
        add("exact-32", {32});
        add("exact-slot-boundary", {0x5800}, 0x5800u);
        add("short-then-full", {31, 32});
        add("zero-then-full", {0, 32});
        add("negative-then-full", {-1, 32});
        add("minimum-negative-then-full", {std::numeric_limits<std::int32_t>::min(), 32});
        add("several-shorts-then-full", {3, 7, 32});
        add("shorts-sum-to-request", {16, 16});
        add("retry-only-prefix", {9});
        add("overread-result", {33});
        scenarios.back().expected_error = TextureTransferTrackerError::InvalidRange;
        add("uncached-manager", {32});
        scenarios.back().uncached_manager = true;
        add("unowned-disjoint-read", {32});
        scenarios.back().selected = false;

        for (const bool uncached : {false, true}) {
            ReadScenario history{};
            history.name = uncached ? "unowned-history-uncached-alias"
                                    : "unowned-history-same-address";
            history.results = {32};
            history.selected = false;
            history.reuse_selected_history = true;
            history.uncached_manager = uncached;
            add_fault(std::move(history));
        }
        ReadScenario retry_route{};
        retry_route.name = "retry-missing-second-state8";
        retry_route.results = {7, 32};
        retry_route.drop = TextureReadCheckpoint::WorkerState8Entry;
        retry_route.drop_occurrence = 2u;
        retry_route.expected_error = TextureTransferTrackerError::InvalidObservation;
        retry_route.expect_retry_route_rejected = true;
        add_fault(std::move(retry_route));
        ReadScenario partial_overlap{};
        partial_overlap.name = "synthetic-partial-descriptor-overlap";
        partial_overlap.results = {32};
        partial_overlap.synthetic_partial_descriptor_overlap = true;
        partial_overlap.expected_error = TextureTransferTrackerError::UnpairedSelectedLoad;
        add_fault(std::move(partial_overlap));
        ReadScenario stale_attempt{};
        stale_attempt.name = "synthetic-same-byte-commit-during-active-attempt";
        stale_attempt.results = {32};
        stale_attempt.interleave = ReadInterleave::SyntheticDescriptorCommitSameBytes;
        stale_attempt.expected_error = TextureTransferTrackerError::InvalidDescriptor;
        stale_attempt.expect_stale_attempt_rejected = true;
        add_fault(std::move(stale_attempt));

        // Exercise request/slot invalidation early so a failing modeled
        // producer interleave is diagnosed before the byte mutation matrix.
        for (const auto [interleave, label, error] : std::array{
                std::tuple{ReadInterleave::OwnerReset, "owner-reset",
                           TextureTransferTrackerError::UnpairedSelectedLoad},
                std::tuple{ReadInterleave::OwnerFreeReuse, "owner-free-reuse",
                           TextureTransferTrackerError::UnpairedSelectedLoad},
                std::tuple{ReadInterleave::SameIdReload, "same-id-reload",
                           TextureTransferTrackerError::UnpairedSelectedLoad}}) {
            ReadScenario invalidated{};
            invalidated.name = std::string("owner-interleave-") + label;
            invalidated.results = {32};
            invalidated.interleave = interleave;
            invalidated.expected_error = error;
            add_fault(std::move(invalidated));
        }

        // Mutate a representative byte from every descriptor field across
        // the four stages that revalidate the full 32-byte snapshot, then
        // restore it before guest execution resumes. Each stage covers
        // multiple field classes.
        constexpr std::array read_points{
            TextureReadCheckpoint::WorkerState8Entry,
            TextureReadCheckpoint::ReadHelperEntry,
            TextureReadCheckpoint::ReadInvoke,
            TextureReadCheckpoint::ReadResult};
        constexpr std::array descriptor_field_samples{
            std::pair{TextureReadCheckpoint::WorkerState8Entry, 0u},
            std::pair{TextureReadCheckpoint::WorkerState8Entry, 4u},
            std::pair{TextureReadCheckpoint::WorkerState8Entry, 24u},
            std::pair{TextureReadCheckpoint::ReadHelperEntry, 2u},
            std::pair{TextureReadCheckpoint::ReadHelperEntry, 8u},
            std::pair{TextureReadCheckpoint::ReadHelperEntry, 27u},
            std::pair{TextureReadCheckpoint::ReadInvoke, 12u},
            std::pair{TextureReadCheckpoint::ReadInvoke, 25u},
            std::pair{TextureReadCheckpoint::ReadInvoke, 28u},
            std::pair{TextureReadCheckpoint::ReadResult, 16u},
            std::pair{TextureReadCheckpoint::ReadResult, 20u},
            std::pair{TextureReadCheckpoint::ReadResult, 26u}};
        for (const auto [checkpoint, byte] : descriptor_field_samples) {
            ReadScenario scenario{};
            scenario.name = std::string("descriptor-resample-") + checkpoint_id(checkpoint) +
                "-byte-" + std::to_string(byte);
            scenario.results = {32};
            scenario.descriptor_override = ReadDescriptorOverride{
                checkpoint, byte, 0u, true};
            scenario.expected_error = TextureTransferTrackerError::InvalidDescriptor;
            scenario.expected_alternate_error = TextureTransferTrackerError::InvalidRange;
            add_fault(std::move(scenario));
        }

        // Every callback phase must be single-use and tied to this runtime,
        // CPU context and worker execution. A missing final result remains an
        // incomplete attempt; earlier missing evidence fails closed.
        for (const auto checkpoint : read_points) {
            ReadScenario dropped{};
            dropped.name = std::string("dropped-") + checkpoint_id(checkpoint);
            dropped.results = {32};
            dropped.drop = checkpoint;
            if (checkpoint == TextureReadCheckpoint::ReadResult)
                dropped.expect_incomplete = true;
            else
                dropped.expected_error = checkpoint == TextureReadCheckpoint::WorkerState8Entry
                    ? TextureTransferTrackerError::UnpairedSelectedLoad
                    : TextureTransferTrackerError::UnpairedSelectedLoad;
            add_fault(std::move(dropped));

            ReadScenario duplicate{};
            duplicate.name = std::string("duplicate-") + checkpoint_id(checkpoint);
            duplicate.results = {32};
            duplicate.duplicate = checkpoint;
            duplicate.expected_error = checkpoint == TextureReadCheckpoint::WorkerState8Entry ||
                checkpoint == TextureReadCheckpoint::ReadHelperEntry
                    ? TextureTransferTrackerError::InvalidObservation
                    : TextureTransferTrackerError::UnpairedSelectedLoad;
            add_fault(std::move(duplicate));

            ReadScenario wrong_runtime{};
            wrong_runtime.name = std::string("wrong-runtime-") + checkpoint_id(checkpoint);
            wrong_runtime.results = {32};
            wrong_runtime.wrong_runtime = checkpoint;
            wrong_runtime.expected_error = TextureTransferTrackerError::InvalidObservation;
            add_fault(std::move(wrong_runtime));

            ReadScenario wrong_context{};
            wrong_context.name = std::string("wrong-context-") + checkpoint_id(checkpoint);
            wrong_context.results = {32};
            wrong_context.wrong_context = checkpoint;
            wrong_context.expected_error = TextureTransferTrackerError::UnpairedSelectedLoad;
            add_fault(std::move(wrong_context));

            ReadScenario switched_thread{};
            switched_thread.name = std::string("switched-thread-") + checkpoint_id(checkpoint);
            switched_thread.results = {32};
            switched_thread.switch_thread = checkpoint;
            switched_thread.expected_error = TextureTransferTrackerError::UnpairedSelectedLoad;
            add_fault(std::move(switched_thread));
        }

        const auto add_register_fault = [&add_fault](
                const char *name, TextureReadCheckpoint checkpoint,
                std::uint32_t reg, std::uint32_t value,
                TextureTransferTrackerError error) {
            ReadScenario scenario{};
            scenario.name = name;
            scenario.results = {32};
            scenario.register_override = ReadRegisterOverride{checkpoint, reg, value, false};
            scenario.expected_error = error;
            add_fault(std::move(scenario));
        };
        add_register_fault("state8-consumer-mismatch",
            TextureReadCheckpoint::WorkerState8Entry, 7u, 1u,
            TextureTransferTrackerError::InvalidObservation);
        add_register_fault("state8-manager-mismatch",
            TextureReadCheckpoint::WorkerState8Entry, 17u, 0x08400000u,
            TextureTransferTrackerError::InvalidObservation);
        add_register_fault("state8-record-mismatch",
            TextureReadCheckpoint::WorkerState8Entry, 18u, 0x08401000u,
            TextureTransferTrackerError::InvalidObservation);
        add_register_fault("helper-manager-mismatch",
            TextureReadCheckpoint::ReadHelperEntry, 4u, 0x08400000u,
            TextureTransferTrackerError::InvalidObservation);
        add_register_fault("helper-fd-mismatch",
            TextureReadCheckpoint::ReadHelperEntry, 5u, 0x7FFFu,
            TextureTransferTrackerError::InvalidObservation);
        add_register_fault("helper-record-mismatch",
            TextureReadCheckpoint::ReadHelperEntry, 6u, 0x08401000u,
            TextureTransferTrackerError::InvalidRange);
        add_register_fault("invoke-fd-mismatch",
            TextureReadCheckpoint::ReadInvoke, 4u, 0x7FFFu,
            TextureTransferTrackerError::InvalidRange);
        add_register_fault("invoke-scratch-mismatch",
            TextureReadCheckpoint::ReadInvoke, 5u, 0x08401000u,
            TextureTransferTrackerError::InvalidRange);
        add_register_fault("invoke-request-mismatch",
            TextureReadCheckpoint::ReadInvoke, 6u, 33u,
            TextureTransferTrackerError::InvalidRange);
        add_register_fault("result-request-mismatch",
            TextureReadCheckpoint::ReadResult, 2u, 33u,
            TextureTransferTrackerError::InvalidObservation);
        add_register_fault("result-record-mismatch",
            TextureReadCheckpoint::ReadResult, 18u, 0x08401000u,
            TextureTransferTrackerError::InvalidObservation);

        // The queue manager may be referenced through either PSP cached alias.
        for (const auto [checkpoint, reg] : std::array{
                std::pair{TextureReadCheckpoint::WorkerState8Entry, 17u},
                std::pair{TextureReadCheckpoint::ReadHelperEntry, 4u}}) {
            ReadScenario alias{};
            alias.name = std::string("cached-uncached-manager-alias-") +
                checkpoint_id(checkpoint);
            alias.results = {32};
            alias.uncached_manager = true;
            alias.register_override = ReadRegisterOverride{checkpoint, reg, 0u, true};
            add_fault(std::move(alias));
        }

        // Invalidation before/within a state-8 read makes that generation
        // ineligible while preserving the pending asynchronous writer.
        for (const auto checkpoint : read_points) {
            for (const auto lifecycle_event : {TextureLifetimeCheckpoint::OwnerReset,
                                               TextureLifetimeCheckpoint::Free}) {
                ReadScenario invalidated{};
                invalidated.name = std::string("owner-invalidated-") +
                    checkpoint_id(checkpoint) + "-" +
                    (lifecycle_event == TextureLifetimeCheckpoint::OwnerReset
                        ? "reset" : "free");
                invalidated.results = {32};
                invalidated.lifecycle_at = checkpoint;
                invalidated.lifecycle_event = lifecycle_event;
                invalidated.expected_error = TextureTransferTrackerError::UnpairedSelectedLoad;
                add_fault(std::move(invalidated));
            }
        }
        ReadScenario attempt_capacity{};
        attempt_capacity.name = "read-attempt-capacity";
        attempt_capacity.results = {0, 32};
        attempt_capacity.max_read_attempts = 1u;
        attempt_capacity.expected_error = TextureTransferTrackerError::NoCapacity;
        add_fault(std::move(attempt_capacity));
        ReadScenario serial_capacity{};
        serial_capacity.name = "read-attempt-serial-capacity";
        serial_capacity.results = {0, 32};
        serial_capacity.read_attempt_serial_limit = 1u;
        serial_capacity.expected_error = TextureTransferTrackerError::CounterExhausted;
        add_fault(std::move(serial_capacity));

        std::size_t scenario_start = 0u, scenario_count = scenarios.size();
        if (argc == 7) {
            std::size_t start_chars{}, count_chars{};
            const auto parsed_start = std::stoull(argv[5], &start_chars);
            const auto parsed_count = std::stoull(argv[6], &count_chars);
            require(start_chars == std::string(argv[5]).size() &&
                    count_chars == std::string(argv[6]).size() &&
                    parsed_start <= scenarios.size() && parsed_count > 0u &&
                    parsed_count <= scenarios.size() - parsed_start,
                    "G1b-read scenario shard is outside the frozen matrix");
            scenario_start = static_cast<std::size_t>(parsed_start);
            scenario_count = static_cast<std::size_t>(parsed_count);
        }
        const auto scenario_end = scenario_start + scenario_count;

        std::uint64_t calls = 0u, max_steps = 0u;
        std::uint64_t state8_entries = 0u, helper_entries = 0u, read_invocations = 0u;
        std::uint64_t read_results = 0u, exact_results = 0u, retry_results = 0u;
        std::uint64_t rejected_results = 0u;
        std::uint64_t attempt_records = 0u, unowned_reads = 0u, unowned_results = 0u;
        std::uint64_t pending_writers_observed = 0u, modeled_read_imports = 0u;
        for (std::size_t i = scenario_start; i < scenario_end; ++i) {
            Gate gate(elf, overlay, module, 0u, false, false,
                TextureTransferTracker::kMaxReadFrames,
                scenarios[i].max_read_attempts,
                scenarios[i].read_attempt_serial_limit);
            TextureTransferTrackerStats stats{};
            try {
                stats = gate.read_prefix_scenario(
                    scenarios[i], static_cast<std::uint32_t>(i));
            } catch (const std::exception &error) {
                std::cerr << "G1b-read scenario " << i << " ("
                          << scenarios[i].name << "): " << error.what() << '\n';
                throw;
            }
            calls += gate.calls;
            max_steps = std::max(max_steps,
                static_cast<std::uint64_t>(gate.max_steps));
            state8_entries += stats.read_state8_entries;
            helper_entries += stats.read_helper_entries;
            read_invocations += stats.read_invocations;
            read_results += stats.read_results;
            exact_results += stats.exact_read_results;
            retry_results += stats.retry_read_results;
            rejected_results += stats.rejected_read_results;
            attempt_records += stats.read_attempt_records;
            unowned_reads += stats.unowned_read_requests;
            unowned_results += stats.unowned_read_results;
            pending_writers_observed += stats.live_writers;
            modeled_read_imports += scenarios[i].results.size();
        }
        std::size_t descriptor_byte_cases = 0u, checkpoint_fault_cases = 0u;
        std::size_t register_argument_fault_cases = 0u, manager_alias_cases = 0u;
        std::size_t owner_invalidation_cases = 0u, capacity_loss_cases = 0u;
        std::size_t retry_route_fault_cases = 0u, unowned_history_cases = 0u;
        std::size_t partial_descriptor_overlap_fault_cases = 0u;
        std::size_t active_generation_reuse_cases = 0u, incomplete_attempt_cases = 0u;
        constexpr std::array register_fault_names{
            "state8-consumer-mismatch", "state8-manager-mismatch", "state8-record-mismatch",
            "helper-manager-mismatch", "helper-fd-mismatch", "helper-record-mismatch",
            "invoke-fd-mismatch", "invoke-scratch-mismatch", "invoke-request-mismatch",
            "result-request-mismatch", "result-record-mismatch"};
        for (std::size_t i = scenario_start; i < scenario_end; ++i) {
            const auto &scenario = scenarios[i];
            const auto &name = scenario.name;
            if (name.starts_with("descriptor-resample-")) ++descriptor_byte_cases;
            if (name.starts_with("dropped-") || name.starts_with("duplicate-") ||
                name.starts_with("wrong-runtime-") || name.starts_with("wrong-context-") ||
                name.starts_with("switched-thread-")) ++checkpoint_fault_cases;
            if (std::find(register_fault_names.begin(), register_fault_names.end(), name) !=
                register_fault_names.end()) ++register_argument_fault_cases;
            if (name.starts_with("cached-uncached-manager-alias-")) ++manager_alias_cases;
            if (name.starts_with("owner-interleave-") ||
                name.starts_with("owner-invalidated-")) ++owner_invalidation_cases;
            if (name.starts_with("read-attempt-")) ++capacity_loss_cases;
            if (name == "retry-missing-second-state8") ++retry_route_fault_cases;
            if (name.starts_with("unowned-history-")) ++unowned_history_cases;
            if (name == "synthetic-partial-descriptor-overlap")
                ++partial_descriptor_overlap_fault_cases;
            if (name == "synthetic-same-byte-commit-during-active-attempt")
                ++active_generation_reuse_cases;
            if (scenario.expect_incomplete) ++incomplete_attempt_cases;
        }
        constexpr std::size_t expected_scenarios = 75u;
        for (std::size_t i = 0; i < scenarios.size(); ++i)
            for (std::size_t j = i + 1u; j < scenarios.size(); ++j)
                require(scenarios[i].name != scenarios[j].name,
                        "G1b-read scenario identifiers must be unique");
        require(scenarios.size() == expected_scenarios && calls > 0u && max_steps > 0u &&
                max_steps < 2000000u && read_results <= read_invocations &&
                exact_results + retry_results + rejected_results + unowned_results == read_results,
                "G1b-read bounded prefix counters differ");
        std::ofstream out(argv[4]);
        out << "{\"schema_version\":1,\"scope\":\"original-g1b-state8-read-attempt-result-prefix\","
            << "\"success\":true,\"scenario_start\":" << scenario_start
            << ",\"scenario_total\":" << scenarios.size()
            << ",\"scenario_count\":" << scenario_count
            << ",\"descriptor_byte_resampling_cases\":" << descriptor_byte_cases
            << ",\"checkpoint_correlation_fault_cases\":" << checkpoint_fault_cases
            << ",\"register_argument_fault_cases\":" << register_argument_fault_cases
            << ",\"cached_uncached_alias_cases\":" << manager_alias_cases
            << ",\"owner_invalidation_cases\":" << owner_invalidation_cases
            << ",\"capacity_loss_cases\":" << capacity_loss_cases
            << ",\"retry_route_fault_cases\":" << retry_route_fault_cases
            << ",\"unowned_history_cases\":" << unowned_history_cases
            << ",\"partial_descriptor_overlap_fault_cases\":"
            << partial_descriptor_overlap_fault_cases
            << ",\"active_generation_reuse_cases\":" << active_generation_reuse_cases
            << ",\"incomplete_attempt_cases\":" << incomplete_attempt_cases
            << ",\"aot_interpreter_calls\":" << calls
            << ",\"modeled_read_imports\":" << modeled_read_imports
            << ",\"max_interpreter_slices\":" << max_steps
            << ",\"state8_entries\":" << state8_entries
            << ",\"read_helper_entries\":" << helper_entries
            << ",\"read_invocations\":" << read_invocations
            << ",\"read_results\":" << read_results
            << ",\"exact_read_results\":" << exact_results
            << ",\"retry_read_results\":" << retry_results
            << ",\"rejected_read_results\":" << rejected_results
            << ",\"unowned_read_results\":" << unowned_results
            << ",\"read_attempt_records\":" << attempt_records
            << ",\"unowned_disjoint_reads\":" << unowned_reads
            << ",\"pending_writers_observed\":" << pending_writers_observed
            << ",\"scenario_ids\":[";
        for (std::size_t i = scenario_start; i < scenario_end; ++i) {
            if (i != scenario_start) out << ',';
            out << '\"' << scenarios[i].name << '\"';
        }
        out << "]"
            << ",\"pending_writers_retained\":true"
            << ",\"full_ram_vram_cpu_compared\":true"
            << ",\"read_prefix_stopped_after_result\":true"
            << ",\"copy_transform_terminal_observations\":0"
            << ",\"completion_receipts\":0"
            << ",\"transfer_readiness\":false"
            << ",\"healthy_authority_exact_notready\":true"
            << ",\"modeled_io_imports\":true,\"event_scheduling_modeled\":true"
            << ",\"game_executed\":false}\n";
        require(static_cast<bool>(out), "Could not write G1b-read report");
        std::cout << "G1b-read: scenarios " << scenario_start << ".." << scenario_end
                  << " of " << scenarios.size()
                  << " passed; readiness remains false\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
#elif defined(MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE)
int main(int argc, char **argv) {
    try {
        require(argc == 5, "usage: texture_transfer_observation_oracle EBOOT.ELF lobby.bin lobby.dylib report.json");
        require(!std::filesystem::exists(argv[4]), "Use a new G1a report path");
        require(sha256_file(argv[1]) == "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c", "Unsupported ELF");
        require(sha256_file(argv[2]) == "c34bf34f5e71993f5f2d20cdc39ec1b965f64b66d46f8d7672b449fba64b5aca", "Unsupported lobby image");
        require(sha256_file(argv[3]) == "35d381ffb06ff45f7357d3ef1634719bcfd4d5810eba1de9b32ca0443b62f538", "Unsupported lobby module");
        std::ifstream stream(argv[2], std::ios::binary);
        std::vector<std::uint8_t> overlay{std::istreambuf_iterator<char>(stream), {}};
        const auto elf = Elf32Image::from_file(argv[1]);
        Library module(argv[3]);
        unsigned calls = 0u, max_steps = 0u;
        {
            Gate gate(elf, overlay, module, 0u);
            gate.healthy_g1a_and_reuse();
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        struct FaultCase { TextureTransferCheckpoint checkpoint; unsigned mode;
                            TextureTransferTrackerError expected; };
        const std::array faults{
            FaultCase{TextureTransferCheckpoint::OwnerLoadEntry, 0u, TextureTransferTrackerError::UnpairedSelectedLoad},
            FaultCase{TextureTransferCheckpoint::OwnerLoadEntry, 1u, TextureTransferTrackerError::ConflictingFrame},
            FaultCase{TextureTransferCheckpoint::OwnerLoadTail, 0u, TextureTransferTrackerError::UnpairedSelectedLoad},
            FaultCase{TextureTransferCheckpoint::EnqueueEntry, 0u, TextureTransferTrackerError::UnpairedSelectedLoad},
            FaultCase{TextureTransferCheckpoint::DescriptorCommit, 0u, TextureTransferTrackerError::InvalidObservation},
            FaultCase{TextureTransferCheckpoint::EnqueueEntry, 1u, TextureTransferTrackerError::ConflictingFrame},
            FaultCase{TextureTransferCheckpoint::EnqueueReturn, 7u, TextureTransferTrackerError::ConflictingFrame},
            FaultCase{TextureTransferCheckpoint::DescriptorCommit, 1u, TextureTransferTrackerError::InvalidObservation},
            FaultCase{TextureTransferCheckpoint::OwnerLoadEntry, 2u, TextureTransferTrackerError::InvalidObservation},
            FaultCase{TextureTransferCheckpoint::OwnerLoadTail, 3u, TextureTransferTrackerError::UnpairedSelectedLoad},
            FaultCase{TextureTransferCheckpoint::OwnerLoadTail, 4u, TextureTransferTrackerError::InvalidObservation},
            FaultCase{TextureTransferCheckpoint::OwnerLoadTail, 5u, TextureTransferTrackerError::UnpairedSelectedLoad},
            FaultCase{TextureTransferCheckpoint::OwnerLoadEntry, 6u, TextureTransferTrackerError::CodeChanged},
            FaultCase{TextureTransferCheckpoint::DescriptorCommit, 8u, TextureTransferTrackerError::InvalidDescriptor}};
        for (const auto &fault : faults) {
            Gate gate(elf, overlay, module, 0u);
            gate.transfer_fault(fault.checkpoint, fault.mode, fault.expected);
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        {
            Gate gate(elf, overlay, module, 0u, true, false);
            gate.capacity_fault();
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        {
            Gate gate(elf, overlay, module, 0u, false, true);
            gate.capacity_fault();
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        const auto guest_ram_end = std::uint64_t{GuestMemory::kPhysicalBase} +
            elf.required_ram_size();
        for (const auto &range : std::array{
                std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(guest_ram_end - 16u), 32u, 0u},
                std::array<std::uint32_t, 3>{0xFFFFFFF0u, 32u, 0u}}) {
            std::cerr << "G1a range fixture: unowned destination span\n";
            Gate gate(elf, overlay, module, 0u);
            gate.invalid_unowned_range(range[0], range[1], range[2]);
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        {
            std::cerr << "G1a range fixture: destination plus offset overflow\n";
            Gate gate(elf, overlay, module, 0u);
            gate.invalid_selected_offset();
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        {
            std::cerr << "G1a range fixture: zero chunk\n";
            Gate gate(elf, overlay, module, 0u);
            gate.invalid_selected_zero_chunk();
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        {
            std::cerr << "G1a boundary fixture: exact selected slot\n";
            Gate gate(elf, overlay, module, 0u);
            gate.exact_selected_slot_boundary();
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        {
            std::cerr << "G1a boundary fixture: over selected slot\n";
            Gate gate(elf, overlay, module, 0u);
            gate.over_selected_slot_boundary();
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        {
            std::cerr << "G1a footprint fixture: rounded transform extent\n";
            Gate gate(elf, overlay, module, 0u);
            gate.rounded_transform_footprint();
            calls += gate.calls;
            max_steps = std::max(max_steps, gate.max_steps);
        }
        require(calls > 0u && max_steps > 0u && max_steps < 2000000u,
                "G1a original-path budget was not met");
        std::ofstream out(argv[4]);
        out << "{\"schema_version\":1,\"scope\":\"original-g1a-load-enqueue-descriptor-generation\",\"success\":true,"
            << "\"aot_interpreter_calls\":" << calls << ",\"normal_loads\":4,\"duplicate_resource_id_loads\":3,"
            << "\"descriptor_generations\":5,\"queued_returns\":4,\"controlled_ring_slot_reuses\":3,"
            << "\"unowned_descriptor_reuses\":1,\"unowned_enqueues\":1,"
            << "\"descriptor_ring_reuses\":4,\"raw_pointer_reuses\":3,"
            << "\"cached_uncached_alias_reuses\":1,\"pending_writers_retained\":4,"
            << "\"fault_cases\":" << faults.size() << ",\"capacity_loss_cases\":2,\"stale_ctx_pc_cases\":1,"
            << "\"range_fault_cases\":4,\"zero_length_fault_cases\":1,"
            << "\"slot_boundary_cases\":2,\"over_slot_rejections\":1,"
            << "\"rounded_transform_footprint_cases\":1,"
            << "\"max_interpreter_slices\":" << max_steps
            << ",\"full_ram_vram_cpu_compared\":true,\"healthy_authority_exact_notready\":true,"
            << "\"transfer_readiness\":false,\"event_notification_modeled\":true,\"game_executed\":false}\n";
        require(static_cast<bool>(out), "Could not write G1a report");
        std::cout << "G1a: compiled load/enqueue/descriptor observations passed; readiness remains false\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
#else
int main(int argc, char **argv) {
    try {
        require(argc == 5, "usage: texture_lifetime_oracle EBOOT.ELF lobby.bin lobby.dylib report.json");
        require(!std::filesystem::exists(argv[4]), "Use a new report path");
        require(sha256_file(argv[1]) == "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c", "Unsupported ELF");
        require(sha256_file(argv[2]) == "c34bf34f5e71993f5f2d20cdc39ec1b965f64b66d46f8d7672b449fba64b5aca", "Unsupported lobby image");
        require(sha256_file(argv[3]) == "35d381ffb06ff45f7357d3ef1634719bcfd4d5810eba1de9b32ca0443b62f538", "Unsupported lobby module");
        std::ifstream stream(argv[2], std::ios::binary);
        std::vector<std::uint8_t> overlay{std::istreambuf_iterator<char>(stream), {}};
        const auto elf = Elf32Image::from_file(argv[1]);
        Library module(argv[3]);
        unsigned calls = 0, factories = 0, callers = 0, reuses = 0, frees = 0, resets = 0, heaps = 0, max_steps = 0;
        std::uint64_t owner_leases = 0, tickets = 0;
        for (auto mirror : {0u, 0x40000000u}) {
            Gate gate(elf, overlay, module, mirror); gate.check();
            calls += gate.calls; factories += gate.factories; callers += gate.caller_tails;
            reuses += gate.owner_reuses; frees += gate.command_frees;
            resets += gate.owner_resets; heaps += gate.heap_resets;
            max_steps = std::max(max_steps, gate.max_steps);
            owner_leases += gate.tracker_stats().owners_constructed;
            tickets += gate.tracker_stats().tickets_consumed;
        }
        for (unsigned fault = 0; fault < 4u; ++fault) {
            Gate gate(elf, overlay, module, 0u);
            if (fault == 0u) gate.missing_constructor_checkpoint();
            else if (fault == 1u) gate.changed_code();
            else if (fault == 2u) gate.switched_thread();
            else gate.empty_command();
            calls += gate.calls; max_steps = std::max(max_steps, gate.max_steps);
            factories += gate.factories; callers += gate.caller_tails;
            owner_leases += gate.tracker_stats().owners_constructed;
            tickets += gate.tracker_stats().tickets_consumed;
        }
        std::ofstream out(argv[4]);
        out << "{\"schema_version\":1,\"scope\":\"original-texture-lifetime-checkpoints\",\"success\":true,"
            << "\"calls_per_path\":" << calls << ",\"factory_chains\":" << factories
            << ",\"caller_chains\":" << callers << ",\"owner_reuses\":" << reuses
            << ",\"command_releases\":" << frees << ",\"max_interpreter_slices\":" << max_steps
            << ",\"owner_resets\":" << resets << ",\"heap_resets\":" << heaps
            << ",\"loss_cases\":3,\"owner_leases\":" << owner_leases << ",\"consumed_tickets\":" << tickets
            << ",\"no_command_cases\":1"
            << ",\"full_ram_vram_cpu_compared\":true,\"transfer_readiness\":false}\n";
        require(static_cast<bool>(out), "Could not write lifetime report");
        std::cout << "Lifetime: " << calls << " original calls, " << factories << " factory chains passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
#endif
