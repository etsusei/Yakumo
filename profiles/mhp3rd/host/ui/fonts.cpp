#include "ui/fonts.hpp"
#include "app_paths.hpp"
#include "install/user_data.hpp"
#include "imgui.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace mhp3rd::ui {
namespace {

// Latin text, then Simplified Chinese and Japanese fallback faces. Load
// both scripts up front so changing the interface language needs no restart.
const char *const kTextFonts[] = {
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
};
const char *const kChineseFonts[] = {
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
    "/run/host/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/run/host/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
};
const char *const kJapaneseFonts[] = {
    "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
    "/run/host/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/run/host/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/run/host/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
    "C:/Windows/Fonts/msgothic.ttc",
};

bool exists(const char *path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(install::path_from_utf8(path), ec);
}

} // namespace

void load_interface_fonts() {
    ImGuiIO &io = ImGui::GetIO();
    const char *text_font = std::getenv("MHP3RD_UI_FONT");
    if (text_font != nullptr && !exists(text_font)) {
        std::cout << "[ui] MHP3RD_UI_FONT " << text_font << " not found\n";
        text_font = nullptr;
    }
    for (const char *candidate : kTextFonts) {
        if (text_font != nullptr) break;
        if (exists(candidate)) text_font = candidate;
    }
    ImFont *font = text_font != nullptr ? io.Fonts->AddFontFromFileTTF(text_font) : nullptr;
#if defined(__ANDROID__)
    // Android's own faces are variable fonts; the Japanese font the app
    // carries has Latin too, and the symbols the menu uses (… ○ ×).
    if (font == nullptr)
        for (const std::filesystem::path &bundled : bundled_fonts()) {
            font = io.Fonts->AddFontFromFileTTF(install::path_to_utf8(bundled).c_str());
            if (font != nullptr) {
                std::cout << "[ui] text in " << install::path_to_utf8(bundled.filename()) << "\n";
                break;
            }
        }
#endif
    if (font == nullptr) {
        io.Fonts->AddFontDefaultVector();
        std::cout << "[ui] no system font found; using Dear ImGui's own\n";
    }
    for (const char *candidate : kChineseFonts) {
        if (!exists(candidate)) continue;
        ImFontConfig merge;
        merge.MergeMode = true;
        if (io.Fonts->AddFontFromFileTTF(candidate, 0.0f, &merge) != nullptr) break;
    }
    std::vector<std::string> japanese(std::begin(kJapaneseFonts), std::end(kJapaneseFonts));
    for (const std::filesystem::path &bundled : bundled_fonts()) japanese.push_back(install::path_to_utf8(bundled));
    for (const std::string &candidate : japanese) {
        if (!exists(candidate.c_str())) continue;
        ImFontConfig merge;
        merge.MergeMode = true;
        if (io.Fonts->AddFontFromFileTTF(candidate.c_str(), 0.0f, &merge) != nullptr) break;
    }
}

} // namespace mhp3rd::ui
