#include "testing/runtime_recording.hpp"
#include "install/game_identity.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

// Synthetic events through the actual C++ writer, for independent Python
// framing/package/comparison checks. No game assets or execution are involved.
int main(int argc, char **argv) {
    using namespace mhp3rd::testing;
    if (argc != 6) {
        std::cerr << "Usage: fixture <new-run-directory> <role> <run-id> <context-sha256> <match|mismatch|interrupt|recorder_error>\n";
        return 2;
    }
    try {
        const std::string role = argv[2], mode = argv[5];
        if (mode != "match" && mode != "mismatch" && mode != "interrupt" && mode != "recorder_error") return 2;
        const bool baseline = role == "baseline";
        Fields metadata{{"recording_mode", std::string("observational-summary")},
                        {"binary_sha256", std::string(64, baseline ? 'a' : 'b')},
                        {"test_context", std::string("synthetic_package_integration")}};
        for (const auto *key : {"MHP3RD_NATIVE_ANGLE_STEP", "MHP3RD_NATIVE_SCALE_MATRIX",
                 "MHP3RD_NATIVE_TRANSLATION_MATRIX", "MHP3RD_NATIVE_VECTOR_CONSTRUCT", "MHP3RD_NATIVE_MATRIX_COPY"})
            metadata.push_back({key, std::string(!baseline && std::string(key) == "MHP3RD_NATIVE_VECTOR_CONSTRUCT" ? "verify" : "off")});
        RuntimeRecording recording({argv[1], role, argv[3], "batch-1", "B0", "4292eb6", argv[4]}, std::move(metadata));
        auto observer = recording.observer();
        observer->emit(EventKind::State, "runtime.inputs", {{"supported_elf", true},
            {"elf_sha256", std::string(mhp3rd::install::kExecutableSha256)}});
        observer->domain(InputDomain::Game);
        observer->time(100000, 3);
        auto identity = [] {
            return Fields{{"case_id", std::string("NATIVE-01")}, {"case_version", std::uint64_t{1}},
                          {"attempt", std::uint64_t{1}}};
        };
        auto begin = identity();
        begin.push_back({"prerequisites_sha256", std::string(64, 'c')});
        observer->emit(EventKind::CaseBegin, "case.begin", std::move(begin), true);
        const auto counters = [&](std::string boundary, std::uint64_t calls) {
            Fields fields{{"entry", std::uint64_t{0x08877818}}, {"leaf", std::string("vector_construct")},
                          {"boundary", std::move(boundary)}, {"counter_epoch", std::uint64_t{1}}};
            for (const auto *key : {"entry_hits", "certified_entries", "completed"}) fields.push_back({key, calls});
            for (const auto *key : {"uncertified_entries", "uncertified_returns", "incomplete", "orphan_exits",
                                   "return_mismatches", "native_calls", "fallback_calls"}) fields.push_back({key, std::uint64_t{0}});
            fields.push_back({"aot_calls", baseline ? calls : std::uint64_t{0}});
            fields.push_back({"verify_calls", baseline ? std::uint64_t{0} : calls});
            observer->emit(EventKind::Probe, "probe.summary", std::move(fields), true);
        };
        counters("case_begin", 0);
        observer->pad({100000, 3, 1, 1, 128, 128, 128, 128});
        observer->emit(EventKind::State, "game.state", {{"supported_executable", true},
            {"quest_status", std::string("verified")}, {"health_current", std::int64_t{100}}});
        auto checkpoint = identity(); checkpoint.push_back({"checkpoint_id", std::string("checked")});
        observer->emit(EventKind::Checkpoint, "case.checkpoint", std::move(checkpoint), true);
        if (mode == "interrupt") {
            // RuntimeRecording flushed RunBegin before publishing its observer;
            // immediate exit must remain incomplete, regardless of queued tail.
            std::_Exit(7);
        }
        if (mode == "mismatch") observer->emit(EventKind::Error, "native.verification_mismatch", {
            {"entry", std::uint64_t{0x08877818}}, {"evidence", std::string("same_input_reference")}, {"certified", true}});
        counters("case_end", 1);
        auto end = identity(); end.push_back({"outcome", std::string("normal")});
        observer->emit(EventKind::CaseEnd, "case.end", std::move(end), true);
        if (mode == "recorder_error") observer->emit(EventKind::State, "invalid_fixture_event", {
            {"event", std::string("duplicate_reserved_key")}});
        return recording.close("window closed", true) ? 4 : 5;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
