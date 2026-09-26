// Independent byte comparison against the runtime's DATA.BIN implementation.
// Use only after the preparation tool has validated the ISO and archive span.
// This program neither runs game code nor writes source/extracted resources.
#include "mods/mhp3rd_data_bin.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace mhp3rd::mods::p3rd;
constexpr std::size_t kChunk = 1024u * 1024u;

std::uint64_t decimal(std::string_view value) {
    std::uint64_t number{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::runtime_error("Archive offset and size must be unsigned decimal integers");
    return number;
}

std::uint32_t little_word(const std::array<std::uint8_t, 4> &bytes) {
    return bytes[0] | (std::uint32_t{bytes[1]} << 8u) | (std::uint32_t{bytes[2]} << 16u) |
           (std::uint32_t{bytes[3]} << 24u);
}

int compare(int argc, char **argv) {
    if (argc != 5) {
        std::cerr << "Usage: mhp3rd_resource_crosscheck ISO ARCHIVE_OFFSET ARCHIVE_SIZE RAW_ENTRIES_DIR\n"
                     "The source and archive span must first pass prepare_resources.py validation.\n";
        return 2;
    }
    const std::filesystem::path source = argv[1], entries_root = argv[4];
    const auto base = decimal(argv[2]), size = decimal(argv[3]);
    const auto image_size = std::filesystem::file_size(source);
    if (base > image_size || size > image_size - base || size % kBlock != 0u)
        throw std::runtime_error("Archive span is outside the image or not block aligned");
    if (std::filesystem::is_symlink(entries_root) || !std::filesystem::is_directory(entries_root))
        throw std::runtime_error("Extracted entry root must be a real directory");
    std::ifstream image(source, std::ios::binary);
    if (!image) throw std::runtime_error("Cannot open source image");
    const auto read = [&](std::uint64_t at, std::span<std::uint8_t> output) {
        if (at > size || output.size() > size - at) throw std::runtime_error("Read outside archive");
        image.seekg(static_cast<std::streamoff>(base + at));
        image.read(reinterpret_cast<char *>(output.data()), static_cast<std::streamsize>(output.size()));
        if (static_cast<std::size_t>(image.gcount()) != output.size())
            throw std::runtime_error("Short source read");
    };
    std::array<std::uint8_t, 4> head{};
    read(0u, head);
    decrypt(head, 0u, 0u);
    const auto directory_blocks = little_word(head);
    if (directory_blocks == 0u || directory_blocks > 512u)
        throw std::runtime_error("Invalid directory size");
    std::vector<std::uint8_t> directory_bytes(directory_blocks * kBlock);
    read(0u, directory_bytes);
    const auto parsed = Directory::parse(directory_bytes, size);
    if (!parsed) throw std::runtime_error("Runtime rejected the DATA.BIN directory");
    const auto &directory = *parsed;
    std::uint64_t total{}, empty{}, verbatim{}, overlays{};
    for (std::uint32_t id = 0; id < directory.entries(); ++id) {
        const auto length = directory.size(id);
        const auto offset = std::uint64_t{directory.blocks[id]} * kBlock;
        if (offset > size || length > size - offset) throw std::runtime_error("Invalid entry span");
        std::vector<std::uint8_t> first(static_cast<std::size_t>(std::min<std::uint64_t>(length, 64u)));
        if (!first.empty()) read(offset, first);
        const bool direct = verbatim_magic(first);
        if (direct) ++verbatim;
        else decrypt(first, directory.blocks[id], 0u);
        if (first.size() >= 64u && std::equal(first.begin(), first.begin() + 4, "MWo3")) ++overlays;
        if (length == 0u) ++empty;
        total += length;
        std::ostringstream filename;
        filename << std::setw(5) << std::setfill('0') << id << ".bin";
        const auto output_path = entries_root / filename.str();
        if (std::filesystem::is_symlink(output_path) || !std::filesystem::is_regular_file(output_path) ||
            std::filesystem::file_size(output_path) != length)
            throw std::runtime_error("Output type or size differs for entry " + std::to_string(id));
        std::ifstream extracted(output_path, std::ios::binary);
        if (!extracted) throw std::runtime_error("Cannot read extracted entry " + std::to_string(id));
        for (std::uint64_t at = 0; at < length;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, length - at));
            std::vector<std::uint8_t> reference(count), actual(count);
            read(offset + at, reference);
            if (!direct) decrypt(reference, directory.blocks[id], at);
            extracted.read(reinterpret_cast<char *>(actual.data()), static_cast<std::streamsize>(count));
            if (static_cast<std::size_t>(extracted.gcount()) != count || reference != actual)
                throw std::runtime_error("Decoded bytes differ at entry " + std::to_string(id) +
                                         ", byte " + std::to_string(at));
            at += count;
        }
    }
    std::cout << "{\"schema_version\":1,\"byte_compared_entries\":" << directory.entries()
              << ",\"byte_compared_bytes\":" << total << ",\"exact_size_rows\":" << directory.sizes.size()
              << ",\"empty_entries\":" << empty << ",\"verbatim_entries\":" << verbatim
              << ",\"overlays\":" << overlays << "}\n";
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    try { return compare(argc, argv); }
    catch (const std::exception &error) {
        std::cerr << "Resource cross-check failed: " << error.what() << '\n';
        return 1;
    }
}
