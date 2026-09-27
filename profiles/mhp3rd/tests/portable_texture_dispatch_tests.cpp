#include "gpu/portable_texture_dispatch.hpp"
#include "gpu/texture_decode.hpp"
#include "perf/frame_stats.hpp"
#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// This test links the frozen software decoder without the frame statistics
// subsystem. Keep its ordinary fast route selected, as in the earlier pixel
// differential oracle.
namespace mhp3rd::perf {
bool alternate_off(NewPath) { return false; }
} // namespace mhp3rd::perf

namespace {
using mhp3rd::gpu::OwnedTextureInput;
using mhp3rd::gpu::PortableTextureDispatcher;
using mhp3rd::gpu::TextureDecodeCounters;
using mhp3rd::gpu::TextureDecodeMode;
using mhp3rd::gpu::TextureFormat;
using mhp3rd::gpu::TextureSnapshot;
using mhp3rd::gpu::TextureState;
using psprecomp::GuestMemory;

constexpr std::uint32_t kTexelAddress = 0x08010000u;
constexpr std::uint32_t kPaletteAddress = 0x08080000u;
constexpr std::uint32_t kSentinel = 0x7A5CA531u;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void check_policy() {
    using mhp3rd::gpu::parse_texture_decode_mode;
    using mhp3rd::gpu::require_texture_decode_policy;
    using mhp3rd::gpu::texture_decode_mode_name;
    const auto rejects = [](auto action) {
        try {
            action();
        } catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    require(parse_texture_decode_mode(nullptr) == TextureDecodeMode::Off &&
                parse_texture_decode_mode("off") == TextureDecodeMode::Off &&
                parse_texture_decode_mode("verify") == TextureDecodeMode::Verify &&
                parse_texture_decode_mode("native") == TextureDecodeMode::Native,
            "texture policy did not parse its exact modes");
    require(std::string{texture_decode_mode_name(TextureDecodeMode::Off)} == "off" &&
                std::string{texture_decode_mode_name(TextureDecodeMode::Verify)} == "verify" &&
                std::string{texture_decode_mode_name(TextureDecodeMode::Native)} == "native",
            "texture policy mode names changed");
    require(rejects([] { (void)parse_texture_decode_mode(""); }) &&
                rejects([] { (void)parse_texture_decode_mode("1"); }) &&
                rejects([] { (void)parse_texture_decode_mode("Native"); }),
            "texture policy accepted an unknown mode");
    require_texture_decode_policy(TextureDecodeMode::Off, true, false);
    require_texture_decode_policy(TextureDecodeMode::Verify, false, true);
    require_texture_decode_policy(TextureDecodeMode::Native, false, true);
    require(rejects([] { require_texture_decode_policy(TextureDecodeMode::Verify, true, true); }) &&
                rejects([] { require_texture_decode_policy(TextureDecodeMode::Native, true, true); }) &&
                rejects([] { require_texture_decode_policy(TextureDecodeMode::Verify, false, false); }) &&
                rejects([] { require_texture_decode_policy(TextureDecodeMode::Native, false, false); }),
            "texture policy permitted candidate modes in a sealed or absent renderer");
}

bool is_indexed(TextureFormat format) {
    return format == TextureFormat::Clut4 || format == TextureFormat::Clut8 ||
           format == TextureFormat::Clut16 || format == TextureFormat::Clut32;
}

bool is_dxt(TextureFormat format) {
    return format == TextureFormat::Dxt1 || format == TextureFormat::Dxt3 ||
           format == TextureFormat::Dxt5;
}

std::uint32_t bits_per_texel(TextureFormat format) {
    switch (format) {
    case TextureFormat::Clut4: return 4u;
    case TextureFormat::Clut8: return 8u;
    case TextureFormat::Rgba5650:
    case TextureFormat::Rgba5551:
    case TextureFormat::Rgba4444:
    case TextureFormat::Clut16: return 16u;
    case TextureFormat::Rgba8888:
    case TextureFormat::Clut32: return 32u;
    default: return 0u;
    }
}

struct Fixture {
    TextureFormat format{};
    std::uint16_t width{}, height{};
    std::uint32_t stride{};
    bool swizzled{};
    std::uint32_t palette_format{};
    std::uint32_t seed{};

    [[nodiscard]] std::string label() const {
        return "format " + std::to_string(static_cast<unsigned>(format)) + ", " +
               std::to_string(width) + "x" + std::to_string(height) + ", stride " +
               std::to_string(stride) + (swizzled ? ", swizzled" : ", linear");
    }

    [[nodiscard]] std::size_t texel_bytes() const {
        if (is_dxt(format)) {
            const std::size_t blocks_x = (static_cast<std::size_t>(width) + 3u) / 4u;
            const std::size_t blocks_y = (static_cast<std::size_t>(height) + 3u) / 4u;
            return blocks_x * blocks_y * (format == TextureFormat::Dxt1 ? 8u : 16u);
        }
        const std::uint32_t pitch = (stride == 0u ? width : stride) * bits_per_texel(format) / 8u;
        return static_cast<std::size_t>(pitch) * height;
    }

    [[nodiscard]] TextureState state() const {
        TextureState state{};
        state.enabled = true;
        state.address = kTexelAddress;
        state.width = width;
        state.height = height;
        state.buffer_width = stride;
        state.format = format;
        state.swizzled = swizzled;
        state.clut_address = kPaletteAddress;
        state.clut_format = palette_format;
        state.clut_shift = format == TextureFormat::Clut32 ? 19u : 0u;
        state.clut_mask = 0x3Fu;
        state.clut_offset = 3u;
        return state;
    }
};

struct Encoded {
    std::vector<std::uint8_t> texels;
    std::vector<std::uint8_t> palette;
};

Encoded make_encoded(const Fixture &fixture) {
    std::mt19937 random{fixture.seed};
    Encoded encoded;
    encoded.texels.resize(fixture.texel_bytes());
    for (auto &byte : encoded.texels) byte = static_cast<std::uint8_t>(random());
    if (encoded.texels.size() >= 4u) {
        encoded.texels[0] = 0u;
        encoded.texels[1] = 0xFFu;
        encoded.texels[2] = 0x81u;
        encoded.texels[3] = 0x7Eu;
    }
    if (is_indexed(fixture.format)) {
        encoded.palette.resize(512u * (fixture.palette_format == 3u ? 4u : 2u));
        for (auto &byte : encoded.palette) byte = static_cast<std::uint8_t>(random());
    }
    return encoded;
}

void install(GuestMemory &memory, const Encoded &encoded) {
    memory.copy_in(kTexelAddress, encoded.texels);
    if (!encoded.palette.empty()) memory.copy_in(kPaletteAddress, encoded.palette);
}

struct DecodeOutput {
    bool ok{};
    std::vector<std::uint32_t> pixels{kSentinel};
};

DecodeOutput reference(const GuestMemory &memory, const TextureState &state) {
    DecodeOutput output;
    output.ok = mhp3rd::gpu::decode_texture(memory, state, output.pixels);
    return output;
}

void same_output(const DecodeOutput &actual, const DecodeOutput &expected, const std::string &where) {
    require(actual.ok == expected.ok, where + ": return value differs from frozen decoder");
    require(actual.pixels == expected.pixels, where + ": output differs from frozen decoder");
}

DecodeOutput immediate(PortableTextureDispatcher &dispatcher, const GuestMemory &memory,
                       const TextureState &state) {
    DecodeOutput output;
    output.ok = dispatcher.decode_immediate(memory, state, output.pixels);
    return output;
}

DecodeOutput owned(PortableTextureDispatcher &dispatcher, const OwnedTextureInput &input) {
    DecodeOutput output;
    output.ok = dispatcher.decode_owned(input, output.pixels);
    return output;
}

std::vector<Fixture> cases() {
    std::vector<Fixture> all;
    for (unsigned id = 0u; id != 11u; ++id) {
        const auto format = static_cast<TextureFormat>(id);
        if (is_dxt(format)) {
            all.push_back({format, 7u, 9u, 0u, false, 0u, 0xA5700000u + id});
            // The existing DXT decoder ignores the swizzle state. Check the
            // dispatcher's state mapping on that original software behavior.
            all.push_back({format, 12u, 8u, 17u, true, 0u, 0xA5701000u + id});
            continue;
        }
        const auto palette_format = id % 4u;
        all.push_back({format, 11u, 7u, 13u, false, palette_format, 0xA5702000u + id});
        const auto aligned_stride = 128u / bits_per_texel(format);
        all.push_back({format, static_cast<std::uint16_t>(aligned_stride + 3u), 8u,
                       aligned_stride, true, palette_format, 0xA5703000u + id});
        // Unaligned swizzle is a deliberate old-decoder bypass, not a strict
        // PSP layout. It must still reach the same pixels in every mode.
        all.push_back({format, 17u, 9u, 19u, true, palette_format, 0xA5704000u + id});
    }
    return all;
}

void check_valid_modes() {
    GuestMemory memory;
    PortableTextureDispatcher off{TextureDecodeMode::Off};
    PortableTextureDispatcher verify{TextureDecodeMode::Verify};
    PortableTextureDispatcher native{TextureDecodeMode::Native};
    std::size_t immediate_count = 0u;
    std::size_t async_count = 0u;
    for (const Fixture &fixture : cases()) {
        const Encoded encoded = make_encoded(fixture);
        const TextureState state = fixture.state();
        install(memory, encoded);
        const DecodeOutput expected = reference(memory, state);
        require(expected.ok && !expected.pixels.empty(), fixture.label() + ": bad oracle fixture");

        same_output(immediate(off, memory, state), expected, "off immediate " + fixture.label());
        same_output(immediate(verify, memory, state), expected, "verify immediate " + fixture.label());
        same_output(immediate(native, memory, state), expected, "native immediate " + fixture.label());
        ++immediate_count;

        require(!off.prepare_async(memory, state), "off captured async " + fixture.label());
        if (!is_dxt(fixture.format)) {
            TextureSnapshot old_snapshot;
            require(mhp3rd::gpu::snapshot_texture(memory, state, old_snapshot),
                    "off worker route could not capture " + fixture.label());
            DecodeOutput old_worker;
            old_worker.ok = mhp3rd::gpu::decode_snapshot(old_snapshot, old_worker.pixels);
            same_output(old_worker, expected, "off worker " + fixture.label());
        }
        const auto verified_packet = verify.prepare_async(memory, state);
        const auto native_packet = native.prepare_async(memory, state);
        if (is_dxt(fixture.format)) {
            require(!verified_packet && !native_packet, "DXT unexpectedly left immediate route " + fixture.label());
            continue;
        }
        require(verified_packet && native_packet, "valid async capture rejected " + fixture.label());
        same_output(owned(verify, *verified_packet), expected, "verify async " + fixture.label());
        same_output(owned(native, *native_packet), expected, "native async " + fixture.label());
        ++async_count;
    }
    const TextureDecodeCounters off_counts = off.counters();
    const TextureDecodeCounters verify_counts = verify.counters();
    const TextureDecodeCounters native_counts = native.counters();
    require(off.mode() == TextureDecodeMode::Off && verify.mode() == TextureDecodeMode::Verify &&
                native.mode() == TextureDecodeMode::Native, "dispatch mode changed");
    require(off_counts.verified == 0u && off_counts.native == 0u && off_counts.portable_success == 0u,
            "off mode recorded portable work");
    require(verify_counts.verified == immediate_count + async_count && verify_counts.native == 0u &&
                verify_counts.mismatches == 0u, "verify mode did not compare every supported request");
    require(native_counts.native == immediate_count + async_count && native_counts.verified == 0u &&
                native_counts.mismatches == 0u, "native mode did not select every supported request");
    require(verify_counts.portable_success == immediate_count + async_count &&
                native_counts.portable_success == immediate_count + async_count,
            "supported fixtures missed the portable core");
    std::cout << "valid dispatch: " << immediate_count << " immediate fixtures and "
              << async_count << " worker fixtures per candidate mode\n";
}

void check_owned_lifetime_and_threads(TextureDecodeMode mode) {
    PortableTextureDispatcher dispatcher{mode};
    std::vector<std::shared_ptr<const OwnedTextureInput>> packets;
    std::vector<DecodeOutput> expected;
    constexpr std::size_t kPacketCount = 8u;
    {
        auto memory = std::make_unique<GuestMemory>();
        const auto fixtures = cases();
        for (const Fixture &fixture : fixtures) {
            if (is_dxt(fixture.format) || packets.size() == kPacketCount) continue;
            const Encoded encoded = make_encoded(fixture);
            install(*memory, encoded);
            const TextureState state = fixture.state();
            expected.push_back(reference(*memory, state));
            auto packet = dispatcher.prepare_async(*memory, state);
            require(packet != nullptr, "owned capture rejected " + fixture.label());
            packets.push_back(std::move(packet));
            memory->zero(kTexelAddress, encoded.texels.size());
            if (!encoded.palette.empty()) memory->zero(kPaletteAddress, encoded.palette.size());
        }
    }
    require(packets.size() == kPacketCount, "insufficient owned packets");
    for (std::size_t i = 0u; i < packets.size(); ++i)
        same_output(owned(dispatcher, *packets[i]), expected[i], "released source packet " + std::to_string(i));

    constexpr std::size_t kWorkers = 4u;
    constexpr std::size_t kRounds = 8u;
    std::array<std::exception_ptr, kWorkers> failures{};
    std::array<std::thread, kWorkers> workers;
    for (std::size_t worker = 0u; worker < kWorkers; ++worker) {
        workers[worker] = std::thread([&, worker] {
            try {
                for (std::size_t round = 0u; round < kRounds; ++round) {
                    for (std::size_t i = 0u; i < packets.size(); ++i)
                        same_output(owned(dispatcher, *packets[i]), expected[i],
                                    "thread " + std::to_string(worker) + " packet " + std::to_string(i));
                }
            } catch (...) {
                failures[worker] = std::current_exception();
            }
        });
    }
    for (auto &worker : workers) worker.join();
    for (const auto &failure : failures) if (failure) std::rethrow_exception(failure);

    const TextureDecodeCounters counts = dispatcher.counters();
    const std::size_t calls = kPacketCount * (1u + kWorkers * kRounds);
    require(counts.async_capture_attempts == kPacketCount && counts.async_requests == calls,
            "threaded packet/call accounting differs");
    require(counts.requests == calls && counts.mismatches == 0u && counts.errors == 0u,
            "threaded session counts differ");
    require((mode == TextureDecodeMode::Verify ? counts.verified : counts.native) == calls,
            "threaded mode count lost or duplicated calls");
    require((mode == TextureDecodeMode::Verify ? counts.native : counts.verified) == 0u,
            "threaded mode count crossed policy");
    std::cout << "owned " << (mode == TextureDecodeMode::Verify ? "verify" : "native")
              << ": " << kPacketCount << " released-source packets, " << calls
              << " concurrent decodes\n";
}

void check_fallback_case(PortableTextureDispatcher &dispatcher, const GuestMemory &memory,
                         const TextureState &state, const std::string &label,
                         bool expect_async_rejection = true) {
    const DecodeOutput expected = reference(memory, state);
    const TextureDecodeCounters before = dispatcher.counters();
    if (expect_async_rejection)
        require(!dispatcher.prepare_async(memory, state), label + ": malformed input captured for worker");
    same_output(immediate(dispatcher, memory, state), expected, label);
    const TextureDecodeCounters after = dispatcher.counters();
    require(after.verified == before.verified && after.native == before.native,
            label + ": unsupported context recorded as portable verification/native");
    require(after.fallbacks > before.fallbacks, label + ": fallback was not recorded");
}

void check_rejections() {
    for (TextureDecodeMode mode : {TextureDecodeMode::Verify, TextureDecodeMode::Native}) {
        PortableTextureDispatcher dispatcher{mode};
        GuestMemory memory;
        const Fixture fixture{TextureFormat::Clut8, 8u, 8u, 8u, false, 3u, 0xBADF00Du};
        const Encoded encoded = make_encoded(fixture);
        install(memory, encoded);
        const TextureState valid = fixture.state();

        TextureState zero_width = valid;
        zero_width.width = 0u;
        check_fallback_case(dispatcher, memory, zero_width, "zero width");
        TextureState over_limit = valid;
        over_limit.width = 1025u;
        check_fallback_case(dispatcher, memory, over_limit, "width over legacy limit");
        TextureState unknown = valid;
        unknown.format = static_cast<TextureFormat>(11u);
        check_fallback_case(dispatcher, memory, unknown, "unknown format");
        TextureState noncanonical_palette = valid;
        noncanonical_palette.clut_format = 7u; // old immediate switch reads 32-bit entries
        check_fallback_case(dispatcher, memory, noncanonical_palette, "noncanonical CLUT format", false);

        // A short encoded run makes the frozen decoder return false after
        // filling opaque black. Keep that exact output and return value.
        GuestMemory short_memory;
        TextureState short_texel = valid;
        short_texel.format = TextureFormat::Rgba8888;
        short_texel.width = 8u;
        short_texel.height = 2u;
        short_texel.buffer_width = 8u;
        short_texel.address = GuestMemory::kPhysicalBase + short_memory.size() - 32u;
        check_fallback_case(dispatcher, short_memory, short_texel, "missing texel bytes");

        // The old decoder throws when a mapped CLUT entry falls outside RAM.
        // The new route may reject it, but it cannot count such a context as
        // verified or native or silently replace the old observable exception.
        GuestMemory short_palette;
        TextureState missing_palette = valid;
        missing_palette.clut_address = GuestMemory::kPhysicalBase + short_palette.size() - 4u;
        const std::array<std::uint8_t, 64u> indices{};
        short_palette.copy_in(kTexelAddress, indices);
        const TextureDecodeCounters before = dispatcher.counters();
        require(!dispatcher.prepare_async(short_palette, missing_palette),
                "missing palette captured for worker");
        bool reference_threw = false;
        bool dispatcher_threw = false;
        try {
            (void)reference(short_palette, missing_palette);
        } catch (const psprecomp::Error &) {
            reference_threw = true;
        }
        try {
            (void)immediate(dispatcher, short_palette, missing_palette);
        } catch (const psprecomp::Error &) {
            dispatcher_threw = true;
        }
        require(reference_threw && dispatcher_threw, "missing palette exception changed");
        const TextureDecodeCounters after = dispatcher.counters();
        require(after.verified == before.verified && after.native == before.native,
                "missing palette counted as portable success");
    }
    std::cout << "rejection and fallback contracts passed\n";
}

void check_palette_beyond_worker_snapshot() {
    const Fixture fixture{TextureFormat::Clut8, 8u, 8u, 8u, false, 3u, 0x512512u};
    Encoded encoded = make_encoded(fixture);
    std::fill(encoded.texels.begin(), encoded.texels.end(), 0u);
    encoded.palette.resize(513u * 4u);
    encoded.palette[512u * 4u + 0u] = 0x44u;
    encoded.palette[512u * 4u + 1u] = 0x33u;
    encoded.palette[512u * 4u + 2u] = 0x22u;
    encoded.palette[512u * 4u + 3u] = 0x11u;
    GuestMemory memory;
    install(memory, encoded);
    TextureState state = fixture.state();
    state.clut_mask = 0u;
    state.clut_offset = 32u; // entry 512, beyond the unchanged worker snapshot
    const DecodeOutput direct = reference(memory, state);
    require(direct.ok && std::all_of(direct.pixels.begin(), direct.pixels.end(),
                                      [](std::uint32_t pixel) { return pixel == 0x11223344u; }),
            "entry-512 direct fixture did not read its real palette bytes");
    TextureSnapshot snapshot;
    require(mhp3rd::gpu::snapshot_texture(memory, state, snapshot),
            "entry-512 worker fixture could not be captured by legacy path");
    DecodeOutput worker_reference;
    worker_reference.ok = mhp3rd::gpu::decode_snapshot(snapshot, worker_reference.pixels);
    require(worker_reference.ok && worker_reference.pixels != direct.pixels,
            "entry-512 fixture did not distinguish direct and worker legacy paths");

    for (TextureDecodeMode mode : {TextureDecodeMode::Verify, TextureDecodeMode::Native}) {
        PortableTextureDispatcher dispatcher{mode};
        same_output(immediate(dispatcher, memory, state), direct,
                    "entry-512 immediate legacy fallback");
        const auto packet = dispatcher.prepare_async(memory, state);
        require(packet != nullptr, "entry-512 legacy worker eligibility changed");
        same_output(owned(dispatcher, *packet), worker_reference,
                    "entry-512 worker legacy fallback");
        const TextureDecodeCounters counts = dispatcher.counters();
        require(counts.verified == 0u && counts.native == 0u && counts.portable_success == 0u &&
                    counts.fallbacks == 2u && counts.unsupported_state == 2u,
                "entry-512 unsupported context counted as portable work");
    }
    std::cout << "palette beyond worker snapshot preserves both legacy routes\n";
}
} // namespace

int main() {
    try {
        check_policy();
        check_valid_modes();
        check_owned_lifetime_and_threads(TextureDecodeMode::Verify);
        check_owned_lifetime_and_threads(TextureDecodeMode::Native);
        check_rejections();
        check_palette_beyond_worker_snapshot();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "portable texture dispatch tests failed: " << error.what() << '\n';
        return 1;
    }
}
