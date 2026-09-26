#include "testing/journal.hpp"
#include "testing/session_recorder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {
using namespace mhp3rd::testing;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;

int failures = 0;
void check(bool condition, const std::string &message) {
    if (!condition && ++failures <= 50) std::cerr << "FAIL: " << message << '\n';
}
template <typename Exception, typename Function>
void check_throws(Function &&function, const std::string &message) {
    try {
        function();
        check(false, message);
    } catch (const Exception &) {
    } catch (...) {
        check(false, message + " (wrong exception type)");
    }
}
void put_le(Bytes &bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8u)));
}
void set_le(Bytes &bytes, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8u));
}
// This bitwise reference is intentionally independent of journal_crc32().
std::uint32_t reference_crc(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}
Bytes fixture_header() {
    return {'Y', 'K', 'M', 'J', 'N', 'L', '1', 0, 1, 0, 16, 0, 0, 0, 0, 0};
}
Bytes fixture_record(EventKind kind, std::uint64_t sequence, std::uint64_t monotonic_ns,
                     const std::string &payload) {
    Bytes frame{'Y', 'K', 'E', '1'};
    put_le(frame, payload.size(), 4);
    put_le(frame, sequence, 8);
    put_le(frame, monotonic_ns, 8);
    put_le(frame, static_cast<std::uint16_t>(kind), 2);
    put_le(frame, 0, 2);
    Bytes checked = frame;
    checked.insert(checked.end(), payload.begin(), payload.end());
    put_le(frame, reference_crc(checked), 4);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}
void replace_checksum(Bytes &journal, std::size_t frame_offset) {
    const std::size_t payload_length = journal[frame_offset + 4] |
        (static_cast<std::size_t>(journal[frame_offset + 5]) << 8u) |
        (static_cast<std::size_t>(journal[frame_offset + 6]) << 16u) |
        (static_cast<std::size_t>(journal[frame_offset + 7]) << 24u);
    Bytes checked(journal.begin() + frame_offset, journal.begin() + frame_offset + 28);
    checked.insert(checked.end(), journal.begin() + frame_offset + 32,
                   journal.begin() + frame_offset + 32 + payload_length);
    set_le(journal, frame_offset + 28, reference_crc(checked), 4);
}
void append(Bytes &destination, const Bytes &source) {
    destination.insert(destination.end(), source.begin(), source.end());
}
struct Fixture {
    Bytes bytes = fixture_header();
    std::array<std::size_t, 3> boundaries{};
    Fixture() {
        append(bytes, fixture_record(EventKind::RunBegin, 1, 10, "{}"));
        boundaries[0] = bytes.size();
        append(bytes, fixture_record(EventKind::Input, 2, 20, "{\"key\":1}"));
        boundaries[1] = bytes.size();
        append(bytes, fixture_record(EventKind::RunEnd, 3, 30, "{\"completed\":false}"));
        boundaries[2] = bytes.size();
    }
};
void test_framing_and_json() {
    const auto header = journal_header();
    const auto expected_header = fixture_header();
    check(std::equal(header.begin(), header.end(), expected_header.begin()), "journal header has exact v1 bytes");
    const std::array<std::uint8_t, 9> digits{'1','2','3','4','5','6','7','8','9'};
    check(reference_crc(digits) == 0xcbf43926u, "reference CRC matches canonical IEEE vector");
    check(journal_crc32(digits) == 0xcbf43926u, "journal CRC matches canonical IEEE vector");

    const std::array<std::uint8_t, 34> golden{'Y','K','E','1',2,0,0,0,1,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,1,0,0,0,0xcf,0x60,0x17,0xdc,'{','}'};
    const auto encoded = encode_record({EventKind::RunBegin, 1, 0, "{}"});
    check(std::equal(encoded.begin(), encoded.end(), golden.begin(), golden.end()),
          "encoded first frame matches fixed golden bytes and checksum");
    check(encoded == fixture_record(EventKind::RunBegin, 1, 0, "{}"),
          "encoder matches independent wire builder");
    check(encode_record({EventKind::Input, 1, 0, std::string(kMaxPayloadBytes, 'a')}).size() ==
          kRecordHeaderBytes + kMaxPayloadBytes, "exact maximum frame payload is accepted");
    check_throws<std::invalid_argument>([] { (void)encode_record({EventKind::Input, 0, 0, "{}"}); },
                                        "zero sequence is rejected");
    check_throws<std::invalid_argument>([] { (void)encode_record({static_cast<EventKind>(999), 1, 0, "{}"}); },
                                        "unknown event kind is rejected by encoder");
    check_throws<std::invalid_argument>([] { (void)encode_record({EventKind::Input, 1, 0, "\xff"}); },
                                        "invalid UTF-8 frame is rejected by encoder");
    check_throws<std::length_error>([] { (void)encode_record({EventKind::Input, 1, 0,
                                        std::string(kMaxPayloadBytes + 1, 'x')}); },
                                    "oversized frame is rejected by encoder");

    check(fields_json(Fields{}) == "{}", "empty field set produces JSON object");
    const std::string japanese = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
    const auto json = fields_json(Fields{
        {"text", std::string("quote \" slash \\ newline\n tab\t control\x01")},
        {"signed", std::int64_t{-7}}, {"unsigned", std::numeric_limits<std::uint64_t>::max()},
        {"ratio", 2.5}, {"enabled", true}, {"missing", nullptr}, {"utf8", japanese}});
    check(json.starts_with('{') && json.ends_with('}'), "typed fields produce JSON object");
    check(json.find("\"quote \\\" slash \\\\ newline\\u000a tab\\u0009 control\\u0001\"") != std::string::npos,
          "string escaping covers quote, backslash and controls");
    check(json.find("\"signed\":-7") != std::string::npos &&
          json.find("\"unsigned\":18446744073709551615") != std::string::npos &&
          json.find("\"ratio\":2.5") != std::string::npos &&
          json.find("\"enabled\":true") != std::string::npos &&
          json.find("\"missing\":null") != std::string::npos,
          "typed fields retain integer, floating, boolean and null values");
    check(json.find(japanese) != std::string::npos, "UTF-8 field value survives encoding");
    check(fields_json(Fields{{"negative_zero", -0.0}}).find("\"negative_zero\":-0.0") !=
          std::string::npos, "negative floating zero retains its sign");
    check_throws<std::invalid_argument>([] { (void)fields_json(Fields{{"", true}}); },
                                        "empty JSON key is rejected");
    check_throws<std::invalid_argument>([] { (void)fields_json(Fields{{"x", true}, {"x", false}}); },
                                        "duplicate JSON key is rejected");
    check_throws<std::invalid_argument>([] { (void)fields_json(Fields{{"\xff", true}}); },
                                        "invalid UTF-8 key is rejected");
    check_throws<std::invalid_argument>([] { (void)fields_json(Fields{{"x", std::string("\xc0\xaf")}}); },
                                        "overlong UTF-8 value is rejected");
    check_throws<std::invalid_argument>([] { (void)fields_json(Fields{{"x", std::numeric_limits<double>::infinity()}}); },
                                        "infinity is rejected");
    check_throws<std::invalid_argument>([] { (void)fields_json(Fields{{"x", std::numeric_limits<double>::quiet_NaN()}}); },
                                        "NaN is rejected");
    check_throws<std::length_error>([] { (void)fields_json(Fields{{"x", std::string(kMaxPayloadBytes, 'x')}}); },
                                    "oversized JSON payload is rejected");
}
void test_recovery() {
    const Fixture fixture;
    const auto complete = recover_journal(fixture.bytes);
    check(complete.complete() && complete.issue == RecoveryIssue::None && complete.begin_seen &&
          complete.end_seen && !complete.loss_seen && complete.records.size() == 3 &&
          complete.valid_bytes == fixture.bytes.size(), "valid journal recovers all records");
    if (complete.records.size() == 3) {
        check(complete.records[0].kind == EventKind::RunBegin && complete.records[0].sequence == 1 &&
              complete.records[1].kind == EventKind::Input && complete.records[1].sequence == 2 &&
              complete.records[1].payload == "{\"key\":1}" &&
              complete.records[2].kind == EventKind::RunEnd && complete.records[2].sequence == 3,
              "recovered sequence, kind and payload match fixed fixture");
    }
    // The footer says completed=false. Framing completeness makes no case-acceptance claim.
    check(complete.complete() && complete.records.back().payload == "{\"completed\":false}",
          "framing completeness is independent of the footer's semantic result");
    for (std::size_t length = 0; length < fixture.bytes.size(); ++length) {
        const auto prefix = recover_journal(std::span<const std::uint8_t>(fixture.bytes.data(), length));
        const std::size_t expected_valid = length >= fixture.boundaries[1] ? fixture.boundaries[1] :
            length >= fixture.boundaries[0] ? fixture.boundaries[0] : length >= kJournalHeaderBytes ?
            kJournalHeaderBytes : 0;
        check(prefix.valid_bytes == expected_valid, "every byte prefix retains exactly the valid frames at " +
              std::to_string(length));
        check(prefix.records.size() == (expected_valid == fixture.boundaries[1] ? 2u :
              expected_valid == fixture.boundaries[0] ? 1u : 0u),
              "every byte prefix retains the expected record count at " + std::to_string(length));
        const bool at_boundary = length == kJournalHeaderBytes || length == fixture.boundaries[0] ||
            length == fixture.boundaries[1];
        check(prefix.issue == (at_boundary ? RecoveryIssue::Open : RecoveryIssue::Truncated),
              "exact boundaries are open and partial headers/frames truncated at " + std::to_string(length));
        check(!prefix.complete() && !prefix.end_seen, "no partial prefix fabricates RunEnd");
    }
    auto bad = fixture.bytes;
    const auto second = fixture.boundaries[0];
    bad[second + 28] ^= 1u;
    auto recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.valid_bytes == second &&
          recovered.records.size() == 1, "checksum corruption preserves only prior valid frame");
    // A plausible marker after a corrupt frame must never be used to resynchronize.
    append(bad, fixture_record(EventKind::RunEnd, 4, 40, "{}"));
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.valid_bytes == second && !recovered.end_seen,
          "reader does not resynchronize after a corrupt frame");
    bad = fixture.bytes;
    bad[second + 4] = 3;
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.valid_bytes == second,
          "length corruption invalidates the frame");
    bad = fixture.bytes;
    set_le(bad, second + 8, 9, 8); replace_checksum(bad, second);
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.valid_bytes == second,
          "valid checksum cannot hide a sequence gap");
    bad = fixture.bytes;
    set_le(bad, second + 24, 999, 2); replace_checksum(bad, second);
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Unsupported && recovered.valid_bytes == second,
          "unknown event kind is unsupported");
    bad = fixture.bytes;
    bad[second + 26] = 1; replace_checksum(bad, second);
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.valid_bytes == second,
          "nonzero record reserved bytes are corrupt");
    bad = fixture.bytes;
    bad[second + 32] = 0xff; replace_checksum(bad, second);
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.valid_bytes == second,
          "invalid UTF-8 payload is corrupt even with correct checksum");
    bad = fixture.bytes;
    bad[8] = 2;
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Unsupported && recovered.valid_bytes == 0,
          "unknown file version is unsupported");
    bad = fixture.bytes;
    bad[12] = 1;
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.valid_bytes == 0,
          "nonzero file reserved bytes are corrupt");
    bad = fixture.bytes;
    bad.push_back(0);
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.valid_bytes == fixture.bytes.size() &&
          recovered.records.size() == 3 && !recovered.complete(), "bytes after RunEnd are corrupt");
    recovered = recover_journal(fixture.bytes, 1);
    check(recovered.issue == RecoveryIssue::LimitExceeded && recovered.valid_bytes == kJournalHeaderBytes,
          "configured payload limit applies before allocating record body");
    recovered = recover_journal(fixture.bytes, kMaxPayloadBytes, 2);
    check(recovered.issue == RecoveryIssue::LimitExceeded && recovered.valid_bytes == fixture.boundaries[1] &&
          recovered.records.size() == 2, "configured record limit retains valid prefix");
    bad = fixture_header();
    append(bad, fixture_record(EventKind::RunEnd, 1, 0, "{}"));
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.records.empty(),
          "RunEnd cannot start a journal");
    bad = fixture_header();
    append(bad, fixture_record(EventKind::RunBegin, 1, 0, "{}"));
    append(bad, fixture_record(EventKind::RunBegin, 2, 1, "{}"));
    recovered = recover_journal(bad);
    check(recovered.issue == RecoveryIssue::Corrupt && recovered.records.size() == 1,
          "RunBegin cannot recur inside a journal");
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static std::atomic<unsigned> next{0};
        for (unsigned trial = 0; trial < 100; ++trial) {
            path = std::filesystem::temp_directory_path() /
                ("yakumo-journal-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(next++));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) return;
        }
        throw std::runtime_error("cannot create isolated test directory");
    }
    ~TempDir() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void test_file_sink_and_reader() {
    TempDir temp;
    const auto path = temp.path / "run.journal";
    const Fixture fixture;
    {
        auto sink = make_file_sink(path);
        check(static_cast<bool>(sink), "file sink creates a new journal exclusively");
        if (sink) {
            check(sink->write(fixture.bytes) && sink->flush(), "file sink writes and flushes fixture");
            check_throws<std::system_error>([&] { (void)make_file_sink(path); },
                                            "existing journal cannot be replaced");
        }
    }
    const auto recovered = read_journal(path);
    check(recovered.complete() && recovered.valid_bytes == fixture.bytes.size(),
          "bounded file reader recovers valid closed fixture");
    const auto limited = read_journal(path, fixture.bytes.size() - 1);
    check(limited.issue == RecoveryIssue::LimitExceeded && limited.records.empty(),
          "file-size cap rejects input before reading it");
    const auto symlink = temp.path / "existing-link";
    std::error_code error;
    std::filesystem::create_symlink(path, symlink, error);
    if (!error) check_throws<std::system_error>([&] { (void)make_file_sink(symlink); },
                                                "existing symlink cannot be replaced");
    check(read_journal(temp.path / "absent").issue != RecoveryIssue::None,
          "missing journal is not reported as complete");
}

struct MemoryState {
    std::mutex mutex;
    std::condition_variable changed;
    Bytes written;
    Bytes persisted;
    unsigned write_attempts{};
    unsigned flush_attempts{};
    unsigned finish_attempts{};
    unsigned blocked_attempt{};
    bool inside_block{};
    bool release_block{};
    unsigned fail_write_attempt{};
    std::size_t partial_bytes{};
    unsigned fail_flush_attempt{};
    unsigned fail_finish_attempt{};
    [[nodiscard]] Bytes durable_copy() {
        std::lock_guard lock(mutex);
        return persisted;
    }
    [[nodiscard]] Bytes written_copy() {
        std::lock_guard lock(mutex);
        return written;
    }
    void unblock() {
        { std::lock_guard lock(mutex); release_block = true; }
        changed.notify_all();
    }
    template <typename Predicate>
    bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 2000ms) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return predicate(*this); });
    }
};
class MemorySink final : public JournalSink {
public:
    explicit MemorySink(std::shared_ptr<MemoryState> state) : state_(std::move(state)) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        std::unique_lock lock(state_->mutex);
        const auto attempt = ++state_->write_attempts;
        if (attempt == state_->blocked_attempt) {
            state_->inside_block = true;
            state_->changed.notify_all();
            if (!state_->changed.wait_for(lock, 2000ms, [&] { return state_->release_block; })) return false;
        }
        if (attempt == state_->fail_write_attempt) {
            const auto count = std::min(state_->partial_bytes, bytes.size());
            state_->written.insert(state_->written.end(), bytes.begin(), bytes.begin() + count);
            state_->changed.notify_all();
            return false;
        }
        state_->written.insert(state_->written.end(), bytes.begin(), bytes.end());
        state_->changed.notify_all();
        return true;
    }
    bool flush() override {
        std::lock_guard lock(state_->mutex);
        const auto attempt = ++state_->flush_attempts;
        if (attempt == state_->fail_flush_attempt) {
            state_->changed.notify_all();
            return false;
        }
        state_->persisted = state_->written;
        state_->changed.notify_all();
        return true;
    }
    bool finish() override {
        std::lock_guard lock(state_->mutex);
        const auto attempt = ++state_->finish_attempts;
        state_->changed.notify_all();
        return attempt != state_->fail_finish_attempt;
    }
private:
    std::shared_ptr<MemoryState> state_;
};
struct ReleaseBlock {
    std::shared_ptr<MemoryState> state;
    ~ReleaseBlock() { state->unblock(); }
};
RecorderOptions options(std::chrono::milliseconds interval = 20ms) {
    RecorderOptions result;
    result.flush_interval = interval;
    auto clock = std::make_shared<std::atomic<std::uint64_t>>(100);
    result.monotonic_ns = [clock] { return clock->fetch_add(100); };
    return result;
}
bool has_kind(const JournalRecovery &recovery, EventKind kind) {
    return std::any_of(recovery.records.begin(), recovery.records.end(),
                       [kind](const auto &record) { return record.kind == kind; });
}
void test_writer_order_and_close() {
    auto state = std::make_shared<MemoryState>();
    SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{{"role", std::string("candidate")}}, options());
    check(recorder.record(EventKind::Input, Fields{{"ordinal", std::int64_t{1}}}) == AppendResult::Accepted,
          "first caller event accepted");
    check(recorder.record(EventKind::State, Fields{{"ordinal", std::int64_t{2}}}) == AppendResult::Accepted,
          "second caller event accepted");
    check(recorder.record(EventKind::Checkpoint, Fields{{"ordinal", std::int64_t{3}}}, true) == AppendResult::Accepted,
          "checkpoint accepted");
    check(recorder.flush(2000ms), "manual flush barrier succeeds");
    auto prefix = recover_journal(state->durable_copy());
    check(prefix.issue == RecoveryIssue::Open && prefix.records.size() == 4 && !prefix.end_seen,
          "barrier makes prior events recoverable before close");
    if (prefix.records.size() == 4) {
        check(prefix.records[0].kind == EventKind::RunBegin &&
              prefix.records[1].kind == EventKind::Input &&
              prefix.records[2].kind == EventKind::State &&
              prefix.records[3].kind == EventKind::Checkpoint &&
              prefix.records[0].payload.find("\"role\":\"candidate\"") != std::string::npos,
              "background writer preserves accepted order and run metadata");
    }
    auto health = recorder.health();
    check(health.accepted_events == 3 && health.written_events == 3 && health.written_records == 4 &&
          health.flushed_records >= 4 && health.dropped_events == 0,
          "health distinguishes caller events from generated RunBegin");
    check(recorder.close("test_finished", true), "explicit close succeeds");
    check(recorder.close("ignored", false), "close is idempotent");
    check(recorder.record(EventKind::Input, Fields{}) == AppendResult::Closed,
          "record after close reports closed state");
    const auto complete = recover_journal(state->durable_copy());
    check(complete.complete() && complete.records.size() == 5 &&
          complete.records.back().kind == EventKind::RunEnd,
          "explicit close flushes one final RunEnd");
    if (complete.complete()) {
        const auto &footer = complete.records.back().payload;
        check(footer.find("\"stop_reason\":\"test_finished\"") != std::string::npos &&
              footer.find("\"completed\":true") != std::string::npos &&
              footer.find("\"accepted_events\":3") != std::string::npos &&
              footer.find("\"written_events\":3") != std::string::npos &&
              footer.find("\"dropped_events\":0") != std::string::npos &&
              footer.find("\"invalid_events\":0") != std::string::npos,
              "footer records explicit reason, completion and caller counters");
    }
    health = recorder.health();
    check(health.closed && health.written_records == 5 && health.flushed_records == 5,
          "closed health counts generated footer after successful flush");
}
void test_rejection_and_destructor() {
    auto state = std::make_shared<MemoryState>();
    {
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options());
        for (const auto kind : {EventKind::RunBegin, EventKind::RunEnd, EventKind::RecordingLoss,
                                static_cast<EventKind>(999)})
            check(recorder.record(kind, Fields{}) == AppendResult::Rejected,
                  "reserved or unknown event kind is rejected");
        check(recorder.record(EventKind::Input, Fields{{"x", true}, {"x", false}}) == AppendResult::Rejected,
              "duplicate fields are rejected by recorder");
        check(recorder.record(EventKind::Input, Fields{{"x", std::string("\xff")}}) == AppendResult::Rejected,
              "invalid UTF-8 fields are rejected by recorder");
        check(recorder.record(EventKind::Input, Fields{{"x", std::numeric_limits<double>::infinity()}}) ==
              AppendResult::Rejected, "non-finite fields are rejected by recorder");
        check(recorder.record(EventKind::Input, Fields{{"x", std::string(kMaxPayloadBytes, 'x')}}) ==
              AppendResult::Rejected, "oversized fields are rejected by recorder");
        check(recorder.record(EventKind::Input, Fields{{"x", 1.0}}) == AppendResult::Accepted,
              "recorder remains usable after rejected events");
        check(recorder.flush(2000ms), "valid event flushes after rejections");
        check(recorder.health().invalid_events == 8 && recorder.health().accepted_events == 1,
              "invalid counter tracks all rejected caller events");
    }
    const auto recovered = recover_journal(state->durable_copy());
    check(recovered.complete() && recovered.records.size() == 3,
          "destructor drains valid event and closes journal");
    if (recovered.complete()) {
        const auto &footer = recovered.records.back().payload;
        check(footer.find("\"stop_reason\":\"recorder_destroyed\"") != std::string::npos &&
              footer.find("\"completed\":false") != std::string::npos &&
              footer.find("\"invalid_events\":8") != std::string::npos,
              "implicit close states incomplete destruction and invalid count");
    }
}
void test_invalid_close_reason() {
    for (const auto &reason : {std::string{}, std::string("\xff"),
                               std::string(kMaxPayloadBytes, 'r')}) {
        auto state = std::make_shared<MemoryState>();
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options());
        check(recorder.close(reason, true), "invalid stop reason still closes journal safely");
        const auto recovered = recover_journal(state->durable_copy());
        check(recovered.complete() && recovered.records.size() == 2,
              "invalid stop reason still produces complete framing");
        if (recovered.complete()) {
            const auto &footer = recovered.records.back().payload;
            check(footer.find("\"stop_reason\":\"invalid_stop_reason\"") != std::string::npos &&
                  footer.find("\"completed\":false") != std::string::npos &&
                  footer.find("\"invalid_events\":1") != std::string::npos,
                  "invalid stop reason becomes an incomplete footer with error count");
        }
        check(recorder.health().invalid_events == 1 && !recorder.health().error.empty(),
              "invalid stop reason is visible in recorder health");
    }
}
void test_flush_scheduling() {
    {
        auto state = std::make_shared<MemoryState>();
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options(20ms));
        check(recorder.record(EventKind::Input, Fields{{"periodic", true}}) == AppendResult::Accepted,
              "periodic test event accepted");
        const bool persisted = state->wait_for([](const MemoryState &s) {
            return recover_journal(s.persisted).records.size() >= 2;
        }, 1500ms);
        check(persisted, "20 ms periodic interval flushes within finite deadline");
        check(recorder.close("periodic_test"), "periodic recorder closes");
    }
    {
        auto state = std::make_shared<MemoryState>();
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options(1000ms));
        check(recorder.record(EventKind::Checkpoint, Fields{{"boundary", true}}, true) == AppendResult::Accepted,
              "boundary event accepted");
        const bool persisted = state->wait_for([](const MemoryState &s) {
            return has_kind(recover_journal(s.persisted), EventKind::Checkpoint);
        }, 500ms);
        check(persisted, "boundary event flushes before long periodic interval");
        check(recorder.close("boundary_test"), "boundary recorder closes");
    }
    {
        auto state = std::make_shared<MemoryState>();
        state->blocked_attempt = 1;
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options());
        ReleaseBlock release{state};
        check(state->wait_for([](const MemoryState &s) { return s.inside_block; }),
              "controlled sink blocks writer at first write");
        check(recorder.record(EventKind::Input, Fields{{"barrier", true}}) == AppendResult::Accepted,
              "event accepted while sink is blocked");
        check(!recorder.flush(30ms), "manual barrier reports timeout while sink is blocked");
        state->unblock();
        check(recorder.flush(2000ms), "manual barrier succeeds after releasing sink");
        check(has_kind(recover_journal(state->durable_copy()), EventKind::Input),
              "successful barrier exposes prior event in durable snapshot");
        check(recorder.close("barrier_test"), "barrier recorder closes");
    }
}
void test_overflow() {
    auto state = std::make_shared<MemoryState>();
    state->blocked_attempt = 1;
    auto config = options(1000ms);
    config.max_queue_events = 1;
    SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, config);
    ReleaseBlock release{state};
    check(state->wait_for([](const MemoryState &s) { return s.inside_block; }),
          "overflow test holds writer on first write");
    check(recorder.record(EventKind::Input, Fields{{"ordinal", std::int64_t{1}}}) == AppendResult::Accepted,
          "first queued event fits capacity");
    check(recorder.record(EventKind::Input, Fields{{"ordinal", std::int64_t{2}}}) == AppendResult::Dropped,
          "second event reports bounded event queue overflow");
    check(recorder.health().dropped_events == 1 && recorder.health().accepted_events == 1,
          "overflow health counts accepted and dropped caller events separately");
    state->unblock();
    check(recorder.flush(2000ms), "barrier flushes known loss before shutdown");
    const auto open = recover_journal(state->durable_copy());
    check(open.issue == RecoveryIssue::Open && open.loss_seen && has_kind(open, EventKind::Input),
          "barrier makes accepted input and known loss recoverable together");
    check(recorder.close("overflow_test"), "close drains accepted event and loss after overflow");
    const auto recovered = recover_journal(state->durable_copy());
    check(recovered.complete() && recovered.loss_seen && has_kind(recovered, EventKind::RecordingLoss) &&
          has_kind(recovered, EventKind::Input), "shutdown persists a recoverable overflow record");
    if (recovered.complete()) {
        const auto loss = std::find_if(recovered.records.begin(), recovered.records.end(), [](const auto &record) {
            return record.kind == EventKind::RecordingLoss;
        });
        check(loss != recovered.records.end() &&
              loss->payload.find("\"dropped_total\":1") != std::string::npos &&
              loss->payload.find("\"dropped_since_last\":1") != std::string::npos,
              "loss record contains total and interval drop counts");
        const auto &footer = recovered.records.back().payload;
        check(footer.find("\"dropped_events\":1") != std::string::npos &&
              footer.find("\"accepted_events\":1") != std::string::npos,
              "footer carries caller overflow counters");
    }
    const auto health = recorder.health();
    check(health.written_events == 1 && health.written_records == 4 && health.flushed_records == 4,
          "generated run and loss records affect record totals, not caller event totals");

    auto byte_state = std::make_shared<MemoryState>();
    byte_state->blocked_attempt = 1;
    auto byte_config = options(1000ms);
    byte_config.max_queue_events = 8;
    byte_config.max_queue_bytes = 1;
    SessionRecorder byte_recorder(std::make_unique<MemorySink>(byte_state), Fields{}, byte_config);
    ReleaseBlock byte_release{byte_state};
    check(byte_state->wait_for([](const MemoryState &s) { return s.inside_block; }),
          "byte overflow test holds writer");
    check(byte_recorder.record(EventKind::Input, Fields{{"payload", std::string(80, 'x')}}) == AppendResult::Dropped,
          "event larger than queued-byte budget is dropped");
    byte_state->unblock();
    check(byte_recorder.close("byte_overflow"), "byte-overflow recorder closes");
    check(recover_journal(byte_state->durable_copy()).loss_seen,
          "byte-limit overflow also persists a loss record");
}
void test_io_failures() {
    {
        auto state = std::make_shared<MemoryState>();
        state->fail_write_attempt = 1;
        state->partial_bytes = 5;
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options());
        check(state->wait_for([](const MemoryState &s) { return s.write_attempts >= 1; }),
              "partial-write failure is reached");
        check(!recorder.flush(200ms), "barrier reports controlled write failure");
        check(recorder.health().io_failed && !recorder.health().error.empty(),
              "partial-write failure becomes sticky visible health");
        check(recorder.record(EventKind::Input, Fields{}) == AppendResult::IoError,
              "new events report sticky write failure");
        check(!recorder.close("write_failed"),
              "flush and close report a prior write failure");
        const auto written = state->written_copy();
        const auto recovered = recover_journal(written);
        check(written.size() == 5 && recovered.valid_bytes == 0 && !recovered.complete(),
              "partial sink write leaves only an incomplete recoverable prefix");
    }
    {
        auto state = std::make_shared<MemoryState>();
        state->fail_write_attempt = 3; // File header and RunBegin succeed; Input is partial.
        state->partial_bytes = 7;
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options(1000ms));
        check(recorder.record(EventKind::Input, Fields{{"partial", true}}) == AppendResult::Accepted,
              "event is accepted before controlled partial frame failure");
        check(!recorder.flush(2000ms), "barrier reports partial event-frame failure");
        const auto written = state->written_copy();
        const auto recovered = recover_journal(written);
        const auto begin_end = fixture_header().size() + fixture_record(EventKind::RunBegin, 1, 100, "{}").size();
        check(written.size() == begin_end + 7 && recovered.issue == RecoveryIssue::Truncated &&
              recovered.valid_bytes == begin_end && recovered.records.size() == 1 &&
              recovered.records[0].kind == EventKind::RunBegin,
              "partial event write preserves a complete RunBegin prefix only");
        check(recorder.health().io_failed && recorder.health().written_records == 1 &&
              recorder.health().written_events == 0 && !recorder.close("partial_frame"),
              "partial frame failure leaves caller event unwritten and remains sticky");
    }
    {
        auto state = std::make_shared<MemoryState>();
        state->fail_flush_attempt = 1;
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options(1000ms));
        check(recorder.record(EventKind::Input, Fields{{"flush", true}}) == AppendResult::Accepted,
              "event accepted before controlled flush failure");
        check(!recorder.flush(2000ms), "failed sink flush fails the barrier");
        check(recorder.health().io_failed && recorder.health().flushed_records == 0,
              "failed flush stays visible and does not advance persisted count");
        check(state->durable_copy().empty(), "failed flush does not publish durable snapshot");
        check(recorder.record(EventKind::Input, Fields{}) == AppendResult::IoError,
              "new events report sticky flush failure");
        check(!recorder.close("flush_failed"), "close reports sticky flush failure");
    }
    {
        auto state = std::make_shared<MemoryState>();
        state->fail_finish_attempt = 1;
        SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, options());
        check(recorder.record(EventKind::Input, Fields{{"finish", true}}) == AppendResult::Accepted,
              "event accepted before controlled sink finish failure");
        check(!recorder.close("finish_failed"), "failed sink finish fails close");
        const auto health = recorder.health();
        check(health.closed && health.io_failed && !health.error.empty() &&
              health.flushed_records == 3,
              "finish failure remains visible after writer joins and final flush succeeds");
        check(recover_journal(state->durable_copy()).complete(),
              "complete framing does not override failed sink health");
        check(recorder.record(EventKind::Input, Fields{}) == AppendResult::Closed &&
              !recorder.close("ignored"), "failed recorder remains closed on later calls");
        {
            std::lock_guard lock(state->mutex);
            check(state->finish_attempts == 1, "idempotent close does not retry sink finish");
        }
    }
}
void test_options_and_concurrent_close() {
    for (const auto bad_interval : {0ms, 1001ms}) {
        check_throws<std::invalid_argument>([&] {
            auto config = options(bad_interval);
            SessionRecorder recorder(std::make_unique<MemorySink>(std::make_shared<MemoryState>()),
                                     Fields{}, config);
        }, "flush interval outside (0, 1 s] is rejected");
    }
    check_throws<std::invalid_argument>([] {
        auto config = options();
        config.max_queue_events = 0;
        SessionRecorder recorder(std::make_unique<MemorySink>(std::make_shared<MemoryState>()),
                                 Fields{}, config);
    }, "zero event capacity is rejected");

    auto state = std::make_shared<MemoryState>();
    auto config = options();
    config.max_queue_events = 1024;
    SessionRecorder recorder(std::make_unique<MemorySink>(state), Fields{}, config);
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool first_recorded = false;
    bool release_producer = false;
    std::atomic<unsigned> accepted{0}, closed{0}, unexpected{0};
    std::thread producer([&] {
        const auto first = recorder.record(EventKind::Input, Fields{{"ordinal", std::int64_t{0}}});
        if (first == AppendResult::Accepted) ++accepted;
        else ++unexpected;
        {
            std::lock_guard lock(gate_mutex);
            first_recorded = true;
        }
        gate.notify_all();
        {
            std::unique_lock lock(gate_mutex);
            (void)gate.wait_for(lock, 2000ms, [&] { return release_producer; });
        }
        for (std::int64_t ordinal = 1; ordinal < 300; ++ordinal) {
            const auto result = recorder.record(EventKind::Input, Fields{{"ordinal", ordinal}});
            if (result == AppendResult::Accepted) ++accepted;
            else if (result == AppendResult::Closed) ++closed;
            else ++unexpected;
        }
    });
    {
        std::unique_lock lock(gate_mutex);
        check(gate.wait_for(lock, 2000ms, [&] { return first_recorded; }),
              "producer signals first accepted event within deadline");
        release_producer = true;
    }
    gate.notify_all();
    check(recorder.flush(2000ms), "flush barrier completes while producer can append");
    check(recorder.close("concurrent_test"), "close completes while producer may append");
    producer.join();
    const auto recovered = recover_journal(state->durable_copy());
    const auto written_inputs = static_cast<unsigned>(std::count_if(
        recovered.records.begin(), recovered.records.end(), [](const auto &record) {
            return record.kind == EventKind::Input;
        }));
    check(recovered.complete() && accepted + closed == 300 && unexpected == 0,
          "finite producer race returns accepted or closed for every event");
    check(written_inputs == accepted && recorder.health().accepted_events == accepted &&
          recorder.health().written_events == accepted,
          "all accepted concurrent events appear once in recovered journal");
    for (std::size_t i = 0; i < recovered.records.size(); ++i)
        check(recovered.records[i].sequence == i + 1, "concurrent journal sequence remains contiguous");
}

int run_process_child(std::string_view mode, const std::filesystem::path &path) {
    try {
        SessionRecorder recorder(make_file_sink(path), Fields{{"role", std::string("process_fixture")}},
                                 options(20ms));
        const std::string text = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \"\\\n\t\x01";
        const Fields payload{{"text", text},
                             {"signed_min", std::numeric_limits<std::int64_t>::min()},
                             {"unsigned_max", std::numeric_limits<std::uint64_t>::max()},
                             {"ratio", 2.5}, {"enabled", true}, {"missing", nullptr}};
        if (recorder.record(EventKind::Input, payload) != AppendResult::Accepted ||
            !recorder.flush(2000ms)) {
            std::cerr << "Child could not record and flush input\n";
            return 3;
        }
        if (mode == "--child-normal") {
            if (!recorder.close("synthetic_normal", true)) {
                std::cerr << "Child could not close journal\n";
                return 4;
            }
            return 0;
        }
        if (mode == "--child-exit") std::_Exit(23);
        if (mode == "--child-wait") {
            auto ready_path = path;
            ready_path += ".ready";
            std::ofstream ready(ready_path, std::ios::binary | std::ios::trunc);
            ready << "ready\n";
            ready.close();
            if (!ready) {
                std::cerr << "Child could not publish ready marker\n";
                return 5;
            }
            std::this_thread::sleep_for(5s);
            std::_Exit(24);
        }
    } catch (const std::exception &error) {
        std::cerr << "Child setup failed: " << error.what() << '\n';
        return 6;
    }
    return 2;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 3) {
        const std::string_view mode(argv[1]);
        if (mode == "--child-normal" || mode == "--child-exit" || mode == "--child-wait")
            return run_process_child(mode, argv[2]);
    }
    if (argc != 1) {
        std::cerr << "Usage: session_recorder_tests [--child-normal|--child-exit|--child-wait PATH]\n";
        return 2;
    }
    try {
        test_framing_and_json();
        test_recovery();
        test_file_sink_and_reader();
        test_writer_order_and_close();
        test_rejection_and_destructor();
        test_invalid_close_reason();
        test_flush_scheduling();
        test_overflow();
        test_io_failures();
        test_options_and_concurrent_close();
    } catch (const std::exception &error) {
        std::cerr << "Test setup failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Session recorder failures: " << failures << '\n';
    return failures ? 1 : 0;
}
