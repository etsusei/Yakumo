#pragma once

#include <cstddef>
#include <cstdint>

namespace psprecomp { class Runtime; struct AllegrexContext; }

namespace mhp3rd::native {

// Callbacks may prepare/compare a plan but must not dispatch guest code,
// register/unregister callbacks, or let exceptions cross this boundary. A
// false entry result must preserve guest state. True means the entry has been
// handled and context.pc names its continuation (or Runtime has stopped).
struct TextureCommandCallbacks {
    void *user{};
    bool (*entry)(void *, psprecomp::Runtime &, psprecomp::AllegrexContext &) noexcept{};
    void (*returned)(void *, psprecomp::Runtime &, psprecomp::AllegrexContext &,
                     std::uint32_t return_pc) noexcept{};
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

} // namespace mhp3rd::native
