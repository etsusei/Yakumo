#include "native/bridge_contracts.hpp"
#include "native/angle_step_bridge.hpp"
#include "native/scale_matrix.hpp"
#include "native/translation_matrix.hpp"
#include "native/vector_construct.hpp"
#include "native/matrix_copy.hpp"
#include "testing/probes.hpp"
#include "testing/game_observers.hpp"
#include "psprecomp/elf32.hpp"
#include "recomp_units.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace psprecomp;
using namespace mhp3rd::testing;
constexpr std::array<std::uint32_t, 5> entries{0x088775AC, 0x08878B28, 0x08878B4C, 0x08877818, 0x08879D08};
constexpr std::array<std::uint32_t, 5> returns{0x48, 0x1C, 0x1C, 0x10, 0x48};
constexpr std::uint32_t scratch = 0x08010000, return_pc = 0x08020000;
int failures{};
void check(bool value, const char *why) {
    if (!value && ++failures <= 30) std::cerr << "FAIL: " << why << '\n';
}
class Sink final : public JournalSink {
public:
    explicit Sink(std::shared_ptr<std::vector<std::uint8_t>> bytes) : bytes_(std::move(bytes)) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        bytes_->insert(bytes_->end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
private:
    std::shared_ptr<std::vector<std::uint8_t>> bytes_;
};
struct Recording {
    std::shared_ptr<std::vector<std::uint8_t>> bytes = std::make_shared<std::vector<std::uint8_t>>();
    std::shared_ptr<SessionRecorder> recorder = std::make_shared<SessionRecorder>(
        std::make_unique<Sink>(bytes), Fields{{"role", std::string("offline_aot")}});
    std::shared_ptr<GameObserver> observer = std::make_shared<GameObserver>(recorder);
    Recording() { configure_native_probes(observer, kProbeAll); }
    ~Recording() { flush_native_probes(true); }
    JournalRecovery finish() {
        flush_native_probes(true);
        check(recorder->close("offline_done"), "recorder closes");
        auto recovered = recover_journal(*bytes);
        check(recovered.complete() && !recovered.loss_seen, "complete journal without lost events");
        return recovered;
    }
};
std::uint64_t number(const std::string &payload, std::string_view key) {
    const auto marker = "\"" + std::string(key) + "\":";
    const auto at = payload.find(marker);
    if (at == std::string::npos) throw std::runtime_error("missing probe field " + std::string(key));
    return std::stoull(payload.substr(at + marker.size()));
}
std::string summary(const JournalRecovery &journal, std::uint32_t entry) {
    for (const auto &record : journal.records) {
        if (record.kind == EventKind::Probe && record.payload.find("\"probe.summary\"") != std::string::npos &&
                number(record.payload, "entry") == entry) return record.payload;
    }
    throw std::runtime_error("missing probe summary");
}
AllegrexContext context(std::mt19937 &random, std::uint32_t entry) {
    AllegrexContext ctx;
    for (auto &v : ctx.gpr) v = random();
    for (auto &v : ctx.fpr) v = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    for (auto &v : ctx.vfpu) v = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    ctx.gpr[0] = 0;
    ctx.gpr[4] = scratch + 32;
    ctx.gpr[5] = scratch + 112;
    ctx.gpr[6] = 127;
    ctx.gpr[31] = return_pc;
    ctx.vfpu_ctrl[0] = ctx.vfpu_ctrl[1] = 0xE4;
    ctx.vfpu_ctrl[2] = 0;
    ctx.pc = entry;
    return ctx;
}
bool original(Runtime &runtime, AllegrexContext &ctx, std::size_t leaf) {
    for (unsigned step = 0; step < 40; ++step) {
        const bool returning = ctx.pc == entries[leaf] + returns[leaf] ||
            (leaf == 0 && ctx.pc == entries[leaf] + 0x5C);
        const auto exit = interpret_allegrex(runtime, ctx, 1);
        if (exit == InterpreterExit::Stopped || exit == InterpreterExit::Unreachable) return false;
        if (returning) return ctx.pc == ctx.gpr[31];
    }
    return false;
}
void compare_leaves(Runtime &oracle, Runtime &aot) {
    std::mt19937 random(0x50524F42);
    constexpr unsigned samples = 128;
    // Compute the independent interpreter result, then compare disabled and
    // enabled production AOT execution on exactly the same CPU and RAM input.
    struct Sample {
        AllegrexContext initial, expected;
        std::array<std::uint8_t, 192> input, output;
        std::size_t leaf;
    };
    std::vector<Sample> cases;
    for (std::size_t leaf = 0; leaf < entries.size(); ++leaf) {
        for (unsigned n = 0; n < samples; ++n) {
            Sample sample{};
            sample.leaf = leaf;
            sample.initial = context(random, entries[leaf]);
            for (auto &byte : sample.input) byte = static_cast<std::uint8_t>(random());
            if (leaf == 0) {
                // Explicitly cover both return labels, not random coverage.
                sample.input[32] = 0; sample.input[33] = 0;
                sample.input[34] = 0; sample.input[35] = 0;
                sample.input[112] = 64; sample.input[113] = n % 2 ? 0xFF : 0;
            }
            if (n % 3 == 0) sample.initial.gpr[4] |= 0x40000000;
            oracle.memory().copy_in(scratch, sample.input);
            sample.expected = sample.initial;
            check(original(oracle, sample.expected, leaf), "interpreter leaf returns within bound");
            oracle.memory().copy_out(scratch, sample.output);
            cases.push_back(sample);
        }
    }
    for (const bool enabled : {false, true}) {
        std::unique_ptr<Recording> recording;
        if (enabled) recording = std::make_unique<Recording>();
        for (const auto &sample : cases) {
            aot.memory().copy_in(scratch, sample.input);
            auto actual = sample.initial;
            check(aot.invoke_isolated_aot(entries[sample.leaf], actual), "production AOT leaf invoked");
            std::array<std::uint8_t, 192> output{};
            aot.memory().copy_out(scratch, output);
            check(mhp3rd::native::same_context(sample.expected, actual), "all CPU state matches independent interpreter");
            check(sample.output == output, "output and memory canaries match independent interpreter");
        }
        if (recording) {
            auto journal = recording->finish();
            for (auto entry : entries) {
                auto row = summary(journal, entry);
                check(number(row, "entry_hits") == samples, "one entry hit per production call");
                check(number(row, "aot_calls") == samples, "one complete AOT scope per production call");
                check(number(row, "incomplete") == 0 && number(row, "uncertified_entries") == 0,
                      "all ordinary scopes certified and complete");
            }
        }
    }
    std::cout << "AOT differential calls=" << cases.size() * 2 << " (disabled and enabled)\n";
}
void local_dispatch(Runtime &aot) {
    std::mt19937 random(7);
    auto ctx = context(random, entries[0]);
    ctx.gpr[31] = entries[3];
    std::array<std::uint8_t, 192> input{};
    input[112] = 64;
    aot.memory().copy_in(scratch, input);
    auto expected = ctx;
    check(aot.invoke_isolated_aot(entries[0], expected), "disabled same-unit chain executes");
    std::array<std::uint8_t, 192> output{};
    aot.memory().copy_out(scratch, output);
    aot.memory().copy_in(scratch, input);
    Recording recording;
    check(aot.invoke_isolated_aot(entries[0], ctx), "observed same-unit chain executes");
    std::array<std::uint8_t, 192> actual{};
    aot.memory().copy_out(scratch, actual);
    check(mhp3rd::native::same_context(expected, ctx) && output == actual, "local-dispatch behavior unchanged");
    const auto journal = recording.finish();
    check(number(summary(journal, entries[0]), "aot_calls") == 1, "angle scope ends before dispatching vector");
    check(number(summary(journal, entries[3]), "aot_calls") == 2047, "each locally dispatched vector has its own scope");
    check(number(summary(journal, entries[1]), "entry_hits") == 0, "unreached leaf remains zero coverage");
}
void direct_chain(Runtime &oracle, Runtime &aot) {
    // This harness targets the registered supported corpus. Exercise its actual
    // compile-time entry path with a stale PC, as generated callers do.
    std::mt19937 random(8);
    auto expected = context(random, entries[3]);
    auto actual = expected;
    actual.pc = 0x08812344;
    std::array<std::uint8_t, 192> input{};
    oracle.memory().copy_in(scratch, input);
    aot.memory().copy_in(scratch, input);
    check(original(oracle, expected, 3), "direct-chain oracle returns");
    Recording recording;
    auto view = aot.memory().aot_fast_view();
    check((aot.invoke_chained_direct<&psprecomp::recomp_unit_0028_entry, 28,
           (0x08877818 - 0x08874000) / 4 + 1, 0x08877818>(actual, &view)),
          "compiled direct chain reaches generated vector entry");
    std::array<std::uint8_t, 192> want{}, got{};
    oracle.memory().copy_out(scratch, want);
    aot.memory().copy_out(scratch, got);
    check(mhp3rd::native::same_context(expected, actual) && want == got,
          "observational direct chain preserves full CPU and memory result");
    const auto row = summary(recording.finish(), entries[3]);
    check(number(row, "aot_calls") == 1 && number(row, "incomplete") == 0,
          "stale PC does not lose or misattribute direct-chain scope");
}
void guard_and_interruption(Runtime &aot) {
    std::mt19937 random(9);
    {
        Recording recording;
        auto ctx = context(random, entries[3]);
        const auto delay = aot.memory().load32(entries[3] + 20);
        aot.memory().store32(entries[3] + 20, delay ^ 1);
        check(aot.invoke_isolated_aot(entries[3], ctx), "observation refusal does not change AOT execution");
        aot.memory().store32(entries[3] + 20, delay);
        auto row = summary(recording.finish(), entries[3]);
        check(number(row, "uncertified_entries") == 1 && number(row, "aot_calls") == 0,
              "changed delay-slot bytes refuse certification");
    }
    {
        Recording recording;
        auto ctx = context(random, entries[3]);
        ctx.gpr[4] = 0;
        bool threw{};
        try { (void)aot.invoke_isolated_aot(entries[3], ctx); } catch (const std::exception &) { threw = true; }
        check(threw, "original invalid-memory exception propagates");
        const auto row = summary(recording.finish(), entries[3]);
        check(number(row, "incomplete") == 1 && number(row, "aot_calls") == 0,
              "exceptional AOT exit remains incomplete");
    }
}
void overhead(Runtime &aot) {
    constexpr unsigned calls = 10000;
    std::mt19937 random(11);
    auto ctx = context(random, entries[3]);
    for (const bool enabled : {false, true}) {
        std::unique_ptr<Recording> recording;
        if (enabled) recording = std::make_unique<Recording>();
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < calls; ++i) (void)aot.invoke_isolated_aot(entries[3], ctx);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        if (recording) (void)recording->finish();
        std::cout << "probe_microbenchmark enabled=" << enabled << " calls=" << calls
                  << " host_total_ns=" << ns << " scope=isolated_vector_not_game_performance\n";
    }
}
void native_paths(Runtime &oracle, Runtime &candidate) {
    using namespace mhp3rd::native;
    using Install = bool (*)(Runtime &, NativeMode);
    constexpr std::array<Install, 5> installers{install_angle_step, install_scale_matrix,
        install_translation_matrix, install_vector_construct, install_matrix_copy};
    std::mt19937 random(13);
    for (const auto mode : {NativeMode::Native, NativeMode::Verify}) {
        Recording recording;
        for (std::size_t leaf = 0; leaf < entries.size(); ++leaf) {
            check(installers[leaf](candidate, mode), "native bridge installs against supported bytes");
            std::array<std::uint8_t, 192> input{};
            for (auto &byte : input) byte = static_cast<std::uint8_t>(random());
            oracle.memory().copy_in(scratch, input);
            candidate.memory().copy_in(scratch, input);
            auto expected = context(random, entries[leaf]);
            auto actual = expected;
            check(original(oracle, expected, leaf), "bridge oracle returns");
            check(candidate.invoke_isolated_aot(entries[leaf], actual), "recorded native bridge executes");
            std::array<std::uint8_t, 192> want{}, got{};
            oracle.memory().copy_out(scratch, want);
            candidate.memory().copy_out(scratch, got);
            check(same_context(expected, actual) && want == got, "observed bridge retains original CPU and memory results");
        }
        const auto journal = recording.finish();
        for (auto entry : entries) {
            const auto row = summary(journal, entry);
            check(number(row, mode == NativeMode::Native ? "native_calls" : "verify_calls") == 1,
                  "bridge records actual variant");
            check(number(row, "aot_calls") == 0 && number(row, "incomplete") == 0,
                  "bridge does not pretend interpreter verification is original AOT timing");
        }
    }
    // An unusual VFPU source prefix invokes the reference from native mode.
    Recording recording;
    check(install_scale_matrix(candidate, NativeMode::Native), "fallback bridge installs");
    auto expected = context(random, entries[1]);
    expected.vfpu_ctrl[0] = 0;
    auto actual = expected;
    std::array<std::uint8_t, 192> input{};
    oracle.memory().copy_in(scratch, input);
    candidate.memory().copy_in(scratch, input);
    check(original(oracle, expected, 1), "fallback oracle returns");
    check(candidate.invoke_isolated_aot(entries[1], actual), "fallback bridge executes");
    std::array<std::uint8_t, 192> want{}, got{};
    oracle.memory().copy_out(scratch, want);
    candidate.memory().copy_out(scratch, got);
    check(same_context(expected, actual) && want == got, "observed fallback preserves reference result");
    const auto row = summary(recording.finish(), entries[1]);
    check(number(row, "fallback_calls") == 1 && number(row, "native_calls") == 0,
          "actual fallback is distinct from requested native mode");
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 2) { std::cerr << "Usage: mhp3rd_aot_probe_tests <local-supported-ELF>\n"; return 2; }
    try {
        const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
        psprecomp::Runtime oracle(elf.required_ram_size()), aot(elf.required_ram_size());
        (void)elf.load_and_relocate(oracle.memory());
        (void)elf.load_and_relocate(aot.memory());
        psprecomp::register_generated_functions(aot);
        compare_leaves(oracle, aot);
        direct_chain(oracle, aot);
        local_dispatch(aot);
        guard_and_interruption(aot);
        overhead(aot);
        native_paths(oracle, aot);
    } catch (const std::exception &error) {
        ++failures;
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    std::cout << "AOT probe failures=" << failures << '\n';
    return failures ? 1 : 0;
}
