#include "testing/case_catalog.hpp"

#include "psprecomp/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mhp3rd::testing {
namespace {

constexpr std::size_t kMaxCatalogBytes = 512 * 1024;
constexpr std::string_view kSchema = "yakumo-case-catalog-v1";

[[noreturn]] void reject(std::string_view reason) {
    throw std::invalid_argument("invalid case catalog: " + std::string(reason));
}

void append_utf8(std::string &output, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

bool ascii_letter(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

void validate_id(std::string_view value) {
    if (value.empty() || value.size() > 96 || !ascii_letter(value.front()))
        reject("invalid ASCII case or checkpoint ID");
    for (const char character : value) {
        if (!ascii_letter(character) && !(character >= '0' && character <= '9') &&
            character != '_' && character != '.' && character != '-')
            reject("invalid ASCII case or checkpoint ID");
    }
}

void validate_text(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum)
        reject("text is empty or exceeds its UTF-8 byte limit");
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x20 || byte == 0x7f ||
            (byte == 0xc2 && index + 1 < value.size() &&
             static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
             static_cast<unsigned char>(value[index + 1]) <= 0x9f))
            reject("text contains a Unicode control character");
    }
}

class JsonCursor {
public:
    explicit JsonCursor(std::string_view input) : input_(input) {
        if (input.size() > kMaxCatalogBytes)
            reject("catalog exceeds 512 KiB");
    }

    void whitespace() {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\t' && character != '\n' && character != '\r')
                break;
            ++position_;
        }
    }

    bool consume(char character) {
        whitespace();
        if (position_ < input_.size() && input_[position_] == character) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char character) {
        if (!consume(character))
            reject("malformed JSON punctuation");
    }

    void finish() {
        whitespace();
        if (position_ != input_.size())
            reject("trailing JSON content");
    }

    std::string string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const auto byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"')
                return result;
            if (byte < 0x20)
                reject("unescaped JSON control character");
            if (byte == '\\') {
                if (position_ == input_.size())
                    reject("incomplete JSON string escape");
                switch (input_[position_++]) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = hex_quad();
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                            input_[position_ + 1] != 'u')
                            reject("unpaired high surrogate");
                        position_ += 2;
                        const std::uint32_t low = hex_quad();
                        if (low < 0xdc00 || low > 0xdfff)
                            reject("unpaired high surrogate");
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                        reject("unpaired low surrogate");
                    }
                    append_utf8(result, codepoint);
                    break;
                }
                default: reject("invalid JSON string escape");
                }
                continue;
            }
            if (byte < 0x80) {
                result.push_back(static_cast<char>(byte));
                continue;
            }
            const std::size_t length = byte >= 0xc2 && byte <= 0xdf ? 2 :
                                       byte >= 0xe0 && byte <= 0xef ? 3 :
                                       byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
            if (length == 0 || position_ + length - 1 > input_.size())
                reject("invalid UTF-8 sequence");
            std::uint32_t codepoint = byte & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
            for (std::size_t index = 1; index < length; ++index) {
                const auto continuation = static_cast<unsigned char>(input_[position_++]);
                if ((continuation & 0xc0) != 0x80)
                    reject("invalid UTF-8 continuation byte");
                codepoint = (codepoint << 6) | (continuation & 0x3f);
            }
            if ((length == 2 && codepoint < 0x80) ||
                (length == 3 && codepoint < 0x800) ||
                (length == 4 && codepoint < 0x10000) ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
                reject("invalid UTF-8 code point");
            append_utf8(result, codepoint);
        }
        reject("unterminated JSON string");
    }

    std::uint64_t unsigned_integer(std::uint64_t maximum, bool allow_zero) {
        whitespace();
        if (position_ == input_.size() || input_[position_] < '0' || input_[position_] > '9')
            reject("expected unsigned integer");
        std::uint64_t result = 0;
        if (input_[position_] == '0') {
            ++position_;
        } else {
            do {
                const auto digit = static_cast<unsigned>(input_[position_] - '0');
                if (result > (maximum - digit) / 10)
                    reject("integer exceeds its limit");
                result = result * 10 + digit;
                ++position_;
            } while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9');
        }
        if (!allow_zero && result == 0)
            reject("expected positive integer");
        return result;
    }

    bool boolean() {
        whitespace();
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            return true;
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            return false;
        }
        reject("expected JSON Boolean");
    }

private:
    std::uint32_t hex_quad() {
        if (position_ + 4 > input_.size())
            reject("incomplete Unicode escape");
        std::uint32_t result = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const char character = input_[position_++];
            unsigned digit{};
            if (character >= '0' && character <= '9') digit = character - '0';
            else if (character >= 'a' && character <= 'f') digit = character - 'a' + 10;
            else if (character >= 'A' && character <= 'F') digit = character - 'A' + 10;
            else reject("invalid Unicode escape");
            result = (result << 4) | digit;
        }
        return result;
    }

    std::string_view input_;
    std::size_t position_{};
};

void mark_key(unsigned &seen, unsigned bit) {
    if ((seen & bit) != 0)
        reject("duplicate JSON key");
    seen |= bit;
}

void next_member(JsonCursor &cursor, bool &first) {
    if (!first)
        cursor.expect(',');
    first = false;
}

std::vector<std::string> parse_strings(JsonCursor &cursor, std::size_t maximum_items,
                                       std::size_t maximum_bytes, bool ids) {
    cursor.expect('[');
    std::vector<std::string> result;
    if (cursor.consume(']'))
        return result;
    for (;;) {
        if (result.size() == maximum_items)
            reject("array exceeds its item limit");
        std::string value = cursor.string();
        if (ids) validate_id(value);
        else validate_text(value, maximum_bytes);
        result.push_back(std::move(value));
        if (cursor.consume(']'))
            return result;
        cursor.expect(',');
    }
}

CaseProbe parse_probe(JsonCursor &cursor) {
    cursor.expect('{');
    CaseProbe probe;
    unsigned seen = 0;
    bool first = true;
    while (!cursor.consume('}')) {
        next_member(cursor, first);
        const std::string key = cursor.string();
        cursor.expect(':');
        if (key == "entry") {
            mark_key(seen, 1);
            probe.entry = static_cast<std::uint32_t>(cursor.unsigned_integer(
                std::numeric_limits<std::uint32_t>::max(), true));
        } else if (key == "min_calls") {
            mark_key(seen, 2);
            probe.min_calls = cursor.unsigned_integer(
                std::numeric_limits<std::uint64_t>::max(), false);
        } else {
            reject("unknown probe key");
        }
    }
    if (seen != 3)
        reject("missing probe key");
    return probe;
}

std::vector<CaseProbe> parse_probes(JsonCursor &cursor) {
    cursor.expect('[');
    std::vector<CaseProbe> result;
    std::unordered_set<std::uint32_t> entries;
    if (cursor.consume(']'))
        return result;
    for (;;) {
        if (result.size() == 32)
            reject("required_probes exceeds 32 entries");
        const CaseProbe probe = parse_probe(cursor);
        if (!entries.insert(probe.entry).second)
            reject("duplicate required probe entry");
        result.push_back(probe);
        if (cursor.consume(']'))
            return result;
        cursor.expect(',');
    }
}

CaseSpec parse_case(JsonCursor &cursor) {
    cursor.expect('{');
    CaseSpec spec;
    unsigned seen = 0;
    bool first = true;
    while (!cursor.consume('}')) {
        next_member(cursor, first);
        const std::string key = cursor.string();
        cursor.expect(':');
        if (key == "id") {
            mark_key(seen, 1);
            spec.id = cursor.string();
            validate_id(spec.id);
        } else if (key == "version") {
            mark_key(seen, 2);
            spec.version = static_cast<std::uint32_t>(cursor.unsigned_integer(
                std::numeric_limits<std::uint32_t>::max(), false));
        } else if (key == "title") {
            mark_key(seen, 4);
            spec.title = cursor.string();
            validate_text(spec.title, 256);
        } else if (key == "steps") {
            mark_key(seen, 8);
            spec.steps = parse_strings(cursor, 128, 512, false);
        } else if (key == "checkpoints") {
            mark_key(seen, 16);
            spec.checkpoints = parse_strings(cursor, 128, 0, true);
            std::unordered_set<std::string> unique(spec.checkpoints.begin(), spec.checkpoints.end());
            if (unique.size() != spec.checkpoints.size())
                reject("duplicate checkpoint ID");
        } else if (key == "required_probes") {
            mark_key(seen, 32);
            spec.required_probes = parse_probes(cursor);
        } else if (key == "required_state_fields") {
            mark_key(seen, 64);
            spec.required_state_fields = parse_strings(cursor, 32, 128, false);
            std::unordered_set<std::string> unique(spec.required_state_fields.begin(),
                                                   spec.required_state_fields.end());
            if (unique.size() != spec.required_state_fields.size())
                reject("duplicate required state field");
        } else if (key == "human_acceptance") {
            mark_key(seen, 128);
            spec.human_acceptance = cursor.boolean();
        } else {
            reject("unknown case key");
        }
    }
    if (seen != 255)
        reject("missing case key");
    return spec;
}

std::vector<CaseSpec> parse_cases(JsonCursor &cursor) {
    cursor.expect('[');
    std::vector<CaseSpec> result;
    std::unordered_set<std::string> ids;
    if (cursor.consume(']'))
        return result;
    for (;;) {
        if (result.size() == 128)
            reject("catalog exceeds 128 cases");
        CaseSpec spec = parse_case(cursor);
        if (!ids.insert(spec.id).second)
            reject("duplicate case ID");
        result.push_back(std::move(spec));
        if (cursor.consume(']'))
            return result;
        cursor.expect(',');
    }
}

void append_json_string(std::string &output, std::string_view value) {
    output.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        } else if (character < 0x20) {
            output += "\\u00";
            output.push_back(hex[character >> 4]);
            output.push_back(hex[character & 0x0f]);
        } else {
            output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

void append_string_array(std::string &output, const std::vector<std::string> &values) {
    output.push_back('[');
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) output.push_back(',');
        append_json_string(output, values[index]);
    }
    output.push_back(']');
}

std::string canonical_json(const CaseCatalog &catalog) {
    std::string output = "{\"cases\":[";
    for (std::size_t index = 0; index < catalog.cases.size(); ++index) {
        if (index) output.push_back(',');
        const CaseSpec &spec = catalog.cases[index];
        output += "{\"checkpoints\":";
        append_string_array(output, spec.checkpoints);
        output += ",\"human_acceptance\":";
        output += spec.human_acceptance ? "true" : "false";
        output += ",\"id\":";
        append_json_string(output, spec.id);
        output += ",\"required_probes\":[";
        for (std::size_t probe_index = 0; probe_index < spec.required_probes.size(); ++probe_index) {
            if (probe_index) output.push_back(',');
            const CaseProbe &probe = spec.required_probes[probe_index];
            output += "{\"entry\":" + std::to_string(probe.entry) +
                      ",\"min_calls\":" + std::to_string(probe.min_calls) + "}";
        }
        output += "],\"required_state_fields\":";
        append_string_array(output, spec.required_state_fields);
        output += ",\"steps\":";
        append_string_array(output, spec.steps);
        output += ",\"title\":";
        append_json_string(output, spec.title);
        output += ",\"version\":" + std::to_string(spec.version) + "}";
    }
    output += "],\"schema\":\"yakumo-case-catalog-v1\"}";
    return output;
}

} // namespace

CaseCatalog parse_case_catalog(std::string_view json) {
    JsonCursor cursor(json);
    cursor.expect('{');
    CaseCatalog catalog;
    unsigned seen = 0;
    bool first = true;
    while (!cursor.consume('}')) {
        next_member(cursor, first);
        const std::string key = cursor.string();
        cursor.expect(':');
        if (key == "schema") {
            mark_key(seen, 1);
            if (cursor.string() != kSchema)
                reject("unsupported catalog schema");
        } else if (key == "cases") {
            mark_key(seen, 2);
            catalog.cases = parse_cases(cursor);
        } else {
            reject("unknown catalog key");
        }
    }
    if (seen != 3)
        reject("missing catalog key");
    cursor.finish();
    const std::string canonical = canonical_json(catalog);
    catalog.sha256 = psprecomp::sha256_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(canonical.data()), canonical.size()));
    return catalog;
}

CaseCatalog load_case_catalog(const std::filesystem::path &path) {
#if defined(__unix__) || defined(__APPLE__)
    int descriptor;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0)
        reject("cannot open regular catalog file without following links");
    struct Descriptor {
        int value;
        ~Descriptor() { ::close(value); }
    } handle{descriptor};
    struct stat metadata {};
    if (::fstat(handle.value, &metadata) != 0 || !S_ISREG(metadata.st_mode))
        reject("catalog must be a regular file");
    if (metadata.st_size < 0 || static_cast<std::uint64_t>(metadata.st_size) > kMaxCatalogBytes)
        reject("catalog exceeds 512 KiB");
    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(metadata.st_size));
    std::array<char, 8192> buffer{};
    for (;;) {
        const std::size_t remaining = kMaxCatalogBytes + 1 - bytes.size();
        const std::size_t requested = remaining < buffer.size() ? remaining : buffer.size();
        ssize_t count;
        do {
            count = ::read(handle.value, buffer.data(), requested);
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            reject("cannot read catalog file");
        if (count == 0)
            break;
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
        if (bytes.size() > kMaxCatalogBytes)
            reject("catalog exceeds 512 KiB");
    }
    return parse_case_catalog(bytes);
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status))
        reject("catalog must be a regular file without a symbolic link");
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaxCatalogBytes)
        reject("catalog exceeds 512 KiB or cannot be measured");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        reject("cannot open catalog file");
    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(size));
    std::array<char, 8192> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        bytes.append(buffer.data(), static_cast<std::size_t>(stream.gcount()));
        if (bytes.size() > kMaxCatalogBytes)
            reject("catalog exceeds 512 KiB");
    }
    if (!stream.eof())
        reject("cannot read catalog file");
    return parse_case_catalog(bytes);
#endif
}

} // namespace mhp3rd::testing
