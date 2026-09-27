#include "portable_texture_dispatch.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace mhp3rd::gpu {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

bool is_block_format(TextureFormat format) {
    return format == TextureFormat::Dxt1 || format == TextureFormat::Dxt3 ||
           format == TextureFormat::Dxt5;
}

bool is_indexed(TextureFormat format) {
    return format == TextureFormat::Clut4 || format == TextureFormat::Clut8 ||
           format == TextureFormat::Clut16 || format == TextureFormat::Clut32;
}

// Map the renderer's format explicitly into the resource boundary.
bool pixel_spec(const TextureState &texture, resources::PixelDecodeSpec &spec) {
    switch (texture.format) {
    case TextureFormat::Rgba5650: spec.format = resources::PixelFormat::Rgba5650; break;
    case TextureFormat::Rgba5551: spec.format = resources::PixelFormat::Rgba5551; break;
    case TextureFormat::Rgba4444: spec.format = resources::PixelFormat::Rgba4444; break;
    case TextureFormat::Rgba8888: spec.format = resources::PixelFormat::Rgba8888; break;
    case TextureFormat::Clut4: spec.format = resources::PixelFormat::Clut4; break;
    case TextureFormat::Clut8: spec.format = resources::PixelFormat::Clut8; break;
    case TextureFormat::Clut16: spec.format = resources::PixelFormat::Clut16; break;
    case TextureFormat::Clut32: spec.format = resources::PixelFormat::Clut32; break;
    case TextureFormat::Dxt1: spec.format = resources::PixelFormat::Dxt1; break;
    case TextureFormat::Dxt3: spec.format = resources::PixelFormat::Dxt3; break;
    case TextureFormat::Dxt5: spec.format = resources::PixelFormat::Dxt5; break;
    default: return false;
    }
    if (texture.width == 0u || texture.height == 0u ||
        texture.width > 1024u || texture.height > 1024u ||
        texture.buffer_width > 65535u)
        return false;

    spec.width = texture.width;
    spec.height = texture.height;
    spec.stride_pixels = texture.buffer_width;
    spec.layout = texture.swizzled ? resources::PixelLayout::LegacySwizzleCompatibility
                                   : resources::PixelLayout::Linear;

    // A direct or block texture never reads a palette. Ignore stale CLUT
    // registers left by the game rather than rejecting an unrelated texture.
    if (!is_indexed(texture.format)) return true;
    if (texture.clut_format > 3u || texture.clut_shift > 31u ||
        texture.clut_mask > 255u || texture.clut_offset > 31u)
        return false;
    switch (texture.clut_format) {
    case 0u: spec.palette_format = resources::PaletteFormat::Rgba5650; break;
    case 1u: spec.palette_format = resources::PaletteFormat::Rgba5551; break;
    case 2u: spec.palette_format = resources::PaletteFormat::Rgba4444; break;
    case 3u: spec.palette_format = resources::PaletteFormat::Rgba8888; break;
    default: return false;
    }
    spec.palette_shift = texture.clut_shift;
    spec.palette_mask = texture.clut_mask;
    spec.palette_offset = texture.clut_offset;
    return true;
}

} // namespace

std::shared_ptr<const OwnedTextureInput> PortableTextureDispatcher::prepare_async(
    const GuestMemory &memory, const TextureState &texture) {
    async_capture_attempts_.fetch_add(1u, std::memory_order_relaxed);
    if (mode_ == TextureDecodeMode::Off || is_block_format(texture.format)) return {};

    TextureSnapshot snapshot;
    if (!snapshot_texture(memory, texture, snapshot)) {
        snapshot_rejected_.fetch_add(1u, std::memory_order_relaxed);
        return {};
    }
    resources::PixelDecodeSpec spec;
    const bool supported = pixel_spec(texture, spec);
    return std::shared_ptr<const OwnedTextureInput>(
        new OwnedTextureInput(std::move(snapshot), spec, false, supported));
}

bool PortableTextureDispatcher::decode_reference(const OwnedTextureInput &input,
                                                  std::vector<std::uint32_t> &out) {
    const auto start = Clock::now();
    try {
        bool ok = false;
        if (input.block_format_) {
            // Only decode_immediate constructs a block packet. Its complete
            // block window is copied into private reusable guest memory so the
            // unchanged decoder sees exactly the captured input in Verify.
            thread_local GuestMemory oracle_memory;
            constexpr std::uint32_t address = GuestMemory::kPhysicalBase;
            oracle_memory.copy_in(address, input.snapshot_.texels);
            TextureState texture = input.snapshot_.texture;
            texture.address = address;
            ok = decode_texture(oracle_memory, texture, out);
        } else {
            // decode_snapshot unswizzles in place, so each comparison/fallback
            // receives an independent copy of the immutable captured bytes.
            TextureSnapshot reference = input.snapshot_;
            ok = decode_snapshot(reference, out);
        }
        reference_elapsed_ns_.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
        if (!ok) legacy_failures_.fetch_add(1u, std::memory_order_relaxed);
        return ok;
    } catch (...) {
        reference_elapsed_ns_.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
        legacy_failures_.fetch_add(1u, std::memory_order_relaxed);
        errors_.fetch_add(1u, std::memory_order_relaxed);
        throw;
    }
}

bool PortableTextureDispatcher::decode_captured(const OwnedTextureInput &input,
                                                 std::vector<std::uint32_t> &out) {
    if (mode_ == TextureDecodeMode::Off) return decode_reference(input, out);
    if (!input.portable_supported_) {
        unsupported_state_.fetch_add(1u, std::memory_order_relaxed);
        fallbacks_.fetch_add(1u, std::memory_order_relaxed);
        return decode_reference(input, out);
    }

    const auto start = Clock::now();
    resources::PixelDecodeResult decoded = resources::decode_pixels(
        input.spec_, std::span<const std::uint8_t>(input.snapshot_.texels),
        std::span<const std::uint8_t>(input.snapshot_.clut), 1024u * 1024u);
    portable_elapsed_ns_.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
    if (!decoded.ok()) {
        errors_.fetch_add(1u, std::memory_order_relaxed);
        fallbacks_.fetch_add(1u, std::memory_order_relaxed);
        return decode_reference(input, out);
    }
    portable_success_.fetch_add(1u, std::memory_order_relaxed);

    if (mode_ == TextureDecodeMode::Native) {
        out = std::move(decoded.rgba);
        native_.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }

    const bool ok = decode_reference(input, out);
    if (ok) {
        verified_.fetch_add(1u, std::memory_order_relaxed);
        if (decoded.rgba != out) mismatches_.fetch_add(1u, std::memory_order_relaxed);
    }
    return ok;
}

bool PortableTextureDispatcher::decode_owned(const OwnedTextureInput &input,
                                              std::vector<std::uint32_t> &out) {
    requests_.fetch_add(1u, std::memory_order_relaxed);
    async_requests_.fetch_add(1u, std::memory_order_relaxed);
    return decode_captured(input, out);
}

bool PortableTextureDispatcher::decode_immediate(const GuestMemory &memory,
                                                  const TextureState &texture,
                                                  std::vector<std::uint32_t> &out) {
    requests_.fetch_add(1u, std::memory_order_relaxed);
    immediate_requests_.fetch_add(1u, std::memory_order_relaxed);
    const auto old_path = [&]() {
        const auto start = Clock::now();
        try {
            const bool ok = decode_texture(memory, texture, out);
            reference_elapsed_ns_.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
            if (!ok) legacy_failures_.fetch_add(1u, std::memory_order_relaxed);
            return ok;
        } catch (...) {
            reference_elapsed_ns_.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
            legacy_failures_.fetch_add(1u, std::memory_order_relaxed);
            errors_.fetch_add(1u, std::memory_order_relaxed);
            throw;
        }
    };
    if (mode_ == TextureDecodeMode::Off) return old_path();

    resources::PixelDecodeSpec spec;
    if (!pixel_spec(texture, spec)) {
        // An immediate legacy decode may consult palette addresses beyond the
        // 512-entry worker snapshot for an invalid GE state. Validate before
        // capture so that the original return/partial-output/throw is retained.
        unsupported_state_.fetch_add(1u, std::memory_order_relaxed);
        fallbacks_.fetch_add(1u, std::memory_order_relaxed);
        return old_path();
    }

    TextureSnapshot snapshot;
    const bool block = is_block_format(texture.format);
    if (block) {
        const std::size_t blocks_x = (static_cast<std::size_t>(texture.width) + 3u) / 4u;
        const std::size_t blocks_y = (static_cast<std::size_t>(texture.height) + 3u) / 4u;
        const std::size_t block_bytes = texture.format == TextureFormat::Dxt1 ? 8u : 16u;
        const std::size_t total = blocks_x * blocks_y * block_bytes;
        if (!memory.contains(texture.address, total)) {
            snapshot_rejected_.fetch_add(1u, std::memory_order_relaxed);
            fallbacks_.fetch_add(1u, std::memory_order_relaxed);
            return old_path();
        }
        snapshot.texture = texture;
        snapshot.texels.resize(total);
        memory.copy_out(texture.address, snapshot.texels);
    } else if (!snapshot_texture(memory, texture, snapshot)) {
        snapshot_rejected_.fetch_add(1u, std::memory_order_relaxed);
        fallbacks_.fetch_add(1u, std::memory_order_relaxed);
        return old_path();
    }

    const OwnedTextureInput input(std::move(snapshot), spec, block, true);
    return decode_captured(input, out);
}

TextureDecodeCounters PortableTextureDispatcher::counters() const noexcept {
    TextureDecodeCounters snapshot;
    snapshot.requests = requests_.load(std::memory_order_relaxed);
    snapshot.immediate_requests = immediate_requests_.load(std::memory_order_relaxed);
    snapshot.async_requests = async_requests_.load(std::memory_order_relaxed);
    snapshot.async_capture_attempts = async_capture_attempts_.load(std::memory_order_relaxed);
    snapshot.snapshot_rejected = snapshot_rejected_.load(std::memory_order_relaxed);
    snapshot.unsupported_state = unsupported_state_.load(std::memory_order_relaxed);
    snapshot.portable_success = portable_success_.load(std::memory_order_relaxed);
    snapshot.verified = verified_.load(std::memory_order_relaxed);
    snapshot.native = native_.load(std::memory_order_relaxed);
    snapshot.fallbacks = fallbacks_.load(std::memory_order_relaxed);
    snapshot.mismatches = mismatches_.load(std::memory_order_relaxed);
    snapshot.errors = errors_.load(std::memory_order_relaxed);
    snapshot.legacy_failures = legacy_failures_.load(std::memory_order_relaxed);
    snapshot.portable_elapsed_ns = portable_elapsed_ns_.load(std::memory_order_relaxed);
    snapshot.reference_elapsed_ns = reference_elapsed_ns_.load(std::memory_order_relaxed);
    return snapshot;
}

} // namespace mhp3rd::gpu
