#pragma once

#include "native/texture_command_dispatch.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mhp3rd::native {

// A decoder reports facts at the observation boundary.  Observed means every
// field required by the audited call/return shape was decoded.  Skip is only a
// known branch that deliberately omits a semantic event (for example, the
// verbatim or digest-not-requested branch).  Invalid covers unknown sites,
// changed source shapes, missing receipts, bad guest ranges and ambiguous
// call/return boundaries.  The decoder never decides whether a writer
// completed, and never changes guest memory or CPU state.
enum class CompletionDecodeStatus : std::uint8_t {
    Observed,
    Skip,
    Invalid,
};

enum class CompletionDecodeError : std::uint8_t {
    None,
    UnknownCheckpoint,
    SitePcMismatch,
    InvalidGuestPointer,
    MissingRequiredField,
    UndecodableCallBoundary,
    DuplicateCallReceipt,
    MissingCallReceipt,
    ReturnMismatch,
    BranchMismatch,
    ReceiptCapacity,
};

enum class CompletionSkipReason : std::uint8_t {
    None,
    VerbatimBranch,
    DigestNotRequested,
    UnsupportedCopyRoute,
    // 0x088653B4 is also the join the worker jumps to after a digest call
    // (0x08865448); the descriptor digest byte is then non-zero.
    DigestJoined,
};

// These are the arguments captured after a real call delay slot.  The fields
// are copied into a bounded receipt before the callee can overwrite them.
// manager/descriptor and destination/source/length/footprint are set only when
// the audited call shape supplies those facts and their guest ranges validate.
struct CompletionCallArguments {
    TextureCompletionCallCheckpoint checkpoint{};
    std::uint32_t site_pc{};
    std::uint32_t callee_pc{};
    std::uint32_t return_pc{};
    std::uint32_t sp{};
    std::uint32_t ra{};
    std::uint32_t a0{}, a1{}, a2{}, a3{};
    std::uint32_t manager{}, descriptor{};
    std::uint32_t destination{}, source{}, bytes{}, logical_length{}, footprint{};
    std::uint32_t event{};
    bool has_manager{};
    bool has_descriptor{};
    bool has_destination{};
    bool has_source{};
    bool has_bytes{};
    bool has_logical_length{}, has_footprint{};
    bool has_event{};
    bool after_delay_slot{};
};

// A CompletionSample is a read-only projection of one callback.  For a return
// checkpoint, call contains the saved pre-call arguments, never a reread of
// the return context's a0..a3 registers.
struct CompletionSample {
    CompletionDecodeStatus status{CompletionDecodeStatus::Invalid};
    CompletionDecodeError error{CompletionDecodeError::UnknownCheckpoint};
    CompletionSkipReason skip_reason{CompletionSkipReason::None};
    TextureCompletionCheckpoint checkpoint{};
    TextureCompletionCallCheckpoint call_checkpoint{};
    std::uint32_t site_pc{};
    std::uint32_t sp{}, ra{};
    std::uint32_t a0{}, a1{}, a2{}, a3{};
    std::uint32_t result{};
    std::uint32_t manager{}, descriptor{};
    bool has_result{};
    bool has_manager{};
    bool has_descriptor{};
    bool has_call{};
    CompletionCallArguments call{};
};

struct CompletionCheckpointInfo {
    TextureCompletionCheckpoint checkpoint{};
    std::uint32_t site_pc{};
    bool has_result{};
    bool may_skip{};
    bool requires_call_receipt{};
    bool retains_call_receipt{};
    TextureCompletionCallCheckpoint call_checkpoint{};
};

struct CompletionCallBoundaryInfo {
    TextureCompletionCallCheckpoint checkpoint{};
    std::uint32_t site_pc{};
    std::uint32_t callee_pc{};
    std::uint32_t return_pc{};
    TextureCompletionCheckpoint return_checkpoint{};
    bool has_return_checkpoint{};
};

// These tables are the auditable source of truth for decoder site identity.
// Instrumenters retain the constants in generated callbacks; callers must not
// substitute context.pc, which may still name an outer dispatch.
[[nodiscard]] std::span<const CompletionCheckpointInfo>
texture_completion_checkpoint_table() noexcept;
[[nodiscard]] std::span<const CompletionCallBoundaryInfo>
texture_completion_call_boundary_table() noexcept;

[[nodiscard]] const CompletionCheckpointInfo *texture_completion_checkpoint_info(
    TextureCompletionCheckpoint checkpoint) noexcept;
[[nodiscard]] const CompletionCallBoundaryInfo *texture_completion_call_boundary_info(
    TextureCompletionCallCheckpoint checkpoint) noexcept;

class TextureCompletionDecoder final {
public:
    static constexpr std::size_t kMaxCallReceipts = 16u;

    TextureCompletionDecoder() noexcept = default;
    TextureCompletionDecoder(const TextureCompletionDecoder &) = delete;
    TextureCompletionDecoder &operator=(const TextureCompletionDecoder &) = delete;

    // Called by the separate dispatch callback emitted after the call's delay
    // slot.  A successful result stores a receipt for the matching return.
    [[nodiscard]] CompletionSample decode_call(
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context,
        TextureCompletionCallCheckpoint checkpoint,
        std::uint32_t site_pc) noexcept;

    // Called by the existing completion checkpoint callback.  Return points
    // consume their matching receipt; branch points only project current facts.
    [[nodiscard]] CompletionSample decode_checkpoint(
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context,
        TextureCompletionCheckpoint checkpoint,
        std::uint32_t site_pc) noexcept;

    // Short aliases keep call sites independent of whether they process a
    // return/branch checkpoint or the separate call-boundary channel.
    [[nodiscard]] CompletionSample decode(
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context,
        TextureCompletionCheckpoint checkpoint,
        std::uint32_t site_pc) noexcept {
        return decode_checkpoint(runtime, context, checkpoint, site_pc);
    }
    [[nodiscard]] CompletionSample capture_call(
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context,
        TextureCompletionCallCheckpoint checkpoint,
        std::uint32_t site_pc) noexcept {
        return decode_call(runtime, context, checkpoint, site_pc);
    }

    void reset() noexcept;
    [[nodiscard]] std::size_t active_receipts() const noexcept;

private:
    struct Receipt {
        bool active{};
        const psprecomp::Runtime *runtime{};
        const psprecomp::AllegrexContext *context{};
        psprecomp::RuntimeExecutionContextToken execution{};
        CompletionCallArguments call{};
        bool entered{};
    };

    std::array<Receipt, kMaxCallReceipts> receipts_{};

    [[nodiscard]] CompletionSample common_sample(
        const psprecomp::AllegrexContext &context,
        TextureCompletionCheckpoint checkpoint,
        std::uint32_t site_pc) const noexcept;
    [[nodiscard]] CompletionSample invalid(
        CompletionSample sample, CompletionDecodeError error) const noexcept;
    [[nodiscard]] bool valid_word(const psprecomp::Runtime &runtime,
                                  std::uint32_t address) const noexcept;
    [[nodiscard]] bool valid_range(const psprecomp::Runtime &runtime,
                                  std::uint32_t address,
                                  std::uint32_t bytes) const noexcept;
    [[nodiscard]] bool read_word(const psprecomp::Runtime &runtime,
                                 std::uint32_t address,
                                 std::uint32_t &value) const noexcept;
    [[nodiscard]] bool read_byte(const psprecomp::Runtime &runtime,
                                 std::uint32_t address,
                                 std::uint8_t &value) const noexcept;
    [[nodiscard]] Receipt *find_receipt(
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context,
        TextureCompletionCallCheckpoint checkpoint,
        std::uint32_t return_pc,
        bool allow_execution_switch) noexcept;
    [[nodiscard]] const Receipt *find_receipt(
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context,
        TextureCompletionCallCheckpoint checkpoint,
        std::uint32_t return_pc,
        bool allow_execution_switch) const noexcept;
};

} // namespace mhp3rd::native
