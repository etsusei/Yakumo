#include "camera/camera_input.hpp"
#include "settings/settings.hpp"
#include "testing/game_observers.hpp"
#include "psprecomp/sha256.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace mhp3rd;
using namespace mhp3rd::testing;
using Bytes = std::vector<std::uint8_t>;

int failures{};
void check(bool condition, std::string_view message) {
    if (!condition && ++failures <= 50) std::cerr << "FAIL: " << message << '\n';
}

struct MemoryState {
    std::mutex mutex;
    Bytes bytes;
    Bytes copy() {
        std::lock_guard lock(mutex);
        return bytes;
    }
};

class MemorySink final : public JournalSink {
public:
    explicit MemorySink(std::shared_ptr<MemoryState> state) : state_(std::move(state)) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        std::lock_guard lock(state_->mutex);
        state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
private:
    std::shared_ptr<MemoryState> state_;
};

struct RecorderFixture {
    std::shared_ptr<MemoryState> state = std::make_shared<MemoryState>();
    std::shared_ptr<SessionRecorder> recorder = std::make_shared<SessionRecorder>(
        std::make_unique<MemorySink>(state), Fields{{"role", std::string("synthetic_hook_test")}});
    std::shared_ptr<GameObserver> observer = std::make_shared<GameObserver>(recorder);

    JournalRecovery close() {
        check(recorder->close("test_done"), "synthetic recorder closes cleanly");
        const auto recovered = recover_journal(state->copy());
        check(recovered.complete(), "synthetic hook journal recovers completely");
        return recovered;
    }
};

// The recorder's JSON builder emits flat objects. This small extractor checks
// typed scalar fields without making test results depend on member order.
std::optional<std::string> json_token(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const auto found = json.find(needle);
    if (found == std::string_view::npos) return std::nullopt;
    std::size_t first = found + needle.size();
    if (first >= json.size()) return std::nullopt;
    if (json[first] == '"') {
        std::size_t last = first + 1;
        bool escaped = false;
        for (; last < json.size(); ++last) {
            const char character = json[last];
            if (character == '"' && !escaped)
                return std::string(json.substr(first, last - first + 1));
            if (character == '\\' && !escaped) escaped = true;
            else escaped = false;
        }
        return std::nullopt;
    }
    const auto last = json.find_first_of(",}", first);
    if (last == std::string_view::npos) return std::nullopt;
    return std::string(json.substr(first, last - first));
}
std::string json_string(std::string_view json, std::string_view key) {
    const auto token = json_token(json, key);
    if (!token || token->size() < 2 || token->front() != '"' || token->back() != '"')
        throw std::runtime_error("missing string field: " + std::string(key));
    return token->substr(1, token->size() - 2);
}
double json_number(std::string_view json, std::string_view key) {
    const auto token = json_token(json, key);
    if (!token || token->empty() || token->front() == '"')
        throw std::runtime_error("missing number field: " + std::string(key));
    return std::stod(*token);
}
bool json_bool(std::string_view json, std::string_view key) {
    const auto token = json_token(json, key);
    if (!token || (*token != "true" && *token != "false"))
        throw std::runtime_error("missing Boolean field: " + std::string(key));
    return *token == "true";
}
std::vector<const JournalRecord *> named_events(const JournalRecovery &recovered,
                                                 std::string_view name) {
    std::vector<const JournalRecord *> result;
    for (const auto &record : recovered.records) {
        if (record.kind == EventKind::RunBegin || record.kind == EventKind::RunEnd ||
            record.kind == EventKind::RecordingLoss) continue;
        if (json_string(record.payload, "event") == name) result.push_back(&record);
    }
    return result;
}
const JournalRecord *camera_action(const std::vector<const JournalRecord *> &records,
                                   std::string_view action, std::size_t occurrence = 0) {
    for (const auto *record : records) {
        if (json_string(record->payload, "action") == action) {
            if (occurrence-- == 0) return record;
        }
    }
    return nullptr;
}
void near(double actual, double expected, std::string_view message) {
    check(std::isfinite(actual) && std::fabs(actual - expected) < 0.00001, message);
}

struct CameraResult {
    camera::Rate rate;
    camera::Turn peek_mouse;
    camera::Turn consume_mouse;
    camera::Turn consume_rest;
    camera::Turn after_discard;
    camera::Turn after_reset;
};
CameraResult exercise_camera() {
    using camera::Source;
    camera::reset();
    camera::set_rate(Source::Stick, 0.5f, -0.25f);
    camera::add_motion(Source::Mouse, 2.0f, 1.0f);
    camera::add_motion(Source::Touch, -0.5f, 3.0f);
    camera::advance(0.2f, 100.0f); // The camera caps one step at 0.1 seconds.
    CameraResult result;
    result.rate = camera::rate(Source::Stick);
    result.peek_mouse = camera::peek(Source::Mouse);
    result.consume_mouse = camera::take(Source::Mouse);
    result.consume_rest = camera::take();
    camera::add_motion(Source::Mouse, 7.0f, -2.0f);
    camera::discard();
    result.after_discard = camera::take();
    camera::reset();
    result.after_reset = camera::take();
    return result;
}
void same_turn(const camera::Turn &actual, const camera::Turn &expected,
               std::string_view message) {
    check(std::fabs(actual.yaw_degrees - expected.yaw_degrees) < 0.00001f &&
          std::fabs(actual.pitch_degrees - expected.pitch_degrees) < 0.00001f &&
          actual.yaw_held == expected.yaw_held &&
          actual.pitch_held == expected.pitch_held, message);
}
void same_result(const CameraResult &actual, const CameraResult &expected,
                 std::string_view message) {
    check(actual.rate.yaw == expected.rate.yaw && actual.rate.pitch == expected.rate.pitch, message);
    same_turn(actual.peek_mouse, expected.peek_mouse, message);
    same_turn(actual.consume_mouse, expected.consume_mouse, message);
    same_turn(actual.consume_rest, expected.consume_rest, message);
    same_turn(actual.after_discard, expected.after_discard, message);
    same_turn(actual.after_reset, expected.after_reset, message);
}

void test_camera_hook() {
    set_active_observer({});
    const auto disabled = exercise_camera();
    near(disabled.rate.yaw, 0.5, "held stick rate is unchanged without observation");
    near(disabled.peek_mouse.yaw_degrees, 2.0, "peek exposes unconsumed mouse motion");
    near(disabled.consume_mouse.yaw_degrees, 2.0, "source take consumes only mouse motion");
    near(disabled.consume_rest.yaw_degrees, 4.5, "aggregate take keeps rate plus touch motion");
    near(disabled.consume_rest.pitch_degrees, 0.5, "aggregate pitch keeps rate plus touch motion");
    check(disabled.consume_rest.yaw_held && disabled.consume_rest.pitch_held &&
          disabled.after_discard.yaw_degrees == 0.0f &&
          disabled.after_reset.pitch_degrees == 0.0f,
          "discard and reset preserve camera consumption semantics");

    RecorderFixture fixture;
    set_active_observer(fixture.observer);
    const auto observed = exercise_camera();
    same_result(observed, disabled, "active observation does not change camera result");
    set_active_observer({});
    const auto recovered = fixture.close();
    const auto records = named_events(recovered, "camera.observation");
    check(records.size() == 14, "real camera functions emit submission, consumption and reset records");
    const auto *rate = camera_action(records, "rate");
    const auto *mouse = camera_action(records, "motion");
    const auto *touch = camera_action(records, "motion", 1);
    const auto *advance = camera_action(records, "advance");
    const auto *consume_mouse = camera_action(records, "consume_source");
    const auto *consume_rest = camera_action(records, "consume_all");
    const auto *discard = camera_action(records, "discard", 1);
    const auto *reset = camera_action(records, "reset", 1);
    check(rate && mouse && touch && advance && consume_mouse && consume_rest && discard && reset,
          "camera journal contains each operation boundary");
    if (rate && mouse && touch && advance && consume_mouse && consume_rest && discard && reset) {
        check(json_number(rate->payload, "source") == 0 &&
              json_bool(rate->payload, "yaw_held") && json_bool(rate->payload, "pitch_held"),
              "rate submission identifies held stick axes");
        near(json_number(rate->payload, "yaw"), 0.5, "rate yaw matches applied value");
        check(json_number(mouse->payload, "source") == 2 &&
              json_number(touch->payload, "source") == 3,
              "mouse and touch motion remain separate sources");
        near(json_number(mouse->payload, "yaw"), 2.0, "mouse submission matches applied degrees");
        near(json_number(touch->payload, "pitch"), 3.0, "touch submission matches applied degrees");
        near(json_number(advance->payload, "yaw"), 5.0, "advance records capped held-rate yaw");
        near(json_number(advance->payload, "seconds"), 0.1, "advance records capped seconds");
        near(json_number(advance->payload, "degrees_per_second"), 100.0,
             "advance records configured camera speed");
        near(json_number(consume_mouse->payload, "yaw"), 2.0,
             "source consumption matches returned mouse turn");
        near(json_number(consume_rest->payload, "yaw"), 4.5,
             "aggregate consumption matches returned remaining turn");
        near(json_number(discard->payload, "yaw"), 7.0,
             "discard records motion removed before later camera take");
        near(json_number(reset->payload, "yaw"), 0.0, "reset is an explicit observation");
    }
    check(fixture.observer->emission_errors() == 0,
          "camera operations were accepted by the real recorder");

    set_active_observer(fixture.observer);
    const auto closed = exercise_camera();
    set_active_observer({});
    same_result(closed, disabled, "closed recorder cannot change camera result");
    check(fixture.observer->emission_errors() > 0,
          "closed recorder failure is visible without affecting camera state");
}

class IsolatedDataDir {
public:
    IsolatedDataDir() {
        if (const char *value = std::getenv("MHP3RD_DATA_DIR")) previous_ = value;
        path_ = std::filesystem::temp_directory_path() /
            ("yakumo-observation-hook-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(path_))
            throw std::runtime_error("cannot create isolated settings directory");
        std::ofstream(path_ / "settings.ini") << "audio.volume=41\n";
        set(path_.string());
    }
    ~IsolatedDataDir() {
        try {
            if (previous_) set(*previous_);
            else unset();
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        } catch (...) {
        }
    }
    IsolatedDataDir(const IsolatedDataDir &) = delete;
    IsolatedDataDir &operator=(const IsolatedDataDir &) = delete;
private:
    std::filesystem::path path_;
    std::optional<std::string> previous_;
    static void set(const std::string &value) {
#if defined(_WIN32)
        if (_putenv_s("MHP3RD_DATA_DIR", value.c_str()) != 0)
#else
        if (setenv("MHP3RD_DATA_DIR", value.c_str(), 1) != 0)
#endif
            throw std::runtime_error("cannot set isolated settings directory");
    }
    static void unset() {
#if defined(_WIN32)
        if (_putenv_s("MHP3RD_DATA_DIR", "") != 0)
#else
        if (unsetenv("MHP3RD_DATA_DIR") != 0)
#endif
            throw std::runtime_error("cannot restore settings directory environment");
    }
};

std::string digest(std::string_view text) {
    return psprecomp::sha256_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

void test_settings_snapshot() {
    IsolatedDataDir isolated; // Must precede the first settings::current() call.
    auto &values = settings::current();
    check(values.volume == 41, "settings load reads only the isolated synthetic directory");
    values.volume = 37;
    values.name = "PrivateName456";
    values.adhoc_server = "private-server.example.invalid";
    values.last_folder = "/private/user/folder";
    values.font = "/private/font.otf";
    values.texture_pack_folder = "/private/texture-pack";
    values.adhoc_nickname = "PrivateNickname";
    values.adhoc_mac = "02:AA:BB:CC:DD:EE";
    values.adhoc_recent = {"private-room"};

    RecorderFixture fixture;
    set_active_observer(fixture.observer);
    settings::record_snapshot();
    settings::record_snapshot();
    values.volume = 38;
    settings::record_snapshot();
    settings::record_snapshot();
    set_active_observer({});
    const auto recovered = fixture.close();
    const auto snapshots = named_events(recovered, "config.effective");
    check(snapshots.size() == 2, "identical effective settings deduplicate and a change emits again");
    if (snapshots.size() == 2) {
        const auto &first = snapshots[0]->payload;
        const auto &second = snapshots[1]->payload;
        check(json_string(first, "audio.volume") == "37" &&
              json_string(second, "audio.volume") == "38",
              "changed effective volume appears in its own snapshot");
        check(json_string(first, "settings_sha256") != json_string(second, "settings_sha256"),
              "changed effective settings alter the snapshot fingerprint");
        for (const auto &[key, expected] : {
                 std::pair<std::string_view, std::string_view>{"input.name.sha256", "PrivateName456"},
                 {"network.server.sha256", "private-server.example.invalid"},
                 {"ui.last_folder.sha256", "/private/user/folder"},
                 {"text.font.sha256", "/private/font.otf"},
                 {"video.texture_pack_folder.sha256", "/private/texture-pack"},
                 {"network.nickname.sha256", "PrivateNickname"},
                 {"network.mac.sha256", "02:AA:BB:CC:DD:EE"}}) {
            check(json_string(first, key) == digest(expected) &&
                  json_string(second, key) == digest(expected),
                  "sensitive effective setting is fingerprinted consistently");
        }
    }
    const auto bytes = fixture.state->copy();
    const std::string journal(bytes.begin(), bytes.end());
    for (const std::string_view private_value : {
             "PrivateName456", "private-server.example.invalid", "/private/user/folder",
             "/private/font.otf", "/private/texture-pack", "PrivateNickname",
             "02:AA:BB:CC:DD:EE", "private-room"}) {
        check(journal.find(private_value) == std::string::npos,
              "raw private settings are absent from the journal");
    }
    check(fixture.observer->emission_errors() == 0,
          "effective setting snapshots were accepted by the recorder");
}
} // namespace

int main() {
    try {
        test_camera_hook();
        test_settings_snapshot();
    } catch (const std::exception &error) {
        set_active_observer({});
        std::cerr << "Unhandled observation hook test exception: " << error.what() << '\n';
        return 1;
    }
    return failures ? 1 : 0;
}
