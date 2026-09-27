#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace mhp3rd::resources {

// A slice shares immutable storage with its parent. Keep the SharedBytes value
// alive while using the non-owning span returned by bytes().
class SharedBytes {
public:
    static SharedBytes take(std::vector<std::uint8_t> bytes);

    std::span<const std::uint8_t> bytes() const noexcept;
    SharedBytes slice(std::size_t offset, std::size_t length) const;

private:
    SharedBytes(std::shared_ptr<const std::vector<std::uint8_t>> storage,
                std::size_t offset, std::size_t length) noexcept;

    std::shared_ptr<const std::vector<std::uint8_t>> storage_;
    std::size_t offset_;
    std::size_t length_;
};

struct BundleEntry {
    // Offset is relative to the start of the decoded parent, not its source file.
    std::uint32_t offset;
    std::uint32_t length;

    bool present() const noexcept { return offset != 0; }
};

class IndexedBundle {
public:
    // The default budget is intentionally small; callers may set a finite bound.
    static IndexedBundle parse(SharedBytes parent, std::size_t max_entries = 4096);

    std::span<const BundleEntry> entries() const noexcept { return entries_; }
    const SharedBytes &parent() const noexcept { return parent_; }

    // An absent in-range slot returns nullopt. An invalid index throws.
    // A present zero-length child returns an engaged, empty SharedBytes slice.
    std::optional<SharedBytes> child(std::size_t index) const;

private:
    IndexedBundle(SharedBytes parent, std::vector<BundleEntry> entries);

    SharedBytes parent_;
    std::vector<BundleEntry> entries_;
};

} // namespace mhp3rd::resources
