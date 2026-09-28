#pragma once

namespace psprecomp { class Runtime; struct AllegrexContext; }

namespace mhp3rd::native {

// Test-only hook referenced only by the separately compiled read-prefix
// oracle copies. The application-generated object set never includes it.
bool texture_read_oracle_stop_after_result(
    psprecomp::Runtime &runtime,
    psprecomp::AllegrexContext &context) noexcept;

} // namespace mhp3rd::native
