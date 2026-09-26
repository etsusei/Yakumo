#pragma once

namespace mhp3rd::ui {
// Load interface fonts, including CJK fallbacks for both supported languages.
// Call after creating the ImGui context and before the first frame.
void load_interface_fonts();
} // namespace mhp3rd::ui
