#include "testing/runtime_diagnostics.hpp"
#include "testing/probes.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace mhp3rd::testing {
std::uint32_t parse_probe_selection(std::string_view names) {
    if (names.empty() || names == "off") return 0;
    if (names == "all") return kProbeAll;
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 5> choices{{
        {"angle", kProbeAngle}, {"scale", kProbeScale}, {"translation", kProbeTranslation},
        {"vector", kProbeVector}, {"copy", kProbeCopy}}};
    std::uint32_t result{};
    for (;;) {
        const auto comma = names.find(',');
        const auto name = names.substr(0, comma);
        std::uint32_t bit{};
        for (const auto &choice : choices) if (choice.first == name) bit = choice.second;
        if (!bit || (result & bit)) throw std::invalid_argument("Invalid or duplicate MHP3RD_RECORD_PROBES selection");
        result |= bit;
        if (comma == names.npos) return result;
        names.remove_prefix(comma + 1);
    }
}

RuntimeDiagnostics::RuntimeDiagnostics(std::shared_ptr<GameObserver> observer,
        const psprecomp::GuestMemory &memory, bool supported_executable, std::uint32_t probe_mask)
    : observer_(std::move(observer)), memory_(&memory), supported_executable_(supported_executable),
      run_probe_mask_(probe_mask) {
    if (!observer_) throw std::invalid_argument("Runtime diagnostics require a recorder");
    if (probe_mask & ~kProbeAll) throw std::invalid_argument("Unknown native probe bits");
    configure_native_probes(observer_, supported_executable_ ? probe_mask : 0);
    observer_->emit(EventKind::State, "diagnostics.configuration", {
        {"requested_probe_mask", std::uint64_t(probe_mask)},
        {"active_probe_mask", std::uint64_t(supported_executable_ ? probe_mask : 0)},
        {"supported_executable", supported_executable_},
        {"state_sampling", std::string("host_second_at_vblank")},
        {"state_coverage", std::string("periodic_samples_not_all_transitions")},
    }, true);
}

RuntimeDiagnostics::~RuntimeDiagnostics() { close(); }

void RuntimeDiagnostics::select_case_probes(std::uint32_t mask) {
    if (closed_ || !supported_executable_ || (mask & ~kProbeAll))
        throw std::runtime_error("Case observations require an active supported runtime");
    // Keep the launcher's batch-wide diagnostic selection, including changed
    // helpers whose real trigger coverage is still being discovered.
    const auto effective_mask = mask | run_probe_mask_;
    configure_native_probes(observer_, effective_mask);
    if (selected_native_probes() != effective_mask)
        throw std::runtime_error("Case probes could not be configured");
}

void RuntimeDiagnostics::capture_state() {
    if (closed_ || !memory_ || !supported_executable_)
        throw std::runtime_error("Case observations require an active supported runtime");
    observe_game_state(*memory_, *observer_, supported_executable_);
}

void RuntimeDiagnostics::tick(const perf::Summary &performance) noexcept {
    if (closed_) return;
    const auto now = std::chrono::steady_clock::now();
    performance_.observe(*observer_, performance);
    if (sampled_ && now - previous_ < std::chrono::seconds(1)) return;
    sampled_ = true;
    previous_ = now;
    observe_game_state(*memory_, *observer_, supported_executable_);
    flush_native_probes();
}

void RuntimeDiagnostics::close() noexcept {
    if (closed_) return;
    closed_ = true;
    flush_native_probes(true);
    configure_native_probes({}, 0);
    memory_ = nullptr;
}
} // namespace mhp3rd::testing
