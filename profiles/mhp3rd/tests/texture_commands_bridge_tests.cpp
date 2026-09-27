#include "native/texture_commands_bridge.hpp"

#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using mhp3rd::native::TextureCommandBounds;
using mhp3rd::native::apply_texture_commands;
using psprecomp::AllegrexContext;
using psprecomp::Elf32Image;
using psprecomp::GuestMemory;

constexpr std::uint32_t kRam = GuestMemory::kPhysicalBase;
constexpr std::uint32_t kEntry = 0x0889E5C0u;
constexpr std::uint32_t kHelper = 0x08876A10u;
constexpr std::uint32_t kTable = 0x089CED28u;
constexpr std::uint32_t kGlobal = 0x08AB3668u;
constexpr std::uint32_t kSource = 0x09000000u;
constexpr std::uint32_t kState = 0x08201020u;
constexpr std::uint32_t kCommands = 0x08220040u;
constexpr std::uint32_t kSp = 0x08310080u;
constexpr std::uint32_t kReturn = 0x08001000u;
constexpr std::size_t kMaxSource = 16u * 1024u * 1024u;

void require(bool condition, const std::string &reason) {
    if (!condition) throw std::runtime_error(reason);
}

void store16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    require(offset <= bytes.size() && bytes.size() - offset >= 2u, "Fixture halfword overflow");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void store32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4u, "Fixture word overflow");
    for (unsigned i = 0; i < 4u; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8u));
}

std::uint32_t load32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4u, "Fixture read overflow");
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1u]) << 8u) |
           (std::uint32_t(bytes[offset + 2u]) << 16u) |
           (std::uint32_t(bytes[offset + 3u]) << 24u);
}

struct Record {
    std::vector<std::uint8_t> bytes;
    std::uint32_t format{};
    std::uint16_t width{}, height{};
    std::uint32_t image_offset{};
    std::uint32_t palette_offset{};
    std::uint32_t palette_format{};
    std::int16_t palette_count{};
};

void append_subblock(std::vector<std::uint8_t> &record, std::uint32_t format,
                     std::uint16_t field12, std::uint16_t field14, std::size_t payload) {
    const auto start = record.size();
    record.resize(start + 16u + payload, 0x6Du);
    store32(record, start, static_cast<std::uint32_t>(16u + payload));
    store32(record, start + 4u, 0x78563412u); // An ignored tag, not a format field.
    store32(record, start + 8u, format);
    store16(record, start + 12u, field12);
    store16(record, start + 14u, field14);
}

Record direct_record() {
    Record record;
    record.bytes.resize(16u, 0x5Au);
    record.format = 8u;
    record.width = record.height = 4u;
    record.image_offset = 32u;
    append_subblock(record.bytes, record.format, record.width, record.height, 8u);
    store32(record.bytes, 0u, static_cast<std::uint32_t>(record.bytes.size()));
    store32(record.bytes, 8u, 1u);
    return record;
}

Record indexed_record(std::int16_t palette_count = 16, unsigned extra_images = 0u) {
    Record record;
    record.bytes.resize(16u, 0x5Au);
    record.format = 4u;
    record.width = record.height = 8u;
    record.image_offset = 32u;
    record.palette_format = 1u;
    record.palette_count = palette_count;
    append_subblock(record.bytes, record.format, record.width, record.height, 32u);
    for (unsigned i = 0; i < extra_images; ++i)
        append_subblock(record.bytes, 8u, 4u, 4u, 8u);
    record.palette_offset = static_cast<std::uint32_t>(record.bytes.size() + 16u);
    append_subblock(record.bytes, record.palette_format,
                    static_cast<std::uint16_t>(palette_count), 0u,
                    2u * static_cast<std::uint16_t>(palette_count));
    store32(record.bytes, 0u, static_cast<std::uint32_t>(record.bytes.size()));
    store32(record.bytes, 8u, 1u + extra_images);
    return record;
}

struct Source {
    std::vector<std::uint8_t> bytes;
    std::vector<Record> records;
    std::vector<std::size_t> starts;
};

Source make_source(std::vector<Record> records, std::uint32_t declared) {
    Source source;
    source.bytes.resize(16u, 0x37u);
    const std::array<std::uint8_t, 8> marker{'.', 'T', 'M', 'H', '0', '.', '1', '4'};
    std::copy(marker.begin(), marker.end(), source.bytes.begin());
    store32(source.bytes, 8u, declared);
    source.records = std::move(records);
    for (const auto &record : source.records) {
        source.starts.push_back(source.bytes.size());
        source.bytes.insert(source.bytes.end(), record.bytes.begin(), record.bytes.end());
    }
    return source;
}

std::vector<std::uint8_t> pattern(std::size_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> bytes(size);
    for (auto &byte : bytes) {
        seed = seed * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(seed >> 24u);
    }
    return bytes;
}

AllegrexContext context_fixture() {
    AllegrexContext context{};
    std::uint32_t seed = 0xD39E7241u;
    const auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    for (auto &word : context.gpr) word = next();
    for (auto &value : context.fpr) value = std::bit_cast<float>(next());
    for (auto &value : context.vfpu) value = std::bit_cast<float>(next());
    for (auto &word : context.vfpu_ctrl) word = next();
    context.hi = next(); context.lo = next(); context.fcr31 = next();
    context.gpr[0] = 0u;
    context.gpr[4] = kState;
    context.gpr[5] = kCommands;
    context.gpr[6] = kSource;
    context.gpr[7] = 0u;
    context.gpr[8] = 0u;
    context.gpr[9] = 1u;
    context.gpr[29] = kSp;
    context.gpr[31] = kReturn;
    context.pc = kEntry;
    return context;
}

bool same_context(const AllegrexContext &a, const AllegrexContext &b) {
    return a.gpr == b.gpr && a.hi == b.hi && a.lo == b.lo && a.pc == b.pc &&
           a.fcr31 == b.fcr31 && a.vfpu_ctrl == b.vfpu_ctrl &&
           std::memcmp(a.fpr.data(), b.fpr.data(), sizeof(a.fpr)) == 0 &&
           std::memcmp(a.vfpu.data(), b.vfpu.data(), sizeof(a.vfpu)) == 0;
}

struct Setup {
    Source source = make_source({direct_record(), direct_record()}, 2u);
    AllegrexContext context = context_fixture();
    TextureCommandBounds bounds{source.bytes.size(), 2u, 4096u};
};

void replace_source(Setup &setup, Source source) {
    setup.source = std::move(source);
    setup.bounds.source_bytes = setup.source.bytes.size();
}

std::size_t ram_offset(std::uint32_t address, std::size_t ram_size, std::size_t length) {
    const auto physical = GuestMemory::canonical(address);
    require(physical >= kRam && static_cast<std::uint64_t>(physical - kRam) + length <= ram_size,
            "Expected guest write outside RAM");
    return physical - kRam;
}

void expected8(std::vector<std::uint8_t> &ram, std::uint32_t address, std::uint8_t value) {
    ram[ram_offset(address, ram.size(), 1u)] = value;
}

void expected32(std::vector<std::uint8_t> &ram, std::uint32_t address, std::uint32_t value) {
    store32(ram, ram_offset(address, ram.size(), 4u), value);
}

std::uint32_t exponent(std::uint32_t dimension) {
    return dimension == 0u ? 0u : static_cast<std::uint32_t>(std::bit_width(dimension - 1u));
}

std::array<std::uint32_t, 9> words_for(const Record &record, std::uint32_t image,
                                       std::uint32_t palette) {
    return {0xC2000000u | (record.format >= 8u && record.format <= 10u ? 0u : 1u),
            0xC3000000u | record.format,
            0xA0000000u | (image & 0x00FFFFFFu),
            0xA8000000u | ((image >> 8u) & 0x00FF0000u) | record.width,
            0xB8000000u | (exponent(record.height) << 8u) | exponent(record.width),
            0xC500FF00u | record.palette_format,
            0xB0000000u | (palette & 0x00FFFFFFu),
            0xB1000000u | ((palette >> 8u) & 0x00FF0000u),
            0xC4000000u | ((static_cast<std::uint32_t>(record.palette_count) + 7u) >> 3u)};
}

class Harness {
public:
    explicit Harness(const Elf32Image &elf) : memory_(elf.required_ram_size()) {
        (void)elf.load_and_relocate(memory_);
        baseline_ram_ = memory_.bytes();
        baseline_vram_ = memory_.vram_bytes();
    }

    using SetupChange = std::function<void(Setup &)>;
    using MemoryChange = std::function<void(GuestMemory &)>;

    void reject(const std::string &name, const SetupChange &change,
                const MemoryChange &tamper = {}) {
        reset();
        Setup setup;
        if (change) change(setup);
        install(setup);
        if (tamper) tamper(memory_);
        const auto before_ram = memory_.bytes();
        const auto before_vram = memory_.vram_bytes();
        const auto before_cpu = setup.context;
        const auto result = apply_texture_commands(memory_, setup.context, setup.bounds);
        require(!result.ok(), name + ": unexpectedly accepted");
        require(same_context(setup.context, before_cpu), name + ": rejection changed CPU");
        require(memory_.bytes() == before_ram, name + ": rejection changed RAM");
        require(memory_.vram_bytes() == before_vram, name + ": rejection changed VRAM");
        ++rejections_;
    }

    void accept(const std::string &name, const SetupChange &change, std::size_t blocks) {
        reset();
        Setup setup;
        if (change) change(setup);
        install(setup);
        auto expected_ram = memory_.bytes();
        const auto expected_vram = memory_.vram_bytes();
        auto expected_cpu = setup.context;
        model_success(setup, blocks, expected_ram, expected_cpu);
        const auto result = apply_texture_commands(memory_, setup.context, setup.bounds);
        require(result.ok() && result.blocks == blocks, name + ": rejected a valid fixture");
        require(same_context(setup.context, expected_cpu), name + ": CPU differs");
        require(memory_.bytes() == expected_ram, name + ": RAM or a canary differs");
        require(memory_.vram_bytes() == expected_vram, name + ": VRAM differs");
        ++successes_;
    }

    void print_counts() const {
        std::cout << "texture_commands_bridge_tests: " << successes_ << " successes, "
                  << rejections_ << " atomic rejections\n";
    }

private:
    GuestMemory memory_;
    std::vector<std::uint8_t> baseline_ram_, baseline_vram_;
    std::size_t rejections_{}, successes_{};

    void reset() {
        memory_.copy_in(kRam, baseline_ram_);
        memory_.copy_in(GuestMemory::kVramPhysicalBase, baseline_vram_);
    }

    void install(const Setup &setup) {
        memory_.copy_in(kSource, setup.source.bytes);
        memory_.copy_in(kState - 32u, pattern(128u, 0x71345261u));
        memory_.copy_in(kCommands - 64u, pattern(4096u, 0xD18A3BC5u));
        memory_.copy_in(kSp - 128u, pattern(256u, 0x77C6D499u));
        memory_.store32(kGlobal, 0x13579BDFu);
    }

    static void model_success(const Setup &setup, std::size_t blocks,
                              std::vector<std::uint8_t> &ram, AllegrexContext &cpu) {
        const auto entry = setup.context;
        const auto state = entry.gpr[4], commands = entry.gpr[5], source = entry.gpr[6];
        const auto frame = entry.gpr[29] - 80u;
        const auto declared = load32(setup.source.bytes, 8u);
        expected32(ram, state + 4u, commands);
        expected32(ram, state, source);
        expected8(ram, state + 8u, static_cast<std::uint8_t>(declared));
        constexpr std::array<unsigned, 10> saved{16u, 17u, 18u, 19u, 20u,
                                                  21u, 22u, 23u, 30u, 31u};
        for (std::size_t i = 0; i < saved.size(); ++i)
            expected32(ram, frame + 32u + static_cast<std::uint32_t>(i * 4u), entry.gpr[saved[i]]);
        cpu.pc = entry.gpr[31];
        cpu.gpr[2] = declared;
        cpu.gpr[3] = entry.gpr[7] & 0xFFu;
        require(blocks <= 1u, "Success model is limited to one command");
        if (blocks == 0u) return;

        const auto record_index = entry.gpr[8] & 0xFFu;
        const auto slot = entry.gpr[7] & 0xFFu;
        require(record_index < setup.source.records.size(), "Success record outside fixture");
        const auto &record = setup.source.records[record_index];
        const auto image = source + static_cast<std::uint32_t>(setup.source.starts[record_index]) + record.image_offset;
        const auto palette = record.palette_offset == 0u ? 0u :
            source + static_cast<std::uint32_t>(setup.source.starts[record_index]) + record.palette_offset;
        const auto words = words_for(record, image, palette);
        expected32(ram, frame, image);
        expected32(ram, frame + 4u, record.format);
        expected32(ram, frame + 8u, std::uint32_t(record.width) | (std::uint32_t(record.height) << 16u));
        expected32(ram, frame + 12u, palette);
        expected32(ram, frame + 16u, record.palette_format);
        expected32(ram, frame + 20u, static_cast<std::uint32_t>(record.palette_count));
        for (std::size_t i = 0; i < words.size(); ++i)
            expected32(ram, commands + slot * 36u + static_cast<std::uint32_t>(i * 4u), words[i]);
        cpu.gpr[3] = 0xC4000000u;
        cpu.gpr[4] = words[7];
        cpu.gpr[5] = words[6];
        cpu.gpr[6] = commands + slot * 36u;
        cpu.gpr[7] = kTable + record.height * 4u;
        cpu.gpr[8] = kTable + record.width * 4u;
        cpu.gpr[9] = words[1];
        cpu.gpr[10] = words[2];
    }
};

void success_cases(Harness &h) {
    h.accept("zero header ignores unusable commands", [](Setup &s) {
        store32(s.source.bytes, 8u, 0u);
        s.context.gpr[9] = 0u;
        s.context.gpr[5] = 0xFFFFFFFFu;
        s.bounds.source_bytes = 12u;
        s.bounds.destination_slots = 0u;
        s.bounds.max_blocks = 0u;
    }, 0u);
    h.accept("signed negative header skips commands", [](Setup &s) {
        store32(s.source.bytes, 8u, 0x800001ABu);
        s.context.gpr[9] = 0u;
        s.context.gpr[5] = 0xFFFFFFFFu;
        s.bounds.destination_slots = 0u;
    }, 0u);
    h.accept("signed negative override skips commands", [](Setup &s) {
        s.context.gpr[9] = 0x80000000u;
        s.context.gpr[5] = 0xFFFFFFFFu;
        s.bounds.destination_slots = 0u;
    }, 0u);
    h.accept("direct command and high input bits", [](Setup &s) {
        s.context.gpr[7] = 0xABCD0001u;
        s.context.gpr[8] = 0xDEAD0000u;
    }, 1u);
    h.accept("uncached alias in disjoint regions", [](Setup &s) {
        for (unsigned reg : {4u, 5u, 6u, 29u}) s.context.gpr[reg] |= 0x40000000u;
    }, 1u);
    h.accept("cached alias in disjoint regions", [](Setup &s) {
        for (unsigned reg : {4u, 5u, 6u, 29u}) s.context.gpr[reg] |= 0x80000000u;
    }, 1u);
    h.accept("indexed palette with odd final record size", [](Setup &s) {
        replace_source(s, make_source({indexed_record(17)}, 1u));
        s.context.gpr[9] = 0u;
    }, 1u);
    h.accept("palette after two image subblocks", [](Setup &s) {
        replace_source(s, make_source({indexed_record(16, 1u)}, 1u));
        s.context.gpr[9] = 0u;
    }, 1u);
    h.accept("positive override ignores negative header", [](Setup &s) {
        store32(s.source.bytes, 8u, 0x800001ABu);
        s.context.gpr[9] = 1u;
    }, 1u);
    h.accept("huge unused output capacity", [](Setup &s) {
        s.bounds.destination_slots = std::numeric_limits<std::size_t>::max();
    }, 1u);
}

void rejection_cases(Harness &h, std::uint32_t ram_end) {
    h.reject("builder fingerprint", {}, [](GuestMemory &m) {
        m.store8(kEntry + 527u, m.load8(kEntry + 527u) ^ 1u);
    });
    h.reject("helper fingerprint", {}, [](GuestMemory &m) {
        m.store8(kHelper + 235u, m.load8(kHelper + 235u) ^ 1u);
    });
    h.reject("exponent table fingerprint", {}, [](GuestMemory &m) {
        m.store8(kTable + 4099u, m.load8(kTable + 4099u) ^ 1u);
    });
    h.reject("wrong entry", [](Setup &s) { s.context.pc += 4u; });

    h.reject("empty source bound", [](Setup &s) { s.bounds.source_bytes = 0u; });
    h.reject("short source header", [](Setup &s) { s.bounds.source_bytes = 15u; });
    h.reject("source over sixteen MiB", [](Setup &s) { s.bounds.source_bytes = kMaxSource + 1u; });
    h.reject("source bound size overflow", [](Setup &s) {
        s.bounds.source_bytes = std::numeric_limits<std::size_t>::max();
    });
    h.reject("unmapped source", [](Setup &s) { s.context.gpr[6] = 0x10000000u; });
    h.reject("unaligned source", [](Setup &s) { ++s.context.gpr[6]; });
    h.reject("short mapped source tail", [ram_end](Setup &s) { s.context.gpr[6] = ram_end - 16u; });
    h.reject("wrapped source address", [](Setup &s) { s.context.gpr[6] = 0xFFFFFFF8u; });
    h.reject("VRAM source", [](Setup &s) { s.context.gpr[6] = 0x44000000u; });

    h.reject("unmapped state", [](Setup &s) { s.context.gpr[4] = 0x10000000u; });
    h.reject("unaligned state", [](Setup &s) { ++s.context.gpr[4]; });
    h.reject("short state tail", [ram_end](Setup &s) { s.context.gpr[4] = ram_end - 8u; });
    h.reject("state address wraps", [](Setup &s) { s.context.gpr[4] = 0xFFFFFFFCu; });
    h.reject("VRAM state", [](Setup &s) { s.context.gpr[4] = 0x04001000u; });
    h.reject("unmapped stack", [](Setup &s) { s.context.gpr[29] = 0x10000000u; });
    h.reject("unaligned stack", [](Setup &s) { ++s.context.gpr[29]; });
    h.reject("short stack at RAM start", [](Setup &s) { s.context.gpr[29] = kRam + 64u; });
    h.reject("short stack at RAM end", [ram_end](Setup &s) { s.context.gpr[29] = ram_end + 4u; });
    h.reject("wrapped stack frame", [](Setup &s) { s.context.gpr[29] = 0x00000040u; });
    h.reject("VRAM stack", [](Setup &s) { s.context.gpr[29] = 0x44001080u; });

    h.reject("unmapped command output", [](Setup &s) { s.context.gpr[5] = 0x10000000u; });
    h.reject("unaligned command output", [](Setup &s) { ++s.context.gpr[5]; });
    h.reject("zero output capacity", [](Setup &s) { s.bounds.destination_slots = 0u; });
    h.reject("masked destination index beyond capacity", [](Setup &s) {
        s.context.gpr[7] = 0xFFFFFF01u;
        s.bounds.destination_slots = 1u;
    });
    h.reject("short command output tail", [ram_end](Setup &s) {
        s.context.gpr[5] = ram_end - 32u;
        s.bounds.destination_slots = 1u;
    });
    h.reject("wrapped command address", [](Setup &s) {
        s.context.gpr[5] = 0xFFFFFFF0u;
        s.bounds.destination_slots = 256u;
        s.context.gpr[7] = 255u;
    });
    h.reject("VRAM command output", [](Setup &s) { s.context.gpr[5] = 0x84002000u; });
    h.reject("zero block budget", [](Setup &s) { s.bounds.max_blocks = 0u; });
    h.reject("header exceeds global block budget", [](Setup &s) {
        store32(s.source.bytes, 8u, 4097u);
        s.context.gpr[9] = 0u;
        s.bounds.max_blocks = std::numeric_limits<std::size_t>::max();
    });
    h.reject("override exceeds global block budget", [](Setup &s) {
        s.context.gpr[9] = 0x7FFFFFFFu;
        s.bounds.max_blocks = std::numeric_limits<std::size_t>::max();
    });
    h.reject("record count beyond source", [](Setup &s) {
        store32(s.source.bytes, 8u, 3u);
        s.context.gpr[9] = 0u;
        s.bounds.destination_slots = 3u;
    });
    h.reject("masked source index beyond source", [](Setup &s) {
        s.context.gpr[8] = 0xABCD00FFu;
    });

    h.reject("source and output physical alias", [](Setup &s) {
        s.context.gpr[5] = kSource | 0x40000000u;
    });
    h.reject("state and source physical alias", [](Setup &s) {
        s.context.gpr[4] = (kSource + 16u) | 0x80000000u;
    });
    h.reject("stack and source physical alias", [](Setup &s) {
        s.context.gpr[29] = (kSource + 80u) | 0x40000000u;
    });
    h.reject("output and state physical alias", [](Setup &s) {
        s.context.gpr[5] = kState | 0x40000000u;
    });
    h.reject("output and stack physical alias", [](Setup &s) {
        s.context.gpr[29] = (kCommands + 80u) | 0x80000000u;
    });
    h.reject("state and stack physical alias", [](Setup &s) {
        s.context.gpr[4] = (kSp - 80u) | 0x40000000u;
    });
    h.reject("output overlaps builder code", [](Setup &s) { s.context.gpr[5] = kEntry; });
    h.reject("output overlaps helper code", [](Setup &s) { s.context.gpr[5] = kHelper; });
    h.reject("output overlaps exponent table", [](Setup &s) { s.context.gpr[5] = kTable; });
    h.reject("output overlaps global read", [](Setup &s) { s.context.gpr[5] = kGlobal; });
    h.reject("state overlaps builder code", [](Setup &s) { s.context.gpr[4] = kEntry; });
    h.reject("state overlaps exponent table", [](Setup &s) { s.context.gpr[4] = kTable; });
    h.reject("stack overlaps helper code", [](Setup &s) { s.context.gpr[29] = kHelper + 80u; });
    h.reject("stack overlaps global read", [](Setup &s) { s.context.gpr[29] = kGlobal + 80u; });

    h.reject("zero stride before selected record", [](Setup &s) {
        store32(s.source.bytes, s.source.starts[0], 0u);
        s.context.gpr[8] = 1u;
    });
    h.reject("unaligned stride before selected record", [](Setup &s) {
        store32(s.source.bytes, s.source.starts[0], 39u);
        s.context.gpr[8] = 1u;
    });
    h.reject("wrapped stride before selected record", [](Setup &s) {
        store32(s.source.bytes, s.source.starts[0], 0xFFFFFFF0u);
        s.context.gpr[8] = 1u;
    });
    h.reject("selected record shorter than metadata", [](Setup &s) {
        store32(s.source.bytes, s.source.starts[0], 31u);
    });
    h.reject("image subblock shorter than metadata", [](Setup &s) {
        store32(s.source.bytes, s.source.starts[0] + 16u, 15u);
    });
    h.reject("palette walk has zero image stride", [](Setup &s) {
        replace_source(s, make_source({indexed_record()}, 1u));
        store32(s.source.bytes, s.source.starts[0] + 16u, 0u);
    });
    h.reject("palette walk has unaligned image stride", [](Setup &s) {
        replace_source(s, make_source({indexed_record()}, 1u));
        store32(s.source.bytes, s.source.starts[0] + 16u, 47u);
    });
    h.reject("excessive image subblock count", [](Setup &s) {
        replace_source(s, make_source({indexed_record()}, 1u));
        store32(s.source.bytes, s.source.starts[0] + 8u, 4097u);
    });
    h.reject("palette subblock shorter than metadata", [](Setup &s) {
        replace_source(s, make_source({indexed_record()}, 1u));
        const auto palette = s.source.starts[0] + s.source.records[0].palette_offset - 16u;
        store32(s.source.bytes, palette, 15u);
    });
    h.reject("negative palette count", [](Setup &s) {
        replace_source(s, make_source({indexed_record()}, 1u));
        const auto palette = s.source.starts[0] + s.source.records[0].palette_offset - 16u;
        store16(s.source.bytes, palette + 12u, 0xFFFFu);
    });
    h.reject("unsupported image format", [](Setup &s) {
        store32(s.source.bytes, s.source.starts[0] + 24u, 11u);
    });
    h.reject("zero image width", [](Setup &s) {
        store16(s.source.bytes, s.source.starts[0] + 28u, 0u);
    });
    h.reject("zero image height", [](Setup &s) {
        store16(s.source.bytes, s.source.starts[0] + 30u, 0u);
    });
    h.reject("width beyond exponent table", [](Setup &s) {
        store16(s.source.bytes, s.source.starts[0] + 28u, 1025u);
    });
    h.reject("height beyond exponent table", [](Setup &s) {
        store16(s.source.bytes, s.source.starts[0] + 30u, 1025u);
    });
    h.reject("unsupported palette format", [](Setup &s) {
        replace_source(s, make_source({indexed_record()}, 1u));
        const auto palette = s.source.starts[0] + s.source.records[0].palette_offset - 16u;
        store32(s.source.bytes, palette + 8u, 4u);
    });
    h.reject("later invalid record is atomic", [](Setup &s) {
        s.context.gpr[9] = 2u;
        store32(s.source.bytes, s.source.starts[1] + 24u, 11u);
    });
    h.reject("later truncated record is atomic", [](Setup &s) {
        s.context.gpr[9] = 2u;
        s.bounds.source_bytes = s.source.starts[1] + 30u;
    });
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: texture_commands_bridge_tests EBOOT.ELF");
        const auto elf = Elf32Image::from_file(argv[1]);
        Harness harness(elf);
        success_cases(harness);
        rejection_cases(harness, kRam + elf.required_ram_size());
        harness.print_counts();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "texture_commands_bridge_tests: " << error.what() << '\n';
        return 1;
    }
}
