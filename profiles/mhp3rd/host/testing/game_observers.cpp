#include "testing/game_observers.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace mhp3rd::testing {
namespace {
constexpr std::array<std::string_view, 8> kCommonKeys{
    "event", "domain", "guest_frame", "virtual_us", "vblank",
    "control_read_ordinal", "observation_ordinal", "focused",
};

bool reserved_key(std::string_view key) noexcept {
    for (const auto reserved : kCommonKeys) {
        if (key == reserved) return true;
    }
    return false;
}

const char *window_event_name(WindowEventKind kind) noexcept {
    switch (kind) {
    case WindowEventKind::Key: return "window.key";
    case WindowEventKind::MouseButton: return "window.mouse_button";
    case WindowEventKind::MouseMotion: return "window.mouse_motion";
    case WindowEventKind::MouseWheel: return "window.mouse_wheel";
    case WindowEventKind::Touch: return "window.touch";
    case WindowEventKind::GamepadButton: return "window.gamepad_button";
    case WindowEventKind::GamepadAxis: return "window.gamepad_axis";
    case WindowEventKind::DeviceAdded: return "window.device_added";
    case WindowEventKind::DeviceRemoved: return "window.device_removed";
    case WindowEventKind::Text: return "window.text";
    case WindowEventKind::FileDrop: return "window.file_drop";
    case WindowEventKind::Focus: return "input.focus";
    case WindowEventKind::Close: return "window.close";
    }
    return nullptr;
}

const char *camera_action_name(CameraObservation::Action action) noexcept {
    switch (action) {
    case CameraObservation::Action::Rate: return "rate";
    case CameraObservation::Action::Motion: return "motion";
    case CameraObservation::Action::Advance: return "advance";
    case CameraObservation::Action::ConsumeAll: return "consume_all";
    case CameraObservation::Action::ConsumeSource: return "consume_source";
    case CameraObservation::Action::Discard: return "discard";
    case CameraObservation::Action::Reset: return "reset";
    }
    return nullptr;
}

void add_finite(Fields &fields, const char *name, double value) {
    if (std::isfinite(value)) fields.push_back({name, value});
    else fields.push_back({name, nullptr});
    fields.push_back({std::string(name) + "_available", std::isfinite(value)});
}

bool valid_overlay(const OverlayIdentity &identity) noexcept {
    if (identity.image_size == 0 || identity.code_size == 0 ||
        identity.code_size > identity.image_size ||
        identity.header_fingerprint.empty() || identity.code_fingerprint.empty())
        return false;
    // An exclusive end of 2^32 is valid: the last covered address is UINT32_MAX.
    return static_cast<std::uint64_t>(identity.base) + identity.image_size <=
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
}

bool same_overlay(const OverlayIdentity &a, const OverlayIdentity &b) noexcept {
    return a.base == b.base && a.image_size == b.image_size &&
        a.code_size == b.code_size && a.name == b.name &&
        a.header_fingerprint == b.header_fingerprint &&
        a.code_fingerprint == b.code_fingerprint &&
        a.matched_corpus == b.matched_corpus;
}

void add_overlay_fields(Fields &fields, const OverlayObservation &observation) {
    const auto &identity = observation.identity;
    fields.push_back({"base", static_cast<std::uint64_t>(identity.base)});
    fields.push_back({"image_size", static_cast<std::uint64_t>(identity.image_size)});
    fields.push_back({"code_size", static_cast<std::uint64_t>(identity.code_size)});
    fields.push_back({"name", identity.name});
    fields.push_back({"header_fingerprint", identity.header_fingerprint});
    fields.push_back({"code_fingerprint", identity.code_fingerprint});
    fields.push_back({"matched_corpus", identity.matched_corpus});
    fields.push_back({"generation", observation.generation});
    fields.push_back({"generation_kind", std::string("code_validation_epoch")});
    fields.push_back({"code_epoch", observation.code_epoch});
    fields.push_back({"active", observation.active});
}
} // namespace

const char *domain_name(InputDomain domain) noexcept {
    switch (domain) {
    case InputDomain::Setup: return "setup";
    case InputDomain::Game: return "game";
    case InputDomain::PausedUi: return "paused_ui";
    case InputDomain::OverGameUi: return "over_game_ui";
    case InputDomain::TextInput: return "text_input";
    }
    return "unknown";
}

struct GameObserver::Impl {
    explicit Impl(std::shared_ptr<SessionRecorder> value) : recorder(std::move(value)) {}

    mutable std::mutex mutex;
    std::shared_ptr<SessionRecorder> recorder;
    Timeline timeline;
    InputDomain underlying_domain{InputDomain::Setup};
    bool text_active{};
    std::uint64_t current_code_epoch{};
    std::unordered_map<std::uint32_t, OverlayObservation> overlays;
    std::atomic<std::uint64_t> errors{};

    void fail() noexcept { errors.fetch_add(1, std::memory_order_relaxed); }

    // The observer mutex remains held through record(), so accepted records have
    // the same ordering as their observation ordinals. The recorder only queues
    // here; its worker owns every sink operation.
    bool emit_locked(EventKind kind, std::string_view event, Fields fields = {},
                     bool boundary = false) noexcept {
        try {
            if (!recorder || event.empty()) {
                fail();
                return false;
            }
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (fields[i].name.empty() || reserved_key(fields[i].name)) {
                    fail();
                    return false;
                }
                for (std::size_t j = 0; j < i; ++j) {
                    if (fields[i].name == fields[j].name) {
                        fail();
                        return false;
                    }
                }
            }
            if (timeline.observation_ordinal == std::numeric_limits<std::uint64_t>::max()) {
                fail();
                return false;
            }
            ++timeline.observation_ordinal;
            Fields payload;
            payload.reserve(fields.size() + kCommonKeys.size());
            payload.push_back({"event", std::string(event)});
            payload.push_back({"domain", std::string(domain_name(timeline.domain))});
            payload.push_back({"guest_frame", timeline.guest_frame});
            payload.push_back({"virtual_us", timeline.virtual_us ?
                FieldValue{*timeline.virtual_us} : FieldValue{nullptr}});
            payload.push_back({"vblank", timeline.vblank ?
                FieldValue{*timeline.vblank} : FieldValue{nullptr}});
            payload.push_back({"control_read_ordinal", timeline.control_read_ordinal});
            payload.push_back({"observation_ordinal", timeline.observation_ordinal});
            payload.push_back({"focused", timeline.focused});
            for (auto &field : fields) payload.push_back(std::move(field));
            if (recorder->record(kind, std::move(payload), boundary) != AppendResult::Accepted) {
                fail();
                return false;
            }
            return true;
        } catch (...) {
            fail();
            return false;
        }
    }

    void set_effective_domain_locked(InputDomain next) noexcept {
        if (timeline.domain == next) return;
        const auto previous = timeline.domain;
        timeline.domain = next;
        try {
            emit_locked(EventKind::State, "input.domain",
                        {{"from_domain", std::string(domain_name(previous))},
                         {"to_domain", std::string(domain_name(next))}});
        } catch (...) {
            fail();
        }
    }

    void set_focus_locked(bool next, const WindowObservation *source = nullptr) noexcept {
        if (timeline.focused == next) return;
        const bool previous = timeline.focused;
        timeline.focused = next;
        try {
            Fields fields{{"from_focused", previous}, {"to_focused", next}};
            if (source) {
                fields.push_back({"window_id", static_cast<std::uint64_t>(source->window_id)});
                fields.push_back({"owned_window", source->owned_window});
                fields.push_back({"source_timestamp_ns", source->source_timestamp_ns});
                fields.push_back({"ui_consumed", source->ui_consumed});
                fields.push_back({"scripted_mode", source->scripted_mode});
            }
            emit_locked(EventKind::Input, "input.focus", std::move(fields));
        } catch (...) {
            fail();
        }
    }
};

GameObserver::GameObserver(std::shared_ptr<SessionRecorder> recorder)
    : impl_(std::make_unique<Impl>(std::move(recorder))) {}
GameObserver::~GameObserver() = default;

InputDomain GameObserver::underlying_domain() const noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        return impl_->underlying_domain;
    } catch (...) {
        impl_->fail();
        return InputDomain::Setup;
    }
}

void GameObserver::emit(EventKind kind, std::string_view event, Fields fields, bool boundary) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        impl_->emit_locked(kind, event, std::move(fields), boundary);
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::domain(InputDomain value) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (value == InputDomain::TextInput) {
            impl_->text_active = true;
            impl_->set_effective_domain_locked(InputDomain::TextInput);
        } else if (value == InputDomain::Setup || value == InputDomain::Game ||
                   value == InputDomain::PausedUi || value == InputDomain::OverGameUi) {
            impl_->underlying_domain = value;
            if (!impl_->text_active) impl_->set_effective_domain_locked(value);
        } else {
            impl_->fail();
        }
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::text_input(bool active) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (impl_->text_active == active) return;
        impl_->text_active = active;
        impl_->set_effective_domain_locked(active ? InputDomain::TextInput : impl_->underlying_domain);
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::focus(bool focused) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        impl_->set_focus_locked(focused);
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::time(std::uint64_t virtual_us, std::uint64_t vblank) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        impl_->timeline.virtual_us = virtual_us;
        impl_->timeline.vblank = vblank;
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::frame(std::uint64_t virtual_us, std::uint64_t vblank) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (impl_->timeline.guest_frame == std::numeric_limits<std::uint64_t>::max()) {
            impl_->fail();
            return;
        }
        impl_->timeline.virtual_us = virtual_us;
        impl_->timeline.vblank = vblank;
        ++impl_->timeline.guest_frame;
        impl_->emit_locked(EventKind::State, "guest.frame");
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::window(const WindowObservation &observation) noexcept {
    const bool global_metadata = !observation.owned_window &&
        (observation.kind == WindowEventKind::DeviceAdded ||
         observation.kind == WindowEventKind::DeviceRemoved ||
         (observation.kind == WindowEventKind::Close && observation.window_id == 0));
    if (!observation.owned_window && !global_metadata) return;
    try {
        std::lock_guard lock(impl_->mutex);
        if (observation.kind == WindowEventKind::Focus) {
            impl_->set_focus_locked(observation.focused, &observation);
            return;
        }
        const char *name = window_event_name(observation.kind);
        if (!name) {
            impl_->fail();
            return;
        }
        const bool allowed_when_unfocused = observation.kind == WindowEventKind::Close ||
            observation.kind == WindowEventKind::DeviceAdded ||
            observation.kind == WindowEventKind::DeviceRemoved;
        if (!impl_->timeline.focused && !allowed_when_unfocused) return;
        Fields fields{{"window_id", static_cast<std::uint64_t>(observation.window_id)},
                      {"owned_window", observation.owned_window},
                      {"source_timestamp_ns", observation.source_timestamp_ns},
                      {"ui_consumed", observation.ui_consumed},
                      {"scripted_mode", observation.scripted_mode}};
        switch (observation.kind) {
        case WindowEventKind::Key:
        case WindowEventKind::MouseButton:
        case WindowEventKind::GamepadButton:
            fields.push_back({"device_id", observation.device_id});
            fields.push_back({"code", observation.code});
            fields.push_back({"down", observation.down});
            fields.push_back({"repeat", observation.repeat});
            break;
        case WindowEventKind::MouseMotion:
        case WindowEventKind::MouseWheel:
        case WindowEventKind::Touch:
        case WindowEventKind::GamepadAxis:
            fields.push_back({"device_id", observation.device_id});
            fields.push_back({"code", observation.code});
            add_finite(fields, "x", observation.x);
            add_finite(fields, "y", observation.y);
            break;
        case WindowEventKind::DeviceAdded:
        case WindowEventKind::DeviceRemoved:
            fields.push_back({"device_id", observation.device_id});
            fields.push_back({"code", observation.code});
            break;
        case WindowEventKind::Text:
        case WindowEventKind::FileDrop:
            fields.push_back({"text_bytes", observation.text_bytes});
            break;
        case WindowEventKind::Close:
        case WindowEventKind::Focus:
            break;
        }
        impl_->emit_locked(EventKind::Input, name, std::move(fields));
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::pad(const PadObservation &sample) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (impl_->timeline.control_read_ordinal == std::numeric_limits<std::uint64_t>::max()) {
            impl_->fail();
            return;
        }
        impl_->timeline.virtual_us = sample.virtual_us;
        impl_->timeline.vblank = sample.vblank;
        ++impl_->timeline.control_read_ordinal;
        impl_->emit_locked(EventKind::Input, "input.pad",
                           {{"buttons", static_cast<std::uint64_t>(sample.buttons)},
                            {"sample_count", static_cast<std::uint64_t>(sample.count)},
                            {"analog_x", static_cast<std::uint64_t>(sample.analog_x)},
                            {"analog_y", static_cast<std::uint64_t>(sample.analog_y)},
                            {"right_x", static_cast<std::uint64_t>(sample.right_x)},
                            {"right_y", static_cast<std::uint64_t>(sample.right_y)}});
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::camera(const CameraObservation &motion) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        const char *action = camera_action_name(motion.action);
        if (!action) {
            impl_->fail();
            return;
        }
        Fields fields{{"action", std::string(action)},
                      {"source", static_cast<std::uint64_t>(motion.source)},
                      {"yaw_held", motion.yaw_held}, {"pitch_held", motion.pitch_held}};
        add_finite(fields, "yaw", motion.yaw);
        add_finite(fields, "pitch", motion.pitch);
        add_finite(fields, "seconds", motion.seconds);
        add_finite(fields, "degrees_per_second", motion.degrees_per_second);
        impl_->emit_locked(EventKind::Input, "camera.observation", std::move(fields));
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::script(std::string_view action, std::uint64_t script_frame,
                          std::string_view safe_argument) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        // Text and drop payloads are private even if a caller accidentally
        // supplies their raw argument.
        if (action == "text" || action == "drop") safe_argument = {};
        impl_->emit_locked(EventKind::Input, "script.action",
                           {{"action", std::string(action)}, {"script_frame", script_frame},
                            {"safe_argument", std::string(safe_argument)}});
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::code_epoch(std::string_view reason) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (impl_->current_code_epoch == std::numeric_limits<std::uint64_t>::max()) {
            impl_->fail();
            return;
        }
        ++impl_->current_code_epoch;
        std::uint64_t invalidated = 0;
        for (auto &[base, observation] : impl_->overlays) {
            (void)base;
            if (observation.active) {
                observation.active = false;
                ++invalidated;
            }
        }
        impl_->emit_locked(EventKind::State, "overlay.code_epoch",
                           {{"code_epoch", impl_->current_code_epoch},
                            {"invalidated_slots", invalidated},
                            {"reason", std::string(reason)}});
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::overlay(const OverlayIdentity &identity) noexcept {
    if (!valid_overlay(identity)) {
        try {
            std::lock_guard lock(impl_->mutex);
            // A malformed refresh cannot certify the previous image at this
            // address. Keep its generation history, but remove attribution.
            if (auto it = impl_->overlays.find(identity.base); it != impl_->overlays.end())
                it->second.active = false;
            impl_->fail();
        } catch (...) {
            impl_->fail();
        }
        return;
    }
    try {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->overlays.find(identity.base);
        if (it != impl_->overlays.end() && it->second.active &&
            it->second.code_epoch == impl_->current_code_epoch &&
            same_overlay(it->second.identity, identity)) return;
        if (it != impl_->overlays.end() &&
            it->second.generation == std::numeric_limits<std::uint64_t>::max()) {
            impl_->fail();
            return;
        }
        const auto generation = it == impl_->overlays.end() ? 1u : it->second.generation + 1u;
        OverlayObservation next{identity, generation, impl_->current_code_epoch, true};
        if (it == impl_->overlays.end()) impl_->overlays.emplace(identity.base, next);
        else it->second = next;
        Fields fields;
        add_overlay_fields(fields, next);
        if (!impl_->emit_locked(EventKind::State, "overlay.load", std::move(fields))) {
            // Without a recorded identity, later address attribution would be
            // impossible to interpret from the journal.
            impl_->overlays.find(identity.base)->second.active = false;
        }
    } catch (...) {
        impl_->fail();
    }
}

void GameObserver::overlay_unload(std::uint32_t base, std::string_view reason) noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->overlays.find(base);
        const bool was_active = it != impl_->overlays.end() && it->second.active;
        Fields fields{{"base", static_cast<std::uint64_t>(base)},
                      {"code_epoch", impl_->current_code_epoch},
                      {"active", false},
                      {"known", it != impl_->overlays.end()},
                      {"was_active", was_active},
                      {"reason", std::string(reason)}};
        if (it != impl_->overlays.end()) {
            it->second.active = false;
            fields.push_back({"generation", it->second.generation});
            fields.push_back({"header_fingerprint", it->second.identity.header_fingerprint});
            fields.push_back({"code_fingerprint", it->second.identity.code_fingerprint});
        }
        impl_->emit_locked(EventKind::State, "overlay.unload", std::move(fields));
    } catch (...) {
        impl_->fail();
    }
}

std::optional<OverlayObservation> GameObserver::overlay_at(std::uint32_t address) const noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        const OverlayObservation *found = nullptr;
        for (const auto &[base, observation] : impl_->overlays) {
            if (!observation.active || observation.code_epoch != impl_->current_code_epoch) continue;
            const std::uint64_t end = static_cast<std::uint64_t>(base) + observation.identity.image_size;
            if (address < base || static_cast<std::uint64_t>(address) >= end) continue;
            if (found) return std::nullopt;
            found = &observation;
        }
        if (!found || !found->identity.matched_corpus) return std::nullopt;
        return *found;
    } catch (...) {
        impl_->fail();
        return std::nullopt;
    }
}

Timeline GameObserver::timeline() const noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        return impl_->timeline;
    } catch (...) {
        impl_->fail();
        return {};
    }
}

RecorderHealth GameObserver::recorder_health() const {
    return impl_->recorder ? impl_->recorder->health() : RecorderHealth{};
}

std::uint64_t GameObserver::emission_errors() const noexcept {
    return impl_->errors.load(std::memory_order_relaxed);
}

namespace {
std::shared_ptr<GameObserver> current_observer;
}

std::shared_ptr<GameObserver> active_observer() noexcept {
    return std::atomic_load_explicit(&current_observer, std::memory_order_acquire);
}

void set_active_observer(std::shared_ptr<GameObserver> observer) noexcept {
    // The displaced shared_ptr is destroyed after the atomic exchange returns,
    // outside the routing primitive and any observer/recorder lock.
    auto displaced = std::atomic_exchange_explicit(&current_observer, std::move(observer),
                                                    std::memory_order_acq_rel);
}
} // namespace mhp3rd::testing
