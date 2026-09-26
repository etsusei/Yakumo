#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mhp3rd::testing {

struct CaseProbe {
    std::uint32_t entry{};
    std::uint64_t min_calls{};
};

struct CaseSpec {
    std::string id;
    std::uint32_t version{};
    std::string title;
    std::vector<std::string> steps;
    std::vector<std::string> checkpoints;
    std::vector<CaseProbe> required_probes;
    std::vector<std::string> required_state_fields;
    bool human_acceptance{};
};

struct CaseCatalog {
    std::vector<CaseSpec> cases;
    // SHA-256 of the validated catalog serialized as sorted, compact UTF-8 JSON.
    std::string sha256;
};

// Both functions reject malformed or out-of-bounds catalogs with std::invalid_argument.
[[nodiscard]] CaseCatalog parse_case_catalog(std::string_view json);
[[nodiscard]] CaseCatalog load_case_catalog(const std::filesystem::path &path);

} // namespace mhp3rd::testing
