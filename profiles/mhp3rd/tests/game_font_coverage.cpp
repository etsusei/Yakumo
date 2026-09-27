#include "fonts/game_font.hpp"
#include "settings/settings.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Sample {
    std::uint32_t code;
    const char *group;
};

// Distinct code points reported blank by both first-pair game logs.
constexpr std::array<std::uint32_t, 23> kReportedMissing = {
    0x68C0u, 0x67E5u, 0x8BB0u, 0x5FC6u, 0x8BF7u, 0x620Fu, 0x52A8u, 0x8BFBu,
    0x6BD5u, 0x952Eu, 0x9009u, 0x62E9u, 0x65F6u, 0x95F4u, 0x8F6Cu, 0x8FD9u,
    0x5417u, 0x7ECFu, 0x7ED3u, 0x7EDDu, 0x5BF9u, 0x519Cu, 0x573Au,
};

constexpr std::array<std::uint32_t, 8> kOtherChinese = {
    0x4E2Du, 0x6587u, 0x83DCu, 0x5355u, 0x6D4Bu, 0x8BD5u, 0x5F00u, 0x59CBu,
};

constexpr std::array<std::uint32_t, 6> kJapanese = {
    0x3042u, 0x3044u, 0x30A2u, 0x30ABu, 0x88C5u, 0x5099u,
};

constexpr std::array<std::uint32_t, 6> kLatin = {
    0x0041u, 0x0042u, 0x0061u, 0x007Au, 0x0030u, 0x0039u,
};

std::string json_string(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (byte < 0x20u) {
                out << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(byte) << std::dec;
            } else {
                out << static_cast<char>(byte);
            }
        }
    }
    out << '"';
    return out.str();
}

std::string code_name(std::uint32_t code) {
    std::ostringstream out;
    out << "U+" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << code;
    return out.str();
}

template <std::size_t N>
void add_samples(std::vector<Sample> &samples, const std::array<std::uint32_t, N> &codes,
                 const char *group) {
    for (const std::uint32_t code : codes) samples.push_back({code, group});
}

void usage() {
    std::cerr << "Usage: game_font_coverage --report REPORT.json [--font FONT_VALUE]\n"
              << "Set MHP3RD_DATA_DIR to an isolated directory. --font '' forces the default; "
                 "omitting --font uses its settings.\n";
}

} // namespace

int main(int argc, char **argv) {
    std::string report_path;
    std::string font_value;
    bool font_argument = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--report" && i + 1 < argc) {
            report_path = argv[++i];
        } else if (arg == "--font" && i + 1 < argc) {
            font_value = argv[++i];
            font_argument = true;
        } else if (arg == "--help") {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    const char *data_dir = std::getenv("MHP3RD_DATA_DIR");
    if (report_path.empty() || data_dir == nullptr || *data_dir == '\0') {
        usage();
        return 2;
    }

    mhp3rd::settings::Settings &settings = mhp3rd::settings::current();
    if (font_argument) settings.font = font_value;
    const std::string selected_font = settings.font;
    const bool font_ready = mhp3rd::fonts::ready();

    std::vector<Sample> samples;
    samples.reserve(kReportedMissing.size() + kOtherChinese.size() + kJapanese.size() + kLatin.size());
    add_samples(samples, kReportedMissing, "reported_missing");
    add_samples(samples, kOtherChinese, "other_chinese");
    add_samples(samples, kJapanese, "japanese");
    add_samples(samples, kLatin, "latin");

    std::ostringstream report;
    report << "{\n"
           << "  \"schema\": \"mhp3rd.game_font_coverage.v1\",\n"
           << "  \"selection_source\": " << json_string(font_argument ? "argument" : "settings") << ",\n"
           << "  \"selected_font\": " << json_string(selected_font) << ",\n"
           << "  \"active_font\": " << json_string(mhp3rd::fonts::active_name()) << ",\n"
           << "  \"fallback_font\": " << json_string(mhp3rd::fonts::fallback_name()) << ",\n"
           << "  \"font_problem\": " << json_string(mhp3rd::fonts::problem()) << ",\n"
           << "  \"font_weight\": " << settings.font_weight << ",\n"
           << "  \"font_ready\": " << (font_ready ? "true" : "false") << ",\n"
           << "  \"glyphs\": [\n";

    std::size_t passed = 0;
    std::size_t reported_missing_passed = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Sample &sample = samples[i];
        const mhp3rd::fonts::GlyphMetrics metrics = mhp3rd::fonts::metrics(sample.code);
        const mhp3rd::fonts::GlyphBitmap bitmap = mhp3rd::fonts::render(sample.code, 0.0f, 0.0f);
        std::size_t ink_pixels = 0;
        for (const std::uint8_t pixel : bitmap.pixels)
            if (pixel != 0u) ++ink_pixels;

        const int metric_y = mhp3rd::fonts::kBaseline - metrics.top;
        const bool metrics_in_bounds = metrics.found && metrics.width > 0 && metrics.height > 0 &&
                                       metrics.left >= mhp3rd::fonts::kInkLeft &&
                                       metrics.left + metrics.width <= mhp3rd::fonts::kInkRight &&
                                       metric_y >= mhp3rd::fonts::kInkTop &&
                                       metric_y + metrics.height <= mhp3rd::fonts::kInkBottom &&
                                       std::isfinite(metrics.advance) && metrics.advance > 0.0f;
        const bool bitmap_in_bounds = bitmap.width == metrics.width && bitmap.height == metrics.height &&
                                      bitmap.width > 0 && bitmap.height > 0 &&
                                      bitmap.pixels.size() == static_cast<std::size_t>(bitmap.width) * bitmap.height &&
                                      metrics.left + bitmap.x >= mhp3rd::fonts::kInkLeft &&
                                      metrics.left + bitmap.x + bitmap.width <= mhp3rd::fonts::kInkRight &&
                                      metric_y + bitmap.y >= mhp3rd::fonts::kInkTop &&
                                      metric_y + bitmap.y + bitmap.height <= mhp3rd::fonts::kInkBottom;
        const bool rendered = ink_pixels > 0;
        const bool glyph_passed = metrics.found && rendered && metrics_in_bounds && bitmap_in_bounds;
        const std::string advance_json = std::isfinite(metrics.advance) ? std::to_string(metrics.advance) : "null";
        if (glyph_passed) {
            ++passed;
            if (std::string_view(sample.group) == "reported_missing") ++reported_missing_passed;
        }

        report << "    {\"codepoint\": " << json_string(code_name(sample.code))
               << ", \"group\": " << json_string(sample.group)
               << ", \"found\": " << (metrics.found ? "true" : "false")
               << ", \"rendered\": " << (rendered ? "true" : "false")
               << ", \"ink_pixels\": " << ink_pixels
               << ", \"metrics_in_bounds\": " << (metrics_in_bounds ? "true" : "false")
               << ", \"bitmap_in_bounds\": " << (bitmap_in_bounds ? "true" : "false")
               << ", \"passed\": " << (glyph_passed ? "true" : "false")
               << ", \"metrics\": {\"width\": " << metrics.width
               << ", \"height\": " << metrics.height << ", \"left\": " << metrics.left
               << ", \"top\": " << metrics.top << ", \"advance\": " << advance_json
               << "}, \"bitmap\": {\"width\": " << bitmap.width
               << ", \"height\": " << bitmap.height << ", \"x\": " << bitmap.x
               << ", \"y\": " << bitmap.y << "}}";
        report << (i + 1 == samples.size() ? "\n" : ",\n");
    }
    const bool success = font_ready && passed == samples.size();
    report << "  ],\n"
           << "  \"total\": " << samples.size() << ",\n"
           << "  \"passed\": " << passed << ",\n"
           << "  \"reported_missing_total\": " << kReportedMissing.size() << ",\n"
           << "  \"reported_missing_passed\": " << reported_missing_passed << ",\n"
           << "  \"success\": " << (success ? "true" : "false") << "\n"
           << "}\n";

    std::ofstream out(report_path, std::ios::binary | std::ios::trunc);
    if (out) out << report.str();
    out.close();
    if (!out) {
        std::cerr << "Cannot write font coverage report: " << report_path << '\n';
        return 2;
    }
    std::cerr << "Font coverage: " << passed << '/' << samples.size() << " glyphs, "
              << reported_missing_passed << '/' << kReportedMissing.size() << " formerly missing glyphs.\n";
    return success ? 0 : 1;
}
