#include "testing/case_controller.hpp"

#include "psprecomp/sha256.hpp"
#include "testing/probes.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <mutex>
#include <span>
#include <utility>

namespace mhp3rd::testing {
namespace {

bool lower_sha256(std::string_view value) noexcept {
    if (value.size() != 64) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool valid_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 96 ||
        !((value.front() >= 'a' && value.front() <= 'z') ||
          (value.front() >= 'A' && value.front() <= 'Z'))) return false;
    for (const char ch : value.substr(1)) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) return false;
    }
    return true;
}

std::uint32_t probe_bit(std::uint32_t entry) noexcept {
    switch (entry) {
    case 0x088775ACu: return kProbeAngle;
    case 0x08878B28u: return kProbeScale;
    case 0x08878B4Cu: return kProbeTranslation;
    case 0x08877818u: return kProbeVector;
    case 0x08879D08u: return kProbeCopy;
    case 0x08877244u: return kProbeNorm;
    case 0x08877264u: return kProbeNormSquared;
    case 0x08877280u: return kProbeDistance;
    case 0x088772A8u: return kProbeDistanceSquared;
    default: return 0;
    }
}

const char *outcome_name(CaseOutcome outcome) noexcept {
    switch (outcome) {
    case CaseOutcome::Normal: return "normal";
    case CaseOutcome::Abnormal: return "abnormal";
    case CaseOutcome::Uncertain: return "uncertain";
    case CaseOutcome::Skipped: return "skipped";
    }
    return nullptr;
}

CaseProgressState progress_state(CaseOutcome outcome) noexcept {
    switch (outcome) {
    case CaseOutcome::Normal: return CaseProgressState::Normal;
    case CaseOutcome::Abnormal: return CaseProgressState::Abnormal;
    case CaseOutcome::Uncertain: return CaseProgressState::Uncertain;
    case CaseOutcome::Skipped: return CaseProgressState::Skipped;
    }
    return CaseProgressState::Interrupted;
}

// Explicit byte recipe. All pieces are bounded, newline-free validated tokens.
// Role, run and build identity are intentionally absent so paired roles match.
std::string prerequisites_sha256(std::string_view basis, std::string_view case_id,
                                 std::uint32_t version, std::string_view configuration) {
    std::string recipe = "yakumo-case-prerequisites-v1\n";
    recipe += basis;
    recipe += '\n';
    recipe += case_id;
    recipe += '\n';
    recipe += std::to_string(version);
    recipe += '\n';
    recipe += configuration;
    recipe += '\n';
    const auto bytes = std::as_bytes(std::span{recipe.data(), recipe.size()});
    return psprecomp::sha256_bytes(
        std::span{reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}

std::mutex active_mutex;
std::shared_ptr<CaseController> active_controller;

} // namespace

CaseController::CaseController(std::shared_ptr<GameObserver> observer, CaseCatalog catalog,
                               CaseSessionInfo session, CaseHooks hooks)
    : observer_(std::move(observer)), catalog_(std::move(catalog)),
      session_(std::move(session)), hooks_(std::move(hooks)),
      progress_(catalog_.cases.size()) {}

CaseController::~CaseController() { close("controller_destroyed"); }

namespace {
bool bad_recording(const RecorderHealth &health, std::uint64_t emission_errors) noexcept {
    return emission_errors != 0 || health.io_failed || health.dropped_events != 0 ||
           health.invalid_events != 0 || !health.error.empty();
}
} // namespace

bool CaseController::healthy() const noexcept {
    if (!observer_) return false;
    try {
        const auto health = observer_->recorder_health();
        return !health.closed && !bad_recording(health, observer_->emission_errors());
    } catch (...) {
        return false;
    }
}

bool CaseController::recording_failed() const noexcept {
    if (!observer_) return true;
    try {
        return bad_recording(observer_->recorder_health(), observer_->emission_errors());
    } catch (...) {
        return true;
    }
}

bool CaseController::recording_healthy() const noexcept {
    return !closed_ && !failed_ && healthy();
}

CaseProgressState CaseController::effective_state(std::size_t case_index) const noexcept {
    if (case_index >= progress_.size()) return CaseProgressState::Interrupted;
    const auto state = progress_[case_index].state;
    return state == CaseProgressState::Normal && (failed_ || recording_failed())
        ? CaseProgressState::Interrupted : state;
}

void CaseController::set_error(std::string_view reason) noexcept {
    try { last_error_.assign(reason); } catch (...) {}
}

Fields CaseController::identity_fields(std::size_t index) const {
    return {
        {"case_id", catalog_.cases[index].id},
        {"case_version", static_cast<std::uint64_t>(catalog_.cases[index].version)},
        {"attempt", progress_[index].attempt},
    };
}

void CaseController::emit_interrupted(std::string_view reason) noexcept {
    if (!active_case_ || !observer_) return;
    try {
        auto fields = identity_fields(*active_case_);
        fields.push_back({"reason", std::string(reason)});
        observer_->emit(EventKind::State, "case.interrupted", std::move(fields), true);
    } catch (...) {}
}

void CaseController::fail(std::string_view reason) noexcept {
    set_error(reason);
    failed_ = true;
    if (active_case_) {
        progress_[*active_case_].state = CaseProgressState::Interrupted;
        emit_interrupted(reason);
    }
    if (observer_) {
        try {
            Fields fields{{"reason", std::string(reason)}};
            if (active_case_) {
                auto identity = identity_fields(*active_case_);
                fields.insert(fields.end(), std::make_move_iterator(identity.begin()),
                              std::make_move_iterator(identity.end()));
            }
            observer_->emit(EventKind::Error, "case.controller_error", std::move(fields), true);
        } catch (...) {}
    }
    active_case_.reset();
}

bool CaseController::emit_checked(EventKind kind, std::string_view event, Fields fields) noexcept {
    if (!healthy()) return false;
    try {
        const auto before = observer_->recorder_health().accepted_events;
        observer_->emit(kind, event, std::move(fields), true);
        return healthy() && observer_->recorder_health().accepted_events > before;
    } catch (...) {
        return false;
    }
}

bool CaseController::call(const std::function<void()> &callback,
                          std::string_view failure) noexcept {
    if (!callback) {
        fail(failure);
        return false;
    }
    try {
        callback();
    } catch (...) {
        fail(failure);
        return false;
    }
    if (!healthy()) {
        fail("recording_failed");
        return false;
    }
    return true;
}

bool CaseController::call_flush(std::string_view boundary) noexcept {
    if (!hooks_.flush_probes) {
        fail("probe_flush_unavailable");
        return false;
    }
    try {
        hooks_.flush_probes(boundary);
    } catch (...) {
        fail("probe_flush_failed");
        return false;
    }
    if (!healthy()) {
        fail("recording_failed");
        return false;
    }
    return true;
}

bool CaseController::same_configuration() noexcept {
    if (!hooks_.configuration_sha256) {
        fail("configuration_unavailable");
        return false;
    }
    try {
        const auto current = hooks_.configuration_sha256();
        if (!lower_sha256(current)) {
            fail("invalid_configuration_sha256");
            return false;
        }
        if (current != active_configuration_sha256_) {
            fail("configuration_changed_during_case");
            return false;
        }
    } catch (...) {
        fail("configuration_read_failed");
        return false;
    }
    return true;
}

void CaseController::configuration_observed() noexcept {
    if (!closed_ && !failed_ && active_case_) (void)same_configuration();
}

bool CaseController::begin(std::size_t case_index) noexcept {
    if (closed_ || failed_) { set_error("case_controller_unavailable"); return false; }
    if (active_case_) { set_error("case_already_active"); return false; }
    if (case_index >= catalog_.cases.size()) { set_error("case_index_out_of_range"); return false; }
    if (!healthy()) { fail("recording_unavailable"); return false; }

    const auto &spec = catalog_.cases[case_index];
    if (catalog_.cases.size() > 128 || spec.required_probes.size() > 32 ||
        !lower_sha256(catalog_.sha256) ||
        !lower_sha256(session_.prerequisite_basis_sha256) ||
        (session_.role != "baseline" && session_.role != "candidate") ||
        session_.build_version.empty() || session_.run_id.empty() ||
        session_.batch_id.empty() || session_.baseline_id.empty() ||
        !valid_id(spec.id) || spec.version == 0 || spec.checkpoints.size() > 128) {
        set_error("invalid_case_session_metadata");
        return false;
    }
    std::uint32_t mask{};
    for (const auto &probe : spec.required_probes) {
        const auto bit = probe_bit(probe.entry);
        if (!bit || probe.min_calls == 0 || (mask & bit) != 0) {
            set_error("unsupported_or_duplicate_case_probe");
            return false;
        }
        mask |= bit;
    }
    for (std::size_t i = 0; i < spec.checkpoints.size(); ++i) {
        if (!valid_id(spec.checkpoints[i]) ||
            std::find(spec.checkpoints.begin(), spec.checkpoints.begin() + i,
                      spec.checkpoints[i]) != spec.checkpoints.begin() + i) {
            set_error("invalid_case_checkpoint");
            return false;
        }
    }
    if (progress_[case_index].attempt == std::numeric_limits<std::uint64_t>::max()) {
        set_error("case_attempt_overflow");
        return false;
    }
    if (!hooks_.configuration_sha256 || !hooks_.select_probes || !hooks_.flush_probes ||
        !hooks_.observe_state || !hooks_.flush_detail) {
        set_error("case_diagnostics_unavailable");
        return false;
    }

    try {
        const auto configuration = hooks_.configuration_sha256();
        if (!lower_sha256(configuration)) {
            set_error("invalid_configuration_sha256");
            return false;
        }
        const auto prerequisites = prerequisites_sha256(
            session_.prerequisite_basis_sha256, spec.id, spec.version, configuration);
        hooks_.select_probes(mask);
        if (!healthy()) { fail("recording_failed"); return false; }

        const auto attempt = progress_[case_index].attempt + 1;
        Fields fields{
            {"case_id", spec.id},
            {"case_version", static_cast<std::uint64_t>(spec.version)},
            {"attempt", attempt},
            {"prerequisites_sha256", prerequisites},
        };
        if (!emit_checked(EventKind::CaseBegin, "case.begin", std::move(fields))) {
            fail("case_begin_recording_failed");
            return false;
        }
        auto &progress = progress_[case_index];
        progress.attempt = attempt;
        progress.next_checkpoint = 0;
        progress.state = CaseProgressState::Active;
        progress.prerequisites_sha256 = prerequisites;
        active_case_ = case_index;
        active_configuration_sha256_ = configuration;
        if (!call_flush("case_begin")) return false;
        last_error_.clear();
        return true;
    } catch (...) {
        fail("case_begin_failed");
        return false;
    }
}

bool CaseController::checkpoint() noexcept {
    if (closed_ || failed_ || !active_case_) { set_error("no_active_case"); return false; }
    const auto index = *active_case_;
    const auto next = progress_[index].next_checkpoint;
    if (next >= catalog_.cases[index].checkpoints.size()) {
        set_error("no_pending_checkpoint");
        return false;
    }
    if (!healthy()) { fail("recording_failed"); return false; }
    if (!same_configuration()) return false;
    if (!call(hooks_.observe_state, "state_observation_failed")) return false;
    try {
        auto fields = identity_fields(index);
        fields.push_back({"checkpoint_id", catalog_.cases[index].checkpoints[next]});
        if (!emit_checked(EventKind::Checkpoint, "case.checkpoint", std::move(fields))) {
            fail("checkpoint_recording_failed");
            return false;
        }
        ++progress_[index].next_checkpoint;
        if (!call(hooks_.flush_detail, "probe_detail_failed")) return false;
        last_error_.clear();
        return true;
    } catch (...) {
        fail("checkpoint_failed");
        return false;
    }
}

bool CaseController::anomaly() noexcept {
    if (closed_ || failed_ || !active_case_) { set_error("no_active_case"); return false; }
    if (!healthy()) { fail("recording_failed"); return false; }
    if (!same_configuration()) return false;
    try {
        if (!emit_checked(EventKind::Anomaly, "case.anomaly", identity_fields(*active_case_))) {
            fail("anomaly_recording_failed");
            return false;
        }
        if (!call(hooks_.flush_detail, "probe_detail_failed")) return false;
        last_error_.clear();
        return true;
    } catch (...) {
        fail("anomaly_failed");
        return false;
    }
}

bool CaseController::finish(CaseOutcome outcome) noexcept {
    if (closed_ || failed_ || !active_case_) { set_error("no_active_case"); return false; }
    const auto name = outcome_name(outcome);
    if (!name) { set_error("invalid_case_outcome"); return false; }
    const auto index = *active_case_;
    if (outcome == CaseOutcome::Normal &&
        progress_[index].next_checkpoint != catalog_.cases[index].checkpoints.size()) {
        set_error("required_checkpoints_pending");
        return false;
    }
    if (!healthy()) { fail("recording_failed"); return false; }
    if (!same_configuration()) return false;
    // For a case without checkpoints, the end marker is its state boundary.
    if (catalog_.cases[index].checkpoints.empty() &&
        !call(hooks_.observe_state, "state_observation_failed")) return false;
    if (!call_flush("case_end")) return false;
    try {
        auto fields = identity_fields(index);
        fields.push_back({"outcome", std::string(name)});
        if (!emit_checked(EventKind::CaseEnd, "case.end", std::move(fields))) {
            fail("case_end_recording_failed");
            return false;
        }
        progress_[index].state = progress_state(outcome);
        active_case_.reset();
        active_configuration_sha256_.clear();
        last_error_.clear();
        return true;
    } catch (...) {
        fail("case_end_failed");
        return false;
    }
}

void CaseController::close(std::string_view reason) noexcept {
    if (closed_) return;
    closed_ = true;
    if (active_case_) {
        progress_[*active_case_].state = CaseProgressState::Interrupted;
        if (reason.empty() || reason.size() > 128) reason = "run_closed";
        emit_interrupted(reason);
        active_case_.reset();
    }
}

std::shared_ptr<CaseController> active_case_controller() noexcept {
    try {
        std::lock_guard lock(active_mutex);
        return active_controller;
    } catch (...) {
        return {};
    }
}

void set_active_case_controller(std::shared_ptr<CaseController> controller) noexcept {
    try {
        std::lock_guard lock(active_mutex);
        active_controller = std::move(controller);
    } catch (...) {}
}

} // namespace mhp3rd::testing
