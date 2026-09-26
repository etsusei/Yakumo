#include "ui/localization.hpp"

#include <string_view>
#include <unordered_map>

namespace mhp3rd::ui {

const char *translate(const char *english, settings::UiLanguage language) {
    if (english == nullptr || language == settings::UiLanguage::English) return english;
    static const std::unordered_map<std::string_view, const char *> chinese{
#include "ui/translations/zh_cn.inc"
    };
    const auto found = chinese.find(english);
    return found == chinese.end() ? english : found->second;
}

std::string tr_format(const char *english, std::initializer_list<std::string_view> arguments) {
    const std::string_view pattern = tr(english);
    std::string result;
    for (std::size_t i = 0; i < pattern.size();) {
        if (i + 2 < pattern.size() && pattern[i] == '{' && pattern[i + 2] == '}' &&
            pattern[i + 1] >= '0' && pattern[i + 1] <= '9') {
            const auto index = static_cast<std::size_t>(pattern[i + 1] - '0');
            if (index < arguments.size()) {
                result += arguments.begin()[index];
                i += 3;
                continue;
            }
        }
        result += pattern[i++];
    }
    return result;
}

} // namespace mhp3rd::ui
