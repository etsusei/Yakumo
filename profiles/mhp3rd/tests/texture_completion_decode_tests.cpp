#include "native/texture_completion_decode.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace mhp3rd::native;
using psprecomp::AllegrexContext;
using psprecomp::Runtime;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

AllegrexContext context() {
    AllegrexContext result{};
    result.gpr[29] = 0x08401000u;
    result.gpr[17] = 0x08402000u;
    result.gpr[18] = 0x08403000u;
    return result;
}

void write_descriptor(Runtime &runtime, std::uint32_t address, std::uint8_t transform,
                      std::uint32_t digest) {
    for (std::uint32_t offset = 0; offset < 32u; ++offset)
        runtime.memory().store8(address + offset, 0u);
    runtime.memory().store8(address + 27u, transform);
    runtime.memory().store32(address + 28u, digest);
}

void call_receipt_is_used_for_return() {
    Runtime runtime;
    TextureCompletionDecoder decoder;
    auto call_context = context();
    call_context.gpr[31] = 0x088652ACu;
    call_context.gpr[4] = 0x08404000u;
    call_context.gpr[5] = 0x08405000u;
    call_context.gpr[6] = 16u;
    call_context.gpr[16] = 0x08402000u;
    call_context.gpr[17] = 0x08403000u;
    const auto call = decoder.decode_call(runtime, call_context,
        TextureCompletionCallCheckpoint::InlineCopyCall, 0x088652A4u);
    require(call.status == CompletionDecodeStatus::Observed && call.has_call,
            "valid inline call was not observed");
    require(call.call.after_delay_slot && call.call.a0 == 0x08404000u &&
            call.call.a1 == 0x08405000u && call.call.bytes == 16u &&
            call.call.source == 0x08405000u && call.call.logical_length == 16u &&
            call.call.footprint == 16u,
            "call receipt did not retain post-delay arguments");

    auto &return_context = call_context;
    return_context.gpr[4] = 0x08406000u;
    return_context.gpr[5] = 0x08407000u;
    return_context.gpr[6] = 1u;
    return_context.gpr[2] = 0xABCD1234u;
    const auto returned = decoder.decode_checkpoint(runtime, return_context,
        TextureCompletionCheckpoint::CopyReturn, 0x088652ACu);
    require(returned.status == CompletionDecodeStatus::Observed && returned.has_call &&
            returned.has_result && returned.result == 0xABCD1234u,
            "inline return was not observed");
    require(returned.call.a0 == 0x08404000u && returned.call.a1 == 0x08405000u &&
            returned.call.bytes == 16u,
            "return reread overwritten argument registers");
    require(decoder.active_receipts() == 0u, "return did not consume the call receipt");

    const auto duplicate = decoder.decode_checkpoint(runtime, return_context,
        TextureCompletionCheckpoint::CopyReturn, 0x088652ACu);
    require(duplicate.status == CompletionDecodeStatus::Invalid &&
            duplicate.error == CompletionDecodeError::MissingCallReceipt,
            "unpaired return was not invalid");
}

void invalid_sites_and_pointers_fail_closed() {
    Runtime runtime;
    TextureCompletionDecoder decoder;
    auto call_context = context();
    call_context.gpr[31] = 0x088652ACu;
    call_context.gpr[4] = 0x09FFFFF8u;
    call_context.gpr[5] = 0x08405000u;
    call_context.gpr[6] = 16u;
    call_context.gpr[16] = 0x08402000u;
    call_context.gpr[17] = 0x08403000u;
    const auto bad_pointer = decoder.decode_call(runtime, call_context,
        TextureCompletionCallCheckpoint::InlineCopyCall, 0x088652A4u);
    require(bad_pointer.status == CompletionDecodeStatus::Invalid &&
            bad_pointer.error == CompletionDecodeError::InvalidGuestPointer,
            "invalid guest pointer was accepted");

    auto valid = context();
    const auto bad_site = decoder.decode_checkpoint(runtime, valid,
        TextureCompletionCheckpoint::ClassifierReturn, 0x08865778u);
    require(bad_site.status == CompletionDecodeStatus::Invalid &&
            bad_site.error == CompletionDecodeError::SitePcMismatch,
            "changed site PC was accepted");
}

void skip_is_reserved_for_known_branches() {
    Runtime runtime;
    TextureCompletionDecoder decoder;
    constexpr std::uint32_t descriptor = 0x08403000u;
    write_descriptor(runtime, descriptor, 0u, 0u);
    auto context_value = context();
    context_value.gpr[16] = descriptor;
    const auto verbatim = decoder.decode_checkpoint(runtime, context_value,
        TextureCompletionCheckpoint::VerbatimBranch, 0x088653A0u);
    require(verbatim.status == CompletionDecodeStatus::Skip &&
            verbatim.skip_reason == CompletionSkipReason::VerbatimBranch,
            "known verbatim branch was not skipped");

    runtime.memory().store8(descriptor + 27u, 1u);
    const auto unexpected = decoder.decode_checkpoint(runtime, context_value,
        TextureCompletionCheckpoint::VerbatimBranch, 0x088653A0u);
    require(unexpected.status == CompletionDecodeStatus::Invalid &&
            unexpected.error == CompletionDecodeError::BranchMismatch,
            "transform branch was incorrectly treated as skip");

    constexpr std::uint32_t manager = 0x08402000u;
    constexpr std::uint32_t manager_holder = 0x08405000u;
    runtime.memory().store32(manager_holder, manager);
    context_value.gpr[5] = manager_holder;
    const auto unsupported = decoder.decode_checkpoint(runtime, context_value,
        TextureCompletionCheckpoint::UnsupportedCopyRoute, 0x088652C4u);
    require(unsupported.status == CompletionDecodeStatus::Observed &&
            unsupported.has_manager && unsupported.manager == manager &&
            !unsupported.has_descriptor,
            "known unsupported route was not observed");
}

void transform_call_uses_wrapper_abi_and_retains_payload() {
    Runtime runtime;
    TextureCompletionDecoder decoder;
    constexpr std::uint32_t manager = 0x08404000u;
    constexpr std::uint32_t descriptor = 0x08405000u;
    constexpr std::uint32_t destination = 0x08406000u;
    write_descriptor(runtime, descriptor, 1u, 0u);
    runtime.memory().store32(descriptor + 4u, destination);
    runtime.memory().store32(descriptor + 8u, 5u);
    runtime.memory().store32(descriptor + 12u, 4u);

    auto call_context = context();
    call_context.gpr[4] = manager;
    call_context.gpr[5] = descriptor;
    call_context.gpr[6] = 0xDEAD0001u;
    call_context.gpr[7] = 0xDEAD0002u;
    call_context.gpr[17] = 0x08402000u;
    call_context.gpr[18] = 0x08403000u;
    call_context.gpr[31] = 0x08865428u;
    const auto call = decoder.decode_call(runtime, call_context,
        TextureCompletionCallCheckpoint::TransformCall, 0x08865420u);
    require(call.status == CompletionDecodeStatus::Observed && call.has_call,
            "transform wrapper call was not observed");
    require(call.call.has_manager && call.call.manager == manager &&
            call.call.has_descriptor && call.call.descriptor == descriptor &&
            call.call.has_destination && call.call.destination == destination + 4u &&
            call.call.has_logical_length && call.call.logical_length == 5u &&
            call.call.has_footprint && call.call.footprint == 8u,
            "transform wrapper did not retain audited ABI payload");

    call_context.gpr[4] = 0x08407000u;
    call_context.gpr[5] = 0x08408000u;
    call_context.gpr[17] = 0x08409000u;
    call_context.gpr[18] = 0x0840A000u;
    call_context.gpr[2] = 0x12345678u;
    const auto returned = decoder.decode_checkpoint(runtime, call_context,
        TextureCompletionCheckpoint::TransformReturn, 0x08865428u);
    require(returned.status == CompletionDecodeStatus::Observed && returned.has_call &&
            returned.call.manager == manager && returned.call.descriptor == descriptor &&
            returned.call.destination == destination + 4u &&
            returned.call.logical_length == 5u && returned.call.footprint == 8u,
            "transform return did not retain the saved call ABI payload");

    call_context.gpr[4] = manager;
    call_context.gpr[5] = descriptor;
    call_context.gpr[17] = 0x08402000u;
    call_context.gpr[18] = 0x08403000u;
    call_context.gpr[31] = 0x08865448u;
    const auto digest = decoder.decode_call(runtime, call_context,
        TextureCompletionCallCheckpoint::DigestCall, 0x08865440u);
    require(digest.status == CompletionDecodeStatus::Observed && digest.has_call &&
            digest.call.manager == manager && digest.call.descriptor == descriptor &&
            digest.call.destination == destination + 4u &&
            digest.call.logical_length == 5u,
            "digest wrapper did not retain its separately audited ABI payload");
}

void retirement_entry_keeps_receipt_until_return() {
    Runtime runtime;
    TextureCompletionDecoder decoder;
    write_descriptor(runtime, 0x08403000u, 0u, 0u);
    auto call_context = context();
    call_context.gpr[31] = 0x0886581Cu;
    const auto call = decoder.decode_call(runtime, call_context,
        TextureCompletionCallCheckpoint::RetirementCall, 0x08865814u);
    require(call.status == CompletionDecodeStatus::Observed,
            "retirement call was not observed");
    const auto entered = decoder.decode_checkpoint(runtime, call_context,
        TextureCompletionCheckpoint::RetirementEntry, 0x08865D8Cu);
    require(entered.status == CompletionDecodeStatus::Observed && entered.has_call &&
            decoder.active_receipts() == 1u,
            "retirement entry consumed its return receipt");
    const auto returned = decoder.decode_checkpoint(runtime, call_context,
        TextureCompletionCheckpoint::RetirementReturn, 0x0886581Cu);
    require(returned.status == CompletionDecodeStatus::Observed && returned.has_call &&
            decoder.active_receipts() == 0u,
            "retirement return did not consume its saved call receipt");
}

void worker_edges_use_worker_frame_registers() {
    // Inside the worker (0x08865378) gpr18 is the manager, gpr16 the
    // descriptor and gpr17 manager + 0x30000; a0/a1 carry the call arguments.
    Runtime runtime;
    TextureCompletionDecoder decoder;
    constexpr std::uint32_t manager = 0x08410000u;
    constexpr std::uint32_t descriptor = 0x08403000u;
    write_descriptor(runtime, descriptor, 1u, 0u);
    auto worker = context();
    worker.gpr[4] = manager;
    worker.gpr[5] = descriptor;
    worker.gpr[16] = descriptor;
    worker.gpr[17] = manager + 0x30000u;
    worker.gpr[18] = manager;
    for (const auto [checkpoint, site] : {
             std::pair{TextureCompletionCheckpoint::TransformCall, 0x08865420u},
             std::pair{TextureCompletionCheckpoint::DigestCall, 0x08865440u}}) {
        const auto edge = decoder.decode_checkpoint(runtime, worker, checkpoint, site);
        require(edge.status == CompletionDecodeStatus::Observed &&
                edge.has_manager && edge.manager == manager &&
                edge.has_descriptor && edge.descriptor == descriptor,
                "worker call edge did not decode manager/descriptor from a0/a1");
    }

    // The digest join is tested with lbu 28(s0): only byte 28 selects the
    // skipped or post-digest meaning, and bytes 29..31 are ignored.
    runtime.memory().store32(descriptor + 28u, 0xFFFFFF00u);
    const auto skipped = decoder.decode_checkpoint(runtime, worker,
        TextureCompletionCheckpoint::DigestSkippedBranch, 0x088653B4u);
    require(skipped.status == CompletionDecodeStatus::Skip &&
            skipped.skip_reason == CompletionSkipReason::DigestNotRequested,
            "digest join read bytes beyond the audited flag byte");
    runtime.memory().store8(descriptor + 28u, 1u);
    const auto joined = decoder.decode_checkpoint(runtime, worker,
        TextureCompletionCheckpoint::DigestSkippedBranch, 0x088653B4u);
    require(joined.status == CompletionDecodeStatus::Skip &&
            joined.skip_reason == CompletionSkipReason::DigestJoined,
            "post-digest join was not distinguished from a skipped digest");

    worker.gpr[4] = 0x0000ABCDu;
    worker.gpr[5] = 0xFFFFFFFEu;
    worker.gpr[31] = 0x088653C4u;
    const auto ack = decoder.decode_call(runtime, worker,
        TextureCompletionCallCheckpoint::WorkerAckCall, 0x088653BCu);
    require(ack.status == CompletionDecodeStatus::Observed && ack.has_call &&
            ack.call.has_event && ack.call.event == 0x0000ABCDu &&
            ack.call.has_manager && ack.call.manager == manager &&
            ack.call.has_descriptor && ack.call.descriptor == descriptor,
            "worker acknowledgement used reader-frame identity registers");
}

} // namespace

int main() {
    try {
        call_receipt_is_used_for_return();
        invalid_sites_and_pointers_fail_closed();
        skip_is_reserved_for_known_branches();
        transform_call_uses_wrapper_abi_and_retains_payload();
        retirement_entry_keeps_receipt_until_return();
        worker_edges_use_worker_frame_registers();
        std::cout << "PASS texture completion decoder\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL texture completion decoder: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
