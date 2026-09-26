#include "testing/game_observers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace mhp3rd::testing;
using Bytes = std::vector<std::uint8_t>;

int failures = 0;
void check(bool condition, std::string_view message) {
    if (!condition && ++failures <= 50) std::cerr << "FAIL: " << message << '\n';
}

struct MemoryState {
    std::mutex mutex;
    Bytes bytes;
    std::thread::id sink_thread;
    bool fail_write{};
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
        state_->sink_thread = std::this_thread::get_id();
        if (state_->fail_write) return false;
        state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
private:
    std::shared_ptr<MemoryState> state_;
};

struct Fixture {
    std::shared_ptr<MemoryState> state = std::make_shared<MemoryState>();
    std::shared_ptr<SessionRecorder> recorder;
    std::shared_ptr<GameObserver> observer;

    explicit Fixture(RecorderOptions options = {}, bool fail_write = false) {
        state->fail_write = fail_write;
        recorder = std::make_shared<SessionRecorder>(std::make_unique<MemorySink>(state),
            Fields{{"role", std::string("synthetic")}}, std::move(options));
        observer = std::make_shared<GameObserver>(recorder);
    }
    JournalRecovery finish() {
        check(recorder->close("test_done"), "synthetic recorder closes cleanly");
        return recover_journal(state->copy());
    }
};

enum class ValueKind { Null, Boolean, Number, String };
struct Value {
    ValueKind kind{};
    std::string text;
};
using Object = std::map<std::string, Value>;

class FlatJsonParser {
public:
    explicit FlatJsonParser(std::string_view input) : input_(input) {}
    Object parse() {
        Object result;
        spaces();
        take('{');
        spaces();
        if (peek('}')) {
            take('}');
            finish();
            return result;
        }
        for (;;) {
            spaces();
            const auto name = string();
            spaces();
            take(':');
            spaces();
            const auto [it, inserted] = result.emplace(name, value());
            (void)it;
            if (!inserted) throw std::runtime_error("duplicate JSON field");
            spaces();
            if (peek('}')) {
                take('}');
                finish();
                return result;
            }
            take(',');
        }
    }
private:
    std::string_view input_;
    std::size_t position_{};

    void spaces() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\n' ||
                input_[position_] == '\r' || input_[position_] == '\t')) ++position_;
    }
    bool peek(char expected) const {
        return position_ < input_.size() && input_[position_] == expected;
    }
    void take(char expected) {
        if (!peek(expected)) throw std::runtime_error("unexpected JSON token");
        ++position_;
    }
    void finish() {
        spaces();
        if (position_ != input_.size()) throw std::runtime_error("trailing JSON data");
    }
    static int hex(char character) {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        throw std::runtime_error("invalid JSON escape");
    }
    std::string string() {
        take('"');
        std::string result;
        while (position_ < input_.size()) {
            const auto character = input_[position_++];
            if (character == '"') return result;
            if (character != '\\') {
                if (static_cast<unsigned char>(character) < 0x20u)
                    throw std::runtime_error("unescaped JSON control");
                result.push_back(character);
                continue;
            }
            if (position_ == input_.size()) throw std::runtime_error("incomplete JSON escape");
            const auto escaped = input_[position_++];
            switch (escaped) {
            case '"': case '\\': case '/': result.push_back(escaped); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (input_.size() - position_ < 4) throw std::runtime_error("short Unicode escape");
                int code = 0;
                for (int i = 0; i < 4; ++i) code = (code << 4) | hex(input_[position_++]);
                if (code > 0x7f) throw std::runtime_error("test parser expects ASCII escapes");
                result.push_back(static_cast<char>(code));
                break;
            }
            default: throw std::runtime_error("invalid JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }
    Value value() {
        if (peek('"')) return {ValueKind::String, string()};
        for (const auto [token, kind] : {
                 std::pair<std::string_view, ValueKind>{"null", ValueKind::Null},
                 {"true", ValueKind::Boolean}, {"false", ValueKind::Boolean}}) {
            if (input_.substr(position_).starts_with(token)) {
                position_ += token.size();
                return {kind, std::string(token)};
            }
        }
        const auto start = position_;
        while (position_ < input_.size() &&
               (input_[position_] == '-' || input_[position_] == '+' ||
                input_[position_] == '.' || input_[position_] == 'e' ||
                input_[position_] == 'E' ||
                (input_[position_] >= '0' && input_[position_] <= '9'))) ++position_;
        if (position_ == start) throw std::runtime_error("invalid JSON value");
        return {ValueKind::Number, std::string(input_.substr(start, position_ - start))};
    }
};

struct Event {
    EventKind kind{};
    std::uint64_t sequence{};
    Object fields;
};
std::vector<Event> events(const JournalRecovery &recovery) {
    check(recovery.complete(), "synthetic journal has complete framing");
    std::vector<Event> result;
    for (const auto &record : recovery.records) {
        if (record.kind == EventKind::RunBegin || record.kind == EventKind::RunEnd ||
            record.kind == EventKind::RecordingLoss) continue;
        result.push_back({record.kind, record.sequence, FlatJsonParser(record.payload).parse()});
    }
    return result;
}
const Value &field(const Event &event, std::string_view name) {
    const auto it = event.fields.find(std::string(name));
    if (it == event.fields.end()) throw std::runtime_error("missing field: " + std::string(name));
    return it->second;
}
std::string text(const Event &event, std::string_view name) { return field(event, name).text; }
std::uint64_t unsigned_number(const Event &event, std::string_view name) {
    const auto &value = field(event, name);
    if (value.kind != ValueKind::Number) throw std::runtime_error("not numeric");
    return std::stoull(value.text);
}
bool boolean(const Event &event, std::string_view name) {
    const auto &value = field(event, name);
    if (value.kind != ValueKind::Boolean) throw std::runtime_error("not Boolean");
    return value.text == "true";
}
const Event *find_event(const std::vector<Event> &items, std::string_view name,
                        std::size_t occurrence = 0) {
    for (const auto &item : items) {
        if (text(item, "event") == name) {
            if (occurrence-- == 0) return &item;
        }
    }
    return nullptr;
}
void common_fields(const std::vector<Event> &items) {
    for (const auto &event : items) {
        check(field(event, "event").kind == ValueKind::String, "event name is typed string");
        check(field(event, "domain").kind == ValueKind::String, "domain is typed string");
        check(field(event, "guest_frame").kind == ValueKind::Number, "guest frame is typed number");
        check(field(event, "virtual_us").kind == ValueKind::Null ||
              field(event, "virtual_us").kind == ValueKind::Number, "virtual time is number or null");
        check(field(event, "vblank").kind == ValueKind::Null ||
              field(event, "vblank").kind == ValueKind::Number, "vblank is number or null");
        check(field(event, "control_read_ordinal").kind == ValueKind::Number,
              "read ordinal is typed number");
        check(field(event, "observation_ordinal").kind == ValueKind::Number,
              "observation ordinal is typed number");
        check(field(event, "focused").kind == ValueKind::Boolean, "focus is typed Boolean");
    }
}

void test_disabled_routing_and_lifetime() {
    set_active_observer({});
    check(!active_observer(), "disabled routing returns no observer");
    Fixture fixture;
    set_active_observer(fixture.observer);
    auto lease = active_observer();
    check(lease == fixture.observer, "active routing gives a shared lifetime");
    set_active_observer({});
    check(!active_observer(), "disabled routing detaches immediately");
    lease->emit(EventKind::State, "lease.alive");
    const auto decoded = events(fixture.finish());
    check(decoded.size() == 1 && text(decoded.front(), "event") == "lease.alive",
          "in-flight shared observer remains usable after detach");
    common_fields(decoded);
}

void test_domain_focus_and_time() {
    Fixture fixture;
    auto &observer = *fixture.observer;
    const auto initial = observer.timeline();
    check(initial.domain == InputDomain::Setup && initial.guest_frame == 0 &&
          !initial.virtual_us && !initial.vblank && !initial.focused,
          "timeline starts in setup with unknown guest clock");
    observer.emit(EventKind::State, "before.clock");
    observer.time(10, 2);
    observer.domain(InputDomain::Game);
    observer.domain(InputDomain::Game);
    observer.text_input(true);
    observer.domain(InputDomain::PausedUi);
    observer.text_input(false);
    observer.text_input(false);
    observer.text_input(true);
    observer.domain(InputDomain::OverGameUi);
    observer.text_input(false);
    observer.focus(true);
    observer.focus(true);
    observer.focus(false);
    observer.focus(false);
    const auto timeline = observer.timeline();
    check(timeline.guest_frame == 0 && timeline.virtual_us == 10 && timeline.vblank == 2 &&
          timeline.domain == InputDomain::OverGameUi && !timeline.focused,
          "time update and nested text domain preserve frame and latest underlying domain");
    const auto decoded = events(fixture.finish());
    common_fields(decoded);
    check(decoded.size() == 8, "only actual domain and focus transitions are recorded");
    if (decoded.size() == 8) {
        check(field(decoded[0], "virtual_us").kind == ValueKind::Null &&
              field(decoded[0], "vblank").kind == ValueKind::Null,
              "unknown clock appears explicitly as null");
        check(text(decoded[1], "to_domain") == "game" &&
              unsigned_number(decoded[1], "virtual_us") == 10,
              "time updates next observation without adding a frame");
        check(text(decoded[2], "to_domain") == "text_input" &&
              text(decoded[3], "from_domain") == "text_input" &&
              text(decoded[3], "to_domain") == "paused_ui",
              "text input restores the current underlying paused domain");
        check(text(decoded[4], "to_domain") == "text_input" &&
              text(decoded[5], "to_domain") == "over_game_ui",
              "changing UI domain under text input takes effect when text ends");
        check(boolean(decoded[6], "to_focused") && !boolean(decoded[7], "to_focused"),
              "focus transitions include the transition to unfocused");
    }
}

void test_modal_domain_restore() {
    Fixture fixture;
    auto &observer = *fixture.observer;
    observer.domain(InputDomain::OverGameUi);
    observer.text_input(true);
    const auto previous = observer.underlying_domain();
    check(previous == InputDomain::OverGameUi, "modal scope saves underlying domain, not text overlay");
    observer.domain(InputDomain::PausedUi);
    observer.text_input(false);
    observer.domain(previous);
    check(observer.timeline().domain == InputDomain::OverGameUi,
          "modal restoration cannot resurrect a text input that already closed");
    common_fields(events(fixture.finish()));
}

void test_pointer_details() {
    Fixture fixture;
    fixture.observer->focus(true);
    WindowObservation click{WindowEventKind::MouseButton};
    click.owned_window = true; click.focused = true; click.down = true;
    click.window_id = 4; click.source_event_type = 1025; click.code = 1;
    click.x = 12.5; click.y = 34.25;
    fixture.observer->window(click);
    auto touch = click;
    touch.kind = WindowEventKind::Touch; touch.source_event_type = 1793; touch.down = false;
    fixture.observer->window(touch);
    const auto decoded = events(fixture.finish());
    const auto *button = find_event(decoded, "window.mouse_button");
    const auto *finger = find_event(decoded, "window.touch");
    check(button && std::stod(field(*button, "x").text) == 12.5 &&
          std::stod(field(*button, "y").text) == 34.25, "click position survives journal serialization");
    check(finger && !boolean(*finger, "down") && unsigned_number(*finger, "source_event_type") == 1793,
          "touch release retains state and raw event type");
}

void test_window_filter_and_privacy() {
    Fixture fixture;
    auto &observer = *fixture.observer;
    WindowObservation other_key{WindowEventKind::Key};
    other_key.code = 77;
    observer.window(other_key);
    WindowObservation other_close{WindowEventKind::Close};
    other_close.window_id = 99;
    observer.window(other_close);
    WindowObservation global_added{WindowEventKind::DeviceAdded};
    global_added.device_id = 17;
    observer.window(global_added);
    WindowObservation global_close{WindowEventKind::Close};
    observer.window(global_close);
    WindowObservation focus{WindowEventKind::Focus};
    focus.owned_window = true;
    focus.focused = true;
    focus.window_id = 4;
    focus.source_timestamp_ns = 81;
    observer.window(focus);
    WindowObservation key{WindowEventKind::Key};
    key.owned_window = true;
    key.window_id = 4;
    key.code = 23;
    key.down = true;
    key.ui_consumed = true;
    key.scripted_mode = true;
    key.source_timestamp_ns = 82;
    const auto unchanged_key = key;
    observer.window(key);
    check(key.code == unchanged_key.code && key.down == unchanged_key.down &&
          key.ui_consumed == unchanged_key.ui_consumed && key.source_timestamp_ns == unchanged_key.source_timestamp_ns,
          "window observation is not modified");
    WindowObservation text_input{WindowEventKind::Text};
    text_input.owned_window = true;
    text_input.text_bytes = 12;
    text_input.code = 123456;
    observer.window(text_input);
    WindowObservation drop{WindowEventKind::FileDrop};
    drop.owned_window = true;
    drop.text_bytes = 34;
    observer.window(drop);
    observer.script("text", 7, "private typed content");
    observer.script("drop", 8, "/private/path");
    observer.script("key", 9, "Escape");
    focus.focused = false;
    observer.window(focus);
    observer.window(key);
    WindowObservation owned_close{WindowEventKind::Close};
    owned_close.owned_window = true;
    owned_close.window_id = 4;
    observer.window(owned_close);
    const auto recovered = fixture.finish();
    const auto decoded = events(recovered);
    common_fields(decoded);
    check(decoded.size() == 11, "window routing excludes foreign and unfocused controls");
    const auto *focus_lost = find_event(decoded, "input.focus", 1);
    check(focus_lost && !boolean(*focus_lost, "focused") &&
          !boolean(*focus_lost, "to_focused"),
          "own focus loss is recorded while the observer is unfocused");
    const auto *global = find_event(decoded, "window.device_added");
    check(global && !boolean(*global, "owned_window") &&
          unsigned_number(*global, "device_id") == 17,
          "global device metadata retains its unowned provenance");
    const auto *global_quit = find_event(decoded, "window.close");
    check(global_quit && !boolean(*global_quit, "owned_window") &&
          unsigned_number(*global_quit, "window_id") == 0,
          "global quit metadata is retained without claiming a window");
    const auto *key_event = find_event(decoded, "window.key");
    check(key_event && boolean(*key_event, "ui_consumed") &&
          boolean(*key_event, "scripted_mode") &&
          unsigned_number(*key_event, "source_timestamp_ns") == 82,
          "accepted key records UI consumption and source timing");
    const auto *text_event = find_event(decoded, "window.text");
    const auto *drop_event = find_event(decoded, "window.file_drop");
    check(text_event && unsigned_number(*text_event, "text_bytes") == 12 &&
          !text_event->fields.contains("code") && drop_event &&
          unsigned_number(*drop_event, "text_bytes") == 34,
          "text and drop events keep lengths without incidental code fields");
    const auto bytes = fixture.state->copy();
    const std::string journal(bytes.begin(), bytes.end());
    check(journal.find("private typed content") == std::string::npos &&
          journal.find("/private/path") == std::string::npos,
          "text and dropped file contents are absent from the journal");
}

void test_frame_and_control_reads() {
    Fixture fixture;
    auto &observer = *fixture.observer;
    observer.time(111, 7);
    PadObservation pad{112, 7, 0x80012345u, 3, 1, 2, 3, 4};
    const auto unchanged = pad;
    observer.pad(pad);
    observer.pad(pad);
    observer.frame(200, 8);
    pad.virtual_us = 201;
    pad.vblank = 8;
    observer.pad(pad);
    observer.frame(300, 9);
    check(unchanged.buttons == 0x80012345u && pad.buttons == unchanged.buttons &&
          pad.analog_x == unchanged.analog_x && pad.right_y == unchanged.right_y,
          "pad observation is not transformed");
    const auto timeline = observer.timeline();
    check(timeline.guest_frame == 2 && timeline.control_read_ordinal == 3 &&
          timeline.virtual_us == 300 && timeline.vblank == 9,
          "frame and successful control read ordinals advance independently");
    const auto decoded = events(fixture.finish());
    common_fields(decoded);
    check(decoded.size() == 5, "time-only update produces no frame event");
    if (decoded.size() == 5) {
        check(text(decoded[0], "event") == "input.pad" &&
              unsigned_number(decoded[0], "guest_frame") == 0 &&
              unsigned_number(decoded[0], "control_read_ordinal") == 1 &&
              unsigned_number(decoded[0], "sample_count") == 3 &&
              unsigned_number(decoded[0], "buttons") == 0x80012345u &&
              unsigned_number(decoded[0], "analog_x") == 1 &&
              unsigned_number(decoded[0], "right_y") == 4,
              "pad records final applied values and sample count");
        check(unsigned_number(decoded[1], "control_read_ordinal") == 2 &&
              unsigned_number(decoded[1], "guest_frame") == 0,
              "multiple reads can occur in one guest frame");
        check(text(decoded[2], "event") == "guest.frame" &&
              unsigned_number(decoded[2], "guest_frame") == 1 &&
              unsigned_number(decoded[2], "control_read_ordinal") == 2,
              "frame increments only the game frame counter");
        check(unsigned_number(decoded[3], "control_read_ordinal") == 3 &&
              unsigned_number(decoded[3], "guest_frame") == 1 &&
              unsigned_number(decoded[4], "guest_frame") == 2,
              "later read and frame keep separate order");
    }
}

void test_camera_actions_and_nonfinite_values() {
    Fixture fixture;
    CameraObservation motion;
    motion.action = CameraObservation::Action::Motion;
    motion.source = 2;
    motion.yaw = std::numeric_limits<double>::quiet_NaN();
    motion.pitch = -1.25;
    motion.seconds = std::numeric_limits<double>::infinity();
    motion.degrees_per_second = 90.0;
    motion.yaw_held = true;
    fixture.observer->camera(motion);
    check(std::isnan(motion.yaw) && std::isinf(motion.seconds) &&
          motion.pitch == -1.25 && motion.yaw_held,
          "camera observation remains immutable");
    motion.action = CameraObservation::Action::ConsumeSource;
    motion.source = 3;
    motion.yaw = 0;
    motion.seconds = 0;
    fixture.observer->camera(motion);
    const auto decoded = events(fixture.finish());
    common_fields(decoded);
    check(decoded.size() == 2 && text(decoded[0], "action") == "motion" &&
          text(decoded[1], "action") == "consume_source" &&
          unsigned_number(decoded[1], "source") == 3,
          "motion and source consumption remain distinct observations");
    if (decoded.size() == 2) {
        check(field(decoded[0], "yaw").kind == ValueKind::Null &&
              !boolean(decoded[0], "yaw_available") &&
              field(decoded[0], "seconds").kind == ValueKind::Null &&
              !boolean(decoded[0], "seconds_available") &&
              boolean(decoded[0], "pitch_available") &&
              boolean(decoded[0], "degrees_per_second_available"),
              "nonfinite camera values become explicit unavailable values");
    }
    check(fixture.observer->emission_errors() == 0,
          "nonfinite camera observations are recorded without rejection");
}

void test_overlay_epochs_and_ambiguity() {
    Fixture fixture;
    auto &observer = *fixture.observer;
    OverlayIdentity first{0x1000, 0x100, 0x80, "slot", "header-a", "code-a", true};
    const auto original = first;
    observer.overlay(first);
    observer.overlay(first);
    auto found = observer.overlay_at(0x1000);
    check(found && found->generation == 1 && found->code_epoch == 0 && found->active,
          "first overlay identity has generation one");
    first.code_fingerprint = "code-b";
    observer.overlay(first);
    found = observer.overlay_at(0x1000);
    check(found && found->generation == 2 && found->identity.code_fingerprint == "code-b",
          "new image at the same base advances generation");
    observer.code_epoch("observed_icache_invalidation");
    check(!observer.overlay_at(0x1000), "code epoch retires prior identity until refreshed");
    observer.overlay(first);
    found = observer.overlay_at(0x1000);
    check(found && found->generation == 3 && found->code_epoch == 1,
          "same image after epoch gets a distinct generation");
    OverlayIdentity overlapping{0x1080, 0x80, 0x40, "other", "header-c", "code-c", true};
    observer.overlay(overlapping);
    check(!observer.overlay_at(0x1080) && observer.overlay_at(0x1000),
          "overlapping active slots are unavailable instead of misattributed");
    observer.overlay_unload(0x1080, "unloaded");
    found = observer.overlay_at(0x1080);
    check(found && found->generation == 3, "unload removes ambiguity for remaining slot");
    OverlayIdentity unmatched{0x2000, 0x100, 0x80, "unknown", "header-u", "code-u", false};
    observer.overlay(unmatched);
    check(!observer.overlay_at(0x2000), "unmatched corpus identity is unavailable for attribution");
    OverlayIdentity invalid{0xfffffff0u, 0x100, 0x40, "bad", "h", "c", true};
    observer.overlay(invalid);
    invalid = {0x3000, 0x20, 0x40, "bad", "h", "c", true};
    observer.overlay(invalid);
    invalid = {0x3000, 0x20, 0x10, "bad", "", "c", true};
    observer.overlay(invalid);
    invalid = {0x1000, 0x20, 0x40, "bad", "h", "c", true};
    observer.overlay(invalid);
    invalid = {0x4000, 0x100, 0x10, std::string("\xff", 1), "h", "c", true};
    observer.overlay(invalid);
    check(observer.emission_errors() == 5 && !observer.overlay_at(0x3000) &&
          !observer.overlay_at(0x1000) && !observer.overlay_at(0x4000),
          "malformed ranges, code bounds, fingerprints and text are unavailable");
    check(original.code_fingerprint == "code-a" && first.code_fingerprint == "code-b",
          "overlay hook inputs retain their immutable fingerprints");
    const auto decoded = events(fixture.finish());
    common_fields(decoded);
    const auto *first_load = find_event(decoded, "overlay.load");
    const auto *third_load = find_event(decoded, "overlay.load", 2);
    const auto *epoch = find_event(decoded, "overlay.code_epoch");
    const auto *unload = find_event(decoded, "overlay.unload");
    check(first_load && third_load && epoch && unload &&
          unsigned_number(*first_load, "generation") == 1 &&
          unsigned_number(*third_load, "generation") == 3 &&
          text(*third_load, "generation_kind") == "code_validation_epoch" &&
          unsigned_number(*third_load, "code_epoch") == 1 &&
          unsigned_number(*epoch, "invalidated_slots") == 1 &&
          boolean(*third_load, "matched_corpus") &&
          !boolean(*unload, "active"),
          "overlay journal preserves epoch, generations, corpus match and unload");
}

void test_emission_failures() {
    Fixture fixture;
    auto &observer = *fixture.observer;
    observer.emit(EventKind::State, "invalid.reserved", {{"event", std::string("collision")}});
    observer.emit(EventKind::State, "invalid.duplicate", {{"x", true}, {"x", false}});
    observer.emit(EventKind::State, "invalid.nonfinite",
                  {{"x", std::numeric_limits<double>::infinity()}});
    check(observer.emission_errors() == 3, "reserved, duplicate and rejected fields count emission errors");
    auto recovered = fixture.finish();
    check(events(recovered).empty(), "invalid fields never write malformed payloads");
    observer.emit(EventKind::State, "after.close");
    check(observer.emission_errors() == 4, "closed recorder failure counts as emission error");

    RecorderOptions tiny;
    tiny.max_queue_bytes = 1;
    Fixture dropped(tiny);
    dropped.observer->emit(EventKind::Input, "queue.full");
    check(dropped.observer->emission_errors() == 1, "dropped enqueue counts as emission error");
    recovered = dropped.finish();
    check(recovered.loss_seen, "dropped observation leaves a recording-loss marker");

    Fixture broken({}, true);
    for (int attempt = 0; attempt < 1000 && !broken.recorder->health().io_failed; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(broken.recorder->health().io_failed, "synthetic sink exposes writer I/O failure");
    broken.observer->emit(EventKind::State, "after.io_failure");
    check(broken.observer->emission_errors() == 1, "I/O failure counts as emission error");
    check(!broken.recorder->close("expected_io_failure"), "failed sink cannot close successfully");
}

void test_concurrent_order_and_file_sink() {
    RecorderOptions options;
    options.max_queue_events = 4096;
    options.max_queue_bytes = 8u * 1024u * 1024u;
    Fixture fixture(options);
    std::vector<std::thread> producers;
    for (std::uint64_t producer = 0; producer < 4; ++producer) {
        producers.emplace_back([observer = fixture.observer, producer] {
            for (std::uint64_t index = 0; index < 100; ++index)
                observer->emit(EventKind::State, "concurrent.observation",
                               {{"producer", producer}, {"index", index}});
        });
    }
    for (auto &producer : producers) producer.join();
    check(fixture.observer->emission_errors() == 0, "finite concurrent producers enqueue without loss");
    const auto recovered = fixture.finish();
    const auto decoded = events(recovered);
    common_fields(decoded);
    check(decoded.size() == 400 && !recovered.loss_seen,
          "all concurrent observations are recovered exactly once");
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        check(unsigned_number(decoded[i], "observation_ordinal") == i + 1,
              "observation ordinals match serialized journal order");
        check(decoded[i].sequence == i + 2, "journal sequence follows observation order");
    }
    check(fixture.state->sink_thread != std::this_thread::get_id(),
          "actual sink writes run on the recorder worker");

    const auto path = std::filesystem::temp_directory_path() /
        ("yakumo-observer-synthetic-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".journal");
    {
        auto recorder = std::make_shared<SessionRecorder>(make_file_sink(path),
            Fields{{"role", std::string("synthetic_file")}});
        GameObserver observer(recorder);
        observer.frame(12, 1);
        check(recorder->close("test_done"), "synthetic file recorder closes");
    }
    const auto file_events = events(read_journal(path));
    check(file_events.size() == 1 && text(file_events.front(), "event") == "guest.frame",
          "file sink recovers typed observer event from a temporary synthetic file");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
} // namespace

int main() {
    try {
        test_disabled_routing_and_lifetime();
        test_domain_focus_and_time();
        test_modal_domain_restore();
        test_pointer_details();
        test_window_filter_and_privacy();
        test_frame_and_control_reads();
        test_camera_actions_and_nonfinite_values();
        test_overlay_epochs_and_ambiguity();
        test_emission_failures();
        test_concurrent_order_and_file_sink();
    } catch (const std::exception &error) {
        std::cerr << "Unhandled test exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) std::cerr << failures << " game observer checks failed\n";
    return failures ? 1 : 0;
}
