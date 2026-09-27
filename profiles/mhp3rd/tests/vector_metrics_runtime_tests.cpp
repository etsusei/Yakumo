#include "native/bridge_contracts.hpp"
#include "native/vector_metrics_runtime.hpp"

#include "testing/game_observers.hpp"
#include "testing/probes.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/sha256.hpp"
#include "recomp_units.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// This executable runs bounded calls into the production generated unit. It
// never starts the game, and it accepts only the registered local ELF image.
namespace {
using namespace mhp3rd::native;
using namespace mhp3rd::testing;
using namespace psprecomp;

constexpr char supported_elf_hash[] =
    "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";
constexpr std::uint32_t data_address = 0x08010000u;
constexpr std::uint32_t external_return = 0x08020000u;
constexpr std::uint32_t same_unit_caller = 0x08877610u;
constexpr char same_unit_caller_hash[] =
    "88228481838b5eded0af56d387b06d33b275cc849e60c6a86da6c54d94751cd6";
constexpr std::size_t window_size = 512u;
constexpr std::uint32_t metric_probe_mask = kProbeNorm | kProbeNormSquared |
    kProbeDistance | kProbeDistanceSquared;
using Window = std::array<std::uint8_t, window_size>;

void require(bool condition, const std::string &reason) {
    if (!condition) throw std::runtime_error(reason);
}

void write32(Window &window, std::size_t offset, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4u; ++byte)
        window.at(offset + byte) = static_cast<std::uint8_t>(value >> (8u * byte));
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
        std::make_unique<Sink>(bytes), Fields{{"role", std::string("vector_metric_runtime_test")}});
    std::shared_ptr<GameObserver> observer = std::make_shared<GameObserver>(recorder);
    Recording() { configure_native_probes(observer, metric_probe_mask); }
    ~Recording() { flush_native_probes(true); }
    JournalRecovery finish() {
        flush_native_probes(true);
        require(recorder->close("offline_done"), "recorder did not close");
        auto journal = recover_journal(*bytes);
        require(journal.complete() && !journal.loss_seen, "probe journal is incomplete or lost records");
        return journal;
    }
};

std::uint64_t number(const std::string &payload, std::string_view key) {
    const auto marker = "\"" + std::string(key) + "\":";
    const auto at = payload.find(marker);
    require(at != std::string::npos, "missing probe field " + std::string(key));
    return std::stoull(payload.substr(at + marker.size()));
}

const std::string &summary(const JournalRecovery &journal, std::uint32_t entry) {
    const std::string *found = nullptr;
    for (const auto &record : journal.records) {
        if (record.kind != EventKind::Probe ||
            record.payload.find("\"probe.summary\"") == std::string::npos ||
            number(record.payload, "entry") != entry) continue;
        require(!found, "duplicate probe summary for " + std::to_string(entry));
        found = &record.payload;
    }
    require(found, "missing probe summary for " + std::to_string(entry));
    return *found;
}

void check_probe(const std::string &row, unsigned calls, unsigned aot,
                 unsigned verify, unsigned native, unsigned fallback,
                 const std::string &label) {
    const auto equal = [&](std::string_view key, std::uint64_t expected) {
        require(number(row, key) == expected,
                label + ": " + std::string(key) + " expected " + std::to_string(expected) +
                ", got " + std::to_string(number(row, key)));
    };
    equal("entry_hits", calls);
    equal("certified_entries", calls);
    equal("completed", calls);
    equal("aot_calls", aot);
    equal("verify_calls", verify);
    equal("native_calls", native);
    equal("fallback_calls", fallback);
    for (const auto key : {"uncertified_entries", "uncertified_returns", "incomplete",
                           "orphan_exits", "return_mismatches"}) equal(key, 0u);
}

struct Fixture {
    AllegrexContext cpu{};
    Window before{};
};

Fixture fixture(const VectorMetricLeaf &leaf, unsigned variant) {
    std::mt19937 random(0x56454333u ^ leaf.entry ^ (variant * 0x9e3779b9u));
    Fixture sample;
    for (auto &reg : sample.cpu.gpr) reg = random();
    for (auto &reg : sample.cpu.fpr) reg = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    for (auto &reg : sample.cpu.vfpu) reg = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
    for (auto &reg : sample.cpu.vfpu_ctrl) reg = random();
    for (auto &byte : sample.before) byte = static_cast<std::uint8_t>(random());
    sample.cpu.gpr[0] = 0u;
    sample.cpu.gpr[4] = data_address + 64u;
    sample.cpu.gpr[5] = data_address + 128u;
    sample.cpu.gpr[29] = data_address + 352u; // single scratch word at +336
    sample.cpu.gpr[31] = external_return;
    sample.cpu.hi = random();
    sample.cpu.lo = random();
    sample.cpu.fcr31 = random();
    sample.cpu.pc = leaf.entry;
    sample.cpu.vfpu_ctrl[0] = variant == 2u ? 0x1e4u : 0xe4u;
    sample.cpu.vfpu_ctrl[1] = 0xe4u;
    sample.cpu.vfpu_ctrl[2] = 0u;
    const std::array<std::uint32_t, 4> first = variant == 1u ?
        std::array<std::uint32_t, 4>{0xbf800000u, 0x3f000000u, 0x3e800000u, 0x7fc01234u} :
        std::array<std::uint32_t, 4>{0x3f800000u, 0x40000000u, 0x40400000u, 0x7fa05678u};
    constexpr std::array<std::uint32_t, 4> second{
        0x40800000u, 0x40a00000u, 0x40c00000u, 0x7fc05678u};
    for (unsigned lane = 0; lane < 4u; ++lane) {
        write32(sample.before, 64u + 4u * lane, first[lane]);
        write32(sample.before, 128u + 4u * lane, second[lane]);
    }
    return sample;
}

struct Expected {
    AllegrexContext cpu{};
    Window after{};
};

Expected original_result(Runtime &reference, const Fixture &sample) {
    reference.memory().copy_in(data_address, sample.before);
    Expected result{sample.cpu, {}};
    const auto return_address = result.cpu.gpr[31];
    result.cpu.gpr[31] = 0u;
    psprecomp::recomp_unit_0028(reference, result.cpu);
    require(!reference.stopped() && result.cpu.pc == 0u,
            "production original did not return at its bounded sentinel");
    result.cpu.gpr[31] = return_address;
    result.cpu.pc = return_address;
    reference.memory().copy_out(data_address, result.after);
    return result;
}

void compare(const Expected &expected, const AllegrexContext &actual,
             const Window &memory, const std::string &label) {
    require(same_context(expected.cpu, actual), label + ": full CPU state differs from production AOT");
    require(expected.after == memory, label + ": 512-byte memory window differs from production AOT");
}

void call_registered(Runtime &candidate, const Fixture &sample, const Expected &expected,
                     const std::string &label) {
    candidate.memory().copy_in(data_address, sample.before);
    auto cpu = sample.cpu;
    require(candidate.invoke_isolated_aot(cpu.pc, cpu), label + ": registered dispatch missing");
    require(!candidate.stopped(), label + ": registered dispatch stopped the runtime");
    Window after{};
    candidate.memory().copy_out(data_address, after);
    compare(expected, cpu, after, label);
}

void load(Runtime &runtime, const Elf32Image &elf) {
    (void)elf.load_and_relocate(runtime.memory());
    register_generated_functions(runtime);
}

void guard_same_unit_caller(const Runtime &runtime) {
    // 0x08877610 through the return delay slot at 0x08877654. This SHA-256
    // was derived from the registered local ELF; no game bytes are embedded.
    std::array<std::uint8_t, 72> code{};
    runtime.memory().copy_out(same_unit_caller, code);
    require(sha256_bytes(code) == same_unit_caller_hash,
            "same-unit caller full-span fingerprint is unknown");
}

void mode_matrix(const Elf32Image &elf, Runtime &reference) {
    constexpr std::array<NativeMode, 3> modes{NativeMode::Off, NativeMode::Verify, NativeMode::Native};
    for (const auto mode : modes) {
        Runtime candidate(elf.required_ram_size());
        load(candidate, elf);
        VectorMetricRuntime owner(candidate, elf, &psprecomp::recomp_unit_0028);
        require(owner.install({mode, mode, mode, mode}), "runtime rejected certified mode install");
        require(!owner.install({mode, mode, mode, mode}), "runtime accepted a second startup install");
        std::array<std::array<Expected, 3>, 4> expected{};
        std::array<std::array<Fixture, 3>, 4> samples{};
        for (std::size_t i = 0; i < kVectorMetricLeaves.size(); ++i)
            for (unsigned variant = 0; variant < 3u; ++variant) {
                samples[i][variant] = fixture(kVectorMetricLeaves[i], variant);
                expected[i][variant] = original_result(reference, samples[i][variant]);
            }
        Recording recording;
        for (std::size_t i = 0; i < kVectorMetricLeaves.size(); ++i)
            for (unsigned variant = 0; variant < 3u; ++variant)
                call_registered(candidate, samples[i][variant], expected[i][variant],
                    std::to_string(kVectorMetricLeaves[i].entry) + "/" + std::to_string(variant));
        const auto journal = recording.finish();
        for (std::size_t i = 0; i < kVectorMetricLeaves.size(); ++i) {
            const auto entry = kVectorMetricLeaves[i].entry;
            const auto label = std::string("mode=") + std::to_string(static_cast<int>(mode)) +
                "/entry=" + std::to_string(entry);
            if (mode == NativeMode::Off)
                check_probe(summary(journal, entry), 3u, 3u, 0u, 0u, 0u, label);
            else if (mode == NativeMode::Verify)
                check_probe(summary(journal, entry), 3u, 0u, 2u, 0u, 1u, label);
            else
                check_probe(summary(journal, entry), 3u, 0u, 0u, 2u, 1u, label);
            const auto stats = owner.stats()[i];
            require(stats.calls == (mode == NativeMode::Off ? 0u : 3u) &&
                    stats.verified == (mode == NativeMode::Verify ? 2u : 0u) &&
                    stats.native == (mode == NativeMode::Native ? 2u : 0u) &&
                    stats.fallbacks == (mode == NativeMode::Off ? 0u : 1u) &&
                    stats.errors == 0u && stats.mismatches == 0u,
                    label + ": owner counters differ from registered calls");
        }
    }
}

bool direct_chain(Runtime &runtime, AllegrexContext &cpu, std::size_t leaf) {
    auto memory = runtime.memory().aot_fast_view();
    switch (leaf) {
    case 0u: return runtime.invoke_chained_direct<&recomp_unit_0028_entry, 28,
        (0x08877244u - 0x08874000u) / 4u + 1u, 0x08877244u>(cpu, &memory);
    case 1u: return runtime.invoke_chained_direct<&recomp_unit_0028_entry, 28,
        (0x08877264u - 0x08874000u) / 4u + 1u, 0x08877264u>(cpu, &memory);
    case 2u: return runtime.invoke_chained_direct<&recomp_unit_0028_entry, 28,
        (0x08877280u - 0x08874000u) / 4u + 1u, 0x08877280u>(cpu, &memory);
    default: return runtime.invoke_chained_direct<&recomp_unit_0028_entry, 28,
        (0x088772a8u - 0x08874000u) / 4u + 1u, 0x088772a8u>(cpu, &memory);
    }
}

void chained_dispatch(const Elf32Image &elf, Runtime &reference) {
    Runtime candidate(elf.required_ram_size());
    load(candidate, elf);
    VectorMetricRuntime owner(candidate, elf, &recomp_unit_0028);
    require(owner.install({NativeMode::Native, NativeMode::Native,
                           NativeMode::Native, NativeMode::Native}),
            "native direct-chain install failed");
    std::array<Fixture, 4> samples{};
    std::array<Expected, 4> expected{};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = fixture(kVectorMetricLeaves[i], 0u);
        expected[i] = original_result(reference, samples[i]);
    }
    Recording recording;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto entry = kVectorMetricLeaves[i].entry;
        candidate.memory().copy_in(data_address, samples[i].before);
        auto cpu = samples[i].cpu;
        cpu.pc = 0x08812344u; // actual compiled callers can carry a stale PC
        require(!direct_chain(candidate, cpu, i), "installed entry was bypassed by a direct chain");
        require(cpu.pc == entry && !candidate.stopped(), "direct chain did not yield at exact metric entry");
        Window before_dispatch{};
        candidate.memory().copy_out(data_address, before_dispatch);
        require(before_dispatch == samples[i].before, "direct chain executed original stores before hook");
        require(candidate.invoke_isolated_aot(entry, cpu), "outer dispatch did not invoke metric hook");
        Window after{};
        candidate.memory().copy_out(data_address, after);
        compare(expected[i], cpu, after, "direct-chain metric " + std::to_string(entry));
    }
    const auto journal = recording.finish();
    for (std::size_t i = 0; i < samples.size(); ++i)
        check_probe(summary(journal, kVectorMetricLeaves[i].entry), 1u, 0u, 0u, 1u, 0u,
                    "direct-chain metric " + std::to_string(kVectorMetricLeaves[i].entry));
}

void actual_same_unit_caller(const Elf32Image &elf) {
    Runtime original(elf.required_ram_size());
    load(original, elf);
    guard_same_unit_caller(original);
    auto sample = fixture(kVectorMetricLeaves[0], 0u);
    sample.cpu.pc = same_unit_caller;
    original.memory().copy_in(data_address, sample.before);
    Expected expected{sample.cpu, {}};
    require(original.invoke_isolated_aot(same_unit_caller, expected.cpu) &&
            !original.stopped() && expected.cpu.pc == external_return,
            "production same-unit caller did not return through norm");
    original.memory().copy_out(data_address, expected.after);

    for (const auto mode : {NativeMode::Verify, NativeMode::Native}) {
        Runtime candidate(elf.required_ram_size());
        load(candidate, elf);
        guard_same_unit_caller(candidate);
        VectorMetricRuntime owner(candidate, elf, &recomp_unit_0028);
        require(owner.install({mode, NativeMode::Off, NativeMode::Off, NativeMode::Off}),
                "same-unit norm install failed");
        Recording recording;
        call_registered(candidate, sample, expected,
                        "production same-unit caller with norm replacement");
        const auto journal = recording.finish();
        check_probe(summary(journal, kVectorMetricLeaves[0].entry), 1u, 0u,
                    mode == NativeMode::Verify ? 1u : 0u,
                    mode == NativeMode::Native ? 1u : 0u, 0u,
                    "same-unit caller norm entry");
        for (std::size_t i = 1; i < kVectorMetricLeaves.size(); ++i)
            check_probe(summary(journal, kVectorMetricLeaves[i].entry), 0u, 0u, 0u, 0u, 0u,
                        "same-unit caller untouched metric");
        const auto stats = owner.stats()[0];
        require(stats.calls == 1u && stats.verified == (mode == NativeMode::Verify ? 1u : 0u) &&
                stats.native == (mode == NativeMode::Native ? 1u : 0u) &&
                stats.fallbacks == 0u && stats.mismatches == 0u && stats.errors == 0u,
                "same-unit caller did not execute one admitted norm replacement");
    }
}

void internal_returns(const Elf32Image &elf, Runtime &reference) {
    // A generated wrapper may continue executing when RA points back into
    // its own unit. Only the installed replacement is invoked with this RA;
    // the original oracle uses the bounded zero-sentinel callback above.
    for (const auto mode : {NativeMode::Verify, NativeMode::Native}) {
        Runtime candidate(elf.required_ram_size());
        load(candidate, elf);
        VectorMetricRuntime owner(candidate, elf, &recomp_unit_0028);
        require(owner.install({mode, mode, mode, mode}), "internal-return install failed");
        std::array<Fixture, 4> samples{};
        std::array<Expected, 4> expected{};
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = fixture(kVectorMetricLeaves[i], 0u);
            samples[i].cpu.gpr[31] = kVectorMetricLeaves[i].entry + 4u;
            expected[i] = original_result(reference, samples[i]);
        }
        Recording recording;
        for (std::size_t i = 0; i < samples.size(); ++i)
            call_registered(candidate, samples[i], expected[i],
                            "internal return " + std::to_string(kVectorMetricLeaves[i].entry));
        const auto journal = recording.finish();
        for (const auto &leaf : kVectorMetricLeaves)
            check_probe(summary(journal, leaf.entry), 1u, 0u,
                        mode == NativeMode::Verify ? 1u : 0u,
                        mode == NativeMode::Native ? 1u : 0u, 0u,
                        "internal return " + std::to_string(leaf.entry));
    }
}

void ownership_and_restoration(const Elf32Image &elf, Runtime &reference) {
    Runtime first(elf.required_ram_size()), second(elf.required_ram_size());
    load(first, elf);
    load(second, elf);
    const auto sample = fixture(kVectorMetricLeaves[0], 0u);
    const auto expected = original_result(reference, sample);
    {
        VectorMetricRuntime first_owner(first, elf, &recomp_unit_0028);
        VectorMetricRuntime second_owner(second, elf, &recomp_unit_0028);
        VectorMetricRuntime duplicate(first, elf, &recomp_unit_0028);
        require(first_owner.install({NativeMode::Native, NativeMode::Off,
                                     NativeMode::Off, NativeMode::Off}), "first runtime install failed");
        require(second_owner.install({NativeMode::Verify, NativeMode::Off,
                                      NativeMode::Off, NativeMode::Off}), "second runtime install failed");
        require(!duplicate.install({NativeMode::Native, NativeMode::Off,
                                    NativeMode::Off, NativeMode::Off}), "duplicate runtime owner was accepted");
        call_registered(first, sample, expected, "first independent runtime");
        call_registered(second, sample, expected, "second independent runtime");
        call_registered(first, sample, expected, "first runtime after duplicate refusal");
        const auto first_stats = first_owner.stats()[0];
        const auto second_stats = second_owner.stats()[0];
        require(first_stats.calls == 2u && first_stats.native == 2u &&
                first_stats.verified == 0u && first_stats.errors == 0u &&
                second_stats.calls == 1u && second_stats.verified == 1u &&
                second_stats.native == 0u && second_stats.errors == 0u &&
                duplicate.stats()[0].calls == 0u,
                "two runtime registrations shared state or duplicate owner took the hook");
    }
    Recording recording;
    call_registered(first, sample, expected, "first runtime after owner destruction");
    call_registered(second, sample, expected, "second runtime after owner destruction");
    first.memory().copy_in(data_address, sample.before);
    auto chained_cpu = sample.cpu;
    chained_cpu.pc = 0x08812344u;
    require(direct_chain(first, chained_cpu, 0u),
            "restored original entry remained unavailable to compiled direct chain");
    Window chained_after{};
    first.memory().copy_out(data_address, chained_after);
    compare(expected, chained_cpu, chained_after, "restored original direct chain");
    const auto row = summary(recording.finish(), kVectorMetricLeaves[0].entry);
    check_probe(row, 3u, 3u, 0u, 0u, 0u, "restored original entries");
}

void fingerprint_guards(const Elf32Image &elf, Runtime &reference) {
    const auto &leaf = kVectorMetricLeaves[0];
    const auto changed_address = leaf.entry + leaf.size - 4u;
    const auto sample = fixture(leaf, 0u);
    Runtime unknown(elf.required_ram_size());
    load(unknown, elf);
    const auto word = unknown.memory().load32(changed_address);
    unknown.memory().store32(changed_address, word ^ 1u);
    VectorMetricRuntime refused(unknown, elf, &recomp_unit_0028);
    require(!refused.install({NativeMode::Native, NativeMode::Off,
                              NativeMode::Off, NativeMode::Off}),
            "unknown full leaf fingerprint installed a hook");
    unknown.memory().store32(changed_address, word);
    require(refused.stats()[0].calls == 0u, "failed install recorded a native call");
    {
        const auto expected = original_result(reference, sample);
        Recording recording;
        call_registered(unknown, sample, expected, "original entry after refused fingerprint");
        check_probe(summary(recording.finish(), leaf.entry), 1u, 1u, 0u, 0u, 0u,
                    "refused fingerprint left original dispatch installed");
    }
    Runtime changed(elf.required_ram_size());
    load(changed, elf);
    VectorMetricRuntime installed(changed, elf, &recomp_unit_0028);
    require(installed.install({NativeMode::Native, NativeMode::Off,
                               NativeMode::Off, NativeMode::Off}), "mutation fixture install failed");
    changed.memory().copy_in(data_address, sample.before);
    const auto old_word = changed.memory().load32(changed_address);
    changed.memory().store32(changed_address, old_word ^ 1u);
    auto cpu = sample.cpu;
    Window before{}, after{};
    changed.memory().copy_out(data_address, before);
    require(changed.invoke_isolated_aot(leaf.entry, cpu), "mutated hook disappeared");
    changed.memory().copy_out(data_address, after);
    const auto stats = installed.stats()[0];
    require(changed.stopped() && same_context(cpu, sample.cpu) && before == after &&
            stats.calls == 0u && stats.errors == 1u && stats.native == 0u,
            "post-install code mutation was executed or changed guest state");
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: vector_metrics_runtime_tests local-EBOOT.ELF");
        require(sha256_file(argv[1]) == supported_elf_hash,
                "unsupported ELF SHA-256; pass the registered local EBOOT.ELF");
        const auto elf = Elf32Image::from_file(argv[1]);
        Runtime reference(elf.required_ram_size());
        (void)elf.load_and_relocate(reference.memory());
        mode_matrix(elf, reference);
        chained_dispatch(elf, reference);
        actual_same_unit_caller(elf);
        internal_returns(elf, reference);
        ownership_and_restoration(elf, reference);
        fingerprint_guards(elf, reference);
        std::cout << "Vector metric runtime: all production dispatch, probe, owner and fingerprint checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
