#pragma once

#include <cstddef>
#include <cstdint>

namespace psprecomp { class Runtime; struct AllegrexContext; }

namespace mhp3rd::native {

// Values identify actual before-instruction checkpoints, not ctx.pc (which
// can still name an outer compiled dispatch). Results are observed before the
// named continuation instruction consumes the callee's registers.
enum class TextureLifetimeCheckpoint : std::uint32_t {
    HeapInit = 0x08879DA4u, HeapReset = 0x08879D58u,
    ForwardAllocate = 0x08879DB4u, ReverseAllocate = 0x08879F08u,
    Free = 0x08879FF0u, OwnerReset = 0x088A53C8u,
    CallerTail = 0x088B0398u, ProviderResult = 0x088B03B0u,
    ChildResult = 0x088B03BCu, CommandAllocationResult = 0x088B03E4u,
    BuilderCall = 0x088B03F8u, FactoryAllocationResult = 0x088BD07Cu,
    FactoryConstructorCall = 0x088BD13Cu, FactoryConstructorResult = 0x088BD144u,
};

// Transfer observations are dispatched separately from lifetime checkpoints.
// Keeping them in distinct callback families prevents a new descriptor event
// from reaching TextureLifetimeTracker's intentionally fail-closed default.
enum class TextureTransferCheckpoint : std::uint32_t {
    OwnerLoadEntry = 0x088A5470u,
    OwnerLoadTail = 0x088A54ECu,
    EnqueueEntry = 0x08863CDCu,
    DescriptorCommit = 0x08863DD8u,
    EnqueueReturn = 0x08863E28u,
};

// Read-side callbacks are separate from both lifetime and enqueue callbacks.
// They report the original state-8 request, helper, import and result edges;
// none of these events implies a completed destination write.
enum class TextureReadCheckpoint : std::uint32_t {
    WorkerState8Entry = 0x088654D4u,
    ReadHelperEntry = 0x08863608u,
    ReadInvoke = 0x0886365Cu,
    ReadResult = 0x0886551Cu,
};

// Completion callbacks cover the selected inline state-8 route after an
// exact read. They are independent from read callbacks so the R1 read-prefix
// stop can remain unchanged while the completion oracle runs to retirement.
enum class TextureCompletionCheckpoint : std::uint32_t {
    ClassifierReturn = 0x0886577Cu,
    CopyReturn = 0x088652ACu,
    HelperReturn = 0x088659B4u,
    PolicyReturn = 0x088659C4u,
    WorkerRequestCall = 0x088659CCu,
    WorkerWaitReturn = 0x08865A10u,
    WorkerEntry = 0x08865378u,
    VerbatimBranch = 0x088653A0u,
    DigestSkippedBranch = 0x088653B4u,
    TransformCall = 0x08865420u,
    TransformReturn = 0x08865428u,
    DigestCall = 0x08865440u,
    DigestReturn = 0x08865448u,
    WorkerAckReturn = 0x088653C4u,
    RetirementCall = 0x08865814u,
    RetirementEntry = 0x08865D8Cu,
    RetirementReturn = 0x0886581Cu,
    GroupCancellation = 0x08866044u,
    FullQueueCancellation = 0x08865F00u,
    UnsupportedCopyRoute = 0x088652C4u,
    WorkerCopyReturn = 0x08865368u,
    WorkerRequestReturn = 0x088659E4u,
    WorkerEventSetReturn = 0x088659F4u,
};

// Call-boundary observations are emitted after the original call's delay
// slot, while a0..a3 and the callee return address still contain the actual
// arguments.  They are intentionally a separate callback family: existing
// completion checkpoints are return/branch observations and their ABI stays
// stable for the tracker and the lower oracle.
enum class TextureCompletionCallCheckpoint : std::uint32_t {
    InlineCopyCall = 0x088652A4u,
    HelperCall = 0x088659ACu,
    WorkerCopyCall = 0x08865360u,
    WorkerAckCall = 0x088653BCu,
    TransformCall = 0x08865420u,
    DigestCall = 0x08865440u,
    RetirementCall = 0x08865814u,
    PolicyCall = 0x088659BCu,
    WorkerRequestCall = 0x088659DCu,
    WorkerEventSetCall = 0x088659ECu,
    WorkerWaitCall = 0x08865A08u,
};

// Callbacks may prepare/compare a plan but must not dispatch guest code,
// register/unregister callbacks, or let exceptions cross this boundary. A
// false entry result must preserve guest state. True means the entry has been
// handled and context.pc names its continuation (or Runtime has stopped).
struct TextureCommandCallbacks {
    void *user{};
    bool (*entry)(void *, psprecomp::Runtime &, psprecomp::AllegrexContext &) noexcept{};
    void (*returned)(void *, psprecomp::Runtime &, psprecomp::AllegrexContext &,
                     std::uint32_t return_pc) noexcept{};
    void (*lifetime)(void *, const psprecomp::Runtime &, const psprecomp::AllegrexContext &,
                     TextureLifetimeCheckpoint) noexcept{};
    void (*transfer)(void *, const psprecomp::Runtime &, const psprecomp::AllegrexContext &,
                     TextureTransferCheckpoint) noexcept{};
    void (*read)(void *, const psprecomp::Runtime &, const psprecomp::AllegrexContext &,
                 TextureReadCheckpoint) noexcept{};
    void (*completion)(void *, const psprecomp::Runtime &,
                      const psprecomp::AllegrexContext &,
                      TextureCompletionCheckpoint,
                      std::uint32_t site_pc) noexcept{};
    void (*completion_call)(void *, const psprecomp::Runtime &,
                           const psprecomp::AllegrexContext &,
                           TextureCompletionCallCheckpoint,
                           std::uint32_t site_pc) noexcept{};
};

// Bounded, runtime-specific callback ownership. Construct after Runtime and
// destroy before it, only while guest execution is quiescent. One binding per
// Runtime is accepted. Callback execution is serialized with registration and
// destruction; the registry holds no allocation/resource authority itself.
// A failed registration never replaces an existing owner.
class TextureCommandDispatch final {
public:
    static constexpr std::size_t kMaxRuntimes = 16u;
    TextureCommandDispatch(psprecomp::Runtime &runtime, TextureCommandCallbacks callbacks);
    ~TextureCommandDispatch();
    TextureCommandDispatch(const TextureCommandDispatch &) = delete;
    TextureCommandDispatch &operator=(const TextureCommandDispatch &) = delete;
    [[nodiscard]] bool installed() const noexcept { return installed_; }

private:
    psprecomp::Runtime *runtime_{};
    bool installed_{};
};

// Null registration leaves the original generated path and guest state intact.
// Entry and return context.pc may name an outer dispatch; callers must project
// the certified entry / supplied return_pc into a copy for ABI comparison.
[[nodiscard]] bool texture_command_entry(psprecomp::Runtime &runtime,
                                         psprecomp::AllegrexContext &context);
void texture_command_return(psprecomp::Runtime &runtime,
                            psprecomp::AllegrexContext &context,
                            std::uint32_t return_pc);
void texture_lifetime_checkpoint(psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context, TextureLifetimeCheckpoint checkpoint);
void texture_transfer_checkpoint(psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context, TextureTransferCheckpoint checkpoint);
void texture_read_checkpoint(psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context, TextureReadCheckpoint checkpoint);
void texture_completion_checkpoint(psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint, std::uint32_t site_pc);
void texture_completion_call_boundary(psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCallCheckpoint checkpoint, std::uint32_t site_pc);

} // namespace mhp3rd::native
