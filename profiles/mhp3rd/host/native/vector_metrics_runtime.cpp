#include "native/vector_metrics_runtime.hpp"
#include "native/vector_metric_dispatch.hpp"

#include "mhp3rd_profile.hpp"
#include "psprecomp/elf32.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace mhp3rd::native {
namespace {
// The callback ABI has no userdata. Only registration ownership lives here;
// arithmetic, modes and counters remain in the runtime's scoped owner.
std::mutex owners_mutex;
std::unordered_map<psprecomp::Runtime *, VectorMetricRuntime *> owners;
}

VectorMetricModes vector_metric_environment_modes() {
    VectorMetricModes modes{};
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const auto mode = parse_native_mode(std::getenv(kVectorMetricSwitches[i]));
        if (!mode) throw std::invalid_argument(std::string(kVectorMetricSwitches[i]) +
                                              " expects off, verify or native");
        modes[i] = *mode;
    }
    return modes;
}

VectorMetricRuntime::VectorMetricRuntime(psprecomp::Runtime &runtime,
        const psprecomp::Elf32Image &elf, psprecomp::Runtime::RecompiledFunction original)
    : runtime_(runtime), original_(original) {
    for (const auto &section : elf.sections())
        if ((section.flags & 4u) != 0u && section.size != 0u)
            excluded_.push_back({psprecomp::GuestMemory::canonical(elf.section_runtime_address(section)),
                                 section.size});
    // Reserve the entire overlay arena, including headers/data/gaps. A later
    // overlay load cannot make a previously admitted range executable.
    excluded_.push_back({kOverlaySlots[0], kOverlaySlots[std::size(kOverlaySlots) - 1u] - kOverlaySlots[0]});
}

bool VectorMetricRuntime::install(const VectorMetricModes &modes) {
    if (installed_) return false;
    const bool any = std::any_of(modes.begin(), modes.end(), [](auto mode) { return mode != NativeMode::Off; });
    if (!any) { modes_ = modes; installed_ = true; return true; }
#if !defined(__APPLE__) || !defined(__aarch64__)
    // The numerical oracle is currently certified only on Apple Silicon.
    // Other hosts may collect Verify evidence while retaining original AOT.
    if (std::any_of(modes.begin(), modes.end(), [](auto mode) { return mode == NativeMode::Native; }))
        return false;
#endif
#if !defined(MHP3RD_VECTOR_METRIC_ENTRY_BOUNDARIES)
    // A local goto can bypass Runtime::register_function. Do not advertise a
    // native mode unless the generated entry seam is present as well.
    return false;
#endif
    std::lock_guard lock(owners_mutex);
    if (owners.contains(&runtime_) || !original_) return false;
    // Validate every requested operation before changing a dispatch entry.
    for (std::size_t i = 0; i < bridges_.size(); ++i)
        if (modes[i] != NativeMode::Off &&
            (!runtime_.has_function(kVectorMetricLeaves[i].entry) ||
             !bridges_[i].configure(runtime_, modes[i], original_, excluded_))) return false;
    constexpr std::array<psprecomp::Runtime::RecompiledFunction, 4> hooks{
        &hook<0>, &hook<1>, &hook<2>, &hook<3>,
    };
    owners.emplace(&runtime_, this);
    set_vector_metric_entry_dispatch(&dispatch_entry);
    modes_ = modes;
    installed_ = true;
    try {
        for (std::size_t i = 0; i < hooks.size(); ++i) {
            if (modes[i] == NativeMode::Off) continue;
            // Mark before registration so a partial allocation failure also
            // takes the restoration path during destruction.
            hooked_[i] = true;
            runtime_.register_function(kVectorMetricLeaves[i].entry, hooks[i], kVectorMetricSwitches[i]);
        }
    } catch (...) {
        runtime_.stop("Vector metric registration failed");
        throw;
    }
    return true;
}

VectorMetricRuntime::~VectorMetricRuntime() {
    std::lock_guard lock(owners_mutex);
    const auto found = owners.find(&runtime_);
    if (found == owners.end() || found->second != this) return;
    for (std::size_t i = 0; i < hooked_.size(); ++i) {
        if (!hooked_[i]) continue;
        try { runtime_.register_function(kVectorMetricLeaves[i].entry, original_, "recomp_unit_vector_metric_restored"); }
        catch (...) {
            // A failed restoration must not leave a callback to freed state.
            runtime_.unregister_functions(kVectorMetricLeaves[i].entry, kVectorMetricLeaves[i].entry + 4u);
            runtime_.stop("Vector metric original restoration failed");
        }
    }
    owners.erase(found);
    if (owners.empty()) set_vector_metric_entry_dispatch(nullptr);
}

bool VectorMetricRuntime::dispatch_entry(psprecomp::Runtime &runtime,
        psprecomp::AllegrexContext &context, std::uint32_t entry) {
    std::lock_guard lock(owners_mutex);
    const auto owner = owners.find(&runtime);
    if (owner == owners.end()) return false;
    const auto leaf = std::find_if(kVectorMetricLeaves.begin(), kVectorMetricLeaves.end(),
                                  [entry](const auto &item) { return item.entry == entry; });
    if (leaf == kVectorMetricLeaves.end()) return false;
    const auto index = static_cast<std::size_t>(leaf - kVectorMetricLeaves.begin());
    if (owner->second->modes_[index] == NativeMode::Off) return false;
    // Local generated calls may still carry their outer dispatch PC. Only an
    // enabled, owned entry changes it; the original-only path is untouched.
    context.pc = entry;
    if (!owner->second->bridges_[index].execute(runtime, context))
        runtime.stop("Vector metric dispatch refused an uncertified invocation");
    return true;
}

void VectorMetricRuntime::invoke(psprecomp::Runtime &runtime,
        psprecomp::AllegrexContext &context, std::size_t index) {
    if (!dispatch_entry(runtime, context, kVectorMetricLeaves[index].entry))
        runtime.stop("Vector metric dispatch has no enabled owner");
}

std::array<NativeStats, 4> VectorMetricRuntime::stats() const noexcept {
    return {bridges_[0].stats(), bridges_[1].stats(), bridges_[2].stats(), bridges_[3].stats()};
}

void VectorMetricRuntime::report() const {
    for (std::size_t i = 0; i < bridges_.size(); ++i) {
        if (modes_[i] == NativeMode::Off) continue;
        const auto value = bridges_[i].stats();
        std::cout << "[native-metric] entry=" << kVectorMetricLeaves[i].entry
                  << " calls=" << value.calls << " verified=" << value.verified
                  << " native=" << value.native << " fallbacks=" << value.fallbacks
                  << " mismatches=" << value.mismatches << " errors=" << value.errors << '\n';
    }
}
} // namespace mhp3rd::native
