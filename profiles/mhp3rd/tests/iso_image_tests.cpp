#include "kernel/iso_image.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kSector = mhp3rd::IsoImage::kSectorSize;
int failures = 0;

void check(bool condition, const char *description) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition) ++failures;
}

void le32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8u * i));
}

void be32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8u * (3u - i)));
}

// ISO 9660 directory record, including both byte orders of each numeric field.
std::vector<std::uint8_t> record(const std::string &name, std::uint32_t lba, std::uint32_t size, bool directory) {
    const std::size_t length = 33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u);
    std::vector<std::uint8_t> bytes(length);
    bytes[0] = static_cast<std::uint8_t>(length);
    le32(bytes, 2, lba);
    be32(bytes, 6, lba);
    le32(bytes, 10, size);
    be32(bytes, 14, size);
    bytes[25] = directory ? 2u : 0u;
    bytes[28] = 1u;
    bytes[31] = 1u;
    bytes[32] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), bytes.begin() + 33);
    return bytes;
}

void put(std::vector<std::uint8_t> &image, std::size_t sector, std::size_t offset,
         const std::vector<std::uint8_t> &bytes) {
    std::copy(bytes.begin(), bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(sector * kSector + offset));
}

std::vector<std::uint8_t> image_bytes() {
    std::vector<std::uint8_t> image(27u * kSector);
    const std::size_t pvd = 16u * kSector;
    image[pvd] = 1u;
    std::copy_n("CD001", 5, image.begin() + static_cast<std::ptrdiff_t>(pvd + 1u));
    image[pvd + 6u] = 1u;
    put(image, 16, 156, record(std::string(1, '\0'), 20, 2u * kSector, true));

    std::size_t at = 0;
    for (const auto &entry : {record(std::string(1, '\0'), 20, 2u * kSector, true),
                              record(std::string(1, '\1'), 20, 2u * kSector, true),
                              record("PSP_GAME", 22, kSector, true)}) {
        put(image, 20, at, entry);
        at += entry.size();
    }
    // A zero-length record pads the rest of the first sector.
    put(image, 21, 0, record("ROOT.TXT;1", 25, 3, false));

    at = 0;
    for (const auto &entry : {record(std::string(1, '\0'), 22, kSector, true),
                              record(std::string(1, '\1'), 20, 2u * kSector, true),
                              record("USRDIR", 23, kSector, true)}) {
        put(image, 22, at, entry);
        at += entry.size();
    }
    at = 0;
    for (const auto &entry : {record(std::string(1, '\0'), 23, kSector, true),
                              record(std::string(1, '\1'), 22, kSector, true),
                              record("DATA.BIN;1", 24, 7, false)}) {
        put(image, 23, at, entry);
        at += entry.size();
    }
    std::copy_n("payload", 7, image.begin() + static_cast<std::ptrdiff_t>(24u * kSector));
    std::copy_n("end", 3, image.begin() + static_cast<std::ptrdiff_t>(25u * kSector));
    return image;
}

class TempImage {
public:
    explicit TempImage(const std::vector<std::uint8_t> &bytes) {
        static std::uint64_t sequence = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("mhp3rd_iso_image_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" + std::to_string(++sequence) + ".iso");
        std::ofstream out(path_, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("cannot write synthetic ISO");
    }

    ~TempImage() { std::filesystem::remove(path_); }
    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool rejects(const std::vector<std::uint8_t> &bytes) {
    TempImage file(bytes);
    try {
        const mhp3rd::IsoImage image(file.path());
        (void)image;
    } catch (const std::exception &) {
        return true;
    }
    return false;
}

void test_valid_image() {
    TempImage file(image_bytes());
    mhp3rd::IsoImage image(file.path());
    check(image.size_bytes() == 27u * kSector, "reports the exact image length");
    const auto root = image.find("");
    check(root && root->directory && root->lba == 20 && root->size == 2u * kSector, "finds the root directory");
    const auto nested = image.find("\\psp_game//usrdir\\data.bin/");
    check(nested && !nested->directory && nested->lba == 24 && nested->size == 7,
          "finds a nested versioned file independent of path case and separator");
    check(image.find("ROOT.TXT") && !image.find("ROOT") && !image.find("MISSING"),
          "removes ISO version suffixes without matching unrelated names");
    check(image.list("") == std::vector<std::string>({"PSP_GAME", "ROOT.TXT"}),
          "lists only immediate root children across a padded sector");
    check(image.list("psp_game") == std::vector<std::string>({"USRDIR"}) &&
              image.list("PSP_GAME/USRDIR") == std::vector<std::string>({"DATA.BIN"}) &&
              image.list("PSP") == std::vector<std::string>{},
          "lists only immediate children of the requested directory");

    std::vector<std::uint8_t> output(10u, 0xA5u);
    check(image.read(24u * kSector, output) == 10u &&
              std::equal(output.begin(), output.begin() + 7, "payload"),
          "reads bytes at the nested file extent");
    std::fill(output.begin(), output.end(), 0xA5u);
    check(image.read(image.size_bytes() - 3u, output) == 3u && output[3] == 0xA5u,
          "clips a read at image EOF without writing the remainder");
    check(image.read(image.size_bytes(), output) == 0u &&
              image.read(std::numeric_limits<std::uint64_t>::max(), output) == 0u,
          "reads at or beyond EOF return zero");
    check(image.read(0, std::span<std::uint8_t>{}) == 0u, "empty output reads zero bytes");
}

void test_invalid_descriptor_and_extent() {
    auto bytes = image_bytes();
    bytes[16u * kSector] = 2u;
    check(rejects(bytes), "rejects a non-primary volume descriptor");
    bytes = image_bytes();
    bytes[16u * kSector + 6u] = 2u;
    check(rejects(bytes), "rejects an unsupported descriptor version");
    bytes = image_bytes();
    bytes.resize(16u * kSector + 100u);
    check(rejects(bytes), "rejects a truncated primary descriptor");
    bytes = image_bytes();
    le32(bytes, 16u * kSector + 156u + 10u, std::numeric_limits<std::uint32_t>::max());
    be32(bytes, 16u * kSector + 156u + 14u, std::numeric_limits<std::uint32_t>::max());
    check(rejects(bytes), "rejects an oversized root extent before allocating it");

    bytes = image_bytes();
    put(bytes, 20, 110, record("OUT.BIN;1", 99, 8, false));
    TempImage file(bytes);
    mhp3rd::IsoImage image(file.path());
    check(!image.find("OUT.BIN") && image.find("ROOT.TXT"),
          "ignores an out-of-image file extent and keeps later entries");
}

void test_malformed_directory_records() {
    auto bytes = image_bytes();
    // A short record still has a trustworthy length, so the following record
    // can be found without reading fields beyond the malformed one.
    bytes[20u * kSector + 110u] = 20u;
    put(bytes, 20, 130, record("GOOD.TXT;1", 25, 3, false));
    TempImage short_file(bytes);
    mhp3rd::IsoImage short_image(short_file.path());
    check(short_image.find("GOOD.TXT").has_value(), "skips a short directory record and continues scanning");

    bytes = image_bytes();
    const auto bad = record("BAD.BIN;1", 25, 3, false);
    put(bytes, 20, 110, bad);
    bytes[20u * kSector + 110u + 32u] = 100u;
    put(bytes, 20, 110u + bad.size(), record("GOOD.TXT;1", 25, 3, false));
    TempImage bad_name_file(bytes);
    mhp3rd::IsoImage bad_name_image(bad_name_file.path());
    check(!bad_name_image.find("BAD.BIN") && bad_name_image.find("GOOD.TXT"),
          "skips a record whose name exceeds its declared length");

    bytes = image_bytes();
    const auto dot = record(std::string(1, '\0'), 20, 2u * kSector, true);
    for (std::size_t at = 0; at < 2040u; at += dot.size()) put(bytes, 20, at, dot);
    put(bytes, 20, 2040, record("X", 25, 3, false));
    put(bytes, 21, 0, record("GOOD.TXT;1", 25, 3, false));
    TempImage cross_file(bytes);
    mhp3rd::IsoImage cross_image(cross_file.path());
    check(!cross_image.find("X") && cross_image.find("GOOD.TXT"),
          "does not join directory records across a sector boundary");
}

} // namespace

int main() {
    test_valid_image();
    test_invalid_descriptor_and_extent();
    test_malformed_directory_records();
    std::printf("ISO image tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
