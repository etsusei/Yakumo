#include "testing/case_runtime.hpp"
#include "testing/probes.hpp"
#include "settings/settings.hpp"

#include <stdexcept>
#include <utility>

namespace mhp3rd::testing {
std::shared_ptr<CaseController> start_case_session(const RecordingOptions &options,
        std::shared_ptr<GameObserver> observer, std::shared_ptr<RuntimeDiagnostics> diagnostics,
        std::string build_version) {
    if (options.case_catalog.empty()) return {};
    if (!observer || !diagnostics || options.context_sha256.empty() ||
        options.case_catalog_sha256.empty() || options.prerequisite_basis_sha256.empty())
        throw std::invalid_argument("Case panel requires a bound recording session");
    if (active_case_controller()) throw std::logic_error("A case panel is already active");
    auto catalog = load_case_catalog(options.case_catalog);
    if (catalog.sha256 != options.case_catalog_sha256)
        throw std::invalid_argument("Case catalog differs from the launch context");
    const std::weak_ptr<RuntimeDiagnostics> weak = diagnostics;
    const auto runtime = [weak] {
        auto current = weak.lock();
        if (!current) throw std::runtime_error("Case observation runtime is unavailable");
        return current;
    };
    CaseHooks hooks;
    hooks.select_probes = [runtime](std::uint32_t mask) { runtime()->select_case_probes(mask); };
    hooks.flush_probes = [](std::string_view boundary) { flush_native_probes(false, boundary); };
    hooks.observe_state = [runtime] { runtime()->capture_state(); };
    hooks.flush_detail = [] { flush_native_probe_detail(); };
    hooks.configuration_sha256 = [] { return settings::case_configuration_sha256(); };
    auto controller = std::make_shared<CaseController>(observer, std::move(catalog),
        CaseSessionInfo{options.role, std::move(build_version), options.run_id, options.batch_id,
                        options.baseline_id, options.prerequisite_basis_sha256}, std::move(hooks));
    observer->emit(EventKind::State, "case.panel_ready", {
        {"case_catalog_sha256", options.case_catalog_sha256},
        {"prerequisite_basis_sha256", options.prerequisite_basis_sha256},
        {"case_count", std::uint64_t(controller->catalog().cases.size())},
    }, true);
    set_active_case_controller(controller);
    return controller;
}

void close_case_session(const std::shared_ptr<CaseController> &controller, std::string_view reason) noexcept {
    if (!controller) return;
    controller->close(reason);
    if (active_case_controller() == controller) set_active_case_controller({});
}
} // namespace mhp3rd::testing
