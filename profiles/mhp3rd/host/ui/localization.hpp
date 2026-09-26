#pragma once

#include "settings/settings.hpp"

#include <initializer_list>
#include <string_view>

namespace mhp3rd::ui {

// Translate interface literals, never paths, identifiers, or player content.
// Catalog strings live for the process lifetime; unknown text keeps the
// caller's pointer. Format strings retain the original argument types.
[[nodiscard]] const char *translate(const char *english, settings::UiLanguage language);
[[nodiscard]] inline const char *tr(const char *english) {
    return translate(english, settings::current().ui_language);
}
// Numbered placeholders allow whole sentences to change word order. Values
// are inserted verbatim and never interpreted as templates or translations.
[[nodiscard]] std::string tr_format(const char *english, std::initializer_list<std::string_view> arguments);

} // namespace mhp3rd::ui
