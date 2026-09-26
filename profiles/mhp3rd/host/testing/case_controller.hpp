#pragma once

#include "testing/case_catalog.hpp"
#include "testing/game_observers.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mhp3rd::testing {

struct CaseSessionInfo {
    std::string role;
    std::string build_version;
    std::string run_id;
    std::string batch_id;
    std::string baseline_id;
    // A launcher-bound digest of the common starting inputs. It must be the
    // same for the paired roles and must not contain the run or build identity.
    std::string prerequisite_basis_sha256;
};

struct CaseHooks {
    // These callbacks run on the emulation thread. They observe or configure
    // diagnostics only; they must not write save data or change game state.
    std::function<void(std::uint32_t)> select_probes;
    std::function<void(std::string_view)> flush_probes;
    std::function<void()> observe_state;
    std::function<void()> flush_detail;
    std::function<std::string()> configuration_sha256;
};

enum class CaseOutcome { Normal, Abnormal, Uncertain, Skipped };
enum class CaseProgressState {
    NotStarted, Active, Normal, Abnormal, Uncertain, Skipped, Interrupted,
};

struct CaseProgress {
    std::uint64_t attempt{};
    std::size_t next_checkpoint{};
    CaseProgressState state{CaseProgressState::NotStarted};
    std::string prerequisites_sha256;
};

// Own this for the complete recording run, independently of panel visibility.
// Actions are called on the emulation thread and never throw into the game.
class CaseController {
public:
    CaseController(std::shared_ptr<GameObserver> observer, CaseCatalog catalog,
                   CaseSessionInfo session, CaseHooks hooks);
    ~CaseController();
    CaseController(const CaseController &) = delete;
    CaseController &operator=(const CaseController &) = delete;

    [[nodiscard]] bool begin(std::size_t case_index) noexcept;
    [[nodiscard]] bool checkpoint() noexcept;
    [[nodiscard]] bool anomaly() noexcept;
    [[nodiscard]] bool finish(CaseOutcome outcome) noexcept;
    // An unfinished attempt stays interrupted. No CaseEnd is invented.
    void close(std::string_view reason) noexcept;

    [[nodiscard]] const CaseCatalog &catalog() const noexcept { return catalog_; }
    [[nodiscard]] const CaseSessionInfo &session() const noexcept { return session_; }
    [[nodiscard]] const std::vector<CaseProgress> &progress() const noexcept { return progress_; }
    [[nodiscard]] std::optional<std::size_t> active_case() const noexcept { return active_case_; }
    [[nodiscard]] const std::string &last_error() const noexcept { return last_error_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] bool recording_healthy() const noexcept;
    // A normal marker is not a valid completion after later recording loss.
    [[nodiscard]] CaseProgressState effective_state(std::size_t case_index) const noexcept;

private:
    [[nodiscard]] bool healthy() const noexcept;
    [[nodiscard]] bool recording_failed() const noexcept;
    [[nodiscard]] bool same_configuration() noexcept;
    [[nodiscard]] bool emit_checked(EventKind kind, std::string_view event, Fields fields) noexcept;
    [[nodiscard]] bool call(const std::function<void()> &callback,
                            std::string_view failure) noexcept;
    [[nodiscard]] bool call_flush(std::string_view boundary) noexcept;
    void fail(std::string_view reason) noexcept;
    void set_error(std::string_view reason) noexcept;
    void emit_interrupted(std::string_view reason) noexcept;
    [[nodiscard]] Fields identity_fields(std::size_t index) const;

    std::shared_ptr<GameObserver> observer_;
    CaseCatalog catalog_;
    CaseSessionInfo session_;
    CaseHooks hooks_;
    std::vector<CaseProgress> progress_;
    std::optional<std::size_t> active_case_;
    std::string active_configuration_sha256_;
    std::string last_error_;
    bool closed_{};
    bool failed_{};
};

[[nodiscard]] std::shared_ptr<CaseController> active_case_controller() noexcept;
void set_active_case_controller(std::shared_ptr<CaseController> controller) noexcept;

} // namespace mhp3rd::testing
