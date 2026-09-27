#include "resources/tmh.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mhp3rd::resources {
namespace {

constexpr std::size_t kMaxRecords = 4096;
constexpr std::size_t kMaxBlocksPerRecord = 32;
constexpr std::uint16_t kMaxDimension = 4096;
constexpr std::array<std::uint8_t, 8> kMarker = {
    '.', 'T', 'M', 'H', '0', '.', '1', '4'
};

// Every caller proves that the complete field lies in the parent first.
std::uint32_t read_le32(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24u);
}

std::int32_t sign_extend_16(std::uint16_t value) noexcept {
    return value < 0x8000u ? static_cast<std::int32_t>(value)
                           : static_cast<std::int32_t>(value) - 0x10000;
}

TmhDescriptor describe(const TmhRecord &record, std::size_t palette_selector) {
    if (record.blocks.empty()) throw std::invalid_argument("TMH: missing image block");
    const TmhBlock &image = record.blocks.front();
    const auto width = static_cast<std::uint16_t>(image.word12 & 0xFFFFu);
    const auto height = static_cast<std::uint16_t>(image.word12 >> 16u);
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        throw std::invalid_argument("TMH: image dimensions outside budget");
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    std::uint64_t minimum_image_size = 0;
    switch (image.format) {
    // CLUT4 uses a minimum byte count only. This does not choose a row
    // packing or swizzle layout for odd dimensions.
    case 4: minimum_image_size = (pixels + 1u) / 2u; break;
    case 5: minimum_image_size = pixels; break;              // CLUT8
    case 8: // DXT1 uses complete 4-by-4 blocks, including edge blocks.
        minimum_image_size = static_cast<std::uint64_t>((width + 3u) / 4u) *
                             ((height + 3u) / 4u) * 8u;
        break;
    default: throw std::invalid_argument("TMH: unsupported image format");
    }
    if (image.payload.bytes().size() < minimum_image_size) {
        throw std::invalid_argument("TMH: truncated encoded image");
    }

    TmhDescriptor result{image.offset + 16u, image.format, width, height,
                         std::nullopt, 0, 0, image.payload, std::nullopt};
    if (image.format == 8) return result;

    if (palette_selector >
        std::numeric_limits<std::size_t>::max() - record.preceding_subblocks) {
        throw std::invalid_argument("TMH: palette selector overflows");
    }
    const std::size_t palette_index =
        static_cast<std::size_t>(record.preceding_subblocks) + palette_selector;
    if (palette_index >= record.blocks.size()) {
        throw std::invalid_argument("TMH: palette block outside record");
    }
    const TmhBlock &palette = record.blocks[palette_index];
    std::uint32_t bytes_per_entry = 0;
    switch (palette.format) {
    case 1: bytes_per_entry = 2; break;
    case 3: bytes_per_entry = 4; break;
    default: throw std::invalid_argument("TMH: unsupported palette format");
    }
    const std::int32_t count = sign_extend_16(static_cast<std::uint16_t>(palette.word12));
    if (count <= 0 || palette.payload.bytes().size() <
                          static_cast<std::uint64_t>(count) * bytes_per_entry) {
        throw std::invalid_argument("TMH: invalid palette count or payload");
    }
    result.palette_offset = palette.offset + 16u;
    result.palette_format = palette.format;
    result.palette_count = count;
    result.palette = palette.payload;
    return result;
}

} // namespace

TmhView::TmhView(SharedBytes parent, std::vector<TmhRecord> records,
                 std::uint32_t reserved_word, std::size_t consumed_bytes)
    : parent_(std::move(parent)), records_(std::move(records)),
      reserved_word_(reserved_word), consumed_bytes_(consumed_bytes) {}

TmhView TmhView::parse(SharedBytes parent, std::size_t max_records) {
    const auto bytes = parent.bytes();
    if (bytes.size() < 16u) throw std::invalid_argument("TMH: truncated header");
    if (!std::equal(kMarker.begin(), kMarker.end(), bytes.begin())) {
        throw std::invalid_argument("TMH: unsupported marker or version");
    }
    // All public block and record offsets are TMH-local 32-bit values.
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("TMH: parent exceeds offset domain");
    }

    const std::uint32_t count = read_le32(bytes, 8);
    if (count > kMaxRecords || count > max_records) {
        throw std::invalid_argument("TMH: record count exceeds budget");
    }
    if (count > (bytes.size() - 16u) / 16u) {
        throw std::invalid_argument("TMH: counted record headers exceed parent");
    }
    const std::uint32_t reserved = read_le32(bytes, 12);
    std::vector<TmhRecord> records;
    records.reserve(count);
    std::size_t at = 16u;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at > bytes.size() || bytes.size() - at < 16u) {
            throw std::invalid_argument("TMH: truncated record header");
        }
        const std::uint32_t size = read_le32(bytes, at);
        if (size < 16u || size > bytes.size() - at) {
            throw std::invalid_argument("TMH: record stride outside parent");
        }
        const std::size_t end = at + size;
        TmhRecord record{static_cast<std::uint32_t>(at), size,
                         read_le32(bytes, at + 4u), read_le32(bytes, at + 8u),
                         read_le32(bytes, at + 12u), {}};
        std::size_t block_at = at + 16u;
        while (block_at < end) {
            if (record.blocks.size() >= kMaxBlocksPerRecord) {
                throw std::invalid_argument("TMH: block count exceeds budget");
            }
            if (end - block_at < 16u) {
                throw std::invalid_argument("TMH: truncated block header");
            }
            const std::uint32_t block_size = read_le32(bytes, block_at);
            if (block_size < 16u || block_size > end - block_at) {
                throw std::invalid_argument("TMH: block stride outside record");
            }
            record.blocks.push_back(TmhBlock{
                static_cast<std::uint32_t>(block_at), block_size,
                read_le32(bytes, block_at + 4u), read_le32(bytes, block_at + 8u),
                read_le32(bytes, block_at + 12u),
                parent.slice(block_at + 16u, block_size - 16u)
            });
            block_at += block_size;
        }
        (void)describe(record, 0); // Validate every default descriptor once.
        records.push_back(std::move(record));
        at = end;
    }
    return TmhView(std::move(parent), std::move(records), reserved, at);
}

TmhDescriptor TmhView::descriptor(std::size_t record_index,
                                  std::size_t palette_selector) const {
    if (record_index >= records_.size()) {
        throw std::out_of_range("TMH: record index outside count");
    }
    return describe(records_[record_index], palette_selector);
}

} // namespace mhp3rd::resources
