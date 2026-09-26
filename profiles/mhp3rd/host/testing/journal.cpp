#include "testing/journal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace mhp3rd::testing {
namespace {
constexpr std::array<std::uint8_t, 8> kMagic{'Y', 'K', 'M', 'J', 'N', 'L', '1', 0};
constexpr std::array<std::uint8_t, 4> kFrame{'Y', 'K', 'E', '1'};

bool valid_utf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80u) continue;
        unsigned continuation{};
        std::uint32_t code{}, minimum{};
        if (lead >= 0xc2u && lead <= 0xdfu) { continuation = 1; code = lead & 0x1fu; minimum = 0x80u; }
        else if (lead >= 0xe0u && lead <= 0xefu) { continuation = 2; code = lead & 0x0fu; minimum = 0x800u; }
        else if (lead >= 0xf0u && lead <= 0xf4u) { continuation = 3; code = lead & 7u; minimum = 0x10000u; }
        else return false;
        if (continuation > text.size() - i) return false;
        for (unsigned j = 0; j < continuation; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0u) != 0x80u) return false;
            code = (code << 6u) | (next & 0x3fu);
        }
        if (code < minimum || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) return false;
    }
    return true;
}

void bounded_append(std::string &output, std::string_view value) {
    if (value.size() > kMaxPayloadBytes - output.size()) throw std::length_error("Journal payload exceeds limit");
    output.append(value);
}

void quoted(std::string &output, std::string_view text) {
    if (text.size() > kMaxPayloadBytes - output.size()) throw std::length_error("Journal string exceeds limit");
    if (!valid_utf8(text)) throw std::invalid_argument("Journal field is not valid UTF-8");
    bounded_append(output, "\"");
    constexpr char hex[] = "0123456789abcdef";
    for (const auto character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (value == '"') bounded_append(output, "\\\"");
        else if (value == '\\') bounded_append(output, "\\\\");
        else if (value < 0x20u) {
            const char escaped[]{'\\', 'u', '0', '0', hex[value >> 4u], hex[value & 15u]};
            bounded_append(output, std::string_view(escaped, sizeof(escaped)));
        } else bounded_append(output, std::string_view(&character, 1));
    }
    bounded_append(output, "\"");
}

template <typename T>
void number(std::string &output, T value) {
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite journal number");
        if (value == 0.0 && std::signbit(value)) { bounded_append(output, "-0.0"); return; }
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) throw std::invalid_argument("Cannot serialize journal number");
    bounded_append(output, std::string_view(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())));
}

void put(std::span<std::uint8_t> bytes, std::size_t at, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}
std::uint64_t get(std::span<const std::uint8_t> bytes, std::size_t at, unsigned width) {
    std::uint64_t value{};
    for (unsigned i = 0; i < width; ++i) value |= std::uint64_t{bytes[at + i]} << (8u * i);
    return value;
}
constexpr auto crc_table() {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t i = 0; i < result.size(); ++i) {
        auto crc = i;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        result[i] = crc;
    }
    return result;
}
constexpr auto kCrcTable = crc_table();
std::uint32_t extend_crc(std::uint32_t crc, std::span<const std::uint8_t> bytes) noexcept {
    for (auto value : bytes) crc = kCrcTable[(crc ^ value) & 0xffu] ^ (crc >> 8u);
    return crc;
}
std::uint32_t record_crc(std::span<const std::uint8_t> header, std::span<const std::uint8_t> payload) noexcept {
    return ~extend_crc(extend_crc(0xffffffffu, header.first(28)), payload);
}
} // namespace

std::string fields_json(std::span<const Field> fields) {
    // Each field requires at least an empty value and key punctuation. Bound
    // the bookkeeping too, before allocating a set for an oversized request.
    if (fields.size() > kMaxPayloadBytes / 5u) throw std::length_error("Too many journal fields");
    std::unordered_set<std::string_view> names;
    std::string output{"{"};
    bool first = true;
    for (const auto &field : fields) {
        if (field.name.size() > kMaxPayloadBytes) throw std::length_error("Journal key exceeds limit");
        if (field.name.empty() || !names.insert(field.name).second)
            throw std::invalid_argument("Empty or duplicate journal field name");
        if (!first) bounded_append(output, ",");
        first = false;
        quoted(output, field.name);
        bounded_append(output, ":");
        std::visit([&output](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) bounded_append(output, "null");
            else if constexpr (std::is_same_v<T, bool>) bounded_append(output, value ? "true" : "false");
            else if constexpr (std::is_same_v<T, std::string>) quoted(output, value);
            else number(output, value);
        }, field.value);
    }
    bounded_append(output, "}");
    return output;
}

bool known_event_kind(EventKind kind) noexcept {
    const auto value = static_cast<std::uint16_t>(kind);
    return value >= static_cast<std::uint16_t>(EventKind::RunBegin) &&
           value <= static_cast<std::uint16_t>(EventKind::RecordingLoss);
}
std::uint32_t journal_crc32(std::span<const std::uint8_t> bytes) noexcept {
    return ~extend_crc(0xffffffffu, bytes);
}
std::array<std::uint8_t, kJournalHeaderBytes> journal_header() {
    std::array<std::uint8_t, kJournalHeaderBytes> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    put(header, 8, kJournalVersion, 2);
    put(header, 10, kJournalHeaderBytes, 2);
    return header;
}
std::vector<std::uint8_t> encode_record(const JournalRecord &record) {
    if (!known_event_kind(record.kind) || record.sequence == 0 || !valid_utf8(record.payload))
        throw std::invalid_argument("Invalid journal record kind, sequence or UTF-8");
    if (record.payload.size() > kMaxPayloadBytes) throw std::length_error("Journal payload exceeds limit");
    std::vector<std::uint8_t> bytes(kRecordHeaderBytes + record.payload.size());
    std::copy(kFrame.begin(), kFrame.end(), bytes.begin());
    put(bytes, 4, record.payload.size(), 4);
    put(bytes, 8, record.sequence, 8);
    put(bytes, 16, record.monotonic_ns, 8);
    put(bytes, 24, static_cast<std::uint16_t>(record.kind), 2);
    std::copy(record.payload.begin(), record.payload.end(), bytes.begin() + kRecordHeaderBytes);
    put(bytes, 28, record_crc(bytes, std::span(bytes).subspan(kRecordHeaderBytes)), 4);
    return bytes;
}

JournalRecovery recover_journal(std::span<const std::uint8_t> bytes,
                               std::size_t max_payload_bytes, std::size_t max_records) {
    JournalRecovery result;
    const auto fail = [&result](RecoveryIssue issue, const char *detail) {
        result.issue = issue;
        result.detail = detail;
    };
    if (bytes.size() < kJournalHeaderBytes) {
        fail(RecoveryIssue::Truncated, "Partial journal header"); return result;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        fail(RecoveryIssue::Corrupt, "Journal magic differs"); return result;
    }
    if (get(bytes, 8, 2) != kJournalVersion || get(bytes, 10, 2) != kJournalHeaderBytes) {
        fail(RecoveryIssue::Unsupported, "Unsupported journal version or header length"); return result;
    }
    if (get(bytes, 12, 4) != 0) {
        fail(RecoveryIssue::Corrupt, "Journal reserved fields are nonzero"); return result;
    }
    result.valid_bytes = kJournalHeaderBytes;
    const auto payload_limit = std::min(max_payload_bytes, kMaxPayloadBytes);
    while (result.valid_bytes < bytes.size()) {
        if (result.end_seen) { fail(RecoveryIssue::Corrupt, "Bytes follow RunEnd"); return result; }
        const auto remaining = bytes.subspan(result.valid_bytes);
        if (remaining.size() < kRecordHeaderBytes) {
            fail(RecoveryIssue::Truncated, "Partial record header"); return result;
        }
        if (!std::equal(kFrame.begin(), kFrame.end(), remaining.begin()) || get(remaining, 26, 2) != 0) {
            fail(RecoveryIssue::Corrupt, "Invalid record marker or reserved fields"); return result;
        }
        const auto kind = static_cast<EventKind>(get(remaining, 24, 2));
        if (!known_event_kind(kind)) { fail(RecoveryIssue::Unsupported, "Unknown event kind"); return result; }
        const auto length = static_cast<std::size_t>(get(remaining, 4, 4));
        if (length > payload_limit || result.records.size() >= max_records) {
            fail(RecoveryIssue::LimitExceeded, "Record count or payload limit exceeded"); return result;
        }
        if (length > remaining.size() - kRecordHeaderBytes) {
            fail(RecoveryIssue::Truncated, "Partial record payload"); return result;
        }
        const auto payload = remaining.subspan(kRecordHeaderBytes, length);
        if (get(remaining, 28, 4) != record_crc(remaining, payload)) {
            fail(RecoveryIssue::Corrupt, "Record checksum differs"); return result;
        }
        const auto sequence = get(remaining, 8, 8);
        if (sequence != static_cast<std::uint64_t>(result.records.size()) + 1u ||
            (!result.begin_seen && kind != EventKind::RunBegin) ||
            (result.begin_seen && kind == EventKind::RunBegin)) {
            fail(RecoveryIssue::Corrupt, "Invalid sequence or run lifecycle"); return result;
        }
        const std::string text(reinterpret_cast<const char *>(payload.data()), payload.size());
        if (!valid_utf8(text)) { fail(RecoveryIssue::Corrupt, "Record payload is not UTF-8"); return result; }
        result.records.push_back({kind, sequence, get(remaining, 16, 8), text});
        result.valid_bytes += kRecordHeaderBytes + length;
        result.begin_seen = true;
        result.end_seen = kind == EventKind::RunEnd;
        result.loss_seen = result.loss_seen || kind == EventKind::RecordingLoss;
    }
    result.issue = result.end_seen ? RecoveryIssue::None : RecoveryIssue::Open;
    result.detail = result.end_seen ? "Complete journal framing; payload semantics require separate validation"
                                    : "Valid prefix without RunEnd";
    return result;
}

JournalRecovery read_journal(const std::filesystem::path &path, std::size_t max_file_bytes) {
    JournalRecovery failure;
    try {
        if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path))
            throw std::runtime_error("Journal must be a regular file");
        const auto length = std::filesystem::file_size(path);
        if (length > max_file_bytes || length > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
            failure.issue = RecoveryIssue::LimitExceeded;
            failure.detail = "Journal exceeds file reader limit";
            return failure;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open journal");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (length != 0) {
            file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(length));
            if (file.gcount() != static_cast<std::streamsize>(length)) throw std::runtime_error("Journal changed or read failed");
        }
        if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Journal grew while reading");
        if (file.bad()) throw std::runtime_error("Journal read failed at end of file");
        return recover_journal(bytes);
    } catch (const std::exception &error) {
        failure.issue = RecoveryIssue::Corrupt;
        failure.detail = error.what();
        return failure;
    }
}
} // namespace mhp3rd::testing
