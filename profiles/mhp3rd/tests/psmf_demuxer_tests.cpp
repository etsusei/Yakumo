#include "movie/psmf_demuxer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using mhp3rd::movie::AccessUnit;
using mhp3rd::movie::PsmfDemuxer;

int failures = 0;

void check(bool condition, const char *description) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition) ++failures;
}

std::vector<std::uint8_t> stamp(std::int64_t ticks, std::uint8_t prefix) {
    return {static_cast<std::uint8_t>(prefix | (((ticks >> 30u) & 7u) << 1u) | 1u),
            static_cast<std::uint8_t>(ticks >> 22u),
            static_cast<std::uint8_t>((((ticks >> 15u) & 0x7Fu) << 1u) | 1u),
            static_cast<std::uint8_t>(ticks >> 7u),
            static_cast<std::uint8_t>(((ticks & 0x7Fu) << 1u) | 1u)};
}

std::vector<std::uint8_t> pes(std::uint8_t stream, const std::vector<std::uint8_t> &payload,
                              std::optional<std::int64_t> pts = std::nullopt,
                              std::optional<std::int64_t> dts = std::nullopt) {
    std::vector<std::uint8_t> optional;
    if (pts) {
        auto bytes = stamp(*pts, dts ? 0x30u : 0x20u);
        optional.insert(optional.end(), bytes.begin(), bytes.end());
    }
    if (dts) {
        auto bytes = stamp(*dts, 0x10u);
        optional.insert(optional.end(), bytes.begin(), bytes.end());
    }
    const std::size_t length = 3u + optional.size() + payload.size();
    std::vector<std::uint8_t> bytes{0u, 0u, 1u, stream, static_cast<std::uint8_t>(length >> 8u),
                                    static_cast<std::uint8_t>(length), 0x80u,
                                    static_cast<std::uint8_t>(dts ? 0xC0u : pts ? 0x80u : 0u),
                                    static_cast<std::uint8_t>(optional.size())};
    bytes.insert(bytes.end(), optional.begin(), optional.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> pack(const std::vector<std::vector<std::uint8_t>> &packets, std::size_t stuffing = 0u) {
    std::vector<std::uint8_t> bytes(mhp3rd::movie::kPackSize);
    bytes[2] = 1u;
    bytes[3] = 0xBAu;
    bytes[13] = static_cast<std::uint8_t>(stuffing);
    std::size_t at = 14u + stuffing;
    for (const auto &packet : packets) {
        if (at + packet.size() > bytes.size()) throw std::runtime_error("synthetic PES exceeds pack");
        std::copy(packet.begin(), packet.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        at += packet.size();
    }
    return bytes;
}

std::vector<std::uint8_t> video(std::uint8_t marker) {
    return {0u, 0u, 1u, 9u, 0xF0u, marker};
}

std::vector<std::uint8_t> frame(std::uint8_t marker) {
    // Parameters: two channels and 16 payload bytes, giving a 24-byte frame.
    std::vector<std::uint8_t> bytes{0x0Fu, 0xD0u, 0x08u, 0x01u, 0u, 0u, 0u, 0u};
    bytes.insert(bytes.end(), 16u, marker);
    return bytes;
}

std::vector<std::uint8_t> private_audio(const std::vector<std::uint8_t> &bytes, std::uint8_t substream = 0u) {
    std::vector<std::uint8_t> payload{substream, 0u, 0u, 0u};
    payload.insert(payload.end(), bytes.begin(), bytes.end());
    return payload;
}

void test_video_packets_and_timestamps() {
    PsmfDemuxer demuxer;
    const auto first = video(0x11u);
    auto first_part = first;
    first_part.insert(first_part.end(), {0u, 0u});
    check(demuxer.push_pack(pack({pes(0xC0u, first), pes(0xE0u, first_part, 90'000, 87'000)})),
          "accepts a pack containing an ignored stream and a video PES");
    check(!demuxer.video_ready() && !demuxer.pop_video(),
          "waits for a following access-unit delimiter before releasing a picture");
    check(demuxer.push_pack(pack({pes(0xE0u, {1u, 9u, 0xF0u, 0x22u})})),
          "accepts a delimiter split across pack and PES boundaries");
    auto unit = demuxer.pop_video();
    check(unit && unit->data == first && unit->pts == 90'000 && unit->dts == 87'000,
          "returns the first picture with its explicit PTS and DTS");
    check(!demuxer.video_ready(), "keeps the final picture pending until end of stream");
    demuxer.end_of_stream();
    unit = demuxer.pop_video();
    check(unit && unit->data == std::vector<std::uint8_t>({0u, 0u, 1u, 9u, 0xF0u, 0x22u}) &&
              unit->pts == 93'003 && unit->dts == 90'003,
          "flushes the final picture and extrapolates its timestamps");
    check(!demuxer.pop_video(), "does not duplicate pictures after end of stream");

    demuxer.reset();
    auto four_byte_start = video(0x33u);
    four_byte_start.insert(four_byte_start.begin(), 0u);
    auto together = four_byte_start;
    const auto following = video(0x44u);
    together.insert(together.end(), following.begin(), following.end());
    constexpr std::int64_t high_pts = (std::int64_t{1} << 32u) + 12345;
    check(demuxer.push_pack(pack({pes(0xE0u, together, high_pts)}, 5u)),
          "skips pack stuffing before parsing a PTS-only video PES");
    unit = demuxer.pop_video();
    check(unit && unit->data == four_byte_start && unit->pts == high_pts && unit->dts == high_pts,
          "preserves a four-byte start code and the upper timestamp bit");
    demuxer.end_of_stream();
    unit = demuxer.pop_video();
    check(unit && unit->data == following && unit->pts == high_pts + 3003 && unit->dts == high_pts + 3003,
          "splits two pictures in one PES and extrapolates after PTS-only headers");
}

void test_audio_frames_and_reset() {
    PsmfDemuxer demuxer;
    const auto first = frame(0x31u);
    const auto second = frame(0x42u);
    std::vector<std::uint8_t> initial(first.begin(), first.begin() + 10);
    check(demuxer.push_pack(pack({pes(0xBDu, private_audio(first, 2u)),
                                  pes(0xBDu, private_audio(initial), 45'000)})) &&
              !demuxer.audio_ready(),
          "ignores another private substream and retains a partial audio frame");
    std::vector<std::uint8_t> remainder(first.begin() + 10, first.end());
    remainder.insert(remainder.end(), second.begin(), second.end());
    check(demuxer.push_pack(pack({pes(0xBDu, private_audio(remainder))})),
          "joins an audio frame across PES packets");
    check(demuxer.audio_frame_size() == 24u && demuxer.audio_channels() == 2u,
          "reports the complete frame size and channel count");
    auto unit = demuxer.pop_audio();
    check(unit && unit->data == std::vector<std::uint8_t>(16u, 0x31u) &&
              unit->pts == 45'000 && unit->dts == 45'000,
          "strips the frame header and applies its PES timestamp");
    unit = demuxer.pop_audio();
    check(unit && unit->data == std::vector<std::uint8_t>(16u, 0x42u) &&
              unit->pts == 49'179 && unit->dts == 49'179,
          "extrapolates the next audio frame at 44.1 kHz");
    check(!demuxer.audio_ready(), "audio queue drains completely");

    std::vector<std::uint8_t> out_of_sync{0x77u, 0x66u};
    out_of_sync.insert(out_of_sync.end(), first.begin(), first.end());
    check(demuxer.push_pack(pack({pes(0xBDu, private_audio(out_of_sync))})) && demuxer.audio_ready(),
          "resynchronizes after non-frame bytes");
    demuxer.reset();
    check(!demuxer.audio_ready() && !demuxer.video_ready() && demuxer.audio_frame_size() == 0u &&
              demuxer.audio_channels() == 0u,
          "reset clears queued data and stream format");
    check(demuxer.push_pack(pack({pes(0xBDu, private_audio(first))})), "accepts audio after reset");
    unit = demuxer.pop_audio();
    check(unit && unit->pts == -1 && unit->dts == -1,
          "reset clears timestamp history rather than extrapolating from the previous stream");

    demuxer.reset();
    const std::vector<std::uint8_t> truncated(first.begin(), first.begin() + 12);
    check(demuxer.push_pack(pack({pes(0xBDu, private_audio(truncated))})), "accepts a partial final audio frame");
    demuxer.end_of_stream();
    check(!demuxer.pop_audio(), "does not emit a truncated audio frame at end of stream");
}

void test_malformed_input() {
    PsmfDemuxer demuxer;
    check(!demuxer.push_pack({}) && !demuxer.push_pack(std::vector<std::uint8_t>(13u)) &&
              !demuxer.push_pack(std::vector<std::uint8_t>(mhp3rd::movie::kPackSize)),
          "rejects empty, short, and non-pack input");
    const auto good = pes(0xE0u, video(0x55u));

    auto oversized = good;
    oversized[4] = 0x08u;
    oversized[5] = 0x00u;
    check(demuxer.push_pack(pack({oversized})), "accepts the pack marker despite a truncated PES");
    demuxer.end_of_stream();
    check(!demuxer.pop_video(), "never emits bytes from a PES whose declared length exceeds the pack");

    demuxer.reset();
    auto short_header = good;
    short_header[8] = 120u;
    check(demuxer.push_pack(pack({short_header})), "accepts a pack with a malformed PES header");
    demuxer.end_of_stream();
    check(!demuxer.pop_video(), "ignores a PES whose optional header exceeds its declared body");

    demuxer.reset();
    auto missing_pts = good;
    missing_pts[7] = 0x80u;
    check(demuxer.push_pack(pack({missing_pts})), "accepts a pack with missing PTS bytes");
    demuxer.end_of_stream();
    check(!demuxer.pop_video(), "ignores a PES whose flags require absent timestamp bytes");

    demuxer.reset();
    std::vector<std::uint8_t> partial = pack({good});
    partial.resize(14u + good.size() - 2u);
    check(demuxer.push_pack(partial), "accepts a short pack span with a declared PES crossing its end");
    demuxer.end_of_stream();
    check(!demuxer.pop_video(), "does not release partial video from a short pack span");

    demuxer.reset();
    const std::vector<std::uint8_t> tiny{0u, 0u, 1u, 0xE0u, 0u, 2u, 0x80u, 0u};
    check(demuxer.push_pack(pack({tiny, good})), "accepts a pack with an undersized PES before a good PES");
    demuxer.end_of_stream();
    auto unit = demuxer.pop_video();
    check(unit && unit->data == video(0x55u) && !demuxer.pop_video(),
          "skips an undersized PES without consuming the following valid packet");
}

} // namespace

int main() {
    test_video_packets_and_timestamps();
    test_audio_frames_and_reset();
    test_malformed_input();
    std::printf("PSMF demuxer tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
