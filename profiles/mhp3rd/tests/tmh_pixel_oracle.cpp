#include "gpu/texture_decode.hpp"
#include "perf/frame_stats.hpp"
#include "psprecomp/sha256.hpp"
#include "resources/tmh_pixel_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Link the frozen software texture decoder without its frame-statistics system.
namespace mhp3rd::perf {
bool alternate_off(NewPath) { return false; }
}

namespace {
using namespace mhp3rd::resources;
using mhp3rd::gpu::TextureState;
using mhp3rd::gpu::TextureFormat;
using psprecomp::GuestMemory;
using psprecomp::sha256_bytes;

constexpr std::uint32_t kTexelAddress = 0x08010000u;
constexpr std::uint32_t kPaletteAddress = 0x08400000u;
constexpr std::size_t kParentLimit = 256u * 1024u * 1024u;
constexpr std::size_t kChildLimit = 16u * 1024u * 1024u;

void require(bool condition, const std::string &reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::string sha256_rgba(std::span<const std::uint32_t> pixels) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(pixels.size() * 4u);
    for (const std::uint32_t pixel : pixels) {
        for (unsigned shift = 0u; shift < 32u; shift += 8u)
            bytes.push_back(static_cast<std::uint8_t>(pixel >> shift));
    }
    return sha256_bytes(bytes);
}

std::string quoted(const std::string &value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value) {
        if (character == '"' || character == '\\') out << '\\' << character;
        else if (character >= 0x20u && character <= 0x7eu) out << character;
        else out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned>(character) << std::dec;
    }
    return out.str() + '"';
}

std::vector<std::uint8_t> read_parent(const std::filesystem::path &path,
                                      std::size_t size, const std::string &hash) {
    require(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),
            "Parent is not a regular file");
    require(size <= kParentLimit && std::filesystem::file_size(path) == size,
            "Parent size differs or exceeds limit");
    std::vector<std::uint8_t> data(size);
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
    require(bool(input) && input.peek() == std::char_traits<char>::eof(),
            "Parent read failed or grew");
    require(sha256_bytes(data) == hash, "Parent SHA-256 differs");
    return data;
}

struct IndexRow {
    std::uint64_t entry_id{}, parent_size{}, offset{}, size{};
    std::string parent_hash, child_hash, child_id;
};

IndexRow parse_row(const std::string &line) {
    IndexRow result;
    std::istringstream input(line);
    std::string extra;
    require(bool(input >> result.entry_id >> result.parent_size >> result.parent_hash >> result.offset >>
                 result.size >> result.child_hash >> result.child_id) && !(input >> extra),
            "Malformed TMH index row");
    require(result.entry_id < 100000u && result.parent_size <= kParentLimit &&
                result.offset <= result.parent_size && result.size <= result.parent_size - result.offset &&
                result.size <= kChildLimit, "TMH index span exceeds budget");
    require(result.child_id == "root" ||
                (!result.child_id.empty() &&
                 result.child_id.find_first_not_of("0123456789.") == std::string::npos &&
                 result.child_id.front() != '.' && result.child_id.back() != '.'),
            "Unsafe child identity");
    require(result.parent_hash.size() == 64u && result.child_hash.size() == 64u,
            "Malformed TMH index hash");
    return result;
}

struct Counts {
    std::size_t inputs{}, records{}, rectangle_compared{}, canvas_compared{};
    std::size_t missing_neighbor_context{}, canvas_after_image{}, canvas_after_tmh{};
    std::size_t compatibility_bypasses{}, strict_rejections{};
    std::size_t top_left_compared{}, top_left_differences{}, pixel_mismatches{};
};

void write_counts(std::ostream &out, const Counts &counts) {
    out << "{\"inputs\":" << counts.inputs << ",\"records\":" << counts.records
        << ",\"rectangle_compared\":" << counts.rectangle_compared
        << ",\"canvas_compared\":" << counts.canvas_compared
        << ",\"missing_neighbor_context\":" << counts.missing_neighbor_context
        << ",\"canvas_after_image\":" << counts.canvas_after_image
        << ",\"canvas_after_tmh\":" << counts.canvas_after_tmh
        << ",\"compatibility_bypasses\":" << counts.compatibility_bypasses
        << ",\"strict_rejections\":" << counts.strict_rejections
        << ",\"top_left_compared\":" << counts.top_left_compared
        << ",\"top_left_differences\":" << counts.top_left_differences
        << ",\"pixel_mismatches\":" << counts.pixel_mismatches << '}';
}

std::string layout_name(PixelLayout layout) {
    switch (layout) {
    case PixelLayout::Linear: return "linear";
    case PixelLayout::Swizzled16x8: return "swizzled_16x8";
    case PixelLayout::LegacySwizzleCompatibility: return "legacy_swizzle_compatibility";
    case PixelLayout::Unspecified: break;
    }
    return "unspecified";
}

void write_spec(std::ostream &out, const PixelDecodeSpec &spec) {
    out << "{\"format\":" << static_cast<unsigned>(spec.format)
        << ",\"width\":" << spec.width << ",\"height\":" << spec.height
        << ",\"stride_pixels\":" << spec.stride_pixels
        << ",\"layout\":" << quoted(layout_name(spec.layout))
        << ",\"palette_format\":" << static_cast<unsigned>(spec.palette_format)
        << ",\"palette_shift\":" << spec.palette_shift
        << ",\"palette_mask\":" << spec.palette_mask
        << ",\"palette_offset\":" << spec.palette_offset << '}';
}

struct Comparison {
    PixelDecodeResult portable;
    std::string portable_hash, legacy_hash;
    bool equal{};
    std::size_t first_difference{};
    std::uint32_t portable_value{}, legacy_value{};
    bool strict_rejected{};
};

Comparison compare(GuestMemory &memory, const PixelDecodeSpec &spec,
                   std::span<const std::uint8_t> texels,
                   std::span<const std::uint8_t> palette) {
    Comparison result;
    result.portable = decode_pixels(spec, texels, palette);
    require(result.portable.ok(), "Portable decoder rejected a complete TMH window: " +
                                  std::to_string(static_cast<unsigned>(result.portable.error)));
    require(result.portable.required_texel_bytes == texels.size(), "TMH window/spec byte count differs");
    if (spec.layout == PixelLayout::LegacySwizzleCompatibility) {
        PixelDecodeSpec strict = spec;
        strict.layout = PixelLayout::Swizzled16x8;
        const auto checked = decode_pixels(strict, texels, palette);
        if (result.portable.legacy_unaligned_bypass) {
            require(checked.error == PixelDecodeError::InvalidSwizzleAlignment && checked.rgba.empty(),
                    "Strict swizzle accepted an unaligned TMH window");
            result.strict_rejected = true;
        } else {
            require(checked.ok() && checked.rgba == result.portable.rgba,
                    "Strict and compatibility swizzle disagree on aligned TMH window");
        }
    }

    require(memory.contains(kTexelAddress, texels.size()) &&
                memory.contains(kPaletteAddress, palette.size()), "Oracle guest fixture exceeds RAM");
    memory.copy_in(kTexelAddress, texels);
    if (!palette.empty()) memory.copy_in(kPaletteAddress, palette);
    TextureState state{};
    state.enabled = true;
    state.address = kTexelAddress;
    state.width = static_cast<std::uint16_t>(spec.width);
    state.height = static_cast<std::uint16_t>(spec.height);
    state.buffer_width = spec.stride_pixels;
    state.format = static_cast<TextureFormat>(spec.format);
    state.swizzled = spec.layout != PixelLayout::Linear;
    state.clut_address = kPaletteAddress;
    state.clut_format = static_cast<unsigned>(spec.palette_format);
    state.clut_shift = spec.palette_shift;
    state.clut_mask = spec.palette_mask;
    state.clut_offset = spec.palette_offset;
    std::vector<std::uint32_t> legacy;
    require(mhp3rd::gpu::decode_texture(memory, state, legacy),
            "Legacy decoder rejected a complete TMH window");
    result.portable_hash = sha256_rgba(result.portable.rgba);
    result.legacy_hash = sha256_rgba(legacy);
    result.equal = result.portable.rgba == legacy;
    if (!result.equal) {
        require(result.portable.rgba.size() == legacy.size(), "Legacy pixel count differs");
        result.first_difference = static_cast<std::size_t>(std::mismatch(
            result.portable.rgba.begin(), result.portable.rgba.end(), legacy.begin()).first -
            result.portable.rgba.begin());
        result.portable_value = result.portable.rgba[result.first_difference];
        result.legacy_value = legacy[result.first_difference];
    }
    return result;
}

void write_comparison(std::ostream &out, const TmhPixelPlan &plan,
                      std::size_t root_offset, std::span<const std::uint8_t> texels,
                      const Comparison &comparison) {
    out << "{\"status\":\"compared\",\"spec\":";
    write_spec(out, plan.decode);
    out << ",\"window_root_offset\":" << root_offset
        << ",\"window_bytes\":" << texels.size()
        << ",\"window_sha256\":" << quoted(sha256_bytes(texels))
        << ",\"required_palette_bytes\":" << comparison.portable.required_palette_bytes
        << ",\"legacy_unaligned_bypass\":"
        << (comparison.portable.legacy_unaligned_bypass ? "true" : "false")
        << ",\"strict_swizzle_rejected\":" << (comparison.strict_rejected ? "true" : "false")
        << ",\"portable_rgba_sha256_le\":" << quoted(comparison.portable_hash)
        << ",\"legacy_rgba_sha256_le\":" << quoted(comparison.legacy_hash)
        << ",\"pixel_equal\":" << (comparison.equal ? "true" : "false");
    if (!comparison.equal) {
        out << ",\"first_difference\":{\"index\":" << comparison.first_difference
            << ",\"x\":" << comparison.first_difference % plan.decode.width
            << ",\"y\":" << comparison.first_difference / plan.decode.width
            << ",\"portable_rgba\":" << comparison.portable_value
            << ",\"legacy_rgba\":" << comparison.legacy_value << '}';
    }
    out << '}';
}

bool same_top_left_row_mapping(const PixelDecodeSpec &rectangle,
                               const PixelDecodeSpec &canvas,
                               const Comparison &raw, const Comparison &padded) {
    if (rectangle.format != canvas.format || rectangle.stride_pixels != canvas.stride_pixels ||
        rectangle.width > canvas.width || rectangle.height > canvas.height) return false;
    if (rectangle.format == PixelFormat::Dxt1) {
        return (rectangle.width + 3u) / 4u == (canvas.width + 3u) / 4u;
    }
    const std::size_t bits = rectangle.format == PixelFormat::Clut4 ? 4u : 8u;
    const std::size_t row_bytes = static_cast<std::size_t>(rectangle.stride_pixels) * bits / 8u;
    const std::size_t last_sample_end = (static_cast<std::size_t>(rectangle.width) * bits + 7u) / 8u;
    return row_bytes >= last_sample_end &&
           raw.portable.legacy_unaligned_bypass == padded.portable.legacy_unaligned_bypass;
}

std::size_t top_left_differences(const PixelDecodeSpec &rectangle,
                                 const PixelDecodeSpec &canvas,
                                 std::span<const std::uint32_t> raw,
                                 std::span<const std::uint32_t> padded) {
    std::size_t differences = 0u;
    for (std::size_t y = 0u; y < rectangle.height; ++y)
        for (std::size_t x = 0u; x < rectangle.width; ++x)
            differences += raw[y * rectangle.width + x] != padded[y * canvas.width + x];
    return differences;
}

void run_input(std::ostream &out, GuestMemory &memory, const IndexRow &row,
               const std::filesystem::path &raw_entries, Counts &counts) {
    std::ostringstream filename;
    filename << std::setw(5) << std::setfill('0') << row.entry_id << ".bin";
    auto root = SharedBytes::take(read_parent(raw_entries / filename.str(),
                                               static_cast<std::size_t>(row.parent_size), row.parent_hash));
    const auto parent = root.bytes();
    const auto child = root.slice(static_cast<std::size_t>(row.offset), static_cast<std::size_t>(row.size));
    require(sha256_bytes(child.bytes()) == row.child_hash, "TMH child SHA-256 differs");
    const TmhView view = TmhView::parse(child);
    out << "{\"entry_id\":" << row.entry_id << ",\"parent_size\":" << row.parent_size
        << ",\"parent_sha256\":" << quoted(row.parent_hash)
        << ",\"child_id\":" << quoted(row.child_id)
        << ",\"child_sha256\":" << quoted(row.child_hash)
        << ",\"offset_in_decoded_entry\":" << row.offset
        << ",\"child_size\":" << row.size
        << ",\"records\":[";
    for (std::size_t index = 0u; index < view.records().size(); ++index) {
        if (index) out << ',';
        const TmhDescriptor descriptor = view.descriptor(index);
        const auto rectangle = plan_model_builder_pixels(descriptor, TmhPixelDomain::EncodedRectangle,
                                                          PixelLayout::LegacySwizzleCompatibility);
        const auto canvas = plan_model_builder_pixels(descriptor, TmhPixelDomain::ModelBuilderCanvas,
                                                       PixelLayout::LegacySwizzleCompatibility);
        require(rectangle.image_window_bytes <= descriptor.image.bytes().size(),
                "Encoded rectangle escapes its image payload");
        const std::size_t image_root_offset = static_cast<std::size_t>(row.offset) + descriptor.image_offset;
        require(image_root_offset <= parent.size() &&
                    descriptor.image.bytes().data() == parent.data() + image_root_offset,
                "Image descriptor is not rooted at its immutable decoded parent");
        std::span<const std::uint8_t> palette;
        if (descriptor.palette) {
            const std::size_t palette_root_offset = static_cast<std::size_t>(row.offset) + *descriptor.palette_offset;
            require(palette_root_offset <= parent.size() &&
                        descriptor.palette->bytes().data() == parent.data() + palette_root_offset,
                    "Palette descriptor is not rooted at its immutable decoded parent");
            palette = descriptor.palette->bytes();
        }
        const auto raw_texels = descriptor.image.bytes().first(rectangle.image_window_bytes);
        const auto raw = compare(memory, rectangle.decode, raw_texels, palette);
        ++counts.rectangle_compared;
        counts.pixel_mismatches += !raw.equal;
        counts.compatibility_bypasses += raw.portable.legacy_unaligned_bypass;
        counts.strict_rejections += raw.strict_rejected;
        const bool after_image = canvas.reads_after_image_payload;
        const bool after_tmh = canvas.image_window_bytes >
            static_cast<std::size_t>(row.size) - descriptor.image_offset;
        counts.canvas_after_image += after_image;
        counts.canvas_after_tmh += after_tmh;

        out << "{\"record_index\":" << index
            << ",\"record_child_offset\":" << view.records()[index].offset
            << ",\"image_child_offset\":" << descriptor.image_offset
            << ",\"image_root_offset\":" << image_root_offset
            << ",\"image_payload_bytes\":" << descriptor.image.bytes().size()
            << ",\"palette_child_offset\":";
        if (descriptor.palette_offset) out << *descriptor.palette_offset; else out << "null";
        out << ",\"palette_payload_bytes\":" << palette.size()
            << ",\"palette_sha256\":";
        if (descriptor.palette) out << quoted(sha256_bytes(palette)); else out << "null";
        out << ",\"canvas_reads_after_image_payload\":" << (after_image ? "true" : "false")
            << ",\"canvas_reads_after_tmh_child\":" << (after_tmh ? "true" : "false")
            << ",\"rectangle\":";
        write_comparison(out, rectangle, image_root_offset, raw_texels, raw);
        out << ",\"canvas\":";

        if (canvas.image_window_bytes > parent.size() - image_root_offset) {
            const auto available = parent.subspan(image_root_offset);
            const auto missing = decode_pixels(canvas.decode, available, palette);
            require(missing.error == PixelDecodeError::MissingTexelBytes && missing.rgba.empty() &&
                        missing.required_texel_bytes == canvas.image_window_bytes,
                    "Portable decoder did not reject missing root-neighbor bytes");
            ++counts.missing_neighbor_context;
            out << "{\"status\":\"missing_neighbor_context\",\"spec\":";
            write_spec(out, canvas.decode);
            out << ",\"window_root_offset\":" << image_root_offset
                << ",\"required_window_bytes\":" << canvas.image_window_bytes
                << ",\"available_root_bytes\":" << available.size()
                << ",\"missing_range\":{\"root_start\":" << parent.size()
                << ",\"root_end_exclusive\":" << image_root_offset + canvas.image_window_bytes
                << "},\"portable_error\":\"MissingTexelBytes\"}";
            out << ",\"top_left\":{\"status\":\"unavailable_canvas\"}";
        } else {
            const auto canvas_texels = parent.subspan(image_root_offset, canvas.image_window_bytes);
            const auto padded = compare(memory, canvas.decode, canvas_texels, palette);
            ++counts.canvas_compared;
            counts.pixel_mismatches += !padded.equal;
            write_comparison(out, canvas, image_root_offset, canvas_texels, padded);
            out << ",\"top_left\":";
            if (same_top_left_row_mapping(rectangle.decode, canvas.decode, raw, padded)) {
                const std::size_t differences = top_left_differences(rectangle.decode, canvas.decode,
                                                                     raw.portable.rgba, padded.portable.rgba);
                ++counts.top_left_compared;
                counts.top_left_differences += differences;
                out << "{\"status\":\"comparable\",\"differing_pixels\":" << differences << '}';
            } else {
                out << "{\"status\":\"different_row_mapping_or_sample_window\"}";
            }
        }
        out << '}';
        ++counts.records;
    }
    out << "]}";
    require(sha256_bytes(root.bytes()) == row.parent_hash, "In-memory parent changed during pixel comparison");
    ++counts.inputs;
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 5, "usage: tmh_pixel_oracle raw-entries index.tsv fast|slow new-shard.json");
        const std::string mode = argv[3];
        require(mode == "fast" || mode == "slow", "Unknown legacy decoder mode");
        require((std::getenv("MHP3RD_NO_FAST_TEXTURE_DECODE") != nullptr) == (mode == "slow"),
                "Legacy decoder environment does not match requested mode");
        require(std::filesystem::is_directory(argv[1]), "Missing raw-entry directory");
        require(!std::filesystem::exists(argv[4]) && !std::filesystem::is_symlink(argv[4]),
                "Report already exists");
        std::ifstream index(argv[2]);
        require(bool(index), "Missing TMH index");
        GuestMemory memory(32u * 1024u * 1024u);
        Counts counts;
        std::ostringstream report;
        report << "{\"schema_version\":1,\"scope\":\"offline_software_TMH_pixels\",\"legacy_mode\":"
               << quoted(mode) << ",\"inputs\":[";
        std::string line;
        std::set<std::string> seen;
        while (std::getline(index, line)) {
            const IndexRow row = parse_row(line);
            require(counts.inputs < 50000u, "TMH input count exceeds budget");
            const auto identity = std::to_string(row.entry_id) + ":" + row.child_id;
            require(seen.insert(identity).second, "Duplicate TMH input identity");
            if (counts.inputs) report << ',';
            run_input(report, memory, row, argv[1], counts);
        }
        require(index.eof() && counts.inputs > 0u, "TMH index incomplete or empty");
        const bool success = counts.pixel_mismatches == 0u && counts.top_left_differences == 0u;
        report << "],\"counts\":";
        write_counts(report, counts);
        report << ",\"success\":" << (success ? "true" : "false") << "}\n";
        std::ofstream output(argv[4], std::ios::binary);
        require(bool(output), "Cannot create TMH pixel report");
        output << report.str();
        output.close();
        require(bool(output), "TMH pixel report write failed");
        std::cout << "TMH pixel " << mode << ": " << counts.inputs << " inputs, " << counts.records
                  << " rectangles, " << counts.canvas_compared << " canvases, "
                  << counts.missing_neighbor_context << " missing neighbor windows, "
                  << counts.pixel_mismatches << " decoder mismatches\n";
        return success ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
