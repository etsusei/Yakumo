#include "native/texture_completion_decode.hpp"

#include <algorithm>
#include <limits>

namespace mhp3rd::native {
namespace {

constexpr std::uint32_t kDescriptorBytes = 32u;

constexpr std::array<CompletionCheckpointInfo, 23> kCheckpointInfo{{
    {TextureCompletionCheckpoint::ClassifierReturn, 0x0886577Cu, true, false, false, false,
        TextureCompletionCallCheckpoint::InlineCopyCall},
    {TextureCompletionCheckpoint::CopyReturn, 0x088652ACu, true, false, true, false,
        TextureCompletionCallCheckpoint::InlineCopyCall},
    {TextureCompletionCheckpoint::HelperReturn, 0x088659B4u, true, false, true, false,
        TextureCompletionCallCheckpoint::HelperCall},
    {TextureCompletionCheckpoint::PolicyReturn, 0x088659C4u, true, false, true, false,
        TextureCompletionCallCheckpoint::PolicyCall},
    {TextureCompletionCheckpoint::WorkerRequestCall, 0x088659CCu, false, false, false, false,
        TextureCompletionCallCheckpoint::WorkerRequestCall},
    {TextureCompletionCheckpoint::WorkerRequestReturn, 0x088659E4u, true, false, true, false,
        TextureCompletionCallCheckpoint::WorkerRequestCall},
    {TextureCompletionCheckpoint::WorkerEventSetReturn, 0x088659F4u, true, false, true, false,
        TextureCompletionCallCheckpoint::WorkerEventSetCall},
    {TextureCompletionCheckpoint::WorkerWaitReturn, 0x08865A10u, true, false, true, false,
        TextureCompletionCallCheckpoint::WorkerWaitCall},
    {TextureCompletionCheckpoint::WorkerEntry, 0x08865378u, false, false, false, false,
        TextureCompletionCallCheckpoint::WorkerCopyCall},
    {TextureCompletionCheckpoint::WorkerCopyReturn, 0x08865368u, true, false, true, false,
        TextureCompletionCallCheckpoint::WorkerCopyCall},
    {TextureCompletionCheckpoint::VerbatimBranch, 0x088653A0u, false, true, false, false,
        TextureCompletionCallCheckpoint::TransformCall},
    {TextureCompletionCheckpoint::DigestSkippedBranch, 0x088653B4u, false, true, false, false,
        TextureCompletionCallCheckpoint::DigestCall},
    {TextureCompletionCheckpoint::TransformCall, 0x08865420u, false, false, false, false,
        TextureCompletionCallCheckpoint::TransformCall},
    {TextureCompletionCheckpoint::TransformReturn, 0x08865428u, true, false, true, false,
        TextureCompletionCallCheckpoint::TransformCall},
    {TextureCompletionCheckpoint::DigestCall, 0x08865440u, false, false, false, false,
        TextureCompletionCallCheckpoint::DigestCall},
    {TextureCompletionCheckpoint::DigestReturn, 0x08865448u, true, false, true, false,
        TextureCompletionCallCheckpoint::DigestCall},
    {TextureCompletionCheckpoint::WorkerAckReturn, 0x088653C4u, true, false, true, false,
        TextureCompletionCallCheckpoint::WorkerAckCall},
    {TextureCompletionCheckpoint::RetirementCall, 0x08865814u, false, false, false, false,
        TextureCompletionCallCheckpoint::RetirementCall},
    {TextureCompletionCheckpoint::RetirementEntry, 0x08865D8Cu, false, false, true, true,
        TextureCompletionCallCheckpoint::RetirementCall},
    {TextureCompletionCheckpoint::RetirementReturn, 0x0886581Cu, true, false, true, false,
        TextureCompletionCallCheckpoint::RetirementCall},
    {TextureCompletionCheckpoint::GroupCancellation, 0x08866044u, false, false, false, false,
        TextureCompletionCallCheckpoint::RetirementCall},
    {TextureCompletionCheckpoint::FullQueueCancellation, 0x08865F00u, false, false, false, false,
        TextureCompletionCallCheckpoint::RetirementCall},
    {TextureCompletionCheckpoint::UnsupportedCopyRoute, 0x088652C4u, false, false, false, false,
        TextureCompletionCallCheckpoint::WorkerCopyCall},
}};

constexpr std::array<CompletionCallBoundaryInfo, 11> kCallBoundaryInfo{{
    {TextureCompletionCallCheckpoint::InlineCopyCall, 0x088652A4u, 0x088EE470u,
        0x088652ACu, TextureCompletionCheckpoint::CopyReturn, true},
    {TextureCompletionCallCheckpoint::HelperCall, 0x088659ACu, 0x08865238u,
        0x088659B4u, TextureCompletionCheckpoint::HelperReturn, true},
    {TextureCompletionCallCheckpoint::WorkerCopyCall, 0x08865360u, 0x088EE470u,
        0x08865368u, TextureCompletionCheckpoint::WorkerCopyReturn, true},
    {TextureCompletionCallCheckpoint::WorkerAckCall, 0x088653BCu, 0x089658B0u,
        0x088653C4u, TextureCompletionCheckpoint::WorkerAckReturn, true},
    {TextureCompletionCallCheckpoint::TransformCall, 0x08865420u, 0x08864BC8u,
        0x08865428u, TextureCompletionCheckpoint::TransformReturn, true},
    {TextureCompletionCallCheckpoint::DigestCall, 0x08865440u, 0x088637E0u,
        0x08865448u, TextureCompletionCheckpoint::DigestReturn, true},
    {TextureCompletionCallCheckpoint::RetirementCall, 0x08865814u, 0x08865D8Cu,
        0x0886581Cu, TextureCompletionCheckpoint::RetirementReturn, true},
    {TextureCompletionCallCheckpoint::PolicyCall, 0x088659BCu, 0x088651D0u,
        0x088659C4u, TextureCompletionCheckpoint::PolicyReturn, true},
    {TextureCompletionCallCheckpoint::WorkerRequestCall, 0x088659DCu, 0x089658B0u,
        0x088659E4u, TextureCompletionCheckpoint::WorkerRequestReturn, true},
    {TextureCompletionCallCheckpoint::WorkerEventSetCall, 0x088659ECu, 0x089657D0u,
        0x088659F4u, TextureCompletionCheckpoint::WorkerEventSetReturn, true},
    {TextureCompletionCallCheckpoint::WorkerWaitCall, 0x08865A08u, 0x08965858u,
        0x08865A10u, TextureCompletionCheckpoint::WorkerWaitReturn, true},
}};

template <typename T>
const T *find_info(std::span<const T> entries, decltype(T::checkpoint) checkpoint) noexcept {
    for (const auto &entry : entries)
        if (entry.checkpoint == checkpoint) return &entry;
    return nullptr;
}

bool same_execution(const psprecomp::RuntimeExecutionContextToken left,
                    const psprecomp::RuntimeExecutionContextToken right) noexcept {
    return left.thread_uid == right.thread_uid &&
           left.switch_generation == right.switch_generation;
}

std::uint32_t load32(const std::uint8_t *bytes) noexcept {
    return std::uint32_t{bytes[0]} | (std::uint32_t{bytes[1]} << 8u) |
           (std::uint32_t{bytes[2]} << 16u) | (std::uint32_t{bytes[3]} << 24u);
}

bool add_u32(std::uint32_t base, std::uint32_t offset,
             std::uint32_t &result) noexcept {
    const auto sum = std::uint64_t{base} + offset;
    if (sum > std::numeric_limits<std::uint32_t>::max()) return false;
    result = static_cast<std::uint32_t>(sum);
    return true;
}

} // namespace

std::span<const CompletionCheckpointInfo>
texture_completion_checkpoint_table() noexcept {
    return kCheckpointInfo;
}

std::span<const CompletionCallBoundaryInfo>
texture_completion_call_boundary_table() noexcept {
    return kCallBoundaryInfo;
}

const CompletionCheckpointInfo *texture_completion_checkpoint_info(
    TextureCompletionCheckpoint checkpoint) noexcept {
    return find_info(std::span<const CompletionCheckpointInfo>{kCheckpointInfo}, checkpoint);
}

const CompletionCallBoundaryInfo *texture_completion_call_boundary_info(
    TextureCompletionCallCheckpoint checkpoint) noexcept {
    return find_info(std::span<const CompletionCallBoundaryInfo>{kCallBoundaryInfo}, checkpoint);
}

CompletionSample TextureCompletionDecoder::common_sample(
    const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint, std::uint32_t site_pc) const noexcept {
    CompletionSample sample;
    sample.checkpoint = checkpoint;
    sample.site_pc = site_pc;
    sample.sp = context.gpr[29];
    sample.ra = context.gpr[31];
    sample.a0 = context.gpr[4];
    sample.a1 = context.gpr[5];
    sample.a2 = context.gpr[6];
    sample.a3 = context.gpr[7];
    return sample;
}

CompletionSample TextureCompletionDecoder::invalid(
    CompletionSample sample, CompletionDecodeError error) const noexcept {
    sample.status = CompletionDecodeStatus::Invalid;
    sample.error = error;
    sample.skip_reason = CompletionSkipReason::None;
    return sample;
}

bool TextureCompletionDecoder::valid_word(const psprecomp::Runtime &runtime,
                                          std::uint32_t address) const noexcept {
    return address != 0u && runtime.memory().contains(address, 4u) &&
           runtime.memory().raw_pointer(address, 4u) != nullptr;
}

bool TextureCompletionDecoder::valid_range(const psprecomp::Runtime &runtime,
                                           std::uint32_t address,
                                           std::uint32_t bytes) const noexcept {
    return address != 0u && bytes != 0u &&
           runtime.memory().contains(address, bytes) &&
           runtime.memory().raw_pointer(address, bytes) != nullptr;
}

bool TextureCompletionDecoder::read_word(const psprecomp::Runtime &runtime,
                                         std::uint32_t address,
                                         std::uint32_t &value) const noexcept {
    if (!valid_word(runtime, address)) return false;
    const auto *bytes = runtime.memory().raw_pointer(address, 4u);
    if (bytes == nullptr) return false;
    value = load32(bytes);
    return true;
}

bool TextureCompletionDecoder::read_byte(const psprecomp::Runtime &runtime,
                                         std::uint32_t address,
                                         std::uint8_t &value) const noexcept {
    if (address == 0u || !runtime.memory().contains(address, 1u)) return false;
    const auto *bytes = runtime.memory().raw_pointer(address, 1u);
    if (bytes == nullptr) return false;
    value = bytes[0];
    return true;
}

TextureCompletionDecoder::Receipt *TextureCompletionDecoder::find_receipt(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCallCheckpoint checkpoint,
    std::uint32_t return_pc,
    bool allow_execution_switch) noexcept {
    Receipt *found = nullptr;
    const auto execution = psprecomp::capture_runtime_execution_context();
    for (auto &receipt : receipts_) {
        if (!receipt.active || receipt.runtime != &runtime ||
            receipt.call.checkpoint != checkpoint ||
            receipt.call.return_pc != return_pc ||
            (!allow_execution_switch && receipt.context != &context) ||
            receipt.call.sp != context.gpr[29] || receipt.call.return_pc != context.gpr[31] ||
            receipt.execution.thread_uid != execution.thread_uid ||
            (!allow_execution_switch &&
             receipt.execution.switch_generation != execution.switch_generation)) continue;
        if (found != nullptr) return nullptr;
        found = &receipt;
    }
    return found;
}

const TextureCompletionDecoder::Receipt *TextureCompletionDecoder::find_receipt(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCallCheckpoint checkpoint,
    std::uint32_t return_pc,
    bool allow_execution_switch) const noexcept {
    const Receipt *found = nullptr;
    const auto execution = psprecomp::capture_runtime_execution_context();
    for (const auto &receipt : receipts_) {
        if (!receipt.active || receipt.runtime != &runtime ||
            receipt.call.checkpoint != checkpoint ||
            receipt.call.return_pc != return_pc ||
            (!allow_execution_switch && receipt.context != &context) ||
            receipt.call.sp != context.gpr[29] || receipt.call.return_pc != context.gpr[31] ||
            receipt.execution.thread_uid != execution.thread_uid ||
            (!allow_execution_switch &&
             receipt.execution.switch_generation != execution.switch_generation)) continue;
        if (found != nullptr) return nullptr;
        found = &receipt;
    }
    return found;
}

CompletionSample TextureCompletionDecoder::decode_call(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCallCheckpoint checkpoint,
    std::uint32_t site_pc) noexcept {
    const auto *info = texture_completion_call_boundary_info(checkpoint);
    CompletionSample sample{};
    sample.site_pc = site_pc;
    sample.sp = context.gpr[29];
    sample.ra = context.gpr[31];
    sample.a0 = context.gpr[4];
    sample.a1 = context.gpr[5];
    sample.a2 = context.gpr[6];
    sample.a3 = context.gpr[7];
    if (info == nullptr) return invalid(sample, CompletionDecodeError::UnknownCheckpoint);
    if (site_pc != info->site_pc) return invalid(sample, CompletionDecodeError::SitePcMismatch);
    if (!valid_word(runtime, context.gpr[29]))
        return invalid(sample, CompletionDecodeError::InvalidGuestPointer);
    if (context.gpr[31] != info->return_pc)
        return invalid(sample, CompletionDecodeError::UndecodableCallBoundary);

    CompletionCallArguments call;
    call.checkpoint = checkpoint;
    call.site_pc = site_pc;
    call.callee_pc = info->callee_pc;
    call.return_pc = info->return_pc;
    call.sp = context.gpr[29];
    call.ra = context.gpr[31];
    call.a0 = context.gpr[4];
    call.a1 = context.gpr[5];
    call.a2 = context.gpr[6];
    call.a3 = context.gpr[7];
    call.after_delay_slot = true;

    const auto set_manager_descriptor = [&](std::uint32_t manager,
                                            std::uint32_t descriptor) {
        if (!valid_word(runtime, manager) || !valid_range(runtime, descriptor, kDescriptorBytes))
            return false;
        call.manager = manager;
        call.descriptor = descriptor;
        call.has_manager = true;
        call.has_descriptor = true;
        return true;
    };
    const auto set_copy = [&]() {
        if (call.a2 == 0u || !valid_range(runtime, call.a0, call.a2) ||
            !valid_range(runtime, call.a1, call.a2)) return false;
        call.destination = call.a0;
        call.source = call.a1;
        call.bytes = call.a2;
        call.logical_length = call.a2;
        call.has_destination = true;
        call.has_source = true;
        call.has_bytes = true;
        call.has_logical_length = true;
        call.footprint = call.a2;
        call.has_footprint = true;
        return true;
    };

    const auto set_descriptor_payload = [&](std::uint32_t descriptor,
                                             bool transform) {
        std::uint32_t destination_base{};
        std::uint32_t logical_length{};
        std::uint32_t offset{};
        if (!read_word(runtime, descriptor + 4u, destination_base) ||
            !read_word(runtime, descriptor + 8u, logical_length) ||
            !read_word(runtime, descriptor + 12u, offset) || logical_length == 0u ||
            !add_u32(destination_base, offset, call.destination)) return false;
        const auto footprint64 = transform
            ? ((std::uint64_t{logical_length} + 3u) & ~std::uint64_t{3u})
            : std::uint64_t{logical_length};
        if (footprint64 == 0u ||
            footprint64 > std::numeric_limits<std::uint32_t>::max() ||
            !valid_range(runtime, call.destination,
                         static_cast<std::uint32_t>(footprint64))) return false;
        call.logical_length = logical_length;
        call.footprint = static_cast<std::uint32_t>(footprint64);
        call.has_destination = true;
        call.has_logical_length = true;
        call.has_footprint = true;
        return true;
    };

    const auto set_event = [&]() {
        // These imports receive a PSP event identity, not a guest pointer.
        // The manager-backed event slot is validated by the tracker against
        // the active operation; the call boundary itself only needs a stable,
        // non-zero identity.
        if (call.a0 == 0u) return false;
        call.event = call.a0;
        call.has_event = true;
        return true;
    };

    bool fields_ok = true;
    switch (checkpoint) {
    case TextureCompletionCallCheckpoint::InlineCopyCall:
        fields_ok = set_copy();
        if (fields_ok) fields_ok = set_manager_descriptor(context.gpr[16], context.gpr[17]);
        break;
    case TextureCompletionCallCheckpoint::WorkerCopyCall:
        fields_ok = set_copy();
        // The copy worker keeps its manager in gpr18 and its descriptor in
        // the saved current-record slot gpr16.  gpr17 is the worker's
        // scratch base and is deliberately not an identity claim.
        if (fields_ok) fields_ok = set_manager_descriptor(context.gpr[18], context.gpr[16]);
        break;
    case TextureCompletionCallCheckpoint::HelperCall:
    case TextureCompletionCallCheckpoint::PolicyCall:
        fields_ok = set_manager_descriptor(call.a0, call.a1);
        break;
    case TextureCompletionCallCheckpoint::WorkerAckCall:
    case TextureCompletionCallCheckpoint::WorkerRequestCall:
    case TextureCompletionCallCheckpoint::WorkerEventSetCall:
    case TextureCompletionCallCheckpoint::WorkerWaitCall:
        fields_ok = true;
        if (checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall) {
            // The acknowledgement is issued from the worker frame
            // (0x088653BC), which keeps manager in gpr18, the descriptor in
            // gpr16 and manager + 0x30000 in gpr17, as for WorkerCopyCall.
            if (valid_word(runtime, context.gpr[18]) &&
                valid_range(runtime, context.gpr[16], kDescriptorBytes))
                (void)set_manager_descriptor(context.gpr[18], context.gpr[16]);
        } else if (valid_word(runtime, context.gpr[17]) &&
                   valid_range(runtime, context.gpr[18], kDescriptorBytes)) {
            // Reader-frame syscalls keep manager/descriptor in gpr17/gpr18.
            (void)set_manager_descriptor(context.gpr[17], context.gpr[18]);
        }
        if (fields_ok && (checkpoint == TextureCompletionCallCheckpoint::WorkerAckCall ||
                          checkpoint == TextureCompletionCallCheckpoint::WorkerRequestCall ||
                          checkpoint == TextureCompletionCallCheckpoint::WorkerEventSetCall ||
                          checkpoint == TextureCompletionCallCheckpoint::WorkerWaitCall))
            fields_ok = set_event();
        break;
    case TextureCompletionCallCheckpoint::TransformCall:
        // 0x08864BC8 is a normal two-argument wrapper.  At the call edge
        // a0/a1 are the manager and descriptor; the worker's gpr17/gpr18
        // values have already been repurposed for scratch/stack state.
        fields_ok = set_manager_descriptor(call.a0, call.a1);
        if (fields_ok) fields_ok = set_descriptor_payload(call.a1, true);
        break;
    case TextureCompletionCallCheckpoint::DigestCall:
        // Keep the SHA-1 helper ABI separate from the transform rule.  The
        // generated caller also supplies manager/descriptor in a0/a1, while
        // its worker temporaries are not stable identity fields.
        fields_ok = set_manager_descriptor(call.a0, call.a1);
        if (fields_ok) fields_ok = set_descriptor_payload(call.a1, false);
        break;
    case TextureCompletionCallCheckpoint::RetirementCall:
        fields_ok = set_manager_descriptor(context.gpr[17], context.gpr[18]);
        break;
    }
    if (!fields_ok) return invalid(sample, CompletionDecodeError::InvalidGuestPointer);

    for (const auto &receipt : receipts_) {
        if (receipt.active && receipt.runtime == &runtime &&
            receipt.context == &context &&
            receipt.call.checkpoint == checkpoint &&
            receipt.call.return_pc == call.return_pc &&
            same_execution(receipt.execution,
                           psprecomp::capture_runtime_execution_context()))
            return invalid(sample, CompletionDecodeError::DuplicateCallReceipt);
    }
    Receipt *slot = nullptr;
    for (auto &candidate : receipts_) {
        if (!candidate.active) { slot = &candidate; break; }
    }
    if (slot == nullptr) return invalid(sample, CompletionDecodeError::ReceiptCapacity);
    slot->active = true;
    slot->runtime = &runtime;
    slot->context = &context;
    slot->execution = psprecomp::capture_runtime_execution_context();
    slot->call = call;
    slot->entered = false;

    sample.status = CompletionDecodeStatus::Observed;
    sample.error = CompletionDecodeError::None;
    sample.has_call = true;
    sample.call_checkpoint = checkpoint;
    sample.call = call;
    sample.has_manager = call.has_manager;
    sample.has_descriptor = call.has_descriptor;
    sample.manager = call.manager;
    sample.descriptor = call.descriptor;
    return sample;
}

CompletionSample TextureCompletionDecoder::decode_checkpoint(
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint,
    std::uint32_t site_pc) noexcept {
    const auto *info = texture_completion_checkpoint_info(checkpoint);
    auto sample = common_sample(context, checkpoint, site_pc);
    if (info == nullptr) return invalid(sample, CompletionDecodeError::UnknownCheckpoint);
    if (site_pc != info->site_pc) return invalid(sample, CompletionDecodeError::SitePcMismatch);
    if (!valid_word(runtime, context.gpr[29]))
        return invalid(sample, CompletionDecodeError::InvalidGuestPointer);

    if (checkpoint == TextureCompletionCheckpoint::VerbatimBranch ||
        checkpoint == TextureCompletionCheckpoint::DigestSkippedBranch) {
        // Both audited branch labels read the descriptor from gpr16.  Do not
        // search neighbouring registers: a stale pointer there would bind a
        // branch to another operation.
        const std::uint32_t descriptor = context.gpr[16];
        if (!valid_range(runtime, descriptor, kDescriptorBytes))
            return invalid(sample, CompletionDecodeError::InvalidGuestPointer);
        if (checkpoint == TextureCompletionCheckpoint::VerbatimBranch) {
            std::uint8_t flag{};
            if (!read_byte(runtime, descriptor + 27u, flag))
                return invalid(sample, CompletionDecodeError::InvalidGuestPointer);
            if (flag != 0u) return invalid(sample, CompletionDecodeError::BranchMismatch);
            sample.skip_reason = CompletionSkipReason::VerbatimBranch;
        } else {
            // The original tests only the digest byte (lbu 28(s0)); bytes
            // 29..31 are not written by the enqueue and must not be read.
            // The label is reached with the byte clear when the digest is
            // skipped, and with it set from the post-digest jump.
            std::uint8_t flag{};
            if (!read_byte(runtime, descriptor + 28u, flag))
                return invalid(sample, CompletionDecodeError::InvalidGuestPointer);
            sample.skip_reason = flag == 0u ? CompletionSkipReason::DigestNotRequested
                                            : CompletionSkipReason::DigestJoined;
        }
        sample.status = CompletionDecodeStatus::Skip;
        sample.error = CompletionDecodeError::None;
        sample.has_descriptor = true;
        sample.descriptor = descriptor;
        return sample;
    }

    if (info->requires_call_receipt) {
        const auto *call_info = texture_completion_call_boundary_info(info->call_checkpoint);
        if (call_info == nullptr)
            return invalid(sample, CompletionDecodeError::MissingCallReceipt);
        const bool allow_execution_switch =
            checkpoint == TextureCompletionCheckpoint::WorkerWaitReturn;
        auto *receipt = find_receipt(runtime, context, info->call_checkpoint,
                                     call_info->return_pc, allow_execution_switch);
        if (receipt == nullptr)
            return invalid(sample, CompletionDecodeError::MissingCallReceipt);
        if (checkpoint == TextureCompletionCheckpoint::RetirementReturn &&
            !receipt->entered)
            return invalid(sample, CompletionDecodeError::ReturnMismatch);
        if (checkpoint == TextureCompletionCheckpoint::RetirementEntry &&
            receipt->entered)
            return invalid(sample, CompletionDecodeError::DuplicateCallReceipt);
        if (checkpoint == TextureCompletionCheckpoint::RetirementEntry) {
            receipt->entered = true;
        } else {
            sample.call = receipt->call;
            sample.call_checkpoint = receipt->call.checkpoint;
            sample.has_call = true;
            sample.has_manager = receipt->call.has_manager;
            sample.has_descriptor = receipt->call.has_descriptor;
            sample.manager = receipt->call.manager;
            sample.descriptor = receipt->call.descriptor;
            receipt->active = false;
        }
        if (checkpoint == TextureCompletionCheckpoint::RetirementEntry) {
            sample.call = receipt->call;
            sample.call_checkpoint = receipt->call.checkpoint;
            sample.has_call = true;
            sample.has_manager = receipt->call.has_manager;
            sample.has_descriptor = receipt->call.has_descriptor;
            sample.manager = receipt->call.manager;
            sample.descriptor = receipt->call.descriptor;
        }
    } else if (checkpoint == TextureCompletionCheckpoint::GroupCancellation ||
               checkpoint == TextureCompletionCheckpoint::FullQueueCancellation) {
        if (!valid_word(runtime, context.gpr[4]))
            return invalid(sample, CompletionDecodeError::InvalidGuestPointer);
        sample.manager = context.gpr[4];
        sample.has_manager = true;
    } else if (checkpoint == TextureCompletionCheckpoint::WorkerEntry ||
               checkpoint == TextureCompletionCheckpoint::UnsupportedCopyRoute) {
        // Both worker-entry labels receive a pointer in a1 whose first word
        // is the manager identity.  The worker's saved gpr16/gpr17/gpr18
        // values are populated only after this entry edge and are not a
        // reliable source for the closure match.
        if (!valid_word(runtime, context.gpr[5]) ||
            !read_word(runtime, context.gpr[5], sample.manager))
            return invalid(sample, CompletionDecodeError::MissingRequiredField);
        sample.has_manager = true;
    } else if (checkpoint == TextureCompletionCheckpoint::TransformCall ||
               checkpoint == TextureCompletionCheckpoint::DigestCall) {
        // These edges are inside the worker (0x08865378), which keeps the
        // manager in gpr18 and manager + 0x30000 in gpr17.  Both call sites
        // have already loaded the callee arguments a0 = manager and
        // a1 = descriptor (0x08865410/14 and 0x0886542C/34), matching the
        // call-boundary decode of the same instruction.
        if (!valid_word(runtime, context.gpr[4]) ||
            !valid_range(runtime, context.gpr[5], kDescriptorBytes))
            return invalid(sample, CompletionDecodeError::MissingRequiredField);
        sample.manager = context.gpr[4];
        sample.descriptor = context.gpr[5];
        sample.has_manager = true;
        sample.has_descriptor = true;
    } else {
        // These are all audited reader-frame boundaries.  Their
        // manager/descriptor pair is carried in the fixed gpr17/gpr18 fields.
        if (!valid_word(runtime, context.gpr[17]) ||
            !valid_range(runtime, context.gpr[18], kDescriptorBytes))
            return invalid(sample, CompletionDecodeError::MissingRequiredField);
        sample.manager = context.gpr[17];
        sample.descriptor = context.gpr[18];
        sample.has_manager = true;
        sample.has_descriptor = true;
    }
    if (info->has_result) {
        sample.result = context.gpr[2];
        sample.has_result = true;
    }
    sample.status = CompletionDecodeStatus::Observed;
    sample.error = CompletionDecodeError::None;
    return sample;
}

void TextureCompletionDecoder::reset() noexcept {
    for (auto &receipt : receipts_) receipt = Receipt{};
}

std::size_t TextureCompletionDecoder::active_receipts() const noexcept {
    std::size_t result = 0u;
    for (const auto &receipt : receipts_)
        if (receipt.active) ++result;
    return result;
}

} // namespace mhp3rd::native
