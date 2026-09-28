// The part of fast loading that decides, kept apart from the host so the
// tests can drive it (tests/fast_loading_tests.cpp).
#include "kernel/fast_loading.hpp"

namespace mhp3rd::fast_loading {

const char *reason_name(Reason reason) {
    switch (reason) {
    case Reason::None: return "none";
    case Reason::Disabled: return "off";
    case Reason::NotLoading: return "reads stopped";
    case Reason::Sound: return "sound";
    case Reason::Buttons: return "button held";
    case Reason::Movie: return "movie";
    case Reason::Online: return "ad hoc";
    case Reason::Menu: return "menu";
    }
    return "?";
}

void Detector::disc_read(std::uint64_t now_us) {
    read_seen_ = true;
    last_read_us_ = now_us;
}

bool Detector::audio(std::uint64_t now_us, int peak) {
    if (peak > kAudiblePeak) {
        sound_seen_ = true;
        last_sound_us_ = now_us;
        // Behind the black loading screen nothing is shown to go with the
        // sound, so it is dropped with the rest of the fast stretch.
        if (fast_ && loading_screen_) return true;
        if (fast_) {
            fast_ = false;
            reason_ = Reason::Sound;
        }
        return false;
    }
    return fast_;
}

bool Detector::update(std::uint64_t now_us, const Guards &guards) {
    const bool reading = read_seen_ && now_us - last_read_us_ <= kReadWindowUs;
    const bool loading = reading || guards.loading_screen;
    loading_screen_ = guards.loading_screen;
    // Outside a load every held button is a candidate to carry into the next
    // one; inside it only releases are followed.
    if (loading) carried_buttons_ &= guards.buttons;
    else carried_buttons_ = guards.buttons;
    const std::uint32_t pressed = guards.carry_held_buttons
        ? guards.buttons & ~carried_buttons_ : guards.buttons;
    Reason reason = Reason::None;
    if (!guards.enabled) reason = Reason::Disabled;
    else if (guards.online) reason = Reason::Online;
    else if (guards.movie) reason = Reason::Movie;
    else if (guards.menu) reason = Reason::Menu;
    else if (pressed != 0u) reason = Reason::Buttons;
    else if (!loading) reason = Reason::NotLoading;
    else if (!guards.loading_screen && sound_seen_ && now_us - last_sound_us_ < kQuietUs) reason = Reason::Sound;
    fast_ = reason == Reason::None;
    reason_ = reason;
    return fast_;
}

} // namespace mhp3rd::fast_loading
