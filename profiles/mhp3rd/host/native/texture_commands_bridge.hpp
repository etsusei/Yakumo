#pragma once

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstddef>
#include <memory>

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
    Descriptor, InvalidPlan, ConsumedPlan, StaleContext, StaleMemory,
};
struct TextureBridgeResult {
    TextureBridgeError error{TextureBridgeError::None};
    std::size_t blocks{};
    [[nodiscard]] bool ok() const noexcept { return error == TextureBridgeError::None; }
};

enum class TextureBridgeCompareError {
    None, InvalidPlan, ConsumedPlan, Mapping, Context, State, Frame,
    Commands, Source, Dependency,
};
struct TextureBridgeComparison {
    TextureBridgeCompareError error{TextureBridgeCompareError::None};
    [[nodiscard]] bool ok() const noexcept {
        return error == TextureBridgeCompareError::None;
    }
};

// An owned, one-shot prediction. Its fields cannot be changed by callers.
// Moving a plan invalidates the source object. Preparation errors are available
// through error(); no guest state is changed for either outcome.
class TextureCommandPlan {
public:
    TextureCommandPlan() noexcept;
    ~TextureCommandPlan();
    TextureCommandPlan(TextureCommandPlan &&other) noexcept;
    TextureCommandPlan &operator=(TextureCommandPlan &&other) noexcept;
    TextureCommandPlan(const TextureCommandPlan &) = delete;
    TextureCommandPlan &operator=(const TextureCommandPlan &) = delete;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] TextureBridgeError error() const noexcept;
    [[nodiscard]] std::size_t blocks() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TextureBridgeResult result_{TextureBridgeError::InvalidPlan, 0u};

    friend TextureCommandPlan prepare_texture_commands(
        const psprecomp::GuestMemory &, const psprecomp::AllegrexContext &,
        const TextureCommandBounds &);
    friend TextureBridgeResult commit_texture_commands(
        psprecomp::GuestMemory &, psprecomp::AllegrexContext &,
        TextureCommandPlan &);
    friend TextureBridgeComparison compare_texture_commands(
        const TextureCommandPlan &, const psprecomp::GuestMemory &,
        const psprecomp::AllegrexContext &);
};

// The caller must hold authority over the bounds and exclusive guest context
// from preparation through commit. Rechecking bytes is a stale-plan guard, not
// a lock. Commit is bound to the GuestMemory instance used for preparation;
// comparison may use a separate instance containing an original-code run.
// All allocation and validation happen before commit's first guest store.
[[nodiscard]] TextureCommandPlan prepare_texture_commands(
    const psprecomp::GuestMemory &memory,
    const psprecomp::AllegrexContext &context,
    const TextureCommandBounds &bounds);
[[nodiscard]] TextureBridgeResult commit_texture_commands(
    psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &context,
    TextureCommandPlan &plan);

// Compare an actual post-original result with the predicted full CPU context,
// state/frame/selected command bytes and unchanged source/code dependencies.
// This reads guest state only; it never applies or rolls back predicted writes.
[[nodiscard]] TextureBridgeComparison compare_texture_commands(
    const TextureCommandPlan &plan, const psprecomp::GuestMemory &memory,
    const psprecomp::AllegrexContext &context);

// Offline adapter, not a registered game hook. Validates current dependencies
// and all reads/writes before changing memory or context. It uses the same
// prepare/commit pipeline, including stale-plan checks. Rejection preserves
// both. Only bounded, disjoint main-RAM ranges are accepted. GuestMemory
// write-watch diagnostics are outside the atomic rejection contract: a
// throwing host diagnostic during commit is not rolled back.
[[nodiscard]] TextureBridgeResult apply_texture_commands(
    psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &context,
    const TextureCommandBounds &bounds);

} // namespace mhp3rd::native
