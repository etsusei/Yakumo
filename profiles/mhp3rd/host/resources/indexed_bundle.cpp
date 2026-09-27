#include "resources/indexed_bundle.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace mhp3rd::resources {
namespace {

std::uint32_t read_le32(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24u);
}

} // namespace

SharedBytes::SharedBytes(std::shared_ptr<const std::vector<std::uint8_t>> storage,
                         std::size_t offset, std::size_t length) noexcept
    : storage_(std::move(storage)), offset_(offset), length_(length) {}

SharedBytes SharedBytes::take(std::vector<std::uint8_t> bytes) {
    auto storage = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    const auto length = storage->size();
    return SharedBytes(std::move(storage), 0, length);
}

std::span<const std::uint8_t> SharedBytes::bytes() const noexcept {
    if (!storage_ || storage_->empty()) return {};
    return {storage_->data() + offset_, length_};
}

SharedBytes SharedBytes::slice(std::size_t offset, std::size_t length) const {
    if (!storage_ || offset > length_ || length > length_ - offset) {
        throw std::out_of_range("shared bytes: slice outside parent");
    }
    return SharedBytes(storage_, offset_ + offset, length);
}

IndexedBundle::IndexedBundle(SharedBytes parent, std::vector<BundleEntry> entries)
    : parent_(std::move(parent)), entries_(std::move(entries)) {}

IndexedBundle IndexedBundle::parse(SharedBytes parent, std::size_t max_entries) {
    const auto bytes = parent.bytes();
    if (bytes.size() < 4u) throw std::invalid_argument("indexed bundle: truncated count");

    const std::uint32_t count = read_le32(bytes, 0);
    if (count > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::invalid_argument("indexed bundle: count exceeds signed-index domain");
    }
    if (count > max_entries) throw std::invalid_argument("indexed bundle: count exceeds entry budget");
    if (static_cast<std::uint64_t>(count) >
        static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max() - 4u) / 8u)) {
        throw std::invalid_argument("indexed bundle: table size overflows");
    }

    const std::size_t table_end = 4u + static_cast<std::size_t>(count) * 8u;
    if (table_end > bytes.size()) throw std::invalid_argument("indexed bundle: truncated table");

    std::vector<BundleEntry> entries;
    entries.reserve(count);
    for (std::size_t at = 4u; at < table_end; at += 8u) {
        const BundleEntry entry{read_le32(bytes, at), read_le32(bytes, at + 4u)};
        if (entry.present()) {
            const std::size_t start = entry.offset;
            if (start < table_end) throw std::invalid_argument("indexed bundle: child overlaps table");
            if (start > bytes.size() || entry.length > bytes.size() - start) {
                throw std::invalid_argument("indexed bundle: child outside parent");
            }
        }
        entries.push_back(entry);
    }
    return IndexedBundle(std::move(parent), std::move(entries));
}

std::optional<SharedBytes> IndexedBundle::child(std::size_t index) const {
    if (index >= entries_.size()) throw std::out_of_range("indexed bundle: child index outside table");
    const BundleEntry &entry = entries_[index];
    if (!entry.present()) return std::nullopt;
    return parent_.slice(entry.offset, entry.length);
}

} // namespace mhp3rd::resources
