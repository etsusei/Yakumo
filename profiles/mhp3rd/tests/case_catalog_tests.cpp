#include "testing/case_catalog.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace {

using namespace mhp3rd::testing;
namespace fs = std::filesystem;

int failures = 0;

constexpr std::string_view kGoldenCatalog = R"json({"cases":[{"version":4294967295,"id":"Native-01","title":"Wave \u2603 \ud83d\ude00 \"quote\" \\ backslash","steps":["First \u00e9 step"],"checkpoints":["Start"],"required_probes":[{"min_calls":18446744073709551615,"entry":4294967295}],"required_state_fields":["health_current"],"human_acceptance":true}],"schema":"yakumo-case-catalog-v1"})json";
constexpr std::string_view kGoldenSha256 =
    "5ca1ba08729cdf7ccdd94082b4f41a979a8c6e00b743d42fbc74d68a021be651";

void check(bool condition, std::string_view description) {
    if (!condition && ++failures <= 50)
        std::cerr << "FAIL: " << description << '\n';
}

template <class Function>
void rejects(Function &&function, std::string_view description) {
    try {
        function();
        check(false, description);
    } catch (const std::invalid_argument &) {
    } catch (const std::exception &error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        check(false, description);
    } catch (...) {
        check(false, description);
    }
}

std::string replace_first(std::string source, std::string_view before, std::string_view after) {
    const std::size_t position = source.find(before);
    if (position == std::string::npos)
        throw std::logic_error("test fixture substitution failed");
    source.replace(position, before.size(), after);
    return source;
}

void reject_replaced(std::string_view before, std::string_view after, std::string_view description) {
    const std::string variant = replace_first(std::string(kGoldenCatalog), before, after);
    rejects([&] { (void)parse_case_catalog(variant); }, description);
}

class TempRoot {
public:
    TempRoot() {
        static std::atomic<std::uint64_t> counter{};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
            ("yakumo-case-catalog-test-" + std::to_string(tick) + "-" +
             std::to_string(counter.fetch_add(1)));
        if (!fs::create_directory(path))
            throw std::runtime_error("temporary test directory already exists");
    }
    ~TempRoot() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

void write_file(const fs::path &path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot create test fixture");
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream)
        throw std::runtime_error("cannot write test fixture");
}

void test_golden_digest_and_unicode() {
    const CaseCatalog catalog = parse_case_catalog(kGoldenCatalog);
    check(catalog.sha256 == kGoldenSha256,
          "catalog digest matches the independent Python canonical JSON fixture");
    check(catalog.cases.size() == 1, "case is loaded");
    const CaseSpec &spec = catalog.cases.at(0);
    check(spec.id == "Native-01" &&
              spec.version == std::numeric_limits<std::uint32_t>::max(),
          "case identity and maximum version are preserved");
    check(spec.required_probes.size() == 1 &&
              spec.required_probes[0].entry == std::numeric_limits<std::uint32_t>::max() &&
              spec.required_probes[0].min_calls == std::numeric_limits<std::uint64_t>::max(),
          "uint32 and uint64 boundaries are preserved");
    check(spec.steps.size() == 1 && spec.checkpoints == std::vector<std::string>{"Start"} &&
              spec.required_state_fields == std::vector<std::string>{"health_current"} &&
              spec.human_acceptance,
          "explicit case requirements are preserved");

    std::string raw_unicode = replace_first(std::string(kGoldenCatalog), "\\u2603", "\xe2\x98\x83");
    raw_unicode = replace_first(raw_unicode, "\\ud83d\\ude00", "\xf0\x9f\x98\x80");
    raw_unicode = replace_first(raw_unicode, "\\u00e9", "\xc3\xa9");
    raw_unicode = replace_first(raw_unicode, "\"id\"", "\"\\u0069d\"");
    check(parse_case_catalog(raw_unicode).sha256 == catalog.sha256,
          "raw UTF-8, Unicode escapes, surrogate pairs and escaped keys normalize identically");
    const CaseCatalog empty = parse_case_catalog(
        R"json({"schema":"yakumo-case-catalog-v1","cases":[]})json");
    check(empty.cases.empty() &&
              empty.sha256 == "a5179411d47ce955061bb0ee9b2af622727fef5e6619d48e08c61b711fa5f18b",
          "empty catalog has the Python canonical digest");
    const CaseCatalog zero_entry = parse_case_catalog(replace_first(
        std::string(kGoldenCatalog), "\"entry\":4294967295", "\"entry\":0"));
    check(zero_entry.cases[0].required_probes[0].entry == 0,
          "zero is a valid uint32 probe entry");
}

void test_object_schema_and_uniqueness() {
    reject_replaced("\"schema\":", "\"extra\":0,\"schema\":", "unknown root key");
    reject_replaced("\"schema\":", "\"schema\":\"yakumo-case-catalog-v1\",\"schema\":",
                    "duplicate root key");
    reject_replaced("\"schema\":\"yakumo-case-catalog-v1\"", "\"schema\":\"other\"",
                    "unsupported schema");
    reject_replaced("\"human_acceptance\":true", "\"extra\":0,\"human_acceptance\":true",
                    "unknown case key");
    reject_replaced("\"human_acceptance\":true", "\"human_acceptance\":true,\"\\u0069d\":\"Other\"",
                    "escaped duplicate case key");
    reject_replaced("\"steps\":[\"First \\u00e9 step\"],", "", "missing case key");
    reject_replaced("\"min_calls\":18446744073709551615", "\"minimum\":1",
                    "unknown probe key");
    reject_replaced("\"min_calls\":18446744073709551615", "\"entry\":1",
                    "duplicate probe key");
    reject_replaced("\"id\":\"Native-01\"", "\"id\":\"1Native\"", "unsafe ID");
    reject_replaced("\"checkpoints\":[\"Start\"]", "\"checkpoints\":[\"Start\",\"Start\"]",
                    "duplicate checkpoint ID");
    reject_replaced("\"required_state_fields\":[\"health_current\"]",
                    "\"required_state_fields\":[\"health_current\",\"health_current\"]",
                    "duplicate state field");
    reject_replaced("\"required_probes\":[{\"min_calls\":18446744073709551615,\"entry\":4294967295}]",
                    "\"required_probes\":[{\"min_calls\":1,\"entry\":0},{\"min_calls\":1,\"entry\":0}]",
                    "duplicate probe entry");
    std::string duplicate_case(kGoldenCatalog);
    constexpr std::string_view cases_key = "\"cases\":[";
    const std::size_t item_begin = duplicate_case.find(cases_key) + cases_key.size();
    const std::size_t item_end = duplicate_case.find("],\"schema\"");
    duplicate_case.insert(item_end, "," + duplicate_case.substr(item_begin, item_end - item_begin));
    rejects([&] { (void)parse_case_catalog(duplicate_case); }, "duplicate case ID");
}

void test_types_and_ranges() {
    for (const std::string_view bad : {"0", "-1", "1.0", "true", "\"1\"", "4294967296",
                                       "18446744073709551616", "01", "1e0", "NaN"}) {
        reject_replaced("\"version\":4294967295", "\"version\":" + std::string(bad),
                        "invalid version scalar");
    }
    for (const std::string_view bad : {"0", "-1", "1.0", "true", "\"1\"",
                                       "18446744073709551616"}) {
        reject_replaced("\"min_calls\":18446744073709551615",
                        "\"min_calls\":" + std::string(bad), "invalid min_calls scalar");
    }
    for (const std::string_view bad : {"-1", "1.0", "true", "4294967296"}) {
        reject_replaced("\"entry\":4294967295", "\"entry\":" + std::string(bad),
                        "invalid entry scalar");
    }
    reject_replaced("\"human_acceptance\":true", "\"human_acceptance\":1",
                    "integer is not a Boolean");
    reject_replaced("\"steps\":[\"First \\u00e9 step\"]", "\"steps\":{}",
                    "steps must be an array");
    reject_replaced("\"title\":\"Wave \\u2603 \\ud83d\\ude00 \\\"quote\\\" \\\\ backslash\"",
                    "\"title\":\"\"", "title must be nonempty");
}

void test_text_unicode_and_bounds() {
    reject_replaced("\\u2603", "\\ud800", "unpaired high surrogate");
    reject_replaced("\\u2603", "\\udc00", "unpaired low surrogate");
    reject_replaced("\\u2603", "\\u0000", "Unicode C0 control");
    reject_replaced("\\u2603", "\\u0085", "Unicode C1 control");
    reject_replaced("\\u2603", "\xc0\xaf", "overlong UTF-8");
    reject_replaced("\\u2603", "\xed\xa0\x80", "raw surrogate UTF-8");
    reject_replaced("\\u2603", "\xff", "invalid UTF-8 byte");
    reject_replaced("\"steps\":[\"First \\u00e9 step\"]",
                    "\"steps\":[\"" + std::string(513, 'a') + "\"]",
                    "step exceeds 512 UTF-8 bytes");
    reject_replaced("\"required_state_fields\":[\"health_current\"]",
                    "\"required_state_fields\":[\"" + std::string(129, 'a') + "\"]",
                    "state field exceeds 128 UTF-8 bytes");
    reject_replaced("\"title\":\"Wave \\u2603 \\ud83d\\ude00 \\\"quote\\\" \\\\ backslash\"",
                    "\"title\":\"" + std::string(257, 'a') + "\"",
                    "title exceeds 256 UTF-8 bytes");
    std::string many_steps = "\"steps\":[";
    for (int index = 0; index < 129; ++index) {
        if (index) many_steps += ',';
        many_steps += "\"step\"";
    }
    many_steps += ']';
    reject_replaced("\"steps\":[\"First \\u00e9 step\"]", many_steps,
                    "steps exceed 128 entries");
    std::string many_checkpoints = "\"checkpoints\":[";
    for (int index = 0; index < 129; ++index) {
        if (index) many_checkpoints += ',';
        many_checkpoints += "\"P" + std::to_string(index) + "\"";
    }
    many_checkpoints += ']';
    reject_replaced("\"checkpoints\":[\"Start\"]", many_checkpoints,
                    "checkpoints exceed 128 entries");
    std::string many_states = "\"required_state_fields\":[";
    for (int index = 0; index < 33; ++index) {
        if (index) many_states += ',';
        many_states += "\"state" + std::to_string(index) + "\"";
    }
    many_states += ']';
    reject_replaced("\"required_state_fields\":[\"health_current\"]", many_states,
                    "state fields exceed 32 entries");
    std::string many_probes = "\"required_probes\":[";
    for (int index = 0; index < 33; ++index) {
        if (index) many_probes += ',';
        many_probes += "{\"entry\":" + std::to_string(index) + ",\"min_calls\":1}";
    }
    many_probes += ']';
    reject_replaced("\"required_probes\":[{\"min_calls\":18446744073709551615,\"entry\":4294967295}]",
                    many_probes, "probes exceed 32 entries");

    std::string many_cases = "{\"schema\":\"yakumo-case-catalog-v1\",\"cases\":[";
    for (int index = 0; index < 129; ++index) {
        if (index) many_cases += ',';
        many_cases += "{\"id\":\"Case" + std::to_string(index) +
                      "\",\"version\":1,\"title\":\"Case\",\"steps\":[],"
                      "\"checkpoints\":[],\"required_probes\":[],"
                      "\"required_state_fields\":[],\"human_acceptance\":false}";
    }
    many_cases += "]}";
    const std::size_t final_case = many_cases.rfind(",{\"id\":\"Case128\"");
    check(final_case != std::string::npos, "case limit fixture is complete");
    if (final_case != std::string::npos) {
        std::string permitted_cases = many_cases;
        permitted_cases.erase(final_case, many_cases.size() - final_case - 2);
        check(parse_case_catalog(permitted_cases).cases.size() == 128,
              "128 cases are permitted");
    }
    rejects([&] { (void)parse_case_catalog(many_cases); }, "129 cases are rejected");

    std::string exact_limit = "{\"schema\":\"yakumo-case-catalog-v1\",\"cases\":[]}";
    exact_limit.append(512 * 1024 - exact_limit.size(), ' ');
    check(parse_case_catalog(exact_limit).cases.empty(), "512 KiB in-memory catalog is permitted");
    rejects([&] { (void)parse_case_catalog(std::string(512 * 1024 + 1, ' ')); },
            "in-memory catalog exceeds 512 KiB");
    rejects([&] { (void)parse_case_catalog("{\"schema\":\"yakumo-case-catalog-v1\",\"cases\":[[[[[[]]]]]]}"); },
            "deeply nested value is rejected without recursive descent");
}

void test_file_loading() {
    TempRoot root;
    const fs::path valid = root.path / "cases.json";
    write_file(valid, kGoldenCatalog);
    check(load_case_catalog(valid).sha256 == kGoldenSha256, "regular catalog file loads");
    std::string exact_limit = "{\"schema\":\"yakumo-case-catalog-v1\",\"cases\":[]}";
    exact_limit.append(512 * 1024 - exact_limit.size(), ' ');
    const fs::path largest_valid = root.path / "largest-valid.json";
    write_file(largest_valid, exact_limit);
    check(load_case_catalog(largest_valid).cases.empty(), "512 KiB regular file is permitted");
    rejects([&] { (void)load_case_catalog(root.path); }, "directory is rejected");
    rejects([&] { (void)load_case_catalog(root.path / "missing.json"); }, "missing file is rejected");

    const fs::path oversized = root.path / "oversized.json";
    write_file(oversized, std::string(512 * 1024 + 1, ' '));
    rejects([&] { (void)load_case_catalog(oversized); }, "oversized file is rejected");
#if defined(__unix__) || defined(__APPLE__)
    const fs::path link = root.path / "link.json";
    fs::create_symlink(valid, link);
    rejects([&] { (void)load_case_catalog(link); }, "symbolic link is rejected");
    const fs::path fifo = root.path / "pipe.json";
    if (::mkfifo(fifo.c_str(), 0600) != 0)
        throw std::runtime_error("cannot create FIFO test fixture");
    rejects([&] { (void)load_case_catalog(fifo); }, "FIFO is rejected without blocking");
#endif
}

} // namespace

int main() {
    try {
        test_golden_digest_and_unicode();
        test_object_schema_and_uniqueness();
        test_types_and_ranges();
        test_text_unicode_and_bounds();
        test_file_loading();
    } catch (const std::exception &error) {
        std::cerr << "Unexpected test failure: " << error.what() << '\n';
        ++failures;
    }
    if (failures)
        std::cerr << failures << " case catalog test(s) failed\n";
    return failures ? 1 : 0;
}
