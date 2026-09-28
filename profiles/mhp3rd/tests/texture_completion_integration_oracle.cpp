// Compiled G1c integration fixture.
//
// This translation unit deliberately reuses the R1 owner/enqueue/read harness,
// but installs the tracker-forwarding completion callbacks and drives each case
// through the instrumented original unit-0024 object set.  The lower original
// oracle and the tracker smoke remain separate executables.
#define MHP3RD_TEXTURE_TRANSFER_G1A_ORACLE 1
#define MHP3RD_TEXTURE_READ_G1B_ORACLE 1
#define MHP3RD_TEXTURE_COMPLETION_G1C_ORACLE 1
#define MHP3RD_TEXTURE_COMPLETION_INTEGRATION_ORACLE 1

// The shared lifetime source contains the lower-oracle mains for its other
// compile modes.  Rename that dormant main while importing the fixture types;
// this executable owns the integration report below.
#define main mhp3rd_texture_completion_integration_hidden_main
#include "texture_lifetime_oracle.cpp"
#undef main

#include "texture_completion_oracle_stop.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace psprecomp;
using namespace mhp3rd::native;

std::size_t g_retirement_stops{};

ReadScenario healthy_case(std::string name, std::uint32_t bytes,
                          bool transform = false, bool digest = false) {
    ReadScenario scenario{};
    scenario.name = std::move(name);
    scenario.results = {static_cast<std::int32_t>(bytes)};
    scenario.requested_bytes = bytes;
    scenario.completion_transform = transform;
    scenario.completion_digest = digest;
    return scenario;
}

struct IntegrationReport {
    std::vector<std::string> named_cases;
    std::size_t writer_released{};
    std::size_t writer_retained{};
    std::size_t cpu_comparisons{};
    std::size_t ram_comparisons{};
    std::size_t vram_comparisons{};
    std::size_t completion_events{};
    std::size_t retirement_observations{};
    std::size_t fault_cases{};
    std::size_t unsupported_cases{};
    std::vector<std::string> failed_cases;
};

void add_name(IntegrationReport &report, const std::string &name) {
    require(std::find(report.named_cases.begin(), report.named_cases.end(), name) ==
                report.named_cases.end(),
            "Duplicate compiled integration case name");
    report.named_cases.push_back(name);
}

void run_healthy(const Elf32Image &elf, const std::vector<std::uint8_t> &overlay,
                 Library &module, std::uint32_t sequence, ReadScenario scenario,
                 IntegrationReport &report) {
    const auto name = scenario.name;
    try {
        const auto stops_before = g_retirement_stops;
        Gate gate(elf, overlay, module, 0u);
        const auto stats = gate.read_prefix_scenario(scenario, sequence, true, true);
        const auto expected_live_writers =
            scenario.completion_old_pending_writer ? 1u : 0u;
        require(stats.completion_started == 1u && stats.completion_completed == 1u &&
                    stats.active_completions == 0u &&
                    stats.live_writers == expected_live_writers,
                "Compiled healthy completion did not release its writer");
        require(gate.completion_retirement_observed(),
                "Healthy integration case did not observe original RetirementReturn");
        require(gate.completion_comparisons_performed(),
                "Healthy integration case did not compare both completion sides");
        require(g_retirement_stops > stops_before,
                "Completion stop header did not stop at original RetirementReturn");
        require(gate.completion_tracker_stats().completion_records == 1u &&
                    gate.completion_observation_count() != 0u,
                "Healthy integration case did not publish a completion record");
        add_name(report, name);
        ++report.writer_released;
        report.writer_retained += stats.live_writers;
        ++report.cpu_comparisons;
        ++report.ram_comparisons;
        ++report.vram_comparisons;
        report.completion_events += gate.completion_observation_count();
        ++report.retirement_observations;
    } catch (const std::exception &error) {
        report.failed_cases.push_back(name);
        std::cerr << "G1c healthy case failed: " << name << ": "
                  << error.what() << '\n';
    }
}

void run_fault(const Elf32Image &elf, const std::vector<std::uint8_t> &overlay,
               Library &module, std::uint32_t sequence, ReadScenario scenario,
               IntegrationReport &report) {
    const auto name = scenario.name;
    try {
        Gate gate(elf, overlay, module, 0u);
        const auto stats = gate.read_prefix_scenario(scenario, sequence, true, false);
        require(gate.completion_fault_applied(),
                "Fault-injection case did not reach its named compiled checkpoint");
        require(stats.live_writers != 0u,
                "Fault-injection case released a writer after incomplete evidence");
        add_name(report, name);
        ++report.writer_retained;
        if (scenario.expected_completion_outcome == TextureCompletionOutcome::Unsupported)
            ++report.unsupported_cases;
        else
            ++report.fault_cases;
        report.completion_events += gate.completion_observation_count();
        if (gate.completion_retirement_observed()) ++report.retirement_observations;
    } catch (const std::exception &error) {
        report.failed_cases.push_back(name);
        std::cerr << "G1c fault case failed: " << name << ": "
                  << error.what() << '\n';
    }
}

} // namespace

namespace mhp3rd::native {

bool texture_completion_oracle_stop_after_retirement(
    psprecomp::Runtime &, psprecomp::AllegrexContext &) noexcept {
    ++::g_retirement_stops;
    return true;
}

} // namespace mhp3rd::native

int main(int argc, char **argv) {
    try {
        require(argc == 7,
                "usage: texture_completion_integration_oracle EBOOT.ELF lobby.bin lobby.dylib report.json encoded decoded");
        require(!std::filesystem::exists(argv[4]) &&
                    !std::filesystem::is_symlink(argv[4]),
                "Use a new compiled integration report path");
        require(sha256_file(argv[1]) ==
                    "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c",
                "Unsupported ELF");
        require(sha256_file(argv[2]) ==
                    "c34bf34f5e71993f5f2d20cdc39ec1b965f64b66d46f8d7672b449fba64b5aca",
                "Unsupported lobby image");
        require(sha256_file(argv[3]) ==
                    "35d381ffb06ff45f7357d3ef1634719bcfd4d5810eba1de9b32ca0443b62f538",
                "Unsupported lobby module");
        require(sha256_file(argv[5]) ==
                    "91f5655fe6f01631d64692c0be30e43acb90588e164e500770ec5dd41e972978" &&
                    sha256_file(argv[6]) ==
                    "3f06d53ef775166b06a1bd98a8a03a0b79c5d895eb4ad652fc9ca3656a781885",
                "Unsupported private transform fixtures");
        std::ifstream stream(argv[2], std::ios::binary);
        std::vector<std::uint8_t> overlay{std::istreambuf_iterator<char>(stream), {}};
        const auto elf = Elf32Image::from_file(argv[1]);
        Library module(argv[3]);
        IntegrationReport report;
        std::uint32_t sequence = 1u;
        run_healthy(elf, overlay, module, sequence++,
                    healthy_case("inline-verbatim", 32u), report);
        run_healthy(elf, overlay, module, sequence++,
                    healthy_case("inline-transform", 32u, true), report);
        run_healthy(elf, overlay, module, sequence++,
                    healthy_case("transform-digest", 32u, true, true), report);
        run_healthy(elf, overlay, module, sequence++,
                    healthy_case("rounded-5-byte-footprint", 5u, true), report);

        auto retry = healthy_case("short-to-full-retry", 32u, true);
        retry.results = {3, 32};
        run_healthy(elf, overlay, module, sequence++, std::move(retry), report);

        auto pending = healthy_case("old-pending-writer-plus-new-completion", 32u);
        pending.completion_old_pending_writer = true;
        run_healthy(elf, overlay, module, sequence++, std::move(pending), report);

        auto unsupported = healthy_case("unsupported-copy-worker-fault", 32u);
        unsupported.completion_fault = CompletionFault::UnsupportedCopyWorker;
        unsupported.expected_completion_outcome = TextureCompletionOutcome::Unsupported;
        run_fault(elf, overlay, module, sequence++, std::move(unsupported), report);

        auto missing_copy = healthy_case("fault-missing-copy", 32u);
        missing_copy.completion_fault = CompletionFault::MissingCopy;
        missing_copy.expected_error = TextureTransferTrackerError::InvalidObservation;
        run_fault(elf, overlay, module, sequence++, std::move(missing_copy), report);

        auto missing_ack = healthy_case("fault-missing-acknowledgement", 32u, true);
        missing_ack.completion_fault = CompletionFault::MissingAcknowledgement;
        missing_ack.expected_error = TextureTransferTrackerError::InvalidObservation;
        run_fault(elf, overlay, module, sequence++, std::move(missing_ack), report);

        auto wrong_worker = healthy_case("fault-wrong-worker", 32u);
        wrong_worker.completion_fault = CompletionFault::WrongWorker;
        // A worker entry bound to another manager cannot be paired with the
        // selected operation, so the tracker locks authority as unpaired.
        wrong_worker.expected_error = TextureTransferTrackerError::UnpairedSelectedLoad;
        run_fault(elf, overlay, module, sequence++, std::move(wrong_worker), report);

        auto descriptor = healthy_case("fault-descriptor-mutation", 32u);
        descriptor.completion_fault = CompletionFault::DescriptorMutation;
        descriptor.expected_error = TextureTransferTrackerError::InvalidObservation;
        run_fault(elf, overlay, module, sequence++, std::move(descriptor), report);

        auto cancellation = healthy_case("fault-cancellation", 32u);
        cancellation.completion_fault = CompletionFault::Cancellation;
        cancellation.expected_completion_outcome = TextureCompletionOutcome::Cancelled;
        run_fault(elf, overlay, module, sequence++, std::move(cancellation), report);

        auto state = healthy_case("fault-wrong-retirement-state", 32u);
        state.completion_fault = CompletionFault::WrongRetirementState;
        state.expected_error = TextureTransferTrackerError::InvalidObservation;
        run_fault(elf, overlay, module, sequence++, std::move(state), report);

        auto consumer = healthy_case("fault-wrong-retirement-consumer", 32u);
        consumer.completion_fault = CompletionFault::WrongRetirementConsumer;
        consumer.expected_error = TextureTransferTrackerError::InvalidObservation;
        run_fault(elf, overlay, module, sequence++, std::move(consumer), report);

        if (report.failed_cases.empty()) {
            require(report.writer_released != 0u && report.writer_retained != 0u &&
                        report.cpu_comparisons != 0u && report.ram_comparisons != 0u &&
                        report.vram_comparisons != 0u && report.retirement_observations != 0u &&
                        report.fault_cases != 0u && report.unsupported_cases != 0u,
                    "Compiled integration coverage was empty");
        }

        std::ofstream out(argv[4]);
        out << "{\"schema_version\":1"
            << ",\"scope\":\"compiled-g1c-texture-completion-integration\""
            << ",\"success\":" << (report.failed_cases.empty() ? "true" : "false")
            << ",\"compiled_integration\":true"
            << ",\"synthetic_completion_sequence_used\":false"
            << ",\"fixture_supplied_successful_retirement\":false"
            << ",\"original_retirement_observed\":"
            << (report.retirement_observations != 0u ? "true" : "false")
            << ",\"transfer_readiness\":false"
            << ",\"source_completion_receipts\":0"
            << ",\"named_cases\":[";
        for (std::size_t i = 0; i < report.named_cases.size(); ++i) {
            if (i != 0u) out << ',';
            out << '"' << report.named_cases[i] << '"';
        }
        out << "]"
            << ",\"writer_released\":" << report.writer_released
            << ",\"writer_retained\":" << report.writer_retained
            << ",\"cpu_comparisons\":" << report.cpu_comparisons
            << ",\"ram_comparisons\":" << report.ram_comparisons
            << ",\"vram_comparisons\":" << report.vram_comparisons
            << ",\"full_ram_vram_cpu_compared\":"
            << (report.cpu_comparisons != 0u && report.ram_comparisons != 0u &&
                        report.vram_comparisons != 0u ? "true" : "false")
            << ",\"completion_events\":" << report.completion_events
            << ",\"retirement_observations\":" << report.retirement_observations
            << ",\"fault_cases\":" << report.fault_cases
            << ",\"unsupported_cases\":" << report.unsupported_cases
            << ",\"failed_cases\": [";
        for (std::size_t i = 0; i < report.failed_cases.size(); ++i) {
            if (i != 0u) out << ',';
            out << '"' << report.failed_cases[i] << '"';
        }
        out << "]"
            << ",\"game_executed\":false}\n";
        require(static_cast<bool>(out), "Could not write compiled integration report");
        return report.failed_cases.empty() ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
