#pragma once

#include "native/texture_command_dispatch.hpp"
#include "psprecomp/runtime.hpp"
#include "resources/source_authority.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mhp3rd::native {

// This callback must check current executable bytes, including the installed
// overlay, against the configured identity. Numeric SourceCodeIdentity fields
// alone are not a code validation. The callback must not mutate guest state.
struct TextureLifetimeTrackerConfig {
    std::uint32_t owner_allocator{};
    std::uint32_t command_allocator{};
    resources::SourceCodeIdentity code{};
    std::uint32_t max_command_blocks{4096u};
    bool (*current_code)(void *, const psprecomp::Runtime &,
                         const psprecomp::AllegrexContext &,
                         const resources::SourceCodeIdentity &) noexcept{};
    void *current_code_user{};
};

enum class TextureLifetimeTrackerError {
    None,
    InvalidConfig,
    InvalidObservation,
    MissingPhase,
    ConflictingPhase,
    CodeChanged,
    AuthorityRejected,
    NoCapacity,
    CounterExhausted,
    ObservationLost,
};

// A factual receipt issued once per observed frame. This value is copyable,
// not an execution capability: the controller must use it only at the current
// builder entry and still obtain/revalidate source authority before any commit.
struct TextureBuilderTicket {
    resources::AuthorityToken owner{}, command{};
    std::uint32_t raw_owner{}, raw_owner_allocation{};
    std::uint32_t raw_child{}, raw_command{}, requested_command_bytes{};
    const psprecomp::Runtime *runtime{};
    const psprecomp::AllegrexContext *context{};
    psprecomp::RuntimeExecutionContextToken execution{};
    std::uint32_t caller_sp{};
    resources::SourceCodeIdentity code{};
};

struct TextureLifetimeTrackerStats {
    std::uint64_t checkpoints{}, owners_constructed{}, commands_allocated{};
    std::uint64_t owners_released{}, commands_released{};
    std::uint64_t tickets_armed{}, tickets_consumed{}, ineligible{};
    std::uint64_t errors{}, losses{};
    std::size_t live_owners{}, live_commands{}, active_frames{};
};

// Runtime-owned, allocation-free observation state. The dispatch registry must
// serialize all calls with every other use of the SourceAuthority. It does not
// exclude guest writers during a later permit/prepare/commit interval.
class TextureLifetimeTracker final {
public:
    static constexpr std::size_t kMaxLeases = 32u;
    static constexpr std::size_t kMaxFrames = 16u;

    TextureLifetimeTracker(psprecomp::Runtime &runtime,
                           resources::SourceAuthority &authority,
                           TextureLifetimeTrackerConfig config) noexcept;

    void observe(const psprecomp::Runtime &runtime,
                 const psprecomp::AllegrexContext &context,
                 TextureLifetimeCheckpoint checkpoint) noexcept;
    [[nodiscard]] std::optional<TextureBuilderTicket> consume_builder_ticket(
        const psprecomp::Runtime &runtime,
        const psprecomp::AllegrexContext &context) noexcept;

    // Queries accept cached/uncached aliases, but tickets retain every raw
    // pointer actually observed at the selected call edges.
    [[nodiscard]] std::optional<resources::AuthorityToken> owner_token(
        std::uint32_t raw) const noexcept;
    // Reset changes source eligibility without freeing the owner allocation.
    // Read attempts snapshot this per-lease generation so reset cannot be
    // mistaken for an unchanged request when the allocation token survives.
    [[nodiscard]] std::optional<std::uint64_t> owner_invalidation_generation(
        std::uint32_t raw) const noexcept;
    // Resolve a live owner lease whose fixed selector-7 slot overlaps a
    // checked guest interval. Used to fail closed when a transfer checkpoint
    // is missing before the transfer tracker has watched the owner.
    [[nodiscard]] bool selected_slot_owner(std::uint32_t raw,
        std::uint32_t bytes, std::uint32_t &raw_owner,
        resources::AuthorityToken &owner) const noexcept;
    [[nodiscard]] std::optional<resources::AuthorityToken> command_token(
        std::uint32_t raw) const noexcept;
    [[nodiscard]] TextureLifetimeTrackerStats stats() const noexcept;
    [[nodiscard]] TextureLifetimeTrackerError error() const noexcept { return error_; }

private:
    enum class LeaseKind { Empty, Owner, Command };
    struct Lease {
        LeaseKind kind{LeaseKind::Empty};
        resources::AuthorityToken token{};
        std::uint32_t raw{}, bytes{}, manager{};
        std::uint64_t invalidation_generation{1u};
    };
    enum class FrameKind { Empty, Factory, Caller };
    enum class FramePhase {
        AllocationResult,
        ConstructorCall,
        ConstructorResult,
        ProviderResult,
        ChildResult,
        ForwardAllocate,
        CommandResult,
        BuilderCall,
        BuilderEntry,
    };
    struct Frame {
        FrameKind kind{FrameKind::Empty};
        FramePhase phase{FramePhase::AllocationResult};
        const psprecomp::Runtime *runtime{};
        const psprecomp::AllegrexContext *context{};
        psprecomp::RuntimeExecutionContextToken execution{};
        std::uint32_t sp{}, owner_manager{}, command_manager{};
        std::uint32_t raw_owner{}, raw_owner_allocation{}, raw_root{};
        std::uint32_t raw_child{}, raw_command{}, requested_bytes{};
        resources::AuthorityToken owner{}, command{};
        bool eligible{};
    };

    psprecomp::Runtime *runtime_{};
    resources::SourceAuthority *authority_{};
    TextureLifetimeTrackerConfig config_{};
    TextureLifetimeTrackerError error_{TextureLifetimeTrackerError::None};
    TextureLifetimeTrackerStats stats_{};
    std::array<Lease, kMaxLeases> leases_{};
    std::array<Frame, kMaxFrames> frames_{};

    void observe_impl(const psprecomp::Runtime &runtime,
                      const psprecomp::AllegrexContext &context,
                      TextureLifetimeCheckpoint checkpoint);
    void fail(TextureLifetimeTrackerError error, bool loss) noexcept;
    [[nodiscard]] bool count(std::uint64_t &counter) noexcept;
    [[nodiscard]] bool code_ok(const psprecomp::Runtime &runtime,
                               const psprecomp::AllegrexContext &context) noexcept;
    [[nodiscard]] Frame *find_frame(FrameKind kind,
                                    const psprecomp::Runtime &runtime,
                                    const psprecomp::AllegrexContext &context) noexcept;
    [[nodiscard]] Frame *new_frame(FrameKind kind,
                                   const psprecomp::Runtime &runtime,
                                   const psprecomp::AllegrexContext &context) noexcept;
    [[nodiscard]] Lease *find_lease(LeaseKind kind, std::uint32_t raw) noexcept;
    [[nodiscard]] const Lease *find_lease(LeaseKind kind,
                                          std::uint32_t raw) const noexcept;
    [[nodiscard]] Lease *new_lease() noexcept;
    void release_lease(Lease &lease);
    void drop_frame(Frame &frame) noexcept;
    void forget_frames(resources::AuthorityToken token) noexcept;
    [[nodiscard]] bool read_word(const psprecomp::Runtime &runtime,
                                 std::uint32_t raw, std::uint32_t &value) const noexcept;
    [[nodiscard]] bool valid_ram(const psprecomp::Runtime &runtime,
                                 std::uint32_t raw, std::uint32_t bytes) const noexcept;
};

} // namespace mhp3rd::native
