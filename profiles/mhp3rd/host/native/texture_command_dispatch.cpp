#include "native/texture_command_dispatch.hpp"

#include <array>
#include <mutex>

namespace mhp3rd::native {
namespace {
struct Slot {
    psprecomp::Runtime *runtime{};
    const TextureCommandDispatch *owner{};
    TextureCommandCallbacks callbacks{};
};
std::mutex mutex;
std::array<Slot, TextureCommandDispatch::kMaxRuntimes> slots{};
}

TextureCommandDispatch::TextureCommandDispatch(psprecomp::Runtime &runtime,
        TextureCommandCallbacks callbacks) : runtime_(&runtime) {
    if (!callbacks.entry || !callbacks.returned) return;
    std::lock_guard lock(mutex);
    Slot *vacant = nullptr;
    for (auto &slot : slots) {
        if (slot.runtime == runtime_) return;
        if (!slot.runtime && !vacant) vacant = &slot;
    }
    if (!vacant) return;
    *vacant = {runtime_, this, callbacks};
    installed_ = true;
}

TextureCommandDispatch::~TextureCommandDispatch() {
    if (!installed_) return;
    std::lock_guard lock(mutex);
    for (auto &slot : slots) {
        if (slot.owner == this && slot.runtime == runtime_) {
            slot = {};
            return;
        }
    }
}

bool texture_command_entry(psprecomp::Runtime &runtime,
        psprecomp::AllegrexContext &context) {
    std::lock_guard lock(mutex);
    for (const auto &slot : slots)
        if (slot.runtime == &runtime)
            return slot.callbacks.entry(slot.callbacks.user, runtime, context);
    return false;
}

void texture_command_return(psprecomp::Runtime &runtime,
        psprecomp::AllegrexContext &context, std::uint32_t return_pc) {
    std::lock_guard lock(mutex);
    for (const auto &slot : slots) {
        if (slot.runtime == &runtime) {
            slot.callbacks.returned(slot.callbacks.user, runtime, context, return_pc);
            return;
        }
    }
}
} // namespace mhp3rd::native
