#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}

namespace mhp3rd::testing {
class GameObserver;
class NativeProbeSession;

enum class ProbeVariant : std::uint8_t { Aot, Native, Verify, Fallback };
inline constexpr std::uint32_t kProbeAngle = 1u << 0u;
inline constexpr std::uint32_t kProbeScale = 1u << 1u;
inline constexpr std::uint32_t kProbeTranslation = 1u << 2u;
inline constexpr std::uint32_t kProbeVector = 1u << 3u;
inline constexpr std::uint32_t kProbeCopy = 1u << 4u;
inline constexpr std::uint32_t kProbeAll = kProbeAngle | kProbeScale | kProbeTranslation |
                                            kProbeVector | kProbeCopy;

struct ProbeVariantStats {
    std::uint64_t calls{};
    std::uint64_t total_ns{};
    std::uint64_t max_ns{};
};

struct ProbeLeafStats {
    std::uint32_t entry{};
    std::uint64_t entry_hits{};
    std::uint64_t certified_entries{};
    std::uint64_t uncertified_entries{};
    std::uint64_t uncertified_returns{};
    std::uint64_t completed{};
    std::uint64_t incomplete{};
    std::uint64_t incomplete_abnormal{};
    std::uint64_t incomplete_mismatch{};
    std::uint64_t incomplete_failure{};
    std::uint64_t incomplete_overflow{};
    std::uint64_t orphan_exits{};
    std::uint64_t return_mismatches{};
    std::array<ProbeVariantStats, 4> variants{};
};

enum class ProbeOutcome : std::uint8_t {
    Completed, UncertifiedReturn, Abnormal, Mismatch, Failure,
    Overflow, ReturnMismatch, OrphanExit,
};
struct ProbeDetail {
    std::uint64_t sequence{};
    std::uint64_t token{};
    std::uint32_t entry{};
    ProbeVariant variant{ProbeVariant::Aot};
    ProbeOutcome outcome{ProbeOutcome::Completed};
    std::uint64_t start_ns{};
    std::uint64_t duration_ns{};
    bool variant_known{};
    bool start_known{};
    bool duration_known{};
    bool certified{};
};

struct ProbeSnapshot {
    static constexpr std::size_t kRecentCapacity = 32;
    std::uint32_t mask{};
    bool final{};
    std::array<ProbeLeafStats, 5> leaves{};
    std::array<ProbeDetail, kRecentCapacity> recent{};
    std::size_t recent_count{};
};

// A fixed-capacity, allocation-free accounting core. The caller supplies the
// fingerprint result before enter(), so certification work is outside the
// reported leaf duration. Clock injection permits deterministic offline tests.
class ProbeTracker {
public:
    using Clock = std::uint64_t (*)(void *) noexcept;
    static constexpr std::size_t kMaxThreads = 32;
    static constexpr std::size_t kMaxDepth = 64;

    explicit ProbeTracker(std::uint32_t mask, Clock clock = nullptr, void *clock_context = nullptr) noexcept;
    ProbeTracker(const ProbeTracker &) = delete;
    ProbeTracker &operator=(const ProbeTracker &) = delete;

    [[nodiscard]] std::uint64_t enter(const void *runtime, const void *context,
                                      std::uint32_t entry, bool certified,
                                      bool aot_path = false) noexcept;
    void exit_aot(std::uint64_t token, const void *runtime, const void *context,
                  std::uint32_t entry, std::uint32_t jump_target,
                  std::uint32_t expected_return) noexcept;
    void finish(std::uint64_t token, const void *runtime, const void *context,
                std::uint32_t entry, ProbeVariant variant, bool success = true) noexcept;
    void abandon(std::uint64_t token, const void *runtime, const void *context,
                 std::uint32_t entry) noexcept;
    void orphan_exit(std::uint32_t entry) noexcept;
    void overflow_entry(std::uint32_t entry, bool certified) noexcept;
    void invalidate_current_thread() noexcept;
    void reject(std::uint64_t token, const void *runtime, const void *context,
                std::uint32_t entry) noexcept;
    [[nodiscard]] ProbeSnapshot flush(bool final = false) noexcept;

private:
    struct Frame {
        const void *runtime{};
        const void *context{};
        std::uint32_t entry{};
        std::uint64_t token{};
        std::uint64_t start_ns{};
        bool certified{};
        bool aot_path{};
    };
    struct ThreadSlot {
        std::thread::id id{};
        std::size_t depth{};
        std::array<Frame, kMaxDepth> frames{};
    };
    [[nodiscard]] ThreadSlot *slot_for(std::thread::id id, bool create) noexcept;
    [[nodiscard]] static int leaf_index(std::uint32_t entry) noexcept;
    void incomplete(Frame frame, std::uint64_t ProbeLeafStats::*reason,
                    ProbeOutcome outcome, ProbeVariant variant = ProbeVariant::Aot,
                    bool variant_known = false) noexcept;
    void complete(Frame frame, ProbeVariant variant, std::uint64_t end_ns) noexcept;
    void mismatch(ThreadSlot &slot, std::uint32_t exit_entry) noexcept;
    void detail(ProbeDetail value) noexcept;

    std::mutex mutex_;
    std::uint32_t mask_{};
    Clock clock_{};
    void *clock_context_{};
    std::uint64_t next_token_{1};
    bool final_{};
    std::array<ThreadSlot, kMaxThreads> threads_{};
    std::array<ProbeLeafStats, 5> leaves_{};
    std::array<ProbeDetail, ProbeSnapshot::kRecentCapacity> recent_{};
    std::size_t recent_next_{};
    std::size_t recent_count_{};
    std::uint64_t detail_sequence_{1};
};

// Selection is local to one recording session; a zero mask is the cheap path.
// The caller must enable this only after accepting the supported ELF identity.
void configure_native_probes(std::shared_ptr<GameObserver> observer, std::uint32_t mask) noexcept;
[[nodiscard]] std::uint32_t selected_native_probes() noexcept;
// final=true detaches the session and marks outstanding scopes incomplete.
void flush_native_probes(bool final = false, std::string_view boundary = {}) noexcept;
void flush_native_probe_detail() noexcept;

// These names and signatures are consumed by instrument_probes.py.
void native_probe_aot_enter(psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
                            std::uint32_t entry) noexcept;
void native_probe_aot_exit(psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
                           std::uint32_t entry, std::uint32_t jump_target) noexcept;
// Called only after a native prediction and bounded original-code result differ.
void native_probe_verification_mismatch(psprecomp::Runtime &runtime,
                                        const psprecomp::AllegrexContext &context,
                                        std::uint32_t entry) noexcept;

class NativeProbeScope {
public:
    NativeProbeScope(psprecomp::Runtime &runtime, const psprecomp::AllegrexContext &context,
                     std::uint32_t entry) noexcept;
    ~NativeProbeScope();
    NativeProbeScope(const NativeProbeScope &) = delete;
    NativeProbeScope &operator=(const NativeProbeScope &) = delete;
    void finish(ProbeVariant variant, bool success = true) noexcept;
private:
    std::shared_ptr<NativeProbeSession> session_;
    const void *runtime_{};
    const void *context_{};
    std::uint32_t entry_{};
    std::uint64_t token_{};
    std::int32_t guest_thread_uid_{-1};
    std::uint64_t guest_thread_generation_{};
};
} // namespace mhp3rd::testing
