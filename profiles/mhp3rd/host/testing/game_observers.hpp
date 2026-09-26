#pragma once

#include "testing/session_recorder.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mhp3rd::testing {
enum class InputDomain { Setup, Game, PausedUi, OverGameUi, TextInput };
[[nodiscard]] const char *domain_name(InputDomain domain) noexcept;
struct Timeline {
    std::uint64_t guest_frame{};
    std::optional<std::uint64_t> virtual_us;
    std::optional<std::uint64_t> vblank;
    std::uint64_t control_read_ordinal{};
    std::uint64_t observation_ordinal{};
    InputDomain domain{InputDomain::Setup};
    bool focused{};
};
enum class WindowEventKind {
    Key, MouseButton, MouseMotion, MouseWheel, Touch,
    GamepadButton, GamepadAxis, DeviceAdded, DeviceRemoved, Text, FileDrop, Focus, Close,
};
struct WindowObservation {
    WindowEventKind kind{};
    bool owned_window{};
    bool focused{};
    bool ui_consumed{};
    bool scripted_mode{};
    bool down{};
    bool repeat{};
    std::uint32_t window_id{};
    std::uint64_t source_timestamp_ns{};
    std::int64_t device_id{};
    std::int64_t code{};
    double x{}, y{};
    std::uint64_t text_bytes{}; // contents and file paths are not captured
};
struct PadObservation {
    std::uint64_t virtual_us{}, vblank{};
    std::uint32_t buttons{}, count{1};
    std::uint8_t analog_x{128}, analog_y{128}, right_x{128}, right_y{128};
};
struct CameraObservation {
    enum class Action { Rate, Motion, Advance, ConsumeAll, ConsumeSource, Discard, Reset };
    Action action{};
    std::uint32_t source{4}; // 0 stick, 1 keys, 2 mouse, 3 touch, 4 aggregate
    double yaw{}, pitch{}, seconds{}, degrees_per_second{};
    bool yaw_held{}, pitch_held{};
};
struct OverlayIdentity {
    std::uint32_t base{}, image_size{}, code_size{};
    std::string name;
    std::string header_fingerprint, code_fingerprint;
    bool matched_corpus{};
};
struct OverlayObservation {
    OverlayIdentity identity;
    std::uint64_t generation{}, code_epoch{};
    bool active{};
};
class GameObserver {
public:
    explicit GameObserver(std::shared_ptr<SessionRecorder> recorder);
    ~GameObserver();
    GameObserver(const GameObserver &) = delete;
    GameObserver &operator=(const GameObserver &) = delete;
    // Logging failures never propagate into gameplay. An error count remains
    // visible even if no additional journal record can be enqueued.
    void emit(EventKind kind, std::string_view event, Fields fields = {}, bool boundary = false) noexcept;
    void domain(InputDomain domain) noexcept;
    void text_input(bool active) noexcept; // temporarily overrides the underlying UI domain
    void focus(bool focused) noexcept;
    void time(std::uint64_t virtual_us, std::uint64_t vblank) noexcept;
    void frame(std::uint64_t virtual_us, std::uint64_t vblank) noexcept;
    void window(const WindowObservation &event) noexcept;
    void pad(const PadObservation &sample) noexcept;
    void camera(const CameraObservation &motion) noexcept;
    void script(std::string_view action, std::uint64_t script_frame, std::string_view safe_argument) noexcept;
    // A code epoch is an observed I-cache invalidation, not an asserted count
    // of loads. Identical images in a new epoch get a distinct generation.
    void code_epoch(std::string_view reason) noexcept;
    void overlay(const OverlayIdentity &identity) noexcept;
    void overlay_unload(std::uint32_t base, std::string_view reason) noexcept;
    [[nodiscard]] std::optional<OverlayObservation> overlay_at(std::uint32_t address) const noexcept;
    [[nodiscard]] Timeline timeline() const noexcept;
    [[nodiscard]] InputDomain underlying_domain() const noexcept;
    [[nodiscard]] RecorderHealth recorder_health() const;
    [[nodiscard]] std::uint64_t emission_errors() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// The disabled path does not allocate or write. In-flight observers own their
// shared lifetime when the runtime detaches the current session.
[[nodiscard]] std::shared_ptr<GameObserver> active_observer() noexcept;
void set_active_observer(std::shared_ptr<GameObserver> observer) noexcept;
} // namespace mhp3rd::testing
