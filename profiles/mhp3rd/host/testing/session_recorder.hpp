#pragma once

#include "testing/journal.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace mhp3rd::testing {
// All sink operations occur on the writer thread. A failed write may have
// written a partial frame; recovery must stop at the valid prefix.
class JournalSink {
public:
    virtual ~JournalSink() = default;
    virtual bool write(std::span<const std::uint8_t> bytes) = 0;
    virtual bool flush() = 0;
    // Called once by the writer after the final successful flush. The default
    // keeps injected sinks that have no separate close operation compatible.
    virtual bool finish() { return true; }
};
// Exclusive creation. Existing files, including symlinks, must not be replaced.
[[nodiscard]] std::unique_ptr<JournalSink> make_file_sink(const std::filesystem::path &path);

struct RecorderOptions {
    std::size_t max_queue_events{1024};
    std::size_t max_queue_bytes{4u * 1024u * 1024u};
    std::size_t max_payload_bytes{kMaxPayloadBytes};
    // Must be positive and at most one second. This flushes to the OS, not a
    // power-loss guarantee. Slow/blocking filesystem calls can delay progress.
    std::chrono::milliseconds flush_interval{1000};
    std::function<std::uint64_t()> monotonic_ns;
};
enum class AppendResult { Accepted, Dropped, Rejected, Closed, IoError };
struct RecorderHealth {
    std::uint64_t accepted_events{}; // caller events, excluding run/loss records
    std::uint64_t written_events{};
    std::uint64_t dropped_events{};  // queue overflow, not successful I/O
    std::uint64_t invalid_events{};
    std::uint64_t written_records{};
    std::uint64_t flushed_records{};
    std::size_t queued_events{};
    std::size_t queued_bytes{};
    bool closed{}; // true after the writer has joined
    bool io_failed{};
    std::string error;
};
class SessionRecorder {
public:
    // Writes a generated RunBegin before any caller event. Metadata comes from
    // the later manifest/launcher layer; core tests use synthetic fields only.
    SessionRecorder(std::unique_ptr<JournalSink> sink, Fields run_fields, RecorderOptions options = {});
    ~SessionRecorder();
    SessionRecorder(const SessionRecorder &) = delete;
    SessionRecorder &operator=(const SessionRecorder &) = delete;
    // RunBegin, RunEnd and RecordingLoss are reserved for the recorder.
    // Case/checkpoint/anomaly boundaries request prompt background flushing.
    [[nodiscard]] AppendResult record(EventKind kind, Fields fields, bool boundary = false);
    // A barrier for records accepted before this call, including known loss.
    [[nodiscard]] bool flush(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
    // Drains accepted events, emits any loss and RunEnd with stop_reason,
    // completed and counters, then flushes. Idempotent. Destructor uses
    // completed=false and stop_reason=recorder_destroyed if not closed explicitly.
    [[nodiscard]] bool close(std::string stop_reason, bool completed = true);
    [[nodiscard]] RecorderHealth health() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mhp3rd::testing
