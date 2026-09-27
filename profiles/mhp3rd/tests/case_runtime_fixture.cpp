#include "testing/case_runtime.hpp"
#include "install/game_identity.hpp"
#include "settings/settings.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *why) { if (!value) throw std::runtime_error(why); }
void data_directory(const std::filesystem::path &path) {
#if defined(_WIN32)
    _putenv_s("MHP3RD_DATA_DIR", path.string().c_str());
#else
    setenv("MHP3RD_DATA_DIR", path.string().c_str(), 1);
#endif
}
}

int main(int argc, char **argv) {
    using namespace mhp3rd::testing;
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--catalog") {
            auto catalog = load_case_catalog(argv[2]);
            std::cout << "{\"sha256\":\"" << catalog.sha256 << "\",\"case_count\":" << catalog.cases.size() << "}\n";
            return 0;
        }
        if (argc != 10 || (std::string_view(argv[1]) != "--run" &&
                           std::string_view(argv[1]) != "--run-all")) return 2;
        const bool all_cases = std::string_view(argv[1]) == "--run-all";
        const std::string mode = argv[9];
        require(all_cases ? (mode == "normal" || mode == "interrupt") :
                (mode == "normal" || mode == "interrupt" || mode == "changed_config" || mode == "skip" ||
                 mode == "restored_config_save" || mode == "restored_config_snapshot" ||
                 mode == "navigation_only"),
                "unknown fixture mode");
        RecordingOptions options;
        options.directory = argv[2]; options.role = argv[3]; options.run_id = argv[4];
        options.batch_id = "batch-1"; options.context_sha256 = argv[5]; options.case_catalog = argv[6];
        options.case_catalog_sha256 = argv[7]; options.prerequisite_basis_sha256 = argv[8];
        data_directory(options.directory.parent_path() / (options.directory.filename().string() + "-settings"));
        auto &settings = mhp3rd::settings::current();
        const auto config = mhp3rd::settings::case_configuration_sha256();
        settings.menu_hint_seen = !settings.menu_hint_seen;
        settings.last_folder = "synthetic-navigation-history";
        settings.adhoc_mac = "01:02:03:04:05:06";
        settings.adhoc_nickname = "synthetic-instance-name";
        settings.adhoc_recent = {"synthetic-history"};
        require(config == mhp3rd::settings::case_configuration_sha256(), "navigation history changed prerequisites");

        Fields metadata{{"recording_mode", std::string("observational-summary")},
                        {"binary_sha256", std::string(64, 'a')},
                        {"test_context", std::string(all_cases ? "synthetic_all_case_pipeline" : "synthetic_case_runtime")}};
        for (const auto *key : {"MHP3RD_NATIVE_ANGLE_STEP", "MHP3RD_NATIVE_SCALE_MATRIX", "MHP3RD_NATIVE_TRANSLATION_MATRIX",
                               "MHP3RD_NATIVE_VECTOR_CONSTRUCT", "MHP3RD_NATIVE_MATRIX_COPY"})
            metadata.push_back({key, std::string("off")});
        RuntimeRecording recording(options, std::move(metadata));
        auto observer = recording.observer();
        observer->emit(EventKind::State, "runtime.inputs", {{"supported_elf", true},
            {"elf_sha256", std::string(mhp3rd::install::kExecutableSha256)}});
        observer->domain(InputDomain::Game);
        observer->time(100000, 3);
        psprecomp::GuestMemory memory(64u * 1024u * 1024u);
        const auto *bytes = memory.raw_pointer(0x08000000, memory.size());
        const auto before = psprecomp::sha256_bytes({bytes, memory.size()});
        auto diagnostics = std::make_shared<RuntimeDiagnostics>(observer, memory, true, 0);
        auto controller = start_case_session(options, observer, diagnostics, "synthetic-build");
        require(controller && active_case_controller() == controller, "case controller not published");
        if (all_cases) {
            const auto &cases = controller->catalog().cases;
            require(!cases.empty(), "all-case catalog is empty");
            std::size_t interrupted_index = cases.size();
            if (mode == "interrupt") {
                for (std::size_t index = 0; index < cases.size(); ++index) {
                    if (cases[index].checkpoints.size() > 1) {
                        interrupted_index = index;
                        break;
                    }
                }
                require(interrupted_index < cases.size(), "no multi-checkpoint case to interrupt");
            }
            std::uint64_t virtual_us = 100000;
            std::uint64_t vblank = 3;
            for (std::size_t index = 0; index < cases.size(); ++index) {
                require(controller->begin(index), "catalog case did not start");
                // These samples exercise journal ordering only. They do not
                // claim physical input or any native helper invocation.
                const auto advance = [&] {
                    virtual_us += 33333;
                    ++vblank;
                    observer->frame(virtual_us, vblank);
                    observer->pad({virtual_us, vblank, 1, 1, 128, 128, 128, 128});
                };
                advance();
                for (std::size_t checkpoint = 0; checkpoint < cases[index].checkpoints.size(); ++checkpoint) {
                    advance();
                    require(controller->checkpoint(), "catalog checkpoint failed");
                    if (index == interrupted_index) break;
                }
                if (index == interrupted_index) {
                    require(controller->active_case() == index, "interrupted case is not active");
                    break;
                }
                require(controller->finish(CaseOutcome::Normal), "catalog normal finish failed");
                require(controller->progress()[index].state == CaseProgressState::Normal,
                        "catalog case did not finish normally");
            }
            if (mode == "normal")
                for (const auto &progress : controller->progress())
                    require(progress.state == CaseProgressState::Normal, "catalog case was not completed");
        } else {
            require(controller->begin(0), "case did not start");
            observer->pad({100000, 3, 1, 1, 128, 128, 128, 128});
            if (mode == "changed_config") {
                settings.mute = !settings.mute;
                require(!controller->checkpoint(), "changed settings did not interrupt the case");
            } else if (mode == "restored_config_save" || mode == "restored_config_snapshot") {
                const auto original = settings.mute;
                const auto original_hash = mhp3rd::settings::case_configuration_sha256();
                settings.mute = !original;
                if (mode == "restored_config_save") mhp3rd::settings::save();
                else mhp3rd::settings::record_snapshot();
                settings.mute = original;
                if (mode == "restored_config_save") mhp3rd::settings::save();
                else mhp3rd::settings::record_snapshot();
                require(mhp3rd::settings::case_configuration_sha256() == original_hash,
                        "restored configuration did not match the case start");
                require(!controller->finish(CaseOutcome::Normal),
                        "restored settings allowed a normal CaseEnd");
                require(!controller->active_case() &&
                        controller->progress()[0].state == CaseProgressState::Interrupted,
                        "observed configuration change did not interrupt the case");
            } else if (mode == "navigation_only") {
                const auto original_hash = mhp3rd::settings::case_configuration_sha256();
                settings.menu_hint_seen = !settings.menu_hint_seen;
                settings.last_folder = "another-synthetic-navigation-folder";
                settings.adhoc_mac = "06:05:04:03:02:01";
                settings.adhoc_nickname = "another-synthetic-instance-name";
                settings.adhoc_recent = {"another-synthetic-history"};
                mhp3rd::settings::save();
                mhp3rd::settings::record_snapshot();
                require(mhp3rd::settings::case_configuration_sha256() == original_hash,
                        "navigation history changed case prerequisites");
                require(controller->checkpoint(), "navigation history interrupted the checkpoint");
                require(controller->finish(CaseOutcome::Normal), "navigation history interrupted normal finish");
                require(controller->progress()[0].state == CaseProgressState::Normal,
                        "navigation history changed the case outcome");
            } else if (mode == "skip") {
                require(controller->finish(CaseOutcome::Skipped), "explicit skip failed");
            } else if (mode == "normal") {
                require(controller->checkpoint(), "checkpoint failed");
                require(controller->finish(CaseOutcome::Normal), "normal finish failed");
            }
        }
        close_case_session(controller, "window closed");
        require(!active_case_controller(), "closed controller remains published");
        if (all_cases && mode == "interrupt") {
            require(!controller->active_case(), "interrupted case remains active");
            require(std::any_of(controller->progress().begin(), controller->progress().end(),
                    [](const CaseProgress &progress) { return progress.state == CaseProgressState::Interrupted; }),
                    "closing the run did not interrupt the active case");
        }
        diagnostics->close();
        require(before == psprecomp::sha256_bytes({bytes, memory.size()}), "case actions changed guest memory");
        return recording.close("window closed", true) ? 4 : 5;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
