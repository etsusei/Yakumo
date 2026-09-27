#pragma once

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstddef>

namespace mhp3rd::native {

// The caller must establish these allocation bounds; mapped RAM alone does
// not prove the lifetime or extent of a resource or command allocation.
struct TextureCommandBounds {
    std::size_t source_bytes{};
    std::size_t destination_slots{};
    std::size_t max_blocks{4096u};
};
enum class TextureBridgeError {
    None, Entry, Dependency, Budget, Range, Alignment, Aliasing, Resource,
    Descriptor,
};
struct TextureBridgeResult {
    TextureBridgeError error{TextureBridgeError::None};
    std::size_t blocks{};
    [[nodiscard]] bool ok() const noexcept { return error == TextureBridgeError::None; }
};

// Offline adapter, not a registered game hook. Validates current dependencies
// and all reads/writes before changing memory or context. Rejection preserves
// both. Only bounded, disjoint main-RAM ranges are accepted. The caller must
// exclude concurrent mutation for the entire call. Adapter-owned allocations
// precede mutation. GuestMemory write-watch diagnostics are outside the atomic
// rejection contract: a throwing host diagnostic during commit is not rolled
// back. The offline gate runs with write watches disabled.
[[nodiscard]] TextureBridgeResult apply_texture_commands(
    psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &context,
    const TextureCommandBounds &bounds);

} // namespace mhp3rd::native
