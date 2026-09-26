#include "testing/case_runtime.hpp"
#include "install/game_identity.hpp"
#include "settings/settings.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/sha256.hpp"

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
        if (argc != 10 || std::string_view(argv[1]) != "--run") return 2;
        const std::string mode = argv[9];
        require(mode == "normal" || mode == "interrupt" || mode == "changed_config" || mode == "skip", "unknown fixture mode");
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
                        {"test_context", std::string("synthetic_case_runtime")}};
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
        require(controller->begin(0), "case did not start");
        observer->pad({100000, 3, 1, 1, 128, 128, 128, 128});
        if (mode == "changed_config") {
            settings.mute = !settings.mute;
            require(!controller->checkpoint(), "changed settings did not interrupt the case");
        } else if (mode == "skip") {
            require(controller->finish(CaseOutcome::Skipped), "explicit skip failed");
        } else if (mode == "normal") {
            require(controller->checkpoint(), "checkpoint failed");
            require(controller->finish(CaseOutcome::Normal), "normal finish failed");
        }
        close_case_session(controller, "window closed");
        require(!active_case_controller(), "closed controller remains published");
        diagnostics->close();
        require(before == psprecomp::sha256_bytes({bytes, memory.size()}), "case actions changed guest memory");
        return recording.close("window closed", true) ? 4 : 5;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
