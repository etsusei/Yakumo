#pragma once

#include "testing/game_observers.hpp"

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <optional>

namespace mhp3rd::testing {

// Decode an already-received SDL event without querying SDL or opening a
// window. Events with another window ID, and unfocused controls, are omitted.
// SDL gamepad events have no window ID; focus is their routing boundary.
[[nodiscard]] std::optional<WindowObservation> decode_sdl_event(
    const SDL_Event &event, std::uint32_t owned_window_id, bool focused,
    bool ui_consumed, bool scripted_mode) noexcept;

// The normal disabled path returns before decoding or allocating anything.
void observe_sdl_event(const SDL_Event &event, std::uint32_t owned_window_id,
                       bool focused, bool ui_consumed, bool scripted_mode) noexcept;

// True while an input-script override is attached. It does not claim that an
// individual SDL event was injected by the script.
void set_scripted_override_active(bool active) noexcept;
[[nodiscard]] bool scripted_override_active() noexcept;

} // namespace mhp3rd::testing
