#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace mhp3rd::testing {
inline constexpr std::uint16_t kJournalVersion = 1;
inline constexpr std::size_t kJournalHeaderBytes = 16;
inline constexpr std::size_t kRecordHeaderBytes = 32;
inline constexpr std::size_t kMaxPayloadBytes = 65536;

enum class EventKind : std::uint16_t {
    RunBegin = 1, RunEnd = 2, CaseBegin = 3, CaseEnd = 4,
    Checkpoint = 5, Anomaly = 6, Input = 7, State = 8,
    Probe = 9, Performance = 10, Error = 11, RecordingLoss = 12,
};
using FieldValue = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string>;
struct Field { std::string name; FieldValue value; };
using Fields = std::vector<Field>;
// Produces a UTF-8 JSON object. Rejects duplicate/empty keys, invalid UTF-8,
// non-finite numbers and payloads larger than the framing limit.
[[nodiscard]] std::string fields_json(std::span<const Field> fields);

struct JournalRecord {
    EventKind kind{};
    std::uint64_t sequence{};
    std::uint64_t monotonic_ns{};
    std::string payload;
};
[[nodiscard]] bool known_event_kind(EventKind kind) noexcept;
[[nodiscard]] std::uint32_t journal_crc32(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::array<std::uint8_t, kJournalHeaderBytes> journal_header();
// Framing only. SessionRecorder supplies payloads using fields_json().
[[nodiscard]] std::vector<std::uint8_t> encode_record(const JournalRecord &record);

enum class RecoveryIssue { None, Open, Truncated, Corrupt, Unsupported, LimitExceeded };
struct JournalRecovery {
    RecoveryIssue issue{RecoveryIssue::Open};
    std::size_t valid_bytes{};
    bool begin_seen{};
    bool end_seen{};
    bool loss_seen{};
    std::vector<JournalRecord> records;
    std::string detail;
    // Complete framing does not mean the user's case passed, or that the run
    // was marked completed in its payload. Payload semantics are a later layer.
    [[nodiscard]] bool complete() const noexcept { return issue == RecoveryIssue::None && end_seen; }
};
// Stops at the first invalid record; never scans past corruption for a later
// plausible marker. Sequence starts at 1. RunBegin must be first and RunEnd last.
[[nodiscard]] JournalRecovery recover_journal(std::span<const std::uint8_t> bytes,
    std::size_t max_payload_bytes = kMaxPayloadBytes, std::size_t max_records = 1000000);
// A bounded, read-only convenience reader for a closed file or crash prefix.
[[nodiscard]] JournalRecovery read_journal(const std::filesystem::path &path,
    std::size_t max_file_bytes = 256u * 1024u * 1024u);
} // namespace mhp3rd::testing
