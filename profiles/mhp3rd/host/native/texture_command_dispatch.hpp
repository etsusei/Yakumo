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

} // namespace mhp3rd::native
