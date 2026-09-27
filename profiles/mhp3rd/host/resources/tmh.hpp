#pragma once

#include "resources/indexed_bundle.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mhp3rd::resources {

// Offsets are relative to the beginning of this TMH child. Payloads retain
// the entire encoded block body, including any bytes beyond the nominal size.
struct TmhBlock {
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t tag;
    std::uint32_t format;
    std::uint32_t word12;
    SharedBytes payload;
};

struct TmhRecord {
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t word4;
    std::uint32_t preceding_subblocks;
    std::uint32_t word12;
    std::vector<TmhBlock> blocks;
};

// These are views of encoded source bytes, not decoded pixels. The image
// layout (including any swizzle) is not established by this descriptor.
struct TmhDescriptor {
    std::uint32_t image_offset;
    std::uint32_t image_format;
    std::uint16_t width;
    std::uint16_t height;
    std::optional<std::uint32_t> palette_offset;
    std::uint32_t palette_format;
    std::int32_t palette_count;
    SharedBytes image;
    std::optional<SharedBytes> palette;
};

class TmhView {
public:
    // Accepts .TMH0.14. The hard limits are 4096 records, 32 blocks per
    // record, and 4096 pixels per image axis. max_records can lower the
    // record budget but cannot raise the hard limit. Invalid data throws
    // std::invalid_argument; allocation failures retain their usual meaning.
    static TmhView parse(SharedBytes parent, std::size_t max_records = 4096);

    std::span<const TmhRecord> records() const noexcept { return records_; }
    const SharedBytes &parent() const noexcept { return parent_; }
    std::uint32_t reserved_word() const noexcept { return reserved_word_; }
    // Bytes through the end of the counted records. The parent may have an
    // unindexed outer tail, retained unchanged in parent().
    std::size_t consumed_bytes() const noexcept { return consumed_bytes_; }

    // The palette candidate is block[preceding_subblocks + palette_selector]
    // for indexed images. An invalid record index throws std::out_of_range;
    // an invalid selection or unsupported block throws std::invalid_argument.
    TmhDescriptor descriptor(std::size_t record_index,
                             std::size_t palette_selector = 0) const;

private:
    TmhView(SharedBytes parent, std::vector<TmhRecord> records,
            std::uint32_t reserved_word, std::size_t consumed_bytes);

    SharedBytes parent_;
    std::vector<TmhRecord> records_;
    std::uint32_t reserved_word_;
    std::size_t consumed_bytes_;
};

} // namespace mhp3rd::resources
