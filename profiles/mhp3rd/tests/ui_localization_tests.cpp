#include "ui/localization.hpp"
#include "install/user_data.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures{};
void check(bool condition, const char *message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void environment(const char *name, const std::string &value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}
struct Entry { const char *english; const char *chinese; };
const Entry catalog[] = {
#include "ui/translations/zh_cn.inc"
};
std::vector<std::string> matches(const std::string &text, const std::regex &pattern) {
    std::vector<std::string> result;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
        result.push_back(it->str());
    return result;
}
}

int main(int argc, char **argv) {
    using namespace mhp3rd;
    using settings::UiLanguage;
    const std::string mode = argc > 1 ? argv[1] : "default";
    const auto directory = std::filesystem::temp_directory_path() /
        ("yakumo-ui-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    environment("MHP3RD_DATA_DIR", install::path_to_utf8(directory));
    environment("MHP3RD_UI_LANGUAGE", "");
    if (mode != "default")
        install::write_settings_file(directory, {{"ui.language", mode == "invalid" ? "unknown" : "en"}});
    if (mode == "override") environment("MHP3RD_UI_LANGUAGE", "zh-CN");
    const UiLanguage expected = mode == "saved" ? UiLanguage::English : UiLanguage::SimplifiedChinese;
    check(settings::current().ui_language == expected, "load default, saved, invalid or overridden language");
    settings::save();
    const auto stored = install::read_settings_file(directory);
    check(stored.at("ui.language") == (mode == "saved" || mode == "override" ? "en" : "zh-CN"),
          "save the chosen language without persisting an environment override");

    // Changing language in the same process must not leave cached English or Chinese strings.
    settings::current().ui_language = UiLanguage::English;
    check(std::string_view(ui::tr("Video")) == "Video", "English is available immediately");
    settings::current().ui_language = UiLanguage::SimplifiedChinese;
    check(std::string_view(ui::tr("Video")) != "Video", "Chinese is available immediately");
    const char unknown[] = "Unknown title / file {0}";
    check(ui::tr(unknown) == unknown, "unrecognized content is preserved verbatim");
    check(ui::tr_format("{0} other files are hidden; only {1} are listed.", {"7", "file {0}"}).find("file {0}") != std::string::npos,
          "inserted paths and labels cannot become format placeholders");
    check(ui::tr_format("{9}", {"one"}) == "{9}", "missing placeholder arguments remain visible");

    std::set<std::string_view> keys;
    const std::regex printf_argument(R"(%(?:%|[-+ #0]*[0-9]*(?:\.[0-9]+)?(?:ll|l|z)?[a-zA-Z]))");
    const std::regex placeholder(R"(\{[0-9]\})");
    for (const auto &[en, zh] : catalog) {
        check(keys.insert(en).second, "catalog keys must be unique");
        check(*zh != '\0', "translations must not be empty");
        check(matches(en, printf_argument) == matches(zh, printf_argument), "printf argument types and order match");
        const auto en_tokens = matches(en, placeholder), zh_tokens = matches(zh, placeholder);
        check(std::multiset<std::string>(en_tokens.begin(), en_tokens.end()) ==
              std::multiset<std::string>(zh_tokens.begin(), zh_tokens.end()), "numbered placeholders match");
        const std::string_view key = en, value = zh;
        const auto hidden = key.find("##");
        if (hidden != std::string_view::npos)
            check(value.find(key.substr(hidden)) != std::string_view::npos, "hidden widget identifiers match");
        check(std::string_view(ui::translate(en, UiLanguage::English)) == en, "English preserves the source");
        check(std::string_view(ui::translate(en, UiLanguage::SimplifiedChinese)) == zh, "every entry resolves");
    }
    std::filesystem::remove_all(directory);
    std::cout << "Checked " << keys.size() << " translations (" << mode << "), failures: " << failures << '\n';
    return failures ? 1 : 0;
}
