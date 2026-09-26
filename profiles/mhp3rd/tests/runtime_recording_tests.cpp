#include "testing/runtime_recording.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace mhp3rd::testing;
namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, std::string_view description) {
    if (!condition && ++failures <= 50)
        std::cerr << "FAIL: " << description << '\n';
}

template <class Function>
void check_throws(Function &&function, std::string_view description) {
    try {
        function();
        check(false, description);
    } catch (const std::exception &) {
    } catch (...) {
        check(false, "unexpected non-standard exception");
    }
}

void set_environment(const char *name, const std::optional<std::string> &value) {
#ifdef _WIN32
    if (_putenv_s(name, value ? value->c_str() : "") != 0)
        throw std::runtime_error("cannot set test environment");
#else
    const int result = value ? ::setenv(name, value->c_str(), 1) : ::unsetenv(name);
    if (result != 0)
        throw std::runtime_error("cannot set test environment");
#endif
}

class Environment {
public:
    Environment() {
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (const char *value = std::getenv(names_[i]))
                old_[i] = value;
            set_environment(names_[i], std::nullopt);
        }
    }
    ~Environment() {
        for (std::size_t i = 0; i < names_.size(); ++i) {
            try { set_environment(names_[i], old_[i]); } catch (...) {}
        }
    }
    void set(std::size_t index, std::optional<std::string> value) {
        set_environment(names_[index], value);
    }
private:
    static constexpr std::array<const char *, 6> names_{
        "MHP3RD_RECORD_DIR", "MHP3RD_RECORD_ROLE", "MHP3RD_RECORD_RUN_ID",
        "MHP3RD_RECORD_BATCH_ID", "MHP3RD_RECORD_BASELINE_ID",
        "MHP3RD_RECORD_BASELINE_COMMIT"};
    std::array<std::optional<std::string>, names_.size()> old_{};
};

class TempRoot {
public:
    TempRoot() {
        static std::atomic<std::uint64_t> counter{};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::weakly_canonical(fs::temp_directory_path()) /
            ("yakumo-runtime-recording-test-" + std::to_string(tick) + "-" +
             std::to_string(counter.fetch_add(1)));
        if (!fs::create_directory(path))
            throw std::runtime_error("temporary test directory already exists");
    }
    ~TempRoot() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

RecordingOptions options(const fs::path &directory) {
    return {directory, "candidate", "run_01", "batch-1", "B0", "4292eb6"};
}

bool contains(std::string_view text, std::string_view fragment) {
    return text.find(fragment) != std::string_view::npos;
}

void check_initial_fields(const JournalRecord &record) {
    check(record.kind == EventKind::RunBegin, "first record is RunBegin");
    check(contains(record.payload, "\"recorder_revision\":\"" + std::string(recording_revision()) + "\""),
          "RunBegin records the compiled observer source revision");
    for (const auto field : {
             "\"schema\":\"journal-v1\"",
             "\"observer_schema\":\"observers-v1\"",
             "\"role\":\"candidate\"",
             "\"run_id\":\"run_01\"",
             "\"batch_id\":\"batch-1\"",
             "\"baseline_id\":\"B0\"",
             "\"baseline_commit\":\"4292eb6\"",
             "\"input_identity_status\":\"pending\"",
             "\"test_context\":\"synthetic\""}) {
        check(contains(record.payload, field), "RunBegin contains required metadata");
    }
}

void test_disabled_and_environment() {
    TempRoot root;
    Environment env;
    env.set(1, "invalid-role");
    env.set(2, "invalid/id");
    env.set(3, "bad batch");
    check(!recording_options_from_environment(), "unset directory disables recording");
    check(!start_runtime_recording(), "disabled factory returns null");
    env.set(0, "");
    check(!recording_options_from_environment(), "empty directory disables recording");
    check(!start_runtime_recording(Fields{{"bad", std::string("\xff")}}),
          "disabled factory does not validate metadata");
    check(fs::is_empty(root.path), "disabled path creates no files");
    check(!active_observer(), "disabled path does not publish an observer");

    const auto run = root.path / "env-run";
    env.set(0, run.string());
    check_throws([&] { (void)recording_options_from_environment(); }, "invalid role is rejected");
    env.set(1, std::nullopt);
    check_throws([&] { (void)recording_options_from_environment(); }, "missing role is rejected");
    env.set(1, "baseline");
    env.set(2, std::nullopt);
    check_throws([&] { (void)recording_options_from_environment(); }, "missing run ID is rejected");
    env.set(2, "run-1");
    env.set(3, std::nullopt);
    check_throws([&] { (void)recording_options_from_environment(); }, "missing batch ID is rejected");
    env.set(3, "batch-1");
    env.set(2, "bad/id");
    check_throws([&] { (void)recording_options_from_environment(); }, "path separator in run ID is rejected");
    env.set(2, std::string(97, 'a'));
    check_throws([&] { (void)recording_options_from_environment(); }, "overlong run ID is rejected");
    env.set(2, "run-1");
    env.set(3, "\xc3\xa9");
    check_throws([&] { (void)recording_options_from_environment(); }, "non-ASCII batch ID is rejected");
    env.set(3, "batch-1");
    env.set(4, "..");
    check_throws([&] { (void)recording_options_from_environment(); }, "unsafe baseline ID is rejected");
    env.set(4, "B0");
    env.set(5, "abc123");
    check_throws([&] { (void)recording_options_from_environment(); }, "short baseline commit is rejected");
    env.set(5, "abc123g");
    check_throws([&] { (void)recording_options_from_environment(); }, "nonhex baseline commit is rejected");
    env.set(5, "abcdef0");
    const auto parsed = recording_options_from_environment();
    check(parsed && parsed->role == "baseline" && parsed->baseline_commit == "abcdef0" &&
          parsed->directory == run, "valid environment produces expected options");
    check(!fs::exists(run), "environment parsing never creates the run directory");
    auto recording = start_runtime_recording(Fields{{"test_context", std::string("synthetic")}});
    check(recording && active_observer() == recording->observer(),
          "enabled factory starts and publishes a recording");
    check(recording->close("test_complete", true), "enabled factory recording closes");
    const auto recovered = read_journal(run / "events.journal");
    check(recovered.complete() && !recovered.records.empty(),
          "enabled factory creates a complete journal");
}

void test_metadata_and_directories() {
    TempRoot root;
    const auto reserved = root.path / "reserved";
    check_throws([&] { RuntimeRecording recording(options(reserved), Fields{{"role", "candidate"}}); },
                 "duplicate reserved metadata is rejected");
    check(!fs::exists(reserved), "duplicate reserved metadata creates no directory");
    const auto duplicate = root.path / "duplicate";
    check_throws([&] { RuntimeRecording recording(options(duplicate), Fields{{"x", true}, {"x", false}}); },
                 "duplicate caller metadata is rejected");
    check(!fs::exists(duplicate), "duplicate caller metadata creates no directory");
    const auto utf8 = root.path / "utf8";
    check_throws([&] { RuntimeRecording recording(options(utf8), Fields{{"x", std::string("\xff")}}); },
                 "invalid UTF-8 metadata is rejected");
    check(!fs::exists(utf8), "invalid UTF-8 metadata creates no directory");
    const auto large = root.path / "large";
    check_throws([&] { RuntimeRecording recording(options(large), Fields{{"x", std::string(kMaxPayloadBytes, 'x')}}); },
                 "oversized metadata is rejected");
    check(!fs::exists(large), "oversized metadata creates no directory");

    const auto existing = root.path / "existing";
    fs::create_directory(existing);
    check_throws([&] { RuntimeRecording recording(options(existing)); },
                 "existing directory cannot be reused");
    check(fs::is_directory(existing) && fs::is_empty(existing), "existing directory remains untouched");
    const auto file = root.path / "file";
    { std::ofstream stream(file); stream << "marker"; }
    check_throws([&] { RuntimeRecording recording(options(file)); }, "existing file cannot be overwritten");
    { std::ifstream stream(file); std::string content; stream >> content;
      check(content == "marker", "existing file remains untouched"); }
    const auto missing_parent = root.path / "missing" / "run";
    check_throws([&] { RuntimeRecording recording(options(missing_parent)); },
                 "missing parent is rejected");
    check(!fs::exists(missing_parent.parent_path()), "missing parent is not created");

    const auto final_link = root.path / "final-link";
    fs::create_symlink(root.path / "absent-target", final_link);
    check_throws([&] { RuntimeRecording recording(options(final_link)); },
                 "dangling final symlink is rejected");
    check(fs::is_symlink(fs::symlink_status(final_link)), "final symlink remains untouched");
    const auto parent_link = root.path / "parent-link";
    fs::create_directory_symlink(existing, parent_link);
    check_throws([&] { RuntimeRecording recording(options(parent_link / "run")); },
                 "symlink parent is rejected");
    check(!fs::exists(existing / "run"), "symlink parent target is untouched");
}

void test_lifecycle() {
    TempRoot root;
    const auto path = root.path / "explicit";
    auto recording = std::make_unique<RuntimeRecording>(options(path),
        Fields{{"test_context", std::string("synthetic")}});
    check(fs::is_directory(path) && fs::exists(path / "events.journal"),
          "startup creates one run directory and journal");
    check(active_observer() == recording->observer(), "startup publishes observer after opening flush");
    const auto prefix = read_journal(path / "events.journal");
    check(prefix.issue == RecoveryIssue::Open && prefix.records.size() == 1,
          "opening record is flushed before constructor returns");
    if (!prefix.records.empty()) check_initial_fields(prefix.records.front());

    const auto collision = root.path / "collision";
    check_throws([&] { RuntimeRecording other(options(collision)); },
                 "active singleton blocks a second recording");
    check(!fs::exists(collision), "singleton collision creates no second directory");
    check(active_observer() == recording->observer(), "collision leaves first observer active");
    set_active_observer({});
    check_throws([&] { RuntimeRecording other(options(collision)); },
                 "singleton blocks a second recording if routing is detached");
    check(!fs::exists(collision), "detached singleton creates no second directory");
    set_active_observer(recording->observer());

    recording->observer()->emit(EventKind::Input, "test.synthetic", Fields{{"value", std::uint64_t{3}}});
    check(recording->close("window_quit", true), "normal close succeeds");
    check(recording->close("ignored_second_close", false), "second close returns first success");
    check(!active_observer(), "close detaches its observer");
    const auto journal = read_journal(path / "events.journal");
    check(journal.complete() && journal.records.size() == 4,
          "normal journal has begin, input, health and one end");
    if (journal.records.size() == 4) {
        check_initial_fields(journal.records.front());
        check(journal.records[1].kind == EventKind::Input, "synthetic observation persisted");
        check(journal.records[2].kind == EventKind::State &&
              contains(journal.records[2].payload, "\"event\":\"observer.health\"") &&
              contains(journal.records[2].payload, "\"emission_errors\":0") &&
              contains(journal.records[2].payload, "\"accepted_events\":1") &&
              contains(journal.records[2].payload, "\"dropped_events\":0") &&
              contains(journal.records[2].payload, "\"invalid_events\":0") &&
              contains(journal.records[2].payload, "\"io_failed\":false"),
              "final observer health includes emission and recorder status");
        check(journal.records[3].kind == EventKind::RunEnd &&
              contains(journal.records[3].payload, "\"stop_reason\":\"window_quit\"") &&
              contains(journal.records[3].payload, "\"completed\":true"),
              "explicit close records completion intent");
    }

    const auto implicit = root.path / "implicit";
    {
        RuntimeRecording unfinished(options(implicit));
        check(active_observer() == unfinished.observer(), "implicit test observer is active");
    }
    check(!active_observer(), "destructor detaches observer");
    const auto ended = read_journal(implicit / "events.journal");
    check(ended.complete() && ended.records.size() == 3, "destructor writes health and RunEnd");
    if (ended.records.size() == 3)
        check(contains(ended.records.back().payload, "\"stop_reason\":\"runtime_scope_unfinished\"") &&
              contains(ended.records.back().payload, "\"completed\":false"),
              "destructor records unfinished intent");

    const auto replaced = root.path / "replaced";
    RuntimeRecording own(options(replaced));
    auto replacement = recording->observer();
    set_active_observer(replacement);
    check(own.close("replaced", false), "run closes after observer replacement");
    check(active_observer() == replacement, "close does not detach another observer");
    set_active_observer({});

    const auto error_path = root.path / "emission-error";
    RuntimeRecording error_recording(options(error_path));
    error_recording.observer()->emit(EventKind::State, "test.bad_field", Fields{{"event", "duplicate"}});
    check(!error_recording.close("test_complete", true),
          "observer emission failure prevents a clean close result");
    const auto error_journal = read_journal(error_path / "events.journal");
    check(error_journal.complete() && error_journal.records.size() == 3,
          "emission failure still produces diagnostic health and RunEnd");
    if (error_journal.records.size() == 3)
        check(contains(error_journal.records[1].payload, "\"emission_errors\":1"),
              "final health captures observer emission failure");
}

} // namespace

int main() {
    try {
        test_disabled_and_environment();
        test_metadata_and_directories();
        test_lifecycle();
    } catch (const std::exception &error) {
        std::cerr << "uncaught test exception: " << error.what() << '\n';
        return 1;
    }
    if (failures)
        std::cerr << failures << " runtime recording assertions failed\n";
    return failures ? 1 : 0;
}
