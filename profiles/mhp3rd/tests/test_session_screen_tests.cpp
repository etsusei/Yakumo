#include "ui/test_session_screen.hpp"

#include "settings/settings.hpp"
#include "testing/case_controller.hpp"
#include "testing/game_observers.hpp"
#include "testing/journal.hpp"
#include "testing/session_recorder.hpp"
#include "ui/layer.hpp"
#include "ui/localization.hpp"
#include "ui/widgets.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace mhp3rd::testing;

int failures{};

void check(bool condition, std::string_view message) {
    if (!condition && ++failures <= 50) std::cerr << "FAIL: " << message << '\n';
}

struct Button {
    std::string label;
    bool disabled{};
};

struct Drawn {
    std::vector<std::string> sections;
    std::vector<std::string> headings;
    std::vector<std::string> paragraphs;
    std::vector<std::pair<std::string, std::string>> info;
    std::vector<Button> buttons;
    std::string requested_button;
    int case_direction{};

    void reset(std::string requested = {}, int direction = 0) {
        sections.clear();
        headings.clear();
        paragraphs.clear();
        info.clear();
        buttons.clear();
        requested_button = std::move(requested);
        case_direction = direction;
    }
};

Drawn drawn;

bool saw(const std::vector<std::string> &texts, std::string_view text) {
    return std::find(texts.begin(), texts.end(), text) != texts.end();
}

bool saw_info(std::string_view label, std::string_view value) {
    return std::any_of(drawn.info.begin(), drawn.info.end(), [&](const auto &item) {
        return item.first == label && item.second == value;
    });
}

bool button_disabled(std::string_view label) {
    const auto found = std::find_if(drawn.buttons.begin(), drawn.buttons.end(), [&](const Button &button) {
        return button.label == label;
    });
    check(found != drawn.buttons.end(), "expected panel button is present");
    return found != drawn.buttons.end() && found->disabled;
}

struct MemoryState {
    std::mutex mutex;
    std::vector<std::uint8_t> bytes;

    std::vector<std::uint8_t> copy() {
        std::lock_guard lock(mutex);
        return bytes;
    }
};

class MemorySink final : public JournalSink {
public:
    explicit MemorySink(std::shared_ptr<MemoryState> state) : state_(std::move(state)) {}
    bool write(std::span<const std::uint8_t> bytes) override {
        std::lock_guard lock(state_->mutex);
        state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool flush() override { return true; }
private:
    std::shared_ptr<MemoryState> state_;
};

struct Fixture {
    std::shared_ptr<MemoryState> state = std::make_shared<MemoryState>();
    std::shared_ptr<SessionRecorder> recorder;
    std::shared_ptr<GameObserver> observer;
    std::shared_ptr<CaseController> controller;

    explicit Fixture(std::string role = "baseline", std::optional<CaseCatalog> supplied = {}) {
        recorder = std::make_shared<SessionRecorder>(
            std::make_unique<MemorySink>(state), Fields{{"role", std::string("ui_test")}});
        observer = std::make_shared<GameObserver>(recorder);

        CaseCatalog catalog;
        catalog.sha256 = std::string(64, 'a');
        CaseSpec first;
        first.id = "PAIR-01";
        first.version = 1;
        first.title = "Load the agreed character";
        first.steps = {"Start from the supplied save copy.", "Literal %s {0} text"};
        first.checkpoints = {"village_ready"};
        catalog.cases.push_back(std::move(first));
        CaseSpec second;
        second.id = "PAIR-02";
        second.version = 1;
        second.title = "Custom %s route";
        second.steps = {"Keep %d and {1} literal"};
        catalog.cases.push_back(std::move(second));
        if (supplied) catalog = std::move(*supplied);

        CaseSessionInfo session{std::move(role), "build-1", "run-1", "batch-1", "B0", std::string(64, 'b')};
        CaseHooks hooks{
            [](std::uint32_t) {},
            [this](std::string_view boundary) {
                observer->emit(EventKind::Probe, "native.probe_summary",
                               {{"boundary", std::string(boundary)}});
            },
            [this] { observer->emit(EventKind::State, "game.state", {{"health_current", std::uint64_t{10}}}); },
            [this] { observer->emit(EventKind::Probe, "native.probe_detail"); },
            [] { return std::string(64, 'c'); },
        };
        controller = std::make_shared<CaseController>(observer, std::move(catalog), std::move(session),
                                                      std::move(hooks));
        set_active_case_controller(controller);
    }

    ~Fixture() {
        set_active_case_controller({});
        if (controller) controller->close("ui_test_done");
        if (recorder) (void)recorder->close("ui_test_done");
    }

    JournalRecovery close_recording(bool completed = true) {
        check(recorder->close("ui_test_complete", completed), "memory recording closes");
        const JournalRecovery recovery = recover_journal(state->copy());
        check(recovery.complete(), "memory journal recovers completely");
        return recovery;
    }
};

std::size_t count(const JournalRecovery &journal, EventKind kind) {
    return static_cast<std::size_t>(std::count_if(journal.records.begin(), journal.records.end(),
                                                  [&](const JournalRecord &record) { return record.kind == kind; }));
}

bool contains_outcome(const JournalRecovery &journal, std::string_view outcome) {
    const std::string token = "\"outcome\":\"" + std::string(outcome) + "\"";
    return std::any_of(journal.records.begin(), journal.records.end(), [&](const JournalRecord &record) {
        return record.kind == EventKind::CaseEnd && record.payload.find(token) != std::string::npos;
    });
}

bool frame(std::string button = {}, int case_direction = 0) {
    drawn.reset(std::move(button), case_direction);
    ImGui::NewFrame();
    ImGui::Begin("##test_parent", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const bool resume = mhp3rd::ui::draw_test_session_screen();
    ImGui::End();
    ImGui::Render();
    return resume;
}

void test_panel_flow() {
    Fixture fixture;
    check(mhp3rd::ui::test_screen_available(), "panel is available with an active controller");
    check(!frame(), "viewing case does not resume game");
    check(saw_info("\u6D4B\u8BD5\u89D2\u8272", "\u57FA\u51C6\u7248"), "Chinese baseline role is visible");
    check(saw_info("\u7A0B\u5E8F\u7248\u672C", "build-1"), "build version is visible");
    check(saw(drawn.headings, "\u8F7D\u5165\u7EA6\u5B9A\u89D2\u8272"), "known case title is Chinese");
    check(saw(drawn.paragraphs, "1.  \u4ECE\u63D0\u4F9B\u7684\u5B58\u6863\u526F\u672C\u5F00\u59CB\u3002"),
          "known case step is Chinese");
    check(saw(drawn.paragraphs, "2.  Literal %s {0} text"), "custom step remains literal");
    check(saw(drawn.paragraphs, "1.  \u6751\u5E84\u5DF2\u5C31\u7EEA  \u2014  \u4E0B\u4E00\u4E2A"),
          "known checkpoint has a Chinese label");
    check(!button_disabled("\u5F00\u59CB\u672C\u6848\u4F8B"), "begin is enabled while recording is healthy");

    check(frame("\u5F00\u59CB\u672C\u6848\u4F8B"), "Begin requests game resume");
    check(fixture.controller->active_case() == 0, "Begin starts the selected case");
    check(!frame(), "returning to panel does not end active case");
    check(fixture.controller->active_case() == 0, "case survives panel close and reopening");
    check(button_disabled("\u6807\u8BB0\u6B63\u5E38\u7ED3\u675F"), "normal is disabled before required checkpoint");
    check(!frame("\u6807\u8BB0\u6B63\u5E38\u7ED3\u675F"), "disabled normal does not resume or finish");
    check(fixture.controller->active_case() == 0, "disabled normal leaves case active");

    ImGui::NewFrame();
    mhp3rd::ui::draw_test_session_hint();
    ImGuiWindow *hint = ImGui::FindWindowByName("##test_session_hint");
    check(hint != nullptr && (hint->Flags & ImGuiWindowFlags_NoInputs) != 0,
          "active hint never captures gameplay input");
    ImGui::Render();

    check(frame("\u8BB0\u5F55\u4E0B\u4E00\u4E2A\u68C0\u67E5\u70B9"), "checkpoint requests game resume");
    check(fixture.controller->progress()[0].next_checkpoint == 1, "checkpoint advances case progress");
    check(frame("\u8BB0\u5F55\u53D1\u73B0\u5F02\u5E38"), "anomaly marker requests game resume");
    check(!frame(), "after checkpoint panel stays observational");
    check(!button_disabled("\u6807\u8BB0\u6B63\u5E38\u7ED3\u675F"), "normal is enabled after checkpoint");
    check(!frame("\u6807\u8BB0\u6B63\u5E38\u7ED3\u675F"), "explicit normal finish stays in the panel");
    check(!fixture.controller->active_case(), "normal button ends case explicitly");
    check(fixture.controller->progress()[0].state == CaseProgressState::Normal,
          "normal state came from the explicit button");

    check(!frame({}, 1), "case selector changes the displayed case without recording");
    check(saw(drawn.headings, "Custom %s route"), "unknown title stays literal");
    check(saw(drawn.paragraphs, "1.  Keep %d and {1} literal"), "unknown step stays literal");
    check(frame("\u5F00\u59CB\u672C\u6848\u4F8B"), "second case begins");
    ImGui::NewFrame();
    mhp3rd::ui::draw_test_session_hint();
    ImGui::Render();
    check(!frame("\u6807\u8BB0\u5F02\u5E38\u7ED3\u675F"), "explicit abnormal finish stays in panel");
    check(fixture.controller->progress()[1].state == CaseProgressState::Abnormal,
          "abnormal outcome is distinct");
    check(frame("\u5F00\u59CB\u672C\u6848\u4F8B"), "second case may be retried");
    check(!frame("\u8DF3\u8FC7\u672C\u6848\u4F8B"), "explicit skip stays in panel");
    check(fixture.controller->progress()[1].state == CaseProgressState::Skipped,
          "skipped outcome is distinct");
    check(frame("\u5F00\u59CB\u672C\u6848\u4F8B"), "case may be retried after skip");
    check(!frame("\u6807\u8BB0\u65E0\u6CD5\u5224\u65AD"), "explicit uncertain finish stays in panel");
    check(fixture.controller->progress()[1].state == CaseProgressState::Uncertain,
          "uncertain outcome is distinct");

    check(fixture.controller->effective_state(0) == CaseProgressState::Normal,
          "healthy recording retains the prior normal status");
    // A duplicate reserved field is rejected by the real observer/recorder path.
    fixture.observer->emit(EventKind::State, "bad.event", {{"event", true}});
    check(fixture.observer->emission_errors() != 0 || fixture.recorder->health().invalid_events != 0,
          "real observer or recorder failure was registered");
    check(!frame({}, -1), "case selector can return to the earlier case");
    check(saw_info("\u6848\u4F8B\u72B6\u6001", "\u5DF2\u4E2D\u65AD"), "bad recorder masks a prior normal status");
    check(saw(drawn.paragraphs,
              "\u8BB0\u5F55\u51FA\u73B0\u95EE\u9898\u3002\u672C\u6B21\u6D4B\u8BD5\u9700\u8981\u590D\u6838\u3002"),
          "bad recorder is clearly shown as requiring review");
    check(button_disabled("\u5F00\u59CB\u672C\u6848\u4F8B"), "bad recorder disables another begin");
    check(!frame("\u5F00\u59CB\u672C\u6848\u4F8B"), "disabled begin cannot record another case");

    const JournalRecovery journal = fixture.close_recording();
    check(count(journal, EventKind::CaseBegin) == 4, "only four explicit begins were recorded");
    check(count(journal, EventKind::CaseEnd) == 4, "only four explicit endings were recorded");
    check(count(journal, EventKind::Checkpoint) == 1, "one checkpoint was recorded");
    check(count(journal, EventKind::Anomaly) == 1, "one anomaly was recorded");
    for (std::string_view outcome : {"normal", "abnormal", "uncertain", "skipped"})
        check(contains_outcome(journal, outcome), "all four outcomes have distinct journal records");
}

void test_bad_recording_while_active() {
    Fixture fixture;
    check(frame("\u5F00\u59CB\u672C\u6848\u4F8B"), "bad-recording fixture begins normally");
    (void)fixture.close_recording(false);
    check(!frame("\u8BB0\u5F55\u4E0B\u4E00\u4E2A\u68C0\u67E5\u70B9"), "checkpoint cannot run after recorder closes");
    check(button_disabled("\u8BB0\u5F55\u4E0B\u4E00\u4E2A\u68C0\u67E5\u70B9"),
          "checkpoint is disabled after recorder closes");
    check(button_disabled("\u8BB0\u5F55\u53D1\u73B0\u5F02\u5E38"), "anomaly is disabled after recorder closes");
    check(button_disabled("\u6807\u8BB0\u6B63\u5E38\u7ED3\u675F"), "normal end is disabled after recorder closes");
    check(button_disabled("\u6807\u8BB0\u5F02\u5E38\u7ED3\u675F"), "abnormal end is disabled after recorder closes");
    check(fixture.controller->active_case() == 0, "failed recording cannot silently finish the case");
}

void test_candidate_role() {
    Fixture fixture("candidate");
    check(!frame(), "candidate panel view does not resume game");
    check(saw_info("\u6D4B\u8BD5\u89D2\u8272", "\u5019\u9009\u7248"), "Chinese candidate role is visible");
}

void test_published_catalog(const std::filesystem::path &path) {
    const auto catalog = load_case_catalog(path);
    Fixture fixture("candidate", catalog);
    for (std::size_t index = 0; index < catalog.cases.size(); ++index) {
        frame({}, index == 0 ? 0 : 1);
        const auto &spec = catalog.cases[index];
        const std::string title = mhp3rd::ui::tr(spec.title.c_str());
        check(title != spec.title && saw(drawn.headings, title),
              "published case title is translated by the real panel");
        for (std::size_t step = 0; step < spec.steps.size(); ++step) {
            const std::string instruction = mhp3rd::ui::tr(spec.steps[step].c_str());
            check(instruction != spec.steps[step] &&
                  saw(drawn.paragraphs, std::to_string(step + 1) + ".  " + instruction),
                  "published step is translated by the real panel");
        }
        for (const auto &checkpoint : spec.checkpoints)
            check(std::none_of(drawn.paragraphs.begin(), drawn.paragraphs.end(), [&](const auto &text) {
                return text.find(checkpoint) != std::string::npos;
            }), "published checkpoint has a user-facing translated label");
        check(frame("\u5F00\u59CB\u672C\u6848\u4F8B"), "published case begins and returns to gameplay");
        check(fixture.controller->active_case() == index, "published case selection matches the catalog");
        for (std::size_t checkpoint = 0; checkpoint < spec.checkpoints.size(); ++checkpoint)
            check(frame("\u8BB0\u5F55\u4E0B\u4E00\u4E2A\u68C0\u67E5\u70B9"),
                  "published checkpoint returns to gameplay");
        check(!frame("\u6807\u8BB0\u6B63\u5E38\u7ED3\u675F"), "published finish stays in the menu");
        check(fixture.controller->progress()[index].state == CaseProgressState::Normal,
              "published case accepts an explicit completed outcome");
    }
}

} // namespace

namespace mhp3rd::ui {

// The production screen renders through these widgets. Stubs make the
// user-visible choices observable while ImGui owns the actual frame lifetime.
void section(const char *title) { drawn.sections.emplace_back(title); }
void info_row(const char *label, const std::string &value) { drawn.info.emplace_back(label, value); }
void paragraph(const std::string &value, ImU32) { drawn.paragraphs.push_back(value); }
void heading(const std::string &value) { drawn.headings.push_back(value); }
void progress_bar(float, const std::string &) {}
int choice_row(const char *, const std::string &, const RowOptions &options) {
    return options.disabled ? 0 : drawn.case_direction;
}
bool button_row(const char *label, const RowOptions &options, ImU32) {
    drawn.buttons.push_back({label, options.disabled});
    return !options.disabled && drawn.requested_button == label;
}
Layer &Layer::get() {
    static Layer layer;
    return layer;
}

} // namespace mhp3rd::ui

int main(int argc, char **argv) {
    if (argc != 1 && argc != 2) return 2;
    namespace fs = std::filesystem;
    const fs::path isolated_data = fs::temp_directory_path() /
        ("yakumo-test-session-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(isolated_data);
#if defined(_WIN32)
    _putenv_s("MHP3RD_DATA_DIR", isolated_data.string().c_str());
#else
    setenv("MHP3RD_DATA_DIR", isolated_data.string().c_str(), 1);
#endif
    mhp3rd::settings::current().ui_language = mhp3rd::settings::UiLanguage::SimplifiedChinese;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {1280.0f, 720.0f};
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();
    check(io.Fonts->Build(), "headless font atlas builds before the first frame");
    test_panel_flow();
    test_bad_recording_while_active();
    test_candidate_role();
    if (argc == 2) test_published_catalog(argv[1]);
    ImGui::DestroyContext();
    check(!mhp3rd::ui::test_screen_available(), "panel disappears when session owner releases controller");
    std::error_code error;
    fs::remove_all(isolated_data, error);
    if (failures == 0) std::cout << "Test session panel: all headless UI checks passed\n";
    return failures == 0 ? 0 : 1;
}
