#include "testing/sdl_observers.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {
using mhp3rd::testing::WindowEventKind;
using mhp3rd::testing::decode_sdl_event;

constexpr std::uint32_t kOwnWindow = 41u;
constexpr std::uint32_t kOtherWindow = 42u;
int failures{};

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_window_ownership_and_keys() {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.timestamp = 123456789u;
    event.key.windowID = kOwnWindow;
    event.key.which = 7u;
    event.key.scancode = SDL_SCANCODE_A;
    event.key.key = SDLK_A;
    event.key.down = true;
    event.key.repeat = true;
    auto decoded = decode_sdl_event(event, kOwnWindow, true, true, true);
    check(decoded && decoded->kind == WindowEventKind::Key && decoded->owned_window &&
              decoded->window_id == kOwnWindow && decoded->device_id == 7 &&
              decoded->code == SDL_SCANCODE_A && decoded->down && decoded->repeat &&
              decoded->ui_consumed && decoded->scripted_mode &&
              decoded->source_timestamp_ns == 123456789u,
          "own key keeps code, event timestamp, UI consumption, and override state");
    check(!decode_sdl_event(event, kOwnWindow, false, false, false), "unfocused key omitted");
    check(!decode_sdl_event(event, 0u, true, false, false), "unknown owned window omitted");
    event.key.windowID = kOtherWindow;
    check(!decode_sdl_event(event, kOwnWindow, true, false, false), "other window key omitted");
    event.key.windowID = kOwnWindow;
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    event.key.repeat = false;
    decoded = decode_sdl_event(event, kOwnWindow, true, false, false);
    check(decoded && !decoded->down && !decoded->repeat && !decoded->scripted_mode,
          "key release and ordinary mode are distinct");
}

void test_mouse_and_touch() {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = kOwnWindow;
    event.button.which = 19u;
    event.button.button = SDL_BUTTON_RIGHT;
    event.button.down = true;
    event.button.x = 120.5f;
    event.button.y = 67.25f;
    auto decoded = decode_sdl_event(event, kOwnWindow, true, false, false);
    check(decoded && decoded->kind == WindowEventKind::MouseButton && decoded->device_id == 19 &&
              decoded->code == SDL_BUTTON_RIGHT && decoded->down &&
              decoded->x == 120.5 && decoded->y == 67.25,
          "mouse button code and position normalized");
    event.button.windowID = kOtherWindow;
    check(!decode_sdl_event(event, kOwnWindow, true, false, false), "other window mouse omitted");
    event.button.windowID = kOwnWindow;
    event.button.which = SDL_TOUCH_MOUSEID;
    check(!decode_sdl_event(event, kOwnWindow, true, false, false), "touch-synthesized mouse omitted");

    event = {};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = kOwnWindow;
    event.motion.which = 23u;
    event.motion.xrel = -4.5f;
    event.motion.yrel = 2.25f;
    decoded = decode_sdl_event(event, kOwnWindow, true, false, false);
    check(decoded && decoded->kind == WindowEventKind::MouseMotion && decoded->device_id == 23 &&
              decoded->x == -4.5 && decoded->y == 2.25,
          "relative mouse motion normalized");

    event = {};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.windowID = kOwnWindow;
    event.wheel.x = -1.0f;
    event.wheel.y = 3.0f;
    decoded = decode_sdl_event(event, kOwnWindow, true, false, false);
    check(decoded && decoded->kind == WindowEventKind::MouseWheel &&
              decoded->x == -1.0 && decoded->y == 3.0,
          "wheel deltas normalized");

    event = {};
    event.type = SDL_EVENT_FINGER_MOTION;
    event.tfinger.windowID = kOwnWindow;
    event.tfinger.touchID = 5u;
    event.tfinger.fingerID = 6u;
    event.tfinger.x = 0.5f;
    event.tfinger.y = 0.25f;
    decoded = decode_sdl_event(event, kOwnWindow, true, false, false);
    check(decoded && decoded->kind == WindowEventKind::Touch && decoded->device_id == 5 &&
              decoded->code == 6 && decoded->down && decoded->x == 0.5 && decoded->y == 0.25,
          "touch ID, finger ID, and normalized coordinates retained");
    check(!decode_sdl_event(event, kOwnWindow, false, false, false), "unfocused touch omitted");
}

void test_gamepad_and_devices() {
    SDL_Event event{};
    event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    event.gbutton.which = 101u;
    event.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    event.gbutton.down = true;
    auto decoded = decode_sdl_event(event, kOwnWindow, true, true, false);
    check(decoded && decoded->kind == WindowEventKind::GamepadButton && decoded->owned_window &&
              decoded->window_id == 0u && decoded->device_id == 101 &&
              decoded->code == SDL_GAMEPAD_BUTTON_SOUTH && decoded->down && decoded->ui_consumed,
          "focused gamepad button attributed without inventing an SDL window ID");
    check(!decode_sdl_event(event, kOwnWindow, false, false, false), "unfocused gamepad control omitted");

    event = {};
    event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    event.gaxis.which = 101u;
    event.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    event.gaxis.value = -16384;
    decoded = decode_sdl_event(event, kOwnWindow, true, false, true);
    check(decoded && decoded->kind == WindowEventKind::GamepadAxis && decoded->owned_window &&
              decoded->code == SDL_GAMEPAD_AXIS_LEFTX && decoded->x == -16384 && decoded->scripted_mode,
          "axis code and raw signed value normalized");

    event = {};
    event.type = SDL_EVENT_GAMEPAD_ADDED;
    event.gdevice.which = 404u;
    decoded = decode_sdl_event(event, kOwnWindow, false, false, false);
    check(decoded && decoded->kind == WindowEventKind::DeviceAdded && !decoded->owned_window &&
              decoded->window_id == 0u && decoded->device_id == 404,
          "global device add retained without false ownership");
    event.type = SDL_EVENT_GAMEPAD_REMOVED;
    decoded = decode_sdl_event(event, kOwnWindow, false, false, false);
    check(decoded && decoded->kind == WindowEventKind::DeviceRemoved && !decoded->owned_window,
          "global device removal retained");
}

void test_redaction_and_lifecycle() {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = kOwnWindow;
    event.text.text = "private text";
    auto decoded = decode_sdl_event(event, kOwnWindow, true, true, false);
    check(decoded && decoded->kind == WindowEventKind::Text && decoded->text_bytes == 12u &&
              decoded->ui_consumed && decoded->code == 0,
          "text input retains byte count without content");
    event.text.windowID = kOtherWindow;
    check(!decode_sdl_event(event, kOwnWindow, true, false, false), "other window text omitted");

    event = {};
    event.type = SDL_EVENT_DROP_FILE;
    event.drop.windowID = kOwnWindow;
    event.drop.data = "/private/path/to/save.dat";
    decoded = decode_sdl_event(event, kOwnWindow, true, false, false);
    check(decoded && decoded->kind == WindowEventKind::FileDrop && decoded->text_bytes == 0u,
          "drop path and its length omitted");

    event = {};
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    event.window.windowID = kOwnWindow;
    decoded = decode_sdl_event(event, kOwnWindow, false, false, false);
    check(decoded && decoded->kind == WindowEventKind::Focus && !decoded->focused &&
              decoded->owned_window && decoded->code == SDL_EVENT_WINDOW_FOCUS_LOST,
          "own focus loss retained when already unfocused");
    event.window.windowID = kOtherWindow;
    check(!decode_sdl_event(event, kOwnWindow, false, false, false), "other window focus omitted");

    event = {};
    event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    event.window.windowID = kOwnWindow;
    decoded = decode_sdl_event(event, kOwnWindow, false, false, false);
    check(decoded && decoded->kind == WindowEventKind::Close && decoded->owned_window &&
              decoded->window_id == kOwnWindow,
          "own close retained when unfocused");
    event.window.windowID = kOtherWindow;
    check(!decode_sdl_event(event, kOwnWindow, false, false, false), "other window close omitted");

    event = {};
    event.type = SDL_EVENT_QUIT;
    decoded = decode_sdl_event(event, kOwnWindow, false, false, false);
    check(decoded && decoded->kind == WindowEventKind::Close && !decoded->owned_window &&
              decoded->window_id == 0u && decoded->code == SDL_EVENT_QUIT,
          "process quit distinguished from window close");
    event = {};
    event.type = SDL_EVENT_DISPLAY_ORIENTATION;
    check(!decode_sdl_event(event, kOwnWindow, true, false, false), "unsupported event omitted");
}

struct MemorySink final : mhp3rd::testing::JournalSink {
    explicit MemorySink(std::shared_ptr<std::vector<std::uint8_t>> bytes) : bytes(std::move(bytes)) {}
    bool write(std::span<const std::uint8_t> data) override {
        bytes->insert(bytes->end(), data.begin(), data.end());
        return true;
    }
    bool flush() override { return true; }
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
};

void test_forwarding_and_journal_redaction() {
    using namespace mhp3rd::testing;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
    auto recorder = std::make_shared<SessionRecorder>(std::make_unique<MemorySink>(bytes), Fields{});
    auto observer = std::make_shared<GameObserver>(recorder);
    set_active_observer(observer);

    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = kOwnWindow;
    event.key.scancode = SDL_SCANCODE_Z;
    event.key.down = true;
    event.key.timestamp = 9876u;
    observe_sdl_event(event, kOwnWindow, true, true, true);
    event.key.windowID = kOtherWindow;
    observe_sdl_event(event, kOwnWindow, true, false, false);

    event = {};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = kOwnWindow;
    event.text.text = "secret input";
    observe_sdl_event(event, kOwnWindow, true, true, false);
    event = {};
    event.type = SDL_EVENT_DROP_FILE;
    event.drop.windowID = kOwnWindow;
    event.drop.data = "/secret/drop/path";
    observe_sdl_event(event, kOwnWindow, true, false, false);

    set_active_observer(nullptr);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = kOwnWindow;
    observe_sdl_event(event, kOwnWindow, true, false, false);
    check(recorder->close("synthetic"), "synthetic journal closes");
    const JournalRecovery recovered = recover_journal(*bytes);
    check(recovered.complete(), "forwarded synthetic observations form a valid journal");
    std::string payloads;
    for (const JournalRecord &record : recovered.records) payloads += record.payload;
    check(payloads.find("\"event\":\"window.key\"") != std::string::npos &&
              payloads.find("\"event\":\"input.focus\"") != std::string::npos &&
              payloads.find("\"source_timestamp_ns\":9876") != std::string::npos,
          "forwarder records focused key and SDL source timestamp");
    check(payloads.find("\"event\":\"window.text\"") != std::string::npos &&
              payloads.find("\"text_bytes\":12") != std::string::npos &&
              payloads.find("\"event\":\"window.file_drop\"") != std::string::npos,
          "text and file drop metadata reaches journal");
    check(payloads.find("secret input") == std::string::npos &&
              payloads.find("/secret/drop/path") == std::string::npos,
          "text and dropped path are absent from journal");
    check(observer->emission_errors() == 0u, "forwarding has no observer emission errors");
}

} // namespace

int main() {
    test_window_ownership_and_keys();
    test_mouse_and_touch();
    test_gamepad_and_devices();
    test_redaction_and_lifecycle();
    test_forwarding_and_journal_redaction();
    mhp3rd::testing::set_scripted_override_active(true);
    check(mhp3rd::testing::scripted_override_active(), "script override state can be enabled");
    mhp3rd::testing::set_scripted_override_active(false);
    check(!mhp3rd::testing::scripted_override_active(), "script override state can be disabled");
    return failures == 0 ? 0 : 1;
}
