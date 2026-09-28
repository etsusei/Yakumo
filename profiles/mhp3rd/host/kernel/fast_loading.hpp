#pragma once

#include <cstdint>

// Fast loading: emulated time runs ahead of real time while the game loads,
// and only then.
//
// The disc reads themselves are instant here, yet a load takes as long as on a
// PSP: the game's loader threads read, check and unpack the data a piece per
// frame or so and wait on the emulated clock in between, which the kernel
// holds to real time. While a load is under way that hold is let go, so the
// load takes as long as the host needs for the work, not the PSP's time.
//
// A load is recognised from what the game does, not from a timer: it is
// reading the disc (DATA.BIN and the rest), and it is silent. Anything that
// could be gameplay keeps real time: sound coming out, a button pressed
// during the load, a movie, the in-game menu, ad hoc play (other players' time
// must stay real) and the Game speed setting, which already lets time run
// free. Buttons already held when the load began, such as dash held while
// running into the next area, may stay held: the game cannot act on them while
// it loads, and their release still reaches it at the next poll.
namespace mhp3rd::fast_loading {

// How long after its last disc read the game still counts as loading, in
// emulated microseconds. A load reads at least every few hundred milliseconds;
// the rest of the time its threads unpack and wait on the clock.
inline constexpr std::uint64_t kReadWindowUs = 500'000u;
// How long the game must have been silent before a load may run fast, so the
// last sound before it plays out in full.
inline constexpr std::uint64_t kQuietUs = 250'000u;
// The loudest sample, after the channel's volume, that still counts as
// silence: none. Loading screens hand the audio exact zeros, so only buffers
// the sink would mix as zeros are ever dropped, and the first sample of a
// sound, however quiet, ends a fast stretch and is played. (A threshold of
// 64 cut the first few milliseconds of a fade-in.)
inline constexpr int kAudiblePeak = 0;
// Emulated time runs at most this many times faster than real time. An M1
// reaches about 12 in the heaviest part of a load, where the game checks what
// it read; the cap only bounds a faster machine.
inline constexpr double kMaxSpeed = 16.0;

// What keeps real time regardless of the loading, sampled at each update.
struct Guards {
    bool enabled{};       // the setting is on, Game speed is Normal and there is a window
    std::uint32_t buttons{};  // the buttons and D-pad directions down now (the sticks do not count)
    // Let buttons held since before the load stay held while it runs fast.
    // Off (MHP3RD_FAST_LOADING_HELD=0): any button down keeps real time.
    bool carry_held_buttons{};
    bool movie{};         // a movie is playing
    bool online{};        // ad hoc networking is on, or a session is going
    bool menu{};          // the in-game menu is open over the game
};

// Why fast loading is off at the moment, for the log.
enum class Reason { None, Disabled, NotLoading, Sound, Buttons, Movie, Online, Menu };
[[nodiscard]] const char *reason_name(Reason reason);

// The decision itself, without the host around it, so tests can drive it.
class Detector {
public:
    // The game read from the disc at emulated time `now_us`.
    void disc_read(std::uint64_t now_us);
    // The game handed a buffer to sceAudio whose loudest sample, after the
    // channel's volume, is `peak`. Sound ends fast loading at once. Returns
    // true when the buffer is silence played while time runs fast, which the
    // caller then drops rather than play it faster than real time.
    bool audio(std::uint64_t now_us, int peak);
    // Decides whether emulated time may run ahead now, and returns that.
    bool update(std::uint64_t now_us, const Guards &guards);

    [[nodiscard]] bool fast() const noexcept { return fast_; }
    // Why the last update or audio call left it off; None while fast.
    [[nodiscard]] Reason reason() const noexcept { return reason_; }

private:
    bool read_seen_{};
    std::uint64_t last_read_us_{};
    bool sound_seen_{};
    std::uint64_t last_sound_us_{};
    bool fast_{};
    Reason reason_{Reason::NotLoading};
    // Buttons held since before the current load; releases drop out of it,
    // so pressing one again counts as a new press.
    std::uint32_t carried_buttons_{};
};

// The running game's detector and what goes with it: the log lines, the
// kernel's clock and the renderer's presents. Everything below is called on
// the emulation thread.
void note_disc_read();
// See Detector::audio. `peak` already has the channel's volume applied.
[[nodiscard]] bool note_audio(int peak);
// The buttons and D-pad directions the game just read.
void note_buttons(std::uint32_t buttons);
// Re-evaluates at a vblank and tells the kernel whether it may run ahead.
void update();
// Whether emulated time runs ahead of real time at the moment.
[[nodiscard]] bool active();

} // namespace mhp3rd::fast_loading
