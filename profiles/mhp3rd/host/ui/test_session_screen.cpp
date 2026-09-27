#include "ui/test_session_screen.hpp"

#include "ui/layer.hpp"
#include "ui/localization.hpp"
#include "ui/widgets.hpp"

#include "testing/case_controller.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace mhp3rd::ui {
namespace {

using testing::CaseController;
using testing::CaseOutcome;
using testing::CaseProgressState;
using testing::CaseSpec;

struct Selection {
    std::weak_ptr<CaseController> controller;
    std::size_t index{};
};

Selection &selection() {
    static Selection state;
    return state;
}

// Only enumerated case strings are translated. Catalog text outside
// this list is displayed verbatim and never used as a format string.
std::string_view case_text(const std::string &value) {
    if (value == "Vector call discovery in the village") return tr("Vector call discovery in the village");
    if (value == "This case discovers vector calls. Missing calls are not a player error; do not repeat the route to force coverage.")
        return tr("This case discovers vector calls. Missing calls are not a player error; do not repeat the route to force coverage.");
    if (value == "Open Test session and record Vector observation complete. Reopen it to mark your outcome, then close the game normally.")
        return tr("Open Test session and record Vector observation complete. Reopen it to mark your outcome, then close the game normally.");
    if (value == "Native data paths in the village") return tr("Native data paths in the village");
    if (value == "Begin this case in Test session. Stay still for three seconds and inspect the hunter, equipment and Chinese text.")
        return tr("Begin this case in Test session. Stay still for three seconds and inspect the hunter, equipment and Chinese text.");
    if (value == "Open Test session and record Native path observed. Reopen it to mark your outcome, then close the game normally.")
        return tr("Open Test session and record Native path observed. Reopen it to mark your outcome, then close the game normally.");
    if (value == "Load the agreed character") return tr("Load the agreed character");
    if (value == "Movement and camera") return tr("Movement and camera");
    if (value == "Native helper observations") return tr("Native helper observations");
    if (value == "Finish and close") return tr("Finish and close");
    if (value == "Start from the supplied save copy.") return tr("Start from the supplied save copy.");
    if (value == "Load the agreed character and enter the village.")
        return tr("Load the agreed character and enter the village.");
    if (value == "Mark the checkpoint after the village is ready.")
        return tr("Mark the checkpoint after the village is ready.");
    if (value == "Walk, stop, and turn on the agreed route.")
        return tr("Walk, stop, and turn on the agreed route.");
    if (value == "Move the camera, then open and close the menu.")
        return tr("Move the camera, then open and close the menu.");
    if (value == "Mark any unexpected behavior.") return tr("Mark any unexpected behavior.");
    if (value == "Inspect the character and equipment, then return to the village.")
        return tr("Inspect the character and equipment, then return to the village.");
    if (value == "Follow the agreed short route.") return tr("Follow the agreed short route.");
    if (value == "Mark the checkpoint when the route is finished.")
        return tr("Mark the checkpoint when the route is finished.");
    if (value == "Finish the active case before closing the window.")
        return tr("Finish the active case before closing the window.");
    if (value == "Close the game window normally after this case ends.")
        return tr("Close the game window normally after this case ends.");
    if (value == "Confirm loaded village") return tr("Confirm loaded village");
    if (value == "Village input and menu") return tr("Village input and menu");
    if (value == "Village native helper coverage") return tr("Village native helper coverage");
    if (value == "Complete recording and close") return tr("Complete recording and close");
    if (value == "Before this case, load the first occupied hunter from the supplied save copy and wait for the village.")
        return tr("Before this case, load the first occupied hunter from the supplied save copy and wait for the village.");
    if (value == "Use that same hunter slot and the same input device in both roles.")
        return tr("Use that same hunter slot and the same input device in both roles.");
    if (value == "Leave controls idle for two seconds. Open Yakumo's menu with Esc or L3+R3, choose Test session, and begin this case.")
        return tr("Leave controls idle for two seconds. Open Yakumo's menu with Esc or L3+R3, choose Test session, and begin this case.");
    if (value == "Check that the hunter and village remain visible. Do not move, change equipment, or enter a quest.")
        return tr("Check that the hunter and village remain visible. Do not move, change equipment, or enter a quest.");
    if (value == "Open Test session, record Village visible, then reopen it and mark your outcome.")
        return tr("Open Test session, record Village visible, then reopen it and mark your outcome.");
    if (value == "Start in the village after REC-01; begin this case in Test session.")
        return tr("Start in the village after REC-01; begin this case in Test session.");
    if (value == "Hold W or left stick up for two seconds, then release for two.")
        return tr("Hold W or left stick up for two seconds, then release for two.");
    if (value == "Hold A or left stick left for one second, then release. Stop early at a wall or door; do not retry or enter it.")
        return tr("Hold A or left stick left for one second, then release. Stop early at a wall or door; do not retry or enter it.");
    if (value == "Hold J or right stick left for one second, then release. Observe the camera.")
        return tr("Hold J or right stick left for one second, then release. Observe the camera.");
    if (value == "Open Yakumo's menu with Esc or L3+R3; record Movement and camera done in Test session.")
        return tr("Open Yakumo's menu with Esc or L3+R3; record Movement and camera done in Test session.");
    if (value == "Open Yakumo's menu again; press Down then Up once with arrow keys or D-pad, then close it.")
        return tr("Open Yakumo's menu again; press Down then Up once with arrow keys or D-pad, then close it.");
    if (value == "Reopen Test session; record Menu round trip. Reopen it again to mark your outcome.")
        return tr("Reopen Test session; record Menu round trip. Reopen it again to mark your outcome.");
    if (value == "Begin in the village where REC-02 ended, with the same hunter and unchanged equipment.")
        return tr("Begin in the village where REC-02 ended, with the same hunter and unchanged equipment.");
    if (value == "Look at the hunter's model and outfit or weapon. Do not change equipment or enter a quest.")
        return tr("Look at the hunter's model and outfit or weapon. Do not change equipment or enter a quest.");
    if (value == "Hold D or left stick right for one second, release, then wait three seconds. Stop early at a wall or door; do not retry or enter it.")
        return tr("Hold D or left stick right for one second, release, then wait three seconds. Stop early at a wall or door; do not retry or enter it.");
    if (value == "Open Test session; record Native route finished, then reopen it and mark your outcome.")
        return tr("Open Test session; record Native route finished, then reopen it and mark your outcome.");
    if (value == "After NATIVE-01 has an outcome, begin this case in Test session while the village remains visible.")
        return tr("After NATIVE-01 has an outcome, begin this case in Test session while the village remains visible.");
    if (value == "Stay still and do not change settings. Reopen Test session and record Ready to close.")
        return tr("Stay still and do not change settings. Reopen Test session and record Ready to close.");
    if (value == "Reopen Test session and mark your outcome.")
        return tr("Reopen Test session and mark your outcome.");
    if (value == "Only after this case has ended, close the game window normally and wait for collection.")
        return tr("Only after this case has ended, close the game window normally and wait for collection.");
    return value;
}

std::string_view checkpoint_text(const std::string &value) {
    if (value == "vector_observation_complete") return tr("Vector observation complete");
    if (value == "native_path_observed") return tr("Native path observed");
    if (value == "village_ready") return tr("Village ready");
    if (value == "route_complete") return tr("Route finished");
    if (value == "ready_to_close") return tr("Ready to close");
    if (value == "village_visible") return tr("Village visible");
    if (value == "movement_camera_done") return tr("Movement and camera done");
    if (value == "menu_round_trip") return tr("Menu round trip");
    if (value == "native_route_finished") return tr("Native route finished");
    return value;
}

const char *progress_text(CaseProgressState state) {
    switch (state) {
    case CaseProgressState::NotStarted: return tr("Not started");
    case CaseProgressState::Active: return tr("In progress");
    case CaseProgressState::Normal: return tr("Marked normal");
    case CaseProgressState::Abnormal: return tr("Marked abnormal");
    case CaseProgressState::Uncertain: return tr("Marked uncertain");
    case CaseProgressState::Skipped: return tr("Skipped");
    case CaseProgressState::Interrupted: return tr("Interrupted");
    }
    return tr("Needs review");
}

std::string role_text(std::string_view role) {
    if (role == "baseline" || role == "Baseline") return tr("Baseline");
    if (role == "candidate" || role == "Candidate") return tr("Candidate");
    return std::string(role);
}

std::size_t selected_case(const std::shared_ptr<CaseController> &controller) {
    Selection &state = selection();
    if (state.controller.lock() != controller) {
        state.controller = controller;
        state.index = 0;
        const auto &progress = controller->progress();
        const auto next = std::find_if(progress.begin(), progress.end(), [](const auto &item) {
            return item.state == CaseProgressState::NotStarted;
        });
        if (next != progress.end()) state.index = static_cast<std::size_t>(next - progress.begin());
    }
    if (const auto active = controller->active_case()) state.index = *active;
    const std::size_t count = controller->catalog().cases.size();
    if (count != 0) state.index = std::min(state.index, count - 1);
    return state.index;
}

void recording_status(const CaseController &controller) {
    section(tr("Recording"));
    if (controller.closed()) {
        paragraph(tr("Recording has ended. An unfinished case needs review."), colors::kDanger);
    } else if (!controller.recording_healthy()) {
        paragraph(tr("Recording needs attention. This run needs review."), colors::kDanger);
    } else {
        paragraph(tr("Recording is active."), colors::kGood);
    }
    if (controller.last_error() == "configuration_changed_during_case")
        paragraph(tr("Settings changed during this case. Close this test run; restoring a setting does not restore its validity."), colors::kDanger);
    else if (!controller.last_error().empty())
        paragraph(tr("The last test action could not be recorded."), colors::kDanger);
    paragraph(tr("Your marks describe what you saw. Results are checked after both runs."), colors::kTextDim);
}

void steps(const CaseSpec &spec) {
    section(tr("What to do"));
    for (std::size_t i = 0; i < spec.steps.size(); ++i) {
        paragraph(std::to_string(i + 1) + ".  " + std::string(case_text(spec.steps[i])));
        ImGui::Spacing();
    }
}

void checkpoints(const CaseSpec &spec, std::size_t next) {
    section(tr("Checkpoints"));
    if (spec.checkpoints.empty()) {
        paragraph(tr("This case has no checkpoints."), colors::kTextDim);
        return;
    }
    const std::size_t done = std::min(next, spec.checkpoints.size());
    progress_bar(static_cast<float>(done) / static_cast<float>(spec.checkpoints.size()),
                 std::to_string(done) + " / " + std::to_string(spec.checkpoints.size()));
    for (std::size_t i = 0; i < spec.checkpoints.size(); ++i) {
        const char *status = i < done ? tr("Recorded") : i == done ? tr("Next") : tr("Pending");
        paragraph(std::to_string(i + 1) + ".  " + std::string(checkpoint_text(spec.checkpoints[i])) +
                      "  —  " + status,
                  i < done ? colors::kGood : i == done ? colors::kAccentBright : colors::kTextDim);
    }
}

bool finish_row(CaseController &controller, CaseOutcome outcome, const char *label, const char *description,
                bool disabled, ImU32 color = colors::kText) {
    return button_row(label, {disabled, {}, description}, color) && controller.finish(outcome);
}

} // namespace

bool test_screen_available() noexcept { return static_cast<bool>(testing::active_case_controller()); }

bool draw_test_session_screen() {
    const std::shared_ptr<CaseController> controller = testing::active_case_controller();
    if (!controller) return false;

    section(tr("Test session"));
    info_row(tr("Test role"), role_text(controller->session().role));
    info_row(tr("Build version"), controller->session().build_version);
    recording_status(*controller);

    const auto &cases = controller->catalog().cases;
    const auto &progress = controller->progress();
    if (cases.empty() || progress.size() != cases.size()) {
        paragraph(tr("No test cases are available."), colors::kDanger);
        return false;
    }

    std::size_t index = selected_case(controller);
    section(tr("Case"));
    const bool active = controller->active_case().has_value();
    const std::string position = std::to_string(index + 1) + " / " + std::to_string(cases.size());
    const int direction = choice_row(tr("Choose case"), position,
                                     {active, {}, active ? tr("Finish the active case before choosing another.")
                                                        : tr("Use left and right to choose a case.")});
    if (direction != 0) {
        const auto count = static_cast<std::ptrdiff_t>(cases.size());
        const auto selected = static_cast<std::ptrdiff_t>(index);
        selection().index = static_cast<std::size_t>((selected + direction + count) % count);
        index = selection().index;
    }

    const CaseSpec &spec = cases[index];
    const testing::CaseProgress &case_progress = progress[index];
    heading(std::string(case_text(spec.title)));
    info_row(tr("Case status"), progress_text(controller->effective_state(index)));
    if (case_progress.attempt != 0)
        info_row(tr("Attempt"), std::to_string(case_progress.attempt));
    steps(spec);
    checkpoints(spec, case_progress.next_checkpoint);

    const bool healthy = !controller->closed() && controller->recording_healthy();
    if (!active) {
        section(tr("Start"));
        if (button_row(tr("Begin this case"),
                       {!healthy, {}, tr("Start recording this case, then return to the game.")},
                       colors::kAccentBright) && controller->begin(index))
            return true;
        return false;
    }

    section(tr("During the case"));
    if (button_row(tr("Record next checkpoint"),
                   {!healthy || case_progress.next_checkpoint >= spec.checkpoints.size(), {},
                    tr("Record the next checkpoint and return to the game.")},
                   colors::kAccentBright) && controller->checkpoint())
        return true;
    if (button_row(tr("Mark a problem"),
                   {!healthy, {}, tr("Record that you saw something unexpected, then return to the game.")},
                   colors::kDanger) && controller->anomaly())
        return true;
    paragraph(tr("Closing this menu keeps the case running."), colors::kTextDim);

    section(tr("Finish this case"));
    const bool all_checkpoints = case_progress.next_checkpoint == spec.checkpoints.size();
    if (finish_row(*controller, CaseOutcome::Normal, tr("Mark normal"),
                   tr("Use after every checkpoint when you saw no problem. Comparison comes later."),
                   !healthy || !all_checkpoints, colors::kGood))
        return false;
    if (!all_checkpoints)
        paragraph(tr("Record every checkpoint before marking normal."), colors::kTextDim);
    if (finish_row(*controller, CaseOutcome::Abnormal, tr("Mark abnormal"),
                   tr("Use when you saw a problem in the game."), !healthy, colors::kDanger))
        return false;
    if (finish_row(*controller, CaseOutcome::Uncertain, tr("Mark uncertain"),
                   tr("Use when you cannot decide what happened."), !healthy))
        return false;
    if (finish_row(*controller, CaseOutcome::Skipped, tr("Skip this case"),
                   tr("Use when you could not carry out this case."), !healthy))
        return false;
    return false;
}

void draw_test_session_hint() {
    const std::shared_ptr<CaseController> controller = testing::active_case_controller();
    if (!controller) return;
    const auto active = controller->active_case();
    if (!active || *active >= controller->catalog().cases.size() ||
        *active >= controller->progress().size())
        return;

    const CaseSpec &spec = controller->catalog().cases[*active];
    const std::size_t next = controller->progress()[*active].next_checkpoint;
    const ImGuiIO &io = ImGui::GetIO();
    const float layer_font = Layer::get().font_size();
    const float font = layer_font > 0.0f ? layer_font : ImGui::GetFontSize();
    ImGui::SetNextWindowPos({font * 0.5f, font * 0.5f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.76f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {font * 0.6f, font * 0.4f});
    ImGui::Begin("##test_session_hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(0.75f);
    const float wrap_width = std::max(font * 12.0f, std::min(font * 27.0f, io.DisplaySize.x * 0.48f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
    ImGui::TextUnformatted(tr("Test in progress"));
    ImGui::TextUnformatted(std::string(case_text(spec.title)).c_str());
    if (!controller->recording_healthy()) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors::kDanger);
        ImGui::TextUnformatted(tr("Recording needs attention. This run needs review."));
        ImGui::PopStyleColor();
    } else if (next < spec.checkpoints.size()) {
        ImGui::TextUnformatted(tr("Next checkpoint"));
        ImGui::TextUnformatted(std::string(checkpoint_text(spec.checkpoints[next])).c_str());
        ImGui::TextUnformatted(tr("Open Testing in the menu to record it."));
    } else {
        ImGui::TextUnformatted(tr("All checkpoints recorded. Open Testing to finish the case."));
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace mhp3rd::ui
