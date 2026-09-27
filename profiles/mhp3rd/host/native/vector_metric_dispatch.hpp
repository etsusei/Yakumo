#pragma once
#include <cstdint>

namespace psprecomp { class Runtime; struct AllegrexContext; }
namespace mhp3rd::native {
using VectorMetricEntryDispatch = bool (*)(psprecomp::Runtime &, psprecomp::AllegrexContext &, std::uint32_t);
// The shared B0 observation build never installs a callback. A null callback
// leaves the original generated entry and all CPU/memory state untouched.
void set_vector_metric_entry_dispatch(VectorMetricEntryDispatch callback) noexcept;
// True means the registered candidate executed this entry and set the return
// PC; the generated caller then resumes its original local-dispatch protocol.
[[nodiscard]] bool dispatch_vector_metric_entry(psprecomp::Runtime &runtime,
    psprecomp::AllegrexContext &context, std::uint32_t entry);
}
