#include "testing/sdl_observers.hpp"

#include <SDL3/SDL_mouse.h>

#include <atomic>
#include <cstring>

namespace mhp3rd::testing {
namespace {

std::atomic<bool> g_scripted_override_active{false};

// SDL event timestamps are SDL_GetTicksNS values, not wall-clock time.
WindowObservation base(const SDL_Event &event, WindowEventKind kind, bool focused,
                       bool ui_consumed, bool scripted_mode) noexcept {
    WindowObservation result{};
    result.kind = kind;
    result.source_event_type = event.type;
    result.focused = focused;
    result.ui_consumed = ui_consumed;
    result.scripted_mode = scripted_mode;
    result.source_timestamp_ns = event.common.timestamp;
    return result;
}

} // namespace

std::optional<WindowObservation> decode_sdl_event(
    const SDL_Event &event, std::uint32_t owned_window_id, bool focused,
    bool ui_consumed, bool scripted_mode) noexcept {
    if (owned_window_id == 0u) return std::nullopt;
    const auto own = [owned_window_id](SDL_WindowID id) noexcept { return id == owned_window_id; };
    WindowObservation result{};
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (!focused || !own(event.key.windowID)) return std::nullopt;
        result = base(event, WindowEventKind::Key, focused, ui_consumed, scripted_mode);
        result.window_id = event.key.windowID;
        result.owned_window = true;
        result.device_id = event.key.which;
        result.code = event.key.scancode;
        result.down = event.key.down;
        result.repeat = event.key.repeat;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!focused || !own(event.button.windowID) || event.button.which == SDL_TOUCH_MOUSEID)
            return std::nullopt;
        result = base(event, WindowEventKind::MouseButton, focused, ui_consumed, scripted_mode);
        result.window_id = event.button.windowID;
        result.owned_window = true;
        result.device_id = event.button.which;
        result.code = event.button.button;
        result.down = event.button.down;
        result.x = event.button.x;
        result.y = event.button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (!focused || !own(event.motion.windowID) || event.motion.which == SDL_TOUCH_MOUSEID)
            return std::nullopt;
        result = base(event, WindowEventKind::MouseMotion, focused, ui_consumed, scripted_mode);
        result.window_id = event.motion.windowID;
        result.owned_window = true;
        result.device_id = event.motion.which;
        result.x = event.motion.xrel;
        result.y = event.motion.yrel;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!focused || !own(event.wheel.windowID) || event.wheel.which == SDL_TOUCH_MOUSEID)
            return std::nullopt;
        result = base(event, WindowEventKind::MouseWheel, focused, ui_consumed, scripted_mode);
        result.window_id = event.wheel.windowID;
        result.owned_window = true;
        result.device_id = event.wheel.which;
        result.code = event.wheel.direction;
        result.x = event.wheel.x;
        result.y = event.wheel.y;
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
        if (!focused || !own(event.tfinger.windowID)) return std::nullopt;
        result = base(event, WindowEventKind::Touch, focused, ui_consumed, scripted_mode);
        result.window_id = event.tfinger.windowID;
        result.owned_window = true;
        result.device_id = static_cast<std::int64_t>(event.tfinger.touchID);
        result.code = static_cast<std::int64_t>(event.tfinger.fingerID);
        result.down = event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION;
        result.x = event.tfinger.x;
        result.y = event.tfinger.y;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (!focused) return std::nullopt;
        result = base(event, WindowEventKind::GamepadButton, focused, ui_consumed, scripted_mode);
        // The event has no SDL window ID; focus routes it to this game's input.
        result.owned_window = true;
        result.device_id = event.gbutton.which;
        result.code = event.gbutton.button;
        result.down = event.gbutton.down;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (!focused) return std::nullopt;
        result = base(event, WindowEventKind::GamepadAxis, focused, ui_consumed, scripted_mode);
        result.owned_window = true;
        result.device_id = event.gaxis.which;
        result.code = event.gaxis.axis;
        result.x = event.gaxis.value;
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (!focused || !own(event.text.windowID)) return std::nullopt;
        result = base(event, WindowEventKind::Text, focused, ui_consumed, scripted_mode);
        result.window_id = event.text.windowID;
        result.owned_window = true;
        result.text_bytes = event.text.text != nullptr ? std::strlen(event.text.text) : 0u;
        break;
    case SDL_EVENT_TEXT_EDITING:
        if (!focused || !own(event.edit.windowID)) return std::nullopt;
        result = base(event, WindowEventKind::Text, focused, ui_consumed, scripted_mode);
        result.window_id = event.edit.windowID;
        result.owned_window = true;
        result.code = event.type;
        result.text_bytes = event.edit.text != nullptr ? std::strlen(event.edit.text) : 0u;
        break;
    case SDL_EVENT_DROP_FILE:
        if (!focused || !own(event.drop.windowID)) return std::nullopt;
        result = base(event, WindowEventKind::FileDrop, focused, ui_consumed, scripted_mode);
        result.window_id = event.drop.windowID;
        result.owned_window = true;
        // The path and even its length are intentionally omitted.
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (!own(event.window.windowID)) return std::nullopt;
        result = base(event, WindowEventKind::Focus,
                      event.type == SDL_EVENT_WINDOW_FOCUS_GAINED, ui_consumed, scripted_mode);
        result.window_id = event.window.windowID;
        result.owned_window = true;
        result.code = event.type;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        if (!own(event.window.windowID)) return std::nullopt;
        result = base(event, WindowEventKind::Geometry, focused, ui_consumed, scripted_mode);
        result.window_id = event.window.windowID;
        result.owned_window = true;
        result.code = event.type;
        // SDL defines data1/data2 per event: dimensions for resize events,
        // a display ID for DISPLAY_CHANGED, and other event-specific values.
        // Keep them raw and use source_event_type to identify their units.
        result.x = event.window.data1;
        result.y = event.window.data2;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (!own(event.window.windowID)) return std::nullopt;
        result = base(event, WindowEventKind::Close, focused, ui_consumed, scripted_mode);
        result.window_id = event.window.windowID;
        result.owned_window = true;
        result.code = event.type;
        break;
    case SDL_EVENT_QUIT:
        // SDL quit is process-wide, with no window ID.
        result = base(event, WindowEventKind::Close, focused, ui_consumed, scripted_mode);
        result.code = event.type;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        result = base(event, event.type == SDL_EVENT_GAMEPAD_ADDED ? WindowEventKind::DeviceAdded
                                                                   : WindowEventKind::DeviceRemoved,
                      focused, ui_consumed, scripted_mode);
        result.device_id = event.gdevice.which;
        result.code = event.type;
        break;
    case SDL_EVENT_KEYBOARD_ADDED:
    case SDL_EVENT_KEYBOARD_REMOVED:
        result = base(event, event.type == SDL_EVENT_KEYBOARD_ADDED ? WindowEventKind::DeviceAdded
                                                                    : WindowEventKind::DeviceRemoved,
                      focused, ui_consumed, scripted_mode);
        result.device_id = event.kdevice.which;
        result.code = event.type;
        break;
    case SDL_EVENT_MOUSE_ADDED:
    case SDL_EVENT_MOUSE_REMOVED:
        result = base(event, event.type == SDL_EVENT_MOUSE_ADDED ? WindowEventKind::DeviceAdded
                                                                 : WindowEventKind::DeviceRemoved,
                      focused, ui_consumed, scripted_mode);
        result.device_id = event.mdevice.which;
        result.code = event.type;
        break;
    default:
        return std::nullopt;
    }
    return result;
}

void observe_sdl_event(const SDL_Event &event, std::uint32_t owned_window_id,
                       bool focused, bool ui_consumed, bool scripted_mode) noexcept {
    const std::shared_ptr<GameObserver> observer = active_observer();
    if (!observer) return;
    if (const std::optional<WindowObservation> decoded =
            decode_sdl_event(event, owned_window_id, focused, ui_consumed, scripted_mode)) {
        // Prime the sampled focus for controls when SDL had no focus-change
        // event in this pump. A focus event itself carries the source stamp.
        if (decoded->kind != WindowEventKind::Focus) observer->focus(focused);
        observer->window(*decoded);
    }
}

void set_scripted_override_active(bool active) noexcept {
    g_scripted_override_active.store(active, std::memory_order_relaxed);
}

bool scripted_override_active() noexcept {
    return g_scripted_override_active.load(std::memory_order_relaxed);
}

} // namespace mhp3rd::testing
