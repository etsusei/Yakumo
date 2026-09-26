#include "testing/session_recorder.hpp"

#include <cerrno>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <condition_variable>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace mhp3rd::testing {
namespace {

class FileJournalSink final : public JournalSink {
public:
    explicit FileJournalSink(std::FILE *file) : file_(file) {}
    ~FileJournalSink() override {
        if (file_)
            std::fclose(file_);
    }

    bool write(std::span<const std::uint8_t> bytes) override {
        return std::fwrite(bytes.data(), 1, bytes.size(), file_) == bytes.size();
    }

    bool flush() override { return std::fflush(file_) == 0; }

    bool finish() override {
        if (!file_)
            return false;
        std::FILE *file = std::exchange(file_, nullptr);
        return std::fclose(file) == 0;
    }

private:
    std::FILE *file_;
};

Fields end_fields(const std::string &reason, bool completed,
                  std::uint64_t accepted, std::uint64_t written,
                  std::uint64_t dropped, std::uint64_t invalid) {
    return {{"stop_reason", reason},
            {"completed", completed},
            {"accepted_events", accepted},
            {"written_events", written},
            {"dropped_events", dropped},
            {"invalid_events", invalid}};
}

Fields loss_fields(std::uint64_t total, std::uint64_t since_last) {
    return {{"dropped_total", total}, {"dropped_since_last", since_last}};
}

bool prompt_flush_kind(EventKind kind) {
    return kind == EventKind::CaseBegin || kind == EventKind::CaseEnd ||
           kind == EventKind::Checkpoint || kind == EventKind::Anomaly ||
           kind == EventKind::Error;
}

std::uint64_t steady_nanoseconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace

std::unique_ptr<JournalSink> make_file_sink(const std::filesystem::path &path) {
#ifdef _WIN32
    const int fd = ::_wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL |
                           _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "create journal");

#ifdef _WIN32
    std::FILE *file = ::_fdopen(fd, "wb");
#else
    std::FILE *file = ::fdopen(fd, "wb");
#endif
    if (!file) {
        const int error = errno;
#ifdef _WIN32
        ::_close(fd);
#else
        ::close(fd);
#endif
        throw std::system_error(error, std::generic_category(), "open journal stream");
    }
    // Avoid a second, implicit buffered write when fclose runs after a failure.
    if (std::setvbuf(file, nullptr, _IONBF, 0) != 0) {
        const int error = errno ? errno : EIO;
        std::fclose(file);
        throw std::system_error(error, std::generic_category(), "configure journal stream");
    }
    return std::make_unique<FileJournalSink>(file);
}

struct SessionRecorder::Impl {
    struct QueuedEvent {
        EventKind kind;
        std::uint64_t monotonic_ns;
        std::string payload;
        std::size_t frame_bytes;
        bool boundary;
    };

    Impl(std::unique_ptr<JournalSink> new_sink, Fields run_fields,
         RecorderOptions new_options)
        : sink(std::move(new_sink)), options(std::move(new_options)) {
        if (!sink)
            throw std::invalid_argument("journal sink is null");
        if (options.max_queue_events == 0 || options.max_queue_bytes == 0 ||
            options.max_payload_bytes == 0 || options.max_payload_bytes > kMaxPayloadBytes ||
            options.flush_interval <= std::chrono::milliseconds::zero() ||
            options.flush_interval > std::chrono::milliseconds(1000))
            throw std::invalid_argument("invalid recorder options");

        // These records are generated without occupying caller queue capacity.
        // Reserve enough payload room even if counters reach their full width.
        constexpr auto max = std::numeric_limits<std::uint64_t>::max();
        const auto minimum_end = fields_json(end_fields("invalid_stop_reason", false,
                                                       max, max, max, max)).size();
        const auto minimum_loss = fields_json(loss_fields(max, max)).size();
        if (options.max_payload_bytes < minimum_end ||
            options.max_payload_bytes < minimum_loss)
            throw std::invalid_argument("payload limit cannot hold lifecycle records");

        begin_payload = fields_json(run_fields);
        if (begin_payload.size() > options.max_payload_bytes)
            throw std::invalid_argument("run metadata exceeds payload limit");
        if (!options.monotonic_ns)
            options.monotonic_ns = steady_nanoseconds;
        begin_ns = options.monotonic_ns();
        worker = std::thread([this] { run(); });
    }

    void fail(const char *message) {
        std::lock_guard lock(mutex);
        io_failed = true;
        error = message;
        queue.clear();
        queued_bytes = 0;
        cv.notify_all();
    }

    AppendResult reject_invalid() {
        std::lock_guard lock(mutex);
        if (close_started)
            return AppendResult::Closed;
        if (io_failed)
            return AppendResult::IoError;
        ++invalid_events;
        return AppendResult::Rejected;
    }

    void run() noexcept {
        // The sink, including its final close, lives exclusively on this thread.
        auto writer_sink = std::move(sink);
        try {
            const auto header = journal_header();
            if (!writer_sink->write(header)) {
                fail("journal header write failed");
                return;
            }
            if (!write_record(*writer_sink, EventKind::RunBegin, begin_ns,
                              begin_payload, false))
                return;

            auto last_flush = std::chrono::steady_clock::now();
            bool force_flush = false;
            for (;;) {
                enum class Action { Event, Loss, Flush, End } action;
                QueuedEvent event{};
                std::uint64_t loss_total = 0;
                std::uint64_t loss_since_last = 0;
                std::string end_payload;
                {
                    std::unique_lock lock(mutex);
                    for (;;) {
                        if (io_failed)
                            return;
                        const bool dirty = written_records > flushed_records;
                        const auto deadline = last_flush + options.flush_interval;
                        const bool periodic = dirty &&
                            std::chrono::steady_clock::now() >= deadline;
                        if (dirty && (force_flush || flush_waiters != 0 || periodic)) {
                            action = Action::Flush;
                            break;
                        }
                        if (dropped_events > written_loss_total &&
                            written_events >= loss_after_accepted) {
                            action = Action::Loss;
                            loss_total = dropped_events;
                            loss_since_last = loss_total - written_loss_total;
                            break;
                        }
                        if (!queue.empty()) {
                            action = Action::Event;
                            event = std::move(queue.front());
                            queue.pop_front();
                            queued_bytes -= event.frame_bytes;
                            break;
                        }
                        if (close_started) {
                            action = Action::End;
                            end_payload = fields_json(end_fields(stop_reason, completed,
                                accepted_events, written_events, dropped_events,
                                invalid_events));
                            break;
                        }
                        if (dirty)
                            cv.wait_until(lock, deadline);
                        else
                            cv.wait(lock);
                    }
                }

                if (action == Action::Flush) {
                    if (!writer_sink->flush()) {
                        fail("journal flush failed");
                        return;
                    }
                    {
                        std::lock_guard lock(mutex);
                        flushed_records = written_records;
                        flushed_events = written_events;
                        flushed_loss_total = written_loss_total;
                        cv.notify_all();
                    }
                    last_flush = std::chrono::steady_clock::now();
                    force_flush = false;
                } else if (action == Action::Event) {
                    if (!write_record(*writer_sink, event.kind, event.monotonic_ns,
                                      event.payload, true))
                        return;
                    force_flush = force_flush || event.boundary;
                } else if (action == Action::Loss) {
                    const auto payload = fields_json(loss_fields(loss_total, loss_since_last));
                    if (payload.size() > options.max_payload_bytes) {
                        fail("recording loss exceeds payload limit");
                        return;
                    }
                    if (!write_record(*writer_sink, EventKind::RecordingLoss,
                                      options.monotonic_ns(), payload, false))
                        return;
                    {
                        std::lock_guard lock(mutex);
                        written_loss_total = loss_total;
                        if (dropped_events > written_loss_total)
                            loss_after_accepted = accepted_events;
                        cv.notify_all();
                    }
                } else {
                    if (end_payload.size() > options.max_payload_bytes) {
                        fail("run end exceeds payload limit");
                        return;
                    }
                    if (!write_record(*writer_sink, EventKind::RunEnd,
                                      options.monotonic_ns(), end_payload, false))
                        return;
                    if (!writer_sink->flush()) {
                        fail("journal final flush failed");
                        return;
                    }
                    {
                        std::lock_guard lock(mutex);
                        flushed_records = written_records;
                        flushed_events = written_events;
                        flushed_loss_total = written_loss_total;
                        cv.notify_all();
                    }
                    if (!writer_sink->finish()) {
                        fail("journal finish failed");
                        return;
                    }
                    return;
                }
            }
        } catch (...) {
            fail("journal writer exception");
        }
    }

    bool write_record(JournalSink &writer_sink, EventKind kind, std::uint64_t ns,
                      const std::string &payload, bool caller_event) {
        std::uint64_t sequence;
        {
            std::lock_guard lock(mutex);
            if (io_failed)
                return false;
            sequence = written_records + 1;
        }
        const auto frame = encode_record({kind, sequence, ns, payload});
        if (!writer_sink.write(frame)) {
            fail("journal record write failed");
            return false;
        }
        {
            std::lock_guard lock(mutex);
            ++written_records;
            if (caller_event)
                ++written_events;
            cv.notify_all();
        }
        return true;
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::unique_ptr<JournalSink> sink;
    RecorderOptions options;
    std::thread worker;
    std::deque<QueuedEvent> queue;
    std::string begin_payload;
    std::string stop_reason;
    std::string error;
    std::uint64_t begin_ns{};
    std::uint64_t accepted_events{};
    std::uint64_t written_events{};
    std::uint64_t dropped_events{};
    std::uint64_t invalid_events{};
    std::uint64_t written_records{};
    std::uint64_t flushed_records{};
    std::uint64_t written_loss_total{};
    std::uint64_t flushed_loss_total{};
    std::uint64_t flushed_events{};
    std::uint64_t loss_after_accepted{};
    std::size_t queued_bytes{};
    std::size_t flush_waiters{};
    bool close_started{};
    bool close_finished{};
    bool completed{};
    bool io_failed{};
};

SessionRecorder::SessionRecorder(std::unique_ptr<JournalSink> sink, Fields run_fields,
                                 RecorderOptions options)
    : impl_(std::make_unique<Impl>(std::move(sink), std::move(run_fields),
                                   std::move(options))) {}

SessionRecorder::~SessionRecorder() {
    if (impl_)
        (void)close("recorder_destroyed", false);
}

AppendResult SessionRecorder::record(EventKind kind, Fields fields, bool boundary) {
    auto &impl = *impl_;
    {
        std::lock_guard lock(impl.mutex);
        if (impl.close_started)
            return AppendResult::Closed;
        if (impl.io_failed)
            return AppendResult::IoError;
    }
    if (!known_event_kind(kind) || kind == EventKind::RunBegin ||
        kind == EventKind::RunEnd || kind == EventKind::RecordingLoss)
        return impl.reject_invalid();

    std::string payload;
    try {
        payload = fields_json(fields);
    } catch (const std::invalid_argument &) {
        return impl.reject_invalid();
    } catch (const std::length_error &) {
        return impl.reject_invalid();
    }
    if (payload.size() > impl.options.max_payload_bytes)
        return impl.reject_invalid();
    const auto ns = impl.options.monotonic_ns();
    const std::size_t frame_bytes = kRecordHeaderBytes + payload.size();
    {
        std::lock_guard lock(impl.mutex);
        if (impl.close_started)
            return AppendResult::Closed;
        if (impl.io_failed)
            return AppendResult::IoError;
        if (impl.queue.size() >= impl.options.max_queue_events ||
            frame_bytes > impl.options.max_queue_bytes ||
            impl.queued_bytes > impl.options.max_queue_bytes - frame_bytes) {
            if (impl.dropped_events == impl.written_loss_total)
                impl.loss_after_accepted = impl.accepted_events;
            ++impl.dropped_events;
            impl.cv.notify_all();
            return AppendResult::Dropped;
        }
        impl.queue.push_back({kind, ns, std::move(payload), frame_bytes,
                              boundary || prompt_flush_kind(kind)});
        impl.queued_bytes += frame_bytes;
        ++impl.accepted_events;
        impl.cv.notify_all();
    }
    return AppendResult::Accepted;
}

bool SessionRecorder::flush(std::chrono::milliseconds timeout) {
    auto &impl = *impl_;
    std::unique_lock lock(impl.mutex);
    if (impl.io_failed)
        return false;
    const auto accepted = impl.accepted_events;
    const auto dropped = impl.dropped_events;
    const auto done = [&] {
        return impl.io_failed ||
            (impl.flushed_records >= 1 && impl.flushed_events >= accepted &&
             impl.flushed_loss_total >= dropped);
    };
    if (!done()) {
        ++impl.flush_waiters;
        impl.cv.notify_all();
        if (timeout > std::chrono::milliseconds::zero())
            impl.cv.wait_for(lock, timeout, done);
        --impl.flush_waiters;
    }
    return !impl.io_failed && done();
}

bool SessionRecorder::close(std::string stop_reason, bool completed) {
    auto &impl = *impl_;
    std::unique_lock lock(impl.mutex);
    if (impl.close_started) {
        impl.cv.wait(lock, [&] { return impl.close_finished; });
        return !impl.io_failed;
    }
    bool bad_reason = stop_reason.empty();
    if (!bad_reason) {
        try {
            // The lock freezes caller counters until close starts. A successful
            // drain writes every accepted event before RunEnd.
            bad_reason = fields_json(end_fields(stop_reason, completed,
                impl.accepted_events, impl.accepted_events, impl.dropped_events,
                impl.invalid_events)).size() > impl.options.max_payload_bytes;
        } catch (const std::invalid_argument &) {
            bad_reason = true;
        } catch (const std::length_error &) {
            bad_reason = true;
        }
    }
    if (bad_reason) {
        stop_reason = "invalid_stop_reason";
        completed = false;
    }
    if (bad_reason) {
        ++impl.invalid_events;
        if (impl.error.empty())
            impl.error = "invalid stop reason";
    }
    impl.stop_reason = std::move(stop_reason);
    impl.completed = completed;
    impl.close_started = true;
    impl.cv.notify_all();
    lock.unlock();
    if (impl.worker.joinable())
        impl.worker.join();
    lock.lock();
    impl.close_finished = true;
    impl.cv.notify_all();
    return !impl.io_failed;
}

RecorderHealth SessionRecorder::health() const {
    const auto &impl = *impl_;
    std::lock_guard lock(impl.mutex);
    return {impl.accepted_events, impl.written_events, impl.dropped_events,
            impl.invalid_events, impl.written_records, impl.flushed_records,
            impl.queue.size(), impl.queued_bytes, impl.close_finished,
            impl.io_failed, impl.error};
}

} // namespace mhp3rd::testing
