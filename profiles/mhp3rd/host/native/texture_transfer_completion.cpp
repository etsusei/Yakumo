#include "native/texture_transfer_tracker.hpp"

namespace mhp3rd::native {

// Completion callbacks are kept in their own translation unit so the
// completion oracle and future host adapters can link the route without
// changing the read-prefix object set. The tracker remains the owner of all
// state and writer authority.
void observe_texture_completion(TextureTransferTracker &tracker,
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCheckpoint checkpoint,
    std::uint32_t site_pc) noexcept {
    tracker.observe_completion(runtime, context, checkpoint, site_pc);
}

void observe_texture_completion_call(TextureTransferTracker &tracker,
    const psprecomp::Runtime &runtime,
    const psprecomp::AllegrexContext &context,
    TextureCompletionCallCheckpoint checkpoint,
    std::uint32_t site_pc) noexcept {
    tracker.observe_completion_call(runtime, context, checkpoint, site_pc);
}

} // namespace mhp3rd::native
