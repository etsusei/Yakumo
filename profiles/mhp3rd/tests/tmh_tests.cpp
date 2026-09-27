#include "resources/tmh.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using mhp3rd::resources::IndexedBundle;
using mhp3rd::resources::SharedBytes;
using mhp3rd::resources::TmhDescriptor;
using mhp3rd::resources::TmhView;

static_assert(std::is_same_v<decltype(std::declval<const TmhView &>().records()),
                             std::span<const mhp3rd::resources::TmhRecord>>);
static_assert(std::is_same_v<decltype(std::declval<const TmhDescriptor &>().image.bytes()),
                             std::span<const std::uint8_t>>);

int failures = 0;

void check(bool condition, const char *description) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition) ++failures;
}

void put32(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes.at(at + i) = static_cast<std::uint8_t>(value >> (8u * i));
    }
}

std::uint32_t dimensions(std::uint16_t width, std::uint16_t height) {
    return static_cast<std::uint32_t>(width) |
           (static_cast<std::uint32_t>(height) << 16u);
}

struct BlockBytes {
    std::uint32_t tag;
    std::uint32_t format;
    std::uint32_t word12;
    std::vector<std::uint8_t> body;
};

void append_block(std::vector<std::uint8_t> &record, const BlockBytes &block) {
    const auto at = record.size();
    record.resize(at + 16u);
    put32(record, at, static_cast<std::uint32_t>(16u + block.body.size()));
    put32(record, at + 4u, block.tag);
    put32(record, at + 8u, block.format);
    put32(record, at + 12u, block.word12);
    record.insert(record.end(), block.body.begin(), block.body.end());
}

std::vector<std::uint8_t> make_record(const std::vector<BlockBytes> &blocks,
                                      std::uint32_t preceding = 1,
                                      std::uint32_t word4 = 0,
                                      std::uint32_t word12 = 0) {
    std::vector<std::uint8_t> record(16u);
    put32(record, 4, word4);
    put32(record, 8, preceding);
    put32(record, 12, word12);
    for (const auto &block : blocks) append_block(record, block);
    put32(record, 0, static_cast<std::uint32_t>(record.size()));
    return record;
}

std::vector<std::uint8_t> make_tmh(
    std::initializer_list<std::vector<std::uint8_t>> records,
    const std::vector<std::uint8_t> &tail = {}, std::uint32_t reserved = 0) {
    std::vector<std::uint8_t> bytes{
        '.', 'T', 'M', 'H', '0', '.', '1', '4', 0, 0, 0, 0, 0, 0, 0, 0
    };
    put32(bytes, 8, static_cast<std::uint32_t>(records.size()));
    put32(bytes, 12, reserved);
    for (const auto &record : records) {
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}

bool rejects(const std::vector<std::uint8_t> &bytes,
             std::size_t max_records = 4096) {
    try {
        (void)TmhView::parse(SharedBytes::take(bytes), max_records);
    } catch (const std::invalid_argument &) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename F>
bool throws_invalid(F &&action) {
    try {
        std::forward<F>(action)();
    } catch (const std::invalid_argument &) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename F>
bool throws_out_of_range(F &&action) {
    try {
        std::forward<F>(action)();
    } catch (const std::out_of_range &) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

void test_clut4_padding_tail_and_raw_fields() {
    // Synthetic 68x68 CLUT4: 2312 nominal bytes and 8 extra encoded bytes.
    std::vector<std::uint8_t> image(2320u, 0x34u);
    std::fill(image.end() - 8, image.end(), 0xE7u);
    std::vector<std::uint8_t> palette(40u, 0x5Au); // 16 entries x 2 plus 8 extra.
    const auto record = make_record({
        {0xAABBCCDDu, 4u, dimensions(68, 68), image},
        {0x01020304u, 1u, 0xBEEF0010u, palette}
    }, 1u, 0x76543210u, 0xABCDEF01u);
    std::vector<std::uint8_t> tail(544u, 0x91u);
    tail.front() = 0x7Cu;
    const auto view = TmhView::parse(SharedBytes::take(make_tmh({record}, tail, 0x13579BDFu)));
    const auto descriptor = view.descriptor(0);

    check(view.records().size() == 1 && view.reserved_word() == 0x13579BDFu &&
              view.records()[0].offset == 16u && view.records()[0].size == 2408u &&
              view.records()[0].word4 == 0x76543210u &&
              view.records()[0].preceding_subblocks == 1u &&
              view.records()[0].word12 == 0xABCDEF01u,
          "retains header and record words without interpreting unknown values");
    check(view.records()[0].blocks.size() == 2 &&
              view.records()[0].blocks[0].offset == 32u &&
              view.records()[0].blocks[0].size == 2336u &&
              view.records()[0].blocks[0].tag == 0xAABBCCDDu &&
              view.records()[0].blocks[1].tag == 0x01020304u &&
              view.records()[0].blocks[1].word12 == 0xBEEF0010u,
          "retains block strides, tags, and the palette count's unknown upper word");
    check(descriptor.image_offset == 48u && descriptor.image_format == 4u &&
              descriptor.width == 68u && descriptor.height == 68u &&
              descriptor.image.bytes().size() == 2320u &&
              descriptor.image.bytes().back() == 0xE7u &&
              descriptor.image.bytes().data() == view.parent().bytes().data() + 48u,
          "exposes the complete encoded CLUT4 payload with its eight extra bytes");
    check(descriptor.palette_offset == 2384u && descriptor.palette_format == 1u &&
              descriptor.palette_count == 16 && descriptor.palette &&
              descriptor.palette->bytes().size() == 40u &&
              descriptor.palette->bytes().data() == view.parent().bytes().data() + 2384u,
          "exposes an owned palette slice and preserves its padding");
    check(view.consumed_bytes() == 2424u && view.parent().bytes().size() == 2968u &&
              view.parent().bytes()[view.consumed_bytes()] == 0x7Cu &&
              view.parent().bytes().back() == 0x91u,
          "retains a nonzero unindexed standalone tail outside the record chain");
}

void test_clut8_dxt1_and_multiple_records() {
    const auto indexed = make_record({
        {0x99u, 5u, dimensions(3, 2), {1, 2, 3, 4, 5, 6}},
        {0x77u, 3u, 0xCAFE0002u, {10, 11, 12, 13, 14, 15, 16, 17, 0xEEu}}
    }, 1u, 0x11111111u, 0x22222222u);
    const auto compressed = make_record({
        {0u, 8u, dimensions(5, 5), std::vector<std::uint8_t>(32u, 0xD1u)}
    }, 0xFFFFFFFFu, 0x33333333u, 0x44444444u);
    const auto view = TmhView::parse(SharedBytes::take(make_tmh({indexed, compressed})));
    const auto first = view.descriptor(0);
    const auto second = view.descriptor(1, std::numeric_limits<std::size_t>::max());
    check(view.records().size() == 2 && view.consumed_bytes() == view.parent().bytes().size() &&
              first.image_format == 5u && first.width == 3u && first.height == 2u &&
              first.image.bytes().size() == 6u && first.palette_format == 3u &&
              first.palette_count == 2 && first.palette &&
              first.palette->bytes().size() == 9u && first.palette->bytes().back() == 0xEEu,
          "reads CLUT8 and RGBA8888 palette metadata across counted records");
    check(second.image_format == 8u && second.width == 5u && second.height == 5u &&
              second.image.bytes().size() == 32u && !second.palette_offset &&
              second.palette_format == 0u && second.palette_count == 0 && !second.palette,
          "uses DXT1 block geometry and returns a palette-free descriptor");
    check(throws_out_of_range([&] { (void)view.descriptor(2); }) &&
              throws_out_of_range([&] {
                  (void)view.descriptor(std::numeric_limits<std::size_t>::max());
              }),
          "rejects invalid record indices before accessing descriptors");
}

void test_palette_selector_and_lifetime() {
    const auto record = make_record({
        {8u, 5u, dimensions(2, 2), {1, 2, 3, 4}},
        {9u, 0xFFFFFFFFu, 0x87654321u, {}},
        {10u, 1u, 0xFEED0002u, {11, 12, 13, 14}},
        {11u, 3u, 0xABCD0001u, {21, 22, 23, 24}}
    }, 2u);
    const auto raw_tmh = make_tmh({record});
    std::vector<std::uint8_t> wrapper(12u, 0xA5u);
    put32(wrapper, 0, 1);
    put32(wrapper, 4, 12);
    put32(wrapper, 8, static_cast<std::uint32_t>(raw_tmh.size()));
    wrapper.insert(wrapper.end(), raw_tmh.begin(), raw_tmh.end());

    std::optional<TmhDescriptor> retained;
    std::optional<SharedBytes> nested;
    const std::uint8_t *source_image = nullptr;
    {
        const auto bundle = IndexedBundle::parse(SharedBytes::take(wrapper));
        auto child = bundle.child(0);
        const auto view = TmhView::parse(*child);
        const auto first = view.descriptor(0);
        const auto alternate = view.descriptor(0, 1);
        check(view.records()[0].blocks.size() == 4u &&
                  view.records()[0].blocks[1].format == 0xFFFFFFFFu &&
                  first.palette_format == 1u && first.palette_count == 2 &&
                  alternate.palette_format == 3u && alternate.palette_count == 1 &&
                  alternate.palette && alternate.palette->bytes()[0] == 21u,
              "selects palette blocks by preceding-subblock count plus selector");
        check(throws_invalid([&] { (void)view.descriptor(0, 2); }) &&
                  throws_invalid([&] {
                      (void)view.descriptor(0, std::numeric_limits<std::size_t>::max());
                  }),
              "rejects out-of-range and overflowing palette selectors");
        source_image = child->bytes().data() + first.image_offset;
        retained.emplace(first);
        nested.emplace(first.image.slice(1, 2));
    }
    check(retained && retained->image.bytes().data() == source_image &&
              retained->image.bytes()[0] == 1u && retained->palette &&
              retained->palette->bytes()[0] == 11u && nested &&
              nested->bytes().data() == source_image + 1u &&
              nested->bytes()[0] == 2u,
          "nested payload slices survive their bundle, TMH view, and parent values");
}

void test_header_record_and_block_bounds() {
    const auto valid_record = make_record({
        {1u, 8u, dimensions(1, 1), std::vector<std::uint8_t>(8u, 0x42u)}
    });
    const auto valid = make_tmh({valid_record});
    check(rejects({}) && rejects(std::vector<std::uint8_t>(15u, 0u)),
          "rejects incomplete TMH headers");
    auto changed = valid;
    changed[7] = '5';
    check(rejects(changed), "accepts only the supported .TMH0.14 version");

    changed = valid;
    put32(changed, 8, 4097u);
    check(rejects(changed, std::numeric_limits<std::size_t>::max()) &&
              rejects(valid, 0),
          "enforces both the hard record cap and a smaller caller budget");
    changed = valid;
    put32(changed, 8, 2u);
    check(rejects(changed), "requires every counted record header");

    changed = valid;
    put32(changed, 16, 15u);
    check(rejects(changed), "rejects a record stride overlapping its header");
    changed = valid;
    put32(changed, 16, 0xFFFFFFFFu);
    check(rejects(changed), "rejects a record stride beyond the parent without wrapping");
    changed = valid;
    put32(changed, 32, 15u);
    check(rejects(changed), "rejects a block stride overlapping its header");
    changed = valid;
    put32(changed, 32, 0xFFFFFFFFu);
    check(rejects(changed), "rejects a block stride beyond the record without wrapping");

    auto short_block_header = valid_record;
    short_block_header.push_back(0xA5u);
    put32(short_block_header, 0, static_cast<std::uint32_t>(short_block_header.size()));
    check(rejects(make_tmh({short_block_header})) &&
              rejects(make_tmh({make_record({})})),
          "rejects a partial final block header and a record with no image block");

    std::vector<BlockBytes> too_many;
    too_many.push_back({1u, 8u, dimensions(1, 1), std::vector<std::uint8_t>(8u, 0x42u)});
    for (std::size_t i = 0; i < 32u; ++i) {
        too_many.push_back({2u, 0u, 0u, {}});
    }
    check(rejects(make_tmh({make_record(too_many)})),
          "rejects more than 32 size-prefixed blocks in one record");

    const auto empty = TmhView::parse(SharedBytes::take(make_tmh({}, {0x55u})));
    check(empty.records().empty() && empty.consumed_bytes() == 16u &&
              empty.parent().bytes()[16] == 0x55u &&
              throws_out_of_range([&] { (void)empty.descriptor(0); }),
          "an empty record table still retains a nonzero outer tail");
}

void test_descriptor_validation() {
    const auto image = [](std::uint32_t format, std::uint16_t width,
                          std::uint16_t height, std::size_t body_size) {
        return BlockBytes{0xFFFFFFFFu, format, dimensions(width, height),
                          std::vector<std::uint8_t>(body_size, 0x33u)};
    };
    const auto palette = [](std::uint32_t format, std::uint32_t count,
                            std::size_t body_size) {
        return BlockBytes{0u, format, count,
                          std::vector<std::uint8_t>(body_size, 0x44u)};
    };
    const auto one = [&](const BlockBytes &first) {
        return make_tmh({make_record({first})});
    };
    const auto indexed = [&](const BlockBytes &first, const BlockBytes &second,
                             std::uint32_t preceding = 1u) {
        return make_tmh({make_record({first, second}, preceding)});
    };

    check(rejects(one(image(3u, 1u, 1u, 16u))) &&
              rejects(indexed(image(4u, 1u, 1u, 1u), palette(4u, 1u, 4u))),
          "rejects unsupported image and selected palette formats");
    check(rejects(one(image(4u, 1u, 1u, 1u))) &&
              rejects(indexed(image(5u, 1u, 1u, 1u), palette(1u, 1u, 2u), 2u)),
          "rejects missing indexed palettes and preceding-block indices outside a record");
    check(rejects(one(image(8u, 0u, 1u, 8u))) &&
              rejects(one(image(8u, 1u, 0u, 8u))) &&
              rejects(one(image(8u, 4097u, 1u, 8u))) &&
              rejects(one(image(5u, 4096u, 4096u, 1u))),
          "checks nonzero dimensions, per-axis budget, and huge encoded minima");
    check(rejects(indexed(image(4u, 3u, 3u, 4u), palette(1u, 1u, 2u))) &&
              !rejects(indexed(image(4u, 3u, 3u, 5u), palette(1u, 1u, 2u))) &&
              rejects(indexed(image(5u, 3u, 2u, 5u), palette(1u, 1u, 2u))) &&
              !rejects(indexed(image(5u, 3u, 2u, 6u), palette(1u, 1u, 2u))) &&
              rejects(one(image(8u, 5u, 5u, 31u))) &&
              !rejects(one(image(8u, 5u, 5u, 32u))),
          "checks CLUT4 byte minimum, CLUT8 area, and DXT1 edge blocks");
    check(rejects(indexed(image(4u, 2u, 2u, 2u), palette(1u, 0u, 2u))) &&
              rejects(indexed(image(4u, 2u, 2u, 2u), palette(1u, 0xFFFFu, 2u))) &&
              rejects(indexed(image(4u, 2u, 2u, 2u), palette(1u, 2u, 3u))) &&
              rejects(indexed(image(5u, 2u, 2u, 4u), palette(3u, 2u, 7u))),
          "sign-extends and validates positive palette counts and full payloads");
}

} // namespace

int main() {
    test_clut4_padding_tail_and_raw_fields();
    test_clut8_dxt1_and_multiple_records();
    test_palette_selector_and_lifetime();
    test_header_record_and_block_bounds();
    test_descriptor_validation();
    std::printf("TMH tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
