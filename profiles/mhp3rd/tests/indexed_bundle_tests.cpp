#include "resources/indexed_bundle.hpp"

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

static_assert(std::is_same_v<decltype(std::declval<const SharedBytes &>().bytes()),
                             std::span<const std::uint8_t>>);

int failures = 0;

void check(bool condition, const char *description) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition) ++failures;
}

void le32(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}

bool equal_bytes(std::span<const std::uint8_t> actual, std::initializer_list<std::uint8_t> expected) {
    return actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin());
}

bool rejects(const std::vector<std::uint8_t> &bytes, std::size_t max_entries = 4096) {
    try {
        (void)IndexedBundle::parse(SharedBytes::take(bytes), max_entries);
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

std::vector<std::uint8_t> one_entry(std::uint32_t offset, std::uint32_t length,
                                    std::size_t parent_size = 16) {
    std::vector<std::uint8_t> bytes(parent_size, 0xA5u);
    le32(bytes, 0, 1);
    le32(bytes, 4, offset);
    le32(bytes, 8, length);
    return bytes;
}

// All bytes here are synthetic. The table ends at 52; payloads are intentionally
// unsorted, overlapping, and aliased, with an unindexed gap and nonzero tail.
std::vector<std::uint8_t> mixed_fixture() {
    std::vector<std::uint8_t> bytes(72, 0xE7u);
    le32(bytes, 0, 6);
    le32(bytes, 4, 62);  le32(bytes, 8, 4);
    le32(bytes, 12, 0);  le32(bytes, 16, 0xDEADBEEFu);
    le32(bytes, 20, 56); le32(bytes, 24, 8);
    le32(bytes, 28, 58); le32(bytes, 32, 4);
    le32(bytes, 36, 56); le32(bytes, 40, 8);
    le32(bytes, 44, 72); le32(bytes, 48, 0);
    bytes[52] = 0xA1u; bytes[53] = 0xA2u; bytes[54] = 0xA3u; bytes[55] = 0xA4u;
    for (std::size_t i = 0; i < 8; ++i) bytes[56 + i] = static_cast<std::uint8_t>(0x20u + i);
    bytes[64] = 0xD1u; bytes[65] = 0xD2u;
    return bytes;
}

void test_empty_and_little_endian() {
    const auto empty = IndexedBundle::parse(SharedBytes::take({0, 0, 0, 0, 0x91u, 0x92u}), 0);
    check(empty.entries().empty() && equal_bytes(empty.parent().bytes(), {0, 0, 0, 0, 0x91u, 0x92u}),
          "accepts an empty table and retains trailing parent bytes");
    check(throws_out_of_range([&] { (void)empty.child(0); }), "rejects an index into an empty table");

    const auto one = IndexedBundle::parse(SharedBytes::take({1, 0, 0, 0, 12, 0, 0, 0, 1, 0, 0, 0, 0x99u}));
    const auto child = one.child(0);
    check(child && equal_bytes(child->bytes(), {0x99u}), "reads the count and record words as little-endian");

    auto owned_empty = SharedBytes::take({});
    check(owned_empty.bytes().empty() && owned_empty.slice(0, 0).bytes().empty(),
          "an empty owned buffer supports an empty slice");
}

void test_slots_aliases_and_retention() {
    const auto original = mixed_fixture();
    const auto bundle = IndexedBundle::parse(SharedBytes::take(original));
    const auto entries = bundle.entries();
    check(entries.size() == 6 && entries[1].offset == 0 && entries[1].length == 0xDEADBEEFu &&
              !entries[1].present() && entries[2].present(),
          "retains absent-slot length separately from presence");
    check(bundle.parent().bytes().size() == original.size() &&
              std::equal(original.begin(), original.end(), bundle.parent().bytes().begin()),
          "retains the entire parent, including its gap and nonzero tail");

    const auto later = bundle.child(0);
    const auto earlier = bundle.child(2);
    const auto overlap = bundle.child(3);
    const auto alias = bundle.child(4);
    const auto absent = bundle.child(1);
    const auto present_empty = bundle.child(5);
    check(later && equal_bytes(later->bytes(), {0x26u, 0x27u, 0xD1u, 0xD2u}) &&
              earlier && equal_bytes(earlier->bytes(), {0x20u, 0x21u, 0x22u, 0x23u,
                                                        0x24u, 0x25u, 0x26u, 0x27u}),
          "resolves unsorted spans from the parent base");
    check(overlap && alias && earlier && equal_bytes(overlap->bytes(), {0x22u, 0x23u, 0x24u, 0x25u}) &&
              overlap->bytes().data() == earlier->bytes().data() + 2 &&
              alias->bytes().data() == earlier->bytes().data(),
          "accepts overlap and aliases without copying child bytes");
    check(!absent && present_empty && present_empty->bytes().empty(),
          "distinguishes an absent slot from a present zero-length child at parent end");
    check(throws_out_of_range([&] { (void)bundle.child(entries.size()); }) &&
              throws_out_of_range([&] { (void)bundle.child(std::numeric_limits<std::size_t>::max()); }),
          "rejects invalid indices before reading any record");
}

void test_shared_lifetime_and_nested_views() {
    std::optional<SharedBytes> retained_child;
    const std::uint8_t *child_address = nullptr;
    {
        auto original_parent = SharedBytes::take(mixed_fixture());
        auto moved_parent = std::move(original_parent);
        const auto bundle = IndexedBundle::parse(std::move(moved_parent));
        retained_child = bundle.child(2);
        child_address = retained_child->bytes().data();
    }
    check(retained_child && equal_bytes(retained_child->bytes(), {0x20u, 0x21u, 0x22u, 0x23u,
                                                                 0x24u, 0x25u, 0x26u, 0x27u}),
          "a child remains readable after its parent and parser are destroyed");

    auto nested = retained_child->slice(2, 3);
    retained_child.reset();
    check(nested.bytes().data() == child_address + 2 &&
              equal_bytes(nested.bytes(), {0x22u, 0x23u, 0x24u}),
          "a nested slice keeps the same storage alive without copying");
    check(throws_out_of_range([&] { (void)nested.slice(4, 0); }) &&
              throws_out_of_range([&] { (void)nested.slice(2, 2); }) &&
              throws_out_of_range([&] { (void)nested.slice(std::numeric_limits<std::size_t>::max(), 1); }),
          "slice bounds checks avoid offset-plus-length overflow");
}

void test_malformed_count_and_table() {
    check(rejects({}) && rejects({0}) && rejects({0, 0, 0}),
          "rejects a truncated count word");
    check(rejects({1, 0, 0, 0}) && rejects({1, 0, 0, 0, 12, 0, 0, 0, 1, 0, 0}),
          "rejects incomplete record tables before allocating entries");

    std::vector<std::uint8_t> two_records(20);
    le32(two_records, 0, 2);
    check(!rejects(two_records, 2) && rejects(two_records, 1) && rejects(two_records, 0),
          "enforces a caller-supplied count budget");

    std::vector<std::uint8_t> huge_count(4);
    le32(huge_count, 0, 0x80000000u);
    check(rejects(huge_count, std::numeric_limits<std::size_t>::max()),
          "rejects counts outside the original signed-index domain");
    le32(huge_count, 0, 0x7FFFFFFFu);
    check(rejects(huge_count, std::numeric_limits<std::size_t>::max()),
          "rejects a huge count with no complete table before allocation");
    le32(huge_count, 0, 4097);
    check(rejects(huge_count), "enforces the default entry budget");
}

void test_malformed_and_exceptional_spans() {
    check(rejects(one_entry(11, 0)) && rejects(one_entry(1, 0)),
          "rejects nonzero child offsets into the table");
    check(rejects(one_entry(17, 0)) && rejects(one_entry(15, 2)),
          "rejects offsets or lengths past the parent end");
    check(rejects(one_entry(12, 0xFFFFFFFFu)),
          "rejects an oversized length without wrapped end arithmetic");

    const auto absent = IndexedBundle::parse(SharedBytes::take(one_entry(0, 0xFFFFFFFFu)));
    check(!absent.child(0) && absent.entries()[0].length == 0xFFFFFFFFu,
          "allows an absent slot to retain any advertised length");
    const auto end_child = IndexedBundle::parse(SharedBytes::take(one_entry(16, 0)));
    check(end_child.child(0) && end_child.child(0)->bytes().empty(),
          "accepts a present zero-length span at the parent end");
}

} // namespace

int main() {
    test_empty_and_little_endian();
    test_slots_aliases_and_retention();
    test_shared_lifetime_and_nested_views();
    test_malformed_count_and_table();
    test_malformed_and_exceptional_spans();
    std::printf("Indexed bundle tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
