#include "testing/runtime_recording.hpp"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mhp3rd::testing {
#ifndef MHP3RD_RECORDING_REVISION
#define MHP3RD_RECORDING_REVISION "unversioned"
#endif
std::string_view recording_revision() noexcept { return MHP3RD_RECORDING_REVISION; }
namespace {

// Lifecycle entry points run on the application main thread. This also
// prevents a second run if a caller temporarily detaches its observer.
RuntimeRecording *active_runtime = nullptr;

std::string required_environment(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value)
        throw std::invalid_argument(std::string("missing recording setting: ") + name);
    return value;
}

bool ascii_safe_id(const std::string &value) {
    if (value.empty() || value.size() > 96 || value == "." || value == "..")
        return false;
    for (const unsigned char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'))
            return false;
    }
    return true;
}

bool hex_commit(const std::string &value) {
    if (value.size() < 7 || value.size() > 40)
        return false;
    for (const unsigned char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}

void validate_options(const RecordingOptions &options) {
    if (options.directory.empty())
        throw std::invalid_argument("recording directory is empty");
    if (options.role != "baseline" && options.role != "candidate")
        throw std::invalid_argument("recording role must be baseline or candidate");
    if (!ascii_safe_id(options.run_id) || !ascii_safe_id(options.batch_id) ||
        !ascii_safe_id(options.baseline_id))
        throw std::invalid_argument("recording identity must be a nonempty ASCII-safe ID of at most 96 bytes");
    if (!hex_commit(options.baseline_commit))
        throw std::invalid_argument("baseline commit must be 7 to 40 hexadecimal digits");
    if (!options.context_sha256.empty()) {
        if (options.context_sha256.size() != 64)
            throw std::invalid_argument("recording context digest must contain 64 lowercase hexadecimal digits");
        for (const char c : options.context_sha256)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                throw std::invalid_argument("recording context digest must contain 64 lowercase hexadecimal digits");
    }
}

Fields begin_fields(const RecordingOptions &options, Fields app_metadata) {
    Fields fields{
        {"schema", std::string("journal-v1")},
        {"recorder_revision", std::string(recording_revision())},
        {"observer_schema", std::string("observers-v1")},
        {"role", options.role},
        {"run_id", options.run_id},
        {"batch_id", options.batch_id},
        {"baseline_id", options.baseline_id},
        {"baseline_commit", options.baseline_commit},
        {"input_identity_status", std::string("pending")},
    };
    if (!options.context_sha256.empty()) fields.push_back({"context_sha256", options.context_sha256});
    fields.insert(fields.end(), std::make_move_iterator(app_metadata.begin()),
                  std::make_move_iterator(app_metadata.end()));
    // This validates all caller fields and catches duplicate reserved names
    // before any directory or journal is created.
    (void)fields_json(fields);
    return fields;
}

std::filesystem::path new_directory_path(const std::filesystem::path &input) {
    namespace fs = std::filesystem;
    fs::path directory = fs::absolute(input).lexically_normal();
    if (!directory.has_filename())
        directory = directory.parent_path();
    if (directory.empty() || directory == directory.root_path())
        throw std::invalid_argument("recording directory must name a new child directory");

    fs::path current = directory.root_path();
    std::error_code error;
    if (!fs::is_directory(fs::symlink_status(current, error)) || error)
        throw fs::filesystem_error("recording root is unavailable", current, error);

    const fs::path relative = directory.lexically_relative(current);
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        current /= *part;
        error.clear();
        const auto status = fs::symlink_status(current, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw fs::filesystem_error("inspect recording path", current, error);
        const bool final = std::next(part) == relative.end();
        if (final) {
            if (status.type() != fs::file_type::not_found)
                throw std::invalid_argument("recording directory already exists");
        } else if (error || status.type() == fs::file_type::symlink ||
                   !fs::is_directory(status)) {
            throw std::invalid_argument("recording parent must exist without symlinks");
        }
    }
    return directory;
}

void report_close_failure(const RecorderHealth &health) noexcept {
    if (health.error.empty())
        std::fputs("runtime recording could not close cleanly\n", stderr);
    else
        std::fprintf(stderr, "runtime recording could not close cleanly: %s\n",
                     health.error.c_str());
}

} // namespace

std::optional<RecordingOptions> recording_options_from_environment() {
    const char *directory = std::getenv("MHP3RD_RECORD_DIR");
    if (!directory || !*directory)
        return std::nullopt;

    RecordingOptions options;
    options.directory = directory;
    options.role = required_environment("MHP3RD_RECORD_ROLE");
    options.run_id = required_environment("MHP3RD_RECORD_RUN_ID");
    options.batch_id = required_environment("MHP3RD_RECORD_BATCH_ID");
    if (const char *value = std::getenv("MHP3RD_RECORD_BASELINE_ID"))
        options.baseline_id = value;
    if (const char *value = std::getenv("MHP3RD_RECORD_BASELINE_COMMIT"))
        options.baseline_commit = value;
    if (const char *value = std::getenv("MHP3RD_RECORD_CONTEXT_SHA256"))
        options.context_sha256 = value;
    validate_options(options);
    return options;
}

RuntimeRecording::RuntimeRecording(RecordingOptions options, Fields app_metadata) {
    validate_options(options);
    Fields run_fields = begin_fields(options, std::move(app_metadata));
    if (active_runtime || active_observer())
        throw std::runtime_error("a runtime recording or game observer is already active");

    const auto directory = new_directory_path(options.directory);
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("recording directory already exists");

    auto sink = make_file_sink(directory / "events.journal");
    recorder_ = std::make_shared<SessionRecorder>(std::move(sink), std::move(run_fields));
    try {
        observer_ = std::make_shared<GameObserver>(recorder_);
        if (!recorder_->flush())
            throw std::runtime_error("initial recording flush failed");
    } catch (...) {
        (void)recorder_->close("runtime_start_failed", false);
        throw;
    }
    set_active_observer(observer_);
    active_runtime = this;
}

RuntimeRecording::~RuntimeRecording() {
    if (!closed_)
        (void)close("runtime_scope_unfinished", false);
}

std::shared_ptr<GameObserver> RuntimeRecording::observer() const noexcept {
    return observer_;
}

bool RuntimeRecording::close(std::string reason, bool completed) noexcept {
    if (closed_)
        return close_result_;
    closed_ = true;
    if (active_runtime == this)
        active_runtime = nullptr;

    if (active_observer() == observer_)
        set_active_observer({});

    bool healthy = !reason.empty();
    try {
        (void)fields_json(Fields{{"stop_reason", reason}});
    } catch (...) {
        healthy = false;
    }
    try {
        const bool preflush = recorder_->flush();
        const auto before = recorder_->health();
        const auto emission_errors = observer_->emission_errors();
        observer_->emit(EventKind::State, "observer.health", {
            {"emission_errors", emission_errors},
            {"accepted_events", before.accepted_events},
            {"written_events", before.written_events},
            {"dropped_events", before.dropped_events},
            {"invalid_events", before.invalid_events},
            {"written_records", before.written_records},
            {"flushed_records", before.flushed_records},
            {"queued_events", static_cast<std::uint64_t>(before.queued_events)},
            {"queued_bytes", static_cast<std::uint64_t>(before.queued_bytes)},
            {"io_failed", before.io_failed},
            {"closed", before.closed},
            {"error", before.error},
        }, true);
        const auto after = recorder_->health();
        healthy = healthy && preflush && !before.io_failed && !after.io_failed &&
            before.dropped_events == 0 && after.dropped_events == 0 &&
            before.invalid_events == 0 && after.invalid_events == 0 &&
            emission_errors == 0 && observer_->emission_errors() == 0 &&
            after.accepted_events > before.accepted_events;
    } catch (...) {
        healthy = false;
    }

    try {
        close_result_ = recorder_->close(std::move(reason), completed && healthy) && healthy;
    } catch (...) {
        close_result_ = false;
    }
    if (!close_result_) {
        try {
            report_close_failure(recorder_->health());
        } catch (...) {
            std::fputs("runtime recording could not close cleanly\n", stderr);
        }
    }
    return close_result_;
}

std::unique_ptr<RuntimeRecording> start_runtime_recording(Fields metadata) {
    auto options = recording_options_from_environment();
    if (!options)
        return nullptr;
    return std::make_unique<RuntimeRecording>(std::move(*options), std::move(metadata));
}

} // namespace mhp3rd::testing
