#pragma once

#include "testing/game_observers.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace mhp3rd::testing {

struct RecordingOptions {
    std::filesystem::path directory;
    std::string role;
    std::string run_id;
    std::string batch_id;
    std::string baseline_id{"B0"};
    std::string baseline_commit{"4292eb6"};
};
[[nodiscard]] std::string_view recording_revision() noexcept;

// An unset or empty MHP3RD_RECORD_DIR disables recording without consulting
// the other recording variables. Invalid enabled settings throw.
[[nodiscard]] std::optional<RecordingOptions> recording_options_from_environment();

// Start and stop on the application main thread. A run owns a new directory;
// starting another run requires another directory. Startup throws before
// publishing its observer if configuration, metadata or the initial flush fails.
class RuntimeRecording {
public:
    explicit RuntimeRecording(RecordingOptions options, Fields app_metadata = {});
    ~RuntimeRecording();
    RuntimeRecording(const RuntimeRecording &) = delete;
    RuntimeRecording &operator=(const RuntimeRecording &) = delete;

    [[nodiscard]] std::shared_ptr<GameObserver> observer() const noexcept;
    // Detaches this observer, emits observer.health and closes the journal.
    // Repeated calls return the first result. Failure is also reported to stderr.
    [[nodiscard]] bool close(std::string reason, bool completed) noexcept;

private:
    std::shared_ptr<SessionRecorder> recorder_;
    std::shared_ptr<GameObserver> observer_;
    bool closed_{};
    bool close_result_{};
};

// Call before locating game assets; later app hooks may record their actual
// identities. Returns null only when recording is disabled.
[[nodiscard]] std::unique_ptr<RuntimeRecording> start_runtime_recording(Fields metadata = {});

} // namespace mhp3rd::testing
