#pragma once

#include "gpu/portable_texture_dispatch.hpp"
#include "testing/game_observers.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace mhp3rd::testing {
// Called on the renderer thread. Periodic counters may include in-flight work;
// the caller drains all owned jobs before setting final=true. Observer identity
// is captured once and never rebound to a subsequent recording session.
class TextureDecodeObservation {
public:
    using Clock = std::chrono::steady_clock;
    TextureDecodeObservation(std::shared_ptr<GameObserver> observer, gpu::TextureDecodeMode mode)
        : observer_(std::move(observer)), mode_(mode) {
        (void)gpu::texture_decode_mode_name(mode_);
    }

    bool report(const gpu::TextureDecodeCounters &counters, bool final,
                Clock::time_point now = Clock::now()) noexcept {
        if (finalized_ || mode_ == gpu::TextureDecodeMode::Off) return false;
        const auto observer = observer_.lock();
        if (!observer) return false;
        if (!final && reported_ && now - previous_ < std::chrono::seconds(1)) return false;
        // Finalization is one-shot even if the sink reports a failure. Recorder
        // health or the explicit error prevents that failure becoming a pass.
        finalized_ = final;
        reported_ = true;
        previous_ = now;
        try {
            observer->emit(EventKind::State, "texture_decode.counters", {
                {"schema", std::string(gpu::kTextureDecodeSchema)},
                {"mode", std::string(gpu::texture_decode_mode_name(mode_))},
                {"scope", std::string("renderer_cache_miss_decodes_not_all_draws")},
                {"final", final}, {"workers_drained", final},
                {"requests", counters.requests},
                {"immediate_requests", counters.immediate_requests},
                {"async_requests", counters.async_requests},
                {"async_capture_attempts", counters.async_capture_attempts},
                {"snapshot_rejected", counters.snapshot_rejected},
                {"unsupported_state", counters.unsupported_state},
                {"portable_success", counters.portable_success},
                {"verified", counters.verified}, {"native", counters.native},
                {"fallbacks", counters.fallbacks}, {"mismatches", counters.mismatches},
                {"errors", counters.errors}, {"legacy_failures", counters.legacy_failures},
                {"portable_elapsed_ns", counters.portable_elapsed_ns},
                {"reference_elapsed_ns", counters.reference_elapsed_ns},
            }, final);
            return true;
        } catch (...) {
            observer->emit(EventKind::Error, "texture_decode.observation_failed", {}, true);
            return false;
        }
    }
private:
    std::weak_ptr<GameObserver> observer_;
    gpu::TextureDecodeMode mode_;
    Clock::time_point previous_{};
    bool reported_{}, finalized_{};
};
} // namespace mhp3rd::testing
