#pragma once

#include "texture_decode.hpp"
#include "texture_decode_policy.hpp"
#include "resources/pixel_decode.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace mhp3rd::gpu {

// One immutable capture. Only the dispatcher constructs it; workers never
// receive guest memory or a pointer into it. The legacy snapshot is retained
// so verify and fallback decode precisely the captured input.
class OwnedTextureInput {
public:
    OwnedTextureInput(const OwnedTextureInput &) = delete;
    OwnedTextureInput &operator=(const OwnedTextureInput &) = delete;

private:
    friend class PortableTextureDispatcher;
    OwnedTextureInput(TextureSnapshot snapshot, resources::PixelDecodeSpec spec,
                      bool block_format, bool portable_supported)
        : snapshot_(std::move(snapshot)), spec_(spec), block_format_(block_format),
          portable_supported_(portable_supported) {}

    TextureSnapshot snapshot_;
    resources::PixelDecodeSpec spec_;
    bool block_format_{};
    bool portable_supported_{};
};

struct TextureDecodeCounters {
    std::uint64_t requests{};
    std::uint64_t immediate_requests{};
    std::uint64_t async_requests{};
    std::uint64_t async_capture_attempts{};
    std::uint64_t snapshot_rejected{};
    std::uint64_t unsupported_state{};
    std::uint64_t portable_success{};
    std::uint64_t verified{};
    std::uint64_t native{};
    std::uint64_t fallbacks{};
    std::uint64_t mismatches{};
    std::uint64_t errors{};
    std::uint64_t legacy_failures{};
    std::uint64_t portable_elapsed_ns{};
    std::uint64_t reference_elapsed_ns{};
};

// The renderer owns this for the lifetime of any submitted worker jobs.
// Modes are fixed at construction; no environment access occurs here.
class PortableTextureDispatcher {
public:
    explicit PortableTextureDispatcher(TextureDecodeMode mode) noexcept : mode_(mode) {}
    PortableTextureDispatcher(const PortableTextureDispatcher &) = delete;
    PortableTextureDispatcher &operator=(const PortableTextureDispatcher &) = delete;

    // Returns a packet only when the unchanged legacy snapshot path was
    // eligible. In Off mode it returns null, leaving the original worker path
    // available without any new capture behavior. DXT remains immediate.
    [[nodiscard]] std::shared_ptr<const OwnedTextureInput> prepare_async(
        const GuestMemory &memory, const TextureState &texture);

    // Decodes an owned packet on any thread. Verify compares independent
    // decodes and returns legacy pixels even on mismatch. Native returns
    // portable pixels when supported, or the unchanged legacy result.
    bool decode_owned(const OwnedTextureInput &input, std::vector<std::uint32_t> &out);

    // The immediate path also supports DXT via an exact bounded block capture.
    // Capture failure and unsupported state retain decode_texture behavior.
    bool decode_immediate(const GuestMemory &memory, const TextureState &texture,
                          std::vector<std::uint32_t> &out);

    [[nodiscard]] TextureDecodeCounters counters() const noexcept;
    [[nodiscard]] TextureDecodeMode mode() const noexcept { return mode_; }

private:
    [[nodiscard]] bool decode_captured(const OwnedTextureInput &input,
                                       std::vector<std::uint32_t> &out);
    [[nodiscard]] bool decode_reference(const OwnedTextureInput &input,
                                        std::vector<std::uint32_t> &out);

    const TextureDecodeMode mode_;
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> immediate_requests_{0};
    std::atomic<std::uint64_t> async_requests_{0};
    std::atomic<std::uint64_t> async_capture_attempts_{0};
    std::atomic<std::uint64_t> snapshot_rejected_{0};
    std::atomic<std::uint64_t> unsupported_state_{0};
    std::atomic<std::uint64_t> portable_success_{0};
    std::atomic<std::uint64_t> verified_{0};
    std::atomic<std::uint64_t> native_{0};
    std::atomic<std::uint64_t> fallbacks_{0};
    std::atomic<std::uint64_t> mismatches_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> legacy_failures_{0};
    std::atomic<std::uint64_t> portable_elapsed_ns_{0};
    std::atomic<std::uint64_t> reference_elapsed_ns_{0};
};

} // namespace mhp3rd::gpu
