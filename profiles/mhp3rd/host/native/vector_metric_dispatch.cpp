#include "native/vector_metric_dispatch.hpp"
#include "testing/probes.hpp"
#include <atomic>

namespace mhp3rd::native {
namespace { std::atomic<VectorMetricEntryDispatch> callback{}; }
void set_vector_metric_entry_dispatch(VectorMetricEntryDispatch next) noexcept {
    callback.store(next, std::memory_order_release);
}
bool dispatch_vector_metric_entry(psprecomp::Runtime &runtime,
        psprecomp::AllegrexContext &context, std::uint32_t entry) {
    const auto dispatch = callback.load(std::memory_order_acquire);
    if (!dispatch || testing::NativeProbeAotSuppression::applies(runtime, context, entry)) return false;
    return dispatch(runtime, context, entry);
}
}
