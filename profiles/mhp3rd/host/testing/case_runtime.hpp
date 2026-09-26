#pragma once

#include "testing/case_controller.hpp"
#include "testing/runtime_diagnostics.hpp"
#include "testing/runtime_recording.hpp"

namespace mhp3rd::testing {
// Optional launch-time setup, before the guest starts. Does not open a window,
// alter native replacement modes, or write any game/save state.
[[nodiscard]] std::shared_ptr<CaseController> start_case_session(
    const RecordingOptions &options, std::shared_ptr<GameObserver> observer,
    std::shared_ptr<RuntimeDiagnostics> diagnostics, std::string build_version);
void close_case_session(const std::shared_ptr<CaseController> &controller, std::string_view reason) noexcept;
} // namespace mhp3rd::testing
