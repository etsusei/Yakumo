#pragma once

#include "testing/state_observation.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace psprecomp { class GuestMemory; }
namespace mhp3rd::testing {

// Empty/off selects no leaves. Names are deliberately stable across builds.
// Invalid configuration throws before publishing any probe session.
[[nodiscard]] std::uint32_t parse_probe_selection(std::string_view names);

// Called on the emulation thread, with close after it has stopped. The caller
// owns the memory lifetime until close; close never reads guest memory.
class RuntimeDiagnostics {
public:
    RuntimeDiagnostics(std::shared_ptr<GameObserver> observer,
                       const psprecomp::GuestMemory &memory,
                       bool supported_executable, std::uint32_t probe_mask);
    ~RuntimeDiagnostics();
    RuntimeDiagnostics(const RuntimeDiagnostics &) = delete;
    RuntimeDiagnostics &operator=(const RuntimeDiagnostics &) = delete;
    void tick(const perf::Summary &performance) noexcept;
    void close() noexcept;
private:
    std::shared_ptr<GameObserver> observer_;
    const psprecomp::GuestMemory *memory_{};
    bool supported_executable_{};
    bool closed_{};
    bool sampled_{};
    std::chrono::steady_clock::time_point previous_{};
    PerformanceObservation performance_;
};
} // namespace mhp3rd::testing
