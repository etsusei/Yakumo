#pragma once

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

namespace mhp3rd::native {

// Test-only terminal hook for the isolated completion object set. It runs at
// the original RetirementReturn edge; the oracle's interpreter driver stops
// at the same edge and the hook never participates in the app.
bool texture_completion_oracle_stop_after_retirement(
    psprecomp::Runtime &, psprecomp::AllegrexContext &) noexcept;

} // namespace mhp3rd::native
