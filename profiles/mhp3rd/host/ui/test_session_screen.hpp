#pragma once

namespace mhp3rd::ui {

// The test tab is present only while a launcher-owned case session exists.
[[nodiscard]] bool test_screen_available() noexcept;

// Draw inside the menu's current begin_content() child. A true result asks
// the menu to resume the game after a successful begin or marker action.
[[nodiscard]] bool draw_test_session_screen();

// A compact, noninteractive guide while an active case is being played.
// Call from an ImGui frame over the game, outside the open menu.
void draw_test_session_hint();

} // namespace mhp3rd::ui
